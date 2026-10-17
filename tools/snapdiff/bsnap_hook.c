/* ---- bsnap: private Bink differential snapshots (not for the repo) ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern size_t g_xbox_map_size; extern size_t g_xbox_total_ram;
static void bsnap_write(const char *tag, uint32_t va, unsigned n)
{
    char path[512]; FILE *f; const char *dir = getenv("RECOMP_BSNAP_DIR");
    uint32_t hdr[16]; size_t sz = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;
    if (!dir) dir = ".";
    snprintf(path, sizeof path, "%s/bsnap_%08X_%u_%s.bin", dir, va, n, tag);
    f = fopen(path, "wb"); if (!f) return;
    hdr[0] = 0x50534E42u; hdr[1] = va; hdr[2] = (uint32_t)sz;
    hdr[3] = g_eax; hdr[4] = g_ecx; hdr[5] = g_edx; hdr[6] = g_ebx; hdr[7] = g_esp;
    hdr[8] = g_ebp; hdr[9] = g_seh_ebp; hdr[10] = g_esi; hdr[11] = g_edi;
    hdr[12] = g_recomp_eflags; hdr[13] = (uint32_t)g_df; hdr[14] = (uint32_t)g_fp_top;
    hdr[15] = g_fp_control_word;
    fwrite(hdr, 4, 16, f);
    fwrite(g_fp_stack, 8, 8, f);
    { RecompMmx m[8] = { g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7 }; fwrite(m, 8, 8, f); }
    { RecompXmm x[8] = { g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7 }; fwrite(x, 16, 8, f); }
    fwrite((const void *)(uintptr_t)g_xbox_mem_offset, 1, sz, f);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x80000000u), 1, 64u << 20, f);  /* contiguous window */
    fclose(f);
    fprintf(stderr, "[BSNAP] %s\n", path);
}
#define BSNAP_WRAP(VA) \
void sub_##VA(void) { \
    static unsigned n; static int lo = -2, hi = -1, outs = 0; unsigned k = ++n; int hit; \
    if (lo == -2) { const char *e = getenv("RECOMP_BSNAP_" #VA); const char *d; lo = e ? atoi(e) : -1; hi = lo; \
        if (e && (d = strchr(e, '-')) != 0) hi = atoi(d + 1); outs = getenv("RECOMP_BSNAP_OUT") != 0; } \
    hit = (int)k >= lo && (int)k <= hi && (getenv("RECOMP_BSNAP_STEP") == 0 || ((int)k - lo) % atoi(getenv("RECOMP_BSNAP_STEP")) == 0); \
    if (hit) bsnap_write("in", 0x##VA##u, k); \
    sub_##VA##__body(); \
    if (hit && outs) bsnap_write("out", 0x##VA##u, k); \
}
