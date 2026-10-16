/*
 * Bring your own game: make sure `game\` holds the one retail Halo 2 this
 * build was lifted from before anything else runs.
 *
 * The lifted code in this program matches exactly one default.xbe (SHA-256
 * H2_XBE_SHA256_HEX). Another revision would load and then fail somewhere
 * deep inside, so it is refused up front with a plain message.
 *
 * A first launch with no `game\` folder offers to import one from the
 * player's own disc image (Redump ISO or XISO) or an extracted copy, through
 * tools/import (h2import.c): only the files the game reads are copied, and
 * the copy is checked against the same hash.
 *
 * RECOMP_SKIP_XBE_CHECK=1 skips the hash (development builds of a different
 * revision only).
 */
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include "h2import.h"
#include "byog.h"

#define GAME_DIR "game"
#define GAME_XBE "game\\default.xbe"

static const char k_title[] = "Halo 2";

/* ---- progress window ---------------------------------------------------- */

typedef struct {
    char src[MAX_PATH * 3];       /* UTF-8 */
    HWND wnd, label;
    volatile LONG done;
    int rc;
    h2_import_result res;
    DWORD last_tick;
} ImportJob;

static void on_progress(uint64_t done, uint64_t total, const char *name, void *user)
{
    ImportJob *j = (ImportJob *)user;
    char line[512];
    DWORD now = GetTickCount();
    if (now - j->last_tick < 100 && done < total)
        return;
    j->last_tick = now;
    snprintf(line, sizeof line, "Copying your game files...\n\n%s\n\n%.0f of %.0f MB (%d%%)",
             name ? name : "", (double)done / 1048576.0, (double)total / 1048576.0,
             total ? (int)(done * 100 / total) : 0);
    SetWindowTextA(j->label, line);
}

static DWORD WINAPI import_thread(LPVOID arg)
{
    ImportJob *j = (ImportJob *)arg;
    j->rc = h2_import(j->src, GAME_DIR, on_progress, j, &j->res);
    InterlockedExchange(&j->done, 1);
    PostMessageA(j->wnd, WM_NULL, 0, 0);
    return 0;
}

static LRESULT CALLBACK prog_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE)
        return 0;                       /* runs to the end; a half copy is .part only */
    return DefWindowProcA(h, m, w, l);
}

static int run_import(const char *src_utf8, h2_import_result *out)
{
    static ImportJob j;
    WNDCLASSA wc = {0};
    MSG msg;
    HANDLE th;

    memset(&j, 0, sizeof j);
    snprintf(j.src, sizeof j.src, "%s", src_utf8);
    wc.lpfnWndProc = prog_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "Halo2Import";
    wc.hCursor = LoadCursor(NULL, IDC_WAIT);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);
    j.wnd = CreateWindowExA(0, wc.lpszClassName, "Halo 2 - importing your disc",
                            WS_CAPTION | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                            520, 200, NULL, NULL, wc.hInstance, NULL);
    j.label = CreateWindowExA(0, "STATIC", "Reading the disc...", WS_CHILD | WS_VISIBLE,
                              16, 16, 480, 130, j.wnd, NULL, wc.hInstance, NULL);
    th = CreateThread(NULL, 0, import_thread, &j, 0, NULL);
    if (!th) {
        DestroyWindow(j.wnd);
        return H2_ERR_NOMEM;
    }
    while (!j.done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    DestroyWindow(j.wnd);
    *out = j.res;
    return j.rc;
}

/* ---- the check ---------------------------------------------------------- */

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int pick_source(char *utf8, size_t n)
{
    wchar_t path[MAX_PATH * 2] = L"";
    OPENFILENAMEW of = {0};
    of.lStructSize = sizeof of;
    of.lpstrFilter = L"Halo 2 disc image or default.xbe\0*.iso;*.xiso;default.xbe\0All files\0*.*\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrTitle = L"Choose your Halo 2 disc image (or default.xbe in an extracted copy)";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of))
        return 0;
    /* default.xbe picked: import the folder it is in. */
    {
        size_t len = wcslen(path);
        if (len >= 11 && _wcsicmp(path + len - 11, L"default.xbe") == 0) {
            path[len - 11] = 0;
            if (len > 11 && (path[len - 12] == L'\\' || path[len - 12] == L'/'))
                path[len - 12] = 0;
        }
    }
    return WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, (int)n, NULL, NULL) > 0;
}

int byog_ensure_game(int interactive)
{
    const char *skip = getenv("RECOMP_SKIP_XBE_CHECK");
    int skip_sha = skip && *skip && *skip != '0';

    while (!file_exists(GAME_XBE)) {
        char src[MAX_PATH * 3], msg[1200];
        h2_import_result res;
        int rc;
        if (!interactive) {
            fprintf(stderr, "[BYOG] no %s; run h2import or start the game by hand once\n", GAME_XBE);
            return 0;
        }
        if (MessageBoxA(NULL,
                "Open Halo 2 runs your own copy of Halo 2 (original Xbox).\n\n"
                "No game files were found next to this program. Choose your disc image "
                "(a Redump .iso or an XISO) or the default.xbe of an extracted copy, and "
                "the files the game needs (about 4.5 GB) are copied into the 'game' folder.\n\n"
                "Choose your game now?", k_title, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return 0;
        if (!pick_source(src, sizeof src))
            continue;
        rc = run_import(src, &res);
        if (rc == H2_OK)
            break;
        snprintf(msg, sizeof msg, "The import did not finish:\n\n%s\n\n%s", res.err,
                 rc == H2_ERR_SHA ? "This build runs one version of Halo 2 only (the original "
                                    "retail disc). Nothing of yours was changed."
                                  : "Nothing of yours was changed.");
        if (MessageBoxA(NULL, msg, k_title, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            return 0;
    }
    if (skip_sha) {
        fprintf(stderr, "[BYOG] XBE hash check skipped (RECOMP_SKIP_XBE_CHECK)\n");
        return 1;
    }
    if (!h2_xbe_sha_ok(GAME_XBE)) {
        fprintf(stderr, "[BYOG] %s is not the supported retail build (want SHA-256 %s)\n",
                GAME_XBE, H2_XBE_SHA256_HEX);
        if (interactive)
            MessageBoxA(NULL,
                "The default.xbe in the 'game' folder is not the version of Halo 2 this "
                "program was built for (the original retail disc).\n\n"
                "Move that folder away and start again to import your disc.",
                k_title, MB_ICONERROR);
        return 0;
    }
    fprintf(stderr, "[BYOG] %s: retail SHA-256 OK\n", GAME_XBE);
    return 1;
}
