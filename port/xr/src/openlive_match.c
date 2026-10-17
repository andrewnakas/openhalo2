/* Open Live: matchmaking, QoS and the session mesh
 * (docs/OPEN_LIVE_DESIGN.md 3.4 / 3.5).
 *
 * A host's XOnlineMatchSessionCreate mints the session's key exchange key
 * with the title's own XNetCreateKey (so XNet treats it as a system-link
 * key: no Live logon behind it), joins the session's mesh room
 * "h2s-<kid>" (net/h2net.c: the virtual cable its players share) and
 * advertises the session in its matchmaking bucket: a private relay room
 * "h2q9809-<bucket>-<shard>" that every advertising host holds
 * (net/olive.c). A search visits the bucket's rooms, asks every member
 * ('Q') and gets each host's record back ('q'): session id and key, host
 * address, slots, the seven result attributes and the host's QoS reply
 * data (what the title's XNetQosListen set). XNetQosLookup is answered
 * from those records (round trip = the visit's). When the title registers
 * a session key it learned this way (XNetRegisterKey, before it connects),
 * the station joins that session's mesh room; XNet then exchanges keys
 * with the host over it exactly as in system link.
 *
 * Layouts (our definitions, measured in the title's code):
 *   attribute         16 bytes: id +0, flag +4, value (u64) +8 (0xB4A90/0xB4B80)
 *   session create    keys 1,2,3,5,6,7 = six values, 8 = flags, 9-16 =
 *                     flag bits (0xB4B80); search: x, y, min, max, party
 *                     size one-hot (0xB4A90); x is the playlist
 *   search result     +0x04 session id, +0x0C host address (36 bytes),
 *                     +0x30 key, +0x40 public open, +0x44 private open,
 *                     +0x48 public filled, +0x4C private filled, +0x50
 *                     attribute count (7) (0x8FB30); our attribute values
 *                     (7 x u64, keys 1,2,3,5,6,7,8) follow at +0x58
 *   XNADDR            36 bytes; Ethernet address at +10
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "openlive.h"

#ifdef OPENHALO2_NET
#include "olive.h"
#include "h2net.h"
#endif

extern void sub_003CD0CB(void);   /* XNetCreateKey (lifted, through its hook) */

#define TITLE_FOLDER "9809"
#define MAX_HOSTED   4
#define MAX_REMOTE   64
#define MAX_SEARCH   8
#define MAX_ATTR     16
#define QOS_BLOB     1024
#define SHARDS       3
#define SHARD_HOSTS  8          /* hosts per bucket room; the relay holds 10 */

#define RESULT_SIZE  0x90u      /* 0x58 + 7 x 8 */
#define HR_NOT_FOUND 0x80155100u   /* the title's "service unavailable" for match calls (0x90020) */

typedef struct { uint32_t id; uint64_t v; } attr_t;

typedef struct {
    int used, advertising;
    uint8_t kid[8], key[16];
    uint32_t pf, po, prf, pro;
    int nattr;
    attr_t attr[MAX_ATTR];
    int hold, shard;
    uint32_t bucket;
    char mesh[32];
} hosted_t;

typedef struct {
    int used;
    uint8_t kid[8], key[16], xnaddr[36];
    uint32_t pf, po, prf, pro;
    int nattr;
    attr_t attr[MAX_ATTR];
    uint8_t qos[QOS_BLOB];
    int qos_len, rtt;
    uint64_t xuid;
    char tag[16];
} remote_t;

typedef struct {
    int used, nvisit, done;
    int visit[SHARDS];
    uint32_t bucket, max;
    uint32_t results, count;    /* guest: pointer array then the records */
} search_t;

typedef struct { int used; uint8_t kid[8]; uint8_t data[QOS_BLOB]; int len; } listen_t;

static hosted_t s_hosted[MAX_HOSTED];
static remote_t s_remote[MAX_REMOTE];
static search_t s_search[MAX_SEARCH];
static listen_t s_listen[MAX_HOSTED * 2];
static uint8_t s_xnaddr[36];
static uint32_t s_scratch;      /* guest scratch for calls into the title's XNet */
static int s_net;               /* the facilitator is available */
static uint64_t s_last_qos_host;

#if defined(_WIN32)
static SRWLOCK s_lk = SRWLOCK_INIT;
static void lock(void)   { AcquireSRWLockExclusive(&s_lk); }
static void unlock(void) { ReleaseSRWLockExclusive(&s_lk); }
#else
#include <pthread.h>
#include <time.h>
static pthread_mutex_t s_lk = PTHREAD_MUTEX_INITIALIZER;
static void lock(void)   { pthread_mutex_lock(&s_lk); }
static void unlock(void) { pthread_mutex_unlock(&s_lk); }
#endif

/* ---- helpers ------------------------------------------------------------------ */

static void b32(char *out, const uint8_t *b, int n)
{
    static const char a[] = "0123456789abcdefghjkmnpqrstvwxyz";   /* Crockford, lower case */
    uint64_t v = 0;
    int i, bits = n * 8, o = 0;
    for (i = 0; i < n; i++) v = v << 8 | b[i];
    for (i = bits - 5; i > -5; i -= 5)
        out[o++] = a[(i >= 0 ? v >> i : v << -i) & 31];
    out[o] = 0;
}

static void mesh_name(char *out, size_t n, const uint8_t *kid)
{
    char k[16];
    b32(k, kid, 8);
    snprintf(out, n, "h2s-%s", k);
}

static void bucket_name(char *out, size_t n, uint32_t bucket, int shard)
{
    snprintf(out, n, "h2q" TITLE_FOLDER "-%u-%d", bucket, shard);
}

static void hexkid(char *o, const uint8_t *kid)
{
    int i;
    for (i = 0; i < 8; i++) sprintf(o + 2 * i, "%02x", kid[i]);
}

static void read_attrs(uint32_t va, uint32_t n, attr_t *out, int *nout)
{
    uint32_t i;
    *nout = 0;
    for (i = 0; i < n && i < MAX_ATTR; i++) {
        uint8_t *a = xo_ptr(va + 16 * i);
        memcpy(&out[i].id, a, 4);
        memcpy(&out[i].v, a + 8, 8);
        (*nout)++;
    }
}

static uint64_t attr_get(const attr_t *a, int n, uint32_t id)
{
    int i;
    for (i = 0; i < n; i++) if (a[i].id == id) return a[i].v;
    return 0;
}

static void log_attrs(const char *what, const attr_t *a, int n)
{
    char line[512];
    size_t o = 0;
    int i;
    for (i = 0; i < n && o + 32 < sizeof line; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, " %u=%lld", a[i].id, (long long)a[i].v);
    fprintf(stderr, "  [XO] %s:%s\n", what, line);
}

static hosted_t *hosted_by_kid(const uint8_t *kid)
{
    int i;
    for (i = 0; i < MAX_HOSTED; i++)
        if (s_hosted[i].used && !memcmp(s_hosted[i].kid, kid, 8)) return &s_hosted[i];
    return NULL;
}

static remote_t *remote_by_kid(const uint8_t *kid)
{
    int i;
    for (i = 0; i < MAX_REMOTE; i++)
        if (s_remote[i].used && !memcmp(s_remote[i].kid, kid, 8)) return &s_remote[i];
    return NULL;
}

static listen_t *listen_by_kid(const uint8_t *kid, int make)
{
    int i, free_i = -1;
    for (i = 0; i < (int)(sizeof s_listen / sizeof s_listen[0]); i++) {
        if (s_listen[i].used && !memcmp(s_listen[i].kid, kid, 8)) return &s_listen[i];
        if (!s_listen[i].used && free_i < 0) free_i = i;
    }
    if (!make) return NULL;
    if (free_i < 0) free_i = 0;
    memset(&s_listen[free_i], 0, sizeof s_listen[free_i]);
    s_listen[free_i].used = 1;
    memcpy(s_listen[free_i].kid, kid, 8);
    return &s_listen[free_i];
}

/* ---- the session record ('q') -------------------------------------------------------
 * 'q' 1, kid[8] key[16] xnaddr[36], u32 slots x4, u8 nattr + nattr x (u32 id,
 * u64 value), u16 qos length + data, u64 host xuid, 16 chars gamertag */

static void put32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }
static void put64(uint8_t **p, uint64_t v) { memcpy(*p, &v, 8); *p += 8; }
static uint32_t get32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static uint64_t get64(const uint8_t **p) { uint64_t v; memcpy(&v, *p, 8); *p += 8; return v; }

static int record_build(const hosted_t *h, uint8_t *out, int cap)
{
    uint8_t *p = out;
    const listen_t *l;
    int i, ql;
    if (cap < 200 + QOS_BLOB) return 0;
    *p++ = 'q'; *p++ = 1;
    memcpy(p, h->kid, 8); p += 8;
    memcpy(p, h->key, 16); p += 16;
    memcpy(p, s_xnaddr, 36); p += 36;
    put32(&p, h->pf); put32(&p, h->po); put32(&p, h->prf); put32(&p, h->pro);
    *p++ = (uint8_t)h->nattr;
    for (i = 0; i < h->nattr; i++) { put32(&p, h->attr[i].id); put64(&p, h->attr[i].v); }
    l = listen_by_kid(h->kid, 0);
    ql = l ? l->len : 0;
    *p++ = (uint8_t)ql; *p++ = (uint8_t)(ql >> 8);
    if (ql) { memcpy(p, l->data, (size_t)ql); p += ql; }
    put64(&p, xo_self_xuid());
    memset(p, 0, 16);
    strncpy((char *)p, xo_self_gamertag(), 15);
    p += 16;
    return (int)(p - out);
}

static int record_parse(const uint8_t *in, int len, remote_t *r)
{
    const uint8_t *p = in, *end = in + len;
    int i, ql;
    if (len < 2 + 8 + 16 + 36 + 16 + 1 || p[0] != 'q' || p[1] != 1) return 0;
    p += 2;
    memset(r, 0, sizeof *r);
    memcpy(r->kid, p, 8); p += 8;
    memcpy(r->key, p, 16); p += 16;
    memcpy(r->xnaddr, p, 36); p += 36;
    r->pf = get32(&p); r->po = get32(&p); r->prf = get32(&p); r->pro = get32(&p);
    r->nattr = *p++;
    if (r->nattr > MAX_ATTR || p + r->nattr * 12 + 2 > end) return 0;
    for (i = 0; i < r->nattr; i++) { r->attr[i].id = get32(&p); r->attr[i].v = get64(&p); }
    ql = p[0] | p[1] << 8; p += 2;
    if (ql > QOS_BLOB || p + ql + 24 > end) return 0;
    memcpy(r->qos, p, (size_t)ql); p += ql;
    r->qos_len = ql;
    r->xuid = get64(&p);
    memcpy(r->tag, p, 15);
    r->used = 1;
    return 1;
}

/* the bucket room's holder: answers a search's 'Q' with this session */
static int on_bucket(void *ctx, int from, const uint8_t *msg, int len, uint8_t *reply, int cap)
{
    int i = (int)(intptr_t)ctx, n = 0;
    (void)from;
    if (len < 1 || msg[0] != 'Q') return 0;
    lock();
    if (i >= 0 && i < MAX_HOSTED && s_hosted[i].used && s_hosted[i].advertising)
        n = record_build(&s_hosted[i], reply, cap);
    unlock();
    return n;
}

/* ---- advertising ----------------------------------------------------------------- */

static void advert_start(int i)
{
#ifdef OPENHALO2_NET
    hosted_t *h = &s_hosted[i];
    char room[32];
    if (!s_net) return;
    bucket_name(room, sizeof room, h->bucket, h->shard);
    h->hold = olive_hold(room, on_bucket, (void *)(intptr_t)i);
    h->advertising = h->hold > 0;
    fprintf(stderr, "  [XO] advertising session in %s\n", room);
#else
    (void)i;
#endif
}

static void advert_stop(int i)
{
#ifdef OPENHALO2_NET
    if (s_hosted[i].hold > 0) olive_release(s_hosted[i].hold);
#endif
    s_hosted[i].hold = 0;
    s_hosted[i].advertising = 0;
}

static void mesh_join(const uint8_t *kid, const char *why)
{
#ifdef OPENHALO2_NET
    char room[32];
    mesh_name(room, sizeof room, kid);
    if (h2net_find(room) < 0) {
        int r = h2net_join(room);
        fprintf(stderr, "  [XO] mesh: joined %s (%s)%s\n", room, why, r < 0 ? " FAILED" : "");
    }
#else
    (void)kid; (void)why;
#endif
}

static void mesh_leave(const uint8_t *kid)
{
#ifdef OPENHALO2_NET
    char room[32];
    mesh_name(room, sizeof room, kid);
    if (h2net_find(room) >= 0) {
        h2net_leave(room);
        fprintf(stderr, "  [XO] mesh: left %s\n", room);
    }
#else
    (void)kid;
#endif
}

/* ---- the API ------------------------------------------------------------------------ */

static uint32_t poll_done(int user) { (void)user; return XO_TASK_SUCCESS; }

static int h_session_create(void)   /* (pub filled, pub open, priv filled, priv open, n, attrs, event, &task) */
{
    hosted_t *h = NULL;
    uint32_t args[2], hdl;
    int i, idx = -1;
    char k[20];
    for (i = 0; i < MAX_HOSTED && idx < 0; i++)
        if (!s_hosted[i].used) idx = i;
    if (idx < 0) { xo_return(XO_HR_FAIL, 8); return 1; }
    h = &s_hosted[idx];
    memset(h, 0, sizeof *h);
    /* the title's own XNetCreateKey: a key XNet accepts without a Live logon */
    args[0] = s_scratch; args[1] = s_scratch + 8;
    xo_guest_call(sub_003CD0CB, 2, args);
    memcpy(h->kid, xo_ptr(s_scratch), 8);
    memcpy(h->key, xo_ptr(s_scratch + 8), 16);
    h->pf = xo_arg(0); h->po = xo_arg(1); h->prf = xo_arg(2); h->pro = xo_arg(3);
    read_attrs(xo_arg(5), xo_arg(4), h->attr, &h->nattr);
    h->bucket = (uint32_t)attr_get(h->attr, h->nattr, 1);
    mesh_name(h->mesh, sizeof h->mesh, h->kid);
    lock();
    h->used = 1;
    unlock();
    hexkid(k, h->kid);
    fprintf(stderr, "  [XO] session %s created: slots %u+%u public, %u+%u private, bucket %u\n",
            k, h->pf, h->po, h->prf, h->pro, h->bucket);
    log_attrs("  attributes", h->attr, h->nattr);
    advert_start(idx);
    hdl = xo_task_new_polled(0x3A06A8, poll_done, idx, NULL);
    if (xo_arg(7)) xo_wr32(xo_arg(7), hdl);
    xo_return(XO_HR_OK, 8);
    return 1;
}

static int h_session_info(void)     /* (task, &kid, &key) */
{
    int i = xo_task_user(xo_arg(0));
    if (i < 0 || i >= MAX_HOSTED || !s_hosted[i].used) return 0;
    if (xo_arg(1)) memcpy(xo_ptr(xo_arg(1)), s_hosted[i].kid, 8);
    if (xo_arg(2)) memcpy(xo_ptr(xo_arg(2)), s_hosted[i].key, 16);
    xo_return(XO_HR_OK, 3);
    return 1;
}

static int h_session_update(void)   /* (kid lo, kid hi, pf, po, prf, pro, n, attrs, event, &task) */
{
    uint8_t kid[8];
    hosted_t *h;
    uint32_t lo = xo_arg(0), hi = xo_arg(1), hdl, bucket;
    memcpy(kid, &lo, 4); memcpy(kid + 4, &hi, 4);
    h = hosted_by_kid(kid);
    if (!h) { xo_return(HR_NOT_FOUND, 10); return 1; }
    lock();
    h->pf = xo_arg(2); h->po = xo_arg(3); h->prf = xo_arg(4); h->pro = xo_arg(5);
    read_attrs(xo_arg(7), xo_arg(6), h->attr, &h->nattr);
    unlock();
    bucket = (uint32_t)attr_get(h->attr, h->nattr, 1);
    if (bucket != h->bucket || !h->advertising) {
        int i = (int)(h - s_hosted);
        advert_stop(i);
        h->bucket = bucket;
        h->shard = 0;
        advert_start(i);
    }
    hdl = xo_task_new_polled(0x3A06B7, poll_done, (int)(h - s_hosted), NULL);
    if (xo_arg(9)) xo_wr32(xo_arg(9), hdl);
    xo_return(XO_HR_OK, 10);
    return 1;
}

static int h_session_delete(void)   /* (kid lo, kid hi, event, &task) */
{
    uint8_t kid[8];
    hosted_t *h;
    uint32_t lo = xo_arg(0), hi = xo_arg(1), hdl;
    memcpy(kid, &lo, 4); memcpy(kid + 4, &hi, 4);
    h = hosted_by_kid(kid);
    if (h) {
        advert_stop((int)(h - s_hosted));
        fprintf(stderr, "  [XO] session advertisement withdrawn\n");
    }
    hdl = xo_task_new(0x3A06F2, 1, XO_TASK_SUCCESS);
    if (xo_arg(3)) xo_wr32(xo_arg(3), hdl);
    xo_return(XO_HR_OK, 4);
    return 1;
}

/* ---- searching ---------------------------------------------------------------------- */

static void search_finish(search_t *s)
{
#ifdef OPENHALO2_NET
    uint8_t buf[OLIVE_MSG_MAX];
    remote_t found[MAX_REMOTE];
    int nf = 0, v, k, cnt, rtt, n;
    for (v = 0; v < s->nvisit; v++) {
        olive_visit_poll(s->visit[v], &cnt);
        for (k = 0; k < cnt && nf < MAX_REMOTE; k++) {
            n = olive_visit_reply(s->visit[v], k, buf, sizeof buf, &rtt);
            if (n > 0 && record_parse(buf, n, &found[nf])) {
                found[nf].rtt = rtt > 0 ? rtt : 1;
                nf++;
            }
        }
        olive_visit_free(s->visit[v]);
        s->visit[v] = 0;
    }
    /* remember them (QoS and the mesh need them later), newest wins */
    for (k = 0; k < nf; k++) {
        remote_t *r = remote_by_kid(found[k].kid);
        int i;
        if (!r) for (i = 0; i < MAX_REMOTE && !r; i++) if (!s_remote[i].used) r = &s_remote[i];
        if (!r) r = &s_remote[k % MAX_REMOTE];
        *r = found[k];
    }
    if ((uint32_t)nf > s->max) nf = (int)s->max;
    s->count = (uint32_t)nf;
    s->results = xo_alloc(4u * (uint32_t)(nf ? nf : 1) + RESULT_SIZE * (uint32_t)(nf ? nf : 1));
    for (k = 0; k < nf; k++) {
        uint32_t rec = s->results + 4u * (uint32_t)nf + RESULT_SIZE * (uint32_t)k;
        uint8_t *p = xo_ptr(rec);
        static const uint32_t keys[7] = { 1, 2, 3, 5, 6, 7, 8 };
        int a;
        xo_wr32(s->results + 4u * (uint32_t)k, rec);
        memcpy(p + 0x04, found[k].kid, 8);
        memcpy(p + 0x0C, found[k].xnaddr, 36);
        memcpy(p + 0x30, found[k].key, 16);
        memcpy(p + 0x40, &found[k].po, 4);
        memcpy(p + 0x44, &found[k].pro, 4);
        memcpy(p + 0x48, &found[k].pf, 4);
        memcpy(p + 0x4C, &found[k].prf, 4);
        xo_wr32(rec + 0x50, 7);
        for (a = 0; a < 7; a++) {
            uint64_t val = attr_get(found[k].attr, found[k].nattr, keys[a]);
            memcpy(p + 0x58 + 8 * a, &val, 8);
        }
        {
            char kh[20];
            hexkid(kh, found[k].kid);
            fprintf(stderr, "  [XO] search result %d: session %s host \"%s\" rtt %d ms, %u/%u public\n",
                    k, kh, found[k].tag, found[k].rtt, found[k].pf, found[k].pf + found[k].po);
        }
    }
    fprintf(stderr, "  [XO] search in bucket %u: %d session(s)\n", s->bucket, nf);
#endif
    s->done = 1;
}

static uint32_t poll_search(int i)
{
    search_t *s = &s_search[i];
    int v, cnt, all = 1;
    if (!s->used) return XO_HR_FAIL;
    if (s->done) return XO_TASK_SUCCESS;
#ifdef OPENHALO2_NET
    for (v = 0; v < s->nvisit; v++)
        if (!olive_visit_poll(s->visit[v], &cnt)) all = 0;
#else
    (void)v; (void)cnt;
#endif
    if (!all) return 0;
    search_finish(s);
    return XO_TASK_SUCCESS;
}

static void close_search(int i)
{
    search_t *s = &s_search[i];
    int v;
    if (!s->used) return;
#ifdef OPENHALO2_NET
    for (v = 0; v < s->nvisit; v++) if (s->visit[v] > 0) olive_visit_free(s->visit[v]);
#else
    (void)v;
#endif
    if (s->results) xo_free(s->results);
    memset(s, 0, sizeof *s);
}

static int h_search(void)   /* (procedure, max results, n, attrs, results size, event, &task) */
{
    attr_t a[MAX_ATTR];
    int n, i, idx = -1;
    search_t *s;
    uint32_t hdl;
    read_attrs(xo_arg(3), xo_arg(2), a, &n);
    for (i = 0; i < MAX_SEARCH && idx < 0; i++)
        if (!s_search[i].used) idx = i;
    if (idx < 0) { xo_return(XO_HR_FAIL, 7); return 1; }
    s = &s_search[idx];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->max = xo_arg(1);
    s->bucket = n > 0 ? (uint32_t)a[0].v : 0;
    log_attrs("search", a, n);
#ifdef OPENHALO2_NET
    if (s_net) {
        static const uint8_t q[2] = { 'Q', 1 };
        for (i = 0; i < SHARDS; i++) {
            char room[32];
            bucket_name(room, sizeof room, s->bucket, i);
            s->visit[s->nvisit] = olive_visit(room, q, 2, OLIVE_ALL, 3000);
            if (s->visit[s->nvisit] > 0) s->nvisit++;
        }
    }
#endif
    hdl = xo_task_new_polled(0x3A072E, poll_search, idx, close_search);
    if (xo_arg(6)) xo_wr32(xo_arg(6), hdl);
    xo_return(XO_HR_OK, 7);
    return 1;
}

static int h_search_len(void)   /* (max results, n attributes, specs) */
{
    xo_return(xo_arg(0) * (4 + RESULT_SIZE) + 16, 3);
    return 1;
}

static int h_search_results(void)   /* (task, &results, &count) */
{
    int i = xo_task_user(xo_arg(0));
    if (i < 0 || i >= MAX_SEARCH || !s_search[i].used) return 0;
    if (xo_arg(1)) xo_wr32(xo_arg(1), s_search[i].results);
    if (xo_arg(2)) xo_wr32(xo_arg(2), s_search[i].count);
    xo_return(XO_HR_OK, 3);
    return 1;
}

/* Joining a friend's or an inviter's session by its id (0x900E0; the title
 * then reads it with XOnlineMatchSearchGetResults): ask the friend whose
 * presence or invite carries that id, at their inbox ('S'). */
static int h_find_from_id(void)   /* (kid lo, kid hi, event, &task) */
{
    uint8_t kid[8];
    uint32_t lo = xo_arg(0), hi = xo_arg(1), hdl;
    char room[32], kh[20];
    int i, idx = -1;
    search_t *s;
    memcpy(kid, &lo, 4); memcpy(kid + 4, &hi, 4);
    for (i = 0; i < MAX_SEARCH && idx < 0; i++)
        if (!s_search[i].used) idx = i;
    if (idx < 0) { xo_return(XO_HR_FAIL, 4); return 1; }
    s = &s_search[idx];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->max = 1;
    hexkid(kh, kid);
#ifdef OPENHALO2_NET
    if (s_net && xo_friends_inbox_for_kid(kid, room, sizeof room)) {
        uint8_t m[40];
        memset(m, 0, sizeof m);
        m[0] = 'S';
        memcpy(m + 25, kid, 8);
        s->visit[0] = olive_visit(room, m, 33, OLIVE_ONE, 4000);
        if (s->visit[0] > 0) s->nvisit = 1;
        fprintf(stderr, "  [XO] finding session %s: asking %s\n", kh, room);
    } else {
        fprintf(stderr, "  [XO] finding session %s: no friend is in it\n", kh);
    }
#else
    (void)room;
#endif
    hdl = xo_task_new_polled(0x3A0710, poll_search, idx, close_search);
    if (xo_arg(3)) xo_wr32(xo_arg(3), hdl);
    xo_return(XO_HR_OK, 4);
    return 1;
}

int xo_match_record_for_kid(const uint8_t *kid, uint8_t *out, int cap)
{
    hosted_t *h;
    int n = 0;
    lock();
    h = hosted_by_kid(kid);
    if (h) n = record_build(h, out, cap);
    unlock();
    return n;
}

static int h_search_parse(void)   /* (result, n, specs, values out) */
{
    uint32_t r = xo_arg(0), n = xo_arg(1), out = xo_arg(3);
    if (n > 7) n = 7;
    if (out) memcpy(xo_ptr(out), xo_ptr(r + 0x58), 8 * n);
    xo_return(XO_HR_OK, 4);
    return 1;
}

/* ---- QoS --------------------------------------------------------------------------------- */

static int h_qos_lookup(void)   /* (n, xnaddrs, kids, keys, n ina, ina, service ids, probes, bps, flags, event, &xnqos) */
{
    uint32_t n = xo_arg(0), kids = xo_arg(2), q, extra = 0, at, i;
    const remote_t *r[64];
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        uint8_t kid[8];
        memcpy(kid, xo_ptr(xo_rd32(kids + 4 * i)), 8);
        r[i] = remote_by_kid(kid);
        if (r[i]) { extra += (uint32_t)(r[i]->qos_len + 3) & ~3u; s_last_qos_host = r[i]->xuid; }
    }
    q = xo_qos_alloc(n, extra);
    at = q + 8 + 24 * (n ? n : 1);
    for (i = 0; i < n; i++) {
        if (r[i]) {
            memcpy(xo_ptr(at), r[i]->qos, (size_t)r[i]->qos_len);
            /* complete, contacted, data received */
            xo_qos_entry(q, i, 0x01 | 0x02 | (r[i]->qos_len ? 0x08 : 0), (uint16_t)r[i]->rtt,
                         1000000u, 8000000u, r[i]->qos_len ? at : 0, (uint16_t)r[i]->qos_len);
            at += (uint32_t)(r[i]->qos_len + 3) & ~3u;
        } else {
            xo_qos_entry(q, i, 0x01 | 0x04, 0, 0, 0, 0, 0);   /* complete, target disabled */
        }
    }
    xo_wr32(q + 4, 0);   /* none pending */
    if (xo_arg(11)) xo_wr32(xo_arg(11), q);
    fprintf(stderr, "  [XO] QoS lookup of %u target(s): answered from the search records\n", n);
    xo_return(0, 12);
    return 1;
}

/* ---- sessions bigger than a relay room (docs/OPEN_LIVE_DESIGN.md 3.4) -------------------
 * The relay holds 10 members a room, Halo 2 sessions up to 16. A station the
 * mesh room turns away ("full"; RECOMP_OL_OVERFLOW=1 forces it, for tests)
 * joins the session through an external h2net room "x<kid>" instead, whose
 * signaling goes through the players' inboxes (net/olive.c), and connects to
 * every member itself:
 *   'M' kid[8] flag   to the session's host: "I am in this session" (flag 1:
 *                     outside the mesh room); the host keeps a roster
 *   'R' kid[8]        to the host: the roster -> 'r' kid[8] n, n x (xuid u64,
 *                     flag u8), the host first
 *   'N' kid[8] (u16 len, h2net signaling)...   to a member's inbox
 * An outside station offers to every room member (the roster, re-read every
 * 5 s) and to the outside members with a higher id; room members answer. The host of a key the title
 * registers is the remote whose QoS data carries it (the joiner registers
 * the host's network session key from that data). */

#define MAX_ROSTER   64
#define MAX_XID      64
#define MAX_OUT      16
#define MAX_MESH     8
#define OLIVE_MSG_MAX_SAFE 3000

static uint64_t ov_now(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

typedef struct { int used; uint8_t kid[8]; uint64_t xuid; int outside; } roster_t;
typedef struct { int used; uint8_t kid[8]; uint64_t host; int outside, sent_m, visit_m, visit_r; uint64_t next_r; } mesh_t;
typedef struct { int used; uint64_t xuid; uint8_t kid[8]; int len, visit; uint8_t buf[OLIVE_MSG_MAX_SAFE]; } outbox_t;

static roster_t s_roster[MAX_ROSTER];

static int kid_eq(const uint8_t *a, const uint8_t *b)
{
    return !memcmp(a + 1, b + 1, 7) && ((a[0] ^ b[0]) & 0x7F) == 0;
}
static uint64_t s_xid[MAX_XID];          /* h2net member ids of external rooms: index + 1 */
static mesh_t s_mesh[MAX_MESH];
static outbox_t s_out[MAX_OUT];
static int s_force_overflow = -1;
static int s_n_in, s_n_out, s_r_got;     /* diagnostics */
static uint64_t s_ov_log;

static int xid_of(uint64_t x)
{
    int i;
    for (i = 0; i < MAX_XID; i++) if (s_xid[i] == x) return i + 1;
    for (i = 0; i < MAX_XID; i++) if (!s_xid[i]) { s_xid[i] = x; return i + 1; }
    return 0;
}

static void ext_name(char *out, size_t n, const uint8_t *kid)
{
    char k[16];
    b32(k, kid, 8);
    snprintf(out, n, "x%s", k);
}

static void inbox_of(char *out, size_t n, uint64_t xuid)
{
    static const char a[] = "0123456789abcdefghjkmnpqrstvwxyz";
    char k[16];
    int i, o = 0;
    for (i = 60; i > -5; i -= 5) k[o++] = a[(i >= 0 ? xuid >> i : xuid << -i) & 31];
    k[o] = 0;
    snprintf(out, n, "h2u-%s", k);
}

static int head(uint8_t *m, char type)
{
    uint64_t self = xo_self_xuid();
    m[0] = (uint8_t)type;
    memcpy(m + 1, &self, 8);
    memset(m + 9, 0, 16);
    strncpy((char *)m + 9, xo_self_gamertag(), 15);
    return 25;
}

#ifdef OPENHALO2_NET
/* h2net's signaling for an external room (any thread): queued per member,
 * sent by the pump in one inbox visit */
static int ext_send(void *ctx, int target, const uint8_t *msg, int len)
{
    mesh_t *m = &s_mesh[(int)(intptr_t)ctx];
    outbox_t *o = NULL;
    uint64_t x;
    int i;
    if (target <= 0 || target > MAX_XID || len <= 0) return -1;
    lock();
    x = s_xid[target - 1];
    for (i = 0; i < MAX_OUT && !o; i++)
        if (s_out[i].used && s_out[i].xuid == x && !memcmp(s_out[i].kid, m->kid, 8) && !s_out[i].visit
            && s_out[i].len + 2 + len <= (int)sizeof s_out[i].buf) o = &s_out[i];
    for (i = 0; i < MAX_OUT && !o; i++)
        if (!s_out[i].used) { o = &s_out[i]; memset(o, 0, sizeof *o); o->used = 1; o->xuid = x; memcpy(o->kid, m->kid, 8); }
    if (o) {
        o->buf[o->len] = (uint8_t)len; o->buf[o->len + 1] = (uint8_t)(len >> 8);
        memcpy(o->buf + o->len + 2, msg, (size_t)len);
        o->len += 2 + len;
    }
    unlock();
    return o ? 0 : -1;
}
#endif

static roster_t *roster_add(const uint8_t *kid, uint64_t xuid, int outside)
{
    int i;
    roster_t *r = NULL;
    for (i = 0; i < MAX_ROSTER && !r; i++)
        if (s_roster[i].used && s_roster[i].xuid == xuid && kid_eq(s_roster[i].kid, kid)) r = &s_roster[i];
    for (i = 0; i < MAX_ROSTER && !r; i++)
        if (!s_roster[i].used) { r = &s_roster[i]; r->used = 1; memcpy(r->kid, kid, 8); r->xuid = xuid; }
    if (r) r->outside = outside;
    return r;
}

/* the host of a registered key: the remote whose QoS data carries it */
static uint64_t host_of_key(const uint8_t *kid)
{
    int i, j;
    if (hosted_by_kid(kid)) return xo_self_xuid();
    for (i = 0; i < MAX_REMOTE; i++) {
        const remote_t *r = &s_remote[i];
        if (!r->used) continue;
        if (!memcmp(r->kid + 1, kid + 1, 7) && ((r->kid[0] ^ kid[0]) & 0x7F) == 0) return r->xuid;
        for (j = 0; j + 8 <= r->qos_len; j++)      /* the online-peer bit (0x7A8D7) aside */
            if (!memcmp(r->qos + j + 1, kid + 1, 7) && ((r->qos[j] ^ kid[0]) & 0x7F) == 0) return r->xuid;
    }
    return s_last_qos_host;
}

/* a session key the title registered: track it (mesh_join already joined its room) */
static void mesh_track(const uint8_t *kid)
{
    int i;
    mesh_t *m = NULL;
    if (s_force_overflow < 0) {
        const char *e = getenv("RECOMP_OL_OVERFLOW");
        s_force_overflow = e && *e && *e != '0';
    }
    for (i = 0; i < MAX_MESH && !m; i++) if (s_mesh[i].used && !memcmp(s_mesh[i].kid, kid, 8)) m = &s_mesh[i];
    for (i = 0; i < MAX_MESH && !m; i++) if (!s_mesh[i].used) m = &s_mesh[i];
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->used = 1;
    memcpy(m->kid, kid, 8);
    m->host = host_of_key(kid);
}

static void mesh_untrack(const uint8_t *kid)
{
    int i;
    for (i = 0; i < MAX_MESH; i++)
        if (s_mesh[i].used && !memcmp(s_mesh[i].kid, kid, 8)) {
#ifdef OPENHALO2_NET
            char room[32];
            ext_name(room, sizeof room, kid);
            if (h2net_find(room) >= 0) h2net_leave(room);
            if (s_mesh[i].visit_m > 0) olive_visit_free(s_mesh[i].visit_m);
            if (s_mesh[i].visit_r > 0) olive_visit_free(s_mesh[i].visit_r);
#endif
            memset(&s_mesh[i], 0, sizeof s_mesh[i]);
        }
}

#ifdef OPENHALO2_NET
static void go_outside(mesh_t *m)
{
    char room[32], ext[32];
    ext_name(ext, sizeof ext, m->kid);
    if (h2net_join_ext(ext, ext_send, (void *)(intptr_t)(m - s_mesh)) < 0) {
        m->outside = -1;        /* no external rooms here (the web build): stay */
        return;
    }
    mesh_name(room, sizeof room, m->kid);
    if (h2net_find(room) >= 0) h2net_leave(room);
    memcpy(room, ext, sizeof room);
    m->outside = 1;
    m->sent_m = 0;          /* tell the host again, flagged */
    m->next_r = 0;
    fprintf(stderr, "  [XO] mesh: session %s is outside its relay room: peers through inboxes (%s)\n",
            room + 1, s_force_overflow ? "forced" : "room full");
}

/* the roster from the host: connect to whom this station must */
static void roster_take(mesh_t *m, const uint8_t *r, int n)
{
    char room[32];
    int k, cnt, made = 0;
    if (n < 10 || r[0] != 'r' || !kid_eq(r + 1, m->kid)) return;
    cnt = r[9];
    if (10 + cnt * 9 > n) return;
    ext_name(room, sizeof room, m->kid);
    for (k = 0; k < cnt; k++) {
        uint64_t x;
        int outside = r[10 + 9 * k + 8], id;
        memcpy(&x, r + 10 + 9 * k, 8);
        if (x == xo_self_xuid()) continue;
        /* one side offers: an outside station to everyone in the room; of
         * two outside stations, the lower id. Room members only answer. */
        if (m->outside <= 0) continue;
        if (outside && x < xo_self_xuid()) continue;
        if (h2net_find(room) < 0) h2net_join_ext(room, ext_send, (void *)(intptr_t)(m - s_mesh));
        id = xid_of(x);
        if (id && !h2net_ext_has(room, id)) { h2net_ext_connect(room, id); made++; }
    }
    if (made) fprintf(stderr, "  [XO] mesh: connecting to %d member(s) of %s through their inboxes\n", made, room + 1);
}
#endif

/* every pump: room full -> outside; tell the host; fetch the roster; send queued signaling */
static void overflow_pump(void)
{
#ifdef OPENHALO2_NET
    int i, cnt;
    uint64_t t = ov_now();
    for (i = 0; i < MAX_MESH; i++) {
        mesh_t *m = &s_mesh[i];
        char room[32];
        if (!m->used) continue;
        if (!m->host && (m->host = host_of_key(m->kid)) != 0) {
            char k[20];
            hexkid(k, m->kid);
            fprintf(stderr, "  [XO] mesh: session key %s is hosted by %016llx\n", k, (unsigned long long)m->host);
        }
        mesh_name(room, sizeof room, m->kid);
        if (m->outside == 0 && m->host && m->host != xo_self_xuid()
            && (s_force_overflow > 0 || h2net_room_full(room)))
            go_outside(m);
        if (!m->host || m->host == xo_self_xuid()) continue;
        if (!m->sent_m && m->visit_m <= 0) {
            uint8_t msg[40];
            int n = head(msg, 'M');
            memcpy(msg + n, m->kid, 8); n += 8;
            msg[n++] = (uint8_t)(m->outside > 0);
            inbox_of(room, sizeof room, m->host);
            m->visit_m = olive_visit(room, msg, n, OLIVE_ONE, 4000);
        } else if (m->visit_m > 0 && olive_visit_poll(m->visit_m, &cnt)) {
            olive_visit_free(m->visit_m);
            m->visit_m = 0;
            m->sent_m = cnt > 0;
            if (!cnt) m->next_r = t + 3000;
        }
        if (m->sent_m && m->visit_r <= 0 && t >= m->next_r) {
            uint8_t msg[40];
            int n = head(msg, 'R');
            memcpy(msg + n, m->kid, 8); n += 8;
            inbox_of(room, sizeof room, m->host);
            m->visit_r = olive_visit(room, msg, n, OLIVE_ONE, 4000);
        } else if (m->visit_r > 0 && olive_visit_poll(m->visit_r, &cnt)) {
            uint8_t r[OLIVE_MSG_MAX];
            int n = cnt > 0 ? olive_visit_reply(m->visit_r, 0, r, sizeof r, NULL) : -1;
            olive_visit_free(m->visit_r);
            m->visit_r = 0;
            m->next_r = t + 5000;        /* members come and go: ask again now and then */
            if (n > 0) { s_r_got++; roster_take(m, r, n); }
        }
    }
    if (t >= s_ov_log) {
        int any = 0;
        for (i = 0; i < MAX_MESH; i++) {
            char room[32], st[600];
            if (!s_mesh[i].used) continue;
            ext_name(room, sizeof room, s_mesh[i].kid);
            if (h2net_find(room) < 0) continue;
            any = 1;
            h2net_status(st, sizeof st);
            fprintf(stderr, "  [XO] mesh %s: host %016llx outside %d, M %d, rosters %d, N in %d out %d | %s\n",
                    room, (unsigned long long)s_mesh[i].host, s_mesh[i].outside, s_mesh[i].sent_m, s_r_got,
                    s_n_in, s_n_out, st);
        }
        s_ov_log = t + (any ? 5000 : 1000);
    }
    for (i = 0; i < MAX_OUT; i++) {
        outbox_t *o = &s_out[i];
        if (!o->used) continue;
        if (o->visit > 0) {
            if (olive_visit_poll(o->visit, &cnt)) { olive_visit_free(o->visit); memset(o, 0, sizeof *o); }
        } else if (o->len) {
            uint8_t msg[OLIVE_MSG_MAX];
            char room[32];
            int n, j, busy = 0;
            /* in order: the next batch to a member waits for the one on its way */
            for (j = 0; j < MAX_OUT; j++)
                if (j != i && s_out[j].used && s_out[j].visit > 0 && s_out[j].xuid == o->xuid) busy = 1;
            if (busy) continue;
            lock();
            n = head(msg, 'N');
            memcpy(msg + n, o->kid, 8); n += 8;
            memcpy(msg + n, o->buf, (size_t)o->len); n += o->len;
            o->len = 0;
            unlock();
            inbox_of(room, sizeof room, o->xuid);
            o->visit = olive_visit(room, msg, n, OLIVE_ONE, 4000);
            s_n_out++;
            if (o->visit <= 0) memset(o, 0, sizeof *o);
        }
    }
#endif
}

/* inbox messages for sessions ('M', 'R', 'N'), on the transport thread */
int xo_match_inbox(char type, uint64_t from, const uint8_t *p, int len, uint8_t *reply, int cap)
{
    int n = 0, i;
    if (len < 8) return 0;
    switch (type) {
    case 'M':
        lock();
        roster_add(p, from, len > 8 ? p[8] : 0);
        unlock();
        reply[0] = 'k';
        return 1;
    case 'R':
        reply[n++] = 'r';
        memcpy(reply + n, p, 8); n += 8;
        reply[n++] = 0;
        {
            uint64_t self = xo_self_xuid();
            memcpy(reply + n, &self, 8); n += 8;
            reply[n++] = 0;
            reply[9]++;
        }
        lock();
        for (i = 0; i < MAX_ROSTER && n + 9 <= cap && reply[9] < 255; i++) {
            if (!s_roster[i].used || !kid_eq(s_roster[i].kid, p)) continue;
            memcpy(reply + n, &s_roster[i].xuid, 8); n += 8;
            reply[n++] = (uint8_t)s_roster[i].outside;
            reply[9]++;
        }
        unlock();
        return n;
    case 'N':
#ifdef OPENHALO2_NET
    {
        char room[32];
        int k = 8, id, mi = -1;
        ext_name(room, sizeof room, p);
        lock();
        for (i = 0; i < MAX_MESH && mi < 0; i++) if (s_mesh[i].used && kid_eq(s_mesh[i].kid, p)) mi = i;
        id = xid_of(from);
        unlock();
        if (mi < 0 || !id) { reply[0] = 'k'; return 1; }   /* not in that session (any more) */
        if (h2net_find(room) < 0) h2net_join_ext(room, ext_send, (void *)(intptr_t)mi);
        s_n_in++;
        while (k + 2 <= len) {
            int l = p[k] | p[k + 1] << 8;
            if (k + 2 + l > len) break;
            h2net_ext_signal(room, id, p + k + 2, l);
            k += 2 + l;
        }
    }
#endif
        reply[0] = 'k';
        return 1;
    default:
        return 0;
    }
}

/* ---- dispatch ------------------------------------------------------------------------------ */

int xo_match_call(uint32_t va)
{
    switch (va) {
    case 0x3A06A8: return h_session_create();
    case 0x3A06E7: return h_session_info();
    case 0x3A06B7: return h_session_update();
    case 0x3A06F2: return h_session_delete();
    case 0x3A072E: return h_search();
    case 0x3A0753: return h_search_len();
    case 0x3A073D: return h_search_results();
    case 0x3A0748: return h_search_parse();
    case 0x3A0710: return h_find_from_id();
    case 0x3CD156: return h_qos_lookup();
    default: return 0;
    }
}

/* ---- the online-peer flag ------------------------------------------------------------
 * The title marks the key of a session it plays on Live as an "online peer"
 * key: after XNetCreateKey it sets the top bit of the id's first byte
 * (0x7A8D7: ab[0] = (ab[0] & 0x0F) | 0x80). XNet then wants a Live
 * security gateway for that peer and refuses the plain address
 * (XNetXnAddrToInAddr: WSAEINVAL). Open Live has no gateway: every peer is
 * on the shared mesh, as in system link. So the title's XNet only ever sees
 * the id without that bit: the calls that take an id get a copy with it
 * cleared, and one that returns an id gets the bit back. The title's own
 * memory keeps the marked ids it compares. */

#define FLAG_ONLINE 0x80u
static uint32_t s_kid_ring;     /* guest: 32 ids of 8 bytes */
static volatile long s_kid_next;
static uint8_t s_marked[16][8]; /* ids seen with the bit, to put it back on output */
static int s_nmarked;

static uint32_t kid_copy_unmarked(uint32_t pkid)
{
    uint8_t kid[8];
    uint32_t slot;
    int i, known = 0;
    if (!pkid || !s_kid_ring) return pkid;
    memcpy(kid, xo_ptr(pkid), 8);
    if (!(kid[0] & FLAG_ONLINE)) return pkid;
    lock();
    for (i = 0; i < s_nmarked; i++) if (!memcmp(s_marked[i], kid + 1, 7)) known = 1;
    if (!known) { memcpy(s_marked[s_nmarked % 16], kid + 1, 7); s_nmarked = s_nmarked < 16 ? s_nmarked + 1 : 16; }
    slot = (uint32_t)(s_kid_next++ & 31);
    unlock();
    kid[0] &= (uint8_t)~FLAG_ONLINE;
    memcpy(xo_ptr(s_kid_ring + 8 * slot), kid, 8);
    return s_kid_ring + 8 * slot;
}

static void kid_mark_again(uint32_t pkid)
{
    uint8_t *k = xo_ptr(pkid);
    int i;
    lock();
    for (i = 0; i < s_nmarked; i++)
        if (!memcmp(s_marked[i], k + 1, 7)) { k[0] |= FLAG_ONLINE; break; }
    unlock();
}

static void stack_arg_set(int i, uint32_t v) { xo_wr32(xo_arg_addr(i), v); }

/* before the title's own XNet runs (the arguments are still on the stack);
 * returns the guest address of an id to mark again afterwards, or 0 */
uint32_t xo_match_before(uint32_t va)
{
    switch (va) {
    case 0x3CD0D6:   /* XNetRegisterKey(&kid, &key) */
    case 0x3CD0E1:   /* XNetUnregisterKey(&kid) */
    case 0x3CD147:   /* XNetQosListen(&kid, ...) */
        stack_arg_set(0, kid_copy_unmarked(xo_arg(0)));
        break;
    case 0x3CD0EC:   /* XNetXnAddrToInAddr(&xnaddr, &kid, &ina) */
        stack_arg_set(1, kid_copy_unmarked(xo_arg(1)));
        break;
    case 0x3CD32D:   /* XNetInAddrToXnAddr(ina, &xnaddr, &kid): an id comes back */
        return xo_arg(2);
    default:
        break;
    }
    return 0;
}

void xo_match_unmark_done(uint32_t pkid)
{
    if (pkid) kid_mark_again(pkid);
}

/* after the title's own XNet ran: what Open Live needs from it */
void xo_match_after(uint32_t va, const uint32_t *a, uint32_t eax)
{
    uint8_t kid[8];
    switch (va) {
    case 0x3CD17D:   /* XNetGetTitleXnAddr(&xnaddr) */
        if (a[0]) {
            lock();
            memcpy(s_xnaddr, xo_ptr(a[0]), 36);
            unlock();
        }
        break;
    case 0x3CD147:   /* XNetQosListen(&kid, data, size, bps, flags): keep the reply data */
        if (a[0] && (a[4] & 4)) {
            listen_t *l;
            memcpy(kid, xo_ptr(a[0]), 8);
            lock();
            l = listen_by_kid(kid, 1);
            l->len = a[2] > QOS_BLOB ? QOS_BLOB : (int)a[2];
            if (a[1] && l->len) memcpy(l->data, xo_ptr(a[1]), (size_t)l->len);
            unlock();
        }
        break;
    case 0x3CD0D6:   /* XNetRegisterKey(&kid, &key): a session we will talk to */
        /* Every session key the title registers is a session it hosts or
         * joins (its network session, a party, a matchmade game): both
         * ends register the same id, so the id names the mesh room. */
        if (a[0] && eax == 0) {
            memcpy(kid, xo_ptr(a[0]), 8);
            mesh_join(kid, "key registered");
            lock();
            mesh_track(kid);
            unlock();
        }
        break;
    case 0x3CD0E1:   /* XNetUnregisterKey(&kid) */
        if (a[0]) {
            hosted_t *h;
            memcpy(kid, xo_ptr(a[0]), 8);
            mesh_leave(kid);
            mesh_untrack(kid);
            h = hosted_by_kid(kid);
            if (h) {
                advert_stop((int)(h - s_hosted));
                lock();
                h->used = 0;
                unlock();
            }
        }
        break;
    default:
        break;
    }
}

/* every hooked call: move full bucket rooms on */
void xo_match_pump(void)
{
#ifdef OPENHALO2_NET
    int i, members, oldest;
    overflow_pump();
    for (i = 0; i < MAX_HOSTED; i++) {
        hosted_t *h = &s_hosted[i];
        if (!h->used || !h->advertising || h->hold <= 0) continue;
        members = olive_hold_members(h->hold, &oldest);
        if (members >= SHARD_HOSTS && h->shard + 1 < SHARDS) {
            advert_stop(i);
            h->shard++;
            advert_start(i);
        }
    }
#endif
}

void xo_match_init(void)
{
    s_scratch = xo_alloc(64);
    s_kid_ring = xo_alloc(8 * 32);
#ifdef OPENHALO2_NET
    s_net = olive_init(getenv("RECOMP_NET_RELAY")) == 0;
#endif
    fprintf(stderr, "  [XO] matchmaking: %s\n", s_net ? "peer to peer over the room relay" : "no transport (offline build)");
}
