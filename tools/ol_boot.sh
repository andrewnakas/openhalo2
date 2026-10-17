#!/bin/bash
# ol_boot.sh <outdir> [extra env assignments...]
# Open Live rig: boots one headless copy with RECOMP_OPEN_LIVE=1 and the
# XOnline trace, waits for the title, presses Start and A (Choose Profile)
# and saves s1.png (the "Sign in to Xbox Live" list). Drive further by
# appending to <outdir>/pad.txt and grabbing frames with tools/shot.sh.
# XRDIR picks the run folder (default build/xr; build/xr_b is the second copy).
O=${1:?outdir}; shift
case "$O" in ""|/|/d|/d/|D:|D:/) echo "refusing outdir '$O'"; exit 1;; esac
rm -rf "$O"; mkdir -p "$O"; : > "$O/pad.txt"
X=${XRDIR:-/d/n64work/halo2/build/xr}
[ "$X" = /d/n64work/halo2/build/xr ] || cp /d/n64work/halo2/build/xr/halo2_recomp.exe "$X/halo2_recomp.exe"
cd "$X"
env RECOMP_OPEN_LIVE=1 RECOMP_XO_TRACE=${XOT:-1} "$@" RECOMP_APU_MUTE=1 RECOMP_WATCHDOG_SECS=${WD:-1500} \
    RECOMP_PB_REPORT_MS=5000 RECOMP_PAD_LIVE=$O/pad.txt RECOMP_FB_DUMP_FLIPS=$O/dumpflag \
    RECOMP_FB_DUMP=$O/fb ./halo2_recomp.exe > "$O/out.log" 2>&1 &
cat /proc/$!/winpid > "$O/winpid" 2>/dev/null   # tools/ol_kill.sh <outdir> stops this copy only
sleep 5
until [ "$(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/')" -ge 1100 ] 2>/dev/null; do sleep 2; done
sleep 4
for k in start:1500 a:300; do echo "$k" >> "$O/pad.txt"; sleep 8; done
bash /d/n64work/openhalo2/tools/shot.sh "$O" s1
# SIGNIN=1: pick the first account (the row under "Don't Sign In") and wait
# for the sign-in to finish (s2.png)
if [ -n "$SIGNIN" ]; then
  echo down:150 >> "$O/pad.txt"; sleep 3; echo a:300 >> "$O/pad.txt"; sleep 15
  bash /d/n64work/openhalo2/tools/shot.sh "$O" s2
fi
