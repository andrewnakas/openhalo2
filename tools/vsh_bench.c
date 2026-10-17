/* vsh_bench.c: offline timing and exactness check of the NV2A vertex program
 * paths in xboxrecomp's nv2a_vsh_interp.c, on blocks captured from the game
 * with RECOMP_VSH_CAPTURE=<file> (program, constants, inputs per record).
 *
 *   bash tools/vsh_bench.sh <capture> [reps]
 *
 * For every record: decode once, run vsh_run_sse (one vertex at a time, the
 * reference), vsh_run_sse_batch (16-vertex AoS block), vsh_run_soa
 * (structure of arrays) and vsh_run_uop (compiled operation lists), compare
 * every output bit with the reference, then time reps runs of each block
 * path. The capture holds game data: keep it out of the repo. */
#define _CRT_SECURE_NO_WARNINGS
#define VSH_BENCH_HOOKS 1
#include "nv2a_vsh_interp.c"   /* -I <xboxrecomp>/src/kernel */

#include <windows.h>

static double now_s(void)
{
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return (double)q.QuadPart / (double)f.QuadPart;
}

typedef int (*BlockFn)(const float (*)[NV2A_VSH_INPUTS][4], Nv2aVshOutput *const *, uint32_t);

int main(int argc, char **argv)
{
    static float in[NV2A_VSH_BATCH][NV2A_VSH_INPUTS][4];
    static Nv2aVshOutput ref[NV2A_VSH_BATCH], o[3][NV2A_VSH_BATCH];
    static const char *const nm[3] = { "batch", "soa", "uop" };
    const BlockFn fn[3] = { vsh_run_sse_batch, vsh_run_soa, vsh_run_uop };
    Nv2aVshOutput *po[3][NV2A_VSH_BATCH];
    FILE *f;
    int reps = argc > 2 ? atoi(argv[2]) : 50, r, m;
    unsigned long long recs = 0, verts = 0, bad[3] = {0, 0, 0}, progs = 0;
    double t[3] = {0, 0, 0};
    uint32_t prev_hash = 0, j;
    double st_ops = 0, st_k = 0, st_z = 0, st_in = 0;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: vsh_bench <capture> [reps]%c", 10);
        return 2;
    }
    if (getenv("VU_SKIP")) g_vu_bench_skip = 1;   /* time all but the operations */
    for (m = 0; m < 3; m++)
        for (j = 0; j < NV2A_VSH_BATCH; j++)
            po[m][j] = &o[m][j];
    for (;;) {
        uint32_t hdr[3], s, h = 2166136261u;
        static uint32_t prog[NV2A_VSH_SLOTS][4];
        static float cst[NV2A_VSH_CONSTANTS][4];
        if (fread(hdr, 4, 3, f) != 3 || hdr[0] != 0x31485356u)
            break;
        if (hdr[1] > NV2A_VSH_SLOTS || hdr[2] > NV2A_VSH_BATCH || !hdr[2])
            break;
        if (fread(prog, 16, hdr[1], f) != hdr[1]
            || fread(cst, 16, NV2A_VSH_CONSTANTS, f) != NV2A_VSH_CONSTANTS
            || fread(in, sizeof in[0], hdr[2], f) != hdr[2])
            break;
        if (!hdr[1])
            continue;
        /* -DNV2A_VSH_BATCH=32: the block twice over, to time larger blocks */
        for (j = hdr[2]; j < NV2A_VSH_BATCH && j < 2 * hdr[2]; j++)
            memcpy(in[j], in[j - hdr[2]], sizeof in[j]);
        hdr[2] = j;
        for (s = 0; s < hdr[1]; s++) {
            nv2a_vsh_set_instruction(s, prog[s]);
            for (j = 0; j < 4; j++) h = (h ^ prog[s][j]) * 16777619u;
        }
        if (h != prev_hash) progs++;
        prev_hash = h;
        nv2a_vsh_set_start_slot(0);
        for (s = 0; s < NV2A_VSH_CONSTANTS; s++)
            nv2a_vsh_set_constant(s, cst[s]);
        decode_program();
        recs++;
        verts += hdr[2];
        { const VshProg *vp = vu_current(); if (vp) { st_ops += vp->nops; st_k += vp->nk; st_z += vp->nzero; st_in += __popcnt(vp->in_used); } }
        for (j = 0; j < hdr[2]; j++)
            vsh_run_sse((const float (*)[4])in[j], &ref[j]);
        for (m = 0; m < 3; m++) {
            fn[m]((const float (*)[NV2A_VSH_INPUTS][4])in, po[m], hdr[2]);
            for (j = 0; j < hdr[2]; j++) {
                if (!memcmp(&ref[j], &o[m][j], sizeof ref[j]))
                    continue;
                if (bad[m]++ < 4) {
                    const float *a = (const float *)&ref[j], *b = (const float *)&o[m][j];
                    int k;
                    for (k = 0; k < (int)(sizeof ref[j] / 4); k++)
                        if (memcmp(&a[k], &b[k], 4))
                            fprintf(stderr, "  %s: block %llu vertex %u float %d: ref %.9g got %.9g%c",
                                    nm[m], recs, j, k, a[k], b[k], 10);
                }
            }
        }
        for (m = 0; m < 3; m++) {
            double t0 = now_s();
            for (r = 0; r < reps; r++)
                fn[m]((const float (*)[NV2A_VSH_INPUTS][4])in, po[m], hdr[2]);
            t[m] += now_s() - t0;
        }
    }
    printf("%llu blocks, %llu vertices, %llu program changes%c", recs, verts, progs, 10);
    printf("mismatching vertices vs vsh_run_sse: batch %llu, soa %llu, uop %llu%c",
           bad[0], bad[1], bad[2], 10);
    printf("ns per vertex: batch %.1f  soa %.1f  uop %.1f  (uop %.2fx batch)%c",
           t[0] * 1e9 / ((double)verts * reps), t[1] * 1e9 / ((double)verts * reps),
           t[2] * 1e9 / ((double)verts * reps), t[2] > 0 ? t[0] / t[2] : 0.0, 10);
    printf("per block: %.1f uops, %.1f constant components, %.1f zeroed temps, %.1f inputs%c", st_ops / recs, st_k / recs, st_z / recs, st_in / recs, 10);
    return (bad[0] || bad[1] || bad[2]) ? 1 : 0;
}
