#!/bin/sh
# run_mesh.sh <stations> [frames] [exe]: N native stations in a fresh private room.
# Prints each station's summary (logs kept in $LOGDIR).
N=${1:-2}; FRAMES=${2:-1000}
EXE=${3:-/d/n64work/halo2/build/net/h2net_test.exe}
ROOM=h2t-$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')
LOGDIR=${LOGDIR:-/d/n64work/halo2/build/net/logs}
mkdir -p "$LOGDIR"
echo "room=$ROOM stations=$N frames=$FRAMES"
pids=""
for i in $(seq 1 "$N"); do
  "$EXE" "$ROOM" "$i" "$N" "$FRAMES" > "$LOGDIR/st$i.log" 2>&1 &
  pids="$pids $!"
  sleep 0.3          # join order = station order (not required, keeps ids readable)
done
fail=0
for p in $pids; do wait "$p" || fail=1; done
for i in $(seq 1 "$N"); do grep -v '^\[h2net' "$LOGDIR/st$i.log"; done
[ $fail = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
