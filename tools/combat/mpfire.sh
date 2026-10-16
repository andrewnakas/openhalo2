#!/bin/bash
# mpfire.sh <outdir> [extra env...]: Ivory Tower via mpgame.sh, then fire and throw grenades
# with the mix recorded; fire.txt = wav seconds at first shot and at end.
O=$1; shift; mkdir -p "$O"
bash /d/n64work/openhalo2/tools/combat/mpgame_x.sh "$O" 600 RECOMP_AC97_READY=1 \
  RECOMP_APU_WAV="$(cygpath -m "$O")/mix.wav,560" RECOMP_APU_WAV_FROM_START=1 RECOMP_APU_BINS=1 \
  RECOMP_FRAME_TRACE="$O/ftflag" "$@" > "$O/mp.txt" 2>&1 || { echo "mpgame failed"; tail -3 "$O/marks.txt"; exit 1; }
sleep 8
bash /d/n64work/openhalo2/tools/shot.sh "$O" before > /dev/null
python -c "import wave,sys;w=wave.open(sys.argv[1]);print(w.getnframes()/48000)" "$O/mix.wav" > "$O/fire.txt"
for i in $(seq 10); do echo "rt:400" >> "$O/pad.txt"; [ $i = 3 ] && touch "$O/ftflag"; sleep 0.25; [ $i = 3 ] && bash /d/n64work/openhalo2/tools/shot.sh "$O" fire3 > /dev/null; sleep 0.75; done
bash /d/n64work/openhalo2/tools/shot.sh "$O" after > /dev/null
echo "lt:400" >> "$O/pad.txt"; sleep 1.5; bash /d/n64work/openhalo2/tools/shot.sh "$O" nade1 > /dev/null; sleep 0.4; bash /d/n64work/openhalo2/tools/shot.sh "$O" nade2 > /dev/null
sleep 6
python -c "import wave,sys;w=wave.open(sys.argv[1]);print(w.getnframes()/48000)" "$O/mix.wav" >> "$O/fire.txt"
taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1
cat "$O/fire.txt"
