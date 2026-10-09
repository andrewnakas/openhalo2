#!/bin/bash
# bootloop.sh <outdir> <count> [extra env assignments...]
# Boots <count> times headless, presses start during the logo videos, and
# classifies each boot: ok (press consumed), dead (pad never read the press),
# crash ([CRASH] in the log), died (process gone with no report).
# One line per boot in <outdir>/summary.txt, totals at the end. Logs of the
# boots that were not ok are kept as boot<N>.log; ok logs are removed unless
# KEEP=1. WAIT (default 22) is the seconds before the press.
OUT=$1; N=$2; shift 2
mkdir -p "$OUT"
cd /d/n64work/halo2/build/xr
ok=0; dead=0; crash=0; died=0
for i in $(seq 1 $N); do
  : > "$OUT/pad.txt"
  L="$OUT/boot$i.log"
  env "$@" RECOMP_WATCHDOG_SECS=60 RECOMP_PB_REPORT_MS=5000 \
      RECOMP_PAD_LIVE=$OUT/pad.txt ./halo2_recomp.exe > "$L" 2>&1 &
  sleep ${WAIT:-22}
  echo "start:300" >> "$OUT/pad.txt"
  sleep 4
  alive=0; tasklist 2>/dev/null | grep -q halo2_recomp && alive=1
  c=$(grep -a -c 'PAD: live "' "$L")
  s=$(grep -a -c 'PAD: t=' "$L")
  if grep -a -q '\[CRASH\]' "$L"; then r=crash; crash=$((crash+1))
  elif [ $alive = 0 ]; then r=died; died=$((died+1))
  elif [ "$c" -ge 1 ] && [ "$s" -ge 1 ]; then r=ok; ok=$((ok+1))
  else r=dead; dead=$((dead+1)); fi
  echo "boot $i: $r  consumed=$c applied=$s $(grep -a 'operational after' "$L" | sed 's/.*operational/operational/;s/;.*//') fps:$(grep -a 'fps (' "$L" | tail -1 | sed 's/.*\] //;s/ fps.*//')" | tee -a "$OUT/summary.txt"
  taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1
  sleep 2
  [ $r = ok ] && [ -z "$KEEP" ] && rm -f "$L"
done
echo "TOTAL $N: ok=$ok dead=$dead crash=$crash died=$died" | tee -a "$OUT/summary.txt"
