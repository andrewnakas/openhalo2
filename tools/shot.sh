#!/bin/bash
# shot.sh <report dir> [label]: dump a few presented frames now, keep the last
# as <label>.png and remove the BMP dumps this run wrote into that directory.
O=${1:?report dir}; L=${2:-shot}
touch "$O/dumpflag"; sleep 0.6; rm -f "$O/dumpflag"; sleep 0.3
f=$(ls -t "$O"/fb*.bmp 2>/dev/null | sed -n 2p); [ -z "$f" ] && f=$(ls -t "$O"/fb*.bmp 2>/dev/null | head -1)
[ -z "$f" ] && { echo "no frame"; exit 0; }
python -c "import sys;from PIL import Image;Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" "$f" "$O/$L.png"
find "$O" -maxdepth 1 -name 'fb*.bmp' -delete
echo "$L.png  $(grep -a 'fps (' "$O/out.log" | tail -1)"
