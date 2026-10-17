/* ma_fx.c -- additions of the clean-room APU model that the hardware leaves to
 * the GP DSP program: an I3DL2-style reverb on the mixbin-10 send and a
 * look-ahead limiter on the output. Both are original code.
 *
 * Reverb: an 8-line feedback delay network (Jot's structure: mutually prime
 * delay lines, a Householder feedback matrix, per-line one-pole low-pass for
 * the HF decay ratio) after a short early-reflection tap line. Parameters
 * follow the IASIG I3DL2 guideline (room, room_hf, decay_time,
 * decay_hf_ratio, reflections, reflections_delay, reverb, reverb_delay,
 * diffusion, density, hf_reference) and are taken from the title's own
 * sound-driver globals when available (ma_reverb_params_from_title). The
 * return is added to both sides at -6 dB.
 *
 * Limiter: 1.5 ms look-ahead (72 samples), threshold -1 dBFS, gain reaches
 * its target over the look-ahead and recovers with an 80 ms release. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ma_internal.h"

#define RV_LINES 8
#define RV_MAXD  8192
#define ER_TAPS  6

struct ma_reverb {
    float  line[RV_LINES][RV_MAXD];
    int    len[RV_LINES], w[RV_LINES];
    float  lp[RV_LINES], g[RV_LINES], damp[RV_LINES];
    float  er[4096];
    int    er_w, er_tap[ER_TAPS];
    float  er_gain[ER_TAPS];
    int    pre;                 /* reverb_delay in samples */
    float  pre_buf[4096];
    int    pre_w;
    float  room, wet_late, wet_er;
    float  diffusion;
};

static const int k_base_len[RV_LINES] = { 1031, 1327, 1523, 1801, 2063, 2357, 2647, 2969 };

static void set_decay(ma_reverb *r, float t60, float hf_ratio, float hf_ref)
{
    int i;
    float w = 2.0f * 3.14159265f * hf_ref / MA_RATE;
    for (i = 0; i < RV_LINES; i++) {
        float d = (float)r->len[i] / MA_RATE;
        float g_dc = powf(10.0f, -3.0f * d / t60);
        float g_hf = powf(10.0f, -3.0f * d / (t60 * hf_ratio));
        float ratio = g_hf / g_dc;          /* gain at hf_ref relative to DC */
        /* one-pole: y = (1-a) x + a y; |H(w)| = (1-a)/sqrt(1-2a cos w + a^2) */
        float a = 0;
        if (ratio < 0.999f) {
            float c = cosf(w), r2 = ratio * ratio;
            float A = 1.0f - r2, B = -2.0f * (1.0f - r2 * c), C = 1.0f - r2;
            /* solve (1-a)^2 = r2 (1 - 2a c + a^2) */
            float disc = B * B - 4 * A * C;
            if (A > 1e-6f && disc >= 0)
                a = (-B - sqrtf(disc)) / (2 * A);
            if (a < 0) a = 0;
            if (a > 0.95f) a = 0.95f;
        }
        r->g[i] = g_dc;
        r->damp[i] = a;
    }
}

ma_reverb *ma_reverb_create(void)
{
    ma_reverb *r = (ma_reverb *)calloc(1, sizeof *r);
    int i;
    static const float er_t[ER_TAPS] = { 0.0043f, 0.0215f, 0.0225f, 0.0268f, 0.0270f, 0.0298f };
    static const float er_g[ER_TAPS] = { 0.841f, 0.504f, 0.491f, 0.379f, 0.380f, 0.346f };
    if (!r)
        return NULL;
    for (i = 0; i < RV_LINES; i++)
        r->len[i] = k_base_len[i];
    for (i = 0; i < ER_TAPS; i++) {
        r->er_tap[i] = (int)(er_t[i] * MA_RATE);
        r->er_gain[i] = er_g[i];
    }
    /* "Generic" until the title's parameters arrive */
    r->room = powf(10.0f, -1000.0f / 2000.0f);
    r->wet_late = powf(10.0f, 200.0f / 2000.0f);
    r->wet_er = powf(10.0f, -2602.0f / 2000.0f);
    r->pre = (int)(0.011f * MA_RATE);
    r->diffusion = 1.0f;
    set_decay(r, 1.49f, 0.83f, 5000.0f);
    return r;
}

void ma_reverb_destroy(ma_reverb *r)
{
    free(r);
}

/* in: mono send (32 samples), lr: stereo accumulate */
void ma_reverb_process(ma_reverb *r, const float *in, float *lr, int frames)
{
    int n, i;
    for (n = 0; n < frames; n++) {
        float x = in[n] * r->room, er = 0, late_in, o[RV_LINES], sum = 0, outl = 0, outr = 0;
        /* early reflections */
        r->er[r->er_w] = x;
        for (i = 0; i < ER_TAPS; i++)
            er += r->er_gain[i] * r->er[(r->er_w - r->er_tap[i]) & 4095];
        r->er_w = (r->er_w + 1) & 4095;
        /* pre-delay into the network */
        r->pre_buf[r->pre_w] = x;
        late_in = r->pre_buf[(r->pre_w - r->pre) & 4095];
        r->pre_w = (r->pre_w + 1) & 4095;
        for (i = 0; i < RV_LINES; i++) {
            float y = r->line[i][(r->w[i] - r->len[i]) & (RV_MAXD - 1)];
            r->lp[i] = (1.0f - r->damp[i]) * y + r->damp[i] * r->lp[i];
            o[i] = r->lp[i] * r->g[i];
            sum += o[i];
        }
        sum *= 2.0f / RV_LINES;                 /* Householder */
        for (i = 0; i < RV_LINES; i++) {
            r->line[i][r->w[i]] = late_in + (o[i] - sum) * r->diffusion;
            r->w[i] = (r->w[i] + 1) & (RV_MAXD - 1);
            if (i & 1) outr += o[i]; else outl += o[i];
        }
        outl = 0.5f * (outl * r->wet_late * 0.5f + er * r->wet_er);
        outr = 0.5f * (outr * r->wet_late * 0.5f + er * r->wet_er);
        lr[2 * n] += outl;                      /* return at -6 dB */
        lr[2 * n + 1] += outr;
    }
}

/* The title's I3DL2 parameters: s_sound_driver_globals (decomp
 * include/sound_driver.h) through the pointer at 0x51EBE4; reverbs[0] at
 * +0x2950: 8 bytes, then room, room_hf, room_rolloff, decay_time,
 * decay_hf_ratio, reflections, reflections_delay, reverb, reverb_delay,
 * diffusion, density, hf_reference (floats; levels in mB, times in s). */
extern ptrdiff_t xbox_GetMemoryOffset(void);

static int sane(float x, float lo, float hi) { return x == x && x >= lo && x <= hi; }

void ma_reverb_params_from_title(ma_state *s)
{
    static float last[12];
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t g;
    float p[12];
    ma_reverb *r = s->rv;
    int i, changed = 0;

    if (!r)
        return;
    memcpy(&g, mem + 0x51EBE4, 4);
    if (g < 0x10000 || g >= 0x84000000u)
        return;
    memcpy(p, mem + g + 0x2950 + 8, sizeof p);
    {
        static int shown;
        if (!shown && getenv("RECOMP_MA_REVERB_DEBUG")) {
            shown = 1;
            fprintf(stderr, "[MA] reverb globals at 0x%08X: %g %g %g %g %g %g %g %g %g %g %g %g\n", g,
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
        }
    }
    if (!sane(p[0], -10000, 0) || !sane(p[3], 0.1f, 20) || !sane(p[4], 0.1f, 2)
            || !sane(p[11], 20, 20000) || !sane(p[6], 0, 0.3f) || !sane(p[8], 0, 0.1f))
        return;
    for (i = 0; i < 12; i++)
        if (p[i] != last[i]) changed = 1;
    if (!changed)
        return;
    memcpy(last, p, sizeof p);
    {
        /* the title keeps the levels in dB (measured: -100 = off, then -12,
         * -12, +4 in the Armory), not the mB of the I3DL2 guideline */
        float div = (p[0] >= -100.5f && p[5] >= -100.5f && p[7] >= -100.5f) ? 20.0f : 2000.0f;
        float floor_ = div == 20.0f ? -100.0f : -10000.0f;
        r->room = p[0] <= floor_ ? 0.0f : powf(10.0f, p[0] / div);
        r->wet_er = p[5] <= floor_ ? 0.0f : powf(10.0f, p[5] / div);
        r->wet_late = p[7] <= floor_ ? 0.0f : powf(10.0f, p[7] / div);
    }
    r->pre = (int)((p[6] + p[8]) * MA_RATE);
    if (r->pre > 4000) r->pre = 4000;
    r->diffusion = sane(p[9], 0, 100) ? 0.5f + 0.5f * p[9] / 100.0f : 1.0f;
    set_decay(r, p[3], p[4], p[11]);
    fprintf(stderr, "[MA] reverb from the title: room %.0f, decay %.2f s x%.2f HF, "
            "reflections %.0f @%.3f s, reverb %.0f @%.3f s (dB)\n",
            p[0], p[3], p[4], p[5], p[6], p[7], p[8]);
}

void ma_limiter_process(ma_limiter *l, float *lr, int frames)
{
    const int la = 72;
    const float thr = 0.891251f;                /* -1 dBFS */
    const float rel = 1.0f - expf(-1.0f / (0.080f * MA_RATE));
    int n, c;
    for (n = 0; n < frames; n++) {
        float peak = 0, target, x[2];
        int r;
        for (c = 0; c < 2; c++) {
            l->buf[c][l->w] = lr[2 * n + c];
        }
        /* the peak over the look-ahead window */
        for (r = 0; r <= la; r++) {
            int k = (l->w - r) & 127;
            float a = fabsf(l->buf[0][k]), b = fabsf(l->buf[1][k]);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        target = peak > thr ? thr / peak : 1.0f;
        if (target < l->gain)
            l->gain += (target - l->gain) * (1.0f / la) * 4.0f;
        else
            l->gain += (target - l->gain) * rel;
        if (l->gain > target && target < 1.0f && peak * l->gain > 1.0f)
            l->gain = target;
        for (c = 0; c < 2; c++)
            x[c] = l->buf[c][(l->w - la) & 127] * l->gain;
        lr[2 * n] = x[0];
        lr[2 * n + 1] = x[1];
        l->w = (l->w + 1) & 127;
    }
}
