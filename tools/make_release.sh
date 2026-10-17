#!/bin/bash
# make_release.sh <version>  -- builds dist/OpenHalo2-<version>-win64.zip from the
# current build (D:/n64work/halo2/build/xr) and refuses to package anything that
# looks like game data. Prints a one-screen summary.
set -e
V=${1:?usage: make_release.sh <version>}
R=/d/n64work/openhalo2
B=${BUILD:-/d/n64work/halo2/build/xr}
NAME=OpenHalo2-$V-win64
OUT=$R/dist/$NAME
case "$OUT" in "$R"/dist/OpenHalo2-*) ;; *) echo "refusing odd output path $OUT"; exit 1;; esac
rm -rf "$OUT" && mkdir -p "$OUT"

cp "$B/halo2_recomp.exe" "$OUT/"
cp "$R/README.md" "$R/LICENSE" "$R/NOTICE" "$OUT/"
[ -f "$R/tools/import/bin/h2import.exe" ] && cp "$R/tools/import/bin/h2import.exe" "$OUT/"
cat > "$OUT/halo2.ini.example" <<'EOF'
; Rename to halo2.ini to use. See README.md.
fullscreen = 0
mouse_sens = 1.0
mouse_deadzone = 0.2
volume_db = 0
log_cap_mb = 64
EOF

# Content check: no game data may ever be in a release.
bad=0
while IFS= read -r -d '' f; do
  sz=$(stat -c %s "$f"); base=$(basename "$f")
  magic=$(head -c 4 "$f" | od -An -c | tr -d ' \n')
  case "$magic" in
    XBEH|daeh|head|BIKi|BIKf|BIKh|RIFF) echo "GAME DATA? $base (magic $magic)"; bad=1;;
  esac
  case "$base" in
    *.xbe|*.map|*.bik|*.xmv|*.wma|*.xsb|*.xwb|*.iso|*.img) echo "GAME DATA? $base (name)"; bad=1;;
  esac
  if [ "$sz" -gt 67108864 ] && [ "$base" != halo2_recomp.exe ]; then echo "TOO BIG $base $sz"; bad=1; fi
done < <(find "$OUT" -type f -print0)
[ -d "$OUT/game" ] || [ -d "$OUT/save" ] && { echo "game/ or save/ in the release"; bad=1; }
[ $bad = 0 ] || { echo "RELEASE REFUSED"; exit 1; }

# Static CRT check: the exe must not need the VC++ redistributable.
if ldd "$OUT/halo2_recomp.exe" 2>/dev/null | grep -qiE 'vcruntime|msvcp1[0-9]'; then
  echo "exe links the dynamic VC runtime"; exit 1; fi

cd "$R/dist" && rm -f "$NAME.zip"
powershell -NoProfile -Command "Compress-Archive -Path '$NAME' -DestinationPath '$NAME.zip'" >/dev/null
echo "release $NAME.zip: $(ls -1 "$OUT" | wc -l) files, $(du -sh "$NAME.zip" | cut -f1); content check OK"
ls -1 "$OUT"
