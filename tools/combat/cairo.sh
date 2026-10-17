#!/bin/bash
# cairo.sh <outdir> [extra env...]: boot Cairo Station from Select Level on a
# save that has it unlocked (the owner's profile copied into XRDIR/save), then
# keep taking a screenshot every SHOT_EVERY seconds (default 8) for SHOT_N shots
# (default 30) while the game runs; sheet.png at the end. Drive the player by
# appending to <outdir>/pad.txt meanwhile. XRDIR/EXE pick the build (use a
# renamed exe: the watchdog is the only stop, nothing is killed by image name
# except EXE itself at the end).
O=${1:?outdir}; shift; mkdir -p "$O"
T=/d/n64work/openhalo2/tools
CAMPAIGN_DOWN=$CAMPAIGN_DOWN CAMPAIGN_HOLD=120 LEVEL_PICK=${LEVEL_PICK:-136} LOADWAIT=${LOADWAIT:-60} \
  bash $T/ingame.sh "$O" ${WD:-900} RECOMP_GPU_AB="${AB:-$O/ab}" "$@" > "$O/ingame.txt" 2>&1 \
  || { echo "ingame failed: $(tail -1 "$O/ingame.txt")"; exit 1; }
mkdir -p "${AB:-$O/ab}"
for i in $(seq -w 1 ${SHOT_N:-30}); do
  [ -f "$O/stop" ] && break
  bash $T/shot.sh "$O" "c$i" > /dev/null
  sleep ${SHOT_EVERY:-8}
done
python - "$O" <<'EOF'
import sys, glob
from PIL import Image
o = sys.argv[1]
fs = sorted(glob.glob(o + '/c[0-9]*.png'))
W, H = 320, 240
s = Image.new('RGB', (W * 5, H * ((len(fs) + 4) // 5 or 1)))
for i, f in enumerate(fs):
    s.paste(Image.open(f).convert('RGB').resize((W, H)), ((i % 5) * W, (i // 5) * H))
s.save(o + '/sheet.png'); print(o + '/sheet.png', len(fs), 'shots')
EOF
