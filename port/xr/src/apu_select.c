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
volatile int recomp_apu_on;

/* Called once, where main.c used to call mcpx_apu_init_standalone. */
int recomp_apu_start(uint8_t *ram_ptr)
{
    const char *e = getenv("RECOMP_APU_IMPL");
    const char *impl = (e && *e) ? e : APU_DEFAULT_IMPL;

#ifdef OPENHALO2_OLD_APU
    if (!strcmp(impl, "old") || !strcmp(impl, "shadow")) {
        g_apu_state = mcpx_apu_init_standalone(ram_ptr);
        s_old_live = g_apu_state != NULL;
        if (!strcmp(impl, "shadow") && s_old_live)
            s_new = ma_create(MA_SHADOW);
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
        if (s_new)
            ma_write(s_new, off, val, size);
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
