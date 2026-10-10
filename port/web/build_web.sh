#!/bin/bash
# Open Halo 2 web build: compile what changed, link, report.
# usage: bash build_web.sh [link-only]     (objects in D:/n64work/halo2/build/web)
# Headers are not tracked: after a header change delete rt/*.o (1 minute) or
# lift/*.o (10 minutes) by hand.
OUT=/d/n64work/halo2/build/web; mkdir -p $OUT/lift $OUT/rt $OUT/site; cd $OUT
source /d/n64work/emsdk/emsdk_env.sh >/dev/null 2>&1
G=/d/n64work/openhalo2/port/xr/src; X=/d/n64work/halo2/third_party/xboxrecomp; W=/d/n64work/openhalo2/port/web
CF="-O1 -pthread -msse2 -mssse3 -msimd128 -fdeclspec -sUSE_SDL=2 -I$G -I$G/recomp -I$G/recomp/gen -I$X/include -I$X/src -I$X/src/kernel -I$X/src/platform -Wno-everything -ferror-limit=5"
cc() { src=$1; obj=$2
  [ -e "$obj" ] && [ "$obj" -nt "$src" ] && return 0
  case "$obj" in */rt/*) dbg=-g;; *) dbg=;; esac     # line numbers for the runtime (tools/websym.sh)
  emcc -c $CF $dbg "$src" -o "$obj" > "${obj%.o}.log" 2>&1 && rm -f "${obj%.o}.log" || echo "FAIL $src"; }
export -f cc; export CF
if [ "$1" != "link-only" ]; then
  { for f in $G/recomp/gen/*.c $G/recomp_manual.c $G/mmio_direct.c; do echo "$f $OUT/lift/$(basename ${f%.c}).o"; done
    for f in $(find $X/src -name '*.c' -not -path '*/d3d/*' -not -path '*smoke*' -not -name 'nv2a_pb_test.c'); do
      b=$(echo ${f#$X/src/} | tr '/' '_'); echo "$f $OUT/rt/${b%.c}.o"; done
    echo "$W/main_web.c $OUT/rt/main_web.o"; echo "$W/compat_web.c $OUT/rt/compat_web.o"; echo "$W/present_web.c $OUT/rt/present_web.o"
  } | xargs -P 3 -L 1 bash -c 'cc $0 $1'
fi
# Memory: 4 GB, guest address + 4 KB = linear address (no wrap anywhere). Guest
# RAM (64 MB) lies under GLOBAL_BASE; program data, stacks and heap sit above it
# and must stay under 2 GB, where the contiguous window and the device
# apertures begin. See win32_compat.c and xbox_memory_layout.c (__EMSCRIPTEN__).
emcc -O1 -pthread -sUSE_SDL=2 -sPROXY_TO_PTHREAD -sALLOW_MEMORY_GROWTH=0 \
     -sGLOBAL_BASE=134217728 -sINITIAL_MEMORY=4294901760 -sMAXIMUM_MEMORY=4294901760 -sSTACK_SIZE=8388608 -sDEFAULT_PTHREAD_STACK_SIZE=4194304 \
     -sPTHREAD_POOL_SIZE=24 -sERROR_ON_UNDEFINED_SYMBOLS=1 -sEXIT_RUNTIME=0 -sWASMFS -sFORCE_FILESYSTEM=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
     -sEXPORTED_RUNTIME_METHODS=ENV -sASSERTIONS=1 -g \
     lift/*.o rt/*.o -o site/halo2.js > link.log 2>&1
echo "LINK_EXIT=$? undefined=$(grep -c 'undefined symbol' link.log) duplicate=$(grep -c 'duplicate symbol' link.log)"
grep -o 'undefined symbol: [A-Za-z0-9_@]*' link.log | sort | uniq -c | sort -rn | head -60
grep 'duplicate symbol' link.log | head -10
