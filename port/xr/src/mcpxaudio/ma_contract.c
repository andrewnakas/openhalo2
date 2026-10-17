/* ma_contract.c -- the symbols the port and the toolkit link against
 * (docs/APU_CLEANROOM_DESIGN.md section 1), as thin shims over the ma_ model,
 * plus the page-fault fallback (decoded with platform/mmio_decode.h, as
 * usb/ohci.c does).
 *
 * The contract names are only defined with MA_CONTRACT, i.e. once the
 * previous model is no longer linked; until then the port reaches the model
 * through ma_* directly (port/xr/src/apu_select.c). */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include "platform/win32_compat.h"
#endif
#include "platform/mmio_decode.h"
#include "ma_internal.h"

static ma_state *s_fault_dev;

static uint64_t fault_rd(void *dev, uint32_t off, int size)
{
    return ma_read((ma_state *)dev, off, size);
}

static void fault_wr(void *dev, uint32_t off, uint64_t val, int size)
{
    ma_write((ma_state *)dev, off, (uint32_t)val, size);
}

/* x64 page-fault route: decode, service, step over. Only the register span
 * 0xFE800000-0xFE82FFFF is the model's. */
bool ma_hook_handle_mmio(ma_state *s, PCONTEXT ctx, uintptr_t fault_addr,
                         uint32_t xbox_va, int is_write)
{
    (void)fault_addr; (void)is_write;
    if (!s || xbox_va < 0xFE800000u || xbox_va >= 0xFE830000u)
        return false;
    s_fault_dev = s;
    return mmio_emulate(ctx, xbox_va - 0xFE800000u, s, fault_rd, fault_wr) != 0;
}

#ifdef MA_CONTRACT
/* Opaque to every caller (they only forward-declare it). */
struct MCPXAPUState { ma_state *m; };
struct MCPXAPUState *g_apu_state;
static struct MCPXAPUState s_contract;

struct MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    (void)ram_ptr;                      /* the model addresses RAM through the
                                         * contiguous window itself */
    s_contract.m = ma_create(MA_LIVE);
    return s_contract.m ? &s_contract : NULL;
}

uint64_t mcpx_apu_mmio_read(struct MCPXAPUState *d, uint64_t off, unsigned size)
{
    return d ? ma_read(d->m, (uint32_t)off, (int)size) : 0;
}

void mcpx_apu_mmio_write(struct MCPXAPUState *d, uint64_t off, uint64_t val, unsigned size)
{
    if (d)
        ma_write(d->m, (uint32_t)off, (uint32_t)val, (int)size);
}

void mcpx_apu_shutdown(struct MCPXAPUState *d)
{
    if (d && d->m) {
        ma_destroy(d->m);
        d->m = NULL;
    }
}

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr, uint32_t xbox_va, int is_write)
{
    return g_apu_state && ma_hook_handle_mmio(g_apu_state->m, ctx, fault_addr, xbox_va, is_write);
}
#endif
