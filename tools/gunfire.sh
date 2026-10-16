#!/bin/bash
# gunfire.sh <outdir> [xr|xr_clean]: Cairo Station (Select Level: down from the
# Armory), then 12 trigger pulls with the APU mix recorded to <outdir>/mix.wav.
# fire.txt holds the seconds into the WAV where the shots start.
O=${1:?outdir}; B=${2:-xr}
mkdir -p "$O"
XRDIR=/d/n64work/halo2/build/$B LEVEL_DIR=${DIR:-up} LEVEL_PICK=${PICK:-92} LOADWAIT=${LOADWAIT:-90} \
  bash /d/n64work/openhalo2/tools/ingame.sh "$O" 600 RECOMP_AC97_READY=1 \
  RECOMP_APU_WAV="$(cygpath -m "$O")/mix.wav,560" RECOMP_APU_WAV_FROM_START=1 RECOMP_APU_BINS=1 > "$O/ingame.txt" 2>&1
T0=$(stat -c %Y "$O/out.log")
sleep ${WAITC:-20}                      # cutscene/opening
for i in 1 2 3 4 5 6; do echo "a:300" >> "$O/pad.txt"; sleep 3; done   # skip any cinematic
sleep 10
bash /d/n64work/openhalo2/tools/shot.sh "$O" before > /dev/null
python -c "import wave,sys;w=wave.open(sys.argv[1]);print(w.getnframes()/48000)" "$O/mix.wav" > "$O/fire.txt" 2>/dev/null
for i in $(seq 12); do echo "rt:250" >> "$O/pad.txt"; sleep 1; done
bash /d/n64work/openhalo2/tools/shot.sh "$O" after > /dev/null
sleep 8
python -c "import wave,sys;w=wave.open(sys.argv[1]);print(w.getnframes()/48000)" "$O/mix.wav" >> "$O/fire.txt" 2>/dev/null
taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1
cat "$O/fire.txt"
