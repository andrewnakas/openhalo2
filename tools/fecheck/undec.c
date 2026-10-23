/* coff.undecorate off Windows: Microsoft's dbghelp UnDecorateSymbolName under Wine.
   Reads "<flags hex> <decorated name>" lines on stdin, writes one undecorated line each
   (the name itself when dbghelp cannot undecorate it). Built and started by coff.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef DWORD (WINAPI *undec_fn)(const char *, char *, DWORD, DWORD);

int main(int argc, char **argv)
{
    HMODULE h = LoadLibraryA(argc > 1 ? argv[1] : "dbghelp.dll");
    undec_fn f = h ? (undec_fn)GetProcAddress(h, "UnDecorateSymbolName") : 0;
    static char line[8192], out[4096];
    if (!f)
        return 2;
    while (fgets(line, sizeof line, stdin)) {
        char *name;
        unsigned long flags = strtoul(line, &name, 16);
        size_t n;
        while (*name == ' ')
            name++;
        n = strlen(name);
        while (n && (name[n - 1] == '\n' || name[n - 1] == '\r'))
            name[--n] = 0;
        fputs(f(name, out, sizeof out, (DWORD)flags) ? out : name, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return 0;
}
