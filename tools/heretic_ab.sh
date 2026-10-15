#!/bin/bash
# heretic_ab.sh <outdir>: boot the retail build into The Heretic (Select Level:
# up from the Armory) with RECOMP_GPU_AB=<outdir>/ab, wait for the opening, and
# capture the same moment as normal, shadowlit and shadowflip -> ab.png.
O=${1:?outdir}
T=/d/n64work/openhalo2/tools
bash $T/boot_to_menu.sh "$O" RECOMP_GPU_AB="$O/ab" RECOMP_GL_PROBE="320,240,$(cygpath -m "$O")/probe" > /dev/null
mkdir -p "$O/ab"
# The main menu reopens on the item last used: go up until Campaign is lit
# (rows at y = 287 + 28n, read past the end of the text; see ingame.sh).
for i in 1 2 3 4 5 6; do
  bash $T/shot.sh "$O" row > /dev/null
  r=$(python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');v=[sum(im.crop((405,y-5,435,y+5)).getdata()) for y in (287,315,343,371,399)];print(v.index(max(v)))" "$O/row.png" 2>/dev/null)
  [ "$r" = 0 ] && break; echo "up:150" >> "$O/pad.txt"; sleep 2
done
for k in a:300 a:300; do echo "$k" >> "$O/pad.txt"; sleep 7; done   # campaign, new campaign
# Select Level: up until The Heretic's row (y 92) is the lit one, not the
# Armory's (y 114).
for i in 1 2 3 4 5 6; do
  sleep 3; bash $T/shot.sh "$O" lvl > /dev/null
  h=$(python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');a=sum(im.crop((140,86,330,98)).getdata());b=sum(im.crop((140,108,330,120)).getdata());print(1 if a>b else 0)" "$O/lvl.png" 2>/dev/null)
  [ "$h" = 1 ] && break; echo "up:150" >> "$O/pad.txt"
done
for k in a:300 a:300; do echo "$k" >> "$O/pad.txt"; sleep 7; done   # level, difficulty
sleep ${WAIT:-60}
echo 320,240 > "$O/probe"; sleep 0.5; rm -f "$O/probe"
bash $T/shot.sh "$O" n0 > /dev/null
touch "$O/ab/shadowlit"; sleep 1.5; bash $T/shot.sh "$O" lit > /dev/null; rm -f "$O/ab/shadowlit"
touch "$O/ab/shadowflip"; sleep 1.5; bash $T/shot.sh "$O" flip > /dev/null; rm -f "$O/ab/shadowflip"
sleep 1; bash $T/shot.sh "$O" n1 > /dev/null
python -c "
import sys
from PIL import Image, ImageDraw
d=sys.argv[1]; names=['n0','lit','flip','n1']
s=Image.new('RGB',(1280,256),(30,30,30)); dr=ImageDraw.Draw(s)
for k,n in enumerate(names):
    s.paste(Image.open(d+'/'+n+'.png').convert('RGB').resize((320,240)),(k*320,16)); dr.text((k*320+4,2),n,fill=(255,255,0))
s.save(d+'/ab.png')" "$O"
grep -a "shadow buffers" "$O/out.log" | tail -1
