#!/bin/bash
# Compile every lifted C file to a wasm32 object (no link): the first check that
# the lifted code itself is portable to WebAssembly. Objects in build/wasm_probe.
# usage: bash wasm_probe.sh   (about 10 minutes at 3 jobs)
OUT=/d/n64work/halo2/build/wasm_probe; mkdir -p $OUT; cd $OUT
source /d/n64work/emsdk/emsdk_env.sh >/dev/null 2>&1
G=/d/n64work/openhalo2/port/xr/src; X=/d/n64work/halo2/third_party/xboxrecomp
one() { f=$1; b=$(basename $f .c)
  emcc -c -O1 -pthread -msse2 -msimd128 -I$G -I$G/recomp -I$G/recomp/gen -I$X/include -I$X/src/kernel \
       -Wno-everything $f -o $b.o > $b.log 2>&1 && rm -f $b.log || echo "FAIL $b"; }
export -f one; export G X
ls $G/recomp/gen/*.c $G/recomp_manual.c $G/mmio_direct.c | xargs -P 3 -I{} bash -c 'one {}'
echo "WASM_PROBE_DONE objects=$(ls *.o | wc -l) failed=$(ls *.log 2>/dev/null | wc -l) bytes=$(du -sb . | cut -f1)"
