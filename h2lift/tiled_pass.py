"""Tiled-aperture pointers in the lifted C, for the web build.

Some title code turns a locked surface's address into its tiled alias by
OR-ing in 0xF0000000 (after D3D's LockRect, sub_003FDBC0) and then writes the
pixels through it. On the native builds the tiled aperture is a second view of
the contiguous window (xbox_memory_layout.c), so the write lands in the
surface. WebAssembly has one linear memory and no second view: in the compact
2.28 GB layout guest 0xF0000000 is past the end of memory and the write traps
("memory access out of bounds"; Halo 2 does it while it first fills a blank
cache partition, i.e. on a bring-your-own-game first boot).

This pass rewrites `X | 0xF0000000u` into `X | RECOMP_TILED_OR`, which is
0xF0000000 everywhere but in the web build, where it is 0x80000000: the
contiguous window, which is what the tiled alias is in this runtime anyway.
Native code is unchanged. Idempotent; prints one line.

usage: python tiled_pass.py <gen dir> [--check]
"""
import os
import re
import sys

PAT = re.compile(r"\| 0xF0000000u\b")
DEF = ("/* h2lift/tiled_pass.py: the tiled aperture is the contiguous window on the web */\n"
       "#ifndef RECOMP_TILED_OR\n#if defined(__EMSCRIPTEN__)\n#define RECOMP_TILED_OR 0x80000000u\n"
       "#else\n#define RECOMP_TILED_OR 0xF0000000u\n#endif\n#endif\n")


def main(argv):
    gen = argv[1]
    check = "--check" in argv
    files = sites = 0
    for n in sorted(os.listdir(gen)):
        if not (n.startswith("recomp_") and n.endswith(".c")):
            continue
        p = os.path.join(gen, n)
        s = open(p, encoding="utf-8", newline="").read()
        k = len(PAT.findall(s))
        if not k:
            continue
        files += 1; sites += k
        if check:
            continue
        s = PAT.sub("| RECOMP_TILED_OR", s)
        if "#ifndef RECOMP_TILED_OR" not in s:
            # after the includes at the top of the file
            m = list(re.finditer(r"^#include [^\n]*\n", s[:4000], re.M))
            at = m[-1].end() if m else 0
            s = s[:at] + DEF + s[at:]
        open(p, "w", encoding="utf-8", newline="").write(s)
    print(f"tiled_pass: {sites} tiled-aperture ORs in {files} files {'found' if check else 'rewritten'}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
