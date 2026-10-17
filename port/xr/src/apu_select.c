/* apu_select.c -- which APU model answers the title.
 *
 * RECOMP_APU_IMPL=old     the previous model (third_party xbox_apu), alone
 *                 new     the clean-room model (src/mcpxaudio), alone
 *                 shadow  the previous model answers and delivers; the new one
 *                         sees the same register writes and guest RAM and
 *                         mixes into its own WAV tap (RECOMP_MA_WAV) for
 *                         tools/apu_ab.py
 * The default is in APU_DEFAULT_IMPL below. Built with OPENHALO2_OLD_APU only
 * while the previous model is still linked; without it only "new" exists.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include "mcpxaudio/mcpx_audio.h"

#ifndef APU_DEFAULT_IMPL
#define APU_DEFAULT_IMPL "old"
#endif

bool ma_hook_handle_mmio(ma_state *s, PCONTEXT ctx, uintptr_t fault_addr,
                         uint32_t xbox_va, int is_write);

#ifdef OPENHALO2_OLD_APU
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
void     mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr, uint32_t xbox_va, int is_write);
#endif

static int s_old_live;          /* the previous model answers */
static ma_state *s_new;         /* the clean-room model, live or shadow */
static ma_state *s_more[4];     /* extra shadows (RECOMP_MA_SHADOWS, RECOMP_MA_CFG<i>) */
ma_state *ma_create_cfg(int mode, const char *cfg, int index);
volatile int recomp_apu_on;

/* RECOMP_APU_CPU=1: every 10 s, the CPU time of each thread whose start
 * routine is in an APU model (the previous model's or ours), by symbol, so
 * the two can be compared ([APUCPU] lines). */
typedef LONG (WINAPI *nqit_fn)(HANDLE, int, PVOID, ULONG, PULONG);
static DWORD WINAPI cpu_reporter(LPVOID arg)
{
    nqit_fn nqit = (nqit_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    (void)arg;
    for (;;) {
        HANDLE snap;
        THREADENTRY32 te;
        Sleep(10000);
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            continue;
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) do {
            HANDLE h;
            PVOID start = NULL;
            FILETIME c, e, k, u;
            char buf[sizeof(SYMBOL_INFO) + 128];
            SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
            DWORD64 disp = 0;
            if (te.th32OwnerProcessID != GetCurrentProcessId())
                continue;
            h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            if (nqit)
                nqit(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL);
            memset(buf, 0, sizeof buf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 127;
            if (start && GetThreadTimes(h, &c, &e, &k, &u)) {
                double secs = ((double)(((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)
                             + (double)(((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime)) / 1e7;
                uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
                int named = SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)start, &disp, si);
                if (secs >= 0.5 || getenv("RECOMP_APU_CPU")[0] == '2')
                    fprintf(stderr, "[APUCPU] thread %lu start %s+0x%llX (%s): %.2f s CPU\n", te.th32ThreadID,
                            (uintptr_t)start > base && (uintptr_t)start < base + 0x10000000 ? "exe" : "other",
                            (unsigned long long)((uintptr_t)start - base), named ? si->Name : "?", secs);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    return 0;
}

/* Called once, where main.c used to call mcpx_apu_init_standalone. */
int recomp_apu_start(uint8_t *ram_ptr)
{
    const char *e = getenv("RECOMP_APU_IMPL");
    const char *impl = (e && *e) ? e : APU_DEFAULT_IMPL;

    if (getenv("RECOMP_APU_CPU"))
        CreateThread(NULL, 0, cpu_reporter, NULL, 0, NULL);

#ifdef OPENHALO2_OLD_APU
    if (!strcmp(impl, "old") || !strcmp(impl, "shadow")) {
        g_apu_state = mcpx_apu_init_standalone(ram_ptr);
        s_old_live = g_apu_state != NULL;
        if (!strcmp(impl, "shadow") && s_old_live) {
            const char *n = getenv("RECOMP_MA_SHADOWS");
            int i, k = n ? atoi(n) : 1;
            s_new = ma_create_cfg(MA_SHADOW, getenv("RECOMP_MA_CFG0"), 0);
            for (i = 1; i < k && i <= 4; i++) {
                char name[32];
                snprintf(name, sizeof name, "RECOMP_MA_CFG%d", i);
                s_more[i - 1] = ma_create_cfg(MA_SHADOW, getenv(name), i);
            }
        }
        recomp_apu_on = s_old_live;
        fprintf(stderr, "[BOOT] APU: previous model%s\n", s_new ? " + clean-room shadow" : "");
        return s_old_live;
    }
#else
    (void)ram_ptr;
    if (strcmp(impl, "new"))
        fprintf(stderr, "[BOOT] APU: '%s' is not built in; using the clean-room model\n", impl);
#endif
    s_new = ma_create(MA_LIVE);
    recomp_apu_on = s_new != NULL;
    fprintf(stderr, "[BOOT] APU: clean-room model %s\n", s_new ? "up" : "FAILED");
    return recomp_apu_on;
}

uint32_t recomp_apu_read(uint32_t off, int size)
{
#ifdef OPENHALO2_OLD_APU
    if (s_old_live)
        return (uint32_t)mcpx_apu_mmio_read(g_apu_state, off, (unsigned)size);
#endif
    return s_new ? ma_read(s_new, off, size) : 0;
}

void recomp_apu_write(uint32_t off, uint32_t val, int size)
{
#ifdef OPENHALO2_OLD_APU
    if (s_old_live) {
        mcpx_apu_mmio_write(g_apu_state, off, val, (unsigned)size);
        if (s_new) {
            int i;
            ma_write(s_new, off, val, size);
            for (i = 0; i < 4; i++)
                if (s_more[i])
                    ma_write(s_more[i], off, val, size);
        }
        return;
    }
#endif
    if (s_new)
        ma_write(s_new, off, val, size);
}

bool recomp_apu_fault(PCONTEXT ctx, uintptr_t fault_addr, uint32_t xbox_va, int is_write)
{
#ifdef OPENHALO2_OLD_APU
    if (s_old_live)
        return apu_hook_handle_mmio(ctx, fault_addr, xbox_va, is_write);
#endif
    return s_new && ma_hook_handle_mmio(s_new, ctx, fault_addr, xbox_va, is_write);
}

void recomp_apu_shutdown(void)
{
    if (s_new && !s_old_live) {
        ma_destroy(s_new);
        s_new = NULL;
    } else if (s_new) {
        ma_destroy(s_new);
        s_new = NULL;
    }
}
