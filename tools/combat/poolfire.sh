#!/bin/bash
# poolfire.sh <outdir>: Ivory Tower, then data-array counts (tools/pools.py) before,
# during a burst of fire, and after a grenade. pools.txt
O=$1; shift; mkdir -p "$O"; T=/d/n64work/openhalo2/tools
bash $T/combat/mpgame_x.sh "$O" 500 "$@" > "$O/mp.txt" 2>&1 || { echo "mpgame failed"; exit 1; }
sleep 8
{ echo "== before"; python $T/pools.py fx; } > "$O/pools.txt"
echo "rt:3000" >> "$O/pad.txt"; sleep 1.0
{ echo "== firing"; python $T/pools.py fx; } >> "$O/pools.txt"
bash $T/shot.sh "$O" firing > /dev/null
sleep 3
echo "lt:300" >> "$O/pad.txt"; sleep 2.5
{ echo "== grenade"; python $T/pools.py fx; } >> "$O/pools.txt"
bash $T/shot.sh "$O" nade > /dev/null
taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1
grep -c . "$O/pools.txt"
