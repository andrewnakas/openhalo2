#!/bin/bash
# apu2/menu.sh <outdir> [secs] [ENV=val...]: boot headless with sound on (no
# device), sit through the attract/title and press Start into the main menu,
# recording the mix (mix.wav, from start) and, unless APU_NOTRACE is set, the
# APU register trace (trace.bin). Ends after <secs> (default 90) of mix.
O=${1:?outdir}; S=${2:-90}; shift; shift
case "$O" in ""|/|/d|/d/) echo "refusing outdir '$O'"; exit 1;; esac
mkdir -p "$O"; : > "$O/pad.txt"; rm -f "$O/mix.wav" "$O/trace.bin" "$O/exit.txt"
W=$(cygpath -m "$O")
TR=""; [ -z "$APU_NOTRACE" ] && TR="RECOMP_APU_TRACE=$W/trace.bin,${TRACE_MB:-1500}"
cd ${XRDIR:-/d/n64work/halo2/build/xr}
(env RECOMP_SOUND=1 RECOMP_APU_MUTE=1 RECOMP_APU_WAV="$W/mix.wav,$S" RECOMP_APU_WAV_FROM_START=1 \
    RECOMP_APU_BINS=1 RECOMP_APU_VOICES=1 $TR "$@" RECOMP_WATCHDOG_SECS=$((S + 120)) \
    RECOMP_PB_REPORT_MS=5000 RECOMP_PAD_LIVE=$W/pad.txt RECOMP_FB_DUMP_FLIPS=$W/dumpflag \
    RECOMP_FB_DUMP=$W/fb ./halo2_recomp.exe > "$O/out.log" 2>&1; echo "exit=$? at $SECONDS" > "$O/exit.txt") &
T0=$SECONDS
until [ "$(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/')" -ge 1100 ] 2>/dev/null; do
  sleep 2; [ -f "$O/exit.txt" ] && { echo "died: $(cat "$O/exit.txt")"; exit 1; }
  [ $((SECONDS - T0)) -gt 200 ] && break
done
echo "title at t=$((SECONDS - T0))" > "$O/marks.txt"
sleep 4; echo "start:1500" >> "$O/pad.txt"; sleep 8; echo "a:300" >> "$O/pad.txt"
echo "menu at t=$((SECONDS - T0))" >> "$O/marks.txt"
while [ ! -f "$O/exit.txt" ]; do
  [ -f "$O/mix.wav" ] && python -c "import os,sys;sys.exit(0 if os.path.getsize(sys.argv[1])>=$S*192000 else 1)" "$O/mix.wav" 2>/dev/null && break
  [ $((SECONDS - T0)) -gt $((S + 200)) ] && break
  sleep 3
done
sleep 2; taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1; sleep 1
echo "end t=$((SECONDS - T0)) wav=$(stat -c %s "$O/mix.wav" 2>/dev/null) trace=$(stat -c %s "$O/trace.bin" 2>/dev/null)" | tee -a "$O/marks.txt"
