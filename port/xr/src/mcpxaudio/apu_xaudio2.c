/**
 * XAudio2 Audio Output Backend
 *
 * The host end of the APU: 48 kHz stereo 16-bit blocks from the frame thread
 * go to one XAudio2 source voice. The frame thread is paced by the wall
 * clock and the device by its own crystal, so the queue between them is
 * kept shallow and bounded: a block that would deepen it past the limit is
 * dropped, and an empty queue is refilled with a short cushion of silence
 * before the next block. Both are counted.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   256    /* one APU block, 5.33 ms */
#define XA2_NUM_BUFS      32
#define XA2_MAX_QUEUED    16     /* 85 ms: past this a block is dropped */
#define XA2_CUSHION       4      /* blocks of silence after running dry */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static unsigned                g_xa2_blocks, g_xa2_underruns, g_xa2_drops;
static int                     g_xa2_last_queued;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d-sample blocks,"
                    " queue limit %d)\n", XA2_SAMPLE_RATE, XA2_BUF_SAMPLES,
            XA2_MAX_QUEUED);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%u blocks written)\n", g_xa2_blocks);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

static int xa2_queue_one(const int16_t *samples, int num_samples)
{
    XAUDIO2_BUFFER xbuf;
    int idx = g_xa2_next_buf;

    if (num_samples > XA2_BUF_SAMPLES)
        num_samples = XA2_BUF_SAMPLES;
    if (samples)
        memcpy(g_xa2_bufs[idx], samples,
               (size_t)num_samples * XA2_CHANNELS * sizeof(int16_t));
    else
        memset(g_xa2_bufs[idx], 0,
               (size_t)num_samples * XA2_CHANNELS * sizeof(int16_t));
    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = (UINT32)(num_samples * XA2_CHANNELS * sizeof(int16_t));
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
    if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL)))
        return 0;
    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    return 1;
}

/* Submit one block of mixed samples. Called from the APU frame thread.
 * Returns 1 if the block was queued, 0 if it was dropped. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    int queued, k;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    queued = (int)state.BuffersQueued;
    g_xa2_last_queued = queued;
    if (queued >= XA2_MAX_QUEUED) {
        g_xa2_drops++;
        return 0;
    }
    if (queued == 0) {
        /* Dry: the device has been playing nothing. Not counted before the
         * first block, when it is simply the start. */
        if (g_xa2_blocks)
            g_xa2_underruns++;
        for (k = 0; k < XA2_CUSHION; k++)
            xa2_queue_one(NULL, XA2_BUF_SAMPLES);
    }
    if (!xa2_queue_one(samples, num_samples))
        return 0;
    g_xa2_blocks++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

void xa2_get_stats(unsigned *blocks, unsigned *underruns, unsigned *drops,
                   int *queued)
{
    *blocks = g_xa2_blocks;
    *underruns = g_xa2_underruns;
    *drops = g_xa2_drops;
    *queued = g_xa2_last_queued;
}

#elif defined(__EMSCRIPTEN__)

/* ============================================================
 * Web: a ring in shared memory, read by an AudioWorklet
 * ============================================================
 *
 * The APU frame thread writes 48 kHz stereo frames into a ring that lives in
 * the program's (shared) memory; the page's AudioWorklet processor reads the
 * same bytes on the audio thread (port/web/audio.js). One writer, one reader,
 * two counters: no lock and no trip through the browser's main thread.
 *
 * Header (little-endian uint32 each), then the int16 frames:
 *   +0  frames written (wraps)      +4  frames read (wraps)
 *   +8  capacity in frames          +12 underruns counted by the reader
 */
#include <stdatomic.h>
#include <string.h>
#include <emscripten.h>

#define WEB_RING_FRAMES 16384               /* 341 ms; a power of two */
#define WEB_MAX_QUEUED  (256 * 20)          /* as XA2_MAX_QUEUED blocks: drop beyond 107 ms */

static struct {
    _Atomic uint32_t written, read;
    uint32_t capacity;
    _Atomic uint32_t underruns;
    int16_t  frames[WEB_RING_FRAMES * 2];
} s_ring = { .capacity = WEB_RING_FRAMES };

static int s_on;
static unsigned s_blocks, s_drops;
static int s_last_queued;

EMSCRIPTEN_KEEPALIVE void *xa2_web_ring(void)
{
    return &s_ring;
}

int xa2_init(void)
{
    s_on = 1;
    return 1;
}

void xa2_shutdown(void)
{
    s_on = 0;
}

int xa2_is_active(void)
{
    return s_on;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    uint32_t w, queued, i;

    if (!s_on || num_samples <= 0)
        return 0;
    w = atomic_load(&s_ring.written);
    queued = w - atomic_load(&s_ring.read);
    s_last_queued = (int)(queued / 256);
    if (queued + (uint32_t)num_samples > WEB_MAX_QUEUED) {
        s_drops++;                          /* nobody is listening, or far behind */
        return 0;
    }
    for (i = 0; i < (uint32_t)num_samples; i++) {
        uint32_t at = (w + i) & (WEB_RING_FRAMES - 1);
        s_ring.frames[at * 2 + 0] = samples ? samples[i * 2 + 0] : 0;
        s_ring.frames[at * 2 + 1] = samples ? samples[i * 2 + 1] : 0;
    }
    atomic_store(&s_ring.written, w + (uint32_t)num_samples);
    s_blocks++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return 256;
}

void xa2_get_stats(unsigned *blocks, unsigned *underruns, unsigned *drops,
                   int *queued)
{
    *blocks = s_blocks;
    *underruns = atomic_load(&s_ring.underruns);
    *drops = s_drops;
    *queued = s_last_queued;
}

#else /* neither Windows nor the web: stubs (no audio output) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }
void xa2_get_stats(unsigned *b, unsigned *u, unsigned *d, int *q)
{ *b = 0; *u = 0; *d = 0; *q = 0; }

#endif /* _WIN32 */
