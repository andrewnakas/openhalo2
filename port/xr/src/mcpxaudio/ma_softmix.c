/* ma_softmix.c -- the small software mixer the toolkit's HLE DirectSound
 * (src/audio/dsound_device.c) plays through: 16-bit PCM voices, 16.16
 * resampling to 48 kHz, linear volume, mixed into the APU output. Halo 2 does
 * not use it. Original code. */
#include <string.h>
#include "ma_internal.h"

#define SM_VOICES 64

static APUMixerVoice s_v[SM_VOICES];

#ifdef MA_CONTRACT
int apu_mixer_alloc_voice(void)
{
    int i;
    for (i = 0; i < SM_VOICES; i++)
        if (!s_v[i].in_use) {
            memset(&s_v[i], 0, sizeof s_v[i]);
            s_v[i].in_use = 1;
            s_v[i].volume = 1.0f;
            s_v[i].num_channels = 1;
            s_v[i].sample_rate = 48000;
            return i;
        }
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot >= 0 && slot < SM_VOICES)
        memset(&s_v[slot], 0, sizeof s_v[slot]);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    return (slot >= 0 && slot < SM_VOICES && s_v[slot].in_use) ? &s_v[slot] : NULL;
}

void apu_mixer_play(int slot, int looping)
{
    APUMixerVoice *v = apu_mixer_get_voice(slot);
    if (v) { v->active = 1; v->looping = looping; v->frac = (uint64_t)(v->play_offset / (2u * (unsigned)(v->num_channels > 1 ? 2 : 1))) << 32; }
}

void apu_mixer_stop(int slot)
{
    APUMixerVoice *v = apu_mixer_get_voice(slot);
    if (v) v->active = 0;
}

int apu_mixer_set_position(int slot, uint32_t byte_pos)
{
    APUMixerVoice *v = apu_mixer_get_voice(slot);
    if (!v) return -1;
    v->play_offset = byte_pos;
    v->frac = (uint64_t)(byte_pos / (2u * (unsigned)(v->num_channels > 1 ? 2 : 1))) << 32;
    return 0;
}

void apu_mixer_get_state(int slot, uint32_t *cursor, int *active, int *looping)
{
    APUMixerVoice *v = apu_mixer_get_voice(slot);
    if (cursor)  *cursor = v ? v->play_offset : 0;
    if (active)  *active = v ? v->active : 0;
    if (looping) *looping = v ? v->looping : 0;
}
#endif

void ma_softmix_render(float *lr, int frames)
{
    int i, n;
    for (i = 0; i < SM_VOICES; i++) {
        APUMixerVoice *v = &s_v[i];
        int ch;
        uint64_t step, total;
        if (!v->in_use || !v->active || !v->pcm_data || !v->sample_rate)
            continue;
        ch = v->num_channels > 1 ? 2 : 1;
        total = v->pcm_bytes / (2u * (unsigned)ch);
        if (!total) { v->active = 0; continue; }
        step = ((uint64_t)v->sample_rate << 32) / 48000u;
        for (n = 0; n < frames; n++) {
            uint64_t idx = v->frac >> 32;
            float l, r;
            if (idx >= total) {
                if (!v->looping) { v->active = 0; break; }
                v->frac -= total << 32;
                idx = v->frac >> 32;
            }
            l = v->pcm_data[idx * ch] * (1.0f / 32768.0f);
            r = ch > 1 ? v->pcm_data[idx * ch + 1] * (1.0f / 32768.0f) : l;
            lr[2 * n] += l * v->volume;
            lr[2 * n + 1] += r * v->volume;
            v->frac += step;
        }
        v->play_offset = (uint32_t)((v->frac >> 32) * 2u * (unsigned)ch);
    }
}
