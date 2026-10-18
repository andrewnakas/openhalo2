#!/bin/bash
# make_release.sh <version>  -- builds dist/OpenHalo2-<version>-win64.zip.
#
# The release carries no code or data from the game. The player's own disc is
# lifted and compiled on their machine (tools/byog: OpenHalo2.exe -> build.py
# -> lift.py), so the zip holds:
#   OpenHalo2.exe, h2import.exe       launcher + importer (ours)
#   kit/       the runtime as libraries (port/xr/build_kit.cmd), kit.json,
#              gen_manifest.json (hashes of the tested lifted C), seeds
#   toolkit/   the xboxrecomp lifter (MIT) with our changes
#   h2lift/    our lifter passes
#   python/    embeddable CPython + capstone
#   toolchain/ clang-cl + lld-link (LLVM) and xwin
# and refuses to package anything that looks like game data or lifted code.
#
# Inputs (env): KIT (build_kit.cmd output dir), XR (xboxrecomp working tree),
# DL (downloads: py/, llvm/x/<clang dir>, xwin/), LAUNCHER (OpenHalo2.exe).
# Prints a one-screen summary.
set -e
V=${1:?usage: make_release.sh <version>}
R=$(cd "$(dirname "$0")/.." && pwd)
KIT=${KIT:-/d/n64work/halo2/build/kit}
XR=${XR:-/d/n64work/halo2/third_party/xboxrecomp}
DL=${DL:-/e/oh2byog/dl}
LAUNCHER=${LAUNCHER:-$R/tools/byog/bin/OpenHalo2.exe}
NAME=OpenHalo2-$V-win64
OUT=$R/dist/$NAME
case "$OUT" in "$R"/dist/OpenHalo2-*) ;; *) echo "refusing odd output path $OUT"; exit 1;; esac
rm -rf "$OUT" && mkdir -p "$OUT"

# ---- launcher, importer, docs
cp "$LAUNCHER" "$OUT/OpenHalo2.exe"
[ -f "$R/tools/import/bin/h2import.exe" ] && cp "$R/tools/import/bin/h2import.exe" "$OUT/"
cp "$R/README.md" "$R/LICENSE" "$R/NOTICE" "$OUT/"
cat > "$OUT/halo2.ini.example" <<'EOF'
; Rename to halo2.ini to use. See README.md.
fullscreen = 0
mouse_sens = 1.0
mouse_deadzone = 0.2
volume_db = 0
log_cap_mb = 64
EOF
mkdir -p "$OUT/tools/byog"
cp "$R/tools/byog/build.py" "$R/tools/byog/lift.py" "$OUT/tools/byog/"

# ---- kit: runtime libraries + link/compile description
mkdir -p "$OUT/kit/lib" "$OUT/kit/include"
[ -f "$KIT/kit_link.txt" ] || { echo "no $KIT/kit_link.txt: run port/xr/build_kit.cmd"; exit 1; }
libs=() sys=()
while IFS= read -r l; do
  l=${l%$'\r'}; [ -n "$l" ] || continue
  case "$l" in
    *.lib) cp "$l" "$OUT/kit/lib/"; libs+=("lib/$(basename "$l")");;
    *) sys+=("$l.lib");;
  esac
done < "$KIT/kit_link.txt"
cp "$R/port/xr/src/recomp/recomp_mmio.h" "$XR/src/kernel/recomp_icall_feedback.h" "$OUT/kit/include/"
cp "$R/config/gen_manifest.json" "$R/config/seed_decomp_functions.json" "$OUT/kit/"
python - "$OUT/kit/kit.json" "$V" "${libs[@]}" -- "${sys[@]}" <<'EOF'
import json, sys
out, ver, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
cut = rest.index("--")
kit_libs, sys_libs = rest[:cut], rest[cut + 1:]
# The first kit library is the runtime itself, linked whole so WinMain and
# every entry point the lifted code calls are there whatever the link order.
kit = {
    "version": ver,
    "compile": {
        # = the CMake Release flags of port/xr for the generated files
        "flags": ["/O2", "/Ob2", "/std:c11", "/MT", "/bigobj"],
        "defines": ["WIN32", "_WINDOWS", "NDEBUG", "NOMINMAX", "WIN32_LEAN_AND_MEAN",
                    "OPENHALO2_NET=1", "RTC_STATIC=1"],
        "include": ["include"],
    },
    "link": {
        "flags": ["/machine:x64", "/INCREMENTAL:NO", "/subsystem:windows",
                  "/LARGEADDRESSAWARE", "/LIBPATH:kit/lib",
                  "/WHOLEARCHIVE:" + kit_libs[0].split("/")[-1]],
        "objects": [],
        "kit_libs": kit_libs,
        "system_libs": sys_libs,
    },
}
json.dump(kit, open(out, "w"), indent=1)
EOF

# ---- lifter: toolkit subset + our passes
T=$OUT/toolkit
mkdir -p "$T/tools" "$T/templates/runtime"
cp "$XR/tools/__init__.py" "$T/tools/"
for d in xbe_parser disasm func_id abi_analysis recomp; do
  mkdir -p "$T/tools/$d"
  (cd "$XR/tools/$d" && find . -name '*.py' ! -name 'test_*' ! -path './output/*' ! -path '*/__pycache__/*' -print0 \
     | xargs -0 -I{} cp --parents {} "$T/tools/$d/")
done
cp "$XR/templates/runtime/recomp_types.h" "$T/templates/runtime/"
cp "$XR/LICENSE" "$T/LICENSE"
mkdir -p "$OUT/h2lift"
cp "$R/h2lift/mmio_pass.py" "$R/h2lift/tiled_pass.py" "$R/h2lift/xo_hle_pass.py" "$OUT/h2lift/"

# ---- Python (embeddable) + capstone
P=$OUT/python
mkdir -p "$P"
(cd "$P" && unzip -q "$DL/py/python-3.12.10-embed-amd64.zip")
(cd "$P" && unzip -q "$DL/py/capstone-5.0.7-py3-none-win_amd64.whl" -d site-packages)
# The lifter runs as `python -m tools.<x>` from toolkit\. An embedded Python
# takes its module path only from this file.
cat > "$P/python312._pth" <<'EOF'
python312.zip
.
site-packages
..\toolkit
EOF

# ---- toolchain
C=$OUT/toolchain
# clang finds its own headers at <exe dir>/../lib/clang/<ver>, so keep bin/ + lib/
mkdir -p "$C/llvm/bin" "$C/llvm/lib"
LL=$(ls -d "$DL"/llvm/x/clang+llvm-*-x86_64-pc-windows-msvc | head -1)
cp "$LL/bin/clang-cl.exe" "$C/llvm/bin/clang-cl.exe" 2>/dev/null || cp "$LL/bin/clang.exe" "$C/llvm/bin/clang-cl.exe"
cp "$LL/bin/lld-link.exe" "$C/llvm/bin/lld-link.exe" 2>/dev/null || cp "$LL/bin/lld.exe" "$C/llvm/bin/lld-link.exe"
cp -r "$LL/lib/clang" "$C/llvm/lib/"
cp "$LL/LICENSE.TXT" "$C/llvm/LICENSE.TXT"
XW=$(ls -d "$DL"/xwin/xwin-*-x86_64-pc-windows-msvc | head -1)
cp "$XW/xwin.exe" "$C/xwin.exe"
for f in "$XW"/LICENSE*; do [ -f "$f" ] && cp "$f" "$C/xwin-$(basename "$f")"; done

# ---- content check: no game data, no lifted code
bad=0
while IFS= read -r -d '' f; do
  base=$(basename "$f")
  magic=$(head -c 4 "$f" | od -An -c | tr -d ' \n')
  case "$magic" in
    XBEH|daeh|head|BIKi|BIKf|BIKh) echo "GAME DATA? $base (magic $magic)"; bad=1;;
  esac
  case "$base" in
    *.xbe|*.map|*.bik|*.xmv|*.wma|*.xsb|*.xwb|*.iso|*.img) echo "GAME DATA? $base (name)"; bad=1;;
    halo2_recomp.exe|recomp_[0-9]*|recomp_dispatch.c|recomp_funcs.h|recomp_stubs_unresolved.c)
      echo "LIFTED CODE? $base"; bad=1;;
  esac
done < <(find "$OUT" -type f -print0)
for d in game save build; do [ -e "$OUT/$d" ] && { echo "$d/ in the release"; bad=1; }; done
# The runtime libraries may call lifted functions (sub_XXXXXXXX) but never define one.
for l in "$OUT"/kit/lib/*.lib; do
  if python "$R/tools/byog/lib_defines.py" "$l" | grep -qE '^sub_[0-9A-Fa-f]{8}$'; then
    echo "LIFTED CODE in $(basename "$l")"; bad=1; fi
done
[ $bad = 0 ] || { echo "RELEASE REFUSED"; exit 1; }

cd "$R/dist" && rm -f "$NAME.zip"
# zipfile, not Compress-Archive: Windows PowerShell 5.1 writes backslash paths
python - "$NAME" <<'EOF'
import os, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + ".zip", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, fs in os.walk(name):
        for f in sorted(fs):
            p = os.path.join(d, f)
            z.write(p, p.replace(os.sep, "/"))
EOF
echo "release $NAME.zip: $(find "$OUT" -type f | wc -l) files, $(du -sh "$NAME.zip" | cut -f1); content check OK"
ls -1 "$OUT"
