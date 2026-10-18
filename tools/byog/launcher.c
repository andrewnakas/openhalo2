/*
 * OpenHalo2.exe: the program a player starts.
 *
 * The release carries no code from the game. On first start this:
 *   1. imports the player's own disc into game\ (byog.c, the same dialog the
 *      game program uses) and checks it is the supported retail build;
 *   2. builds halo2_recomp.exe from that copy: tools\byog\build.py, run by
 *      the bundled Python in a console window the player can watch;
 *   3. starts halo2_recomp.exe with the same command line.
 * Later starts go straight to step 3. A release with a different kit version
 * (kit\kit.json "version" vs build\stamp) rebuilds once.
 *
 * Built by tools/byog/build_launcher.cmd (MSVC, static CRT).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "byog.h"

static const char k_title[] = "Open Halo 2";

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Read up to n-1 bytes of a small file, trimmed at the first newline. */
static void read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    buf[0] = 0;
    if (!f)
        return;
    if (fgets(buf, (int)n, f))
        buf[strcspn(buf, "\r\n")] = 0;
    fclose(f);
}

/* "version": "<v>" out of kit\kit.json (a flat file written by make_release.sh). */
static void kit_version(char *buf, size_t n)
{
    char all[4096] = "", *p, *q;
    FILE *f = fopen("kit\\kit.json", "rb");
    buf[0] = 0;
    if (!f)
        return;
    all[fread(all, 1, sizeof all - 1, f)] = 0;
    fclose(f);
    if (!(p = strstr(all, "\"version\"")) || !(p = strchr(p + 9, '"')) || !(q = strchr(++p, '"')))
        return;
    if ((size_t)(q - p) < n) {
        memcpy(buf, p, q - p);
        buf[q - p] = 0;
    }
}

static int needs_build(void)
{
    char want[128], have[128];
    if (!file_exists("halo2_recomp.exe"))
        return 1;
    kit_version(want, sizeof want);
    read_line("build\\stamp", have, sizeof have);
    return want[0] && strcmp(want, have) != 0;
}

static int run_wait(char *cmd, DWORD flags, DWORD *exit_code)
{
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static int build_game(void)
{
    /* On failure the window waits for a key so the player can read the error. */
    char cmd[512] =
        "cmd.exe /c \"python\\python.exe -I tools\\byog\\build.py || "
        "(echo. & echo Press a key to close this window. & pause >nul & exit /b 1)\"";
    DWORD rc = 1;
    if (MessageBoxA(NULL,
            "Open Halo 2 now builds the game program from your own copy of Halo 2.\n\n"
            "This happens once. It takes about 10-30 minutes and needs about 3 GB of "
            "free disk space. A window shows the progress.\n\n"
            "Build it now?", k_title, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return 0;
    if (!file_exists("python\\python.exe") || !file_exists("tools\\byog\\build.py")) {
        MessageBoxA(NULL, "This folder is incomplete (python\\ or tools\\byog\\ is missing). "
                    "Unzip the whole release again.", k_title, MB_ICONERROR);
        return 0;
    }
    if (!run_wait(cmd, CREATE_NEW_CONSOLE, &rc) || rc != 0 || !file_exists("halo2_recomp.exe")) {
        MessageBoxA(NULL, "The game program was not built. The reason is in build\\build.log.\n\n"
                    "Start OpenHalo2.exe again to retry.", k_title, MB_ICONWARNING);
        return 0;
    }
    return 1;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR args, int show)
{
    char dir[MAX_PATH], cmd[MAX_PATH + 4096];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    (void)hi; (void)hp; (void)show;

    if (n && n < sizeof dir) {
        char *slash = strrchr(dir, '\\');
        if (slash) {
            *slash = 0;
            SetCurrentDirectoryA(dir);
        }
    }
    if (!byog_ensure_game(1))
        return 1;
    if (needs_build() && !build_game())
        return 1;

    snprintf(cmd, sizeof cmd, "\"halo2_recomp.exe\" %s", args ? args : "");
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxA(NULL, "Could not start halo2_recomp.exe.", k_title, MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
