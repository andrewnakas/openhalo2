#!/bin/sh
# TEST-ONLY: end-to-end checks for h2import.
#   run_tests.sh <h2import-exe> <dirty-game-folder> <scratch-dir> [--full]
# Builds a small Redump-layout image (and with --full a whole-disc XISO, ~4.7 GB),
# imports image + folder, compares byte-for-byte, then runs the negative cases.
# Writes only inside <scratch-dir>; delete it afterwards.
set -u
B=$1; SRC=$2; W=$3; FULL=${4:-}
T=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python}
mkdir -p "$W" || exit 1
cd "$W" || exit 1
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1"; }
SMALL="default.xbe media/* maps/fonts/* TDATA/* UDATA/*"

$PY "$T/mkxiso.py" "$SRC" redump.iso --redump --only $SMALL update.xbe downloader.xbe >/dev/null
"$B" redump.iso out_redump >/dev/null 2>&1 && $PY "$T/compare.py" "$SRC" out_redump $SMALL && ok redump || bad redump
"$B" out_redump out_folder >/dev/null 2>&1 && $PY "$T/compare.py" out_redump out_folder && ok folder || bad folder
if [ "$FULL" = "--full" ]; then
  $PY "$T/mkxiso.py" "$SRC" full.iso >/dev/null
  "$B" full.iso out_full >/dev/null 2>&1 && $PY "$T/compare.py" "$SRC" out_full && ok full-xiso || bad full-xiso
  "$B" "$SRC" out_full_folder >/dev/null 2>&1 && $PY "$T/compare.py" "$SRC" out_full_folder && ok full-folder || bad full-folder
fi

# corrupted xbe: refused, user's files untouched, no .part left behind
mkdir -p badsrc/media && cp out_redump/default.xbe badsrc/ && cp out_redump/media/Sound.xsb badsrc/media/
$PY -c "f=open('badsrc/default.xbe','r+b');f.seek(4096);b=f.read(1);f.seek(4096);f.write(bytes([b[0]^255]))"
mkdir -p out_user && echo mine > out_user/keep.txt && echo old > out_user/default.xbe
"$B" badsrc out_user >/dev/null 2>&1; r=$?
[ $r -ne 0 ] && [ "$(cat out_user/default.xbe)" = old ] && [ -f out_user/keep.txt ] && [ ! -e out_user/media ] \
  && [ -z "$(ls out_user | grep part)" ] && ok corrupt-xbe || bad corrupt-xbe

# traversal names inside an image
echo evil > evil.txt
mkdir -p goodsrc/media && cp out_redump/default.xbe goodsrc/ && cp out_redump/media/Sound.xsb goodsrc/media/
for n in '..' '../x' '..\x' 'a\b' 'a/b' 'c:x'; do
  $PY "$T/mkxiso.py" goodsrc trav.iso --extra media "$n" evil.txt >/dev/null
  "$B" trav.iso out_trav >/dev/null 2>&1; r=$?
  [ $r -ne 0 ] && [ ! -e out_trav ] && ok "traversal [$n]" || bad "traversal [$n]"
done

# refused output paths
for o in "" "/" "C:\\" "D:" "." ".."; do
  # MSYS_NO_PATHCONV: stop Git Bash rewriting "/" into its install folder
  if MSYS_NO_PATHCONV=1 "$B" goodsrc "$o" >/dev/null 2>&1; then bad "outpath [$o]"; else ok "outpath [$o]"; fi
done
echo "passed $pass, failed $fail"
[ $fail -eq 0 ]
