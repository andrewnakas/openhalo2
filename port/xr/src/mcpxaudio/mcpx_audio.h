/* mcpx_audio.h -- a clean-room model of the MCPX audio processor (APU).
 *
 * Written from docs/APU_FACTS.md only: public documentation, the title's own
 * code and black-box measurement of the previous model. See
 * docs/APU_CLEANROOM_DESIGN.md for the contract this satisfies.
 *
 * Two ways to use it:
 *  - live: ma_create(MA_LIVE) owns the device. It answers the title's register
 *    reads, writes guest RAM (descriptor write-backs, notifiers, tables),
 *    delivers the interrupt and feeds the host output.
 *  - shadow: ma_create(MA_SHADOW) sees the same register writes as another
 *    model that is live, reads the same guest RAM, and never writes guest RAM,
 *    raises an interrupt or opens a device. Its mix goes to its own WAV tap,
 *    so the two can be compared sample for sample (tools/apu_ab.py).
 *
 * The contract symbols the port links against (g_apu_state,
 * mcpx_apu_init_standalone, mcpx_apu_mmio_read/write, mcpx_apu_shutdown,
 * apu_hook_handle_mmio, apu_mixer_*) are thin shims in ma_contract.c.
 */
#ifndef MCPX_AUDIO_H
#define MCPX_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ma_state ma_state;

enum { MA_LIVE = 1, MA_SHADOW = 2 };

/* Creates the model and starts its frame thread. In live mode the host
 * output opens unless RECOMP_APU_MUTE=1. NULL on failure. */
ma_state *ma_create(int mode);
void      ma_destroy(ma_state *s);

/* Register access, off = VA - 0xFE800000, 0 .. 0x2FFFF, size 1, 2 or 4. */
uint32_t  ma_read(ma_state *s, uint32_t off, int size);
void      ma_write(ma_state *s, uint32_t off, uint32_t val, int size);

/* Lets a shadow model answer reads too (the last stage of the comparison). */
void      ma_set_mode(ma_state *s, int mode);

/* One-line totals on stderr ([MA] lines). */
void      ma_report(ma_state *s);

/* ---- software mixer used by the toolkit's HLE DirectSound ---------------- */
/* (Halo 2 does not use it; it has to link.) */
typedef struct APUMixerVoice {
    const int16_t *pcm_data;
    uint32_t       pcm_bytes;
    uint32_t       play_offset;     /* bytes */
    int            num_channels;
    uint32_t       sample_rate;
    float          volume;          /* linear */
    /* ours */
    int            active, looping, in_use;
    uint64_t       frac;            /* 32.32 position in frames */
} APUMixerVoice;

int            apu_mixer_alloc_voice(void);
void           apu_mixer_free_voice(int slot);
APUMixerVoice *apu_mixer_get_voice(int slot);
void           apu_mixer_play(int slot, int looping);
void           apu_mixer_stop(int slot);
int            apu_mixer_set_position(int slot, uint32_t byte_pos);
void           apu_mixer_get_state(int slot, uint32_t *cursor, int *active, int *looping);
/* Adds the software voices into a 48 kHz stereo float block (internal). */
void           ma_softmix_render(float *lr, int frames);

#ifdef __cplusplus
}
#endif

#endif
