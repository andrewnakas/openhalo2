#!/bin/bash
# nearwalk.sh <outdir> [extra env...]: Ivory Tower, then walk forward in short steps,
# turning a little between them, with a screenshot after each step (step01..step12.png)
# and a sheet nearwalk.png. For near-object culling A/B: run once with
# RECOMP_GPU_QUERY_NEAR=0 and once without, and compare sheets and the
# "visibility test ... clipped near the eye" lines in out.log. XRDIR picks the build.
O=$1; shift; mkdir -p "$O"
T=/d/n64work/openhalo2/tools
bash $T/combat/mpgame_x.sh "$O" 500 "$@" > "$O/mp.txt" 2>&1 || { echo "mpgame failed"; exit 1; }
sleep 6
for i in $(seq -w 1 12); do
  echo "lup:900" >> "$O/pad.txt"; sleep 1.2
  bash $T/shot.sh "$O" "step$i" > /dev/null
  [ $((10#$i % 3)) = 0 ] && { echo "rright:250" >> "$O/pad.txt"; sleep 0.5; }
done
taskkill //F //IM ${EXE:-halo2_recomp.exe} > /dev/null 2>&1
python - "$O" <<'EOF'
import sys, glob
from PIL import Image
o = sys.argv[1]
fs = sorted(glob.glob(o + '/step*.png'))
W, H = 320, 240
s = Image.new('RGB', (W * 4, H * ((len(fs) + 3) // 4 or 1)))
for i, f in enumerate(fs):
    s.paste(Image.open(f).convert('RGB').resize((W, H)), ((i % 4) * W, (i // 4) * H))
s.save(o + '/nearwalk.png'); print(o + '/nearwalk.png', len(fs), 'shots')
EOF
grep -a -c "clipped near the eye" "$O/out.log"
