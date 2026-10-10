/* Checked device-register access for lifted hardware code.
 *
 * h2lift/mmio_pass.py rewrites the memory accesses of the title's hardware
 * libraries (Direct3D, DirectSound, XNet, the USB stack) into these forms.
 * An address at or above RECOMP_MMIO_BASE goes to the device models through
 * recomp_mmio_read / recomp_mmio_write (mmio_direct.c); anything below is the
 * plain access. No page fault and no instruction decoder is involved, so the
 * same code runs on 32-bit hosts and in WebAssembly.
 */
#ifndef RECOMP_MMIO_H
#define RECOMP_MMIO_H

#include <stdint.h>
#include <string.h>

#define RECOMP_MMIO_BASE 0xFD000000u

uint32_t recomp_mmio_read(uint32_t va, int size);
void     recomp_mmio_write(uint32_t va, uint32_t val, int size);

#define RECOMP_MMIO_RW(bits, type)                                              \
static inline type recomp_mr##bits(uint32_t a)                                  \
{                                                                               \
    if (a >= RECOMP_MMIO_BASE) return (type)recomp_mmio_read(a, bits / 8);      \
    return MEM##bits(a);                                                        \
}                                                                               \
static inline type recomp_mw##bits(uint32_t a, type v)                          \
{                                                                               \
    if (a >= RECOMP_MMIO_BASE) recomp_mmio_write(a, v, bits / 8);               \
    else MEM##bits(a) = v;                                                      \
    return v;                                                                   \
}

RECOMP_MMIO_RW(8, uint8_t)
RECOMP_MMIO_RW(16, uint16_t)
RECOMP_MMIO_RW(32, uint32_t)

static inline float recomp_mrf(uint32_t a)
{
    uint32_t u = recomp_mr32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline float recomp_mwf(uint32_t a, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    recomp_mw32(a, u);
    return f;
}

static inline uint64_t recomp_mr64(uint32_t a)
{
    return (uint64_t)recomp_mr32(a) | ((uint64_t)recomp_mr32(a + 4) << 32);
}

static inline uint64_t recomp_mw64(uint32_t a, uint64_t v)
{
    recomp_mw32(a, (uint32_t)v);
    recomp_mw32(a + 4, (uint32_t)(v >> 32));
    return v;
}

static inline double recomp_mrd(uint32_t a)
{
    uint64_t u = recomp_mr64(a);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static inline double recomp_mwd(uint32_t a, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    recomp_mw64(a, u);
    return d;
}

#define MEMR8(a)    recomp_mr8((uint32_t)(a))
#define MEMR16(a)   recomp_mr16((uint32_t)(a))
#define MEMR32(a)   recomp_mr32((uint32_t)(a))
#define MEMR64(a)   recomp_mr64((uint32_t)(a))
#define SMEMR8(a)   ((int8_t)recomp_mr8((uint32_t)(a)))
#define SMEMR16(a)  ((int16_t)recomp_mr16((uint32_t)(a)))
#define SMEMR32(a)  ((int32_t)recomp_mr32((uint32_t)(a)))
#define SMEMR64(a)  ((int64_t)recomp_mr64((uint32_t)(a)))
#define MEMRF(a)    recomp_mrf((uint32_t)(a))
#define MEMRD(a)    recomp_mrd((uint32_t)(a))

#define MEMW8(a, v)   recomp_mw8((uint32_t)(a), (uint8_t)(v))
#define MEMW16(a, v)  recomp_mw16((uint32_t)(a), (uint16_t)(v))
#define MEMW32(a, v)  recomp_mw32((uint32_t)(a), (uint32_t)(v))
#define MEMW64(a, v)  recomp_mw64((uint32_t)(a), (uint64_t)(v))
#define MEMWF(a, v)   recomp_mwf((uint32_t)(a), (float)(v))
#define MEMWD(a, v)   recomp_mwd((uint32_t)(a), (double)(v))

#endif /* RECOMP_MMIO_H */
