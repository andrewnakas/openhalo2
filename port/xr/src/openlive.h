/* Open Live (docs/OPEN_LIVE_DESIGN.md): shared declarations of openlive.c
 * (hook, trace, guest helpers) and the host side of the XOnline API. */
#ifndef OPENLIVE_H
#define OPENLIVE_H

#include <stdint.h>

#define XO_MODE_TRACE 1
#define XO_MODE_LIVE  2

extern int g_xo_mode;

/* the wrapper target (h2lift/xo_hle_pass.py) */
void xo_hook(uint32_t va, void (*lifted)(void));

/* guest helpers, for handlers running on the calling guest thread */
uint8_t *xo_ptr(uint32_t va);
uint32_t xo_rd32(uint32_t va);
void     xo_wr32(uint32_t va, uint32_t v);
uint32_t xo_arg(int i);                       /* i-th stdcall argument */
void     xo_return(uint32_t eax, int nargs);  /* set eax, pop 4 + 4 nargs */
const char *xo_name(uint32_t va);

/* openlive_api.c */
void xo_live_init(void);
int  xo_live_call(uint32_t va);               /* 1 = answered (returned to the caller) */
void xo_trace_detail(uint32_t va, const uint32_t *args, uint32_t eax);

#endif
