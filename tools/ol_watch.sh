#!/bin/bash
# ol_watch.sh <outdir> <seconds> [extra env...]: boots one Open Live copy
# (XRDIR, default build/xr) without pressing anything and saves a shot every
# 10 s (w<n>.png) for <seconds>, to see where a boot lands. Stop it with
# tools/ol_kill.sh <outdir>.
O=${1:?outdir}; S=${2:?seconds}; shift 2
case "$O" in ""|/|/d|/d/|D:|D:/) echo "refusing outdir '$O'"; exit 1;; esac
rm -rf "$O"; mkdir -p "$O"; : > "$O/pad.txt"
X=${XRDIR:-/d/n64work/halo2/build/xr}
[ "$X" = /d/n64work/halo2/build/xr ] || cp /d/n64work/halo2/build/xr/halo2_recomp.exe "$X/halo2_recomp.exe"
cd "$X"
env RECOMP_NO_HOST_PAD=1 RECOMP_OPEN_LIVE=1 RECOMP_XO_TRACE=${XOT:-1} "$@" RECOMP_APU_MUTE=1 RECOMP_WATCHDOG_SECS=${WD:-1500} \
    RECOMP_PB_REPORT_MS=5000 RECOMP_PAD_LIVE=$O/pad.txt RECOMP_FB_DUMP_FLIPS=$O/dumpflag \
    RECOMP_FB_DUMP=$O/fb ./halo2_recomp.exe > "$O/out.log" 2>&1 &
cat /proc/$!/winpid > "$O/winpid" 2>/dev/null
i=0
while [ $((i * 10)) -lt "$S" ]; do
  sleep 10; i=$((i + 1))
  bash /d/n64work/openhalo2/tools/shot.sh "$O" "w$i"
done
