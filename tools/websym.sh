#!/bin/bash
# websym.sh 0x1f5bf22 [...]: source line of a wasm code offset from a browser stack trace
source /d/n64work/emsdk/emsdk_env.sh >/dev/null 2>&1
for a in "$@"; do python /d/n64work/emsdk/upstream/emscripten/tools/emsymbolizer.py /d/n64work/halo2/build/web/site/halo2.wasm $a 2>&1 | tail -2; done
