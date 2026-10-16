# h2net: peer-to-peer system link transport

Carries the nForce NIC model's Ethernet frames between players with WebRTC
data channels, so native and browser builds share one mesh and no server
carries game traffic. The existing room relay (`wss://skatemods.com/rooms/room/<code>`)
is used only for signaling (a few KB per peer pair at join time).

## API (`h2net.h`)

```c
int  h2net_open(const char *room_code, const char *relay_url); /* 0 = started; relay NULL = default */
void h2net_send(const uint8_t *f, uint32_t len);               /* one Ethernet frame */
int  h2net_recv(uint8_t *f, uint32_t max);                     /* frame length, 0 = none */
void h2net_status(char *buf, size_t n);                        /* one line, for logs/UI */
int  h2net_peers(void);                                        /* open data channels */
void h2net_close(void);
```

All calls are non-blocking and thread-safe (send/recv from the emulator's
device thread). Received frames wait in a bounded queue (1024 frames; newer
frames are dropped when full, as a busy wire would). The three hub functions
in nvnet.c map onto this one to one: `hub_open` -> `h2net_open(room, NULL)`,
`hub_send` -> `h2net_send`, `hub_recv` -> `h2net_recv`.

Browser: `h2net.js` is an ES module with the same semantics:
`new H2Net().open(room)`, `send(Uint8Array)`, `recv()` (Uint8Array or null),
optional `onframe` push, `peers()`, `status()`, `await stats()`, `close()`.
RTCPeerConnection does not exist in workers, so it runs on the page; the
web build's net.js worker would hand frames over by postMessage (or the page
drains `recv()` into the rx ring).

## How it works

- Relay protocol (unchanged relay): welcome/join/leave JSON notices; binary
  `u32 LE member id + payload`. Payload = one type byte + UTF-8:
  `o` offer SDP, `a` answer SDP, `c` `"<mid>\n<candidate>"` (trickle ICE).
- The lower member id offers: existing members offer to each newcomer.
- One data channel per pair, label `h2`, unordered, maxRetransmits 0.
- Learning switch: the source MAC of every frame received from a peer is
  remembered; unicast goes to that peer only, unknown/broadcast/multicast
  destinations are flooded to all open peers.
- ICE: public STUN only (`stun.l.google.com:19302`, `stun.cloudflare.com:3478`).
- Logging: `H2NET_LOG=<0-6>` (libdatachannel levels, 0/unset = off) to stderr.

## Build (Windows, MSVC, /MT)

1. libdatachannel from a vcpkg clone on D: (the copy bundled with VS Build
   Tools has no ports tree):
   ```
   git clone https://github.com/microsoft/vcpkg D:\n64work\vcpkg
   D:\n64work\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   D:\n64work\vcpkg\vcpkg install libdatachannel:x64-windows-static
   ```
   (about 13 minutes, mostly OpenSSL; binary cache kept in D:\n64work\vcpkg-cache
   when VCPKG_DEFAULT_BINARY_CACHE points there.)
2. `net\build_win.cmd` -> `D:\n64work\halo2\build\net\h2net.lib` + `h2net_test.exe`.

Linking into another C project built with /MT (e.g. port/xr): compile h2net.c
with `/DRTC_STATIC /ID:\n64work\vcpkg\installed\x64-windows-static\include`
(or link the `h2net` CMake target), then link

```
h2net.lib datachannel.lib juice.lib usrsctp.lib libssl.lib libcrypto.lib
ws2_32.lib iphlpapi.lib crypt32.lib bcrypt.lib user32.lib advapi32.lib
```

from `D:\n64work\vcpkg\installed\x64-windows-static\lib`. libdatachannel is
C++ (static CRT, /MT); the MSVC C++ runtime comes in through the libraries'
default-lib records, so a C-only project needs no extra flags. Adds about
6 MB to the executable.

## Tests

```
sh   net/test/run_mesh.sh 2 1000      # (a) two native stations
sh   net/test/run_mesh.sh 4 1000      # (b) four-station mesh
node net/test/run_browser.mjs 1000    # (c) native <-> headless Edge (page offers)
node net/test/run_browser.mjs 1000 --native-first   # native offers
node net/test/run_browser.mjs 1000 --mdns           # Chrome's default mDNS host hiding
```

Each station waits for its peers, learns MACs from broadcast HELLOs, then
sends per round one unicast PING to every other station (echoed as PONG) and
one broadcast, 2 ms apart, every 10th frame 1514 bytes, the rest 300.
Results 2026-10-16, all processes on one Windows PC, live relay:

| test | delivery | RTT p50 / p95 | pair | misdelivered / unicast flooded |
|---|---|---|---|---|
| (a) 2 native, 1000 frames each way | 1000/1000 ping, bcast, pong both ways | 4.1 / 4.6 ms | host-host | 0 / 0 |
| (b) 4 native mesh, 3000 per station | 1000/1000 from every peer, all 12 directions | 2.2 / 4.3 ms | host-host | 0 / 0 |
| (c) native <-> headless Edge | 1000/1000 both ways (both offer directions, mDNS on and off) | 4.1-4.7 / 5.8-6.6 ms | host-host | 0 / 0 |

Mesh up (relay join to all channels open): 0.2-2.3 s. The RTTs include the
test loop's 1 ms sleep between polls on both sides.

## Known limits

- No TURN: peers behind symmetric NAT / strict firewalls on both ends cannot
  connect (status shows the peer stuck at `connecting`/`failed`). All tests
  ran on one machine (host candidates); the srflx path across two real NATs
  is untested here.
- 10 members per room (relay limit); a full mesh of 10 is 45 channels.
- If the relay connection drops, existing channels keep working but new
  members cannot join (no reconnect in v1). The relay's `host-left` notice is
  treated as that member leaving; the mesh carries on.
- Native signaling TLS is not certificate-verified: libdatachannel does not
  verify against the Windows root store. Data channels are still DTLS
  encrypted, but someone able to intercept the relay connection could
  impersonate peers.
- Unreliable transport by design (like Ethernet): frames lost on the internet
  are not retransmitted; the game's own protocol handles loss.

## Licences of linked code

| component | version | licence |
|---|---|---|
| libdatachannel | 0.24.6 | MPL-2.0 |
| libjuice | 1.7.4 | MPL-2.0 |
| usrsctp | 0.9.5.0 | BSD-3-Clause |
| OpenSSL | 3.6.5 | Apache-2.0 |
| plog (header-only, inside libdatachannel) | 1.1.11 | MIT |
| nlohmann-json (header-only, build dependency) | 3.12.0 | MIT |

MPL-2.0 is file-level copyleft: shipping a statically linked binary is fine
as long as the libdatachannel/libjuice source (unmodified, from vcpkg) is
available and their licence notices ship with the binary.
