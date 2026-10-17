#!/bin/bash
# ol_pair.sh <outdir> [quick]: two Open Live copies on this machine, A from
# build/xr (gamertag Andrew), B from build/xr_b (gamertag Bravo), each on
# the Xbox Live menu (their profiles must already be bound to those
# accounts: sign in once). With "quick": A starts Quickmatch and waits as a
# host, then B starts Quickmatch and should find and join A.
# Shots: <outdir>/a, <outdir>/b; side by side with
#   bash tools/sl_pair.sh --sheet <outdir> <label>
# Stop with: bash tools/ol_kill.sh <outdir>/a <outdir>/b
O=${1:?outdir}
T=/d/n64work/openhalo2/tools
case "$O" in ""|/|/d|/d/) echo "refusing outdir '$O'"; exit 1;; esac
mkdir -p "$O"
(bash $T/ol_boot.sh "$O/a" RECOMP_OL_GAMERTAG=Andrew > "$O/a.boot.txt" 2>&1 &)
sleep 25
XRDIR=/d/n64work/halo2/build/xr_b bash $T/ol_boot.sh "$O/b" RECOMP_OL_GAMERTAG=Bravo > "$O/b.boot.txt" 2>&1
sleep 3
bash $T/ol_tolive.sh "$O/a"
bash $T/ol_tolive.sh "$O/b"
if [ "$2" = quick ]; then
  bash $T/ol_keys.sh "$O/a" qm_a a:300@14
  bash $T/ol_keys.sh "$O/b" qm_b a:300@14
  bash $T/sl_pair.sh --sheet "$O" join1 > /dev/null
  echo "A: $(grep -a '\[XO\] session .* created\|mesh:' "$O/a/out.log" | tail -2 | tr '\n' ' ')"
  echo "B: $(grep -a 'search result\|mesh:\|registered' "$O/b/out.log" | tail -3 | tr '\n' ' ')"
fi
