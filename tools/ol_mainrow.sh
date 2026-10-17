#!/bin/bash
# ol_mainrow.sh <outdir> <row>: moves the main menu's highlight to row
# (0 Campaign, 1 Xbox Live, 2 Split Screen, 3 System Link, 4 Settings) on a
# running copy, reading the lit row from the screen (rows at y = 287 + 28 n,
# the brightest band between x 250 and 400), as tools/sl_pair.sh does.
O=${1:?outdir}; R=${2:?row}
for i in 1 2 3 4 5 6 7 8; do
  bash /d/n64work/openhalo2/tools/shot.sh "$O" row > /dev/null
  r=$(python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');v=[sum(im.crop((250,y-6,400,y+6)).get_flattened_data()) for y in (287,315,343,371,399)];print(v.index(max(v)))" "$O/row.png")
  [ "$r" = "$R" ] && { echo "main menu on row $r"; exit 0; }
  [ "$r" -lt "$R" ] && echo "down:400" >> "$O/pad.txt" || echo "up:400" >> "$O/pad.txt"
  sleep 3
done
echo "main menu: row $R not reached (on $r)"; exit 1
