#!/bin/bash
# boot_to_menu.sh <outdir> [extra env assignments...]
# Boots headless (retail build unless XRDIR is set) with a live pad file and
# a frame-trace flag, presses Start, A, A (profile, sign-in) and saves s1.png.
# Drive further by appending to <outdir>/pad.txt; grab frames with shot.sh.
O=${1:?outdir}; shift
case "$O" in ""|/|/d|/d/) echo "refusing outdir '$O'"; exit 1;; esac
rm -rf "$O"; mkdir -p "$O"; : > "$O/pad.txt"
cd ${XRDIR:-/d/n64work/halo2/build/xr}
env "$@" RECOMP_APU_MUTE=1 RECOMP_WATCHDOG_SECS=1800 RECOMP_PB_REPORT_MS=5000 \
    RECOMP_PAD_LIVE=$O/pad.txt RECOMP_FB_DUMP_FLIPS=$O/dumpflag RECOMP_FB_DUMP=$O/fb \
    RECOMP_FRAME_TRACE="$O/trace" ./halo2_recomp.exe > "$O/out.log" 2>&1 &
sleep 5
until [ "$(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/')" -ge 1100 ] 2>/dev/null; do sleep 2; done
sleep 4
for k in start:1500 a:300 a:300; do echo "$k" >> "$O/pad.txt"; sleep 8; done
bash /d/n64work/openhalo2/tools/shot.sh "$O" s1
