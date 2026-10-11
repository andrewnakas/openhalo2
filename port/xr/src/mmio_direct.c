/* Device registers reached without a page fault.
 *
 * The lifted hardware libraries call these through the checked accessors of
 * recomp/recomp_mmio.h (h2lift/mmio_pass.py) for any address at or above
 * 0xFD000000. The three modelled devices answer for their own registers;
 * everything else up there is ordinary memory in this runtime (the NV2A
 * aperture outside the trapped pages, AC'97, the flash image) and is read
 * and written in place.
 *
 * The page-fault route in main.c stays as it was on the 64-bit build: an
 * access the pass did not reach still works there and is counted, with the
 * faulting sites listed, so "0 faults" is a measured statement.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

extern ptrdiff_t g_xbox_mem_offset;

int      xbox_Nv2aRegTrapOwns(uint32_t xbox_va);
uint32_t xbox_Nv2aRegTrapRead(uint32_t xbox_va, int size);
void     xbox_Nv2aRegTrapWrite(uint32_t xbox_va, uint32_t val, int size);
int      xbox_OhciOwnsAddress(uint32_t xbox_va);
uint32_t xbox_OhciRead(uint32_t xbox_va, int size);
void     xbox_OhciWrite(uint32_t xbox_va, uint32_t val, int size);

/* The network controller model (nvnet.c). */
int      recomp_nic_owns(uint32_t va);
uint32_t recomp_nic_read(uint32_t va, int size);
void     recomp_nic_write(uint32_t va, uint32_t val, int size);

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
void     mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

/* The APU's registers, as the memory layout traps them (APU_TRAP_BYTES in
 * xbox_memory_layout.c): the first 0x30000 only. Above that, to 0xFE880000,
 * are the two DSPs' own memory windows, which are plain memory here; the DSP
 * sentinel (RECOMP_APU_DSP_SENTINEL, a word at 0xFE85A018) lives there, and
 * sending those reads to the register model made the title see 0 and reset
 * its DSPs every frame again. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE830000u

static volatile long s_direct[4];   /* nv2a, ohci, apu, plain */
static volatile long s_faults;
#define MAX_SITES 64
static struct { uintptr_t ip; uint32_t va; long n; } s_site[MAX_SITES];
static int s_sites;

void recomp_mmio_report(void);

/* A test run ends at a watchdog, which runs no exit handlers: with
 * RECOMP_MMIO_REPORT set the totals are printed about every 10 s. */
static void tick(void)
{
    static int on = -1;
    static volatile long calls;
    static clock_t last;

    if (on < 0)
        on = getenv("RECOMP_MMIO_REPORT") != NULL;
    if (!on || (++calls & 0x3FF))
        return;
    if (clock() - last >= 10 * CLOCKS_PER_SEC) {
        last = clock();
        recomp_mmio_report();
    }
}

/* The network controller (nForce, registers at 0xFEF00000) has no model yet:
 * its registers are plain zeroed memory. RECOMP_NIC_TRACE=1 lists the first
 * accesses the title's XNet driver makes, which is the specification for one. */
static void nic_trace(uint32_t va, uint32_t val, int size, int write)
{
    static int on = -1;
    static unsigned n;

    if (on < 0)
        on = getenv("RECOMP_NIC_TRACE") != NULL;
    if (!on || va < 0xFEF00000u || va >= 0xFEF00400u || n >= 400)
        return;
    n++;
    fprintf(stderr, "  [NIC] %s +0x%03X = 0x%0*X (%d)\n",
            write ? "write" : "read ", va - 0xFEF00000u, size * 2, val, size);
}

uint32_t recomp_mmio_read(uint32_t va, int size)
{
    tick();
    if (recomp_nic_owns(va))
        return recomp_nic_read(va, size);
    if (xbox_Nv2aRegTrapOwns(va)) {
        s_direct[0]++;
        return xbox_Nv2aRegTrapRead(va, size);
    }
    if (xbox_OhciOwnsAddress(va)) {
        s_direct[1]++;
        return xbox_OhciRead(va, size);
    }
    if (g_apu_state && va >= APU_TRAP_BASE && va < APU_TRAP_END) {
        s_direct[2]++;
        return (uint32_t)mcpx_apu_mmio_read(g_apu_state, va - APU_TRAP_BASE, (unsigned)size);
    }
    s_direct[3]++;
    {
        const volatile uint8_t *p = (const volatile uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
        uint32_t v = size == 1 ? *p : size == 2 ? *(const volatile uint16_t *)p
                                               : *(const volatile uint32_t *)p;
        nic_trace(va, v, size, 0);
        return v;
    }
}

void recomp_mmio_write(uint32_t va, uint32_t val, int size)
{
    if (recomp_nic_owns(va)) {
        recomp_nic_write(va, val, size);
        return;
    }
    if (xbox_Nv2aRegTrapOwns(va)) {
        s_direct[0]++;
        xbox_Nv2aRegTrapWrite(va, val, size);
        return;
    }
    if (xbox_OhciOwnsAddress(va)) {
        s_direct[1]++;
        xbox_OhciWrite(va, val, size);
        return;
    }
    if (g_apu_state && va >= APU_TRAP_BASE && va < APU_TRAP_END) {
        s_direct[2]++;
        mcpx_apu_mmio_write(g_apu_state, va - APU_TRAP_BASE, val, (unsigned)size);
        return;
    }
    s_direct[3]++;
    nic_trace(va, val, size, 1);
    {
        volatile uint8_t *p = (volatile uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
        if (size == 1) *p = (uint8_t)val;
        else if (size == 2) *(volatile uint16_t *)p = (uint16_t)val;
        else *(volatile uint32_t *)p = val;

        /* AC'97 bus-master control bytes (0xFEC0010B, +0x10 per channel up to
         * 0x17B): bit 1 is "reset registers", which the controller clears by
         * itself and DirectSound reads back exactly once, a few instructions
         * after setting it (see ac97_clear_reset_bits in
         * xbox_memory_layout.c, which does this from a page trap on 64-bit
         * Windows). The write must never leave the bit set. */
        if (va + (uint32_t)size > 0xFEC00100u && va < 0xFEC00180u) {
            int k;
            for (k = 0; k < size; k++)
                if (((va + (uint32_t)k) & 0xFu) == 0xBu && va + (uint32_t)k >= 0xFEC0010Bu)
                    p[k] = (uint8_t)(p[k] & ~0x02u);
        }
    }
}

/* A register access that still arrived as a page fault (64-bit build). */
void recomp_mmio_note_fault(uintptr_t ip, uint32_t va)
{
    int i;

    s_faults++;
    for (i = 0; i < s_sites; i++)
        if (s_site[i].ip == ip) { s_site[i].n++; return; }
    if (s_sites < MAX_SITES) {
        s_site[s_sites].ip = ip;
        s_site[s_sites].va = va;
        s_site[s_sites].n = 1;
        s_sites++;
        fprintf(stderr, "  [MMIO] page-fault site %d: ip=%p va=0x%08X\n",
                s_sites, (void *)ip, va);
    }
}

void recomp_mmio_report(void)
{
    int i;

    fprintf(stderr, "  [MMIO] direct: nv2a %ld, usb %ld, apu %ld, plain %ld; "
                    "by page fault: %ld from %d sites%s\n",
            s_direct[0], s_direct[1], s_direct[2], s_direct[3],
            s_faults, s_sites, s_sites == MAX_SITES ? " (or more)" : "");
    for (i = 0; i < s_sites; i++)
        fprintf(stderr, "  [MMIO]   site ip=%p first va=0x%08X count=%ld\n",
                (void *)s_site[i].ip, s_site[i].va, s_site[i].n);
    fflush(stderr);
}
