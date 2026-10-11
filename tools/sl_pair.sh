#!/bin/bash
# sl_pair.sh <outdir> [watchdog secs]
# Starts two native copies on the local frame hub and walks both to the
# System Link "Available Games" list: copy A from build/xr, copy B from
# build/xr_b (its own game and save folders; the exe is copied over first).
# Reports land in <outdir>/a and <outdir>/b; drive each with its pad.txt and
# tools/shot.sh, and put both screens side by side with
#   bash tools/sl_pair.sh --sheet <outdir> <label>
# The main menu remembers its last item, so the walk reads which row is lit
# (rows at y = 287 + 28 n) and steps to row 3, System Link.
T=/d/n64work/openhalo2/tools
if [ "$1" = "--sheet" ]; then
  O=$2; L=$3
  for x in a b; do bash $T/shot.sh $O/$x $L | tail -1; done
  python - "$O" "$L" <<'EOF'
import sys
from PIL import Image
o, l = sys.argv[1:3]
w = Image.new('RGB', (1280, 480))
for i, x in enumerate('ab'):
    w.paste(Image.open(f'{o}/{x}/{l}.png').convert('RGB'), (i * 640, 0))
w.save(f'{o}/{l}.png')
EOF
  exit 0
fi
O=${1:?outdir}; WD=${2:-900}
cp /d/n64work/halo2/build/xr/halo2_recomp.exe /d/n64work/halo2/build/xr_b/halo2_recomp.exe
for x in a b; do
  D=$O/$x; mkdir -p $D; rm -f $D/dumpflag $D/fb*.bmp; : > $D/pad.txt
  [ $x = a ] && X=/d/n64work/halo2/build/xr || X=/d/n64work/halo2/build/xr_b
  ( cd $X && env "${@:3}" RECOMP_WATCHDOG_SECS=$WD RECOMP_PB_REPORT_MS=5000 \
      RECOMP_PAD_LIVE=$D/pad.txt RECOMP_FB_DUMP_FLIPS=$D/dumpflag RECOMP_FB_DUMP=$D/fb \
      RECOMP_NIC_FRAMES=${NICF:-1} RECOMP_APU_MUTE=1 RECOMP_CRYPTO_TRACE=1 \
      ./halo2_recomp.exe > $D/out.log 2>&1 & )
done
flips() { grep -a "fps (" "$O/$1/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/'; }
for x in a b; do
  for i in $(seq 60); do f=$(flips $x); [ -n "$f" ] && [ "$f" -ge 1000 ] && break; sleep 2; done
done
sleep 5
press() { for x in a b; do echo "$1" >> $O/$x/pad.txt; done; sleep ${2:-7}; }
press start:1500; press a:500; press a:500 9
row() {   # which main-menu row is lit on copy $1
  bash $T/shot.sh $O/$1 row > /dev/null
  python - "$O/$1/row.png" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('L')
v = [sum(im.crop((250, y - 6, 400, y + 6)).getdata()) for y in (287, 315, 343, 371, 399)]
print(v.index(max(v)))
EOF
}
for x in a b; do
  for i in 1 2 3 4 5 6; do
    r=$(row $x); [ "$r" = 3 ] && break
    [ "$r" -lt 3 ] && echo "down:120" >> $O/$x/pad.txt || echo "up:120" >> $O/$x/pad.txt
    sleep 2
  done
  echo "copy $x on main-menu row $r"
done
press a:500 8      # System Link -> player screen
press a:400 10     # -> Available Games
for x in a b; do echo "$x: $(grep -a 'fps (' $O/$x/out.log | tail -1)  $(grep -a 'Ethernet address' $O/$x/out.log | tail -1)"; done
