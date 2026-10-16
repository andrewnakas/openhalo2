#!/bin/bash
# burst.sh <outdir> [extra env...]: Ivory Tower, then frame dumps of every flip during a
# 1.5 s burst of fire and during a grenade throw; sheets fire_sheet.png / nade_sheet.png.
O=$1; shift; mkdir -p "$O"
S=$(dirname "$0")
bash $S/mpgame_x.sh "$O" 400 "$@" > "$O/mp.txt" 2>&1 || { echo "mpgame failed"; exit 1; }
sleep 8
mkdir -p "$O/fire" "$O/nade"
rm -f "$O"/fb*.bmp; echo "rt:3000" >> "$O/pad.txt"; sleep 0.6; touch "$O/dumpflag"; sleep 1.2; rm -f "$O/dumpflag"; sleep 0.5
mv "$O"/fb*.bmp "$O/fire/" 2>/dev/null
sleep 3
rm -f "$O"/fb*.bmp; echo "lt:300" >> "$O/pad.txt"; sleep 2.2; touch "$O/dumpflag"; sleep 2.0; rm -f "$O/dumpflag"; sleep 0.5
mv "$O"/fb*.bmp "$O/nade/" 2>/dev/null
taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1
for k in fire nade; do
python - "$O/$k" "$O/${k}_sheet.png" <<'PY'
import sys,glob
from PIL import Image
fs=sorted(glob.glob(sys.argv[1]+'/fb*.bmp'))[:30]
W,H=320,240; s=Image.new('RGB',(W*6,H*((len(fs)+5)//6 or 1)))
for i,f in enumerate(fs): s.paste(Image.open(f).convert('RGB').resize((W,H)),((i%6)*W,(i//6)*H))
s.save(sys.argv[2]); print(sys.argv[2], len(fs), 'frames')
PY
done
