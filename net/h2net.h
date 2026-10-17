/* h2net: peer-to-peer Ethernet frame transport for Open Halo 2 system link.
 *
 * One room = one cable. Every station in a room keeps a WebRTC data channel
 * (unordered, no retransmits) to every other station; the existing room relay
 * (wss://skatemods.com/rooms/room/<code>) only carries the WebRTC signaling.
 * Frames are forwarded like a learning switch: unicast to the station whose
 * source MAC was seen on that peer's channel, flooded otherwise (and always
 * for broadcast/multicast destinations).
 *
 * All functions are non-blocking and may be called from any thread.
 */
#ifndef H2NET_H
#define H2NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2NET_DEFAULT_RELAY "wss://skatemods.com/rooms"
#define H2NET_FRAME_MAX     2048u   /* larger frames are dropped */
#define H2NET_RX_QUEUE      1024u   /* received frames held; newer ones dropped when full */
#define H2NET_MAX_ROOMS     6       /* rooms one station can be in at once */

/* Joins room `room_code` (4-24 of [A-Za-z0-9_-]) on `relay_url` (the relay's
 * base, e.g. H2NET_DEFAULT_RELAY; NULL = that default; "/room/<code>" is
 * appended). Returns 0 when the connection was started, -1 on bad arguments
 * or if already open. Peers connect in the background: see h2net_status. */
int  h2net_open(const char *room_code, const char *relay_url);

/* Several rooms at once (Open Live: a party's room and the room of the game
 * being joined). All rooms are one segment: frames are switched and flooded
 * across every peer of every room.
 *   h2net_init   starts the transport with no room (relay as for h2net_open)
 *   h2net_join   joins a room (0..H2NET_MAX_ROOMS-1 = its slot, -1 = failed);
 *                joining a room already joined returns its slot
 *   h2net_leave  leaves it; h2net_find gives its slot or -1
 *   h2net_room_peers  open channels in that room (-1 if not joined) */
int  h2net_init(const char *relay_url);
int  h2net_join(const char *room_code);
void h2net_leave(const char *room_code);
int  h2net_find(const char *room_code);
int  h2net_room_peers(const char *room_code);

/* External rooms: no relay; the caller carries the signaling. fn sends one
 * signaling message (type byte + text) to member `target` of the room. */
typedef int (*h2net_signal_fn)(void *ctx, int target, const uint8_t *msg, int len);
int  h2net_join_ext(const char *room_code, h2net_signal_fn fn, void *ctx);
/* a signaling message from member `from` (> 0) of an external room */
void h2net_ext_signal(const char *room_code, int from, const uint8_t *msg, int len);
/* offers a connection to member `id` of an external room */
int  h2net_ext_connect(const char *room_code, int id);
int  h2net_ext_has(const char *room_code, int id);
/* the relay turned the room away (it holds 10 members) */
int  h2net_room_full(const char *room_code);

/* Queues one Ethernet frame for the other stations (dropped silently if no
 * peer is connected or the frame is malformed). */
void h2net_send(const uint8_t *f, uint32_t len);

/* Copies the oldest received frame into f and returns its length; 0 when
 * nothing is waiting. A frame longer than max is discarded (returns 0). */
int  h2net_recv(uint8_t *f, uint32_t max);

/* One-line human-readable state, e.g.
 * "relay=open self=2 host=1 peers=1/1 [1:open host/udp rtt=0.4ms] tx=10 rx=9 drop=0". */
void h2net_status(char *buf, size_t n);

/* Number of peers whose data channel is open (extra, for tests and UIs). */
int  h2net_peers(void);

/* Leaves the room and frees everything. h2net_open may be called again. */
void h2net_close(void);

#ifdef __cplusplus
}
#endif

#endif
