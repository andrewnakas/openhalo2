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

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
void     mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

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

uint32_t recomp_mmio_read(uint32_t va, int size)
{
    tick();
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
        if (size == 1) return *p;
        if (size == 2) return *(const volatile uint16_t *)p;
        return *(const volatile uint32_t *)p;
    }
}

void recomp_mmio_write(uint32_t va, uint32_t val, int size)
{
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
    {
        volatile uint8_t *p = (volatile uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
        if (size == 1) *p = (uint8_t)val;
        else if (size == 2) *(volatile uint16_t *)p = (uint16_t)val;
        else *(volatile uint32_t *)p = val;
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
