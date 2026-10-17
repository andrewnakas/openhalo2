/* h2net: peer-to-peer Ethernet frame transport (native, libdatachannel C API).
 *
 * Signaling: the room relay numbers members 1, 2, 3, ... in join order and
 * forwards binary messages "u32 LE target id + payload" (it rewrites the id to
 * the source on delivery). Our payloads are one type byte + UTF-8 text:
 *   'o' SDP offer    'a' SDP answer    'c' ICE candidate as "<mid>\n<candidate>"
 * The lower member id offers to the higher one: a member that sees a "join"
 * notice offers to the newcomer; a newcomer waits for the offers.
 *
 * Rooms: a station can be in several rooms at once (Open Live: a party's
 * room and the room of the game it is joining; h2net_join / h2net_leave).
 * All of them form one segment: the learning switch spans every peer of
 * every room, and flooded frames go to all of them. h2net_open is the old
 * one-room form (system link with a room code).
 *
 * Data: one data channel per peer and room ("h2", unordered, maxRetransmits
 * 0); each message is one Ethernet frame. Source MACs seen on a channel are
 * learned so unicast frames go to one peer; unknown and group destinations
 * are flooded.
 *
 * Locking: one lock guards the room and peer tables, MAC table and receive
 * queue. It is never held across a libdatachannel call, because those can
 * run our own callbacks synchronously or wait on threads that are running
 * them.
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

#define MAX_ROOMS   H2NET_MAX_ROOMS
#define MAX_PEERS   48          /* relay allows 10 members a room */
#define MAX_MACS    64
#define MAX_PENDING 16          /* remote candidates held until the remote description */

typedef struct {
    int used;
    char code[32];
    int ws;                     /* relay WebSocket handle */
    int ws_state;               /* 0 connecting, 1 open, 2 closed, 3 error */
    int self, host;
    char err[96];
} room_t;

typedef struct {
    int room;                   /* index into s_room; with id, the key */
    int id;                     /* relay member id; 0 = free slot */
    int pc, dc;                 /* libdatachannel handles; 0 = none */
    int open;                   /* data channel open */
    int remote_set;             /* remote description applied */
    int state;                  /* last rtcState */
    char *pending[MAX_PENDING];
    int npending;
    uint64_t rx, tx;
} peer_t;

/* a learned station: which peer (slot index + 1) it was last seen behind */
typedef struct { uint8_t mac[6]; int slot; uint64_t seen; } mac_t;

typedef struct { uint32_t len; uint8_t data[H2NET_FRAME_MAX]; } slot_t;

static int s_init;              /* between h2net_init and h2net_close */
static char s_relay[400];
static room_t s_room[MAX_ROOMS];
static peer_t s_peer[MAX_PEERS];
static mac_t s_mac[MAX_MACS];
static slot_t *s_rxq;
static uint32_t s_rx_head, s_rx_tail;   /* free-running indices */
static uint64_t s_tx, s_rx, s_drop, s_flood;

static const char *s_ice[] = { "stun:stun.l.google.com:19302", "stun:stun.cloudflare.com:3478" };

/* user pointers of libdatachannel handles: the room index, plus one */
#define RP(r) ((void *)(intptr_t)((r) + 1))
#define PR(p) ((int)(intptr_t)(p) - 1)

/* ---- helpers (call with the lock held) ----------------------------------- */

static peer_t *peer_by_id(int room, int id)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id == id && id && s_peer[i].room == room) return &s_peer[i];
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

static void forget_macs(int slot)
{
    int i;
    for (i = 0; i < MAX_MACS; i++)
        if (s_mac[i].slot == slot) s_mac[i].slot = 0;
}

static void learn_mac(const uint8_t *mac, int slot)
{
    int i, free_i = -1, old_i = 0;
    uint64_t t = now_ms();
    if (mac[0] & 1) return;                       /* group address as source: ignore */
    for (i = 0; i < MAX_MACS; i++) {
        if (s_mac[i].slot && !memcmp(s_mac[i].mac, mac, 6)) { s_mac[i].slot = slot; s_mac[i].seen = t; return; }
        if (!s_mac[i].slot && free_i < 0) free_i = i;
        if (s_mac[i].seen < s_mac[old_i].seen) old_i = i;
    }
    i = free_i >= 0 ? free_i : old_i;
    memcpy(s_mac[i].mac, mac, 6);
    s_mac[i].slot = slot;
    s_mac[i].seen = t;
}

static int lookup_mac(const uint8_t *mac)
{
    int i;
    for (i = 0; i < MAX_MACS; i++)
        if (s_mac[i].slot && !memcmp(s_mac[i].mac, mac, 6)) return s_mac[i].slot;
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
    forget_macs((int)(p - s_peer) + 1);
    memset(p, 0, sizeof *p);
}

static void destroy_handles(int pc, int dc, char **pend, int npend)
{
    int i;
    for (i = 0; i < npend; i++) free(pend[i]);
    if (dc > 0) rtcDeleteDataChannel(dc);
    if (pc > 0) rtcDeletePeerConnection(pc);
}

static void drop_peer(int room, int id)
{
    int pc = 0, dc = 0, npend = 0;
    char *pend[MAX_PENDING];
    peer_t *p;
    lock();
    p = peer_by_id(room, id);
    if (p) detach_peer(p, &pc, &dc, pend, &npend);
    unlock();
    if (p) destroy_handles(pc, dc, pend, npend);
}

/* ---- signaling out ------------------------------------------------------- */

static void signal_send(int room, int target, char type, const char *a, const char *b)
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
    ws = (room >= 0 && room < MAX_ROOMS && s_room[room].used) ? s_room[room].ws : 0;
    unlock();
    if (ws > 0) rtcSendMessage(ws, m, (int)n);
    free(m);
}

/* ---- peer connection callbacks ------------------------------------------- */

static int member_of_pc(int pc, int *room)
{
    peer_t *p;
    int id;
    lock();
    p = peer_by_pc(pc);
    id = p ? p->id : 0;
    *room = p ? p->room : -1;
    unlock();
    return id;
}

static void RTC_API on_local_description(int pc, const char *sdp, const char *type, void *ptr)
{
    int room, id = member_of_pc(pc, &room);
    (void)ptr;
    if (id) signal_send(room, id, (type && type[0] == 'o') ? 'o' : 'a', sdp, NULL);
}

static void RTC_API on_local_candidate(int pc, const char *cand, const char *mid, void *ptr)
{
    int room, id = member_of_pc(pc, &room);
    (void)ptr;
    if (id) signal_send(room, id, 'c', mid ? mid : "0", cand);
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
        learn_mac((const uint8_t *)msg + 6, (int)(p - s_peer) + 1);
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

/* Creates the connection to member `id` of `room` (replacing any old one). */
static int create_peer(int room, int id, int offerer)
{
    rtcConfiguration cfg;
    int pc, i;
    peer_t *p;

    drop_peer(room, id);
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
    if (p) { memset(p, 0, sizeof *p); p->room = room; p->id = id; p->pc = pc; }
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

static void flush_pending(int room, int id)
{
    char *pend[MAX_PENDING];
    int n = 0, pc = 0, i;
    peer_t *p;
    lock();
    p = peer_by_id(room, id);
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

static void on_notice(int room, const char *s)
{
    room_t *r = &s_room[room];
    if (strstr(s, "\"welcome\"")) {
        lock();
        r->self = json_int(s, "\"self\"");
        r->host = json_int(s, "\"host\"");
        unlock();
        /* existing members all have lower ids: they offer to us */
    } else if (strstr(s, "\"join\"")) {
        int id = json_int(s, "\"id\"");
        int self;
        lock();
        self = r->self;
        unlock();
        if (id > 0 && self > 0 && self < id) create_peer(room, id, 1);
    } else if (strstr(s, "\"leave\"")) {
        drop_peer(room, json_int(s, "\"id\""));
    } else if (strstr(s, "\"host-left\"")) {
        /* the relay's own session model ends here; our mesh carries on */
        int host, next = 0, i;
        lock();
        host = r->host;
        unlock();
        drop_peer(room, host);
        lock();
        for (i = 0; i < MAX_PEERS; i++)
            if (s_peer[i].id && s_peer[i].room == room && (!next || s_peer[i].id < next)) next = s_peer[i].id;
        r->host = next && next < r->self ? next : r->self;
        unlock();
    } else if (strstr(s, "\"full\"")) {
        lock();
        snprintf(r->err, sizeof r->err, "room full");
        unlock();
    }
}

static void on_signal(int room, int from, const char *m, int n)
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
        int pc = create_peer(room, from, 0);
        if (pc > 0) {
            rtcSetRemoteDescription(pc, text, "offer");  /* auto-answers */
            flush_pending(room, from);
        }
    } else if (type == 'a') {
        int pc;
        peer_t *p;
        lock();
        p = peer_by_id(room, from);
        pc = p ? p->pc : 0;
        unlock();
        if (pc > 0) {
            rtcSetRemoteDescription(pc, text, "answer");
            flush_pending(room, from);
        }
    } else if (type == 'c') {
        int pc = 0, now = 0;
        peer_t *p;
        lock();
        p = peer_by_id(room, from);
        if (p) {
            if (p->remote_set) { pc = p->pc; now = 1; }
            else if (p->npending < MAX_PENDING) { p->pending[p->npending++] = text; text = NULL; }
        }
        unlock();
        if (now) add_candidate(pc, text);
    }
    free(text);
}

static int room_of_ws(int ws, void *ptr)
{
    int r = PR(ptr);
    if (r < 0 || r >= MAX_ROOMS) return -1;
    lock();
    if (!s_room[r].used || s_room[r].ws != ws) r = -1;
    unlock();
    return r;
}

static void RTC_API on_ws_open(int ws, void *ptr)
{
    int r = room_of_ws(ws, ptr);
    if (r < 0) return;
    lock(); s_room[r].ws_state = 1; unlock();
}

static void RTC_API on_ws_closed(int ws, void *ptr)
{
    int r = room_of_ws(ws, ptr);
    if (r < 0) return;
    lock(); s_room[r].ws_state = 2; unlock();
}

static void RTC_API on_ws_error(int ws, const char *err, void *ptr)
{
    int r = room_of_ws(ws, ptr);
    if (r < 0) return;
    lock();
    s_room[r].ws_state = 3;
    snprintf(s_room[r].err, sizeof s_room[r].err, "%s", err ? err : "error");
    unlock();
}

static void RTC_API on_ws_message(int ws, const char *msg, int size, void *ptr)
{
    int r = room_of_ws(ws, ptr);
    if (r < 0) return;
    if (size < 0) { on_notice(r, msg); return; }
    if (size >= 4) {
        const uint8_t *u = (const uint8_t *)msg;
        int from = (int)(u[0] | u[1] << 8 | u[2] << 16 | (uint32_t)u[3] << 24);
        on_signal(r, from, msg + 4, size - 4);
    }
}

/* ---- public API ---------------------------------------------------------- */

static void RTC_API on_log(rtcLogLevel level, const char *message)
{
    fprintf(stderr, "[h2net %d] %s\n", (int)level, message);
}

static int valid_code(const char *room_code)
{
    size_t i, rl;
    if (!room_code) return 0;
    rl = strlen(room_code);
    if (rl < 4 || rl > 24) return 0;
    for (i = 0; i < rl; i++) {
        char c = room_code[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int h2net_init(const char *relay_url)
{
    const char *e;
    size_t rl;
    if (!relay_url || !*relay_url) relay_url = H2NET_DEFAULT_RELAY;
    rl = strlen(relay_url);
    while (rl && relay_url[rl - 1] == '/') rl--;
    if (rl >= sizeof s_relay) return -1;

    lock();
    if (s_init) { unlock(); return 0; }
    s_init = 1;
    memcpy(s_relay, relay_url, rl);
    s_relay[rl] = 0;
    if (!s_rxq) s_rxq = (slot_t *)malloc(sizeof(slot_t) * H2NET_RX_QUEUE);
    s_rx_head = s_rx_tail = 0;
    s_tx = s_rx = s_drop = s_flood = 0;
    memset(s_room, 0, sizeof s_room);
    memset(s_peer, 0, sizeof s_peer);
    memset(s_mac, 0, sizeof s_mac);
    unlock();

    e = getenv("H2NET_LOG");
    rtcInitLogger(e && *e ? (rtcLogLevel)atoi(e) : RTC_LOG_NONE, e && *e ? on_log : NULL);
    return 0;
}

int h2net_find(const char *room_code)
{
    int i, r = -1;
    lock();
    for (i = 0; i < MAX_ROOMS; i++)
        if (s_room[i].used && !strcmp(s_room[i].code, room_code)) r = i;
    unlock();
    return r;
}

int h2net_join(const char *room_code)
{
    char url[512];
    rtcWsConfiguration wcfg;
    int ws, i, r = -1;

    if (!valid_code(room_code)) return -1;
    if ((r = h2net_find(room_code)) >= 0) return r;
    lock();
    if (!s_init) { unlock(); return -1; }
    for (i = 0; i < MAX_ROOMS && r < 0; i++)
        if (!s_room[i].used) r = i;
    if (r >= 0) {
        memset(&s_room[r], 0, sizeof s_room[r]);
        s_room[r].used = 1;
        snprintf(s_room[r].code, sizeof s_room[r].code, "%s", room_code);
    }
    if (snprintf(url, sizeof url, "%s/room/%s", s_relay, room_code) >= (int)sizeof url) {
        if (r >= 0) s_room[r].used = 0;
        r = -1;
    }
    unlock();
    if (r < 0) return -1;

    memset(&wcfg, 0, sizeof wcfg);
    wcfg.connectionTimeoutMs = 15000;
    ws = rtcCreateWebSocketEx(url, &wcfg);
    if (ws < 0) {
        lock();
        s_room[r].used = 0;
        unlock();
        return -1;
    }
    lock();
    s_room[r].ws = ws;
    unlock();
    rtcSetUserPointer(ws, RP(r));
    rtcSetOpenCallback(ws, on_ws_open);
    rtcSetClosedCallback(ws, on_ws_closed);
    rtcSetErrorCallback(ws, on_ws_error);
    rtcSetMessageCallback(ws, on_ws_message);
    if (rtcIsOpen(ws)) on_ws_open(ws, RP(r));
    return r;
}

void h2net_leave(const char *room_code)
{
    int r = h2net_find(room_code), ws, pcs[MAX_PEERS], dcs[MAX_PEERS], np = 0, i, j, npend;
    char *pend[MAX_PENDING];
    if (r < 0) return;
    lock();
    ws = s_room[r].ws;
    s_room[r].ws = 0;
    unlock();
    if (ws > 0) rtcDeleteWebSocket(ws);
    lock();
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id && s_peer[i].room == r) {
            detach_peer(&s_peer[i], &pcs[np], &dcs[np], pend, &npend);
            for (j = 0; j < npend; j++) free(pend[j]);
            np++;
        }
    memset(&s_room[r], 0, sizeof s_room[r]);
    unlock();
    for (i = 0; i < np; i++) destroy_handles(pcs[i], dcs[i], NULL, 0);
}

int h2net_open(const char *room_code, const char *relay_url)
{
    if (!valid_code(room_code)) return -1;
    lock();
    if (s_init) { unlock(); return -1; }
    unlock();
    if (h2net_init(relay_url) != 0) return -1;
    return h2net_join(room_code) >= 0 ? 0 : -1;
}

void h2net_send(const uint8_t *f, uint32_t len)
{
    int dcs[MAX_PEERS], n = 0, i, target;
    if (!f || len < 14 || len > H2NET_FRAME_MAX) return;
    lock();
    if (!s_init) { unlock(); return; }
    target = (f[0] & 1) ? 0 : lookup_mac(f);
    for (i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &s_peer[i];
        if (!p->id || !p->open || p->dc <= 0) continue;
        if (target && i + 1 != target) continue;
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

int h2net_room_peers(const char *room_code)
{
    int r = h2net_find(room_code), i, n = 0;
    if (r < 0) return -1;
    lock();
    for (i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id && s_peer[i].open && s_peer[i].room == r) n++;
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
    static const char *stn[] = { "new", "connecting", "connected", "disconnected", "failed", "closed" };
    room_t rooms[MAX_ROOMS];
    peer_t peers[MAX_PEERS];
    uint64_t tx, rx, drop, flood;
    size_t o = 0;
    int r, i, nr = 0;

    if (!buf || !n) return;
    lock();
    memcpy(rooms, s_room, sizeof rooms);
    memcpy(peers, s_peer, sizeof peers);
    tx = s_tx; rx = s_rx; drop = s_drop; flood = s_flood;
    unlock();

#define PUT(...) do { if (o < n) { int w_ = snprintf(buf + o, n - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_; } } while (0)
    for (r = 0; r < MAX_ROOMS; r++) {
        int np = 0, nopen = 0;
        if (!rooms[r].used) continue;
        for (i = 0; i < MAX_PEERS; i++)
            if (peers[i].id && peers[i].room == r) { np++; nopen += peers[i].open; }
        PUT("%s%s%s%s relay=%s self=%d host=%d peers=%d/%d", nr ? " | " : "", nr || r ? "room " : "",
            nr || r ? rooms[r].code : "", nr || r ? ":" : "", wsn[rooms[r].ws_state & 3],
            rooms[r].self, rooms[r].host, nopen, np);
        for (i = 0; i < MAX_PEERS; i++) {
            char lc[256], rc[256];
            if (!peers[i].id || peers[i].room != r) continue;
            PUT(" [%d:%s", peers[i].id, peers[i].open ? "open" : stn[peers[i].state >= 0 && peers[i].state <= 5 ? peers[i].state : 0]);
            if (peers[i].pc > 0 && rtcGetSelectedCandidatePair(peers[i].pc, lc, sizeof lc, rc, sizeof rc) > 0)
                PUT(" %s-%s", cand_type(lc), cand_type(rc));
            PUT("]");
        }
        if (rooms[r].err[0]) PUT(" err=\"%s\"", rooms[r].err);
        nr++;
    }
    if (!nr) PUT("relay=closed self=0 host=0 peers=0/0");
    PUT(" tx=%llu rx=%llu drop=%llu flood=%llu", (unsigned long long)tx, (unsigned long long)rx,
        (unsigned long long)drop, (unsigned long long)flood);
#undef PUT
    if (o >= n) buf[n - 1] = 0;
}

void h2net_close(void)
{
    int r;
    char code[32];
    lock();
    if (!s_init) { unlock(); return; }
    unlock();
    for (r = 0; r < MAX_ROOMS; r++) {
        lock();
        code[0] = 0;
        if (s_room[r].used) snprintf(code, sizeof code, "%s", s_room[r].code);
        unlock();
        if (code[0]) h2net_leave(code);
    }
    rtcCleanup();

    lock();
    free(s_rxq);
    s_rxq = NULL;
    s_init = 0;
    unlock();
}
