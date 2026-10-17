/* Open Live: friends, presence, friend requests and game invites with no
 * server (docs/OPEN_LIVE_DESIGN.md 3.2 / 3.3).
 *
 * Every signed-in player holds two relay rooms (net/olive.c):
 *   h2u-<xuid in base 32>   the inbox: friends' clients visit it
 *   h2n-<gamertag>          the name: lets others find the player by gamertag
 * Messages to an inbox (type byte, sender id u64, sender gamertag 16):
 *   'P' presence exchange (title id, state flags, session id, state data);
 *       the holder answers 'p' with its own
 *   'F' friend request          'A' its answer (1 byte: 0 no, 1 yes, 2 block)
 *   'X' friend removed          'I' game invite (session id, title id)
 *   'J' invite answer (1 byte)  'S' session query (session id): the holder
 *       answers with the session's record ('q', openlive_match.c) or 'x'
 * and to a name room: 'W' (who) -> 'w' id + gamertag.
 * Each client exchanges presence with every friend every few seconds;
 * requests, answers and invites are re-sent until the other side has
 * acknowledged them ('k'), so a friend who is offline gets them on return.
 * Relationships are kept in openlive_friends.txt beside the program.
 *
 * The title's view (layouts measured in its code):
 *   friend record 0x56 bytes (stride, 0x1A3728): id +0 (12), gamertag +0xC,
 *   state +0x1C, session id +0x28, title id +0x30, state data size +0x34,
 *   state data +0x35 (0x8CEC0, 0x8D230)
 *   state flags (0x8CF00): online 0x1, playing 0x2, voice 0x8, joinable
 *   0x10, received request 0x80000000, sent request 0x40000000, invite
 *   rejected 0x20000000, received invite 0x08000000, sent invite 0x04000000
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "openlive.h"

#ifdef OPENHALO2_NET
#include "olive.h"
#endif

#define TITLE_ID        0x4D530064u
#define MAX_FRIENDS     100
#define FRIEND_SIZE     0x56u
#define PRESENCE_MS     6000u
#define OFFLINE_MS      20000u

#define FS_ONLINE        0x00000001u
#define FS_PLAYING       0x00000002u
#define FS_VOICE         0x00000008u
#define FS_JOINABLE      0x00000010u
#define FS_RECV_REQUEST  0x80000000u
#define FS_SENT_REQUEST  0x40000000u
#define FS_INV_REJECTED  0x20000000u
#define FS_RECV_INVITE   0x08000000u
#define FS_SENT_INVITE   0x04000000u

enum { REL_NONE, REL_FRIEND, REL_SENT, REL_RECEIVED };

typedef struct {
    int used, rel;
    uint64_t xuid;
    char tag[16];
    /* their presence */
    int online;
    uint32_t pflags;
    uint8_t kid[8];
    uint8_t data[32];
    int data_len;
    uint64_t seen;
    /* invites */
    int invite_in, invite_out, invite_rejected;
    uint8_t invite_kid[8];
    /* messages owed to them, re-sent until acknowledged */
    int owe_request, owe_answer, owe_invite, owe_remove, answer;
    int visit;            /* presence or delivery visit in flight */
    uint64_t next;        /* when to visit next */
} friend_t;

static friend_t s_fr[MAX_FRIENDS];
static uint64_t s_self;
static char s_tag[16];
static int s_inbox, s_name;
static int s_signed_in;
static uint32_t s_state_flags;      /* our presence (NotificationSetState) */
static uint8_t s_state_kid[8], s_state_data[32];
static int s_state_len;
static int s_lookup_visit;          /* a request by gamertag in flight */
static char s_lookup_tag[16];
static int s_lookup_done, s_lookup_ok;

/* Messages (the title's message list, 0x8E750): Halo 2 shows a game invite
 * as a message of type 3 from the inviter (the Msg column of the friends
 * list, the message screen 0x2C788A, the notification channel 0x6D080).
 * Received invites are kept here until answered. */
#define MAX_MSG 16
typedef struct { int used; uint32_t id; uint64_t xuid; char tag[16]; uint8_t type; uint8_t kid[8];
                 uint64_t ft; int read; uint64_t ctx; } msg_t;
static msg_t s_msg[MAX_MSG];
static uint32_t s_msg_next = 1;

#if defined(_WIN32)
static SRWLOCK s_lk = SRWLOCK_INIT;
static void lock(void)   { AcquireSRWLockExclusive(&s_lk); }
static void unlock(void) { ReleaseSRWLockExclusive(&s_lk); }
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
static void lock(void) {}
static void unlock(void) {}
static uint64_t now_ms(void) { return 0; }
#endif

/* ---- names ----------------------------------------------------------------------- */

static void b32(char *out, uint64_t v)
{
    static const char a[] = "0123456789abcdefghjkmnpqrstvwxyz";
    int i, o = 0;
    for (i = 60; i > -5; i -= 5) out[o++] = a[(i >= 0 ? v >> i : v << -i) & 31];
    out[o] = 0;
}

static void inbox_name(char *out, size_t n, uint64_t xuid)
{
    char k[16];
    b32(k, xuid);
    snprintf(out, n, "h2u-%s", k);
}

static void tag_room(char *out, size_t n, const char *tag)
{
    char t[20];
    int i;
    for (i = 0; tag[i] && i < 15; i++) {
        char c = tag[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        t[i] = c == ' ' ? '_' : c;
    }
    t[i] = 0;
    snprintf(out, n, "h2n-%s", t);
}

static friend_t *by_xuid(uint64_t x)
{
    int i;
    for (i = 0; i < MAX_FRIENDS; i++) if (s_fr[i].used && s_fr[i].xuid == x) return &s_fr[i];
    return NULL;
}

static friend_t *add_friend(uint64_t x, const char *tag, int rel)
{
    friend_t *f = by_xuid(x);
    int i;
    if (!f) for (i = 0; i < MAX_FRIENDS && !f; i++) if (!s_fr[i].used) f = &s_fr[i];
    if (!f) return NULL;
    if (!f->used) { memset(f, 0, sizeof *f); f->used = 1; f->xuid = x; }
    if (tag && tag[0]) { memcpy(f->tag, tag, 15); f->tag[15] = 0; }
    if (rel) f->rel = rel;
    return f;
}

/* ---- the relationships file --------------------------------------------------------- */

static const char *friends_path(void)
{
    const char *p = getenv("RECOMP_OL_FRIENDS");
    return p && *p ? p : "openlive_friends.txt";
}

static void friends_save(void)
{
    /* one line per relationship of every local account: owner, friend,
     * F (friends) / S (request sent) / R (request received), gamertag */
    char lines[64][96];
    int n = 0, i;
    FILE *f = fopen(friends_path(), "r");
    if (f) {
        char l[96];
        while (n < 64 && fgets(l, sizeof l, f)) {
            unsigned long long o = strtoull(l, NULL, 16);
            if (o && o != s_self) { strncpy(lines[n], l, 95); lines[n][95] = 0; n++; }
        }
        fclose(f);
    }
    f = fopen(friends_path(), "w");
    if (!f) return;
    for (i = 0; i < n; i++) fputs(lines[i], f);
    for (i = 0; i < MAX_FRIENDS; i++) {
        friend_t *r = &s_fr[i];
        if (!r->used || r->rel == REL_NONE) continue;
        fprintf(f, "%016llx %016llx %c %s\n", (unsigned long long)s_self, (unsigned long long)r->xuid,
                r->rel == REL_FRIEND ? 'F' : r->rel == REL_SENT ? 'S' : 'R', r->tag);
    }
    fclose(f);
}

static void friends_load(void)
{
    FILE *f = fopen(friends_path(), "r");
    char l[128];
    memset(s_fr, 0, sizeof s_fr);
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        unsigned long long o, x;
        char rel, tag[32] = "";
        friend_t *r;
        if (sscanf(l, "%llx %llx %c %31[^\r\n]", &o, &x, &rel, tag) < 3 || o != s_self) continue;
        r = add_friend(x, tag, rel == 'F' ? REL_FRIEND : rel == 'S' ? REL_SENT : REL_RECEIVED);
        if (r && r->rel == REL_SENT) r->owe_request = 1;
    }
    fclose(f);
}

/* ---- messages ---------------------------------------------------------------------- */

static int msg_head(uint8_t *m, char type)
{
    m[0] = (uint8_t)type;
    memcpy(m + 1, &s_self, 8);
    memset(m + 9, 0, 16);
    memcpy(m + 9, s_tag, 15);
    return 25;
}

static int presence_body(uint8_t *m)
{
    int n = msg_head(m, 'P');
    uint32_t t = TITLE_ID, fl = s_state_flags | FS_ONLINE;
    memcpy(m + n, &t, 4); n += 4;
    memcpy(m + n, &fl, 4); n += 4;
    memcpy(m + n, s_state_kid, 8); n += 8;
    m[n++] = (uint8_t)s_state_len;
    memcpy(m + n, s_state_data, (size_t)s_state_len); n += s_state_len;
    return n;
}

static void presence_take(friend_t *f, const uint8_t *m, int len)
{
    uint32_t fl;
    int dl;
    if (len < 25 + 4 + 4 + 8 + 1) return;
    memcpy(&fl, m + 29, 4);
    f->online = 1;
    f->pflags = fl;
    memcpy(f->kid, m + 33, 8);
    dl = m[41];
    if (dl > 32 || 42 + dl > len) dl = 0;
    memcpy(f->data, m + 42, (size_t)dl);
    f->data_len = dl;
    f->seen = now_ms();
}

static uint64_t filetime_now(void)
{
#if defined(_WIN32)
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime;
#else
    return 0;
#endif
}

/* one message per sender and type: a newer invite replaces the older (locked) */
static void msg_add(uint64_t xuid, const char *tag, uint8_t type, const uint8_t *kid)
{
    msg_t *m = NULL;
    int i;
    for (i = 0; i < MAX_MSG && !m; i++) if (s_msg[i].used && s_msg[i].xuid == xuid && s_msg[i].type == type) m = &s_msg[i];
    for (i = 0; i < MAX_MSG && !m; i++) if (!s_msg[i].used) m = &s_msg[i];
    if (!m) m = &s_msg[0];
    memset(m, 0, sizeof *m);
    m->used = 1;
    m->id = s_msg_next++;
    m->xuid = xuid;
    memcpy(m->tag, tag, 15);
    m->type = type;
    if (kid) memcpy(m->kid, kid, 8);
    m->ft = filetime_now();
}

static void msg_drop(uint64_t xuid, uint8_t type)
{
    int i;
    for (i = 0; i < MAX_MSG; i++) if (s_msg[i].used && s_msg[i].xuid == xuid && s_msg[i].type == type) s_msg[i].used = 0;
}

static msg_t *msg_by_id(uint32_t id)
{
    int i;
    for (i = 0; i < MAX_MSG; i++) if (s_msg[i].used && s_msg[i].id == id) return &s_msg[i];
    return NULL;
}

/* a message from elsewhere (a clan recruit: the recruiter, the clan as its context) */
void xo_friends_message(uint64_t from, const char *name, uint8_t type, uint64_t ctx)
{
    int i;
    lock();
    msg_add(from, name, type, NULL);
    for (i = 0; i < MAX_MSG; i++)
        if (s_msg[i].used && s_msg[i].xuid == from && s_msg[i].type == type) s_msg[i].ctx = ctx;
    unlock();
}

void xo_friends_message_drop(uint64_t from, uint8_t type)
{
    lock();
    msg_drop(from, type);
    unlock();
}

/* an inbox visitor (transport thread) */
static int on_inbox(void *ctx, int from, const uint8_t *m, int len, uint8_t *reply, int cap)
{
    uint64_t x;
    char tag[16];
    friend_t *f;
    int n = 0;
    (void)ctx; (void)from;
    if (len < 25 || cap < 128) return 0;
    memcpy(&x, m + 1, 8);
    memcpy(tag, m + 9, 15); tag[15] = 0;
    lock();
    f = by_xuid(x);
    switch (m[0]) {
    case 'P':
        if (f && f->rel == REL_FRIEND) {
            presence_take(f, m, len);
            n = presence_body(reply);
            reply[0] = 'p';
        }
        break;
    case 'O':          /* anyone: are you online (recent players' status) */
        n = presence_body(reply);
        reply[0] = 'o';
        break;
    case 'F':
        if (!f || f->rel == REL_NONE) {
            f = add_friend(x, tag, REL_RECEIVED);
            fprintf(stderr, "  [XO] friends: request from \"%s\"\n", tag);
            friends_save();
        } else if (f->rel == REL_SENT) {
            f->rel = REL_FRIEND;           /* both asked: friends */
            f->owe_request = 0;
            friends_save();
        }
        n = msg_head(reply, 'k');
        break;
    case 'A':
        if (f && f->rel == REL_SENT && len >= 26) {
            if (m[25] == 1) { f->rel = REL_FRIEND; fprintf(stderr, "  [XO] friends: \"%s\" accepted\n", f->tag); }
            else { f->rel = REL_NONE; f->used = 0; fprintf(stderr, "  [XO] friends: \"%s\" declined\n", tag); }
            f->owe_request = 0;
            friends_save();
        }
        n = msg_head(reply, 'k');
        break;
    case 'X':
        if (f) { f->used = 0; friends_save(); fprintf(stderr, "  [XO] friends: \"%s\" removed us\n", tag); }
        n = msg_head(reply, 'k');
        break;
    case 'I':
        if (f && f->rel == REL_FRIEND && len >= 25 + 8) {
            f->invite_in = 1;
            memcpy(f->invite_kid, m + 25, 8);
            fprintf(stderr, "  [XO] friends: game invite from \"%s\"\n", f->tag);
            msg_add(f->xuid, f->tag, 3, f->invite_kid);
        }
        n = msg_head(reply, 'k');
        break;
    case 'J':
        if (f && len >= 26) {
            f->invite_out = 0;
            f->invite_rejected = m[25] != 1;
            fprintf(stderr, "  [XO] friends: \"%s\" %s the invite\n", f->tag, m[25] == 1 ? "accepted" : "declined");
        }
        n = msg_head(reply, 'k');
        break;
    case 'C': case 'T': case 'U': case 'V':   /* clans (openlive_clans.c) */
        unlock();
        return xo_clans_inbox((char)m[0], x, tag, m + 25, len - 25, reply, cap);
    case 'M': case 'R': case 'N':
        unlock();
        return xo_match_inbox((char)m[0], x, m + 25, len - 25, reply, cap);
    case 'S':
        if (len >= 25 + 8) {
            unlock();
            n = xo_match_record_for_kid(m + 25, reply, cap);
            if (n <= 0) { reply[0] = 'x'; n = 1; }
            return n;
        }
        break;
    default:
        break;
    }
    unlock();
    return n;
}

/* the name room: who holds this gamertag */
static int on_name(void *ctx, int from, const uint8_t *m, int len, uint8_t *reply, int cap)
{
    (void)ctx; (void)from;
    if (len < 1 || m[0] != 'W' || cap < 32) return 0;
    return msg_head(reply, 'w');
}

/* ---- the pump: presence, deliveries ------------------------------------------------ */

static void visit_friend(friend_t *f)
{
#ifdef OPENHALO2_NET
    uint8_t m[OLIVE_MSG_MAX];
    char room[32];
    int n;
    if (f->owe_request) n = msg_head(m, 'F');
    else if (f->owe_answer) { n = msg_head(m, 'A'); m[n++] = (uint8_t)f->answer; }
    else if (f->owe_remove) n = msg_head(m, 'X');
    else if (f->owe_invite) { n = msg_head(m, 'I'); memcpy(m + n, f->invite_kid, 8); n += 8; }
    else if (f->rel == REL_FRIEND) n = presence_body(m);
    else return;
    inbox_name(room, sizeof room, f->xuid);
    f->visit = olive_visit(room, m, n, OLIVE_ONE, 4000);
#else
    (void)f;
#endif
}

static void visit_done(friend_t *f)
{
#ifdef OPENHALO2_NET
    uint8_t r[OLIVE_MSG_MAX];
    int cnt = 0, n, rtt;
    olive_visit_poll(f->visit, &cnt);
    n = cnt > 0 ? olive_visit_reply(f->visit, 0, r, sizeof r, &rtt) : -1;
    olive_visit_free(f->visit);
    f->visit = 0;
    if (n > 0 && r[0] == 'k') {
        if (f->owe_request) f->owe_request = 0;
        else if (f->owe_answer) { f->owe_answer = 0; if (f->rel == REL_NONE) f->used = 0; }
        else if (f->owe_remove) { f->owe_remove = 0; f->used = 0; }
        else if (f->owe_invite) f->owe_invite = 0;
        f->next = 0;        /* next message right away */
        return;
    }
    if (n > 0 && r[0] == 'p') {
        lock();
        presence_take(f, r, n);
        unlock();
    } else if (f->online && now_ms() - f->seen > OFFLINE_MS) {
        f->online = 0;
    }
    f->next = now_ms() + PRESENCE_MS;
#else
    (void)f;
#endif
}

static void pres_pump(void);

void xo_friends_pump(void)
{
#ifdef OPENHALO2_NET
    int i, busy = 0;
    uint64_t t = now_ms();
    olive_pump();
    if (!s_signed_in) return;
    pres_pump();
    for (i = 0; i < MAX_FRIENDS; i++) {
        friend_t *f = &s_fr[i];
        int cnt;
        if (!f->used) continue;
        if (f->visit > 0) {
            if (olive_visit_poll(f->visit, &cnt)) visit_done(f);
            else busy++;
        } else if (t >= f->next && busy < 6) {
            visit_friend(f);
            if (f->visit > 0) busy++;
            f->next = t + PRESENCE_MS;
        }
    }
    if (s_lookup_visit > 0) {
        int cnt;
        if (olive_visit_poll(s_lookup_visit, &cnt)) {
            uint8_t r[64];
            int n = cnt > 0 ? olive_visit_reply(s_lookup_visit, 0, r, sizeof r, NULL) : -1;
            olive_visit_free(s_lookup_visit);
            s_lookup_visit = 0;
            s_lookup_done = 1;
            if (n >= 25 && r[0] == 'w') {
                uint64_t x;
                char tag[16];
                friend_t *f;
                memcpy(&x, r + 1, 8);
                memcpy(tag, r + 9, 15); tag[15] = 0;
                if (x != s_self) {
                    lock();
                    f = add_friend(x, tag, 0);
                    if (f && f->rel != REL_FRIEND) { f->rel = REL_SENT; f->owe_request = 1; f->next = 0; }
                    friends_save();
                    unlock();
                    s_lookup_ok = 1;
                    fprintf(stderr, "  [XO] friends: \"%s\" found by gamertag, request sent\n", tag);
                }
            } else {
                fprintf(stderr, "  [XO] friends: nobody online is called \"%s\"\n", s_lookup_tag);
            }
        }
    }
#endif
}

/* ---- presence of other players (XOnlinePresence*) -----------------------------------------
 * The title asks for the online state of the players in its friends and
 * players (recent players) lists, by group ('frnd', 'clan'), with records of
 * 0x3D bytes (0x8C470): id +0, state +0xC (the friend flags), session id
 * +0x10, title id +0x18. Friends' state comes from the presence exchange;
 * anyone else is asked once at their inbox ('O'). */

#define MAX_PRES 128
typedef struct { uint32_t group; uint64_t xuid; int online, visit; uint32_t flags; uint8_t kid[8]; } pres_t;
static pres_t s_pres[MAX_PRES];
static int s_npres;

static void pres_pump(void)
{
#ifdef OPENHALO2_NET
    int i, cnt;
    for (i = 0; i < s_npres; i++) {
        pres_t *p = &s_pres[i];
        if (p->visit <= 0 || !olive_visit_poll(p->visit, &cnt)) continue;
        if (cnt > 0) {
            uint8_t r[128];
            int n = olive_visit_reply(p->visit, 0, r, sizeof r, NULL);
            if (n >= 42 && r[0] == 'o') {
                p->online = 1;
                memcpy(&p->flags, r + 29, 4);
                memcpy(p->kid, r + 33, 8);
            }
        }
        olive_visit_free(p->visit);
        p->visit = 0;
    }
#endif
}

static int h_presence_add(void)      /* (task, group, count, xuids (12 bytes each)) */
{
    uint32_t g = xo_arg(1), n = xo_arg(2), a = xo_arg(3), i;
    for (i = 0; i < n && s_npres < MAX_PRES; i++) {
        pres_t *p = &s_pres[s_npres++];
        memset(p, 0, sizeof *p);
        p->group = g;
        memcpy(&p->xuid, xo_ptr(a + 12 * i), 8);
    }
    return ret(XO_HR_OK, 4);
}

static int h_presence_submit(void)   /* (task) */
{
#ifdef OPENHALO2_NET
    int i;
    for (i = 0; i < s_npres; i++) {
        pres_t *p = &s_pres[i];
        friend_t *f;
        uint8_t m[32];
        char room[32];
        lock();
        f = by_xuid(p->xuid);
        unlock();
        if ((f && f->rel == REL_FRIEND) || p->xuid == s_self || p->visit > 0) continue;
        msg_head(m, 'O');
        inbox_name(room, sizeof room, p->xuid);
        p->visit = olive_visit(room, m, 25, OLIVE_ONE, 4000);
    }
#endif
    return ret(XO_HR_OK, 1);
}

static int h_presence_clear(void)    /* (task) */
{
#ifdef OPENHALO2_NET
    int i;
    for (i = 0; i < s_npres; i++) if (s_pres[i].visit > 0) olive_visit_free(s_pres[i].visit);
#endif
    s_npres = 0;
    return ret(XO_HR_OK, 1);
}

static int h_presence_latest(void)   /* (task, group, count, records) */
{
    uint32_t g = xo_arg(1), max = xo_arg(2), out = xo_arg(3), n = 0, title = TITLE_ID;
    int i;
    for (i = 0; i < s_npres && n < max; i++) {
        pres_t *p = &s_pres[i];
        uint8_t *r = xo_ptr(out + 0x3D * n);
        friend_t *f;
        uint32_t st = 0;
        if (p->group != g) continue;
        memset(r, 0, 0x3D);
        memcpy(r, &p->xuid, 8);
        lock();
        f = by_xuid(p->xuid);
        if (f && f->rel == REL_FRIEND && f->online) { st = FS_ONLINE | (f->pflags & (FS_PLAYING | FS_VOICE | FS_JOINABLE)); memcpy(r + 0x10, f->kid, 8); }
        unlock();
        if (!st && p->online) { st = FS_ONLINE | (p->flags & (FS_PLAYING | FS_VOICE | FS_JOINABLE)); memcpy(r + 0x10, p->kid, 8); }
        if (st) memcpy(r + 0x18, &title, 4);
        memcpy(r + 0xC, &st, 4);
        n++;
    }
    return ret(XO_HR_OK, 4);
}

/* ---- sign-in -------------------------------------------------------------------------- */

void xo_friends_signin(uint64_t xuid, const char *tag)
{
#ifdef OPENHALO2_NET
    char room[32];
    if (s_signed_in && s_self == xuid) return;
    s_self = xuid;
    memset(s_tag, 0, sizeof s_tag);
    strncpy(s_tag, tag, 15);
    friends_load();
    inbox_name(room, sizeof room, xuid);
    s_inbox = olive_hold(room, on_inbox, NULL);
    tag_room(room, sizeof room, tag);
    s_name = olive_hold(room, on_name, NULL);
    s_signed_in = 1;
    {
        int i, n = 0;
        for (i = 0; i < MAX_FRIENDS; i++) n += s_fr[i].used;
        inbox_name(room, sizeof room, xuid);
        fprintf(stderr, "  [XO] friends: \"%s\" online (inbox %s), %d relationship(s)\n", tag, room, n);
    }
#else
    (void)xuid; (void)tag;
#endif
}

/* the session id a friend is in, for joining them; 1 if found */
int xo_friends_inbox_for_kid(const uint8_t *kid, char *room, size_t n)
{
    int i, found = 0;
    lock();
    for (i = 0; i < MAX_FRIENDS && !found; i++) {
        friend_t *f = &s_fr[i];
        if (!f->used) continue;
        if ((f->online && !memcmp(f->kid, kid, 8)) || (f->invite_in && !memcmp(f->invite_kid, kid, 8))) {
            inbox_name(room, n, f->xuid);
            found = 1;
        }
    }
    unlock();
    return found;
}

/* ---- the API -------------------------------------------------------------------------- */

static int ret(uint32_t v, int nargs) { xo_return(v, nargs); return 1; }

static void friend_write(uint32_t at, const friend_t *f)
{
    uint8_t *p = xo_ptr(at);
    uint32_t st = 0, title = TITLE_ID;
    memset(p, 0, FRIEND_SIZE);
    memcpy(p, &f->xuid, 8);
    memcpy(p + 0xC, f->tag, 15);
    if (f->rel == REL_RECEIVED) st |= FS_RECV_REQUEST;
    if (f->rel == REL_SENT) st |= FS_SENT_REQUEST;
    if (f->rel == REL_FRIEND && f->online) {
        st |= FS_ONLINE | (f->pflags & (FS_PLAYING | FS_VOICE | FS_JOINABLE));
        memcpy(p + 0x28, f->kid, 8);
        memcpy(p + 0x30, &title, 4);
        p[0x34] = (uint8_t)f->data_len;
        memcpy(p + 0x35, f->data, (size_t)f->data_len);
    }
    if (f->invite_in) { st |= FS_RECV_INVITE; memcpy(p + 0x28, f->invite_kid, 8); memcpy(p + 0x30, &title, 4); }
    if (f->invite_out) st |= FS_SENT_INVITE;
    if (f->invite_rejected) st |= FS_INV_REJECTED;
    memcpy(p + 0x1C, &st, 4);
}

static int h_friends_latest(void)    /* (pad, max, friends) -> count */
{
    uint32_t max = xo_arg(1), out = xo_arg(2), n = 0;
    int i;
    lock();
    for (i = 0; i < MAX_FRIENDS && n < max; i++) {
        if (!s_fr[i].used || s_fr[i].rel == REL_NONE) continue;
        friend_write(out + FRIEND_SIZE * n, &s_fr[i]);
        n++;
    }
    unlock();
    return ret(n, 3);
}

static uint64_t friend_arg_xuid(uint32_t p) { uint64_t x; memcpy(&x, xo_ptr(p), 8); return x; }

static int h_friends_answer(void)    /* (pad, &friend, answer: 0 no, 1 yes, 2 block (0x8CE40)) */
{
    friend_t *f;
    uint32_t a = xo_arg(2);
    lock();
    f = by_xuid(friend_arg_xuid(xo_arg(1)));
    if (f && f->rel == REL_RECEIVED) {
        f->answer = a == 1 ? 1 : a == 2 ? 2 : 0;
        f->owe_answer = 1;
        f->rel = a == 1 ? REL_FRIEND : REL_NONE;
        f->next = 0;
        fprintf(stderr, "  [XO] friends: request from \"%s\" %s\n", f->tag, a == 1 ? "accepted" : "declined");
        friends_save();
    }
    unlock();
    return ret(XO_HR_OK, 3);
}

static int h_friends_remove(void)    /* (pad, &friend) */
{
    friend_t *f;
    lock();
    f = by_xuid(friend_arg_xuid(xo_arg(1)));
    if (f) { f->rel = REL_NONE; f->owe_remove = 1; f->next = 0; friends_save(); }
    unlock();
    return ret(XO_HR_OK, 2);
}

static void request_xuid(uint64_t x)
{
    friend_t *f;
    lock();
    f = add_friend(x, NULL, 0);
    if (f && f->rel != REL_FRIEND) { f->rel = REL_SENT; f->owe_request = 1; f->next = 0; friends_save(); }
    unlock();
}

static int h_friends_request(void)   /* (pad, xuid lo, xuid hi, flags, message, event, &task) */
{
    uint64_t x = (uint64_t)xo_arg(2) << 32 | xo_arg(1);
    request_xuid(x);
    fprintf(stderr, "  [XO] friends: request to %016llx\n", (unsigned long long)x);
    if (xo_arg(6)) xo_wr32(xo_arg(6), xo_task_new(0x3A835D, 1, XO_TASK_SUCCESS));
    return ret(XO_HR_OK, 7);
}

static uint32_t poll_lookup(int user)
{
    (void)user;
    if (!s_lookup_done) return 0;
    return s_lookup_ok ? XO_TASK_SUCCESS : 0x80154002u;   /* "no such user" kind of failure */
}

static int h_friends_request_name(void)   /* (pad, gamertag, message, event, &task) */
{
#ifdef OPENHALO2_NET
    uint8_t m[32];
    char room[32];
    int n = msg_head(m, 'W');
    memset(s_lookup_tag, 0, sizeof s_lookup_tag);
    strncpy(s_lookup_tag, (const char *)xo_ptr(xo_arg(1)), 15);
    tag_room(room, sizeof room, s_lookup_tag);
    if (s_lookup_visit > 0) olive_visit_free(s_lookup_visit);
    s_lookup_done = s_lookup_ok = 0;
    s_lookup_visit = olive_visit(room, m, n, OLIVE_ONE, 4000);
    fprintf(stderr, "  [XO] friends: looking for \"%s\"\n", s_lookup_tag);
#endif
    if (xo_arg(4)) xo_wr32(xo_arg(4), xo_task_new_polled(0x3A838A, poll_lookup, 0, NULL));
    return ret(XO_HR_OK, 5);
}

static void invite_xuid(uint64_t x, const uint8_t *kid)
{
    friend_t *f;
    lock();
    f = by_xuid(x);
    if (f && f->rel == REL_FRIEND) {
        memcpy(f->invite_kid, kid, 8);
        f->owe_invite = 1;
        f->invite_out = 1;
        f->invite_rejected = 0;
        f->next = 0;
        fprintf(stderr, "  [XO] friends: inviting \"%s\"\n", f->tag);
    }
    unlock();
}

static int h_friends_game_invite(void)   /* (pad, kid lo, kid hi, count, friends) */
{
    uint8_t kid[8];
    uint32_t lo = xo_arg(1), hi = xo_arg(2), n = xo_arg(3), arr = xo_arg(4), i;
    memcpy(kid, &lo, 4); memcpy(kid + 4, &hi, 4);
    for (i = 0; i < n && i < 16; i++) invite_xuid(friend_arg_xuid(arr + FRIEND_SIZE * i), kid);
    return ret(XO_HR_OK, 5);
}

static int h_game_invite_send(void)   /* (pad, count, xuids (12 bytes each), kid lo, kid hi, 0, message, event, &task) */
{
    uint8_t kid[8];
    uint32_t n = xo_arg(1), arr = xo_arg(2), lo = xo_arg(3), hi = xo_arg(4), i;
    memcpy(kid, &lo, 4); memcpy(kid + 4, &hi, 4);
    for (i = 0; i < n && i < 16; i++) invite_xuid(friend_arg_xuid(arr + 12 * i), kid);
    if (xo_arg(8)) xo_wr32(xo_arg(8), xo_task_new(0x3A8321, 1, XO_TASK_SUCCESS));
    return ret(XO_HR_OK, 9);
}

static void answer_invite(uint64_t x, int yes)
{
    friend_t *f;
    lock();
    f = by_xuid(x);
    if (f && f->invite_in) {
        f->invite_in = yes;          /* accepted: kept until joined (FindFromID uses it) */
        fprintf(stderr, "  [XO] friends: invite from \"%s\" %s\n", f->tag, yes ? "accepted" : "declined");
    }
    msg_drop(x, 3);
    unlock();
#ifdef OPENHALO2_NET
    {
        uint8_t m[32];
        char room[32];
        int n = msg_head(m, 'J'), v;
        m[n++] = (uint8_t)(yes ? 1 : 0);
        inbox_name(room, sizeof room, x);
        v = olive_visit(room, m, n, OLIVE_ONE, 4000);
        (void)v;   /* best effort; freed by the reaper below */
        if (v > 0) xo_friends_reap(v);
    }
#endif
}

static int h_friends_answer_invite(void)   /* (pad, &friend, answer: 1 yes, 0 no, 2 remove (0x8D050)) */
{
    answer_invite(friend_arg_xuid(xo_arg(1)), xo_arg(2) == 1);
    return ret(XO_HR_OK, 3);
}

static int h_game_invite_answer(void)   /* (pad, &answer info, answer, event, &task) */
{
    /* the info starts with the inviter's id (0x8D0B0 builds it) */
    answer_invite(friend_arg_xuid(xo_arg(1)), xo_arg(2) == 1);
    if (xo_arg(4)) xo_wr32(xo_arg(4), xo_task_new(0x3A834E, 1, XO_TASK_SUCCESS));
    return ret(XO_HR_OK, 5);
}

static int h_title_name(void)   /* (title id, language, length, name out) */
{
    static const char n[] = "Halo 2";
    uint32_t len = xo_arg(2), out = xo_arg(3), i;
    if (xo_arg(0) != TITLE_ID || !out || len < 2) return ret(XO_HR_FAIL, 4);
    for (i = 0; i + 1 < len && n[i]; i++) { uint16_t c = (uint8_t)n[i]; memcpy(xo_ptr(out + 2 * i), &c, 2); }
    memset(xo_ptr(out + 2 * i), 0, 2);
    return ret(XO_HR_OK, 4);
}

/* MessageEnumerate(pad, summaries, &count): summaries of 0x40 bytes, as the
 * title converts them (0x8EBD0): sender id +0 (12), type +0xC, time +0x18
 * (compared as a FILETIME, 0x2359CE), message id +0x20, flags +0x24, title
 * id +0x28 (the invite's title, 0x2C788A), sender gamertag +0x30 (16) */
static int h_msg_enumerate(void)
{
    uint32_t out = xo_arg(1), n = 0, title = TITLE_ID;
    int i;
    lock();
    for (i = 0; i < MAX_MSG && n < 125; i++) {
        msg_t *m = &s_msg[i];
        uint8_t *p;
        if (!m->used) continue;
        p = xo_ptr(out + 0x40 * n);
        memset(p, 0, 0x40);
        memcpy(p, &m->xuid, 8);
        p[0xC] = m->type;
        memcpy(p + 0x10, &m->ctx, 8);      /* the message's context (a recruit: the clan) */
        memcpy(p + 0x18, &m->ft, 8);
        memcpy(p + 0x20, &m->id, 4);
        memcpy(p + 0x28, &title, 4);
        memcpy(p + 0x30, m->tag, 15);
        n++;
    }
    unlock();
    if (xo_arg(2)) xo_wr32(xo_arg(2), n);
    return ret(XO_HR_OK, 3);
}

static int h_msg_delete(void)        /* (pad, id, block) */
{
    msg_t *m;
    lock();
    m = msg_by_id(xo_arg(1));
    if (m) m->used = 0;
    unlock();
    return ret(XO_HR_OK, 3);
}

static uint32_t poll_now(int user) { (void)user; return XO_TASK_SUCCESS; }

static int h_msg_details(void)       /* (pad, id, flags (read), 0, 0, &task) */
{
    msg_t *m;
    uint32_t id = xo_arg(1);
    lock();
    m = msg_by_id(id);
    if (m) m->read = 1;
    unlock();
    if (!m) return ret(XO_HR_FAIL, 6);
    if (xo_arg(5)) xo_wr32(xo_arg(5), xo_task_new_polled(0x3AB2A6, poll_now, (int)id, NULL));
    return ret(XO_HR_OK, 6);
}

/* MessageDetailsGetResultsProperty(task, property, size, buffer, &size, 0):
 * the title asks by the ids of 0x8EDA0; an invite answers the session id
 * (0x581, 8 bytes, read by the message screen) and has no text, voice or
 * attachment */
static int h_msg_property(void)
{
    msg_t *m;
    uint32_t prop = xo_arg(1) & 0xFFFF, size = xo_arg(2), buf = xo_arg(3);
    int id = xo_task_user(xo_arg(0));
    uint8_t kid[8];
    lock();
    m = id > 0 ? msg_by_id((uint32_t)id) : NULL;
    if (m) memcpy(kid, m->kid, 8);
    unlock();
    if (!m || prop != 0x581) return ret(XO_HR_FAIL, 6);
    if (size < 8) { if (xo_arg(4)) xo_wr32(xo_arg(4), 8); return ret(0x80155A02u, 6); }
    memcpy(xo_ptr(buf), kid, 8);
    if (xo_arg(4)) xo_wr32(xo_arg(4), 8);
    return ret(XO_HR_OK, 6);
}

/* GetNotificationEx(pad, &info, &state): TRUE while an unread message
 * waits; info is packed: type (1 byte), message id (4), notify flags (4)
 * (0x6D080 reads it) */
static int h_notification(void)
{
    uint32_t info = xo_arg(1), z = 0;
    msg_t *best = NULL;
    int i;
    lock();
    for (i = 0; i < MAX_MSG; i++)
        if (s_msg[i].used && !s_msg[i].read && (!best || s_msg[i].ft > best->ft)) best = &s_msg[i];
    if (best && info) {
        uint8_t *p = xo_ptr(info);
        p[0] = best->type;
        memcpy(p + 1, &best->id, 4);
        memcpy(p + 5, &z, 4);
    }
    unlock();
    if (xo_arg(2)) xo_wr32(xo_arg(2), 0);
    return ret(best ? 1u : 0u, 3);
}

int xo_friends_call(uint32_t va)
{
    switch (va) {
    case 0x3A07A3:   /* FriendsStartup(event, &task): runs while signed in */
        if (xo_arg(1)) xo_wr32(xo_arg(1), xo_task_new_results(va));
        return ret(XO_HR_OK, 2);
    case 0x3A07AE:   /* FriendsEnumerate(pad, event, &task) */
        if (xo_arg(2)) xo_wr32(xo_arg(2), xo_task_new_results(va));
        return ret(XO_HR_OK, 3);
    case 0x3A07B9: return ret(XO_HR_OK, 1);            /* FriendsEnumerateFinish(task) */
    case 0x3A07C4: return h_friends_latest();
    case 0x3A07CF: return h_title_name();
    case 0x3A07DA: return h_friends_remove();
    case 0x3A07E5: return h_friends_game_invite();
    case 0x3A0806: return h_friends_answer();
    case 0x3A0811: return h_friends_answer_invite();
    case 0x3A081C: return ret(XO_HR_OK, 2);            /* FriendsJoinGame: same title, nothing to hand over */
    case 0x3A835D: return h_friends_request();
    case 0x3A838A: return h_friends_request_name();
    case 0x3A8321: return h_game_invite_send();
    case 0x3A834E: return h_game_invite_answer();
    case 0x3A075E:   /* the two title-id predicates: this title or not */
    case 0x3A0769:
        return ret(xo_arg(0) == TITLE_ID, 1);
    /* the mute list: kept empty */
    case 0x3A0841:   /* MutelistStartup(event, &task) */
        if (xo_arg(1)) xo_wr32(xo_arg(1), xo_task_new_results(va));
        return ret(XO_HR_OK, 2);
    case 0x3A0832:   /* MutelistGet(pad, max, event, &task, list, &count) (0x58BD6) */
        if (xo_arg(5)) xo_wr32(xo_arg(5), 0);
        if (xo_arg(3)) xo_wr32(xo_arg(3), xo_task_new(va, 1, XO_TASK_SUCCESS));
        return ret(XO_HR_OK, 6);
    case 0x3A084C: return ret(XO_HR_OK, 4);            /* MutelistAdd */
    case 0x3A086E: return ret(XO_HR_OK, 4);            /* MutelistRemove */
    /* presence of other players (friends and recent players lists) */
    case 0x3A8316:   /* PresenceInit(pad, event, &task) */
        if (xo_arg(2)) xo_wr32(xo_arg(2), xo_task_new_results(va));
        return ret(XO_HR_OK, 3);
    case 0x3A695E: return h_presence_add();
    case 0x3A6F9B: return h_presence_submit();
    case 0x3A6FA6: return h_presence_latest();
    case 0x3A6969: return h_presence_clear();
    /* messages: a friend request or invite carries one; nothing is stored */
    case 0x3A9D8E:   /* MessageCreate(type, n, 0, context lo, hi, flags, expire, &handle) */
        if (xo_arg(7)) xo_wr32(xo_arg(7), 0x0E5B0001u);
        return ret(XO_HR_OK, 8);
    case 0x3A9DC3: return ret(XO_HR_OK, 5);            /* MessageSetProperty */
    case 0x3A9DB8: return ret(XO_HR_OK, 1);            /* MessageDestroy */
    case 0x3AB060:   /* MessageSend(pad, message, n, recipients, event, &task) */
        if (xo_arg(5)) xo_wr32(xo_arg(5), xo_task_new(va, 1, XO_TASK_SUCCESS));
        return ret(XO_HR_OK, 6);
    case 0x3AB04A: return h_msg_enumerate();
    case 0x3AB055: return h_msg_delete();
    case 0x3AB2A6: return h_msg_details();
    case 0x3A9D65: return h_msg_property();
    case 0x3A0798: return h_notification();
    case 0x3A0827: return ret(1, 1);                   /* FriendsGetAcceptedGameInvite: none (S_FALSE) */
    /* clans (teams): there is no clan service. Lookups finish with no
     * clans; the result readers (12 bytes of arguments: task, &count,
     * list) return none, the detail readers (20: task, id, out) fail;
     * every other team call ends in a task that finishes. */
    case 0x3AEBA5: case 0x3AEBB0:
        if (xo_arg(1)) xo_wr32(xo_arg(1), 0);
        return ret(XO_HR_OK, 3);
    case 0x3AFC50: case 0x3AFCA3:
        return ret(XO_HR_FAIL, 5);
    case 0x3AFB80: case 0x3AFB8F: case 0x3AFC26:      /* 6 arguments, the task last */
        if (xo_arg(5)) xo_wr32(xo_arg(5), xo_task_new(va, 1, XO_TASK_SUCCESS));
        return ret(XO_HR_OK, 6);
    case 0x3AFC76: case 0x3A8407:                      /* 7 */
        if (xo_arg(6)) xo_wr32(xo_arg(6), xo_task_new(va, 1, XO_TASK_SUCCESS));
        return ret(XO_HR_OK, 7);
    case 0x3AFBF1: case 0x3A83D4:                      /* 9 */
        if (xo_arg(8)) xo_wr32(xo_arg(8), xo_task_new(va, 1, XO_TASK_SUCCESS));
        return ret(XO_HR_OK, 9);
    case 0x3AFBB9:                                     /* 10 */
        if (xo_arg(9)) xo_wr32(xo_arg(9), xo_task_new(va, 1, XO_TASK_SUCCESS));
        return ret(XO_HR_OK, 10);
    case 0x3A8399:                                     /* 11 */
        if (xo_arg(10)) xo_wr32(xo_arg(10), xo_task_new(va, 1, XO_TASK_SUCCESS));
        return ret(XO_HR_OK, 11);
    default:
        return 0;
    }
}

/* our own presence, from the title */
void xo_friends_set_state(uint32_t flags, const uint8_t *kid, const uint8_t *data, int len)
{
    lock();
    s_state_flags = flags;
    memcpy(s_state_kid, kid, 8);
    if (len > 32) len = 32;
    memcpy(s_state_data, data, (size_t)(len > 0 ? len : 0));
    s_state_len = len > 0 ? len : 0;
    unlock();
}

/* one-off visits (invite answers) freed once they finish */
static int s_reap[32];
void xo_friends_reap(int visit)
{
    int i;
    for (i = 0; i < 32; i++) if (!s_reap[i]) { s_reap[i] = visit; return; }
}

void xo_friends_reap_pump(void)
{
#ifdef OPENHALO2_NET
    int i, cnt;
    for (i = 0; i < 32; i++)
        if (s_reap[i] && olive_visit_poll(s_reap[i], &cnt)) { olive_visit_free(s_reap[i]); s_reap[i] = 0; }
#endif
}
