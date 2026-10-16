#!/bin/bash
# apu2/armory.sh <outdir> [ENV=val...]: retail 01a_tutorial (the Armory) with sound
# on (no device): boots via ingame.sh, then stands 20 s, turns right 3 s, stands
# 8 s, turns left 3 s, walks forward 3 s, stands 8 s. mix.wav from start;
# APU trace unless APU_NOTRACE; marks.txt / wavmarks.txt give the WAV second of
# each step. Ends by killing the game.
O=${1:?outdir}; shift
case "$O" in ""|/|/d|/d/) echo "refusing outdir '$O'"; exit 1;; esac
mkdir -p "$O"; rm -f "$O/mix.wav" "$O/trace.bin" "$O/wavmarks.txt"
W=$(cygpath -m "$O")
TR=""; [ -z "$APU_NOTRACE" ] && TR="RECOMP_APU_TRACE=$W/trace.bin,${TRACE_MB:-1500}"
bash /d/n64work/openhalo2/tools/ingame.sh "$O" 900 RECOMP_SOUND=1 RECOMP_APU_MUTE=1 \
    RECOMP_APU_WAV="$W/mix.wav,600" RECOMP_APU_WAV_FROM_START=1 RECOMP_APU_BINS=1 RECOMP_APU_VOICES=${VOICES:-0} \
    $TR "$@" > "$O/ingame.txt" 2>&1 || { echo "ingame failed: $(tail -2 "$O/ingame.txt")"; taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1; exit 1; }
wsec() { python -c "import os,sys;print('%.2f'%((os.path.getsize(sys.argv[1])-44)/192000))" "$O/mix.wav" 2>/dev/null; }
mark() { echo "$1 $(wsec)" >> "$O/wavmarks.txt"; }
mark ingame; sleep 20
mark turn_right; echo "rright:3000" >> "$O/pad.txt"; sleep 3.5
mark stand1; sleep 8
mark turn_left; echo "rleft:3000" >> "$O/pad.txt"; sleep 3.5
mark walk; echo "lup:3000" >> "$O/pad.txt"; sleep 3.5
mark stand2; sleep 8
mark end
bash /d/n64work/openhalo2/tools/shot.sh "$O" end > /dev/null
taskkill //F //IM halo2_recomp.exe > /dev/null 2>&1; sleep 1
cat "$O/wavmarks.txt" | tr '\n' ' '; echo; grep -a "fps (" "$O/out.log" | tail -1
