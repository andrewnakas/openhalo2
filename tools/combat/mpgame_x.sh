#!/bin/bash
# mpgame.sh <outdir> <watchdog secs> [extra env assignments...]
# Boots headless, walks the menus with ingame.sh's presses (with Split Screen
# remembered as the last main-menu choice they end at or near the pregame
# lobby), then keeps pressing A until the frame rate says a map is running
# (menus present at 50-60, a map at 30 or less). The map is the one the lobby
# remembers (Ivory Tower = cyclotron.map at the time of writing). About 4
# minutes. A 300 ms press is sometimes missed; this uses 500.
# A/B switches while it runs: touch <outdir>/oldidx | nocull | nopoly |
# nozpersp | nostipple (RECOMP_GPU_AB). Drive with pad.txt and tools/shot.sh.
OUT=$1; WD=$2; shift 2
AHOLD=500 LOADWAIT=4 MENU_ROW=2 bash /d/n64work/openhalo2/tools/combat/ingame_x.sh "$OUT" "$WD" RECOMP_GPU_AB=$OUT "$@" || exit 1
fpsn() { grep -a 'fps (' "$OUT/out.log" | tail -$1 | head -1 | sed 's/.*\] \([0-9]*\)\..*/\1/'; }
for i in 1 2 3 4 5 6 7 8; do
  a=$(fpsn 1); b=$(fpsn 2)
  [ "${a:-99}" -lt 38 ] && [ "${b:-99}" -lt 38 ] && break
  echo "a:500" >> "$OUT/pad.txt"; sleep 13
done
sleep ${MPLOAD:-45}
tasklist 2>/dev/null | grep -q halo2_recomp || { echo "died during map load"; exit 1; }
echo "MARK mpgame at t=$SECONDS" >> "$OUT/marks.txt"
echo "in map: $(grep -a 'fps (' "$OUT/out.log" | tail -1)"
