/* h2net: peer-to-peer Ethernet frame transport (native, libdatachannel C API).
 *
 * Signaling: the room relay numbers members 1, 2, 3, ... in join order and
 * forwards binary messages "u32 LE target id + payload" (it rewrites the id to
 * the source on delivery). Our payloads are one type byte + UTF-8 text:
 *   'o' SDP offer    'a' SDP answer    'c' ICE candidate as "<mid>\n<candidate>"
 * The lower member id offers to the higher one: a member that sees a "join"
 * notice offers to the newcomer; a newcomer waits for the offers.
 *
 * Data: one data channel per peer ("h2", unordered, maxRetransmits 0); each
 * message is one Ethernet frame. Source MACs seen on a channel are learned so
 * unicast frames go to one peer; unknown and group destinations are flooded.
 *
 * Locking: one lock guards the peer table, MAC table and receive queue. It is
 * never held across a libdatachannel call, because those can run our own
 * callbacks synchronously or wait on threads that are running them.
 */
#include "h2net.h"

#include <rtc/rtc.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK s_lk = SRWLOCK_INIT;
static void lock(void)   { AcquireSRWLockExclusive(&s_lk); }
static void unlock(void) { ReleaseSRWLockExclusive(&s_lk); }
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
#include <pthread.h>
#include <time.h>
static pthread_mutex_t s_lk = PTHREAD_MUTEX_INITIALIZER;
static void lock(void)   { pthread_mutex_lock(&s_lk); }
static void unlock(void) { pthread_mutex_unlock(&s_lk); }
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

#define MAX_PEERS   16          /* relay allows 10 members */
#define MAX_MACS    64
#define MAX_PENDING 16          /* remote candidates held until the remote description */

typedef struct {
    int id;                     /* relay member id; 0 = free slot */
    int pc, dc;                 /* libdatachannel handles; 0 = none */
    int open;                   /* data channel open */
    int remote_set;             /* remote description applied */
    int state;                  /* last rtcState */
    char *pending[MAX_PENDING];
    int npending;
    uint64_t rx, tx;
} peer_t;

typedef struct { uint8_t mac[6]; int id; uint64_t seen; } mac_t;

typedef struct { uint32_t len; uint8_t data[H2NET_FRAME_MAX]; } slot_t;

static int s_open;              /* between h2net_open and h2net_close */
static int s_ws;                /* relay WebSocket handle */
static int s_ws_state;          /* 0 connecting, 1 open, 2 closed, 3 error */
static int s_self, s_host;
static peer_t s_peer[MAX_PEERS];
static mac_t s_mac[MAX_MACS];
static slot_t *s_rxq;
static uint32_t s_rx_head, s_rx_tail;   /* free-running indices */
static uint64_t s_tx, s_rx, s_drop, s_flood;
static char s_err[96];

static const char *s_ice[] = { "stun:stun.l.google.com:19302", "stun:stun.cloudflare.com:3478" };

/* ---- helpers (call with the lock held) ----------------------------------- */

static peer_t *peer_by_id(int id)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id == id && id) return &s_peer[i];
    return NULL;
}

static peer_t *peer_by_pc(int pc)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id && s_peer[i].pc == pc) return &s_peer[i];
    return NULL;
}

static peer_t *peer_by_dc(int dc)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id && s_peer[i].dc == dc) return &s_peer[i];
    return NULL;
}

static void forget_macs(int id)
{
    int i;
    for (i = 0; i < MAX_MACS; i++)
        if (s_mac[i].id == id) s_mac[i].id = 0;
}

static void learn_mac(const uint8_t *mac, int id)
{
    int i, free_i = -1, old_i = 0;
    uint64_t t = now_ms();
    if (mac[0] & 1) return;                       /* group address as source: ignore */
    for (i = 0; i < MAX_MACS; i++) {
        if (s_mac[i].id && !memcmp(s_mac[i].mac, mac, 6)) { s_mac[i].id = id; s_mac[i].seen = t; return; }
        if (!s_mac[i].id && free_i < 0) free_i = i;
        if (s_mac[i].seen < s_mac[old_i].seen) old_i = i;
    }
    i = free_i >= 0 ? free_i : old_i;
    memcpy(s_mac[i].mac, mac, 6);
    s_mac[i].id = id;
    s_mac[i].seen = t;
}

static int lookup_mac(const uint8_t *mac)
{
    int i;
    for (i = 0; i < MAX_MACS; i++)
        if (s_mac[i].id && !memcmp(s_mac[i].mac, mac, 6)) return s_mac[i].id;
    return 0;
}

/* Detaches a peer from the table; returns its handles for deletion outside the lock. */
static void detach_peer(peer_t *p, int *pc, int *dc, char **pend, int *npend)
{
    int i;
    *pc = p->pc;
    *dc = p->dc;
    *npend = p->npending;
    for (i = 0; i < p->npending; i++) pend[i] = p->pending[i];
    forget_macs(p->id);
    memset(p, 0, sizeof *p);
}

static void destroy_handles(int pc, int dc, char **pend, int npend)
{
    int i;
    for (i = 0; i < npend; i++) free(pend[i]);
    if (dc > 0) rtcDeleteDataChannel(dc);
    if (pc > 0) rtcDeletePeerConnection(pc);
}

static void drop_peer(int id)
{
    int pc = 0, dc = 0, npend = 0;
    char *pend[MAX_PENDING];
    peer_t *p;
    lock();
    p = peer_by_id(id);
    if (p) detach_peer(p, &pc, &dc, pend, &npend);
    unlock();
    if (p) destroy_handles(pc, dc, pend, npend);
}

/* ---- signaling out ------------------------------------------------------- */

static void signal_send(int target, char type, const char *a, const char *b)
{
    size_t la = strlen(a), lb = b ? strlen(b) : 0;
    size_t n = 4 + 1 + la + (b ? 1 + lb : 0);
    char *m = (char *)malloc(n);
    int ws;
    if (!m) return;
    m[0] = (char)(target & 255); m[1] = (char)((target >> 8) & 255);
    m[2] = (char)((target >> 16) & 255); m[3] = (char)((target >> 24) & 255);
    m[4] = type;
    memcpy(m + 5, a, la);
    if (b) { m[5 + la] = '\n'; memcpy(m + 6 + la, b, lb); }
    lock();
    ws = s_ws;
    unlock();
    if (ws > 0) rtcSendMessage(ws, m, (int)n);
    free(m);
}

/* ---- peer connection callbacks ------------------------------------------- */

static int member_of_pc(int pc)
{
    peer_t *p;
    int id;
    lock();
    p = peer_by_pc(pc);
    id = p ? p->id : 0;
    unlock();
    return id;
}

static void RTC_API on_local_description(int pc, const char *sdp, const char *type, void *ptr)
{
    int id = member_of_pc(pc);
    (void)ptr;
    if (id) signal_send(id, (type && type[0] == 'o') ? 'o' : 'a', sdp, NULL);
}

static void RTC_API on_local_candidate(int pc, const char *cand, const char *mid, void *ptr)
{
    int id = member_of_pc(pc);
    (void)ptr;
    if (id) signal_send(id, 'c', mid ? mid : "0", cand);
}

static void RTC_API on_state(int pc, rtcState state, void *ptr)
{
    peer_t *p;
    (void)ptr;
    lock();
    p = peer_by_pc(pc);
    if (p) {
        p->state = (int)state;
        if (state == RTC_FAILED || state == RTC_CLOSED || state == RTC_DISCONNECTED) p->open = 0;
    }
    unlock();
}

static void RTC_API on_dc_open(int dc, void *ptr)
{
    peer_t *p;
    (void)ptr;
    lock();
    p = peer_by_dc(dc);
    if (p) p->open = 1;
    unlock();
}

static void RTC_API on_dc_closed(int dc, void *ptr)
{
    peer_t *p;
    (void)ptr;
    lock();
    p = peer_by_dc(dc);
    if (p) p->open = 0;
    unlock();
}

static void RTC_API on_dc_message(int dc, const char *msg, int size, void *ptr)
{
    peer_t *p;
    (void)ptr;
    if (size < 14 || size > (int)H2NET_FRAME_MAX) return;   /* text (size < 0) or not a frame */
    lock();
    p = peer_by_dc(dc);
    if (p) {
        p->rx++;
        learn_mac((const uint8_t *)msg + 6, p->id);
        if (s_rxq && s_rx_head - s_rx_tail < H2NET_RX_QUEUE) {
            slot_t *s = &s_rxq[s_rx_head % H2NET_RX_QUEUE];
            s->len = (uint32_t)size;
            memcpy(s->data, msg, (size_t)size);
            s_rx_head++;
            s_rx++;
        } else {
            s_drop++;
        }
    }
    unlock();
}

static void attach_dc(int pc, int dc)
{
    peer_t *p;
    rtcSetOpenCallback(dc, on_dc_open);
    rtcSetClosedCallback(dc, on_dc_closed);
    rtcSetMessageCallback(dc, on_dc_message);
    lock();
    p = peer_by_pc(pc);
    if (p) p->dc = dc;
    unlock();
    if (!p) { rtcDeleteDataChannel(dc); return; }
    if (rtcIsOpen(dc)) on_dc_open(dc, NULL);       /* opened before the callback was set */
}

static void RTC_API on_data_channel(int pc, int dc, void *ptr)
{
    (void)ptr;
    attach_dc(pc, dc);
}

/* Creates the connection to member `id` (replacing any old one). */
static int create_peer(int id, int offerer)
{
    rtcConfiguration cfg;
    int pc, i;
    peer_t *p;

    drop_peer(id);
    memset(&cfg, 0, sizeof cfg);
    cfg.iceServers = s_ice;
    cfg.iceServersCount = 2;
    cfg.maxMessageSize = 65536;
    pc = rtcCreatePeerConnection(&cfg);
    if (pc < 0) return -1;
    rtcSetLocalDescriptionCallback(pc, on_local_description);
    rtcSetLocalCandidateCallback(pc, on_local_candidate);
    rtcSetStateChangeCallback(pc, on_state);
    rtcSetDataChannelCallback(pc, on_data_channel);

    lock();
    p = NULL;
    for (i = 0; i < MAX_PEERS && !p; i++)
        if (!s_peer[i].id) p = &s_peer[i];
    if (p) { memset(p, 0, sizeof *p); p->id = id; p->pc = pc; }
    unlock();
    if (!p) { rtcDeletePeerConnection(pc); return -1; }

    if (offerer) {
        rtcDataChannelInit init;
        int dc;
        memset(&init, 0, sizeof init);
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        init.reliability.unreliable = true;          /* without it the C API ignores maxRetransmits */
        dc = rtcCreateDataChannelEx(pc, "h2", &init);   /* triggers the offer */
        if (dc > 0) attach_dc(pc, dc);
    }
    return pc;
}

static void add_candidate(int pc, const char *text)
{
    const char *nl = strchr(text, '\n');
    char mid[64];
    size_t ml;
    if (!nl) return;
    ml = (size_t)(nl - text);
    if (ml >= sizeof mid) return;
    memcpy(mid, text, ml);
    mid[ml] = 0;
    rtcAddRemoteCandidate(pc, nl + 1, mid);
}

static void flush_pending(int id)
{
    char *pend[MAX_PENDING];
    int n = 0, pc = 0, i;
    peer_t *p;
    lock();
    p = peer_by_id(id);
    if (p) {
        p->remote_set = 1;
        pc = p->pc;
        n = p->npending;
        for (i = 0; i < n; i++) pend[i] = p->pending[i];
        p->npending = 0;
    }
    unlock();
    for (i = 0; i < n; i++) { add_candidate(pc, pend[i]); free(pend[i]); }
}

/* ---- relay WebSocket ----------------------------------------------------- */

static int json_int(const char *s, const char *key)
{
    const char *k = strstr(s, key);
    if (!k) return 0;
    k += strlen(key);
    while (*k == ' ' || *k == ':' || *k == '"') k++;
    return atoi(k);
}

static void on_notice(const char *s)
{
    if (strstr(s, "\"welcome\"")) {
        lock();
        s_self = json_int(s, "\"self\"");
        s_host = json_int(s, "\"host\"");
        unlock();
        /* existing members all have lower ids: they offer to us */
    } else if (strstr(s, "\"join\"")) {
        int id = json_int(s, "\"id\"");
        int self;
        lock();
        self = s_self;
        unlock();
        if (id > 0 && self > 0 && self < id) create_peer(id, 1);
    } else if (strstr(s, "\"leave\"")) {
        drop_peer(json_int(s, "\"id\""));
    } else if (strstr(s, "\"host-left\"")) {
        /* the relay's own session model ends here; our mesh carries on */
        int host, next = 0, i;
        lock();
        host = s_host;
        unlock();
        drop_peer(host);
        lock();
        for (i = 0; i < MAX_PEERS; i++)
            if (s_peer[i].id && (!next || s_peer[i].id < next)) next = s_peer[i].id;
        s_host = next && next < s_self ? next : s_self;
        unlock();
    } else if (strstr(s, "\"full\"")) {
        lock();
        snprintf(s_err, sizeof s_err, "room full");
        unlock();
    }
}

static void on_signal(int from, const char *m, int n)
{
    char *text;
    char type;
    if (n < 1) return;
    type = m[0];
    text = (char *)malloc((size_t)n);
    if (!text) return;
    memcpy(text, m + 1, (size_t)n - 1);
    text[n - 1] = 0;

    if (type == 'o') {
        int pc = create_peer(from, 0);
        if (pc > 0) {
            rtcSetRemoteDescription(pc, text, "offer");  /* auto-answers */
            flush_pending(from);
        }
    } else if (type == 'a') {
        int pc;
        peer_t *p;
        lock();
        p = peer_by_id(from);
        pc = p ? p->pc : 0;
        unlock();
        if (pc > 0) {
            rtcSetRemoteDescription(pc, text, "answer");
            flush_pending(from);
        }
    } else if (type == 'c') {
        int pc = 0, now = 0;
        peer_t *p;
        lock();
        p = peer_by_id(from);
        if (p) {
            if (p->remote_set) { pc = p->pc; now = 1; }
            else if (p->npending < MAX_PENDING) { p->pending[p->npending++] = text; text = NULL; }
        }
        unlock();
        if (now) add_candidate(pc, text);
    }
    free(text);
}

static void RTC_API on_ws_open(int ws, void *ptr)   { (void)ws; (void)ptr; lock(); s_ws_state = 1; unlock(); }
static void RTC_API on_ws_closed(int ws, void *ptr) { (void)ws; (void)ptr; lock(); s_ws_state = 2; unlock(); }

static void RTC_API on_ws_error(int ws, const char *err, void *ptr)
{
    (void)ws; (void)ptr;
    lock();
    s_ws_state = 3;
    snprintf(s_err, sizeof s_err, "%s", err ? err : "error");
    unlock();
}

static void RTC_API on_ws_message(int ws, const char *msg, int size, void *ptr)
{
    (void)ws; (void)ptr;
    if (size < 0) { on_notice(msg); return; }
    if (size >= 4) {
        const uint8_t *u = (const uint8_t *)msg;
        int from = (int)(u[0] | u[1] << 8 | u[2] << 16 | (uint32_t)u[3] << 24);
        on_signal(from, msg + 4, size - 4);
    }
}

/* ---- public API ---------------------------------------------------------- */

static void RTC_API on_log(rtcLogLevel level, const char *message)
{
    fprintf(stderr, "[h2net %d] %s\n", (int)level, message);
}

int h2net_open(const char *room_code, const char *relay_url)
{
    char url[512];
    rtcWsConfiguration wcfg;
    const char *e;
    size_t i, rl;
    int ws;

    if (!room_code) return -1;
    rl = strlen(room_code);
    if (rl < 4 || rl > 24) return -1;
    for (i = 0; i < rl; i++) {
        char c = room_code[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return -1;
    }
    if (!relay_url || !*relay_url) relay_url = H2NET_DEFAULT_RELAY;
    rl = strlen(relay_url);
    while (rl && relay_url[rl - 1] == '/') rl--;
    if (snprintf(url, sizeof url, "%.*s/room/%s", (int)rl, relay_url, room_code) >= (int)sizeof url) return -1;

    lock();
    if (s_open) { unlock(); return -1; }
    s_open = 1;
    unlock();

    e = getenv("H2NET_LOG");
    rtcInitLogger(e && *e ? (rtcLogLevel)atoi(e) : RTC_LOG_NONE, e && *e ? on_log : NULL);

    lock();
    if (!s_rxq) s_rxq = (slot_t *)malloc(sizeof(slot_t) * H2NET_RX_QUEUE);
    s_rx_head = s_rx_tail = 0;
    s_tx = s_rx = s_drop = s_flood = 0;
    s_self = s_host = 0;
    s_ws_state = 0;
    s_err[0] = 0;
    memset(s_peer, 0, sizeof s_peer);
    memset(s_mac, 0, sizeof s_mac);
    unlock();

    memset(&wcfg, 0, sizeof wcfg);
    wcfg.connectionTimeoutMs = 15000;
    ws = rtcCreateWebSocketEx(url, &wcfg);
    if (ws < 0) {
        lock();
        s_open = 0;
        unlock();
        return -1;
    }
    rtcSetOpenCallback(ws, on_ws_open);
    rtcSetClosedCallback(ws, on_ws_closed);
    rtcSetErrorCallback(ws, on_ws_error);
    rtcSetMessageCallback(ws, on_ws_message);
    lock();
    s_ws = ws;
    unlock();
    if (rtcIsOpen(ws)) on_ws_open(ws, NULL);
    return 0;
}

void h2net_send(const uint8_t *f, uint32_t len)
{
    int dcs[MAX_PEERS], n = 0, i, target;
    if (!f || len < 14 || len > H2NET_FRAME_MAX) return;
    lock();
    if (!s_open) { unlock(); return; }
    target = (f[0] & 1) ? 0 : lookup_mac(f);
    for (i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &s_peer[i];
        if (!p->id || !p->open || p->dc <= 0) continue;
        if (target && p->id != target) continue;
        dcs[n++] = p->dc;
        p->tx++;
    }
    if (n) s_tx++;
    if (!target && n) s_flood++;
    unlock();
    for (i = 0; i < n; i++) rtcSendMessage(dcs[i], (const char *)f, (int)len);
}

int h2net_recv(uint8_t *f, uint32_t max)
{
    uint32_t len = 0;
    lock();
    if (s_rxq && s_rx_tail != s_rx_head) {
        slot_t *s = &s_rxq[s_rx_tail % H2NET_RX_QUEUE];
        len = s->len;
        if (len <= max) memcpy(f, s->data, len);
        else len = 0;
        s_rx_tail++;
    }
    unlock();
    return (int)len;
}

int h2net_peers(void)
{
    int i, n = 0;
    lock();
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id && s_peer[i].open) n++;
    unlock();
    return n;
}

static const char *cand_type(const char *c)
{
    const char *t = strstr(c, " typ ");
    if (!t) return "?";
    t += 5;
    if (!strncmp(t, "host", 4)) return "host";
    if (!strncmp(t, "srflx", 5)) return "srflx";
    if (!strncmp(t, "prflx", 5)) return "prflx";
    if (!strncmp(t, "relay", 5)) return "relay";
    return "?";
}

void h2net_status(char *buf, size_t n)
{
    static const char *wsn[] = { "connecting", "open", "closed", "error" };
    int ids[MAX_PEERS], pcs[MAX_PEERS], open[MAX_PEERS], st[MAX_PEERS], np = 0, nopen = 0, i;
    int self, host, wst;
    uint64_t tx, rx, drop, flood;
    char err[96];
    size_t o = 0;

    if (!buf || !n) return;
    lock();
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id) {
            ids[np] = s_peer[i].id; pcs[np] = s_peer[i].pc; open[np] = s_peer[i].open; st[np] = s_peer[i].state;
            nopen += open[np];
            np++;
        }
    self = s_self; host = s_host; wst = s_open ? s_ws_state : 2;
    tx = s_tx; rx = s_rx; drop = s_drop; flood = s_flood;
    memcpy(err, s_err, sizeof err);
    unlock();

#define PUT(...) do { if (o < n) { int w_ = snprintf(buf + o, n - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_; } } while (0)
    PUT("relay=%s self=%d host=%d peers=%d/%d", wsn[wst & 3], self, host, nopen, np);
    for (i = 0; i < np; i++) {
        char lc[256], rc[256];
        static const char *stn[] = { "new", "connecting", "connected", "disconnected", "failed", "closed" };
        PUT(" [%d:%s", ids[i], open[i] ? "open" : stn[st[i] >= 0 && st[i] <= 5 ? st[i] : 0]);
        if (pcs[i] > 0 && rtcGetSelectedCandidatePair(pcs[i], lc, sizeof lc, rc, sizeof rc) > 0)
            PUT(" %s-%s", cand_type(lc), cand_type(rc));
        PUT("]");
    }
    PUT(" tx=%llu rx=%llu drop=%llu flood=%llu", (unsigned long long)tx, (unsigned long long)rx,
        (unsigned long long)drop, (unsigned long long)flood);
    if (err[0]) PUT(" err=\"%s\"", err);
#undef PUT
    if (o >= n) buf[n - 1] = 0;
}

void h2net_close(void)
{
    int ws, pcs[MAX_PEERS], dcs[MAX_PEERS], np = 0, i, j, npend;
    char *pend[MAX_PENDING];

    lock();
    if (!s_open) { unlock(); return; }
    ws = s_ws;
    s_ws = 0;
    unlock();
    if (ws > 0) rtcDeleteWebSocket(ws);

    lock();
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id) {
            detach_peer(&s_peer[i], &pcs[np], &dcs[np], pend, &npend);
            for (j = 0; j < npend; j++) free(pend[j]);
            np++;
        }
    unlock();
    for (i = 0; i < np; i++) destroy_handles(pcs[i], dcs[i], NULL, 0);
    rtcCleanup();

    lock();
    free(s_rxq);
    s_rxq = NULL;
    s_open = 0;
    s_ws_state = 2;
    unlock();
}
