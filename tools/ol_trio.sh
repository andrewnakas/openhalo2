#!/bin/bash
# ol_trio.sh <outdir> [copies]: Open Live copies on this machine, each on the
# Xbox Live menu: a = build/xr (Andrew), b = build/xr_b (Bravo), c =
# build/xr_c (Charlie); their profiles must already be bound to those
# accounts (sign in once). copies defaults to "a b c". Sheet:
#   bash tools/ol_sheet.sh <outdir> <label>
# Stop with: bash tools/ol_kill.sh <outdir>/a <outdir>/b <outdir>/c
O=${1:?outdir}; L=${2:-a b c}
T=/d/n64work/openhalo2/tools
case "$O" in ""|/|/d|/d/) echo "refusing outdir '$O'"; exit 1;; esac
mkdir -p "$O"
declare -A DIR=([a]=xr [b]=xr_b [c]=xr_c [d]=xr_d)
declare -A TAG=([a]=Andrew [b]=Bravo [c]=Charlie [d]=Delta)
last=
for k in $L; do last=$k; done
for k in $L; do
  if [ "$k" = "$last" ]; then
    XRDIR=/d/n64work/halo2/build/${DIR[$k]} bash $T/ol_boot.sh "$O/$k" RECOMP_OL_GAMERTAG=${TAG[$k]} > "$O/$k.boot.txt" 2>&1
  else
    (XRDIR=/d/n64work/halo2/build/${DIR[$k]} bash $T/ol_boot.sh "$O/$k" RECOMP_OL_GAMERTAG=${TAG[$k]} > "$O/$k.boot.txt" 2>&1 &)
    sleep 20
  fi
done
sleep 5
python $T/ol_drive.py tolive "$O" $L
bash $T/ol_sheet.sh "$O" live $L
