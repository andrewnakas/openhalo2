#!/bin/bash
# vsh_bench.sh <capture> [reps]: build tools/vsh_bench.c against the toolkit's
# nv2a_vsh_interp.c with MSVC (/O2, as the native build) and run it on a
# capture made with RECOMP_VSH_CAPTURE=<file> (see nv2a_vsh_interp.c).
B=${BENCH_DIR:-/d/n64work/halo2/build/vsh_bench}; mkdir -p "$B"
K=$(cygpath -w /d/n64work/halo2/third_party/xboxrecomp/src/kernel)
SRC=$(cygpath -w /d/n64work/openhalo2/tools/vsh_bench.c)
WB=$(cygpath -w "$B")
VC='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
printf '@call "%s" >nul\r\ncl /nologo /O2 /GS- /W3 %s /I"%s" /Fe"%s\\vsh_bench.exe" /Fo"%s\\vsh_bench.obj" "%s" > "%s\\cl.log" 2>&1\r\n' \
  "$VC" "${BENCH_CFLAGS:-}" "$K" "$WB" "$WB" "$SRC" "$WB" > "$B/b.cmd"
rm -f "$B/vsh_bench.exe"
cmd //c "$(cygpath -w "$B/b.cmd")" > /dev/null 2>&1
[ -f "$B/vsh_bench.exe" ] || { grep -i "error" "$B/cl.log" | head; exit 1; }
"$B/vsh_bench.exe" "$@"
