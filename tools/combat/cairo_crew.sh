#!/bin/bash
# cairo_crew.sh <outdir> [extra env...]: Cairo Station from Select Level (cairo.sh),
# through the opening cinematic, then turn and walk to the hall where the
# white-uniformed crew walk past the marines (the "white silhouettes" of the
# owner's 2026-10-17 playtest). Shots w1..w6 on the way; the game keeps running
# (watchdog WD) with RECOMP_GL_PROBE armed on <outdir>/probe for probing.
O=${1:?outdir}; shift; mkdir -p "$O"
T=/d/n64work/openhalo2/tools
SHOT_N=${CINE_SHOTS:-24} SHOT_EVERY=6 bash $T/combat/cairo.sh "$O" \
  RECOMP_GL_PROBE="${PROBE_MODE:-320,240},$(cygpath -m "$O")/probe" "$@" || exit 1
for i in 1 2 3 4 5 6 7 8; do echo "rright:300" >> "$O/pad.txt"; sleep 1.5; done
bash $T/shot.sh "$O" turned > /dev/null
# AB_FLAG=<name>: at each step also a pair with that A/B file (abpairs.sh, prefix w<i>)
for i in 1 2 3 4 5 6; do
  echo "lup:1200" >> "$O/pad.txt"; [ $i = 2 ] && [ -n "$PROBE_MODE" ] && touch "$O/probe"; sleep 1.8
  bash $T/shot.sh "$O" w$i > /dev/null
  [ -n "$AB_FLAG" ] && bash $T/combat/abpairs.sh "$O" "$AB_FLAG" 1 w$i > /dev/null
done; rm -f "$O/probe"
echo walked
