/* Open Live: clans (the title's "teams") with no server (docs/OPEN_LIVE_DESIGN.md).
 *
 * A clan's record lives with its founder: id, version, properties (name,
 * description, motto, URL) and members. Every member keeps a copy in
 * openlive_clans.txt beside the program (per local account: a "C" line with
 * the properties in hex, then "M" lines) and refreshes it from the founder
 * when the title lists the members. Inbox messages (net/olive.c):
 *   'C'            anyone: your clan -> 'c' id, name (32 bytes), member count
 *   'T' id name    a recruit offer: a message of type 4 from the recruiter
 *                  with the clan as its context (summary +0x10); the
 *                  title answers with the id it read there (0x2C705C ->
 *                  0xABFA0); an answer naming the recruiter is taken too
 *   'U' id         to the founder: "I accept" -> 'j' + the record
 *   'V' id         to the founder: the record -> 'j' + the record
 * Record: 'j' id u64, version u32, founder u64, properties (0x688), n u8,
 * n x (id u64, gamertag 16, privileges u32).
 *
 * The title's calls (VAs mapped from their callers, decomp names):
 *   0x3AFB80 TeamCreate(pad, &properties, &member properties, 100, event, &task)      (0xABE60)
 *   0x3AFB8F TeamDelete(pad, team id (12), event, &task)                                (0xABF10)
 *   0x3AFC26 TeamEnumerateByUserXUID(pad, user id (12), event, &task)                  (0xABC70)
 *   0x3AEBA5 TeamEnumerateGetResults(task, &count, ids (12 each))                       (0xABD00)
 *   0x3AFC50 TeamGetDetails(task, team id (12), &team)                                  (0xABDB0)
 *   0x3AFC76 TeamMembersEnumerate(pad, team id (12), 1, event, &task)                   (0xAC110)
 *   0x3AEBB0 TeamMembersEnumerateGetResults(task, &count, ids (12 each))                (0xAC1B0)
 *   0x3AFCA3 TeamMemberGetDetails(task, member id (12), &member)                        (0xAC250)
 *   0x3AFBF1 TeamMemberRemove(pad, team (12), member (12), event, &task)                (0xAC050)
 *   0x3AFBB9 TeamMemberSetProperties(pad, team (12), member (12), &props, event, &task) (0xAC360)
 *   0x3A8399 TeamMemberRecruit(pad, team (12), member (12), &props, message, event, &task) (0x8F5BC)
 *   0x3A83D4 TeamMemberRecruitByName(pad, team (12), gamertag, &props, message, event, &task)
 *   0x3A8407 TeamMemberAnswerRecruit(pad, team (12), answer (0 no, 1 yes, 2 never), event, &task) (0xABFA0)
 * Layouts (measured): team 0x6A0 bytes (memset in 0xABDB0): id +0 (12),
 * properties +0xC (name 16 wide chars, description, motto and URL 256
 * each: the clan keyboard 0x237A74 edits them at +0, +0x20, +0x220,
 * +0x420), member count +0x694; member 0x90 bytes (0xAC250): id +0 (12),
 * gamertag +0xC, privileges +0x1C (all bits = leader, 0x1C = officer, 0x10
 * = member: 0xAC360 / 0xAC250).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
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

#include "openlive.h"

#ifdef OPENHALO2_NET
#include "olive.h"
#endif

#define PROPS_SIZE   0x688u
#define TEAM_SIZE    0x6A0u
#define MEMBER_SIZE  0x90u
#define MAX_MEMBERS  100
#define WIRE_MEMBERS 80
#define MAX_CT       16
#define MAX_OFFERS   8
#define TEAM_FLAG    0x000E000000000000ull
#define PRIV_MEMBER  0x10u

typedef struct { uint64_t xuid; char tag[16]; uint32_t priv; } member_t;

typedef struct {
    int used;
    uint64_t id, founder;
    uint32_t version;
    uint8_t props[PROPS_SIZE];
    int nmem;
    member_t mem[MAX_MEMBERS];
} clan_t;

static clan_t s_clan;              /* the signed-in player's clan (one) */
static uint64_t s_owner;
static int s_loaded;

typedef struct { int used; uint64_t id, from; uint8_t name[32]; } offer_t;
static offer_t s_offer[MAX_OFFERS];

/* a task with results or a conversation behind it */
enum { CT_USER = 1, CT_MEMBERS, CT_JOIN, CT_RECRUIT_NAME };
typedef struct {
    uint32_t task;
    int kind;
    uint64_t who, id;
    int visit, step;
    int has;
    uint8_t name[32];
    uint32_t count;
    uint32_t result;
} ct_t;
static ct_t s_ct[MAX_CT];

static int ret(uint32_t v, int nargs) { xo_return(v, nargs); return 1; }
static uint64_t arg64(int i) { return (uint64_t)xo_arg(i + 1) << 32 | xo_arg(i); }

static void inbox_room(char *out, size_t n, uint64_t xuid)
{
    static const char a[] = "0123456789abcdefghjkmnpqrstvwxyz";
    char k[16];
    int j, o = 0;
    for (j = 60; j > -5; j -= 5) k[o++] = a[(j >= 0 ? xuid >> j : xuid << -j) & 31];
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

static void ascii_name(char *out, const uint8_t *wide)
{
    int i;
    for (i = 0; i < 15 && wide[2 * i]; i++) out[i] = (char)wide[2 * i];
    out[i] = 0;
}

/* ---- the file (call with the lock held) ---------------------------------------------- */

static const char *clans_path(void)
{
    const char *p = getenv("RECOMP_OL_CLANS");
    return p && *p ? p : "openlive_clans.txt";
}

static void clans_load(void)
{
    FILE *f;
    char *l;
    uint64_t self = xo_self_xuid();
    if (s_loaded && s_owner == self) return;
    memset(&s_clan, 0, sizeof s_clan);
    s_owner = self;
    s_loaded = 1;
    f = fopen(clans_path(), "r");
    if (!f) return;
    l = (char *)malloc(8192);
    while (l && fgets(l, 8192, f)) {
        unsigned long long o, id, x, fo;
        unsigned v, priv;
        char hex[2 * PROPS_SIZE + 8], tag[32] = "";
        if (l[0] == 'C' && sscanf(l, "C %llx %llx %u %llx %3400s", &o, &id, &v, &fo, hex) == 5 && o == self) {
            size_t i;
            s_clan.used = 1;
            s_clan.id = id;
            s_clan.version = v;
            s_clan.founder = fo;
            for (i = 0; i < PROPS_SIZE && hex[2 * i] && hex[2 * i + 1]; i++) {
                unsigned b;
                sscanf(hex + 2 * i, "%2x", &b);
                s_clan.props[i] = (uint8_t)b;
            }
        } else if (l[0] == 'M' && sscanf(l, "M %llx %llx %llx %x %31[^\r\n]", &o, &id, &x, &priv, tag) >= 4
                   && o == self && s_clan.used && id == s_clan.id && s_clan.nmem < MAX_MEMBERS) {
            member_t *m = &s_clan.mem[s_clan.nmem++];
            m->xuid = x;
            m->priv = priv;
            memset(m->tag, 0, sizeof m->tag);
            strncpy(m->tag, tag, 15);
        }
    }
    free(l);
    fclose(f);
}

static void clans_save(void)
{
    char *keep = NULL, *l;
    size_t kn = 0;
    uint64_t self = xo_self_xuid();
    FILE *f = fopen(clans_path(), "r");
    int i;
    l = (char *)malloc(8192);
    if (f && l) {
        while (fgets(l, 8192, f)) {        /* the other accounts' lines stay */
            unsigned long long o = 0;
            size_t n = strlen(l);
            char *k2;
            if (sscanf(l + 1, " %llx", &o) == 1 && o == self) continue;
            k2 = (char *)realloc(keep, kn + n + 1);
            if (!k2) break;
            keep = k2;
            memcpy(keep + kn, l, n + 1);
            kn += n;
        }
    }
    if (f) fclose(f);
    free(l);
    f = fopen(clans_path(), "w");
    if (!f) { free(keep); return; }
    if (keep) fputs(keep, f);
    free(keep);
    if (s_clan.used) {
        size_t k;
        fprintf(f, "C %016llx %016llx %u %016llx ", (unsigned long long)self, (unsigned long long)s_clan.id,
                s_clan.version, (unsigned long long)s_clan.founder);
        for (k = 0; k < PROPS_SIZE; k++) fprintf(f, "%02x", s_clan.props[k]);
        fputc('\n', f);
        for (i = 0; i < s_clan.nmem; i++)
            fprintf(f, "M %016llx %016llx %016llx %x %s\n", (unsigned long long)self, (unsigned long long)s_clan.id,
                    (unsigned long long)s_clan.mem[i].xuid, s_clan.mem[i].priv, s_clan.mem[i].tag);
    }
    fclose(f);
}

/* ---- the record on the wire (lock held) ---------------------------------------------- */

static int record_put(uint8_t *out, int cap)
{
    uint8_t *p = out;
    int i, n = s_clan.nmem < WIRE_MEMBERS ? s_clan.nmem : WIRE_MEMBERS;
    if (cap < 1 + 8 + 4 + 8 + (int)PROPS_SIZE + 1 + n * 28) return 0;
    *p++ = 'j';
    memcpy(p, &s_clan.id, 8); p += 8;
    memcpy(p, &s_clan.version, 4); p += 4;
    memcpy(p, &s_clan.founder, 8); p += 8;
    memcpy(p, s_clan.props, PROPS_SIZE); p += PROPS_SIZE;
    *p++ = (uint8_t)n;
    for (i = 0; i < n; i++) {
        memcpy(p, &s_clan.mem[i].xuid, 8); p += 8;
        memcpy(p, s_clan.mem[i].tag, 16); p += 16;
        memcpy(p, &s_clan.mem[i].priv, 4); p += 4;
    }
    return (int)(p - out);
}

static int record_take(const uint8_t *in, int len)
{
    const uint8_t *p = in;
    int i, n;
    if (len < 1 + 8 + 4 + 8 + (int)PROPS_SIZE + 1 || in[0] != 'j') return 0;
    p++;
    memset(&s_clan, 0, sizeof s_clan);
    s_clan.used = 1;
    memcpy(&s_clan.id, p, 8); p += 8;
    memcpy(&s_clan.version, p, 4); p += 4;
    memcpy(&s_clan.founder, p, 8); p += 8;
    memcpy(s_clan.props, p, PROPS_SIZE); p += PROPS_SIZE;
    n = *p++;
    if (p + n * 28 > in + len) n = (int)((in + len - p) / 28);
    for (i = 0; i < n; i++) {
        member_t *m = &s_clan.mem[s_clan.nmem++];
        memcpy(&m->xuid, p, 8); p += 8;
        memcpy(m->tag, p, 16); m->tag[15] = 0; p += 16;
        memcpy(&m->priv, p, 4); p += 4;
    }
    return 1;
}

/* ---- tasks ----------------------------------------------------------------------- */

static ct_t *ct_new(int kind, uint64_t who)
{
    int i;
    ct_t *c = &s_ct[0];
    for (i = 0; i < MAX_CT; i++) if (!s_ct[i].task && !s_ct[i].kind) { c = &s_ct[i]; break; }
#ifdef OPENHALO2_NET
    if (c->visit > 0) olive_visit_free(c->visit);
#endif
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->who = who;
    return c;
}

static ct_t *ct_of(uint32_t task)
{
    int i;
    for (i = 0; i < MAX_CT; i++) if (s_ct[i].task && s_ct[i].task == task) return &s_ct[i];
    return NULL;
}

static int visit_inbox(uint64_t who, const uint8_t *m, int n)
{
#ifdef OPENHALO2_NET
    char room[32];
    inbox_room(room, sizeof room, who);
    return olive_visit(room, m, n, OLIVE_ONE, 4000);
#else
    (void)who; (void)m; (void)n;
    return 0;
#endif
}

/* 0 while working, then the result */
static uint32_t poll_ct(int i)
{
    ct_t *c = &s_ct[i];
#ifdef OPENHALO2_NET
    uint8_t r[OLIVE_MSG_MAX];
    int cnt, n = -1;
    if (c->visit > 0) {
        if (!olive_visit_poll(c->visit, &cnt)) return 0;
        if (cnt > 0) n = olive_visit_reply(c->visit, 0, r, sizeof r, NULL);
        olive_visit_free(c->visit);
        c->visit = 0;
        switch (c->kind) {
        case CT_USER:
            if (n >= 1 + 8 + 32 + 4 && r[0] == 'c') {
                memcpy(&c->id, r + 1, 8);
                memcpy(c->name, r + 9, 32);
                memcpy(&c->count, r + 41, 4);
                c->has = c->id != 0;
            }
            break;
        case CT_MEMBERS:          /* the founder's record, newer than ours */
        case CT_JOIN:
            lock();
            if (n > 0 && r[0] == 'j') {
                uint64_t id;
                uint32_t v;
                memcpy(&id, r + 1, 8);
                memcpy(&v, r + 9, 4);
                if (c->kind == CT_JOIN || (s_clan.used && id == s_clan.id && v >= s_clan.version)) {
                    record_take(r, n);
                    clans_save();
                    if (c->kind == CT_JOIN) {
                        char nm[16];
                        ascii_name(nm, s_clan.props);
                        fprintf(stderr, "  [XO] clans: joined \"%s\" (%d members)\n", nm, s_clan.nmem);
                    }
                }
            } else if (c->kind == CT_JOIN) {
                c->result = 0x80004005u;
            }
            unlock();
            break;
        case CT_RECRUIT_NAME:
            if (c->step == 0 && n >= 25 && r[0] == 'w') {      /* the name's holder: now the offer */
                uint8_t m[80];
                int k;
                memcpy(&c->who, r + 1, 8);
                lock();
                k = head(m, 'T');
                memcpy(m + k, &s_clan.id, 8); k += 8;
                memcpy(m + k, s_clan.props, 32); k += 32;
                unlock();
                c->step = 1;
                c->visit = visit_inbox(c->who, m, k);
                fprintf(stderr, "  [XO] clans: recruiting \"%.15s\"\n", (const char *)r + 9);
                return c->visit > 0 ? 0 : 0x80004005u;
            }
            if (c->step == 0) c->result = 0x80154002u;        /* nobody online by that name */
            break;
        default:
            break;
        }
    }
#endif
    return c->result ? c->result : XO_TASK_SUCCESS;
}

static void close_ct(int i)
{
#ifdef OPENHALO2_NET
    if (s_ct[i].visit > 0) olive_visit_free(s_ct[i].visit);
#endif
    memset(&s_ct[i], 0, sizeof s_ct[i]);
}

static uint32_t new_task(uint32_t va, ct_t *c)
{
    c->task = xo_task_new_polled(va, poll_ct, (int)(c - s_ct), close_ct);
    return c->task;
}

static void write_id(uint32_t at, uint64_t id) { memset(xo_ptr(at), 0, 12); memcpy(xo_ptr(at), &id, 8); }

/* ---- the API ----------------------------------------------------------------------- */

static int h_create(void)    /* (pad, &properties, &member properties, 100, event, &task) */
{
    uint64_t r;
    char name[16];
    lock();
    clans_load();
    memset(&s_clan, 0, sizeof s_clan);
    s_clan.used = 1;
#if defined(_WIN32)
    r = (uint64_t)GetTickCount64() * 0x9E3779B97F4A7C15ull ^ xo_self_xuid();
#else
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        r = ((uint64_t)ts.tv_nsec + (uint64_t)ts.tv_sec * 1000000007ull) * 0x9E3779B97F4A7C15ull ^ xo_self_xuid();
    }
#endif
    s_clan.id = TEAM_FLAG | (r & 0x0000FFFFFFFFFFFFull);
    s_clan.founder = xo_self_xuid();
    s_clan.version = 1;
    memcpy(s_clan.props, xo_ptr(xo_arg(1)), PROPS_SIZE);
    s_clan.nmem = 1;
    s_clan.mem[0].xuid = xo_self_xuid();
    strncpy(s_clan.mem[0].tag, xo_self_gamertag(), 15);
    s_clan.mem[0].priv = 0xFFFFFFFFu;
    clans_save();
    ascii_name(name, s_clan.props);
    unlock();
    fprintf(stderr, "  [XO] clans: \"%s\" created (%016llx)\n", name, (unsigned long long)s_clan.id);
    if (xo_arg(5)) xo_wr32(xo_arg(5), xo_task_new(0x3AFB80, 1, XO_TASK_SUCCESS));
    return ret(XO_HR_OK, 6);
}

static int h_delete(void)    /* (pad, team (12), event, &task) */
{
    lock();
    clans_load();
    if (s_clan.used && s_clan.id == arg64(1)) {
        memset(&s_clan, 0, sizeof s_clan);
        clans_save();
        fprintf(stderr, "  [XO] clans: clan deleted\n");
    }
    unlock();
    if (xo_arg(5)) xo_wr32(xo_arg(5), xo_task_new(0x3AFB8F, 1, XO_TASK_SUCCESS));
    return ret(XO_HR_OK, 6);
}

static int h_enum_user(void) /* (pad, user (12), event, &task) */
{
    uint64_t who = arg64(1);
    ct_t *c;
    lock();
    clans_load();
    unlock();
    c = ct_new(CT_USER, who);
    if (who && who != xo_self_xuid()) {
        uint8_t m[32];
        int n = head(m, 'C');
        c->visit = visit_inbox(who, m, n);
    }
    if (xo_arg(5)) xo_wr32(xo_arg(5), new_task(0x3AFC26, c));
    return ret(XO_HR_OK, 6);
}

static int h_enum_results(void)   /* (task, &count, ids) */
{
    ct_t *c = ct_of(xo_arg(0));
    uint32_t cap = xo_arg(1) ? xo_rd32(xo_arg(1)) : 0, n = 0;
    lock();
    if (c && c->kind == CT_USER && cap) {
        if (c->who == xo_self_xuid() && s_clan.used) { write_id(xo_arg(2), s_clan.id); n = 1; }
        else if (c->has) { write_id(xo_arg(2), c->id); n = 1; }
    } else if (c && c->kind == CT_MEMBERS && s_clan.used) {
        int i;
        for (i = 0; i < s_clan.nmem && n < cap; i++) write_id(xo_arg(2) + 12 * n++, s_clan.mem[i].xuid);
    }
    unlock();
    if (xo_arg(1)) xo_wr32(xo_arg(1), n);
    return ret(XO_HR_OK, 3);
}

static int h_details(void)   /* (task, team (12), &team) */
{
    ct_t *c = ct_of(xo_arg(0));
    uint64_t id = arg64(1);
    uint8_t *p = xo_ptr(xo_arg(4));
    int ok = 0;
    memset(p, 0, TEAM_SIZE);
    memcpy(p, &id, 8);
    lock();
    if (s_clan.used && s_clan.id == id) {
        uint32_t n = (uint32_t)s_clan.nmem;
        memcpy(p + 0xC, s_clan.props, PROPS_SIZE);
        memcpy(p + 0x694, &n, 4);
        ok = 1;
    } else if (c && c->has && c->id == id) {
        memcpy(p + 0xC, c->name, 32);
        memcpy(p + 0x694, &c->count, 4);
        ok = 1;
    }
    unlock();
    return ret(ok ? XO_HR_OK : XO_HR_FAIL, 5);
}

static int h_members(void)   /* (pad, team (12), 1, event, &task) */
{
    ct_t *c = ct_new(CT_MEMBERS, arg64(1));
    uint64_t founder = 0;
    lock();
    clans_load();
    if (s_clan.used && s_clan.id == c->who) founder = s_clan.founder;
    unlock();
    if (founder && founder != xo_self_xuid()) {     /* the founder has the current list */
        uint8_t m[40];
        int n = head(m, 'V');
        memcpy(m + n, &c->who, 8); n += 8;
        c->visit = visit_inbox(founder, m, n);
    }
    if (xo_arg(6)) xo_wr32(xo_arg(6), new_task(0x3AFC76, c));
    return ret(XO_HR_OK, 7);
}

static int h_member_details(void)   /* (task, member (12), &member) */
{
    uint64_t x = arg64(1);
    uint8_t *p = xo_ptr(xo_arg(4));
    int i, ok = 0;
    memset(p, 0, MEMBER_SIZE);
    lock();
    for (i = 0; i < s_clan.nmem && !ok; i++) {
        if (s_clan.mem[i].xuid != x) continue;
        memcpy(p, &x, 8);
        memcpy(p + 0xC, s_clan.mem[i].tag, 15);
        memcpy(p + 0x1C, &s_clan.mem[i].priv, 4);
        ok = 1;
    }
    unlock();
    return ret(ok ? XO_HR_OK : XO_HR_FAIL, 5);
}

static int h_member_remove(void)    /* (pad, team (12), member (12), event, &task) */
{
    uint64_t x = arg64(4);
    int i;
    lock();
    clans_load();
    for (i = 0; i < s_clan.nmem; i++)
        if (s_clan.mem[i].xuid == x) { s_clan.mem[i] = s_clan.mem[--s_clan.nmem]; s_clan.version++; break; }
    if (x == xo_self_xuid() || !s_clan.nmem) memset(&s_clan, 0, sizeof s_clan);   /* left the clan */
    clans_save();
    unlock();
    if (xo_arg(8)) xo_wr32(xo_arg(8), xo_task_new(0x3AFBF1, 1, XO_TASK_SUCCESS));
    return ret(XO_HR_OK, 9);
}

static int h_member_set(void)       /* (pad, team (12), member (12), &props, event, &task) */
{
    uint64_t x = arg64(4);
    int i;
    lock();
    clans_load();
    for (i = 0; i < s_clan.nmem; i++)
        if (s_clan.mem[i].xuid == x) { memcpy(&s_clan.mem[i].priv, xo_ptr(xo_arg(7)), 4); s_clan.version++; }
    clans_save();
    unlock();
    if (xo_arg(9)) xo_wr32(xo_arg(9), xo_task_new(0x3AFBB9, 1, XO_TASK_SUCCESS));
    return ret(XO_HR_OK, 10);
}

static int h_recruit(void)   /* (pad, team (12), member (12), &props, message, event, &task) */
{
    uint64_t who = arg64(4);
    ct_t *c = ct_new(CT_USER, who);
    uint8_t m[80];
    int n;
    lock();
    n = head(m, 'T');
    memcpy(m + n, &s_clan.id, 8); n += 8;
    memcpy(m + n, s_clan.props, 32); n += 32;
    unlock();
    c->kind = CT_RECRUIT_NAME;
    c->step = 1;
    c->visit = visit_inbox(who, m, n);
    fprintf(stderr, "  [XO] clans: recruiting %016llx\n", (unsigned long long)who);
    if (xo_arg(10)) xo_wr32(xo_arg(10), new_task(0x3A8399, c));
    return ret(XO_HR_OK, 11);
}

static int h_recruit_name(void)   /* (pad, team (12), gamertag, &props, message, event, &task) */
{
    ct_t *c = ct_new(CT_RECRUIT_NAME, 0);
#ifdef OPENHALO2_NET
    char tag[16], room[32];
    uint8_t m[32];
    int i, n = head(m, 'W');
    memset(tag, 0, sizeof tag);
    strncpy(tag, (const char *)xo_ptr(xo_arg(4)), 15);
    for (i = 0; tag[i]; i++) {
        char ch = tag[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        tag[i] = ch == ' ' ? '_' : ch;
    }
    snprintf(room, sizeof room, "h2n-%s", tag);
    c->visit = olive_visit(room, m, n, OLIVE_ONE, 4000);
#endif
    if (xo_arg(8)) xo_wr32(xo_arg(8), new_task(0x3A83D4, c));
    return ret(XO_HR_OK, 9);
}

static int h_answer_recruit(void)   /* (pad, team (12), answer, event, &task) */
{
    uint64_t id = arg64(1);
    uint32_t answer = xo_arg(4);
    ct_t *c = ct_new(CT_JOIN, 0);
    offer_t *o = NULL;
    int i;
    lock();
    for (i = 0; i < MAX_OFFERS && !o; i++) if (s_offer[i].used && s_offer[i].id == id) o = &s_offer[i];
    for (i = 0; i < MAX_OFFERS && !o; i++) if (s_offer[i].used && s_offer[i].from == id) o = &s_offer[i];
    if (o) id = o->id;
    if (o && answer == 1) {
        uint8_t m[40];
        int n = head(m, 'U');
        memcpy(m + n, &id, 8); n += 8;
        c->who = o->from;
        unlock();
        c->visit = visit_inbox(c->who, m, n);
        lock();
    } else {
        c->kind = CT_USER;      /* nothing to wait for */
    }
    if (o) o->used = 0;
    unlock();
    if (o) xo_friends_message_drop(o->from, 4);
    if (xo_arg(6)) xo_wr32(xo_arg(6), new_task(0x3A8407, c));
    return ret(XO_HR_OK, 7);
}

int xo_clans_call(uint32_t va)
{
    switch (va) {
    case 0x3AFB80: return h_create();
    case 0x3AFB8F: return h_delete();
    case 0x3AFC26: return h_enum_user();
    case 0x3AEBA5: return h_enum_results();
    case 0x3AEBB0: return h_enum_results();
    case 0x3AFC50: return h_details();
    case 0x3AFC76: return h_members();
    case 0x3AFCA3: return h_member_details();
    case 0x3AFBF1: return h_member_remove();
    case 0x3AFBB9: return h_member_set();
    case 0x3A8399: return h_recruit();
    case 0x3A83D4: return h_recruit_name();
    case 0x3A8407: return h_answer_recruit();
    default: return 0;
    }
}

/* clan messages at our inbox (transport thread); p is after the 25-byte head */
int xo_clans_inbox(char type, uint64_t from, const char *tag, const uint8_t *p, int len, uint8_t *reply, int cap)
{
    int n = 0, i;
    lock();
    switch (type) {
    case 'C': {
        uint64_t id = 0;
        uint32_t cnt = 0;
        if (cap < 1 + 8 + 32 + 4) break;
        memset(reply, 0, 1 + 8 + 32 + 4);
        reply[0] = 'c';
        if (s_loaded && s_clan.used) {
            id = s_clan.id;
            cnt = (uint32_t)s_clan.nmem;
            memcpy(reply + 9, s_clan.props, 32);
        }
        memcpy(reply + 1, &id, 8);
        memcpy(reply + 41, &cnt, 4);
        n = 1 + 8 + 32 + 4;
        break;
    }
    case 'T': {
        offer_t *o = NULL;
        char nm[16];
        if (len < 8 + 32) break;
        for (i = 0; i < MAX_OFFERS && !o; i++) if (!s_offer[i].used) o = &s_offer[i];
        if (!o) o = &s_offer[0];
        o->used = 1;
        memcpy(&o->id, p, 8);
        memcpy(o->name, p + 8, 32);
        o->from = from;
        ascii_name(nm, o->name);
        unlock();
        xo_friends_message(from, tag, 4, o->id);
        fprintf(stderr, "  [XO] clans: \"%s\" asks us to join \"%s\"\n", tag, nm);
        reply[0] = 'k';
        return 1;
    }
    case 'U':
    case 'V': {
        uint64_t id;
        if (len < 8) break;
        memcpy(&id, p, 8);
        if (!s_loaded || !s_clan.used || s_clan.id != id) break;
        if (type == 'U') {
            int known = 0;
            for (i = 0; i < s_clan.nmem; i++) if (s_clan.mem[i].xuid == from) known = 1;
            if (!known && s_clan.nmem < MAX_MEMBERS) {
                member_t *m = &s_clan.mem[s_clan.nmem++];
                memset(m, 0, sizeof *m);
                m->xuid = from;
                strncpy(m->tag, tag, 15);
                m->priv = PRIV_MEMBER;
                s_clan.version++;
                clans_save();
                fprintf(stderr, "  [XO] clans: \"%s\" joined the clan\n", tag);
            }
        }
        n = record_put(reply, cap);
        break;
    }
    default:
        break;
    }
    unlock();
    return n;
}
