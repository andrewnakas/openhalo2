/* fecheck_rt.c: runtime facts + state snapshots for the decomp-assist bench
 * (docs/DECOMP_ASSIST_PLAN.md, P1/P2). Compiled only with -DRECOMP_FECHECK
 * (CMake -DOPENHALO2_FECHECK=ON, which also points the build at a private
 * gen copy post-passed by h2lift/fe_pass.py). Without the define this file
 * is empty and RECOMP_FN_ENTER expands to nothing, so release builds are
 * unchanged.
 *
 * Every game function starts with RECOMP_FN_ENTER(idx, va) -> recomp_fe_enter.
 * Per function it keeps: call count, up to FE_NCALLER distinct return
 * addresses (the call sites; indirect-call targets fall out of this offline,
 * since the site's instruction is known) and FE_NSAMP entry samples at calls
 * 1,2,3,10,100,1e3,1e4,1e5 (registers + 8 stack dwords + return address).
 *
 * Environment:
 *   RECOMP_FE_OUT=<file>         binary table, rewritten every 15 s (and at exit)
 *   RECOMP_FE_SNAP=<file>        VAs (hex, one per line) to snapshot at entry
 *   RECOMP_FE_SNAP_DIR=<dir>     snapshot store (pages.dat + s/<VA>_<n>.fes)
 *   RECOMP_FE_SNAP_K=<n>         snapshots per function (default 4)
 *   RECOMP_FE_SNAP_GAP_MS=<ms>   minimum time between two of one function (20000)
 *   RECOMP_FE_SNAP_MAX=<n>       snapshots per run (default 3000)
 *   RECOMP_FE_SNAP_SKIP=<n>      skip a function's first n calls (default 0)
 *
 * Snapshot store: pages are 4 KB, deduplicated by a 128-bit content hash over
 * the whole run, appended to pages.dat; a .fes file is a header (registers,
 * x87, MMX, XMM, sizes) followed by one uint32 page number per page of guest
 * RAM and then of the 64 MB contiguous window (0xFFFFFFFF = zero page).
 * tools/fecheck/snapstore.py reads them. Snapshots hold retail data: keep the
 * store outside every repository. */
#ifdef RECOMP_FECHECK
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RECOMP_GENERATED_CODE   /* for RecompMmx and the register globals */
#include "recomp_types.h"
#undef eax
#undef ecx
#undef edx
#undef esp
#undef ebx
#undef esi
#undef edi

extern size_t g_xbox_map_size; extern size_t g_xbox_total_ram;
extern const uint32_t g_fe_table_va[];
extern const uint32_t g_fe_table_n;

#define FE_NCALLER 8
#define FE_NSAMP 8
#define FE_SAMPW 16
typedef struct {
    volatile uint64_t count;
    uint32_t callers[FE_NCALLER];
    uint32_t caller_hits[FE_NCALLER];
    uint32_t nsamp;
    uint32_t samp[FE_NSAMP][FE_SAMPW];
    /* snapshots */
    uint32_t snaps;
    uint32_t last_snap_ms;
    uint8_t want;
} fe_rec_t;

static fe_rec_t *g_fe;
static volatile LONG g_fe_init_state;  /* 0 none, 1 initialising, 2 ready */
static const char *g_fe_out;
static CRITICAL_SECTION g_fe_snap_lock;
static int g_fe_snap_on, g_fe_snap_k = 4, g_fe_snap_gap = 20000, g_fe_snap_max = 3000, g_fe_snap_skip;
static volatile LONG g_fe_snap_total, g_fe_snap_busy;
static char g_fe_snap_dir[512];
static FILE *g_fe_pages;
static uint32_t g_fe_npages;
static uint32_t g_fe_t0;

/* page hash table: open addressing, key = 2x64-bit hash, value = page number */
#define FE_HBITS 21
typedef struct { uint64_t a, b; uint32_t page; uint32_t used; } fe_hent_t;
static fe_hent_t *g_fe_hash;
static uint32_t g_fe_hcount;

static void fe_dump(void)
{
    char tmp[600]; FILE *f; uint32_t i, magic = 0x31544546u /* FET1 */, n = g_fe_table_n;
    if (!g_fe_out || !g_fe) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_fe_out);
    f = fopen(tmp, "wb"); if (!f) return;
    fwrite(&magic, 4, 1, f); fwrite(&n, 4, 1, f);
    { uint32_t w[2] = { FE_NCALLER, FE_NSAMP }; fwrite(w, 4, 2, f); }
    for (i = 0; i < n; i++) {
        fe_rec_t *r = &g_fe[i]; uint64_t c = r->count;
        fwrite(&g_fe_table_va[i], 4, 1, f); fwrite(&c, 8, 1, f);
        fwrite(r->callers, 4, FE_NCALLER, f); fwrite(r->caller_hits, 4, FE_NCALLER, f);
        fwrite(&r->nsamp, 4, 1, f); fwrite(r->samp, 4, FE_NSAMP * FE_SAMPW, f);
        fwrite(&r->snaps, 4, 1, f);
    }
    fclose(f);
    MoveFileExA(tmp, g_fe_out, MOVEFILE_REPLACE_EXISTING);
}

static DWORD WINAPI fe_dump_thread(LPVOID p)
{
    (void)p;
    for (;;) { Sleep(15000); fe_dump(); }
    return 0;
}

static void fe_atexit(void) { fe_dump(); if (g_fe_pages) fflush(g_fe_pages); }

static void fe_load_snap_list(const char *path)
{
    FILE *f = fopen(path, "r"); char line[128]; uint32_t i, n = g_fe_table_n, hits = 0;
    if (!f) { fprintf(stderr, "[FE] cannot read %s\n", path); return; }
    while (fgets(line, sizeof line, f)) {
        uint32_t va = (uint32_t)strtoul(line, 0, 16), lo = 0, hi = n;
        if (!va) continue;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_fe_table_va[m] < va) lo = m + 1; else hi = m; }
        if (lo < n && g_fe_table_va[lo] == va) { g_fe[lo].want = 1; hits++; }
    }
    fclose(f);
    fprintf(stderr, "[FE] snapshot list: %u functions\n", hits);
    (void)i;
}

static void fe_init(void)
{
    const char *e;
    g_fe = (fe_rec_t *)calloc(g_fe_table_n, sizeof(fe_rec_t));
    g_fe_out = getenv("RECOMP_FE_OUT");
    g_fe_t0 = GetTickCount();
    InitializeCriticalSection(&g_fe_snap_lock);
    if ((e = getenv("RECOMP_FE_SNAP_K"))) g_fe_snap_k = atoi(e);
    if ((e = getenv("RECOMP_FE_SNAP_GAP_MS"))) g_fe_snap_gap = atoi(e);
    if ((e = getenv("RECOMP_FE_SNAP_MAX"))) g_fe_snap_max = atoi(e);
    if ((e = getenv("RECOMP_FE_SNAP_SKIP"))) g_fe_snap_skip = atoi(e);
    if ((e = getenv("RECOMP_FE_SNAP")) && getenv("RECOMP_FE_SNAP_DIR")) {
        char p[600];
        snprintf(g_fe_snap_dir, sizeof g_fe_snap_dir, "%s", getenv("RECOMP_FE_SNAP_DIR"));
        CreateDirectoryA(g_fe_snap_dir, 0);
        snprintf(p, sizeof p, "%s/s", g_fe_snap_dir); CreateDirectoryA(p, 0);
        snprintf(p, sizeof p, "%s/pages.dat", g_fe_snap_dir);
        g_fe_pages = fopen(p, "ab");
        g_fe_hash = (fe_hent_t *)calloc((size_t)1 << FE_HBITS, sizeof(fe_hent_t));
        if (g_fe_pages && g_fe_hash) {
            _fseeki64(g_fe_pages, 0, SEEK_END);
            g_fe_npages = (uint32_t)(_ftelli64(g_fe_pages) / 4096);
            if (g_fe_npages) fprintf(stderr, "[FE] pages.dat already holds %u pages: not deduplicated against\n", g_fe_npages);
            fe_load_snap_list(e);
            g_fe_snap_on = 1;
        }
    }
    atexit(fe_atexit);
    if (g_fe_out) CloseHandle(CreateThread(0, 0, fe_dump_thread, 0, 0, 0));
    fprintf(stderr, "[FE] fecheck runtime: %u functions, out=%s, snapshots %s\n", g_fe_table_n,
            g_fe_out ? g_fe_out : "(none)", g_fe_snap_on ? g_fe_snap_dir : "off");
}

static void fe_hash_page(const uint64_t *p, uint64_t *ha, uint64_t *hb)
{
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full; int i;
    for (i = 0; i < 512; i++) {
        uint64_t v = p[i];
        a = (a ^ v) * 0x100000001B3ull; a ^= a >> 29;
        b = (b + v) * 0xFF51AFD7ED558CCDull; b ^= b >> 32;
    }
    *ha = a; *hb = b ^ 1;   /* never both zero */
}

static uint32_t fe_store_page(const uint8_t *pg)
{
    const uint64_t *q = (const uint64_t *)pg; uint64_t a, b; uint32_t h, i;
    for (i = 0; i < 512 && !q[i]; i++) {}
    if (i == 512) return 0xFFFFFFFFu;
    fe_hash_page(q, &a, &b);
    h = (uint32_t)(a >> (64 - FE_HBITS));
    for (;;) {
        fe_hent_t *e = &g_fe_hash[h];
        if (!e->used) {
            if (g_fe_hcount >= (3u << (FE_HBITS - 2))) return 0xFFFFFFFEu;  /* table full */
            e->used = 1; e->a = a; e->b = b; e->page = g_fe_npages++; g_fe_hcount++;
            fwrite(pg, 1, 4096, g_fe_pages);
            return e->page;
        }
        if (e->a == a && e->b == b) return e->page;
        h = (h + 1) & ((1u << FE_HBITS) - 1);
    }
}

static void fe_snapshot(uint32_t idx, uint32_t va, uint32_t hit)
{
    char path[700]; FILE *f; uint32_t hdr[32], i, np, sz, csz = 64u << 20, ms = GetTickCount() - g_fe_t0;
    uint32_t *table; const uint8_t *ram = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    sz = (uint32_t)(g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram);
    np = (sz + csz) / 4096;
    table = (uint32_t *)malloc(np * 4); if (!table) return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 0x31534546u; /* FES1 */ hdr[1] = va; hdr[2] = sz; hdr[3] = csz;
    hdr[4] = g_eax; hdr[5] = g_ecx; hdr[6] = g_edx; hdr[7] = g_ebx; hdr[8] = g_esp;
    hdr[9] = g_ebp; hdr[10] = g_seh_ebp; hdr[11] = g_esi; hdr[12] = g_edi;
    hdr[13] = g_recomp_eflags; hdr[14] = (uint32_t)g_df; hdr[15] = (uint32_t)g_fp_top;
    hdr[16] = g_fp_control_word; hdr[17] = g_fs_base; hdr[18] = hit; hdr[19] = ms;
    hdr[20] = GetCurrentThreadId();
    for (i = 0; i < np; i++) {
        const uint8_t *pg = i < sz / 4096 ? ram + (size_t)i * 4096 : ram + 0x80000000u + (size_t)(i - sz / 4096) * 4096;
        table[i] = fe_store_page(pg);
        if (table[i] == 0xFFFFFFFEu) { free(table); g_fe_snap_on = 0; fprintf(stderr, "[FE] page table full: snapshots off\n"); return; }
    }
    fflush(g_fe_pages);
    snprintf(path, sizeof path, "%s/s/%08X_%u.fes", g_fe_snap_dir, va, hit);
    f = fopen(path, "wb");
    if (f) {
        fwrite(hdr, 4, 32, f);
        fwrite(g_fp_stack, 8, 8, f);
        { RecompMmx m[8] = { g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7 }; fwrite(m, 8, 8, f); }
        { RecompXmm x[8] = { g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7 }; fwrite(x, 16, 8, f); }
        fwrite(table, 4, np, f);
        fclose(f);
    }
    free(table);
    (void)idx;
    fprintf(stderr, "[FE] snap %08X hit %u t=%us pages=%u\n", va, hit, ms / 1000, g_fe_npages);
}

void recomp_fe_enter(uint32_t idx, uint32_t va)
{
    fe_rec_t *r; uint64_t c; uint32_t ra, i;
    if (g_fe_init_state != 2) {
        if (InterlockedCompareExchange(&g_fe_init_state, 1, 0) == 0) { fe_init(); g_fe_init_state = 2; }
        else { while (g_fe_init_state != 2) Sleep(0); }
    }
    r = &g_fe[idx];
    c = ++r->count;
    ra = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
    for (i = 0; i < FE_NCALLER; i++) {
        if (r->callers[i] == ra) { r->caller_hits[i]++; break; }
        if (!r->callers[i]) { r->callers[i] = ra; r->caller_hits[i] = 1; break; }
    }
    if (c <= 3 || c == 10 || c == 100 || c == 1000 || c == 10000 || c == 100000) {
        uint32_t k = r->nsamp;
        if (k < FE_NSAMP) {
            uint32_t *s = r->samp[k], j; const uint32_t *st = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
            r->nsamp = k + 1;
            s[0] = g_eax; s[1] = g_ecx; s[2] = g_edx; s[3] = g_ebx; s[4] = g_esi; s[5] = g_edi;
            s[6] = g_esp; s[7] = ra;
            for (j = 0; j < 8; j++) s[8 + j] = st[1 + j];
        }
    }
    if (r->want && g_fe_snap_on && r->snaps < (uint32_t)g_fe_snap_k && c > (uint64_t)g_fe_snap_skip) {
        uint32_t now = GetTickCount();
        if (!r->snaps || now - r->last_snap_ms >= (uint32_t)g_fe_snap_gap) {
            if (InterlockedCompareExchange(&g_fe_snap_busy, 1, 0) == 0) {
                if (g_fe_snap_total < g_fe_snap_max && r->snaps < (uint32_t)g_fe_snap_k) {
                    r->snaps++; g_fe_snap_total++;
                    fe_snapshot(idx, va, (uint32_t)c);
                    r->last_snap_ms = GetTickCount();
                }
                g_fe_snap_busy = 0;
            }
        }
    }
}
#endif /* RECOMP_FECHECK */
