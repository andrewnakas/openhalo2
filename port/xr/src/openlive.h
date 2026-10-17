/* Open Live (docs/OPEN_LIVE_DESIGN.md): shared declarations of openlive.c
 * (hook, trace, guest helpers) and the host side of the XOnline API. */
#ifndef OPENLIVE_H
#define OPENLIVE_H

#include <stddef.h>
#include <stdint.h>

#define XO_MODE_TRACE 1
#define XO_MODE_LIVE  2

extern int g_xo_mode;

/* the wrapper target (h2lift/xo_hle_pass.py) */
void xo_hook(uint32_t va, void (*lifted)(void));

/* guest helpers, for handlers running on the calling guest thread */
uint8_t *xo_ptr(uint32_t va);
uint32_t xo_rd32(uint32_t va);
void     xo_wr32(uint32_t va, uint32_t v);
uint32_t xo_arg(int i);                       /* i-th stdcall argument */
void     xo_return(uint32_t eax, int nargs);  /* set eax, pop 4 + 4 nargs */
const char *xo_name(uint32_t va);

/* results the title's task pump knows (openlive_api.c has the sources) */
#define XO_HR_OK            0x00000000u
#define XO_HR_FAIL          0x80004005u
#define XO_TASK_RUNNING     0x00000000u
#define XO_TASK_SUCCESS     0x001500F0u

/* tasks (openlive_api.c): a handle the title pumps with XOnlineTaskContinue */
#define XO_MAX_TASKS 128
typedef uint32_t (*xo_task_poll_fn)(int user);   /* 0 = working, else the final result */
uint32_t xo_task_new(uint32_t va, int need, uint32_t done);
uint32_t xo_task_new_results(uint32_t va);      /* runs on, results available */
uint32_t xo_task_new_polled(uint32_t va, xo_task_poll_fn poll, int user, void (*on_close)(int user));
int      xo_task_user(uint32_t handle);           /* -1 when not ours */

/* the signed-in player */
const char *xo_self_gamertag(void);
uint64_t    xo_self_xuid(void);

/* guest allocations (the kernel's heap) and a call into lifted code */
uint32_t xo_alloc(uint32_t size);
void     xo_free(uint32_t va);
uint32_t xo_guest_call(void (*fn)(void), int nargs, const uint32_t *args);

/* openlive_match.c: matchmaking, QoS and the session mesh */
void xo_match_init(void);
void xo_match_pump(void);
int  xo_match_call(uint32_t va);                 /* like xo_live_call */
void xo_match_after(uint32_t va, const uint32_t *args, uint32_t eax);  /* after a lifted call */
uint32_t xo_match_before(uint32_t va);           /* before a lifted call (may edit its arguments) */
void xo_match_unmark_done(uint32_t pkid);
uint32_t xo_arg_addr(int i);                     /* guest address of the i-th argument */
void xo_qos_entry(uint32_t q, uint32_t i, uint8_t flags, uint16_t rtt, uint32_t up, uint32_t down,
                  uint32_t data, uint16_t size);
uint32_t xo_qos_alloc(uint32_t n, uint32_t extra);   /* n entries + data bytes */

/* the record of a session we host, as sent to a joiner ('q'); 0 if not ours */
int  xo_match_record_for_kid(const uint8_t *kid, uint8_t *out, int cap);
/* inbox messages about sessions ('M' member, 'R' roster, 'N' signaling): p
 * after the 25-byte head; returns the reply length */
int  xo_match_inbox(char type, uint64_t from, const uint8_t *p, int len, uint8_t *reply, int cap);

/* openlive_friends.c: friends, presence, requests, invites */
int  xo_friends_call(uint32_t va);
void xo_friends_pump(void);
void xo_friends_reap(int visit);
void xo_friends_reap_pump(void);
void xo_friends_signin(uint64_t xuid, const char *tag);
void xo_friends_set_state(uint32_t flags, const uint8_t *kid, const uint8_t *data, int len);
int  xo_friends_inbox_for_kid(const uint8_t *kid, char *room, size_t n);

/* openlive_api.c */
void xo_live_init(void);
int  xo_live_call(uint32_t va);               /* 1 = answered (returned to the caller) */
void xo_trace_detail(uint32_t va, const uint32_t *args, uint32_t eax);

#endif
