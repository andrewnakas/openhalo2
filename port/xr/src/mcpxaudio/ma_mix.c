/* ma_mix.c -- the output stage of the clean-room APU model: fold the 32
 * mixbins down to stereo, apply the gain, the reverb return and the limiter,
 * and feed the WAV tap and the host device.
 *
 * Mixbin layout (DirectSound's public numbering): 0-5 front left, front
 * right, centre, LFE, back left, back right; 6-9 the 3D outputs (6/8 left,
 * 7/9 right, from HRTF_BINS 0x09070806); 10 the I3DL2 send; the rest are
 * effect sends. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ma_internal.h"
#include "apu_xaudio2.h"

#define env_float(n, d) env_float_(s, n, d)
static float env_float_(ma_state *s, const char *n, float d)
{
    const char *e = ma_getenv(s, n);
    return (e && *e) ? (float)atof(e) : d;
}

void ma_mix_init(ma_state *s)
{
    const char *e;
    int b;
    float c = env_float("RECOMP_MA_FOLD_C", 0.7071f);
    float r = env_float("RECOMP_MA_FOLD_REAR", 1.0f);
    float d3 = env_float("RECOMP_MA_FOLD_3D", 1.0f);
    float lfe = env_float("RECOMP_MA_FOLD_LFE", 0.0f);

    memset(s->fold, 0, sizeof s->fold);
    s->fold[0][0] = 1;  s->fold[1][1] = 1;
    s->fold[2][0] = c;  s->fold[2][1] = c;
    s->fold[3][0] = lfe; s->fold[3][1] = lfe;
    s->fold[4][0] = r;  s->fold[5][1] = r;
    s->fold[6][0] = d3; s->fold[7][1] = d3;
    s->fold[8][0] = d3; s->fold[9][1] = d3;
    /* RECOMP_MA_FOLD=b:l:r,b:l:r overrides single bins */
    e = ma_getenv(s, "RECOMP_MA_FOLD");
    while (e && *e) {
        float l, rr;
        if (sscanf(e, "%d:%f:%f", &b, &l, &rr) == 3 && b >= 0 && b < MA_BINS) {
            s->fold[b][0] = l;
            s->fold[b][1] = rr;
        }
        e = strchr(e, ',');
        if (e) e++;
    }
    s->gain = (float)pow(10.0, env_float("RECOMP_APU_GAIN_DB", 0.0f) / 20.0);
    e = ma_getenv(s, "RECOMP_MA_BINDUMP");
    if (e && *e)
        s->bindump = fopen(e, "wb");
    e = ma_getenv(s, "RECOMP_APU_LIMIT");
    s->limit_mode = !e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "tanh") ? 2 : 1;
    s->lim.gain = 1.0f;
    e = ma_getenv(s, "RECOMP_APU_REVERB");
    s->reverb_on = e ? atoi(e) : s->mode == MA_LIVE;   /* an addition: on when live, off in a shadow (dry comparison) */
    if (s->reverb_on)
        s->rv = ma_reverb_create();
}

/* bins -> 32 stereo frames (float, 1.0 = 16-bit full scale) */
void ma_mix_frame(ma_state *s, float *lr)
{
    int b, n;
    memset(lr, 0, sizeof(float) * 2 * MA_FRAME);
    for (b = 0; b < MA_BINS; b++) {
        float gl = s->fold[b][0], gr = s->fold[b][1];
        const float *x = s->bin[b];
        if (s->report_bins || s->bindump) {
            double e = 0;
            for (n = 0; n < MA_FRAME; n++)
                e += (double)x[n] * x[n];
            s->bin_energy[b] += e;
            s->dump_acc[b] += (float)e;
        }
        if (gl == 0.0f && gr == 0.0f)
            continue;
        for (n = 0; n < MA_FRAME; n++) {
            lr[2 * n] += gl * x[n];
            lr[2 * n + 1] += gr * x[n];
        }
    }
    if (s->rv)
        ma_reverb_process(s->rv, s->bin[10], lr, MA_FRAME);
    s->bin_energy_n += MA_FRAME;
    /* RECOMP_MA_BINDUMP=<file>: per 256-sample block, 32 floats = each bin's
     * sum of squares (1.0 = 16-bit full scale), for tools/apu2/binab2.py */
    if (s->bindump && ++s->dump_frames == MA_BLOCK / MA_FRAME) {
        fwrite(s->dump_acc, sizeof(float), MA_BINS, s->bindump);
        memset(s->dump_acc, 0, sizeof s->dump_acc);
        s->dump_frames = 0;
    }
}

/* ---- WAV tap ---------------------------------------------------------------- */

static void wav_header(FILE *f, uint32_t frames)
{
    uint8_t h[44];
    uint32_t data = frames * 4u;
    memcpy(h, "RIFF", 4);
    h[4] = (uint8_t)(data + 36); h[5] = (uint8_t)((data + 36) >> 8);
    h[6] = (uint8_t)((data + 36) >> 16); h[7] = (uint8_t)((data + 36) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = 0x80; h[25] = 0xBB; h[26] = 0; h[27] = 0;          /* 48000 */
    h[28] = 0x00; h[29] = 0xEE; h[30] = 0x02; h[31] = 0;        /* 192000 */
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)data; h[41] = (uint8_t)(data >> 8);
    h[42] = (uint8_t)(data >> 16); h[43] = (uint8_t)(data >> 24);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

static void tap_open(ma_state *s)
{
    const char *e = ma_getenv(s, s->mode == MA_LIVE ? "RECOMP_APU_WAV" : "RECOMP_MA_WAV");
    char path[512];
    const char *c;
    size_t n;

    if (s->tap_tried || !e || !*e)
        return;
    s->tap_tried = 1;
    c = strchr(e, ',');
    n = c ? (size_t)(c - e) : strlen(e);
    if (n >= sizeof path) n = sizeof path - 1;
    memcpy(path, e, n); path[n] = 0;
    s->tap.f = fopen(path, "wb");
    s->tap.limit = c ? (uint32_t)(atof(c + 1) * MA_RATE) : 0xFFFFFFFFu;
    s->tap.from_start = getenv("RECOMP_APU_WAV_FROM_START") != NULL;
    if (s->tap.f)
        wav_header(s->tap.f, 0);
    fprintf(stderr, "[MA] WAV tap %s: %s\n", path, s->tap.f ? "recording" : "cannot open");
}

static void tap_write(ma_state *s, const int16_t *pcm, int frames, int silent)
{
    if (!s->tap.f)
        return;
    if (!s->tap.frames && silent && !s->tap.from_start)
        return;
    if (s->tap.frames + (uint32_t)frames > s->tap.limit)
        frames = (int)(s->tap.limit - s->tap.frames);
    if (frames <= 0) {
        if (s->tap.f) {
            wav_header(s->tap.f, s->tap.frames);
            fclose(s->tap.f);
            s->tap.f = NULL;
            fprintf(stderr, "[MA] WAV tap closed at %.1f s\n", s->tap.frames / (double)MA_RATE);
        }
        return;
    }
    fwrite(pcm, 4, (size_t)frames, s->tap.f);
    s->tap.frames += (uint32_t)frames;
    if ((s->tap.frames & 0xFFFF) < (uint32_t)frames)
        wav_header(s->tap.f, s->tap.frames);   /* keep the file readable */
}

void ma_tap_close(ma_state *s)
{
    if (s->tap.f) {
        wav_header(s->tap.f, s->tap.frames);
        fclose(s->tap.f);
        s->tap.f = NULL;
    }
}

/* ---- block output ------------------------------------------------------------- */

void ma_output_block(ma_state *s, const float *lr_in, int frames)
{
    float lr[2 * MA_BLOCK];
    int16_t pcm[2 * MA_BLOCK];
    int i, silent = 1;

    memcpy(lr, lr_in, sizeof(float) * 2 * (size_t)frames);
    ma_softmix_render(lr, frames);
    for (i = 0; i < 2 * frames; i++)
        lr[i] *= s->gain;
    if (s->limit_mode == 1)
        ma_limiter_process(&s->lim, lr, frames);
    for (i = 0; i < 2 * frames; i++) {
        float x = lr[i];
        int y;
        if (s->limit_mode == 2)
            x = x > 0.7f ? 0.7f + 0.3f * (float)tanh((x - 0.7f) / 0.3f)
              : x < -0.7f ? -0.7f + 0.3f * (float)tanh((x + 0.7f) / 0.3f) : x;
        y = (int)lrintf(x * 32768.0f);
        if (y > 32767) { y = 32767; s->st_clipped++; }
        if (y < -32768) { y = -32768; s->st_clipped++; }
        pcm[i] = (int16_t)y;
        if (y) silent = 0;
    }
    if (!s->tap.f)
        tap_open(s);
    tap_write(s, pcm, frames, silent);
    if (s->device)
        xa2_submit_samples(pcm, frames);
    s->st_blocks++;
}
