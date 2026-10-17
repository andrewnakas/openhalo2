/* ma_thread.c -- the frame clock of the clean-room APU model: 8 frames of 32
 * samples make one 256-sample block, paced to 187.5 blocks per second; after
 * each block the GP doorbell and the DSP sentinel are serviced and pending
 * interrupts are delivered to the title's ISR on this thread (the pattern of
 * usb/ohci.c: own TIB, a worker stack per call, the dispatch lock). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#else
/* the web build (and any POSIX host): the toolkit's Win32 shim */
#include "platform/win32_compat.h"
#define timeEndPeriod(ms) ((void)(ms))
#define TIMER_ALL_ACCESS 0
#define CreateWaitableTimerExW(a, n, f, acc) ((HANDLE)NULL)
#endif
#include "ma_internal.h"
#include "apu_xaudio2.h"
#if defined(__EMSCRIPTEN__)
#define MA_DEVICE_NAME "page ring"   /* audio.js plays it */
#else
#define MA_DEVICE_NAME "XAudio2"
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint32_t  xbox_AllocThreadTib(void);
extern int       xbox_DeviceRaiseInterrupt(uint32_t vector);
extern uint32_t  xbox_GetConnectedInterrupt(uint32_t vector);
extern int       xbox_IrqlBlocksInterrupts(void);
extern void      xbox_TimerThreadWake(void);
extern __declspec(thread) uint32_t g_fs_base;

void ma_tap_close(ma_state *s);

#define MA_VECTOR 5     /* HalGetInterruptVector(5) at 0x37F302 */

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static int vp_running(ma_state *s)
{
    uint32_t c = s->reg[R_SE_CONTROL / 4];
    return (c & 7) == 7 && (c & 0x18) != 0;
}

/* GP doorbell: DSOUND posts a command at GP scratch +0x810 and spins until it
 * reads 0 (0x37E38E). Scratch page 0 is the first entry of the page table at
 * RAM[reg 0x2040]. */
static void service_dsp(ma_state *s)
{
    uint32_t tab = s->reg[R_GPS_TABLE / 4];
    if (s->mode != MA_LIVE)
        return;
    if (tab && s->gps_ack) {
        uint32_t page = ma_rd32(s, tab) & ~0xFFFu;
        if (page) {
            uint32_t pa = page + s->gps_ack;
            if (ma_rd32(s, pa)) {
                ma_wr32(s, pa, 0);
                s->st_doorbell++;
            }
        }
    }
    if (s->sentinel_ptr) {
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t va;
        memcpy(&va, mem + s->sentinel_ptr, 4);
        if (va >= 0xFE830000u && va < 0xFE880000u) {
            uint32_t cur;
            memcpy(&cur, mem + va, 4);
            if (cur != s->sentinel_val)
                memcpy(mem + va, &s->sentinel_val, 4);
        }
    }
}

/* Posts the next voice-ended trap once the previous one was taken (FE_CONTROL
 * bits 5-7 cleared by DSOUND's helper, mode 1). */
static void post_trap(ma_state *s)
{
    if (s->trap_head == s->trap_tail)
        return;
    if (s->reg[R_FE_CONTROL / 4] & 0xE0)
        return;
    s->reg[R_TRAP_METHOD / 4] = 0x8000;
    s->reg[R_TRAP_ARG / 4] = s->trapq[s->trap_head & 511u];
    s->trap_head++;
    s->reg[R_FE_CONTROL / 4] |= 0xFE0;
    s->reg[R_IRQ_STATUS / 4] |= 0x11;
}

static void deliver(ma_state *s)
{
    int i;
    static int held;

    ma_lock(s);
    if (s->irq_pending) {
        s->reg[R_IRQ_STATUS / 4] |= s->irq_pending | 1;
        s->irq_pending = 0;
    }
    post_trap(s);
    ma_unlock(s);
    if (s->mode != MA_LIVE) {
        /* a shadow only counts what it would have raised */
        ma_lock(s);
        if (s->reg[R_IRQ_STATUS / 4]) s->st_irqs++;
        s->reg[R_IRQ_STATUS / 4] = 0;
        s->reg[R_FE_CONTROL / 4] &= ~0xE0u;
        ma_unlock(s);
        return;
    }
    for (i = 0; i < 4; i++) {
        uint32_t st = s->reg[R_IRQ_STATUS / 4], en = s->reg[R_IRQ_ENABLE / 4];
        if (!(st & en & 0xFE) || !(en & 1))
            return;
        if (xbox_IrqlBlocksInterrupts() && ++held < 20)
            return;                     /* try again after the next block */
        held = 0;
        s->st_irqs++;
        xbox_DeviceRaiseInterrupt(MA_VECTOR);
        xbox_TimerThreadWake();
        ma_lock(s);
        post_trap(s);
        ma_unlock(s);
    }
}

/* Shadow only: compare this model's per-voice state with what the live model
 * wrote back into the descriptors (state word, position), once per block, and
 * log each voice's first divergence and every activity mismatch ([MAD]). The
 * two run on separate clocks, so a position may lag by up to about one block
 * of source samples. */
static void shadow_check(ma_state *s, uint64_t block)
{
    static uint8_t reported[MA_VOICES];
    static unsigned lines;
    uint32_t base = s->reg[R_DESC_BASE / 4];
    int v;

    if (s->mode != MA_SHADOW || !base || s->index)
        return;
    {
        /* is the set of active voices the same in both models this block? */
        int mis = 0;
        for (v = 0; v < MA_VOICES; v++) {
            int oa = (ma_rd32(s, base + (uint32_t)v * 0x80u + D_STATE) & ST_ACTIVE) != 0;
            int na = (s->v[v].state & ST_ACTIVE) != 0;
            if (oa != na) {
                mis++;
                s->st_vmis_v[v][na]++;
            }
        }
        s->st_sets++;
        if (!mis)
            s->st_sets_equal++;
        s->st_vmis += (uint64_t)mis;
    }
    if (lines > 400)
        return;
    if (getenv("RECOMP_MA_WATCHRAM")) {
        /* which descriptor words change between blocks while no voice
         * method touched that voice: the live model's own RAM writes */
        static uint32_t prev[MA_VOICES][32];
        static int have;
        static unsigned wl;
        for (v = 0; v < MA_VOICES; v++) {
            uint32_t now[32];
            int k;
            memcpy(now, ma_pa(s, base + (uint32_t)v * 0x80u), 0x80);
            if (have && !s->v[v].touched && wl < 300)
                for (k = 0; k < 32; k++)
                    if (now[k] != prev[v][k] && k != D_POS / 4 && k != D_STATE / 4) {
                        wl++;
                        fprintf(stderr, "[MAW] blk %llu v%d +%02X %08X -> %08X\n",
                                (unsigned long long)block, v, k * 4, prev[v][k], now[k]);
                    }
            memcpy(prev[v], now, 0x80);
            s->v[v].touched = 0;
        }
        have = 1;
    }
    for (v = 0; v < MA_VOICES; v++) {
        uint32_t pa = base + (uint32_t)v * 0x80u;
        uint32_t ost = ma_rd32(s, pa + D_STATE), opos = ma_rd32(s, pa + D_POS);
        uint32_t mst = s->v[v].state, mpos = s->v[v].pos_word;
        int oa = (ost & ST_ACTIVE) != 0, na = (mst & ST_ACTIVE) != 0;
        if (oa != na) {
            if (reported[v] < 3) {
                reported[v]++;
                lines++;
                fprintf(stderr, "[MAD] blk %llu v%d active: live %d new %d (state %08X/%08X pos %06X/%06X fmt %08X)\n",
                        (unsigned long long)block, v, oa, na, ost, mst, opos & 0xFFFFFF, mpos & 0xFFFFFF,
                        ma_desc(s, v, D_FORMAT));
            }
            continue;
        }
        if (!oa)
            continue;
        {
            int d = (int)(opos & 0xFFFFFF) - (int)(mpos & 0xFFFFFF);
            if ((d > 700 || d < -700 || (ost & 0x70000000u) != (mst & 0x70000000u)
                    || (opos >> 24) != (mpos >> 24)) && reported[v] < 3) {
                reported[v]++;
                lines++;
                fprintf(stderr, "[MAD] blk %llu v%d pos live %06X new %06X (%+d) state %08X/%08X env %02X/%02X fmt %08X ssl %d seg %d\n",
                        (unsigned long long)block, v, opos & 0xFFFFFF, mpos & 0xFFFFFF, d, ost, mst,
                        opos >> 24, mpos >> 24, ma_desc(s, v, D_FORMAT), s->v[v].ssl_cur, s->v[v].seg);
            }
        }
    }
}

static void report(ma_state *s, double elapsed, uint64_t blocks)
{
    int v, active = 0;
    for (v = 0; v < MA_VOICES; v++)
        if (s->v[v].state & ST_ACTIVE)
            active++;
    fprintf(stderr, "[MA] %s out: %.1f blocks/s, voices %d, irqs %llu, traps %llu, notifies %llu, "
            "doorbell %llu, clipped %llu, pause frames %llu\n",
            s->mode == MA_LIVE ? "live" : "shadow", blocks / elapsed, active,
            (unsigned long long)s->st_irqs, (unsigned long long)s->st_traps,
            (unsigned long long)s->st_notifies, (unsigned long long)s->st_doorbell,
            (unsigned long long)s->st_clipped, (unsigned long long)s->st_pause_frames);
    {
        /* the title's own counters of DSP re-initialisations: 0x386AD8 is
         * bumped by the sentinel path (0x37E9AB), 0x386AFC by the IRQ bit-3
         * path (0x37E93D) */
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        uint32_t a, b;
        memcpy(&a, mem + 0x386AD8, 4);
        memcpy(&b, mem + 0x386AFC, 4);
        fprintf(stderr, "[MA] DSP resets by the title: sentinel %u, irq %u\n", a, b);
        if (s->mode == MA_SHADOW && s->st_sets)
            fprintf(stderr, "[MA] shadow: active-voice set equal to the live model's in %.3f%% of %llu blocks (%llu voice mismatches)\n",
                    100.0 * (double)s->st_sets_equal / (double)s->st_sets,
                    (unsigned long long)s->st_sets, (unsigned long long)s->st_vmis);
        if (s->mode == MA_SHADOW && s->st_vmis) {
            int k, v2;
            fprintf(stderr, "[MA] shadow: most mismatched voices (blocks active only live / only new):");
            for (k = 0; k < 6; k++) {
                int best = -1;
                uint32_t bn = 0;
                for (v2 = 0; v2 < MA_VOICES; v2++) {
                    uint32_t t = s->st_vmis_v[v2][0] + s->st_vmis_v[v2][1];
                    if (t > bn) { bn = t; best = v2; }
                }
                if (best < 0) break;
                fprintf(stderr, " v%d %u/%u", best, s->st_vmis_v[best][0], s->st_vmis_v[best][1]);
                s->st_vmis_v[best][0] = s->st_vmis_v[best][1] = 0;
            }
            fprintf(stderr, "\n");
        }
    }
    if (s->report_bins && s->bin_energy_n) {
        char line[1024];
        int b, n = 0;
        n += snprintf(line + n, sizeof line - (size_t)n, "[MA] mixbin energy:");
        for (b = 0; b < MA_BINS; b++) {
            /* same scale as the oracle's line: mean square of 16-bit values / 1000 */
            double e = s->bin_energy[b] / s->bin_energy_n * 32768.0 * 32768.0 / 1000.0;
            if (e >= 0.01)
                n += snprintf(line + n, sizeof line - (size_t)n, " %d:%.3g", b, e);
            s->bin_energy[b] = 0;
        }
        s->bin_energy_n = 0;
        fprintf(stderr, "%s\n", line);
    }
    if (s->report_voices) {
        for (v = 0; v < MA_VOICES; v++) {
            ma_voice *vc = &s->v[v];
            if (!(vc->state & ST_ACTIVE))
                continue;
            fprintf(stderr, "[MAV] v%d fmt %08X state %08X pos %06X rate %.4f consumed %llu out %llu env %.2f e %.4g\n",
                    v, ma_desc(s, v, D_FORMAT), vc->state, vc->pos_word & 0xFFFFFF,
                    vc->out_rendered ? (double)vc->src_consumed / vc->out_rendered : 0.0,
                    (unsigned long long)vc->src_consumed, (unsigned long long)vc->out_rendered,
                    vc->aenv_level, vc->energy * 32768.0 * 32768.0 / 1000.0 / (elapsed * MA_RATE));
            vc->energy = 0;
        }
    }
    fflush(stderr);
}

/* Shadow only: how many frames has the live model processed since the last
 * call? It writes each playing voice's position back after every frame, so
 * one voice's position, divided by its source samples per frame, is the live
 * model's frame clock. Running our frames on it keeps the two models' voice
 * starts and stops in the same frame (RECOMP_MA_LOCK=0 turns this off). */
static int live_frames(ma_state *s)
{
    static int tracker = -1;
    static uint32_t last;
    static double per_frame;
    static uint32_t span;
    static int still;
    uint32_t base = s->reg[R_DESC_BASE / 4];
    int v, best = -1;
    double best_step = 0;

    if (!base)
        return 0;
    if (tracker >= 0) {
        uint32_t pa = base + (uint32_t)tracker * 0x80u;
        uint32_t p = ma_rd32(s, pa + D_POS) & 0xFFFFFF;
        if ((ma_rd32(s, pa + D_STATE) & ST_ACTIVE) && p != last) {
            int d = (int)p - (int)last;
            int n;
            if (d < 0) d += (int)span;
            last = p;
            still = 0;
            n = (int)(d / per_frame + 0.5);
            {
                static int logn = -1;
                if (logn < 0) logn = getenv("RECOMP_MA_TICKLOG") ? 400 : 0;
                if (logn > 0) {
                    logn--;
                    fprintf(stderr, "[MAT] t=%.6f v%d d=%d n=%d per=%.2f\n", now_s(), tracker, d, n, per_frame);
                }
            }
            return n < 1 ? 1 : n > 16 ? 1 : n;
        }
        if ((ma_rd32(s, pa + D_STATE) & ST_ACTIVE) && ++still < 30)
            return 0;
        tracker = -1;                   /* stopped: pick another */
    }
    for (v = 0; v < MA_VOICES; v++) {
        uint32_t pa = base + (uint32_t)v * 0x80u;
        uint32_t st = ma_rd32(s, pa + D_STATE);
        uint32_t f = ma_rd32(s, pa + D_FORMAT);
        uint32_t end = ma_rd32(s, pa + D_END) & 0xFFFFFF;
        if ((st & ST_ACTIVE) && !(f & (F_STREAM | F_MULTIPASS)) && end >= 2048) {
            double step = pow(2.0, (int16_t)(ma_rd32(s, pa + D_LINK) >> 16) / 4096.0);
            if (step > best_step) { best_step = step; best = v; }
        }
    }
    if (best >= 0) {
        uint32_t pa = base + (uint32_t)best * 0x80u;
        tracker = best;
        last = ma_rd32(s, pa + D_POS) & 0xFFFFFF;
        per_frame = MA_FRAME * best_step;
        span = (ma_rd32(s, pa + D_END) & 0xFFFFFF) + 1;
        still = 0;
    }
    return 0;
}

static void wait_s(HANDLE timer, double w)
{
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(w * 1e7);
    if (due.QuadPart > -1000)
        due.QuadPart = -1000;
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, 50);
    else
        Sleep(1);
}

/* One frame (1500 Hz) at a time; every 8 frames a 256-sample block goes out. */
static DWORD WINAPI frame_thread(LPVOID arg)
{
    ma_state *s = (ma_state *)arg;
    const double frame_s = (double)MA_FRAME / MA_RATE;
    double next, t_report, t_last, t_chg, step_s;
    uint64_t blocks = 0, rep_blocks = 0;
    float lr[2 * MA_BLOCK];
    HANDLE timer;
    int lock_live, fi = 0, step;

    if (s->mode == MA_LIVE) {
        uint32_t tib = xbox_AllocThreadTib();
        if (tib)
            g_fs_base = tib;
        else
            fprintf(stderr, "[MA] no TIB for the frame thread; no interrupts\n");
    }
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    /* a high-resolution waitable timer: no spinning beside the guest */
    timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    t_report = t_last = t_chg = now_s();
    {
        const char *e = ma_getenv(s, "RECOMP_MA_LOCK");
        lock_live = s->mode == MA_SHADOW && e && *e == '1';  /* off by default: it can drop blocks */
        e = ma_getenv(s, "RECOMP_MA_BURST");
        /* 8 frames back to back once per block (what the previous model does,
         * so voice starts and stops land on the same block boundaries), or
         * one frame at a time at 1500 Hz with RECOMP_MA_BURST=0 */
        step = (e && *e == '0') ? 1 : MA_BLOCK / MA_FRAME;
    }
    step_s = step * frame_s;
    next = t_last + step_s;
    while (!s->quit) {
        double t = now_s();
        int n, k;
        if (lock_live) {
            int fr = live_frames(s);
            if (fr) {
                int fresh = t - t_chg > 0.0025;     /* a new burst of the live model */
                t_chg = t;
                if (step > 1) {
                    if (!fresh) {
                        wait_s(timer, 0.0002);
                        continue;
                    }
                    n = 1;
                } else {
                    n = fr;
                }
                t_last = t;
            } else {
                if (t < t_last + 3 * MA_BLOCK * frame_s) {
                    wait_s(timer, 0.0002);
                    continue;
                }
                /* no clock to follow: run what is due */
                n = (int)((t - t_last) / step_s);
                if (n > 64) n = 64;
                t_last += n * step_s;
            }
        } else {
            if (t < next) {
                wait_s(timer, next - t);
                continue;
            }
            if (t - next > 0.1)
                next = t;                   /* fell far behind: do not race */
            next += step_s;
            n = 1;
        }
        for (k = 0; k < n * step; k++) {
            ma_lock(s);
            if (vp_running(s)) {
                ma_process_frame(s);
                ma_mix_frame(s, lr + 2 * MA_FRAME * fi);
            } else {
                memset(s->bin, 0, sizeof s->bin);
                ma_mix_frame(s, lr + 2 * MA_FRAME * fi);
                s->st_pause_frames++;
            }
            ma_unlock(s);
            if (step == 1)
                deliver(s);
            if (++fi < MA_BLOCK / MA_FRAME)
                continue;
            fi = 0;
            ma_output_block(s, lr, MA_BLOCK);
            blocks++; rep_blocks++;
            if (step > 1)
                deliver(s);
            service_dsp(s);
            shadow_check(s, blocks);
            if (s->rv && (blocks & 31) == 0)
                ma_reverb_params_from_title(s);
            if (t - t_report >= 5.0) {
                report(s, t - t_report, rep_blocks);
                t_report = t;
                rep_blocks = 0;
            }
        }
    }
    timeEndPeriod(1);
    if (timer)
        CloseHandle(timer);
    return 0;
}

int ma_thread_start(ma_state *s)
{
    const char *m = getenv("RECOMP_APU_MUTE");
    s->mute = m && *m == '1';
    if (s->mode == MA_LIVE && !s->mute)
        s->device = xa2_init();
    s->thread = CreateThread(NULL, 0, frame_thread, s, 0, NULL);
    fprintf(stderr, "[MA] clean-room APU %s up (%s)\n", s->mode == MA_LIVE ? "live" : "shadow",
            s->device ? MA_DEVICE_NAME : "no device");
    return s->thread ? 0 : -1;
}

void ma_thread_stop(ma_state *s)
{
    s->quit = 1;
    if (s->thread) {
        WaitForSingleObject((HANDLE)s->thread, 1000);
        CloseHandle((HANDLE)s->thread);
        s->thread = NULL;
    }
    ma_tap_close(s);
    if (s->device)
        xa2_shutdown();
}

void ma_raise(ma_state *s, uint32_t bits)
{
    s->irq_pending |= bits;
}

void ma_report(ma_state *s)
{
    report(s, 1.0, 0);
}
