#!/bin/bash
# perf_ivory.sh <outdir> [extra env...]: Ivory Tower (mpgame_x.sh, the lobby's
# remembered map) for the GPU-executor frame-rate comparisons: once the map
# runs, walk, turn, fire and throw grenades for ~90 s, then let the watchdog
# (WD, default 330 s) end the run. Nothing is killed by image name: use a
# renamed exe (EXE=..., XRDIR=...). perf_summary.py <outdir> mpgame 90 reads it.
O=${1:?outdir}; shift; mkdir -p "$O"
T=/d/n64work/openhalo2/tools
bash $T/combat/mpgame_x.sh "$O" ${WD:-330} "$@" > "$O/mp.txt" 2>&1 || { echo "mpgame failed: $(tail -1 "$O/mp.txt")"; exit 1; }
bash $T/shot.sh "$O" start > /dev/null
for r in 1 2 3; do
  echo "lup:1500" >> "$O/pad.txt"; sleep 2
  for i in 1 2 3 4 5 6; do echo "rt:300" >> "$O/pad.txt"; sleep 0.6; done
  echo "rright:600" >> "$O/pad.txt"; sleep 1
  echo "lt:300" >> "$O/pad.txt"; sleep 2
  bash $T/shot.sh "$O" fight$r > /dev/null
  echo "lup:1500" >> "$O/pad.txt"; sleep 2
  echo "rleft:500" >> "$O/pad.txt"; sleep 1
  for i in 1 2 3 4 5 6; do echo "rt:300" >> "$O/pad.txt"; sleep 0.6; done
  sleep 6
done
echo "MARK fought at t=$SECONDS" >> "$O/marks.txt"
while tasklist 2>/dev/null | grep -q "${EXE:-halo2_recomp.exe}"; do sleep 5; done
echo done
