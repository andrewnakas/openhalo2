#!/bin/bash
# Full regeneration of the lifted C: disasm -> func_id -> abi_analysis -> recomp.
# Generates into build/gen_tmp, then copies only the files whose content changed
# into the project's gen dir, so an incremental build recompiles only those.
# usage: bash relift.sh [--recomp-only]
set -e
XR=/d/n64work/halo2/third_party/xboxrecomp
XBE=D:/n64work/halo2/dirty/game/default.xbe
TMP=D:/n64work/halo2/build/gen_tmp
GEN=/d/n64work/openhalo2/port/xr/src/recomp/gen
cd "$XR"
if [ "$1" != "--recomp-only" ]; then
  python -m tools.disasm "$XBE" --force --seed-functions D:/n64work/openhalo2/config/seed_decomp_functions.json
  python -m tools.func_id "$XBE"
  python -m tools.abi_analysis "$XBE"
fi
mkdir -p "$TMP"
python -m tools.recomp "$XBE" --all --split 250 --game-name halo2 --gen-dir "$TMP" 2>&1 | grep -v "gen_tmp" || true
# the pipeline's recomp_types.h comes from the template; keep the project copy in step
cp templates/runtime/recomp_types.h "$TMP/recomp_types.h"
# hardware libraries get checked register access (no page faults): see h2lift/mmio_pass.py
python /d/n64work/openhalo2/h2lift/mmio_pass.py "$TMP"
# tiled-aperture pointers: the contiguous window in the web build (see h2lift/tiled_pass.py)
python /d/n64work/openhalo2/h2lift/tiled_pass.py "$TMP"
changed=0
for f in "$TMP"/*; do
  b=$(basename "$f")
  if ! cmp -s "$f" "$GEN/$b"; then cp "$f" "$GEN/$b"; changed=$((changed+1)); fi
done
for f in "$GEN"/recomp_*.c; do
  b=$(basename "$f")
  [ -e "$TMP/$b" ] || { echo "stale (not regenerated): $b"; }
done
echo "RELIFT_DONE changed_files=$changed"
grep -ho 'RECOMP_UNIMPL("[a-z0-9]*' "$GEN"/*.c | sort | uniq -c | sort -rn | grep -E 'lock|mxcsr|cpuid|pushfd|popfd|xchg' || echo "no lock/mxcsr/cpuid/pushfd/popfd sites left untranslated"
