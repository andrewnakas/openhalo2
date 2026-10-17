#!/bin/bash
# ol_wkeys.sh <dir> <label> key[:ms][@wait] ...: tools/ol_keys.sh for a browser
# tab that halo2-cleanroom/ports/wasm/cdp_pair.py drives from <dir>/cmd.txt
# (tab a). Pad names map to the page's keys (port/web/input.js): a Space,
# b Backspace, x KeyE, y Tab, start Enter, up/down/left/right arrows. Saves
# <label>_a.png after the last key.
O=${1:?dir}; L=${2:?label}; shift 2
declare -A K=([a]=Space [b]=Backspace [x]=KeyE [y]=Tab [start]=Enter [up]=ArrowUp [down]=ArrowDown [left]=ArrowLeft [right]=ArrowRight)
for k in "$@"; do
  w=3; case "$k" in *@*) w=${k#*@}; k=${k%@*};; esac
  ms=${HOLD:-400}; case "$k" in *:*) ms=${k#*:}; k=${k%:*};; esac
  [ "$k" = wait ] || echo "a key ${K[$k]:-$k} $ms" >> "$O/cmd.txt"
  sleep "$w"
done
echo "shot $L" >> "$O/cmd.txt"; sleep ${SHOTWAIT:-3}
echo "$L: $(grep -a 'frames a second' "$O/console_a.txt" | tail -1 | cut -c7-60)"
