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
#include <string.h>

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

/* The APU model (apu_select.c). */
extern volatile int recomp_apu_on;
uint32_t recomp_apu_read(uint32_t off, int size);
void     recomp_apu_write(uint32_t off, uint32_t val, int size);

/* The APU's registers, as the memory layout traps them (APU_TRAP_BYTES in
 * xbox_memory_layout.c): the first 0x30000 only. Above that, to 0xFE880000,
 * are the two DSPs' own memory windows, which are plain memory here; the DSP
 * sentinel (RECOMP_APU_DSP_SENTINEL, a word at 0xFE85A018) lives there, and
 * sending those reads to the register model made the title see 0 and reset
 * its DSPs every frame again. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE830000u

/* RECOMP_APU_TRACE=<file>[,MB]: every access to 0xFE800000-0xFE87FFFF (the
 * APU registers and the two DSP windows) as 16-byte records
 * {u64 microseconds since the first record, u32 offset | size << 24 |
 * write << 31, u32 value}; a read records the value returned. Capped at MB
 * megabytes (default 512). Black-box measurement for the clean-room APU
 * (docs/APU_CLEANROOM_DESIGN.md 3c.1); tools/apu_trace.py reads it. */
#include <windows.h>
static FILE *s_tr;
static long long s_tr_left = -1;
static LARGE_INTEGER s_tr_t0, s_tr_f;
static CRITICAL_SECTION s_tr_cs;

/* RECOMP_APU_SNAP=<dir>[,period ms[,lo,hi]]: guest physical RAM lo..hi (hex,
 * default 0..0x4000000) written to <dir>/snapNNNN.bin every period ms
 * (default 10000; 0 = only on demand), and on demand when <dir>/snapnow
 * exists (deleted once taken). RECOMP_APU_SNAP_ON=<off hex>[,count] also
 * takes one right after each of the first count (default 8) writes to that
 * register offset. Each snapshot is marked in the trace by a record with
 * offset field 0x7FFFFFFF and the snapshot number as its value. */
/* physical P is guest 0x80000000 + P (the contiguous window) */
static char s_snap_dir[400];
static unsigned s_snap_period = 10000, s_snap_lo = 0, s_snap_hi = 0x4000000;
static unsigned s_snap_n, s_snap_on = 0xFFFFFFFFu, s_snap_on_left = 8;

static void snap_take_locked(uint64_t us)
{
    char p[512];
    FILE *f;
    struct { uint64_t us; uint32_t ow; uint32_t v; } r;

    snprintf(p, sizeof p, "%s/snap%04u.bin", s_snap_dir, s_snap_n);
    f = fopen(p, "wb");
    if (f) {
        fwrite((uint8_t *)((uintptr_t)0x80000000u + g_xbox_mem_offset) + s_snap_lo, 1, s_snap_hi - s_snap_lo, f);
        fclose(f);
    }
    r.us = us; r.ow = 0x7FFFFFFFu; r.v = s_snap_n++;
    if (s_tr && s_tr_left > 0) { fwrite(&r, sizeof r, 1, s_tr); s_tr_left -= sizeof r; fflush(s_tr); }
}

static uint64_t tr_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart - s_tr_t0.QuadPart) * 1000000 / s_tr_f.QuadPart);
}

static DWORD WINAPI snap_thread(LPVOID arg)
{
    char trig[512];
    DWORD waited = 0;
    (void)arg;
    snprintf(trig, sizeof trig, "%s/snapnow", s_snap_dir);
    for (;;) {
        Sleep(100); waited += 100;
        if (GetFileAttributesA(trig) != INVALID_FILE_ATTRIBUTES
                || (s_snap_period && waited >= s_snap_period)) {
            waited = 0;
            EnterCriticalSection(&s_tr_cs);
            snap_take_locked(tr_now());
            LeaveCriticalSection(&s_tr_cs);
            DeleteFileA(trig);
        }
    }
    return 0;
}

static void snap_init(void)
{
    const char *e = getenv("RECOMP_APU_SNAP"), *c;
    size_t n;
    if (!e || !*e) return;
    c = strchr(e, ',');
    n = c ? (size_t)(c - e) : strlen(e);
    if (n >= sizeof s_snap_dir) n = sizeof s_snap_dir - 1;
    memcpy(s_snap_dir, e, n); s_snap_dir[n] = 0;
    if (c) {
        s_snap_period = (unsigned)atoi(c + 1);
        c = strchr(c + 1, ',');
        if (c) {
            s_snap_lo = (unsigned)strtoul(c + 1, NULL, 16);
            c = strchr(c + 1, ',');
            if (c) s_snap_hi = (unsigned)strtoul(c + 1, NULL, 16);
        }
    }
    e = getenv("RECOMP_APU_SNAP_ON");
    if (e && *e) {
        s_snap_on = (unsigned)strtoul(e, NULL, 16);
        c = strchr(e, ',');
        if (c) s_snap_on_left = (unsigned)atoi(c + 1);
    }
    CreateThread(NULL, 0, snap_thread, NULL, 0, NULL);
    fprintf(stderr, "[APUTRACE] snapshots to %s every %u ms, 0x%X..0x%X\n",
            s_snap_dir, s_snap_period, s_snap_lo, s_snap_hi);
}

static void apu_trace(uint32_t va, uint32_t val, int size, int write)
{
    struct { uint64_t us; uint32_t ow; uint32_t v; } r;
    LARGE_INTEGER t;

    if (s_tr_left == 0 || va < APU_TRAP_BASE || va >= 0xFE880000u)
        return;
    if (s_tr_left < 0) {
        static volatile long once;
        const char *e = getenv("RECOMP_APU_TRACE");
        if (InterlockedExchange(&once, 1)) { while (s_tr_left < 0) Sleep(0); return; }
        if (e && *e) {
            char path[512];
            const char *c = strchr(e, ',');
            size_t n = c ? (size_t)(c - e) : strlen(e);
            if (n >= sizeof path) n = sizeof path - 1;
            memcpy(path, e, n); path[n] = 0;
            InitializeCriticalSection(&s_tr_cs);
            QueryPerformanceFrequency(&s_tr_f);
            QueryPerformanceCounter(&s_tr_t0);
            s_tr = fopen(path, "wb");
            if (s_tr) setvbuf(s_tr, NULL, _IOFBF, 1 << 20);
            s_tr_left = s_tr ? (long long)(c ? atoi(c + 1) : 512) << 20 : 0;
            fprintf(stderr, "[APUTRACE] %s: %s\n", path, s_tr ? "recording" : "cannot open");
            if (s_tr) snap_init();
        } else {
            s_tr_left = 0;
        }
        if (!s_tr_left) return;
    }
    QueryPerformanceCounter(&t);
    r.us = (uint64_t)((t.QuadPart - s_tr_t0.QuadPart) * 1000000 / s_tr_f.QuadPart);
    r.ow = (va - APU_TRAP_BASE) | ((uint32_t)size << 24) | (write ? 0x80000000u : 0);
    r.v = val;
    EnterCriticalSection(&s_tr_cs);
    if (s_tr_left > 0) {
        fwrite(&r, sizeof r, 1, s_tr);
        s_tr_left -= sizeof r;
        if (s_tr_left <= 0) { s_tr_left = 0; fclose(s_tr); s_tr = NULL; }
        else {
            static uint64_t last_flush;
            if (r.us - last_flush > 500000) { last_flush = r.us; fflush(s_tr); }
        }
        if (write && s_snap_on_left && s_snap_dir[0] && va - APU_TRAP_BASE == s_snap_on) {
            s_snap_on_left--;
            snap_take_locked(r.us);
        }
    }
    LeaveCriticalSection(&s_tr_cs);
}

/* RECOMP_APU_TRACE_DESC=1 (with the trace): around every voice-method write
 * (0x20100-0x203FF) the selected voice's 0x80-byte descriptor is recorded
 * twice, before (offset field 0x7FFFFFFE) and after (0x7FFFFFFD) the model
 * took the write: a header record whose value is the voice number, then the
 * 128 bytes. The voice is the last value written to 0x202F8 and the
 * descriptor array base the last value written to 0x202C (both measured,
 * docs/APU_FACTS.md). */
static int s_trd = -1;
static uint32_t s_trd_voice, s_trd_vpv;

static void apu_trace_desc(uint32_t va, uint32_t val, int write, uint32_t tag)
{
    struct { uint64_t us; uint32_t ow; uint32_t v; } r;
    uint32_t o = va - APU_TRAP_BASE;

    if (s_trd < 0) { const char *e = getenv("RECOMP_APU_TRACE_DESC"); s_trd = e && *e == '1'; }
    if (!s_trd || !s_tr || !write) return;
    if (tag == 0x7FFFFFFEu) {
        if (o == 0x202C) s_trd_vpv = val;
        if (o == 0x202F8) s_trd_voice = val & 0xFFFF;
    }
    if (o < 0x20100 || o >= 0x20400 || !s_trd_vpv || s_trd_voice > 0xFF) return;
    r.us = tr_now(); r.ow = tag; r.v = s_trd_voice;
    EnterCriticalSection(&s_tr_cs);
    if (s_tr && s_tr_left > 0x100) {
        fwrite(&r, sizeof r, 1, s_tr);
        fwrite((uint8_t *)((uintptr_t)0x80000000u + g_xbox_mem_offset) + s_trd_vpv + s_trd_voice * 0x80, 1, 0x80, s_tr);
        s_tr_left -= sizeof r + 0x80;
    }
    LeaveCriticalSection(&s_tr_cs);
}

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
    if (recomp_apu_on && va >= APU_TRAP_BASE && va < APU_TRAP_END) {
        uint32_t v;
        s_direct[2]++;
        v = recomp_apu_read(va - APU_TRAP_BASE, size);
        apu_trace(va, v, size, 0);
        return v;
    }
    s_direct[3]++;
    {
        const volatile uint8_t *p = (const volatile uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
        uint32_t v = size == 1 ? *p : size == 2 ? *(const volatile uint16_t *)p
                                               : *(const volatile uint32_t *)p;
        nic_trace(va, v, size, 0);
        apu_trace(va, v, size, 0);
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
    if (recomp_apu_on && va >= APU_TRAP_BASE && va < APU_TRAP_END) {
        s_direct[2]++;
        apu_trace(va, val, size, 1);
        apu_trace_desc(va, val, 1, 0x7FFFFFFEu);
        recomp_apu_write(va - APU_TRAP_BASE, val, size);
        apu_trace_desc(va, val, 1, 0x7FFFFFFDu);
        return;
    }
    s_direct[3]++;
    nic_trace(va, val, size, 1);
    apu_trace(va, val, size, 1);
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
