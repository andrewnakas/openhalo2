/* The console's network controller (nForce Ethernet, registers at 0xFEF00000).
 *
 * P6, decisions 73-75 in STATUS.md: a register model the title's own XNet
 * driver brings up, with an Ethernet frame transport attached (one room = one
 * cable). Written from the driver's observed accesses (RECOMP_NIC_TRACE=1)
 * and public descriptions of the hardware's register and descriptor layout;
 * no code from any GPL driver or emulator.
 *
 * Registers modelled:
 *   +0x000 interrupt status (write one to clear)   +0x004 interrupt mask
 *   +0x088 / +0x098 transmitter / receiver status: never busy
 *   +0x100 / +0x104 transmit / receive ring (physical addresses)
 *   +0x108 ring sizes: (receive - 1) << 16 | (transmit - 1)
 *   +0x144 transmit/receive control: bit 0 = "look at the transmit ring";
 *          reads back with the idle bit (0x08) set while that is not
 *          pending, and the reset bit (0x10) clear
 *   +0x180 MII status, +0x190 / +0x194 MII control / data: a PHY at address
 *          1 with the link up at 100 Mbit full duplex
 * Everything else is storage.
 *
 * A ring entry is two words: the buffer's physical address, then length in
 * the low 16 bits and flags in the high 16. Transmit: bit 31 = ours to send,
 * bit 16 = last piece of the frame, length stored minus one; sending clears
 * bit 31. Receive: bit 31 = free for us, length = the buffer's size minus
 * one; a delivered frame clears bit 31, sets bit 16 and stores its length.
 *
 * Where a buffer is: the title gives "physical" addresses, which here can
 * mean the contiguous window or ordinary memory at that address; the kernel
 * layer remembers which the title last asked about for each page
 * (xbox_PhysicalKind).
 *
 * Transport: Windows builds join a hub on the local machine (UDP on
 * 127.0.0.1, ports 34900-34907, one per running copy; every frame goes to
 * the other seven ports). Nothing leaves the machine. Other hosts (the
 * browser) have no transport yet.
 *
 * The model ticks on the USB model's thread (g_xbox_device_tick), the one
 * runtime thread that can run guest code, and raises its interrupt there.
 *
 * RECOMP_NIC=0: registers are plain memory (no network, as before).
 * RECOMP_NIC_HUB=0: no transport (frames are only counted).
 * RECOMP_NIC_TRACE=1: first 600 register accesses.
 * RECOMP_NIC_FRAMES=1: one line per frame sent or received; 2 adds the UDP payload.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

extern ptrdiff_t g_xbox_mem_offset;
extern void (*g_xbox_device_tick)(void);
int      xbox_DeviceRaiseInterrupt(uint32_t vector);
uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
int      xbox_PhysicalKind(uint32_t pa);

#define NIC_BASE   0xFEF00000u
#define NIC_SIZE   0x00000400u
#define NIC_VECTOR 0u               /* what the title's XNet connects (KeConnectInterrupt log) */

#define REG_IRQ_STATUS   0x000
#define REG_IRQ_MASK     0x004
#define REG_TX_STATUS    0x088
#define REG_RX_STATUS    0x098
#define REG_TX_RING      0x100
#define REG_RX_RING      0x104
#define REG_RING_SIZES   0x108
#define REG_TXRX_CONTROL 0x144
#define REG_MII_STATUS   0x180
#define REG_MII_CONTROL  0x190
#define REG_MII_DATA     0x194

#define IRQ_RX       0x02u
#define IRQ_TX_OK    0x10u

#define TXRX_KICK    0x01u
#define TXRX_IDLE    0x08u
#define TXRX_RESET   0x10u

#define MII_INUSE    0x8000u
#define MII_WRITE    0x0400u

#define DESC_OWNED   0x80000000u    /* transmit: valid; receive: available */
#define DESC_LAST    0x00010000u    /* transmit: last piece; receive: holds a frame */

#define FRAME_MAX    1600u

static uint32_t s_reg[NIC_SIZE / 4];
static uint16_t s_phy[32];
static int s_on = -1, s_trace, s_frames;
static unsigned s_traced;
static volatile int s_kick;
static uint32_t s_tx_next, s_rx_next;
static uint8_t  s_tx_frame[2048];
static uint32_t s_tx_have;
static unsigned long s_tx_frames, s_tx_bytes, s_rx_frames, s_rx_bytes, s_rx_dropped;
static unsigned long s_irqs, s_irq_declined, s_unknown_kind;

static void nic_tick(void);
static void hub_open(void);

/* The registers and rings are touched by the title's threads (register
 * accesses, and a transmit at the kick) and by the device thread (receive,
 * interrupts). One short lock covers them; it is never held across a call
 * into the title. */
#if defined(_WIN32)
static volatile LONG s_lock;
static void nic_lock(void)   { while (InterlockedExchange(&s_lock, 1)) YieldProcessor(); }
static void nic_unlock(void) { InterlockedExchange(&s_lock, 0); }
static double now_ms(void)
{
    /* The performance counter is one clock for the whole machine, so two
     * copies' frame logs line up; shown modulo 10000 s to stay readable. */
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart % (f.QuadPart * 10000)) * 1000.0 / (double)f.QuadPart;
}
#else
static volatile int s_lock;
static void nic_lock(void)   { while (__sync_lock_test_and_set(&s_lock, 1)) { } }
static void nic_unlock(void) { __sync_lock_release(&s_lock); }
static double now_ms(void) { return 0.0; }
#endif
static void hub_send(const uint8_t *f, uint32_t len);
static int  hub_recv(uint8_t *f, uint32_t max);

static void nic_init(void)
{
    const char *e = getenv("RECOMP_NIC");
    s_on = !(e && *e == '0');
    s_trace = getenv("RECOMP_NIC_TRACE") != NULL;
    s_frames = getenv("RECOMP_NIC_FRAMES") ? atoi(getenv("RECOMP_NIC_FRAMES")) : 0;
    /* A plain 10/100 PHY: link up, auto-negotiation complete, partner able
     * to do 100 Mbit full duplex. */
    s_phy[0] = 0x3100;
    s_phy[1] = 0x782D;
    s_phy[4] = 0x01E1;
    s_phy[5] = 0x45E1;
    if (s_on) {
        hub_open();
        g_xbox_device_tick = nic_tick;
    }
}

int recomp_nic_owns(uint32_t va)
{
    if (s_on < 0)
        nic_init();
    return s_on && va >= NIC_BASE && va < NIC_BASE + NIC_SIZE;
}

/* ---- Guest memory ---------------------------------------------------------- */

static uint8_t *window(uint32_t pa)
{
    return (uint8_t *)((uintptr_t)(0x80000000u | (pa & 0x0FFFFFFFu)) + g_xbox_mem_offset);
}

static uint32_t rd32(uint32_t pa) { uint32_t v; memcpy(&v, window(pa), 4); return v; }
static void wr32(uint32_t pa, uint32_t v) { memcpy(window(pa), &v, 4); }

/* A packet buffer: by what the title last asked about that page, else (never
 * asked) the contiguous window, which is what a physical address means. */
static uint8_t *buffer(uint32_t pa)
{
    int kind = xbox_PhysicalKind(pa);
    if (kind == 2)
        return (uint8_t *)((uintptr_t)pa + g_xbox_mem_offset);
    if (kind == 0)
        s_unknown_kind++;
    return window(pa);
}

/* ---- Frames ---------------------------------------------------------------- */

static void describe(const char *dir, const uint8_t *f, uint32_t len)
{
    static unsigned said_bcast, said_unicast;
    unsigned type, bcast;
    char line[1024];
    int n;

    if (!s_frames || len < 14)
        return;
    /* Broadcasts repeat for ever (the first 300 are listed); frames to one
     * station are the conversation, and get a longer allowance. The line is
     * built whole, so another thread's output cannot land inside it. */
    bcast = (f[0] & f[1] & f[2] & f[3] & f[4] & f[5]) == 0xFF;
    if (bcast ? said_bcast++ >= 300 : said_unicast++ >= 400)
        return;
    type = (unsigned)f[12] << 8 | f[13];
    n = snprintf(line, sizeof line, "  [NIC] %s %4u B  t=%.1f  %02X:%02X:%02X:%02X:%02X:%02X -> "
                 "%02X:%02X:%02X:%02X:%02X:%02X  type %04X",
                 dir, len, now_ms(), f[6], f[7], f[8], f[9], f[10], f[11],
                 f[0], f[1], f[2], f[3], f[4], f[5], type);
    if (type == 0x0800 && len >= 42 && f[23] == 17) {
        n += snprintf(line + n, sizeof line - (size_t)n, "  UDP %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u",
                      f[26], f[27], f[28], f[29], (unsigned)f[34] << 8 | f[35],
                      f[30], f[31], f[32], f[33], (unsigned)f[36] << 8 | f[37]);
        /* RECOMP_NIC_FRAMES=2: the UDP payload too (first 160 bytes). */
        if (s_frames > 1 && (!bcast || said_bcast < 6)) {
            uint32_t k;
            n += snprintf(line + n, sizeof line - (size_t)n, "  |");
            for (k = 42; k < len && k < 42 + 160; k++)
                n += snprintf(line + n, sizeof line - (size_t)n, " %02X", f[k]);
        }
    } else if (type == 0x0806)
        n += snprintf(line + n, sizeof line - (size_t)n, "  ARP");
    fprintf(stderr, "%s\n", line);
}

static void frame_out(const uint8_t *f, uint32_t len)
{
    s_tx_frames++;
    s_tx_bytes += len;
    describe("tx", f, len);
    hub_send(f, len);
}

/* ---- Rings ----------------------------------------------------------------- */

static void tx_run(void)
{
    uint32_t ring = s_reg[REG_TX_RING / 4];
    uint32_t count = (s_reg[REG_RING_SIZES / 4] & 0xFFFFu) + 1;
    uint32_t n, sent = 0;

    if (!ring)
        return;
    for (n = 0; n < count; n++) {
        uint32_t at = ring + (s_tx_next % count) * 8;
        uint32_t buf = rd32(at), fl = rd32(at + 4);
        uint32_t len = (fl & 0xFFFFu) + 1;

        if (!(fl & DESC_OWNED))
            break;
        if (len <= FRAME_MAX && s_tx_have + len <= sizeof s_tx_frame) {
            memcpy(s_tx_frame + s_tx_have, buffer(buf), len);
            s_tx_have += len;
        }
        /* A frame may be handed over in pieces (the title sends its headers
         * and its payload from different buffers); bit 16 marks the last. */
        if (fl & DESC_LAST) {
            if (s_tx_have >= 14)
                frame_out(s_tx_frame, s_tx_have);
            s_tx_have = 0;
        }
        wr32(at + 4, fl & ~DESC_OWNED);
        s_tx_next++;
        sent++;
    }
    if (sent)
        s_reg[REG_IRQ_STATUS / 4] |= IRQ_TX_OK;
}

/* One received frame into the next free receive entry. 0 if there is none
 * (the frame is dropped, as the hardware would). */
static int rx_deliver(const uint8_t *f, uint32_t len)
{
    uint32_t ring = s_reg[REG_RX_RING / 4];
    uint32_t count = ((s_reg[REG_RING_SIZES / 4] >> 16) & 0xFFFFu) + 1;
    uint32_t at, buf, fl, room;

    if (!ring || !(s_reg[0x094 / 4] & 1u))      /* receiver not started */
        return 0;
    at = ring + (s_rx_next % count) * 8;
    buf = rd32(at);
    fl = rd32(at + 4);
    room = (fl & 0xFFFFu) + 1;
    if (!(fl & DESC_OWNED) || len > room)
        return 0;
    memcpy(buffer(buf), f, len);
    wr32(at + 4, DESC_LAST | (len & 0xFFFFu));
    s_rx_next++;
    s_rx_frames++;
    s_rx_bytes += len;
    describe("rx", f, len);
    s_reg[REG_IRQ_STATUS / 4] |= IRQ_RX;
    return 1;
}

/* On the USB model's thread, every few milliseconds. */
static void nic_tick(void)
{
    static uint8_t in[2048];
    int len, budget = 16;

    uint32_t pending;

    /* How often this really runs (the thread sleeps 4 ms, then does the USB
     * model's work): every frame received waits for the next turn. */
    {
        static double last, sum, worst;
        static unsigned long n;
        double t = now_ms();
        if (last > 0.0) {
            double d = t - last;
            sum += d;
            if (d > worst) worst = d;
            if (++n % 2500 == 0) {
                if (s_frames)
                    fprintf(stderr, "  [NIC] device turn every %.1f ms on average, "
                                    "longest %.1f ms (last 2500)\n", sum / 2500.0, worst);
                sum = worst = 0.0;
            }
        }
        last = t;
    }
    nic_lock();
    if (s_kick) {
        s_kick = 0;
        tx_run();
    }
    while (budget-- > 0 && (len = hub_recv(in, sizeof in)) > 0) {
        if (len < 14 || (uint32_t)len > FRAME_MAX || !rx_deliver(in, (uint32_t)len))
            s_rx_dropped++;
    }
    pending = s_reg[REG_IRQ_STATUS / 4] & s_reg[REG_IRQ_MASK / 4];
    nic_unlock();
    if (pending && xbox_GetConnectedInterrupt(NIC_VECTOR)) {
        int claimed = xbox_DeviceRaiseInterrupt(NIC_VECTOR);
        s_irqs++;
        if (claimed <= 0)
            s_irq_declined++;
        if (s_irqs <= 3 || s_irqs % 2000 == 0)
            fprintf(stderr, "  [NIC] interrupt %lu -> ISR %s; sent %lu frames (%lu B), "
                            "received %lu (%lu B), dropped %lu, declined %lu, "
                            "buffers of unknown kind %lu\n", s_irqs,
                    claimed < 0 ? "not callable" : claimed ? "claimed it" : "declined it",
                    s_tx_frames, s_tx_bytes, s_rx_frames, s_rx_bytes, s_rx_dropped,
                    s_irq_declined, s_unknown_kind);
    }
}

/* ---- Registers ------------------------------------------------------------- */

static uint32_t reg_read(uint32_t off)
{
    uint32_t v = s_reg[off / 4];

    switch (off) {
    case REG_TXRX_CONTROL:
        v &= ~(TXRX_RESET | TXRX_KICK);
        if (!s_kick)
            v |= TXRX_IDLE;
        break;
    case REG_TX_STATUS:
    case REG_RX_STATUS:
        v &= ~1u;
        break;
    case REG_MII_CONTROL:
        v &= ~MII_INUSE;
        break;
    default:
        break;
    }
    return v;
}

static void reg_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case REG_IRQ_STATUS:
    case REG_MII_STATUS:
        s_reg[off / 4] &= ~v;
        return;
    case REG_TX_RING:
        s_reg[off / 4] = v;
        s_tx_next = 0;
        s_tx_have = 0;
        return;
    case REG_RX_RING:
        s_reg[off / 4] = v;
        s_rx_next = 0;
        return;
    case REG_TXRX_CONTROL:
        s_reg[off / 4] = v;
        if (v & TXRX_RESET) {
            s_tx_next = s_rx_next = 0;
            s_tx_have = 0;
            s_kick = 0;
        } else if (v & TXRX_KICK) {
            tx_run();
        }
        return;
    case REG_MII_CONTROL: {
        unsigned phy_addr = (v >> 5) & 31, r = v & 31;
        if (phy_addr == 1) {
            if (v & MII_WRITE)
                s_phy[r] = (uint16_t)s_reg[REG_MII_DATA / 4];
            else
                s_reg[REG_MII_DATA / 4] = s_phy[r];
        } else if (!(v & MII_WRITE)) {
            s_reg[REG_MII_DATA / 4] = 0xFFFF;
        }
        s_reg[off / 4] = v & ~MII_INUSE;
        return;
    }
    default:
        s_reg[off / 4] = v;
        return;
    }
}

static void trace(const char *what, uint32_t off, uint32_t val, int size)
{
    if (!s_trace || s_traced >= 600)
        return;
    s_traced++;
    fprintf(stderr, "  [NIC] %s +0x%03X = 0x%0*X (%d)\n", what, off, size * 2, val, size);
}

uint32_t recomp_nic_read(uint32_t va, int size)
{
    uint32_t off = va - NIC_BASE, shift = (off & 3u) * 8;
    uint32_t v;

    nic_lock();
    v = reg_read(off & ~3u) >> shift;
    nic_unlock();

    if (size == 1) v &= 0xFFu;
    else if (size == 2) v &= 0xFFFFu;
    trace("read ", off, v, size);
    return v;
}

void recomp_nic_write(uint32_t va, uint32_t val, int size)
{
    uint32_t off = va - NIC_BASE, shift = (off & 3u) * 8;

    trace("write", off, val, size);
    nic_lock();
    if (size >= 4) {
        reg_write(off & ~3u, val);
    } else {
        uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        uint32_t old = s_reg[(off & ~3u) / 4];
        if ((off & ~3u) == REG_IRQ_STATUS || (off & ~3u) == REG_MII_STATUS)
            reg_write(off & ~3u, (val << shift) & mask);
        else
            reg_write(off & ~3u, (old & ~mask) | ((val << shift) & mask));
    }
    nic_unlock();
}

/* ---- Transport: a hub on the local machine --------------------------------- */

#define HUB_PORT0 34900
#define HUB_PORTS 8

#if defined(_WIN32)

static SOCKET s_sock = INVALID_SOCKET;
static int s_my_port;

/* Online system link (net/h2net.c): with a room code (RECOMP_NET_ROOM, or
 * `room = ...` in halo2.ini) the frames go peer to peer over WebRTC data
 * channels to everyone in that room, browsers included; the skatemods room
 * relay only introduces the peers. RECOMP_NET_RELAY overrides the relay URL.
 * Without a code: the local hub below, as before. */
#ifdef OPENHALO2_NET
#include "h2net.h"
static int s_online;
#endif

static void hub_open(void)
{
    const char *e = getenv("RECOMP_NIC_HUB");
    WSADATA wsa;
    u_long nb = 1;
    int i;

    if (e && *e == '0')
        return;
#ifdef OPENHALO2_NET
    {
        const char *room = getenv("RECOMP_NET_ROOM");
        if (room && *room) {
            if (h2net_open(room, getenv("RECOMP_NET_RELAY")) == 0) {
                s_online = 1;
                fprintf(stderr, "  [NIC] online system link: room %s\n", room);
            } else {
                fprintf(stderr, "  [NIC] online room %s could not be opened: no transport\n", room);
            }
            return;
        }
    }
#endif
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET)
        return;
    for (i = 0; i < HUB_PORTS; i++) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((u_short)(HUB_PORT0 + i));
        if (bind(s_sock, (struct sockaddr *)&a, sizeof a) == 0) {
            s_my_port = HUB_PORT0 + i;
            break;
        }
    }
    if (!s_my_port) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        fprintf(stderr, "  [NIC] no free hub port (%d-%d): no transport\n",
                HUB_PORT0, HUB_PORT0 + HUB_PORTS - 1);
        return;
    }
    ioctlsocket(s_sock, FIONBIO, &nb);
    {   /* SIO_UDP_CONNRESET off: see hub_recv */
        BOOL report = FALSE;
        DWORD got = 0;
        WSAIoctl(s_sock, _WSAIOW(IOC_VENDOR, 12), &report, sizeof report, NULL, 0, &got, NULL, NULL);
    }
    fprintf(stderr, "  [NIC] local hub: this copy is station %d (UDP 127.0.0.1:%d)\n",
            s_my_port - HUB_PORT0, s_my_port);
}

static void hub_send(const uint8_t *f, uint32_t len)
{
    int i;

#ifdef OPENHALO2_NET
    if (s_online) {
        h2net_send(f, len);
        return;
    }
#endif
    if (s_sock == INVALID_SOCKET)
        return;
    for (i = 0; i < HUB_PORTS; i++) {
        struct sockaddr_in a;
        if (HUB_PORT0 + i == s_my_port)
            continue;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((u_short)(HUB_PORT0 + i));
        sendto(s_sock, (const char *)f, (int)len, 0, (struct sockaddr *)&a, sizeof a);
    }
}

static int hub_recv(uint8_t *f, uint32_t max)
{
    int n;

#ifdef OPENHALO2_NET
    if (s_online)
        return h2net_recv(f, max);
#endif
    if (s_sock == INVALID_SOCKET)
        return 0;
    /* Every frame goes to all eight hub ports, and Windows reports each send
     * to a port nobody has open as a "connection reset" on a later recv. One
     * such report used to end the device thread's receive turn, so a copy
     * that had just sent took six turns (about 30 ms) to reach the frames
     * waiting behind them. The reports are switched off in hub_open; any
     * that still arrive are skipped here. */
    for (;;) {
        n = recv(s_sock, (char *)f, (int)max, 0);
        if (n >= 0 || WSAGetLastError() != WSAECONNRESET)
            break;
    }
    return n > 0 ? n : 0;
}

#elif defined(__EMSCRIPTEN__)

/* In the browser the frames cross to the page through two byte rings in the
 * program's (shared) memory, the way sound and the pads do. A worker the page
 * starts (port/web/net.js) waits on the transmit ring and carries frames to
 * the other players, over a BroadcastChannel between tabs of one browser or a
 * WebSocket to a relay, and writes what arrives into the receive ring.
 *
 * Layout at xbox_web_nic_buffer(): 8 uint32 (tx write, tx read, rx write,
 * rx read, ring size in bytes, frames dropped by the page, 2 spare), then
 * the transmit ring, then the receive ring. A record is a 2-byte length and
 * the frame, at byte positions modulo the ring size; the write and read
 * positions run free (unsigned wrap). One writer and one reader per ring. */
#include <emscripten.h>
#include <emscripten/threading.h>

#define WEB_RING 65536u                 /* a power of two */
static struct {
    volatile uint32_t head[8];
    uint8_t tx[WEB_RING];
    uint8_t rx[WEB_RING];
} s_web;

EMSCRIPTEN_KEEPALIVE uint8_t *xbox_web_nic_buffer(void)
{
    s_web.head[4] = WEB_RING;
    return (uint8_t *)&s_web;
}

static void hub_open(void)
{
    s_web.head[4] = WEB_RING;
}

static void hub_send(const uint8_t *f, uint32_t len)
{
    uint32_t w = __atomic_load_n(&s_web.head[0], __ATOMIC_SEQ_CST);
    uint32_t r = __atomic_load_n(&s_web.head[1], __ATOMIC_SEQ_CST);
    uint32_t i;

    if (len > FRAME_MAX || WEB_RING - (w - r) < len + 2u)
        return;                         /* the page is not draining: drop, as a full wire would */
    s_web.tx[w & (WEB_RING - 1)] = (uint8_t)len;
    s_web.tx[(w + 1) & (WEB_RING - 1)] = (uint8_t)(len >> 8);
    for (i = 0; i < len; i++)
        s_web.tx[(w + 2 + i) & (WEB_RING - 1)] = f[i];
    __atomic_store_n(&s_web.head[0], w + 2 + len, __ATOMIC_SEQ_CST);
    emscripten_futex_wake((void *)&s_web.head[0], 1);
}

static int hub_recv(uint8_t *f, uint32_t max)
{
    uint32_t w = __atomic_load_n(&s_web.head[2], __ATOMIC_SEQ_CST);
    uint32_t r = __atomic_load_n(&s_web.head[3], __ATOMIC_SEQ_CST);
    uint32_t len, i;

    if (w == r)
        return 0;
    len = s_web.rx[r & (WEB_RING - 1)] | (uint32_t)s_web.rx[(r + 1) & (WEB_RING - 1)] << 8;
    if (len <= max)
        for (i = 0; i < len; i++)
            f[i] = s_web.rx[(r + 2 + i) & (WEB_RING - 1)];
    __atomic_store_n(&s_web.head[3], r + 2 + len, __ATOMIC_SEQ_CST);
    return len <= max ? (int)len : 0;
}

#else   /* no transport on this host yet */

static void hub_open(void) {}
static void hub_send(const uint8_t *f, uint32_t len) { (void)f; (void)len; }
static int  hub_recv(uint8_t *f, uint32_t max) { (void)f; (void)max; return 0; }

#endif
