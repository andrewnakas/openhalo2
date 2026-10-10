#!/bin/bash
# drive.sh <outdir> <watchdog secs> <step wait secs> [extra env assignments...]
# Boots headless, waits for the title screen, then walks Start -> Choose
# Profile -> Default -> Xbox Live prompt -> main menu, dumping a few presented
# frames at each screen (flag-file gated).
OUT=$1; WD=$2; WAIT=$3; shift 3
mkdir -p "$OUT"
rm -f "$OUT/pad.txt" "$OUT/dumpflag"
: > "$OUT/pad.txt"
cd ${XRDIR:-/d/n64work/halo2/build/xr}   # XRDIR=/d/n64work/halo2/build/xr32 runs the 32-bit build
env "$@" RECOMP_WATCHDOG_SECS=$WD RECOMP_PB_REPORT_MS=${REPORT_MS:-5000} \
    RECOMP_PAD_LIVE=$OUT/pad.txt RECOMP_FB_DUMP_FLIPS=$OUT/dumpflag \
    RECOMP_FB_DUMP=$OUT/fb ./halo2_recomp.exe > "$OUT/out.log" 2>&1 &
flips() { grep -a "fps (" "$OUT/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/'; }
alive() { tasklist 2>/dev/null | grep -q halo2_recomp; }
mark() { echo "MARK $1 at flips $(flips) t=$SECONDS" >> "$OUT/marks.txt"; }
shot() { touch "$OUT/dumpflag"; sleep $1; rm -f "$OUT/dumpflag"; }
: > "$OUT/marks.txt"
sleep 5
if [ -n "$PRE_FLIPS" ]; then
  while alive; do f=$(flips); [ -n "$f" ] && [ "$f" -ge $PRE_FLIPS ] && break; sleep 0.3; done
  mark pre; shot ${PRE_SHOT:-20}
fi
# the title screen follows the two logo videos (about flip 560)
while alive; do f=$(flips); [ -n "$f" ] && [ "$f" -ge ${TITLE_FLIPS:-640} ] && break; sleep 2; done
alive || { echo "died before title" >> "$OUT/marks.txt"; exit 1; }
mark title; shot ${SHOT:-1}
[ -z "$SKIP_T2" ] && { sleep $WAIT; mark title2; shot ${SHOT:-1}; }
echo "start:3000" >> "$OUT/pad.txt"; sleep $WAIT; sleep $WAIT
mark profile; shot ${SHOT:-1}
[ -z "$NODOWN" ] && { echo "down:1500" >> "$OUT/pad.txt"; sleep $WAIT; }
echo "a:${AHOLD:-1500}" >> "$OUT/pad.txt"; sleep $WAIT; sleep $WAIT
mark live; shot ${SHOT:-1}
echo "a:${AHOLD:-1500}" >> "$OUT/pad.txt"; sleep $WAIT; sleep $WAIT
mark menu; shot ${SHOT:-1}
sleep $WAIT; sleep $WAIT
mark menu2; shot ${SHOT:-1}
echo "a:${AHOLD:-1500}" >> "$OUT/pad.txt"; sleep $WAIT; sleep $WAIT
mark campaign; shot ${SHOT:-1}
while alive; do sleep 2; done
echo done >> "$OUT/marks.txt"
