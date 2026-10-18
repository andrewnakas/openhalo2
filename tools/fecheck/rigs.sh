#!/bin/bash
# rigs.sh <outroot> <scenario>... [-- extra env assignments]
# Runs the instrumented build (XRDIR, default build/xr_fe; EXE, default h2fe.exe)
# through headless play scenarios, one process each, with RECOMP_FE_OUT=<outroot>/<scenario>/fe.bin.
# Scenarios: title menus armory cairo ivory. Extra env (after --) goes to every run,
# e.g. RECOMP_FE_SNAP=<list> RECOMP_FE_SNAP_DIR=... (the snapshot dir gets /<scenario> appended
# when FE_SNAP_ROOT=<dir> is set instead). Only EXE is ever killed. One line per scenario.
OUTR=${1:?outroot}; shift
case "$OUTR" in ""|/|/d|/d/|/c|/c/) echo "refusing outroot '$OUTR'"; exit 1;; esac
SC=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do SC+=("$1"); shift; done; [ "$1" = "--" ] && shift
export XRDIR=${XRDIR:-/d/n64work/halo2/build/xr_fe} EXE=${EXE:-h2fe.exe}
T=/d/n64work/openhalo2/tools
kill_exe() { taskkill //F //IM "$EXE" > /dev/null 2>&1; sleep 3; }
alive() { tasklist 2>/dev/null | grep -q "$EXE"; }
pad() { echo "$1" >> "$O/pad.txt"; sleep ${2:-1}; }
for s in "${SC[@]}"; do
  O=$OUTR/$s; mkdir -p "$O"; rm -f "$O/fe.bin"
  E=(RECOMP_APU_MUTE=1 RECOMP_FB_DUMP_REPORT=0 RECOMP_FE_OUT="$(cygpath -m "$O")/fe.bin" "$@")
  [ -n "$FE_SNAP_ROOT" ] && E+=(RECOMP_FE_SNAP_DIR="$FE_SNAP_ROOT/$s")
  kill_exe; t0=$SECONDS
  case $s in
  title|menus)
    : > "$O/pad.txt"
    ( cd $XRDIR; env "${E[@]}" RECOMP_WATCHDOG_SECS=400 RECOMP_PB_REPORT_MS=5000 RECOMP_PAD_LIVE=$O/pad.txt \
        RECOMP_FB_DUMP_FLIPS=$O/dumpflag RECOMP_FB_DUMP=$O/fb ./$EXE > "$O/out.log" 2>&1 & )
    sleep 5
    until [ "$(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/')" -ge 1100 ] 2>/dev/null; do sleep 2; alive || break; done
    if [ $s = menus ]; then
      pad start:1500 8; pad a:300 8; pad a:300 8
      # main menu: walk every row in and out (Campaign, Xbox Live, Split Screen, System Link, Settings)
      for r in 1 2 3 4 5; do pad a:300 6; pad down:150 2; pad a:300 5; pad b:300 4; pad b:300 4; pad down:150 3; done
      pad b:300 4; pad b:300 4; pad b:300 4
    else
      sleep 60
    fi
    bash $T/shot.sh "$O" end > /dev/null ;;
  armory)
    STEP=7 bash $T/combat/ingame_x.sh "$O" 600 "${E[@]}" > "$O/rig.txt" 2>&1 || { echo "$s: rig failed $(tail -1 "$O/marks.txt")"; }
    for i in $(seq 8); do pad lup:1500 2; pad rright:300 1; pad rt:300 1; done
    pad a:300 2; pad x:300 2; sleep 20
    bash $T/shot.sh "$O" end > /dev/null ;;
  cairo)
    SHOT_N=${CINE_SHOTS:-22} SHOT_EVERY=8 WD=900 bash $T/combat/cairo.sh "$O" "${E[@]}" > "$O/rig.txt" 2>&1 || echo "$s: rig failed $(tail -1 "$O/rig.txt")"
    for i in $(seq 12); do pad lup:1500 2; pad rright:350 1; pad rt:400 1.5; done
    pad lt:300 3; for i in $(seq 6); do pad lup:1200 1.5; pad rleft:300 1; done
    sleep 15; bash $T/shot.sh "$O" end > /dev/null ;;
  ivory)
    MPLOAD=45 bash $T/combat/mpgame_x.sh "$O" 600 "${E[@]}" > "$O/rig.txt" 2>&1 || echo "$s: rig failed $(tail -2 "$O/rig.txt" | head -1)"
    sleep 6
    for i in $(seq 10); do pad lup:1200 1.5; pad rt:1500 2; pad rright:300 1; done
    pad lt:300 3; pad lt:300 3; pad y:300 2; pad rt:3000 4; pad x:300 2
    for i in $(seq 6); do pad ldown:1000 1.2; pad rleft:400 1; pad rt:800 1; done
    sleep 10; bash $T/shot.sh "$O" end > /dev/null ;;
  *) echo "unknown scenario $s"; continue ;;
  esac
  sleep 16   # one more fe.bin rewrite (every 15 s)
  kill_exe
  echo "$s: $((SECONDS - t0)) s, $(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*\] //'), fe.bin $(stat -c %s "$O/fe.bin" 2>/dev/null || echo missing), snaps $(grep -a -c '\[FE\] snap ' "$O/out.log")"
done
