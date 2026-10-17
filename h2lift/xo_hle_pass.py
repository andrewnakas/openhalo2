"""Open Live hooks in the lifted C (docs/OPEN_LIVE_DESIGN.md, section 3.1).

The title reaches Xbox Live only through the XOnline library's public API: a
table of small thunks (`mov ecx, [0x48480C]; jmp <method>`) plus a few direct
entry points, and through four XNet QoS calls. This pass renames each of
those generated functions to `sub_XXXXXXXX_lifted` and puts a wrapper under
the original name:

    void sub_003A0641(void) { if (g_xo_mode) xo_hook(0x003A0641u, sub_003A0641_lifted);
                              else sub_003A0641_lifted(); }

`xo_hook` (port/xr/src/openlive.c) either answers the call in host code
(RECOMP_OPEN_LIVE=1), or runs the lifted body, tracing it with
RECOMP_XO_TRACE=1. With neither set, `g_xo_mode` is 0 and the wrapper is a
plain call of the lifted body: the real library path, unchanged.

The direct and indirect call sites, and the dispatch table, keep naming
`sub_XXXXXXXX`, so every route into the API goes through the wrapper.

The wrappers are compiled only where openlive.c is linked: native builds, or a
build that defines OPENHALO2_OPEN_LIVE. Elsewhere (the web build, until it
links openlive.c) they are a direct call of the lifted body.

Which functions: every function whose whole body is the thunk pattern (found
in the generated C, so a relift that moves code still finds them), plus the
fixed list below (direct entry points, XNet QoS, and XNet calls traced only).
Idempotent; prints one line.

usage: python xo_hle_pass.py <gen dir> [--check]
"""
import os
import re
import sys

# Not thunks of the 0x48480C singleton, but part of the boundary.
EXTRA = [
    0x3A0C39,  # XOnlineStartup (direct)
    0x3A0D36,  # XOnlineCleanup (direct)
    # the design's table (docs/OPEN_LIVE_DESIGN.md 1.2); most are also found
    # by the pattern, the rest have longer bodies (they push their arguments
    # again, or call through another object)
    0x3A0890, 0x3A8321, 0x3A834E, 0x3A835D, 0x3A838A, 0x3A8399, 0x3A83D4, 0x3A8407,
    0x3AFBB9, 0x3AFBF1, 0x3AFC76,
    # XNet QoS (answered by Open Live) and the XNet calls the trace reports
    0x3CD147, 0x3CD156, 0x3CD172, 0x3CD365,
    0x3CD576, 0x3CD58D, 0x3CD688,
    0x3CD0C0, 0x3CD0CB, 0x3CD0D6, 0x3CD0E1, 0x3CD0EC, 0x3CD103, 0x3CD32D, 0x3CD17D,
    0x3CD344, 0x3CD34F, 0x3CD35A,
]
LO, HI = 0x3A0000, 0x3CD000   # the XONLINE section (thunks are searched only here)

FN = re.compile(r"^void sub_([0-9A-F]{8})\(void\)\n\{\n(.*?)^\}\n", re.M | re.S)
THUNK = "\n    ecx = MEM32(0x48480C);\n"
MARK = "/* h2lift/xo_hle_pass.py */"
DECL = ("/* h2lift/xo_hle_pass.py: Open Live hooks (see port/xr/src/openlive.c) */\n"
        "#if !defined(__EMSCRIPTEN__) || defined(OPENHALO2_OPEN_LIVE)\n"
        "#define XO_HOOKED 1\n"
        "extern int g_xo_mode;\n"
        "void xo_hook(uint32_t va, void (*lifted)(void));\n"
        "#endif\n")
CR = chr(13)
LF = chr(10)


def wrapper(va):
    n = f"sub_{va:08X}"
    return (f"{MARK}\nvoid {n}_lifted(void);\nvoid {n}(void)\n{{\n"
            f"#ifdef XO_HOOKED\n    if (g_xo_mode) {{ xo_hook(0x{va:08X}u, {n}_lifted); return; }}\n#endif\n"
            f"    {n}_lifted();\n}}\n\n")


def main(argv):
    gen = argv[1]
    check = "--check" in argv
    extra = set(EXTRA)
    found = []
    for name in sorted(os.listdir(gen)):
        if not (name.startswith("recomp_") and name.endswith(".c")):
            continue
        p = os.path.join(gen, name)
        s = open(p, encoding="utf-8", newline="").read()
        crlf = (CR + LF) in s[:2000]
        s = s.replace(CR + LF, LF)
        if MARK in s:
            found += [int(v, 16) for v in re.findall(r"xo_hook\(0x([0-9A-F]{8})u", s)]
            continue  # already done
        hits = []
        for m in FN.finditer(s):
            va = int(m.group(1), 16)
            body = m.group(2)
            # a thunk: loads the singleton and makes one call or tail jump
            # (some keep a frame and push their arguments again first)
            if va in extra or (LO <= va < HI and THUNK in body
                               and body.count("RECOMP_ABI_CALL") <= 1
                               and body.count("\n    ") <= 24 and body.count("\nloc_") <= 2):
                hits.append(m)
        if not hits:
            continue
        found += [int(m.group(1), 16) for m in hits]
        if check:
            continue
        out = []
        last = 0
        for m in hits:
            va = int(m.group(1), 16)
            out.append(s[last:m.start()])
            out.append(wrapper(va))
            out.append(m.group(0).replace(f"void sub_{va:08X}(void)", f"void sub_{va:08X}_lifted(void)", 1))
            last = m.end()
        out.append(s[last:])
        s = "".join(out)
        inc = list(re.finditer(r"^#include [^\n]*\n", s[:4000], re.M))
        at = inc[-1].end() if inc else 0
        s = s[:at] + DECL + s[at:]
        if crlf:
            s = s.replace(LF, CR + LF)
        open(p, "w", encoding="utf-8", newline="").write(s)
    thunks = sum(1 for v in found if v not in extra)
    missing = sorted(extra - set(found))
    print(f"xo_hle_pass: {len(found)} functions hooked ({thunks} thunks + {len(found) - thunks} listed)"
          + (f"; MISSING {', '.join(hex(v) for v in missing)}" if missing else ""))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
