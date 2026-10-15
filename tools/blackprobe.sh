#!/bin/bash
# blackprobe.sh <report dir> <count> [x,y]: during a run started with
# RECOMP_GL_PROBE=x,y,<dir>/probeflag, take a frame every ~4 s; when it is black
# (mean luma < 8) turn the probe on for about a second and keep the frame as
# b<i>.png, else keep it as n<i>.png. Probe lines land in <dir>/out.log.
O=${1:?dir}; N=${2:-20}; XY=${3:-320,300}
for i in $(seq 1 $N); do
  bash /d/n64work/openhalo2/tools/shot.sh "$O" cur > /dev/null
  m=$(python -c "import sys;from PIL import Image,ImageStat;print(int(ImageStat.Stat(Image.open(sys.argv[1]).convert('L')).mean[0]))" "$O/cur.png" 2>/dev/null)
  if [ -n "$m" ] && [ "$m" -lt 8 ]; then
    echo "$XY" > "$O/probeflag"; echo "PROBE ON $i at $(grep -a 'fps (' "$O/out.log" | tail -1)" >> "$O/marks.txt"
    sleep 1; rm -f "$O/probeflag"; mv "$O/cur.png" "$O/b$i.png"; echo "$i black ($m)"
  else
    mv "$O/cur.png" "$O/n$i.png"; echo "$i ok ($m)"
  fi
  sleep 3
done
