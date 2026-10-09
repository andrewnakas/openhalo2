#!/bin/bash
# padtest.sh <outdir> [extra env assignments...]
# 30-second check that the title is reading the scripted pad: boots, presses
# start during the logo videos and reports whether the press was consumed.
OUT=$1; shift
mkdir -p "$OUT"
: > "$OUT/pad.txt"
cd /d/n64work/halo2/build/xr
env "$@" RECOMP_WATCHDOG_SECS=40 RECOMP_PB_REPORT_MS=5000 \
    RECOMP_PAD_LIVE=$OUT/pad.txt ./halo2_recomp.exe > "$OUT/padtest.log" 2>&1 &
sleep 22
echo "start:300" >> "$OUT/pad.txt"
sleep 4
echo "pad lines consumed: $(grep -a -c 'PAD: live \"' "$OUT/padtest.log")  $(grep -a 'operational after' "$OUT/padtest.log" | sed 's/.*operational/operational/')"
taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1
sleep 1
