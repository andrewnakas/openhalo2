/* olive: the Open Live facilitator's transport (docs/OPEN_LIVE_DESIGN.md 3.3).
 *
 * Small request/reply messages between players over the unchanged room
 * relay (wss://skatemods.com/rooms): no server of ours. Two primitives:
 *
 *   hold   be a member of a room and answer what visitors send (a player's
 *          inbox "h2u-...", a matchmaking bucket "h2q-..." that every host
 *          advertising in that bucket holds)
 *   visit  join a room, send one message to its oldest member (OLIVE_ONE)
 *          or to every member (OLIVE_ALL), collect the replies until all
 *          have answered or the time is up, leave
 *
 * Only private rooms are used: the relay's public directory (pub- rooms) is
 * shared with other games' room browsers.
 *
 * Messages on the wire: the relay's u32 LE member id, then one type byte and
 * the payload. All functions are thread-safe; handlers run on the
 * transport's threads and must not block.
 */
#ifndef OLIVE_H
#define OLIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLIVE_MSG_MAX   4096
#define OLIVE_REPLIES   16
#define OLIVE_ONE       0
#define OLIVE_ALL       1

/* Answers one message to a held room: writes the reply (type byte first)
 * into reply and returns its length, or 0 for no reply. */
typedef int (*olive_handler)(void *ctx, int from, const uint8_t *msg, int len, uint8_t *reply, int cap);

int  olive_init(const char *relay_base);       /* NULL = wss://skatemods.com/rooms */
int  olive_ready(void);                        /* 1 when built with a transport */

/* Holds room `code`. Returns a handle (> 0) or -1. */
int  olive_hold(const char *code, olive_handler h, void *ctx);
void olive_release(int handle);
/* -1 connecting or failed, else the members in the room besides us, and
 * (through oldest) whether we are its oldest member */
int  olive_hold_members(int handle, int *oldest);

/* Visits room `code` with one message (type byte first); returns an id (> 0)
 * or -1. */
int  olive_visit(const char *code, const uint8_t *msg, int len, int mode, int timeout_ms);
/* 0 still waiting, 1 finished (all answered, or time up, or nobody there).
 * *count = replies so far. Free every id with olive_visit_free. */
int  olive_visit_poll(int id, int *count);
/* The i-th reply: copies it, returns its length (-1 none); rtt in ms. */
int  olive_visit_reply(int id, int i, uint8_t *buf, int cap, int *rtt_ms);
void olive_visit_free(int id);

#ifdef __cplusplus
}
#endif

#endif
