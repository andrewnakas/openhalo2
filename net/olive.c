/* olive: the Open Live facilitator's transport (see olive.h). Native builds,
 * libdatachannel's WebSocket client (the same library h2net uses). */
#include "olive.h"

#include <rtc/rtc.h>

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

#define MAX_HOLDS   16
#define MAX_VISITS  64
#define MAX_MEMBERS 16

typedef struct {
    int used, gen, ws, state;       /* state: 0 connecting, 1 welcomed, 2 closed/failed */
    int self, nmem, mem[MAX_MEMBERS];
    olive_handler h;
    void *ctx;
    char code[64];                  /* to open it again when the relay drops it */
    uint64_t retry_at;
    int drops;
} hold_t;

typedef struct {
    int from, len, rtt;
    uint8_t *buf;
} reply_t;

typedef struct {
    int used, gen, ws, done, mode;
    int self, ntarget, target[MAX_MEMBERS];
    uint8_t *msg;
    int len;
    uint64_t start, deadline, sent;
    int nrep;
    reply_t rep[OLIVE_REPLIES];
} visit_t;

static char s_relay[400];
static int s_init;
static hold_t s_hold[MAX_HOLDS];
static visit_t s_visit[MAX_VISITS];
static int s_gen;

/* user pointers: kind, a generation (so a late callback of a freed socket
 * never touches the slot's next user), slot */
#define UP_HOLD  0x1000000
#define UP_VISIT 0x2000000
#define UPTR(k, g, i) ((void *)(intptr_t)((k) | (((g) & 0xFFF) << 8) | (i)))
#define UP_KIND(p) ((int)(intptr_t)(p) & 0x3000000)
#define UP_GEN(p)  (((int)(intptr_t)(p) >> 8) & 0xFFF)
#define UP_SLOT(p) ((int)(intptr_t)(p) & 0xFF)

/* ---- JSON notices from the relay ------------------------------------------ */

static int json_int(const char *s, const char *key)
{
    const char *k = strstr(s, key);
    if (!k) return 0;
    k += strlen(key);
    while (*k == ' ' || *k == ':' || *k == '"') k++;
    return atoi(k);
}

static int json_members(const char *s, int *out, int max)
{
    const char *k = strstr(s, "\"members\"");
    int n = 0;
    if (!k || !(k = strchr(k, '['))) return 0;
    k++;
    while (*k && *k != ']' && n < max) {
        while (*k == ' ' || *k == ',') k++;
        if (*k >= '0' && *k <= '9') { out[n++] = atoi(k); while (*k >= '0' && *k <= '9') k++; }
        else if (*k != ']') k++;
    }
    return n;
}

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void send_to(int ws, int target, const uint8_t *msg, int len)
{
    uint8_t buf[4 + OLIVE_MSG_MAX];
    if (len <= 0 || len > OLIVE_MSG_MAX) return;
    put_u32(buf, (uint32_t)target);
    memcpy(buf + 4, msg, (size_t)len);
    rtcSendMessage(ws, (const char *)buf, len + 4);
}

/* ---- holds ------------------------------------------------------------------- */

static void hold_notice(hold_t *h, const char *s)
{
    int i, id;
    if (strstr(s, "\"welcome\"")) {
        h->self = json_int(s, "\"self\"");
        h->nmem = json_members(s, h->mem, MAX_MEMBERS);
        h->state = 1;
    } else if (strstr(s, "\"join\"")) {
        id = json_int(s, "\"id\"");
        if (h->nmem < MAX_MEMBERS) h->mem[h->nmem++] = id;
    } else if (strstr(s, "\"leave\"")) {
        id = json_int(s, "\"id\"");
        for (i = 0; i < h->nmem; i++)
            if (h->mem[i] == id) { h->mem[i] = h->mem[--h->nmem]; break; }
    } else if (strstr(s, "\"full\"")) {
        h->state = 2;
    }
}

static void RTC_API on_msg(int ws, const char *msg, int size, void *ptr);

static void RTC_API on_closed(int ws, void *ptr)
{
    int k = UP_KIND(ptr), g = UP_GEN(ptr), i = UP_SLOT(ptr);
    (void)ws;
    lock();
    if (k == UP_HOLD && i < MAX_HOLDS && s_hold[i].used && s_hold[i].gen == g && s_hold[i].state != 2) {
        s_hold[i].state = 2;
        s_hold[i].retry_at = now_ms() + 2000u;
        s_hold[i].drops++;
        fprintf(stderr, "  [OLIVE] hold %s dropped by the relay (%d); reopening\n", s_hold[i].code, s_hold[i].drops);
    }
    if (k == UP_VISIT && i < MAX_VISITS && s_visit[i].used && s_visit[i].gen == g) s_visit[i].done = 1;
    unlock();
}

static void RTC_API on_error(int ws, const char *err, void *ptr)
{
    (void)err;
    on_closed(ws, ptr);
}

static void visit_notice(visit_t *v, const char *s)
{
    int mem[MAX_MEMBERS], n, i, host;
    if (strstr(s, "\"full\"")) { v->done = 1; return; }
    if (!strstr(s, "\"welcome\"")) return;
    v->self = json_int(s, "\"self\"");
    host = json_int(s, "\"host\"");
    n = json_members(s, mem, MAX_MEMBERS);
    v->ntarget = 0;
    /* OLIVE_ONE asks every member too, and finishes on the first reply: the
     * relay can keep a member whose process died (killed, crashed) for a
     * while, and that ghost is the room's oldest member (its "host") when
     * the holder came back. Other visitors ignore what is not theirs. */
    (void)host;
    for (i = 0; i < n; i++)
        if (mem[i] != v->self) v->target[v->ntarget++] = mem[i];
    if (!v->ntarget) { v->done = 1; return; }
    v->sent = now_ms();
}

static void RTC_API on_msg(int ws, const char *msg, int size, void *ptr)
{
    int k = UP_KIND(ptr), g = UP_GEN(ptr), i = UP_SLOT(ptr);
    if (k == UP_HOLD) {
        olive_handler h = NULL;
        void *ctx = NULL;
        uint8_t reply[OLIVE_MSG_MAX];
        int from, n = 0;
        if (i >= MAX_HOLDS) return;
        lock();
        if (s_hold[i].used && s_hold[i].gen == g) {
            if (size < 0) hold_notice(&s_hold[i], msg);
            else { h = s_hold[i].h; ctx = s_hold[i].ctx; }
        }
        unlock();
        if (!h || size < 5) return;
        from = (int)((uint8_t)msg[0] | (uint8_t)msg[1] << 8 | (uint8_t)msg[2] << 16 | (uint32_t)(uint8_t)msg[3] << 24);
        n = h(ctx, from, (const uint8_t *)msg + 4, size - 4, reply, sizeof reply);
        if (n > 0) send_to(ws, from, reply, n);
    } else if (k == UP_VISIT) {
        int targets[MAX_MEMBERS], nt = 0, send_now = 0, j;
        uint8_t *m = NULL;
        int ml = 0;
        if (i >= MAX_VISITS) return;
        lock();
        if (s_visit[i].used && s_visit[i].gen == g && !s_visit[i].done) {
            visit_t *v = &s_visit[i];
            if (size < 0) {
                visit_notice(v, msg);
                if (!v->done && v->ntarget) {
                    nt = v->ntarget;
                    memcpy(targets, v->target, sizeof(int) * (size_t)nt);
                    m = (uint8_t *)malloc((size_t)v->len);
                    if (m) { memcpy(m, v->msg, (size_t)v->len); ml = v->len; send_now = 1; }
                }
            } else if (size >= 5 && v->nrep < OLIVE_REPLIES) {
                int from = (int)((uint8_t)msg[0] | (uint8_t)msg[1] << 8 | (uint8_t)msg[2] << 16 | (uint32_t)(uint8_t)msg[3] << 24);
                int known = 0;
                for (j = 0; j < v->ntarget; j++) if (v->target[j] == from) known = 1;
                for (j = 0; j < v->nrep; j++) if (v->rep[j].from == from) known = 0;
                if (known) {
                    reply_t *r = &v->rep[v->nrep];
                    r->buf = (uint8_t *)malloc((size_t)size - 4);
                    if (r->buf) {
                        memcpy(r->buf, msg + 4, (size_t)size - 4);
                        r->len = size - 4;
                        r->from = from;
                        r->rtt = (int)(now_ms() - v->sent);
                        v->nrep++;
                    }
                    if (v->nrep >= v->ntarget || v->mode == OLIVE_ONE) v->done = 1;
                }
            }
        }
        unlock();
        if (send_now) {
            for (j = 0; j < nt; j++) send_to(ws, targets[j], m, ml);
            free(m);
        }
    }
}

/* ---- API ----------------------------------------------------------------- */

int olive_ready(void) { return 1; }

int olive_init(const char *relay_base)
{
    size_t rl;
    if (!relay_base || !*relay_base) relay_base = "wss://skatemods.com/rooms";
    rl = strlen(relay_base);
    while (rl && relay_base[rl - 1] == '/') rl--;
    if (rl >= sizeof s_relay) return -1;
    lock();
    memcpy(s_relay, relay_base, rl);
    s_relay[rl] = 0;
    s_init = 1;
    unlock();
    return 0;
}

static int open_ws(const char *code, int kind, int gen, int slot)
{
    char url[512];
    rtcWsConfiguration cfg;
    int ws;
    snprintf(url, sizeof url, "%s/room/%s", s_relay, code);
    memset(&cfg, 0, sizeof cfg);
    cfg.connectionTimeoutMs = 10000;
    ws = rtcCreateWebSocketEx(url, &cfg);
    if (ws < 0) return -1;
    rtcSetUserPointer(ws, UPTR(kind, gen, slot));
    rtcSetClosedCallback(ws, on_closed);
    rtcSetErrorCallback(ws, on_error);
    rtcSetMessageCallback(ws, on_msg);
    return ws;
}

int olive_hold(const char *code, olive_handler h, void *ctx)
{
    int i, slot = -1, ws;
    lock();
    if (!s_init) { unlock(); return -1; }
    for (i = 0; i < MAX_HOLDS && slot < 0; i++)
        if (!s_hold[i].used) slot = i;
    if (slot >= 0) {
        memset(&s_hold[slot], 0, sizeof s_hold[slot]);
        s_hold[slot].used = 1;
        s_hold[slot].gen = ++s_gen & 0xFFF;
        s_hold[slot].ws = -1;
        s_hold[slot].h = h;
        s_hold[slot].ctx = ctx;
        strncpy(s_hold[slot].code, code, sizeof s_hold[slot].code - 1);
    }
    unlock();
    if (slot < 0) return -1;
    ws = open_ws(code, UP_HOLD, s_hold[slot].gen, slot);
    lock();
    s_hold[slot].ws = ws;
    if (ws < 0) s_hold[slot].used = 0;
    unlock();
    return ws < 0 ? -1 : slot + 1;
}

void olive_release(int handle)
{
    int ws = -1, i = handle - 1;
    if (i < 0 || i >= MAX_HOLDS) return;
    lock();
    if (s_hold[i].used) { ws = s_hold[i].ws; memset(&s_hold[i], 0, sizeof s_hold[i]); }
    unlock();
    if (ws >= 0) rtcDeleteWebSocket(ws);
}

/* Holds the relay dropped (an idle timeout, a network blip, a relay
 * restart) are opened again, under a new generation so a late callback of
 * the old socket is ignored. Called from the users' pumps. */
void olive_pump(void)
{
    int i, old[MAX_HOLDS], nold = 0;
    uint64_t t = now_ms();
    for (i = 0; i < MAX_HOLDS; i++) {
        int ws, gen;
        char code[64];
        lock();
        if (!s_hold[i].used || s_hold[i].state != 2 || t < s_hold[i].retry_at) { unlock(); continue; }
        old[nold++] = s_hold[i].ws;
        s_hold[i].gen = ++s_gen & 0xFFF;
        s_hold[i].state = 0;
        s_hold[i].nmem = 0;
        s_hold[i].ws = -1;
        s_hold[i].retry_at = t + 10000u;     /* if this one fails too */
        gen = s_hold[i].gen;
        memcpy(code, s_hold[i].code, sizeof code);
        unlock();
        ws = open_ws(code, UP_HOLD, gen, i);
        lock();
        if (s_hold[i].used && s_hold[i].gen == gen) {
            s_hold[i].ws = ws;
            if (ws < 0) s_hold[i].state = 2;
        }
        unlock();
    }
    for (i = 0; i < nold; i++) if (old[i] >= 0) rtcDeleteWebSocket(old[i]);
}

int olive_hold_members(int handle, int *oldest)
{
    int i = handle - 1, n = -1, j;
    if (oldest) *oldest = 0;
    if (i < 0 || i >= MAX_HOLDS) return -1;
    lock();
    if (s_hold[i].used && s_hold[i].state == 1) {
        n = 0;
        if (oldest) *oldest = 1;
        for (j = 0; j < s_hold[i].nmem; j++) {
            if (s_hold[i].mem[j] == s_hold[i].self) continue;
            n++;
            if (s_hold[i].mem[j] < s_hold[i].self && oldest) *oldest = 0;
        }
    }
    unlock();
    return n;
}

int olive_visit(const char *code, const uint8_t *msg, int len, int mode, int timeout_ms)
{
    int i, slot = -1, ws;
    if (len <= 0 || len > OLIVE_MSG_MAX) return -1;
    lock();
    if (!s_init) { unlock(); return -1; }
    for (i = 0; i < MAX_VISITS && slot < 0; i++)
        if (!s_visit[i].used) slot = i;
    if (slot >= 0) {
        visit_t *v = &s_visit[slot];
        memset(v, 0, sizeof *v);
        v->used = 1;
        v->gen = ++s_gen & 0xFFF;
        v->ws = -1;
        v->mode = mode;
        v->msg = (uint8_t *)malloc((size_t)len);
        if (v->msg) memcpy(v->msg, msg, (size_t)len);
        v->len = len;
        v->start = now_ms();
        v->deadline = v->start + (uint64_t)timeout_ms;
    }
    unlock();
    if (slot < 0) return -1;
    ws = open_ws(code, UP_VISIT, s_visit[slot].gen, slot);
    lock();
    s_visit[slot].ws = ws;
    if (ws < 0) s_visit[slot].done = 1;
    unlock();
    return slot + 1;
}

int olive_visit_poll(int id, int *count)
{
    int i = id - 1, done = 1;
    if (count) *count = 0;
    if (i < 0 || i >= MAX_VISITS) return 1;
    lock();
    if (s_visit[i].used) {
        if (!s_visit[i].done && now_ms() >= s_visit[i].deadline) s_visit[i].done = 1;
        done = s_visit[i].done;
        if (count) *count = s_visit[i].nrep;
    }
    unlock();
    return done;
}

int olive_visit_reply(int id, int k, uint8_t *buf, int cap, int *rtt_ms)
{
    int i = id - 1, n = -1;
    if (i < 0 || i >= MAX_VISITS) return -1;
    lock();
    if (s_visit[i].used && k >= 0 && k < s_visit[i].nrep) {
        reply_t *r = &s_visit[i].rep[k];
        n = r->len < cap ? r->len : cap;
        memcpy(buf, r->buf, (size_t)n);
        if (rtt_ms) *rtt_ms = r->rtt;
    }
    unlock();
    return n;
}

void olive_visit_free(int id)
{
    int i = id - 1, ws = -1, k;
    uint8_t *m = NULL;
    uint8_t *bufs[OLIVE_REPLIES];
    int nb = 0;
    if (i < 0 || i >= MAX_VISITS) return;
    lock();
    if (s_visit[i].used) {
        ws = s_visit[i].ws;
        m = s_visit[i].msg;
        for (k = 0; k < s_visit[i].nrep; k++) bufs[nb++] = s_visit[i].rep[k].buf;
        memset(&s_visit[i], 0, sizeof s_visit[i]);
    }
    unlock();
    if (ws >= 0) rtcDeleteWebSocket(ws);
    free(m);
    for (k = 0; k < nb; k++) free(bufs[k]);
}
