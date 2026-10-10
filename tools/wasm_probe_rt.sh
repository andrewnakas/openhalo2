#!/bin/bash
# Compile every runtime source (xboxrecomp src/ + the port's main.c) to wasm32
# objects and list what fails: the size of the runtime port to the web.
OUT=/d/n64work/halo2/build/wasm_probe_rt; rm -rf $OUT; mkdir -p $OUT; cd $OUT
source /d/n64work/emsdk/emsdk_env.sh >/dev/null 2>&1
G=/d/n64work/openhalo2/port/xr/src; X=/d/n64work/halo2/third_party/xboxrecomp
one() { f=$1; b=$(echo ${f#$X/src/} | tr '/' '_'); b=${b%.c}
  emcc -c -O1 -pthread -msse2 -mssse3 -msimd128 -fdeclspec -sUSE_SDL=2 -I$G -I$G/recomp -I$G/recomp/gen -I$X/include -I$X/src -I$X/src/kernel -I$X/src/platform \
       -Wno-everything -ferror-limit=5 $f -o $b.o > $b.log 2>&1 && rm -f $b.log; }
export -f one; export G X
(find $X/src -name '*.c' -not -path '*smoke*' -not -path '*/tests/*'; echo $G/main.c) | xargs -P 3 -I{} bash -c 'one {}'
echo "RT_PROBE ok=$(ls *.o 2>/dev/null | wc -l) failed=$(ls *.log 2>/dev/null | wc -l)"
for l in *.log; do echo "$l: $(grep -m1 error $l | sed "s/.*error: //" | cut -c1-90)"; done
