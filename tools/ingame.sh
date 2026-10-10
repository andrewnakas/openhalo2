#!/bin/bash
# ingame.sh <outdir> <watchdog secs> [extra env assignments...]
# Boots headless and walks Start -> profile -> Don't sign in -> Campaign ->
# New Campaign -> level -> difficulty, then returns once the armory is up
# (about 2.5 minutes). The game keeps running until the watchdog; drive it by
# appending to <outdir>/pad.txt and grab frames with tools/shot.sh.
# SHOTS=1 in the environment saves a PNG at every step (m_<step>.png).
# STEP (7 s between presses), SHOLD/AHOLD (hold ms), TITLE_FLIPS, LOADWAIT
# tune it; at software speed use STEP=25 SHOLD=3000 AHOLD=3000 TITLE_FLIPS=600.
# A boot whose pad is dead (it happens) stops with "pad press not consumed".
OUT=$1; WD=$2; shift 2
mkdir -p "$OUT"
rm -f "$OUT/dumpflag"
: > "$OUT/pad.txt"
cd ${XRDIR:-/d/n64work/halo2/build/xr}   # XRDIR=/d/n64work/halo2/build/xr32 runs the 32-bit build
env "$@" RECOMP_WATCHDOG_SECS=$WD RECOMP_PB_REPORT_MS=${REPORT_MS:-5000} \
    RECOMP_PAD_LIVE=$OUT/pad.txt RECOMP_FB_DUMP_FLIPS=$OUT/dumpflag \
    RECOMP_FB_DUMP=$OUT/fb ./halo2_recomp.exe > "$OUT/out.log" 2>&1 &
flips() { grep -a "fps (" "$OUT/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/'; }
alive() { tasklist 2>/dev/null | grep -q halo2_recomp; }
mark() { echo "MARK $1 at flips $(flips) t=$SECONDS" >> "$OUT/marks.txt"
         [ -n "$SHOTS" ] && bash /d/n64work/openhalo2/tools/shot.sh "$OUT" "m_$1" > /dev/null; }
: > "$OUT/marks.txt"
sleep 5
while alive; do f=$(flips); [ -n "$f" ] && [ "$f" -ge ${TITLE_FLIPS:-1000} ] && break; sleep 2; done
alive || { echo "died before title" | tee -a "$OUT/marks.txt"; exit 1; }
sleep 4; mark title
S=${STEP:-7}
echo "start:${SHOLD:-1500}" >> "$OUT/pad.txt"; sleep $S; mark profile
grep -a -q 'PAD: live "' "$OUT/out.log" || { echo "pad press not consumed" | tee -a "$OUT/marks.txt"; exit 1; }
for n in profile nosignin campaign newcampaign level difficulty; do
  echo "a:${AHOLD:-300}" >> "$OUT/pad.txt"; sleep $S; mark "after_$n"
done
sleep ${LOADWAIT:-75}
alive || { echo "died during load" | tee -a "$OUT/marks.txt"; exit 1; }
mark ingame
echo "in game: $(grep -a 'fps (' "$OUT/out.log" | tail -1)"
