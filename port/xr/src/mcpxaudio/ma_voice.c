/* ma_voice.c -- the voice processor of the clean-room APU model: list walk,
 * sample fetch (buffer pages and stream segments), ADPCM, resampling,
 * amplitude envelope, multipass sources, HRTF, and accumulation into the 32
 * mixbins. Field meanings: docs/APU_FACTS.md section 5. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ma_internal.h"

/* IMA ADPCM tables (Xbox ADPCM uses them unchanged: XboxDevWiki Xbox_ADPCM). */
static const int8_t k_index_tab[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t k_step_tab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767 };

static float s_vol_tab[4096];       /* 12-bit attenuation (1/64 dB) -> gain */
static int   s_tab_ready;

static void tables(void)
{
    int i;
    if (s_tab_ready)
        return;
    for (i = 0; i < 4096; i++)
        s_vol_tab[i] = i >= 0xFFF ? 0.0f : (float)pow(10.0, -(double)i / (64.0 * 20.0));
    s_tab_ready = 1;
}

/* Decodes one block: header sample, then 64 nibbles per channel; stereo
 * interleaves 4 bytes of left and 4 of right after the two headers. */
static void adpcm_block(const uint8_t *b, int ch, int16_t out[2][65])
{
    int c;
    for (c = 0; c < ch; c++) {
        const uint8_t *h = b + 4 * c;
        int pred = (int16_t)(h[0] | (h[1] << 8));
        int idx = h[2];
        int n;
        if (idx > 88) idx = 88;
        out[c][0] = (int16_t)pred;
        for (n = 0; n < 64; n++) {
            int byte_i, nib, step, diff;
            if (ch == 1) {
                byte_i = 4 + (n >> 1);
            } else {
                int group = n >> 3;             /* 8 nibbles per 4 bytes */
                byte_i = 8 + group * 8 + c * 4 + ((n & 7) >> 1);
            }
            nib = (n & 1) ? (b[byte_i] >> 4) : (b[byte_i] & 15);
            step = k_step_tab[idx];
            diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            if (nib & 8) pred -= diff; else pred += diff;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            idx += k_index_tab[nib];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            out[c][n + 1] = (int16_t)pred;
        }
    }
}

/* ---- format --------------------------------------------------------------- */

typedef struct fmtinfo {
    int kind;           /* 0 8-bit, 1 16-bit, 2 ADPCM, 3 24-in-32, 4 32-bit */
    int ch;
    int frame_bytes;    /* PCM bytes per sample frame */
} fmtinfo;

static fmtinfo decode_format(uint32_t f)
{
    fmtinfo fi;
    uint32_t top = f >> 28;
    fi.ch = (f & F_MULTIPASS) ? 1 : (int)((f >> 16) & 0x1F) + 1;
    if (fi.ch > 2) fi.ch = 2;
    switch (top) {
    case 0x5: fi.kind = 1; break;
    case 0xA: fi.kind = 2; break;
    case 0xE: fi.kind = 3; break;
    case 0xF: fi.kind = 4; break;
    default:  fi.kind = (top & 3) == 1 ? 1 : 0; break;
    }
    fi.frame_bytes = (fi.kind == 0 ? 1 : fi.kind == 1 ? 2 : 4) * fi.ch;
    return fi;
}

/* buffer page space -> physical */
static uint32_t page_pa(ma_state *s, uint32_t addr)
{
    uint32_t tab = s->reg[R_PAGE_TABLE / 4];
    uint32_t pg = (addr >> 12) & 0x7FF;
    return (ma_rd32(s, tab + pg * 8u) & ~0xFFFu) + (addr & 0xFFF);
}

static float pcm_at(ma_state *s, uint32_t pa, int kind)
{
    const uint8_t *p = ma_pa(s, pa);
    switch (kind) {
    case 0: return ((int)p[0] - 128) * (1.0f / 128.0f);
    case 1: return (int16_t)(p[0] | (p[1] << 8)) * (1.0f / 32768.0f);
    case 3: {
        int32_t x = (int32_t)((uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16)) << 8) >> 8;
        return x * (1.0f / 8388608.0f);
    }
    default: {
        int32_t x;
        memcpy(&x, p, 4);
        return x * (1.0f / 2147483648.0f);
    }
    }
}

/* Reads source frame idx of a sample block that starts at 'start'
 * (page-space address when paged, else physical). */
static void fetch_at(ma_state *s, ma_voice *vc, const fmtinfo *fi, uint32_t start,
                     int paged, uint32_t idx, float out[2])
{
    int c;
    if (fi->kind == 2) {
        uint32_t bb = 36u * (uint32_t)fi->ch;
        uint32_t blk = idx / (uint32_t)s->adpcm_spb, in = idx % (uint32_t)s->adpcm_spb;
        uint32_t a = start + blk * bb;
        if (vc->blk_id != (int64_t)blk || vc->blk_src != start) {
            uint8_t tmp[72];
            uint32_t k;
            for (k = 0; k < bb; k++)
                tmp[k] = *ma_pa(s, paged ? page_pa(s, a + k) : a + k);
            adpcm_block(tmp, fi->ch, vc->blk);
            vc->blk_id = blk;
            vc->blk_src = start;
        }
        out[0] = vc->blk[0][in] * (1.0f / 32768.0f);
        out[1] = fi->ch > 1 ? vc->blk[1][in] * (1.0f / 32768.0f) : out[0];
        return;
    }
    for (c = 0; c < fi->ch; c++) {
        int cb = fi->frame_bytes / fi->ch;
        uint32_t a = start + idx * (uint32_t)fi->frame_bytes + (uint32_t)(c * cb);
        out[c] = pcm_at(s, paged ? page_pa(s, a) : a, fi->kind);
    }
    if (fi->ch == 1)
        out[1] = out[0];
}

/* ---- the next source frame of a voice ---------------------------------------
 * Advances the voice by one source frame. Returns 0 if the voice ended (a
 * one-shot buffer ran past its end), 1 otherwise; a starving stream returns
 * silence and stays where it is. */
static int next_frame(ma_state *s, int v, const fmtinfo *fi, uint32_t fmt, float out[2])
{
    ma_voice *vc = &s->v[v];
    uint32_t pos = vc->pos_word & 0xFFFFFF;

    if (fmt & F_STREAM) {
        for (;;) {
            if (!vc->list_live) {
                uint32_t list = vc->ssl[vc->ssl_cur];
                if (!vc->ssl_valid[vc->ssl_cur] || !(list & 0xFF)) {
                    out[0] = out[1] = 0;
                    return 1;                   /* starving: wait for data */
                }
                vc->list_live = 1;
                vc->list_first = (list >> 8) & 0xFFFF;
                vc->list_count = list & 0xFF;
                vc->list_gen = vc->ssl_gen[vc->ssl_cur];
                vc->seg = 0;
                vc->seg_live = 0;
            }
            if (!vc->seg_live) {
                uint32_t ent = s->reg[R_SEG_TABLE / 4] + (vc->list_first + (uint32_t)vc->seg) * 8u;
                vc->seg_addr = ma_rd32(s, ent);
                vc->seg_frames = ma_rd32(s, ent + 4) & 0xFFFF;
                vc->seg_live = 1;
                vc->blk_id = -1;
                {
                    static int lv = -2;
                    if (lv == -2) { const char *e = getenv("RECOMP_MA_SEGLOG"); lv = e ? atoi(e) : -1; }
                    if (lv == v)
                        fprintf(stderr, "[MAS] frame %llu v%d list %d first %04X seg %d addr %08X frames %u gen %u/%u valid %d/%d ssl %06X/%06X\n",
                                (unsigned long long)s->frames, v, vc->ssl_cur, vc->list_first, vc->seg,
                                vc->seg_addr, vc->seg_frames, vc->list_gen, vc->ssl_gen[vc->ssl_cur],
                                vc->ssl_valid[0], vc->ssl_valid[1], vc->ssl[0], vc->ssl[1]);
                }
            }
            if (pos < vc->seg_frames) {
                fetch_at(s, vc, fi, vc->seg_addr, 0, pos, out);
                pos++;
                vc->pos_word = (vc->pos_word & 0xFF000000u) | pos;
                return 1;
            }
            /* segment done */
            pos = 0;
            vc->pos_word &= 0xFF000000u;
            vc->seg_live = 0;
            if ((uint32_t)++vc->seg >= vc->list_count) {
                ma_notify(s, v, vc->ssl_cur);
                s->irq_pending |= 0x61;         /* oracle raises 0x61 here */
                if (vc->ssl_gen[vc->ssl_cur] == vc->list_gen)
                    vc->ssl_valid[vc->ssl_cur] = 0;
                vc->ssl_cur ^= 1;
                vc->list_live = 0;
            }
        }
    } else {
        uint32_t end = vc->end_word & 0xFFFFFF;
        uint32_t base = vc->snap[D_BASE / 4];
        if (pos > end) {
            if (fmt & F_LOOP) {
                pos = vc->snap[D_LOOP / 4] & 0xFFFFFF;
                if (pos > end) pos = 0;
            } else {
                out[0] = out[1] = 0;
                return 0;
            }
        }
        fetch_at(s, vc, fi, base, 1, pos, out);
        pos++;
        vc->pos_word = (vc->pos_word & 0xFF000000u) | (pos & 0xFFFFFF);
        return 1;
    }
}

/* ---- envelope ------------------------------------------------------------- */

/* Amplitude envelope, once per frame (1500 Hz). Times are 12-bit in units of
 * 16 frames; sustain is 8-bit (XboxDevWiki APU, "Envelopes"). */
static void aenv_frame(ma_state *s, int v)
{
    ma_voice *vc = &s->v[v];
    uint32_t a = vc->snap[D_AENV_A / 4], b = vc->snap[D_AENV_B / 4];
    uint32_t rel = vc->snap[D_AENV_REL / 4] & 0xFFF;
    float sus = (float)(b >> 24) / 255.0f;
    int guard = 0;

again:
    if (++guard > 8)
        return;
    switch (vc->aenv_state) {
    case 0: vc->aenv_level = 1.0f; break;
    case 1: {                                   /* delay */
        int t = (int)((a >> 12) & 0xFFF) * 16;
        vc->aenv_level = 0;
        if (vc->aenv_count >= t) { vc->aenv_state = 2; vc->aenv_count = 0; goto again; }
        vc->aenv_count++;
        break;
    }
    case 2: {                                   /* attack, linear */
        int t = (int)(a & 0xFFF) * 16;
        if (vc->aenv_count >= t) { vc->aenv_level = 1; vc->aenv_state = 3; vc->aenv_count = 0; goto again; }
        vc->aenv_level = (float)vc->aenv_count / (float)t;
        vc->aenv_count++;
        break;
    }
    case 3: {                                   /* hold */
        int t = (int)((b >> 12) & 0xFFF) * 16;
        vc->aenv_level = 1;
        if (vc->aenv_count >= t) { vc->aenv_state = 4; vc->aenv_count = 0; goto again; }
        vc->aenv_count++;
        break;
    }
    case 4: {                                   /* decay towards sustain */
        int t = (int)(b & 0xFFF) * 16;
        if (vc->aenv_count >= t || vc->aenv_level <= sus) {
            vc->aenv_level = sus; vc->aenv_state = 5; vc->aenv_count = 0; goto again;
        }
        vc->aenv_level = 1.0f - (1.0f - sus) * (float)vc->aenv_count / (float)t;
        vc->aenv_count++;
        break;
    }
    case 5: vc->aenv_level = sus; break;
    case 6:                                     /* release: see ma_render_voice */
        break;
    default: break;
    }
    vc->state = (vc->state & ~0x70000000u) | ((uint32_t)vc->aenv_state << 28);
    {
        int lv = (int)(vc->aenv_level * 255.0f + 0.5f);
        vc->pos_word = (vc->pos_word & 0xFFFFFF) | ((uint32_t)lv << 24);
    }
}

/* ---- HRTF ------------------------------------------------------------------ */

/* 31 taps per side, interleaved left/right bytes, sign-magnitude; bytes 62-63
 * a signed delay (docs/APU_FACTS.md; oracle [APUH] lines decode the same
 * bytes). */
static void hrtf_coefs(ma_state *s, int entry, float c[2][31], int *itd)
{
    const uint8_t *p = s->hrtf_tab[entry & 0xFF];
    double n = 0;
    int k, ch;
    for (k = 0; k < 31; k++)
        for (ch = 0; ch < 2; ch++) {
            uint8_t b = p[2 * k + ch];
            float x = (float)(b & 0x7F) / 128.0f;
            c[ch][s->hrtf_rev ? 30 - k : k] = (b & 0x80) ? -x : x;
        }
    /* byte 63: interaural delay, signed, in half samples (matched against
     * the oracle's [APUH] "itd" print: 42 -> 21.0, 252 -> -2.0) */
    *itd = (int8_t)p[63];
    /* gain: raw (/128), or one scale for both ears so the level difference
     * between them stays: l1 (sum |c| = 1 on the louder ear) or the default,
     * unit RMS over both ears */
    if (s->hrtf_norm == 0)
        return;
    if (s->hrtf_norm == 1) {
        double l[2] = { 0, 0 };
        for (ch = 0; ch < 2; ch++)
            for (k = 0; k < 31; k++)
                l[ch] += fabs(c[ch][k]);
        n = l[0] > l[1] ? l[0] : l[1];
    } else if (s->hrtf_norm == 5) {
        /* each ear to unit RMS on its own */
        for (ch = 0; ch < 2; ch++) {
            double e = 0;
            for (k = 0; k < 31; k++)
                e += (double)c[ch][k] * c[ch][k];
            e = sqrt(e);
            if (e > 1e-6)
                for (k = 0; k < 31; k++)
                    c[ch][k] = (float)(c[ch][k] / e);
        }
        return;
    } else {
        double e[2] = { 0, 0 };
        for (ch = 0; ch < 2; ch++)
            for (k = 0; k < 31; k++)
                e[ch] += (double)c[ch][k] * c[ch][k];
        n = s->hrtf_norm == 3 ? sqrt(e[0] > e[1] ? e[0] : e[1])
          : s->hrtf_norm == 4 ? sqrt(e[0] + e[1])
          : sqrt((e[0] + e[1]) / 2.0);
    }
    /* the previous model's HRTF path is 1.34 dB below unit energy over both
     * ears: fitted, oracle ~ g x this model's HRTF output, over 123 one-second
     * windows of Ivory Tower fire with a residual of 0.2% of the energy
     * (docs/APU_FACTS.md 9) */
    if (n > 1e-6)
        n /= s->hrtf_trim;
    if (n > 1e-6)
        for (ch = 0; ch < 2; ch++)
            for (k = 0; k < 31; k++)
                c[ch][k] = (float)(c[ch][k] / n);
}

/* ---- one voice, one frame ---------------------------------------------------- */

static void vols(ma_state *s, int v, float g[8], int sel[8])
{
    const uint32_t *w = s->v[v].snap;
    uint32_t b0 = w[D_BINS / 4], f = w[D_FORMAT / 4];
    uint32_t a = w[D_VOL0 / 4], b = w[D_VOL0 / 4 + 1], c = w[D_VOL0 / 4 + 2];
    uint32_t att[8];
    int i;
    sel[0] = b0 & 31; sel[1] = (b0 >> 5) & 31; sel[2] = (b0 >> 10) & 31;
    sel[3] = (b0 >> 16) & 31; sel[4] = (b0 >> 21) & 31; sel[5] = (b0 >> 26) & 31;
    sel[6] = f & 31; sel[7] = (f >> 5) & 31;
    att[0] = (a >> 4) & 0xFFF; att[1] = a >> 20;
    att[2] = (b >> 4) & 0xFFF; att[3] = b >> 20;
    att[4] = (c >> 4) & 0xFFF; att[5] = c >> 20;
    att[6] = (a & 15) | ((b & 15) << 4) | ((c & 15) << 8);
    att[7] = ((a >> 16) & 15) | (((b >> 16) & 15) << 4) | (((c >> 16) & 15) << 8);
    for (i = 0; i < 8; i++)
        g[i] = s_vol_tab[att[i] & 0xFFF];
}

static float interp(const float h[4][2], int c, float t, int mode)
{
    float x0 = h[0][c], x1 = h[1][c], x2 = h[2][c], x3 = h[3][c];
    if (!mode)
        return x1 + (x2 - x1) * t;
    {
        float c1 = 0.5f * (x2 - x0);
        float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        return ((c3 * t + c2) * t + c1) * t + x1;
    }
}

static void push(ma_voice *vc, const float x[2])
{
    memmove(vc->hist[0], vc->hist[1], sizeof vc->hist[0] * 3);
    vc->hist[3][0] = x[0];
    vc->hist[3][1] = x[1];
}

void ma_voice_start(ma_state *s, int v)
{
    ma_voice *vc = &s->v[v];
    tables();
    vc->frac = 0;
    memset(vc->hist, 0, sizeof vc->hist);
    vc->hist_pos = -1;                  /* not primed */
    vc->blk_id = -1;
    vc->ssl_cur = 0;
    vc->seg = 0;
    vc->list_live = 0;
    vc->seg_live = 0;
    vc->aenv_state = (int)((vc->state >> 28) & 7);
    vc->aenv_count = 0;
    vc->aenv_level = vc->aenv_state == 0 || vc->aenv_state >= 3 ? 1.0f : 0.0f;
    vc->rel_count = vc->rel_total = 0;
    memset(vc->fir_hist, 0, sizeof vc->fir_hist);
    memset(vc->svf, 0, sizeof vc->svf);
    memset(vc->i3, 0, sizeof vc->i3);
    vc->fir_pos = 0;
    vc->frames_played = 0;
}

void ma_render_voice(ma_state *s, int v, int list)
{
    ma_voice *vc = &s->v[v];
    uint32_t fmt;
    fmtinfo fi;

    if (vc->wait_live > 0) {
        uint32_t pa = s->reg[R_DESC_BASE / 4] + (uint32_t)v * 0x80u;
        if (ma_rd32(s, pa + D_STATE) == vc->wait_state && ma_rd32(s, pa + D_POS) == vc->wait_pos
                && --vc->wait_live > 0)
            return;                         /* the live model has not run it yet */
        vc->wait_live = 0;
    }

    if (!vc->locked || !vc->snap[D_FORMAT / 4])
        memcpy(vc->snap, ma_pa(s, s->reg[R_DESC_BASE / 4] + (uint32_t)v * 0x80u), 0x80);
    fmt = vc->snap[D_FORMAT / 4];
    fi = decode_format(fmt);
    float g[8], src[MA_FRAME][2];
    int sel[8], n, i, ended = 0;
    float env;

    (void)list;
    aenv_frame(s, v);
    env = vc->aenv_level;
    if (vc->aenv_state == 6) {
        /* output = level x count / (rate x 16), count down per frame
         * (XboxDevWiki APU, Release) */
        if (vc->rel_count <= 0) {
            vc->state = (vc->state & ~(ST_ACTIVE | 0x77000000u)) | 0x77000000u;
            ma_desc_set(s, v, D_STATE, vc->state);
            ma_notify(s, v, vc->ssl_cur);
            ma_voice_ended(s, v);
            return;
        }
        env *= (float)vc->rel_count / (float)vc->rel_total;
        vc->rel_count--;
        ma_wr32(s, s->reg[R_DESC_BASE / 4] + (uint32_t)v * 0x80u + 0x34, (uint32_t)vc->rel_count);
    }
    vols(s, v, g, sel);

    if (fmt & F_MULTIPASS) {
        /* source = a mixbin (bits 16-20); reading it empties it */
        int sb = (int)((fmt >> 16) & 31);
        for (n = 0; n < MA_FRAME; n++) {
            src[n][0] = src[n][1] = s->bin[sb][n];
            if (s->mp_mode == 0)
                s->bin[sb][n] = 0;
        }
        fi.ch = 1;
    } else {
        int p = (int16_t)(vc->snap[D_LINK / 4] >> 16);
        double step = pow(2.0, p / 4096.0);
        if (vc->hist_pos < 0) {
            float x[2];
            for (i = 0; i < 3; i++) {
                if (!next_frame(s, v, &fi, fmt, x)) { ended = 1; x[0] = x[1] = 0; }
                push(vc, x);
            }
            vc->hist_pos = 0;
        }
        for (n = 0; n < MA_FRAME; n++) {
            float t = (float)vc->frac;
            src[n][0] = interp((const float (*)[2])vc->hist, 0, t, s->interp);
            src[n][1] = fi.ch > 1 ? interp((const float (*)[2])vc->hist, 1, t, s->interp) : src[n][0];
            vc->frac += step;
            while (vc->frac >= 1.0) {
                float x[2];
                vc->frac -= 1.0;
                if (!ended && !next_frame(s, v, &fi, fmt, x)) ended = 1;
                if (ended) x[0] = x[1] = 0;
                push(vc, x);
                vc->src_consumed++;
            }
        }
        vc->out_rendered += MA_FRAME;
        /* RECOMP_MA_VOICEWAV=<v>,<file>: that voice's resampled source, raw
         * stereo float32 at 48 kHz (diagnostics) */
        {
            static int tv = -2;
            static FILE *tf;
            if (tv == -2) {
                const char *e = getenv("RECOMP_MA_VOICEWAV");
                tv = -1;
                if (e && *e) {
                    const char *c = strchr(e, ',');
                    tv = atoi(e);
                    if (c) tf = fopen(c + 1, "wb");
                }
            }
            if (v == tv && tf && !s->index) {
                fwrite(src, sizeof(float), 2 * MA_FRAME, tf);
                fflush(tf);
            }
        }
    }

    if (s->only) {
        int is_hrtf = s->hrtf_on && (vc->snap[D_HRTF / 4] & 0xFFFF) != 0xFFFF && (fmt & F_MULTIPASS);
        if ((s->only == 1 && !is_hrtf) || (s->only == 2 && is_hrtf) || (s->only == 3 && list != 0) || (s->only == 4 && (list == 0 || is_hrtf))) {
            /* keep the source's writes into the bin the HRTF voice reads */
            for (i = 0; i < 8; i++)
                if (sel[i] == 31 && g[i] != 0.0f)
                    for (n = 0; n < MA_FRAME; n++)
                        s->bin[31][n] += g[i] * env * src[n][fi.ch > 1 ? (i & 1) : 0];
            goto done;
        }
    }
    /* Filters (SetFilter: +0x18 bits 16-17 mode, +0x74 coefficients 0/1,
     * +0x78 coefficients 2/3). Coefficients are in DSOUND's hardware form
     * (XboxDevWiki APU, DLS2): a cutoff as 4096 x log2(2 sin(pi f / 48000)),
     * i.e. log2 of the frequency coefficient of a state-variable filter, and
     * a damping in 1.15. Mode 1 (DLS2) runs a resonant two-pole low-pass;
     * a 3D (HRTF) voice also runs a one-pole low-pass with coefficient 3
     * (the I3DL2 direct path). */
    {
        uint32_t c18 = vc->snap[D_MENV_C / 4], c74 = vc->snap[0x74 / 4], c78 = vc->snap[0x78 / 4];
        int mode = (int)((c18 >> 16) & 3);
        int is3d = (vc->snap[D_HRTF / 4] & 0xFFFF) != 0xFFFF;
        int nch = fi.ch > 1 ? 2 : 1, c;
        if ((s->filt & 1) && (mode == 1 || mode == 3)) {
            /* cutoff from F = 2 sin(pi f / 48000); resonance from the 1.15
             * damping (0x8000 = 0 dB); a biquad, so it cannot run away */
            double F = pow(2.0, (int16_t)(c74 & 0xFFFF) / 4096.0);
            double q = (double)((c74 >> 16) & 0xFFFF) / 32768.0;
            if (F < 2.0 && F > 0.0 && (s->filt & 4 || !(F >= 0.999 && q >= 0.999))) {
                double f = asin(F / 2.0) / 3.14159265358979;    /* f / fs */
                double w0 = 2 * 3.14159265358979 * (f > 0.45 ? 0.45 : f);
                double Q = 0.70710678 / (q < 0.1 ? 0.1 : q);
                double al = sin(w0) / (2 * Q), cw = cos(w0), a0 = 1 + al;
                float b0 = (float)((1 - cw) / 2 / a0), b1 = (float)((1 - cw) / a0);
                float a1 = (float)(-2 * cw / a0), a2 = (float)((1 - al) / a0);
                for (c = 0; c < nch; c++) {
                    float z1 = vc->svf[c][0], z2 = vc->svf[c][1];
                    for (n = 0; n < MA_FRAME; n++) {
                        float x = src[n][c];
                        float y = b0 * x + z1;
                        z1 = b1 * x - a1 * y + z2;
                        z2 = b0 * x - a2 * y;
                        src[n][c] = y;
                    }
                    vc->svf[c][0] = z1; vc->svf[c][1] = z2;
                }
            }
        }
        if ((s->filt & 2) && is3d && (fmt & F_MULTIPASS)) {
            float F = (float)pow(2.0, (int16_t)(c78 >> 16) / 4096.0);
            float gdb = (float)(int16_t)(c78 & 0xFFFF);
            float gain = gdb < 0 ? (float)pow(2.0, gdb / 4096.0) : 1.0f;
            if (F > 1.0f) F = 1.0f;
            for (c = 0; c < nch; c++) {
                float y = vc->i3[c];
                for (n = 0; n < MA_FRAME; n++) {
                    y += F * (src[n][c] - y);
                    src[n][c] = y * gain;
                }
                vc->i3[c] = y;
            }
        }
        if (nch == 1)
            for (n = 0; n < MA_FRAME; n++)
                src[n][1] = src[n][0];
    }
    /* HRTF voices: left FIR to slots 0/1, right FIR to slots 2/3, the dry
     * mono signal to the rest */
    if (s->hrtf_on && (vc->snap[D_HRTF / 4] & 0xFFFF) != 0xFFFF) {
        float c[2][31], out[2][MA_FRAME];
        int itd, k;
        hrtf_coefs(s, (int)(vc->snap[D_HRTF / 4] & 0xFFFF), c, &itd);
        for (n = 0; n < MA_FRAME; n++) {
            float acc0 = 0, acc1 = 0;
            int dl = itd > 0 ? (itd + 1) / 2 : 0, dr = itd < 0 ? (-itd + 1) / 2 : 0;
            if (!s->hrtf_itd) dl = dr = 0;
            else if (s->hrtf_itd < 0) { int t2 = dl; dl = dr; dr = t2; }
            vc->fir_hist[vc->fir_pos] = src[n][0];
            for (k = 0; k < 31; k++) {
                acc0 += c[0][k] * vc->fir_hist[(vc->fir_pos - k - dl) & 127];
                acc1 += c[1][k] * vc->fir_hist[(vc->fir_pos - k - dr) & 127];
            }
            vc->fir_pos = (vc->fir_pos + 1) & 127;
            out[0][n] = acc0;
            out[1][n] = acc1;
        }
        for (i = 0; i < 8; i++) {
            float gg = g[i] * env;
            float *dst;
            if (gg == 0.0f)
                continue;
            dst = s->bin[sel[i]];
            for (n = 0; n < MA_FRAME; n++) {
                float y = gg * (i < 2 ? out[0][n] : i < 4 ? out[1][n] : s->hrtf_dry == 1 ? out[0][n] : s->hrtf_dry == 4 ? out[i & 1][n] : s->hrtf_dry == 2 ? 0.5f * (out[0][n] + out[1][n]) : s->hrtf_dry == 3 ? src[n][0] * s->hrtf_trim : src[n][0]);
                dst[n] += y;
                vc->energy += (double)y * y;
            }
        }
    } else {
        for (i = 0; i < 8; i++) {
            float gg = g[i] * env;
            float *dst;
            int ch = (fi.ch > 1) ? (i & 1) : 0;
            if (gg == 0.0f)
                continue;
            dst = s->bin[sel[i]];
            for (n = 0; n < MA_FRAME; n++) {
                dst[n] += gg * src[n][ch];
                vc->energy += (double)(gg * src[n][ch]) * (gg * src[n][ch]);
            }
        }
    }

    if (s->sat == 1) {
        for (i = 0; i < 8; i++) {
            float *dst = s->bin[sel[i]];
            for (n = 0; n < MA_FRAME; n++)
                dst[n] = dst[n] > 0.99999988f ? 0.99999988f : dst[n] < -1.0f ? -1.0f : dst[n];
        }
    }
done:
    vc->frames_played++;
    if (ended) {
        vc->state &= ~ST_ACTIVE;
        ma_notify(s, v, vc->ssl_cur);
        ma_voice_ended(s, v);
    }
    /* write-backs: state, position (+ envelope level) */
    ma_desc_set(s, v, D_STATE, vc->state);
    ma_desc_set(s, v, D_POS, vc->pos_word);
}

/* ---- one frame -------------------------------------------------------------- */

/* walks the three lists in order; pass 0 = every voice, pass 1 = all but the
 * multipass (bin-reading) voices, pass 2 = only those */
static void walk(ma_state *s, int pass)
{
    uint32_t *stamp = s->stamp, gen = ++s->gen;
    int list;
    for (list = 0; list < 3; list++) {
        uint32_t v = s->reg[R_LIST_TOP(list) / 4] & 0xFFFF;
        int guard = 0;
        while (v < MA_VOICES && guard++ < MA_VOICES) {
            uint32_t next;
            if (stamp[v] == gen)
                break;                          /* a loop in the links */
            stamp[v] = gen;
            next = ma_desc(s, (int)v, D_LINK) & 0xFFFF;
            if ((s->v[v].state & ST_ACTIVE) && !s->v[v].paused) {
                int mp = (ma_rd32(s, s->reg[R_DESC_BASE / 4] + v * 0x80u + D_FORMAT) & F_MULTIPASS) != 0;
                if (pass == 0 || (pass == 1 && !mp) || (pass == 2 && mp))
                    ma_render_voice(s, (int)v, list);
            }
            v = next;
        }
    }
}

void ma_process_frame(ma_state *s)
{
    tables();
    memset(s->bin, 0, sizeof s->bin);
    if (s->mp_mode == 1) {
        walk(s, 1);
        walk(s, 2);
    } else {
        walk(s, 0);
    }
    {
        /* an active voice no list reaches any more cannot play: the title
         * unlinked it (in a shadow, after the live model's end report) */
        int v;
        uint32_t g = s->gen;
        for (v = 0; v < MA_VOICES; v++)
            if ((s->v[v].state & ST_ACTIVE) && s->stamp[v] != g && s->stamp[v] + 8 < g)
                s->v[v].state &= ~ST_ACTIVE;
    }
    if (s->sat == 2) {
        int b, n;
        for (b = 0; b < MA_BINS; b++)
            for (n = 0; n < MA_FRAME; n++)
                s->bin[b][n] = s->bin[b][n] > 0.99999988f ? 0.99999988f : s->bin[b][n] < -1.0f ? -1.0f : s->bin[b][n];
    }
    s->frames++;
    s->sample_count += MA_FRAME;
}
