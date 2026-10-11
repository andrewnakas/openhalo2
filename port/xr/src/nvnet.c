/* The console's network controller (nForce Ethernet, registers at 0xFEF00000).
 *
 * Work in progress (P6, decision 73 in STATUS.md): a register model the
 * title's own XNet driver can bring up, to which an Ethernet frame transport
 * is attached (one relay room = one cable). Written from the driver's
 * observed accesses (RECOMP_NIC_TRACE=1) and public descriptions of the
 * hardware's register and descriptor layout; no code from any GPL driver or
 * emulator.
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
 * length is stored minus one; sending clears bit 31. Receive: bit 31 = free
 * for us; a delivered frame clears it, sets bit 16 and stores the length.
 *
 * Frames go to and come from nic_frame_out / the transport's queue (see
 * nvnet_transport below: a stub that only counts and logs for now).
 * Interrupts are raised on the USB model's thread (g_xbox_device_tick),
 * which is the one runtime thread that can run guest code.
 *
 * RECOMP_NIC=0 leaves the registers as plain memory. RECOMP_NIC_TRACE=1
 * prints the first 600 register accesses; RECOMP_NIC_FRAMES=1 one line per
 * frame sent (first 200).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern void (*g_xbox_device_tick)(void);
int      xbox_DeviceRaiseInterrupt(uint32_t vector);
uint32_t xbox_GetConnectedInterrupt(uint32_t vector);

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
#define DESC_DONE    0x00010000u    /* receive: descriptor holds a frame */

static uint32_t s_reg[NIC_SIZE / 4];
static uint16_t s_phy[32];
static int s_on = -1, s_trace, s_frames;
static unsigned s_traced;
static volatile int s_kick;
static uint32_t s_tx_next, s_rx_next;
static uint8_t  s_tx_frame[2048];
static uint32_t s_tx_have;
#define DESC_LAST 0x00010000u       /* transmit: last piece of the frame */
static unsigned long s_tx_frames, s_tx_bytes, s_irqs, s_irq_declined;

static void nic_tick(void);

static void nic_init(void)
{
    const char *e = getenv("RECOMP_NIC");
    s_on = !(e && *e == '0');
    s_trace = getenv("RECOMP_NIC_TRACE") != NULL;
    s_frames = getenv("RECOMP_NIC_FRAMES") != NULL;
    /* A plain 10/100 PHY: link up, auto-negotiation complete, partner able
     * to do 100 Mbit full duplex. */
    s_phy[0] = 0x3100;
    s_phy[1] = 0x782D;
    s_phy[4] = 0x01E1;
    s_phy[5] = 0x45E1;
    if (s_on)
        g_xbox_device_tick = nic_tick;
}

int recomp_nic_owns(uint32_t va)
{
    if (s_on < 0)
        nic_init();
    return s_on && va >= NIC_BASE && va < NIC_BASE + NIC_SIZE;
}

/* Guest physical memory as the title's drivers address it. */
static uint8_t *phys(uint32_t pa)
{
    return (uint8_t *)((uintptr_t)(0x80000000u | (pa & 0x0FFFFFFFu)) + g_xbox_mem_offset);
}

static uint32_t rd32(uint32_t pa) { uint32_t v; memcpy(&v, phys(pa), 4); return v; }
static void wr32(uint32_t pa, uint32_t v) { memcpy(phys(pa), &v, 4); }

/* ---- Transport (stub) ----------------------------------------------------
 * One function out, one queue in. For now a frame sent is counted and, with
 * RECOMP_NIC_FRAMES=1, described; nothing is received. */
static void nic_frame_out(const uint8_t *f, uint32_t len)
{
    s_tx_frames++;
    s_tx_bytes += len;
    if (s_frames && s_tx_frames <= 200 && len >= 14) {
        unsigned type = (unsigned)f[12] << 8 | f[13];
        fprintf(stderr, "  [NIC] tx %4u B  %02X:%02X:%02X:%02X:%02X:%02X -> "
                        "%02X:%02X:%02X:%02X:%02X:%02X  type %04X",
                len, f[6], f[7], f[8], f[9], f[10], f[11],
                f[0], f[1], f[2], f[3], f[4], f[5], type);
        if (type == 0x0800 && len >= 42 && f[23] == 17)
            fprintf(stderr, "  UDP %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u",
                    f[26], f[27], f[28], f[29], (unsigned)f[34] << 8 | f[35],
                    f[30], f[31], f[32], f[33], (unsigned)f[36] << 8 | f[37]);
        else if (type == 0x0806)
            fprintf(stderr, "  ARP");
        fprintf(stderr, "\n");
    }
}

/* ---- Rings ---------------------------------------------------------------- */

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
        if (len <= 1600 && s_tx_have + len <= sizeof s_tx_frame) {
            /* A buffer address is "physical". Memory from the contiguous
             * allocator lives in the window at 0x80000000 + address; pool
             * and static memory keeps its own address (the kernel layer's
             * MmGetPhysicalAddress passes those through), so the same number
             * can name either. A buffer the driver filled is not all zeroes
             * at its start: take the candidate that is not. */
            static const uint8_t zero[14];
            const uint8_t *win = phys(buf);
            const uint8_t *low = (const uint8_t *)((uintptr_t)buf + g_xbox_mem_offset);
            const uint8_t *f = (buf < 0x04000000u && memcmp(win, zero, len < 14 ? len : 14) == 0)
                               ? low : win;
            if (s_frames && s_tx_frames < 4)
                fprintf(stderr, "  [NIC] tx piece 0x%08X %u B flags %04X: %s\n", buf, len,
                        fl >> 16, f == win ? "contiguous window" : "its own address");
            memcpy(s_tx_frame + s_tx_have, f, len);
            s_tx_have += len;
        }
        /* A frame may be handed over in pieces (the title sends its headers
         * and its payload from different buffers); bit 16 marks the last. */
        if (fl & DESC_LAST) {
            if (s_tx_have >= 14)
                nic_frame_out(s_tx_frame, s_tx_have);
            s_tx_have = 0;
        }
        wr32(at + 4, fl & ~DESC_OWNED);
        s_tx_next++;
        sent++;
    }
    if (sent)
        s_reg[REG_IRQ_STATUS / 4] |= IRQ_TX_OK;
}

/* On the USB model's thread, every few milliseconds. */
static void nic_tick(void)
{
    if (s_kick) {
        s_kick = 0;
        tx_run();
    }
    if ((s_reg[REG_IRQ_STATUS / 4] & s_reg[REG_IRQ_MASK / 4])
            && xbox_GetConnectedInterrupt(NIC_VECTOR)) {
        int claimed = xbox_DeviceRaiseInterrupt(NIC_VECTOR);
        s_irqs++;
        if (claimed <= 0)
            s_irq_declined++;
        if (s_irqs <= 5 || (s_frames && s_irqs % 500 == 0))
            fprintf(stderr, "  [NIC] interrupt %lu -> ISR %s (status %02X mask %02X); "
                            "%lu frames, %lu B sent\n", s_irqs,
                    claimed < 0 ? "not callable" : claimed ? "claimed it" : "declined it",
                    s_reg[REG_IRQ_STATUS / 4] & 0xFFu, s_reg[REG_IRQ_MASK / 4] & 0xFFu,
                    s_tx_frames, s_tx_bytes);
    }
}

/* ---- Registers ------------------------------------------------------------ */

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
        return;
    case REG_RX_RING:
        s_reg[off / 4] = v;
        s_rx_next = 0;
        return;
    case REG_TXRX_CONTROL:
        s_reg[off / 4] = v;
        if (v & TXRX_RESET) {
            s_tx_next = s_rx_next = 0;
            s_kick = 0;
        } else if (v & TXRX_KICK) {
            s_kick = 1;
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
    uint32_t v = reg_read(off & ~3u) >> shift;

    if (size == 1) v &= 0xFFu;
    else if (size == 2) v &= 0xFFFFu;
    trace("read ", off, v, size);
    return v;
}

void recomp_nic_write(uint32_t va, uint32_t val, int size)
{
    uint32_t off = va - NIC_BASE, shift = (off & 3u) * 8;

    trace("write", off, val, size);
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
}
