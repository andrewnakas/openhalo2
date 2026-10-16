/*
 * h2import CLI.
 *   h2import <iso-or-folder> <out-game-dir>
 *   h2import --check <default.xbe>
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "h2import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif
#endif

static int last_pct = -1;

static void on_progress(uint64_t done, uint64_t total, const char *name, void *user) {
    int pct = total ? (int)(done * 100 / total) : 100;
    (void)user;
    if (pct == last_pct) return;
    last_pct = pct;
    fprintf(stderr, "\r  %3d%%  %-48.48s", pct, name);
    fflush(stderr);
}

static int run(int argc, char **argv) {
    h2_import_result res;
    time_t t0;
    double secs;
    int r;

    if (argc == 3 && !strcmp(argv[1], "--check")) {
        int ok = h2_xbe_sha_ok(argv[2]);
        printf("%s: %s\n", argv[2], ok ? "sha ok (supported Halo 2 default.xbe)" : "MISMATCH or unreadable");
        return ok ? 0 : 3;
    }
    if (argc != 3) {
        fprintf(stderr,
                "usage: h2import <halo2.iso | extracted-folder> <out-game-dir>\n"
                "       h2import --check <default.xbe>\n"
                "Copies default.xbe, maps/, media/, bink/, TDATA/, UDATA/ from your own Halo 2 disc\n"
                "(Redump ISO, XISO or extracted folder) and verifies default.xbe (SHA-256).\n");
        return 2;
    }
    t0 = time(NULL);
    r = h2_import(argv[1], argv[2], on_progress, NULL, &res);
    secs = difftime(time(NULL), t0);
    if (last_pct >= 0) fprintf(stderr, "\n");
    if (r != H2_OK) {
        fprintf(stderr, "h2import: error %d: %s\n", r, res.err);
        return 1;
    }
    printf("source    %s (%s", argv[1], res.source_kind ? "disc image" : "folder");
    if (res.source_kind) printf(", game partition at 0x%llX", (unsigned long long)res.partition_offset);
    printf(")\n");
    printf("output    %s\n", argv[2]);
    printf("copied    %u files, %u folders, %llu bytes (%.1f MB) in %.1f s\n", res.files, res.dirs,
           (unsigned long long)res.bytes, res.bytes / 1048576.0, secs);
    printf("skipped   %u top-level entries not needed by the game\n", res.skipped);
    printf("xbe sha   %s %s\n", res.sha_hex, res.sha_ok ? "OK" : "MISMATCH");
    return res.sha_ok ? 0 : 1;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* take the UTF-16 command line so non-ASCII paths survive */
    int wargc = 0, i, r;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **u8;
    if (!wargv) return run(argc, argv);
    u8 = (char **)calloc((size_t)wargc + 1, sizeof(char *));
    for (i = 0; u8 && i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        u8[i] = (char *)malloc(n > 0 ? (size_t)n : 1);
        if (!u8[i]) return 1;
        if (n <= 0) u8[i][0] = 0;
        else WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u8[i], n, NULL, NULL);
    }
    LocalFree(wargv);
    if (!u8) return 1;
    r = run(wargc, u8);
    for (i = 0; i < wargc; i++) free(u8[i]);
    free(u8);
    return r;
#else
    return run(argc, argv);
#endif
}
