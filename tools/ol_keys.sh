#!/bin/bash
# ol_keys.sh <outdir> <label> key[:ms][@wait] ...
# Presses keys on a running copy (its pad.txt) and saves <label>.png after
# the last one. Each key waits `wait` seconds after it (default 3), e.g.
#   bash tools/ol_keys.sh $O quick a@8          (A, wait 8 s, shot)
#   bash tools/ol_keys.sh $O row down down a@6
O=${1:?outdir}; L=${2:?label}; shift 2
for k in "$@"; do
  w=3; case "$k" in *@*) w=${k#*@}; k=${k%@*};; esac
  case "$k" in *:*) ;; *) k=$k:${HOLD:-500};; esac
  echo "$k" >> "$O/pad.txt"; sleep "$w"
done
bash /d/n64work/openhalo2/tools/shot.sh "$O" "$L"
