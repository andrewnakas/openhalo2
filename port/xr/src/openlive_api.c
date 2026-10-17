/* Open Live: host answers for the XOnline API (docs/OPEN_LIVE_DESIGN.md 3.7).
 * See openlive.c for the hook and the calling convention.
 *
 * Facts used here, each read from the title's own code (addresses are the
 * game functions that show them; tools/xdis.py prints them):
 *
 *   stdcall argument sizes        tools/xo_argcounts.py (each implementation's
 *                                 `ret N`), confirmed by RECOMP_XO_TRACE
 *   task results                  0x6C450: 0 = running, 0x1500F1 = results
 *                                 available, 0x1500F2 = running idle, any
 *                                 other success = done, failure = failed;
 *                                 0x6C4A0: 0x1500F0 = done (success)
 *   logon results                 0x6CD50: 0x1510F0 connected, 0x1512F0 user
 *                                 has a message, 0 still working,
 *                                 0x80151001-6 / 0x80151200 errors
 *   change-users task             0x6C360: 0x1513F0 working, 0x1513F1 done
 *   NAT types                     0x6B550: 1 open, 2 moderate, 3 strict
 *   GetServiceInfo                0x6BFF0: called with each service id and a
 *                                 null info pointer; a failure for a service
 *                                 marked required fails the sign-in
 *   the user record (0x70 bytes)  0x18FA94 (stride 0x70, id at +0, gamertag
 *                                 at +0xC, hr at +0x6C), 0x6C830 (flags at
 *                                 +8, options at +0x1C, bit 0 = passcode);
 *                                 the 16-entry buffer of 0x24AF1A
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#else
#include <unistd.h>
#endif

#include "openlive.h"

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void     xbox_HeapFree(uint32_t xbox_va);

/* ---- results ---------------------------------------------------------------- */

#define HR_OK                 0x00000000u
#define HR_FALSE              0x00000001u
#define HR_FAIL               0x80004005u
#define HR_TASK_RUNNING       0x00000000u
#define HR_TASK_SUCCESS       0x001500F0u
#define HR_TASK_RESULTS       0x001500F1u
#define HR_TASK_IDLE          0x001500F2u
#define HR_LOGON_CONNECTED    0x001510F0u
#define HR_CHANGE_WORKING     0x001513F0u
#define HR_CHANGE_DONE        0x001513F1u
#define HR_SERVICE_UNAVAIL    0x80150005u   /* a 0x8015xxxx the title maps to "unavailable" (0x6C4A0) */

/* ---- the user record (our layout, from the title's use; see above) --------- */

#define USER_SIZE      0x70u
#define USER_XUID      0x00u   /* 64-bit id */
#define USER_FLAGS     0x08u   /* low 2 bits: guest number */
#define USER_GAMERTAG  0x0Cu   /* 16 chars, zero-terminated */
#define USER_OPTIONS   0x1Cu   /* bit 0: passcode required */
#define USER_HR        0x6Cu
#define MAX_LOGON      4

/* ---- local accounts (openlive.json beside the program) --------------------- */

typedef struct {
    char     gamertag[16];
    uint64_t xuid;
    uint8_t  secret[16];
} account_t;

static account_t s_acct[8];
static int s_nacct;

static void hex_out(char *o, const uint8_t *b, int n)
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) { o[2 * i] = h[b[i] >> 4]; o[2 * i + 1] = h[b[i] & 15]; }
    o[2 * n] = 0;
}

static void rand_bytes(uint8_t *b, int n)
{
#if defined(_WIN32)
    HCRYPTPROV p;
    if (CryptAcquireContextA(&p, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(p, (DWORD)n, b);
        CryptReleaseContext(p, 0);
        return;
    }
#else
    if (getentropy(b, (size_t)n) == 0)    /* the browser's crypto random */
        return;
#endif
    {
        static uint64_t x;
        int i;
        if (!x) x = (uint64_t)time(NULL) * 6364136223846793005ull ^ (uint64_t)(uintptr_t)&x;
        for (i = 0; i < n; i++) { x = x * 6364136223846793005ull + 1442695040888963407ull; b[i] = (uint8_t)(x >> 56); }
    }
}

/* Xbox gamertag rules: 1-15 characters, letters, digits and spaces, starting
 * with a letter. Anything else is dropped. */
static void clean_tag(char *out, const char *in)
{
    int n = 0;
    for (; *in && n < 15; in++) {
        char c = *in;
        int letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        int digit = c >= '0' && c <= '9';
        if (!n && !letter) continue;
        if (letter || digit || c == ' ') out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

/* A tiny reader for the file we write ourselves: "key": "value" pairs inside
 * one object per account, in an "accounts" array. */
static const char *json_str(const char *p, const char *key, char *out, size_t n)
{
    char pat[32];
    const char *q, *e;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    q = strstr(p, pat);
    if (!q) return NULL;
    q = strchr(q + strlen(pat), '"');
    if (!q) return NULL;
    e = strchr(q + 1, '"');
    if (!e) return NULL;
    if ((size_t)(e - q - 1) >= n) return NULL;
    memcpy(out, q + 1, (size_t)(e - q - 1));
    out[e - q - 1] = 0;
    return e + 1;
}

static const char *profile_path(void)
{
    const char *p = getenv("RECOMP_OL_PROFILE");
    return p && *p ? p : "openlive.json";
}

static void accounts_save(void)
{
    FILE *f = fopen(profile_path(), "w");
    int i;
    if (!f) return;
    fprintf(f, "{\n  \"version\": 1,\n  \"accounts\": [\n");
    for (i = 0; i < s_nacct; i++) {
        char sec[40];
        hex_out(sec, s_acct[i].secret, 16);
        fprintf(f, "    { \"gamertag\": \"%s\", \"xuid\": \"%016llx\", \"secret\": \"%s\" }%s\n",
                s_acct[i].gamertag, (unsigned long long)s_acct[i].xuid, sec, i + 1 < s_nacct ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void account_new(const char *tag)
{
    account_t *a = &s_acct[s_nacct];
    uint8_t r[6];
    clean_tag(a->gamertag, tag);
    if (!a->gamertag[0]) strcpy(a->gamertag, "Spartan");
    rand_bytes(r, 6);
    /* 0x0009 in the top 16 bits, as Live user ids have; the rest random */
    a->xuid = 0x0009000000000000ull | ((uint64_t)r[0] << 40) | ((uint64_t)r[1] << 32)
            | ((uint64_t)r[2] << 24) | ((uint64_t)r[3] << 16) | ((uint64_t)r[4] << 8) | r[5];
    rand_bytes(a->secret, 16);
    s_nacct++;
}

static void accounts_load(void)
{
    FILE *f = fopen(profile_path(), "rb");
    const char *env_tag = getenv("RECOMP_OL_GAMERTAG");
    char buf[8192];
    size_t n = 0;
    s_nacct = 0;
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    {
        const char *p = buf;
        char tag[64], xu[32], sec[64];
        while (s_nacct < 8 && (p = strstr(p, "\"gamertag\"")) != NULL) {
            const char *q = json_str(p, "gamertag", tag, sizeof tag);
            account_t *a = &s_acct[s_nacct];
            int i;
            if (!q || !json_str(p, "xuid", xu, sizeof xu)) break;
            clean_tag(a->gamertag, tag);
            a->xuid = strtoull(xu, NULL, 16);
            memset(a->secret, 0, 16);
            if (json_str(p, "secret", sec, sizeof sec) && strlen(sec) == 32)
                for (i = 0; i < 16; i++) { unsigned v; sscanf(sec + 2 * i, "%2x", &v); a->secret[i] = (uint8_t)v; }
            if (a->gamertag[0] && a->xuid)
                s_nacct++;
            p = q;
        }
    }
    if (env_tag && *env_tag) {
        /* a test copy or a first run that names its player: that account first */
        char want[16];
        int i, found = -1;
        clean_tag(want, env_tag);
        for (i = 0; i < s_nacct; i++)
            if (_stricmp(s_acct[i].gamertag, want) == 0) found = i;
        if (found < 0) {
            if (s_nacct == 8) s_nacct = 7;
            account_new(want);
            found = s_nacct - 1;
            accounts_save();
        }
        if (found > 0) { account_t t = s_acct[0]; s_acct[0] = s_acct[found]; s_acct[found] = t; }
    }
    if (!s_nacct) {
        char tag[16];
        uint8_t r[2];
        rand_bytes(r, 2);
        snprintf(tag, sizeof tag, "Spartan %u", (unsigned)(((r[0] << 8) | r[1]) % 10000));
        account_new(tag);
        accounts_save();
    }
}

const char *xo_self_gamertag(void) { return s_nacct ? s_acct[0].gamertag : ""; }
uint64_t    xo_self_xuid(void) { return s_nacct ? s_acct[0].xuid : 0; }

/* ---- guest scratch memory ---------------------------------------------------- */

static uint32_t s_logon_users;      /* MAX_LOGON user records, returned by GetLogonUsers */

/* ---- tasks ------------------------------------------------------------------ */

/* A task handle is TASK_TAG | slot (the title only compares handles with 0
 * and -1). A task either finishes after `need` Continue calls with `done`,
 * or asks its poll function, which returns 0 while working and the result
 * once finished (openlive_match.c). */
#define TASK_TAG   0x0E5A0000u

enum { T_FREE, T_LOGON, T_CHANGE, T_GENERIC, T_POLLED };

typedef struct {
    int type;
    int polls;          /* Continue calls so far */
    int need;           /* Continue calls before it is done */
    uint32_t running;   /* result while working */
    uint32_t done;      /* result once done */
    uint32_t va;        /* the API that made it, for the trace */
    xo_task_poll_fn poll;
    int user;
    void (*on_close)(int user);
} task_t;

static task_t s_task[XO_MAX_TASKS];

static uint32_t task_new(int type, uint32_t va, int need, uint32_t running, uint32_t done)
{
    int i;
    for (i = 1; i < XO_MAX_TASKS; i++) {
        if (s_task[i].type == T_FREE) {
            task_t *t = &s_task[i];
            memset(t, 0, sizeof *t);
            t->type = type; t->polls = 0; t->need = need;
            t->running = running; t->done = done; t->va = va;
            return TASK_TAG | (uint32_t)i;
        }
    }
    fprintf(stderr, "  [XO] out of task slots\n");
    return 0;
}

uint32_t xo_task_new(uint32_t va, int need, uint32_t done)
{
    return task_new(T_GENERIC, va, need, HR_TASK_RUNNING, done);
}

/* a task that keeps running with results available (friends, presence:
 * the title reads them while the task is "running", 0x6C450 / 0x6B5D0) */
uint32_t xo_task_new_results(uint32_t va)
{
    return task_new(T_GENERIC, va, 0x7FFFFFFF, HR_TASK_RESULTS, HR_TASK_SUCCESS);
}

uint32_t xo_task_new_polled(uint32_t va, xo_task_poll_fn poll, int user, void (*on_close)(int user))
{
    uint32_t h = task_new(T_POLLED, va, 0, HR_TASK_RUNNING, 0);
    if (h) {
        s_task[h & 0xFFFF].poll = poll;
        s_task[h & 0xFFFF].user = user;
        s_task[h & 0xFFFF].on_close = on_close;
    }
    return h;
}

static task_t *task_get(uint32_t h)
{
    uint32_t i = h & 0xFFFFu;
    if ((h & 0xFFFF0000u) != TASK_TAG || i == 0 || i >= XO_MAX_TASKS || s_task[i].type == T_FREE)
        return NULL;
    return &s_task[i];
}

int xo_task_user(uint32_t h)
{
    task_t *t = task_get(h);
    return t ? t->user : -1;
}

static uint32_t task_continue(task_t *t)
{
    if (t->type == T_POLLED) {
        uint32_t r = t->poll(t->user);
        if (r) t->polls = 1;
        return r ? r : HR_TASK_RUNNING;
    }
    if (t->polls < t->need) {
        t->polls++;
        return t->running;
    }
    t->polls = t->need + 1;
    return t->done;
}

static int task_finished(const task_t *t) { return t->polls > t->need; }

/* ---- handlers ----------------------------------------------------------------- */

static void put_handle(uint32_t pp, uint32_t h) { if (pp) xo_wr32(pp, h); }

static void user_write(uint32_t at, const account_t *a)
{
    uint8_t *u = xo_ptr(at);
    memset(u, 0, USER_SIZE);
    memcpy(u + USER_XUID, &a->xuid, 8);
    strncpy((char *)u + USER_GAMERTAG, a->gamertag, 15);
}

static int h_get_users(void)              /* (users, &count) */
{
    uint32_t users = xo_arg(0), pcount = xo_arg(1);
    int i;
    for (i = 0; i < s_nacct; i++)
        user_write(users + USER_SIZE * (uint32_t)i, &s_acct[i]);
    if (pcount) xo_wr32(pcount, (uint32_t)s_nacct);
    xo_return(HR_OK, 2);
    return 1;
}

static void logon_users_set(uint32_t users)
{
    int i;
    for (i = 0; i < MAX_LOGON; i++) {
        uint8_t *dst = xo_ptr(s_logon_users + USER_SIZE * (uint32_t)i);
        memcpy(dst, xo_ptr(users + USER_SIZE * (uint32_t)i), USER_SIZE);
        if (memcmp(dst, "\0\0\0\0\0\0\0\0", 8))
            memset(dst + USER_HR, 0, 4);     /* signed in */
        else
            memset(dst, 0, USER_SIZE);
        if (memcmp(dst, "\0\0\0\0\0\0\0\0", 8)) {
            uint64_t x;
            char tag[16];
            memcpy(&x, dst + USER_XUID, 8);
            memcpy(tag, dst + USER_GAMERTAG, 15);
            tag[15] = 0;
            fprintf(stderr, "  [XO] pad %d signed in as \"%s\"\n", i, tag);
            xo_friends_signin(x, tag);   /* the first signed-in player is the one friends see */
        }
    }
}

static int h_set_state(void)              /* (pad, flags, kid lo, kid hi, size, data) */
{
    uint8_t kid[8], data[32];
    uint32_t lo = xo_arg(2), hi = xo_arg(3), n = xo_arg(4);
    memcpy(kid, &lo, 4); memcpy(kid + 4, &hi, 4);
    if (n > 32) n = 32;
    if (n && xo_arg(5)) memcpy(data, xo_ptr(xo_arg(5)), n);
    xo_friends_set_state(xo_arg(1), kid, data, (int)n);
    xo_return(HR_OK, 6);
    return 1;
}

static int h_logon(void)                  /* (users[4], services, count, event, &task) */
{
    logon_users_set(xo_arg(0));
    put_handle(xo_arg(4), task_new(T_LOGON, 0x3A0641, 3, HR_TASK_RUNNING, HR_LOGON_CONNECTED));
    xo_return(HR_OK, 5);
    return 1;
}

static int h_logon_results(void)          /* (task) */
{
    task_t *t = task_get(xo_arg(0));
    if (!t) return 0;
    xo_return(task_finished(t) ? HR_LOGON_CONNECTED : HR_OK, 1);
    return 1;
}

static int h_change_users(void)           /* (users[4], event, &task) */
{
    logon_users_set(xo_arg(0));
    put_handle(xo_arg(2), task_new(T_CHANGE, 0x3A065B, 2, HR_CHANGE_WORKING, HR_CHANGE_DONE));
    xo_return(HR_OK, 3);
    return 1;
}

static int h_change_results(void)         /* (task, hr[4]) */
{
    uint32_t r = xo_arg(1);
    int i;
    if (!task_get(xo_arg(0))) return 0;
    for (i = 0; r && i < MAX_LOGON; i++) xo_wr32(r + 4u * (uint32_t)i, HR_OK);
    xo_return(HR_OK, 2);
    return 1;
}

static int h_task_continue(void)          /* (task) */
{
    task_t *t = task_get(xo_arg(0));
    if (!t) return 0;
    xo_return(task_continue(t), 1);
    return 1;
}

static int h_task_close(void)             /* (task) */
{
    task_t *t = task_get(xo_arg(0));
    if (!t) return 0;
    if (t->on_close) t->on_close(t->user);
    t->type = T_FREE;
    xo_return(HR_OK, 1);
    return 1;
}

static int h_task_progress(void)          /* (task, &percent, a, b) */
{
    task_t *t = task_get(xo_arg(0));
    if (!t) return 0;
    if (xo_arg(1)) xo_wr32(xo_arg(1), task_finished(t) ? 100u : (uint32_t)(t->polls * 100 / (t->need + 1)));
    xo_return(HR_OK, 4);
    return 1;
}

static int h_service_info(void)           /* (service id, &info or NULL) */
{
    /* every service "available"; no server addresses (the title connects to
     * one only for service 0x14 with its connect flag set, which it is not) */
    if (xo_arg(1)) memset(xo_ptr(xo_arg(1)), 0, 16);
    xo_return(HR_OK, 2);
    return 1;
}

static int h_nat_type(void)               /* () */
{
    xo_return(1, 0);   /* open: the mesh connects peer to peer */
    return 1;
}

static int h_offering_new(void)           /* (title?, 0, &task) */
{
    put_handle(xo_arg(2), task_new(T_GENERIC, 0x3A069D, 1, HR_TASK_RUNNING, HR_TASK_SUCCESS));
    xo_return(HR_OK, 3);
    return 1;
}

/* ---- XNet QoS ------------------------------------------------------------------
 * The result block, as the title reads it (0x7B6C0-0x7B870): a count at +0,
 * the number still pending at +4, then one 24-byte entry per target: flags
 * byte (1 complete, 2 target contacted, 4 target disabled, 8 data received,
 * 0x10 partially complete), probes sent (u16 +2) and received (u16 +4), data
 * size (u16 +6), data pointer (+8), minimum and median round trip in ms (u16
 * +0xC, +0xE), upstream and downstream bits per second (+0x10, +0x14). */
#define QOS_ENTRY 24u
#define QOS_MAX   64
static uint32_t s_qos[QOS_MAX];

/* n entries, then `extra` bytes for the entries' data (released together) */
uint32_t xo_qos_alloc(uint32_t n, uint32_t extra)
{
    uint32_t sz = 8 + QOS_ENTRY * (n ? n : 1) + extra;
    uint32_t q = xbox_HeapAlloc(sz, 16);
    int i;
    memset(xo_ptr(q), 0, sz);
    xo_wr32(q, n);
    for (i = 0; i < QOS_MAX; i++)
        if (!s_qos[i]) { s_qos[i] = q; break; }
    return q;
}

void xo_qos_entry(uint32_t q, uint32_t i, uint8_t flags, uint16_t rtt, uint32_t up, uint32_t down,
                      uint32_t data, uint16_t size)
{
    uint8_t *e = xo_ptr(q + 8 + QOS_ENTRY * i);
    uint16_t probes = 4;
    e[0] = flags;
    memcpy(e + 2, &probes, 2);
    memcpy(e + 4, &probes, 2);
    memcpy(e + 6, &size, 2);
    memcpy(e + 8, &data, 4);
    memcpy(e + 0xC, &rtt, 2);
    memcpy(e + 0xE, &rtt, 2);
    memcpy(e + 0x10, &up, 4);
    memcpy(e + 0x14, &down, 4);
}

static int h_qos_service_lookup(void)     /* (flags, event, &xnqos) */
{
    /* the bandwidth probe the title runs once signed in (0x2BB04E): it waits
     * for this to complete. One entry: contacted, a typical home line. */
    uint32_t q = xo_qos_alloc(1, 0);
    xo_qos_entry(q, 0, 0x01 | 0x02, 20, 1000000u, 8000000u, 0, 0);
    xo_wr32(xo_arg(2), q);
    xo_return(0, 3);
    return 1;
}

static int h_qos_release(void)            /* (xnqos) */
{
    uint32_t q = xo_arg(0);
    int i;
    for (i = 0; i < QOS_MAX; i++) {
        if (s_qos[i] == q && q) {
            s_qos[i] = 0;
            xbox_HeapFree(q);
            xo_return(0, 1);
            return 1;
        }
    }
    return 0;   /* the title's own XNet made it (system link) */
}

/* ---- title storage ---------------------------------------------------------------
 * The title names a file by facility (4 = title files, folder "9809/";
 * 5 = the user's own; 3 = a team's: 0xB4120), asks CreateServerPath for a
 * server path, then downloads it into its own buffer. We serve files from
 * openlive/storage/ beside the program:
 *     facility 4 -> title/<path>          facility 5 -> user/<xuid>/<path>
 *     facility 3 -> team/<team id>/<path>
 * The server path we hand out is "<facility>|<owner id hex>|<path>". The
 * files are ours (tools/ol_storage.py); a missing file fails the download
 * the way the title's library does offline (0x80150002), and the title
 * falls back to its defaults (the request's retry callback). */

#define HR_STORAGE_FAIL 0x80150002u

static void guest_wstr(uint32_t va, char *out, size_t n)
{
    size_t i = 0;
    if (va) {
        for (; i + 1 < n; i++) {
            uint16_t c;
            memcpy(&c, xo_ptr(va + 2u * (uint32_t)i), 2);
            if (!c) break;
            out[i] = (c < 0x80 && c >= 0x20) ? (char)c : '_';
        }
    }
    out[i] = 0;
}

static void guest_put_wstr(uint32_t va, const char *s, uint32_t cap)
{
    uint32_t i;
    for (i = 0; i + 1 < cap && s[i]; i++) {
        uint16_t c = (uint8_t)s[i];
        memcpy(xo_ptr(va + 2 * i), &c, 2);
    }
    memset(xo_ptr(va + 2 * i), 0, 2);
}

static int storage_local(const char *server, char *out, size_t n)
{
    unsigned fac = 0;
    char owner[32] = "", path[512] = "", *p;
    if (sscanf(server, "%u|%31[^|]|%511[^\n]", &fac, owner, path) != 3)
        return 0;
    for (p = path; *p; p++) {
        if (*p == '\\') *p = '/';
        if (p[0] == '.' && p[1] == '.') return 0;     /* stay inside storage/ */
    }
    if (fac == 4) snprintf(out, n, "openlive/storage/title/%s", path);
    else if (fac == 5) snprintf(out, n, "openlive/storage/user/%s/%s", owner, path);
    else snprintf(out, n, "openlive/storage/team/%s/%s", owner, path);
    return 1;
}

static int h_storage_path(void)   /* (facility, user lo, user hi, team lo, team hi, path, out, &chars) */
{
    uint32_t fac = xo_arg(0), out = xo_arg(6), pn = xo_arg(7), cap = pn ? xo_rd32(pn) : 0x100;
    uint64_t owner = fac == 3 ? ((uint64_t)xo_arg(4) << 32 | xo_arg(3)) : ((uint64_t)xo_arg(2) << 32 | xo_arg(1));
    char path[512], server[600];
    guest_wstr(xo_arg(5), path, sizeof path);
    snprintf(server, sizeof server, "%u|%016llx|%s", fac, (unsigned long long)owner, path);
    if (strlen(server) + 1 > cap) { xo_return(0x8007007Au, 8); return 1; }   /* buffer too small */
    guest_put_wstr(out, server, cap);
    if (pn) xo_wr32(pn, (uint32_t)strlen(server) + 1);
    xo_return(HR_OK, 8);
    return 1;
}

typedef struct { uint32_t buf, len; } dl_t;
static dl_t s_dl[XO_MAX_TASKS];

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* The web build fetches the title-storage files from the site, beside the
 * page (openlive/storage/...), with a synchronous request: this runs on a
 * worker thread, where those are allowed. Returns the bytes copied, -1 if
 * the file is not there. */
EM_JS(int, ol_fetch_sync, (const char *url, uint8_t *out, int cap), {
    try {
        const x = new XMLHttpRequest();
        x.open('GET', UTF8ToString(url), false);
        x.responseType = 'arraybuffer';
        x.send();
        if (x.status !== 200 || !x.response) return -1;
        const b = new Uint8Array(x.response);
        const n = Math.min(b.length, cap);
        HEAPU8.set(b.subarray(0, n), out);
        return n;
    } catch (e) { return -1; }
});
#endif

/* the file behind a server path: from openlive/storage/ (the web build: the site) */
static long storage_read(const char *local, uint8_t *out, size_t cap)
{
    FILE *f = fopen(local, "rb");
    size_t n;
    if (f) {
        n = fread(out, 1, cap, f);
        fclose(f);
        return (long)n;
    }
#ifdef __EMSCRIPTEN__
    return ol_fetch_sync(local, out, (int)cap);
#else
    return -1;
#endif
}

static int h_storage_download_mem(void)   /* (facility, pad, server path, buf, size, a, b, &task) */
{
    static uint8_t tmp[1 << 20];
    char server[600], local[700];
    uint32_t buf = xo_arg(3), cap = xo_arg(4), h;
    long got;
    size_t n;
    guest_wstr(xo_arg(2), server, sizeof server);
    if (!storage_local(server, local, sizeof local) || (got = storage_read(local, tmp, sizeof tmp)) < 0) {
        fprintf(stderr, "  [XO] storage: %s not served\n", server);
        xo_return(HR_STORAGE_FAIL, 8);
        return 1;
    }
    n = (size_t)got;
    if (n > cap) n = cap;
    memcpy(xo_ptr(buf), tmp, n);
    h = task_new(T_GENERIC, 0x3AB6EA, 2, HR_TASK_RUNNING, HR_TASK_SUCCESS);
    s_dl[h & 0xFFFF].buf = buf;
    s_dl[h & 0xFFFF].len = (uint32_t)n;
    put_handle(xo_arg(7), h);
    fprintf(stderr, "  [XO] storage: %s -> %u bytes from %s\n", server, (unsigned)n, local);
    xo_return(HR_OK, 8);
    return 1;
}

static int h_storage_download_results(void)   /* (task, &buf, &received, &total, &owner, &created) */
{
    uint32_t h = xo_arg(0);
    dl_t *d;
    if (!task_get(h)) return 0;
    d = &s_dl[h & 0xFFFF];
    if (xo_arg(1)) xo_wr32(xo_arg(1), d->buf);
    if (xo_arg(2)) xo_wr32(xo_arg(2), d->len);
    if (xo_arg(3)) xo_wr32(xo_arg(3), d->len);
    if (xo_arg(4)) memset(xo_ptr(xo_arg(4)), 0, 8);
    if (xo_arg(5)) memset(xo_ptr(xo_arg(5)), 0, 8);
    xo_return(HR_OK, 6);
    return 1;
}

static int h_storage_upload_mem(void)   /* (facility, pad, server path, expires lo, hi, buf, size, a, b, &task) */
{
    char server[600], local[700];
    FILE *f;
    guest_wstr(xo_arg(2), server, sizeof server);
    if (storage_local(server, local, sizeof local)) {
        /* a player's own files are kept locally; folders made on demand */
        char dir[700], *s;
        strcpy(dir, local);
        for (s = dir; *s; s++) {
            if (*s != '/') continue;
            *s = 0;
#if defined(_WIN32)
            CreateDirectoryA(dir, NULL);
#endif
            *s = '/';
        }
        if ((f = fopen(local, "wb")) != NULL) {
            fwrite(xo_ptr(xo_arg(5)), 1, xo_arg(6), f);
            fclose(f);
        }
    }
    put_handle(xo_arg(9), task_new(T_GENERIC, 0x3AD176, 1, HR_TASK_RUNNING, HR_TASK_SUCCESS));
    xo_return(HR_OK, 10);
    return 1;
}

/* to and from a directory on the cache drive: not used by the files above */
static int h_storage_download_dir(void) { xo_return(HR_STORAGE_FAIL, 7); return 1; }
static int h_storage_upload_dir(void)   { xo_return(HR_STORAGE_FAIL, 9); return 1; }

/* ---- statistics ------------------------------------------------------------------
 * The title reads six values per player and playlist before it matchmakes
 * (0x74AA0: skill and experience, its "stat specs" are 24-byte records:
 * player id +0 (12 bytes), leaderboard +0xC, count +0x10, values +0x14; a
 * value is 16 bytes: id (u16) +0, type +4, value +8). There is no
 * leaderboard server: every value reads back empty (type 0), which the
 * title takes as 0 and clamps to its lowest rank. Writes are accepted and
 * dropped. */

static int h_stat_read(void)          /* (count, specs, event, &task) */
{
    put_handle(xo_arg(3), task_new(T_GENERIC, 0x3A08CB, 1, HR_TASK_RUNNING, HR_TASK_SUCCESS));
    xo_return(HR_OK, 4);
    return 1;
}

static int h_stat_read_result(void)   /* (task, count, specs, extra size, extra) */
{
    uint32_t n = xo_arg(1), specs = xo_arg(2), i, j;
    if (!task_get(xo_arg(0))) return 0;
    for (i = 0; specs && i < n; i++) {
        uint32_t rec = specs + 24 * i, cnt = xo_rd32(rec + 0x10), vals = xo_rd32(rec + 0x14);
        for (j = 0; vals && j < cnt && j < 64; j++) {
            xo_wr32(vals + 16 * j + 4, 0);
            xo_wr32(vals + 16 * j + 8, 0);
        }
    }
    xo_return(HR_OK, 5);
    return 1;
}

static int h_stat_write(void)         /* (count, specs, event, &task) */
{
    put_handle(xo_arg(3), task_new(T_GENERIC, 0x3A08C0, 1, HR_TASK_RUNNING, HR_TASK_SUCCESS));
    xo_return(HR_OK, 4);
    return 1;
}

static int h_stat_write_result(void)  /* (task, &handle, &references, &count) */
{
    if (!task_get(xo_arg(0))) return 0;
    if (xo_arg(1)) xo_wr32(xo_arg(1), 0);
    if (xo_arg(2)) xo_wr32(xo_arg(2), 0);
    if (xo_arg(3)) xo_wr32(xo_arg(3), 0);
    xo_return(HR_OK, 4);
    return 1;
}

/* ---- arbitration -------------------------------------------------------------------
 * Ranked games register their round with Live's arbitration service and
 * report results to it (0x924E0-0x927B0); a failed registration ends the
 * match. There is no arbitrator: rounds are accepted and reports dropped
 * (no ranked integrity without a server; docs/OPEN_LIVE_DESIGN.md 3.7). */

static int h_arb_round_id(void)       /* (&round id) */
{
    uint8_t r[8];
    rand_bytes(r, 8);
    if (xo_arg(0)) memcpy(xo_ptr(xo_arg(0)), r, 8);
    xo_return(HR_OK, 1);
    return 1;
}

static int h_arb_register(void)       /* (&arbitration id, seconds, flags, event, &task) */
{
    put_handle(xo_arg(4), task_new(T_GENERIC, 0x3AE32B, 1, HR_TASK_RUNNING, HR_TASK_SUCCESS));
    xo_return(HR_OK, 5);
    return 1;
}

static int h_arb_extend(void)         /* (&arbitration id, seconds, event, &task) */
{
    put_handle(xo_arg(3), task_new(T_GENERIC, 0x3AE33A, 1, HR_TASK_RUNNING, HR_TASK_SUCCESS));
    xo_return(HR_OK, 4);
    return 1;
}

static int h_arb_report(void)         /* (&arbitration id, count, procedures, ?, flags, event, &task) */
{
    put_handle(xo_arg(6), task_new(T_GENERIC, 0x3AE36A, 1, HR_TASK_RUNNING, HR_TASK_SUCCESS));
    xo_return(HR_OK, 7);
    return 1;
}

static int ret_value(uint32_t v, int nargs) { xo_return(v, nargs); return 1; }

/* ---- the table ---------------------------------------------------------------- */

typedef struct { uint32_t va; int (*fn)(void); } handler_t;

static const handler_t k_handlers[] = {
    {0x3A0620, h_task_continue},
    {0x3A062B, h_task_close},
    {0x3AD1A6, h_task_progress},
    {0x3A0636, h_get_users},
    {0x3A0641, h_logon},
    {0x3A0650, h_logon_results},
    {0x3A065B, h_change_users},
    {0x3A0666, h_change_results},
    {0x3A067C, h_service_info},
    {0x3A11AE, h_nat_type},
    {0x3A069D, h_offering_new},
    {0x3CD365, h_qos_service_lookup},
    {0x3CD172, h_qos_release},
    {0x3AB6B1, h_storage_path},
    {0x3AB6EA, h_storage_download_mem},
    {0x3AB6F9, h_storage_download_results},
    {0x3AD176, h_storage_upload_mem},
    {0x3AB6DB, h_storage_download_dir},
    {0x3AD149, h_storage_upload_dir},
    {0x3A08CB, h_stat_read},
    {0x3A08D6, h_stat_read_result},
    {0x3A08C0, h_stat_write},
    {0x3AB6A6, h_stat_write_result},
    {0x3ADC4C, h_arb_round_id},
    {0x3AE32B, h_arb_register},
    {0x3AE33A, h_arb_extend},
    {0x3AE36A, h_arb_report},
};

int xo_live_call(uint32_t va)
{
    size_t i;
    xo_match_pump();
    xo_friends_pump();
    xo_friends_reap_pump();
    if (xo_match_call(va) || xo_clans_call(va) || xo_friends_call(va))
        return 1;
    switch (va) {
    case 0x3A0671: return ret_value(s_logon_users, 0);          /* GetLogonUsers() */
    case 0x3A0687: return ret_value(HR_OK, 1);                  /* SaveLogonState(p) */
    case 0x3A0774: return h_set_state();                        /* NotificationSetState */
    default: break;
    }
    for (i = 0; i < sizeof k_handlers / sizeof k_handlers[0]; i++)
        if (k_handlers[i].va == va)
            return k_handlers[i].fn();
    return 0;
}

void xo_live_init(void)
{
    accounts_load();
    xo_match_init();
    s_logon_users = xbox_HeapAlloc(USER_SIZE * MAX_LOGON, 16);
    memset(xo_ptr(s_logon_users), 0, USER_SIZE * MAX_LOGON);
    fprintf(stderr, "  [XO] Open Live: %d local account(s), first \"%s\" (%016llx), profile %s\n",
            s_nacct, xo_self_gamertag(), (unsigned long long)xo_self_xuid(), profile_path());
}

/* ---- trace detail ------------------------------------------------------------ */

static void dump(const char *what, uint32_t va, uint32_t n)
{
    char line[4096];
    size_t o = 0;
    uint32_t i;
    if (va < 0x10000 || va >= 0x80000000u) return;
    o += (size_t)snprintf(line + o, sizeof line - o, "  [XO]   %s @%08X:", what, va);
    for (i = 0; i < n && o + 8 < sizeof line; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, "%s%02X", (i % 16) ? " " : "\n  [XO]     ", xo_ptr(va)[i]);
    fprintf(stderr, "%s\n", line);
}

/* Trace: what the real library returned through pointer arguments. */
void xo_trace_detail(uint32_t va, const uint32_t *a, uint32_t eax)
{
    switch (va) {
    case 0x3A0636:  /* GetUsers(users, &count) */
        dump("count", a[1], 4);
        if (xo_rd32(a[1])) dump("users", a[0], 0xE0);
        break;
    case 0x3A0671:  /* GetLogonUsers() -> users[4] */
        dump("logon users", eax, 0x70);
        break;
    case 0x3CD0D6:  /* XNetRegisterKey(&kid, &key) */
        dump("kid", a[0], 8);
        break;
    case 0x3CD0EC:  /* XNetXnAddrToInAddr(&xnaddr, &kid, &ina) */
        dump("xnaddr", a[0], 36);
        dump("kid", a[1], 8);
        break;
    default:
        break;
    }
}
