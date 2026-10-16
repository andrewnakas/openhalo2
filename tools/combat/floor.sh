#!/bin/bash
# floor.sh <outdir>: Ivory Tower, look down at the floor at three pitches; each view shot with
# the new clipper and with the old one (RECOMP_GPU_AB oldclip), sheet floor_sheet.png.
O=$1; mkdir -p "$O"; S=$(dirname "$0")
bash $S/mpgame_x.sh "$O" 300 > "$O/mp.txt" 2>&1 || { echo "mpgame failed"; exit 1; }
sleep 6
for k in 1 2 3; do
  echo "rdown:350" >> "$O/pad.txt"; sleep 2
  rm -f "$O/oldclip"; sleep 0.5; bash /d/n64work/openhalo2/tools/shot.sh "$O" v${k}_new > /dev/null
  touch "$O/oldclip"; sleep 0.5; bash /d/n64work/openhalo2/tools/shot.sh "$O" v${k}_old > /dev/null
  rm -f "$O/oldclip"
done
taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1
python - "$O" <<'PY'
import sys
from PIL import Image
O=sys.argv[1]; s=Image.new('RGB',(960,1080))
for k in range(3):
    for j,t in enumerate(('new','old')):
        s.paste(Image.open('%s/v%d_%s.png'%(O,k+1,t)).convert('RGB').resize((480,360)),(j*480,k*360))
s.save(O+'/floor_sheet.png'); print(O+'/floor_sheet.png')
PY
