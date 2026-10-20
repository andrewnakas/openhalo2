#!/bin/bash
# rigs_link.sh <outroot> <scenario>... [-- extra env assignments]
# Two-copy SYSTEM LINK rig: copy A (DIRA, default /e/h2link1) hosts, copy B (DIRB, default
# /e/h2link2) joins over the local nvnet hub (UDP 127.0.0.1:34900-34907), both play, both record.
# Each copy folder holds its own h2fe.exe (EXE), a real game\ copy and a real save\ copy, so the
# owner's profiles (E:\halo2\dirty\game\UDATA) and save are never written; A signs in as
# Halo0001, B as Halo0002. RESTORE=1 puts each copy's save images/caches and UDATA back from the
# pristine sources (PRISTINE_SAVE, PRISTINE_GAME; only read) before every scenario (slow: it
# compares ~5 GB per copy); the menus are driven from screenshots, so it is not needed.
# Reports: <outroot>/<scenario>/{A,B}/ (step screens, out.log, pad.txt, fe.bin).
# Snapshots: FE_SNAP_ROOT=<dir> -> RECOMP_FE_SNAP_DIR=<dir>/{A,B}/<scenario> (own pages.dat each).
# Only the PIDs this rig started are killed (never taskkill /IM): other h2fe copies are safe.
# Scenarios: link<map>_<type>: map = Select Map row (0 Lockout 1 Ascension 2 Midship 3 Ivory Tower
# 4 Beaver Creek 5 Burial Mounds 6 Colossus 7 Zanzibar 8 Coagulation 9 Headlong 10 Waterworks
# 11 Foundation); type = slayer koth oddball juggernaut ctf assault territories (first built-in
# variant of that game type). FIGHT_MIN (default 6) minutes of play on both copies.
# The machine is often loaded (E: busy, other jobs): the UI then runs seconds behind the pad and
# the lobby and game lists put their focus back on the first item every few seconds, so every
# step is checked on a screenshot (scr.py + link_refs/) and moves are made with long presses.
# Example:
#   FE_SNAP_ROOT=E:/halo2/snaps/link1 bash rigs_link.sh /e/halo2/reports/link1 link7_ctf \
#     -- RECOMP_FE_SNAP=E:/halo2/build/fecheck/queue/rig_snap_list2.txt RECOMP_FE_SNAP_K=4 RECOMP_FE_SNAP_GAP_MS=15000
# One summary line per scenario. RIG_LIB=1: only define the functions (source it to probe).
OUTR=${1:?outroot}; shift
case "$OUTR" in ""|/|/d*|/c|/c/|/e|/e/) echo "refusing outroot '$OUTR'"; exit 1;; esac
SC=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do SC+=("$1"); shift; done; [ "$1" = "--" ] && shift
XENV=("$@")
DIRA=${DIRA:-/e/h2link1} DIRB=${DIRB:-/e/h2link2} EXE=${EXE:-h2fe.exe}
PRISTINE_SAVE=${PRISTINE_SAVE:-/e/halo2/build/xr_fe/save}
PRISTINE_GAME=${PRISTINE_GAME:-/e/halo2/dirty/game}
MACA=${MACA:-02:48:32:00:00:0a} MACB=${MACB:-02:48:32:00:00:0b}
PRIO=${PRIO:-BelowNormal}
T=/e/openhalo2/tools
declare -A PID OD
# setsc <scenario>: report dirs <outroot>/<scenario>/{A,B}; PIDs survive in their pid files
setsc() { S=$1; local c; for c in A B; do OD[$c]=$OUTR/$S/$c; mkdir -p "${OD[$c]}"
  PID[$c]=$(cat "${OD[$c]}/pid" 2>/dev/null); done; }
# use A|B: the following pad/shot/scr calls go to that copy
use() { C=$1; O=${OD[$1]}; }
pad() { echo "$1" >> "$O/pad.txt"; sleep ${2:-1}; }
# shot <label>: <label>.png from a fresh frame (a slow frame dump can miss the 0.6 s window: retry)
shot() { local i; rm -f "$O/$1.png"; for i in 1 2 3 4 5 6; do bash $T/shot.sh "$O" "$1" > /dev/null 2>&1
  [ -s "$O/$1.png" ] && return 0; sleep 1; done; return 1; }
alive() { [ -n "${PID[$C]}" ] && tasklist //FI "PID eq ${PID[$C]}" 2>/dev/null | grep -q "$EXE"; }
kill_mine() { local c; for c in A B; do [ -n "${PID[$c]}" ] && taskkill //F //PID ${PID[$c]} > /dev/null 2>&1; PID[$c]=; rm -f "${OD[$c]}/pid"; done; sleep 3; }
flips() { grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/'; }
# restore <dir>: the copy's save images/caches and UDATA back to the pristine sources (only the
# 1 MB blocks that differ are written)
SAVEF="Partition0.img Partition1.img Partition2.img Partition3.img Partition4.img Partition5.img Cache/preferences.dat CacheY/savegame0.bin"
restore() {
  local d=$1 f; case "$d" in /e/h2link?) ;; *) echo "refusing restore '$d'"; return 1;; esac
  for f in $SAVEF; do [ -s "$PRISTINE_SAVE/$f" ] && python -c "
import sys,time
a,b=sys.argv[1],sys.argv[2]
for t in range(30):
    try: fb=open(b,'r+b'); break
    except PermissionError: time.sleep(2)
with open(a,'rb') as fa, fb:
    o=0
    while True:
        x=fa.read(1<<20)
        if not x: break
        fb.seek(o); y=fb.read(len(x))
        if x!=y: fb.seek(o); fb.write(x)
        o+=len(x)
    fb.truncate(o)
" "$PRISTINE_SAVE/$f" "$d/save/$f"; done
  [ -d "$PRISTINE_GAME/UDATA" ] && rm -rf "$d/game/UDATA" && cp -r "$PRISTINE_GAME/UDATA" "$d/game/UDATA"
}
# lit <png> <x0> <x1> <y>...: index of the brightest row band; -1 unless 1.1 times the next
lit() { python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');x0,x1=int(sys.argv[2]),int(sys.argv[3]);v=[sum(im.crop((x0,y-4,x1,y+4)).tobytes()) for y in map(int,sys.argv[4:])];w=sorted(v);print(v.index(max(v)) if w[-1]>=1.1*w[-2] else -1)" "$@" 2>/dev/null; }
# party2: 1 when the lobby's player list shows a second player (a coloured bar at y 96)
party2() { python -c "import sys;from PIL import Image;v=list(Image.open(sys.argv[1]).convert('RGB').crop((420,92,600,100)).getdata());print(1 if sum(max(p)-min(p) for p in v)/len(v)>36 else 0)" "$1" 2>/dev/null; }
# joinedq: 1 when a lobby is a joined one ("<host> is the party leader" where the host's Start
# Game / Game Setup buttons are): its top band is nearer link_refs/lobby2.png than lobby.png
joinedq() { python -c "import sys;from PIL import Image
c=lambda p:Image.open(p).convert('L').crop((30,60,300,100)).tobytes()
a=c(sys.argv[1]);d=lambda r:sum(abs(x-y) for x,y in zip(a,c(r)))
print(1 if d(sys.argv[3])<d(sys.argv[2]) else 0)" "$1" $T/fecheck/link_refs/lobby.png $T/fecheck/link_refs/lobby2.png 2>/dev/null; }
# setuplit: 1 when the lobby's Game Setup button is lit (brighter than Start Game)
setuplit() { python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');a=sum(im.crop((45,68,200,86)).tobytes());b=sum(im.crop((225,68,380,86)).tobytes());print(1 if b>1.15*a else 0)" "$1" 2>/dev/null; }
# start <A|B> <mac> <watchdog>: start that copy headless (cwd = its folder), remember its PID
start() {
  use $1; local d; [ $1 = A ] && d=$DIRA || d=$DIRB
  : > "$O/pad.txt"; rm -f "$O/fe.bin" "$O"/fb*.bmp "$O/dumpflag"
  local E=(RECOMP_APU_MUTE=1 RECOMP_FB_DUMP_REPORT=0 RECOMP_FE_OUT="$(cygpath -m "$O")/fe.bin" RECOMP_MAC=$2 RECOMP_NIC_FRAMES=${NIC_FRAMES:-0} "${XENV[@]}")
  # (the runtime creates only the last directory level)
  [ -n "$FE_SNAP_ROOT" ] && { mkdir -p "$(cygpath -u "$FE_SNAP_ROOT")/$1/$S"; E+=(RECOMP_FE_SNAP_DIR="$FE_SNAP_ROOT/$1/$S"); }
  ( cd $d && exec env "${E[@]}" RECOMP_WATCHDOG_SECS=$3 RECOMP_PB_REPORT_MS=5000 RECOMP_PAD_LIVE="$(cygpath -m "$O")/pad.txt" \
      RECOMP_FB_DUMP_FLIPS="$(cygpath -m "$O")/dumpflag" RECOMP_FB_DUMP="$(cygpath -m "$O")/fb" ./$EXE > "$O/out.log" 2>&1 ) &
  local p= i; for i in $(seq 20); do sleep 1
    p=$(powershell -NoProfile -c "Get-Process h2fe -ea 0 | ?{\$_.Path -like '$(cygpath -w $d)\\*'} | %{ \$_.PriorityClass='$PRIO'; \$_.Id }" | tr -d '\r' | head -1)
    [ -n "$p" ] && break; done
  PID[$1]=$p; echo "$p" > "$O/pid"; [ -n "$p" ]
}
# waittitle: until the copy has presented 1100 frames (the title)
# (gives up after TITLE_MAX seconds, default 900: once a copy sat at 2 flips for 50 minutes)
waittitle() { local d=0 t=$SECONDS; until [ "$(flips)" -ge 1100 ] 2>/dev/null; do sleep 3; if alive; then d=0; else d=$((d + 1)); fi
  [ $d -ge 3 ] && return 1; [ $((SECONDS - t)) -ge ${TITLE_MAX:-900} ] && return 1; done; sleep 4; }
# boot <A|B>: start the copy and wait for its title; a copy that hangs on the way is killed and
# started once more
boot() { local c=$1 mac; [ $c = A ] && mac=$MACA || mac=$MACB
  for k in 1 2; do start $c $mac ${WD:-5400} && waittitle && return 0
    [ -n "${PID[$c]}" ] && taskkill //F //PID ${PID[$c]} > /dev/null 2>&1; PID[$c]=; sleep 5; done; return 1; }
# userbusy: the owner is playing (a ReSkate / Skate.exe process): the rig waits between scenarios
userbusy() {
  tasklist 2>/dev/null | grep -qi "^skate.exe" && return 0
  # any h2fe.exe the rig did not start (the owner's own copy); with none running, tasklist prints
  # an INFO line whose words are not PIDs (they made the rig wait forever)
  local p; for p in $(tasklist //FI "IMAGENAME eq $EXE" //FO CSV //NH 2>/dev/null | cut -d, -f2 | tr -d '"' | grep -E '^[0-9]+$'); do
    case " ${PID[*]} " in *" $p "*) ;; *) return 0 ;; esac
  done
  return 1
}
# scr: classify the current frame against link_refs (title profiles signin mainmenu players
# available lobby setup gametype variant selmap gamemenu carnage ingame exitdlg black); prints
# "<name> <distance>" (a distance over 30 is no known screen)
scr() { shot cl || { echo "none 999"; return; }; [ "$(hud "$O/cl.png")" = 1 ] && { echo "ingame 0"; return; }
  python $T/fecheck/scr.py "$O/cl.png" 2>/dev/null || echo "none 999"; }
# hud: 1 when the multiplayer HUD's motion tracker (bright blue, x 50-125 y 365-435) is up
hud() { python -W ignore -c "import sys;from PIL import Image;v=Image.open(sys.argv[1]).convert('RGB').crop((50,365,125,435)).tobytes();print(1 if sum(1 for i in range(len(v)//3) if v[3*i+2]>200 and v[3*i+2]>v[3*i]+60)>60 else 0)" "$1" 2>/dev/null; }
# rowto <target> <x0> <x1> <y>...: up/down until row <target> is lit; after each press wait (up to
# 12 s) for the lit row to change
rowto() { local tg=$1 r r2 i k h=${ROWHOLD:-300}; shift
  for i in $(seq 16); do shot rw; r=$(lit "$O/rw.png" "$@"); [ "$r" = $tg ] && return 0
    if [ "${r:--1}" = -1 ]; then [ $((i % 4)) = 3 ] && pad down:$h 1; sleep 2; continue; fi
    if [ "$r" -gt $tg ]; then pad up:$h 1; else pad down:$h 1; fi
    for k in 1 2 3 4 5 6; do sleep 2; shot rw; r2=$(lit "$O/rw.png" "$@"); [ "$r2" != "$r" ] && break; done
    # the hold adapts: a press that moved two rows (auto-repeat) is made shorter, one that moved
    # nothing (a slow frame swallowed it) longer
    if [ "$r2" = "$r" ]; then h=$((h + 150 > 700 ? 700 : h + 150))
    elif [ "${r2:--1}" != -1 ] && [ $(( (r2 - r) * (r2 - r) )) -gt 1 ]; then h=$((h / 2 < 100 ? 100 : h / 2)); fi
  done; return 1; }
# press <button> <screen>...: press, then wait (up to 40 s) for one of the screens; press again
# only while the screen is still the one the first press was made on (up to 3 presses)
press() { local b=$1 i k n d n0 d0; shift; read n0 d0 <<< "$(scr)"
  for i in 1 2 3; do pad $b:500 1
    for k in $(seq 20); do sleep 2; read n d <<< "$(scr)"; [ $d -le 30 ] && [[ " $* " == *" $n "* ]] && return 0; done
    [ "$n" = "$n0" ] || return 1
  done; return 1; }
# toavail <profile>: from wherever the copy is in the front end, reach System Link's Available
# Games signed in with profile row n (0 = Halo0001); every press is chosen from a screenshot
toavail() { local i n d r
  for i in $(seq ${NAVMAX:-40}); do read n d <<< "$(scr)"; [ $d -gt 30 ] && n=other
    case $n in
    available) cp "$O/cl.png" "$O/s_available.png"; return 0 ;;
    title) pad start:1500 6 ;;
    profiles) r=$(lit "$O/cl.png" 220 450 242 263 284 305)
      if [ "$r" = $((1 + $1)) ]; then cp "$O/cl.png" "$O/s_profile.png"; pad a:500 6
      elif [ "${r:--1}" -gt $((1 + $1)) ]; then pad up:450 3; else pad down:450 3; fi ;;
    signin|players) pad a:500 6 ;;
    exitdlg|lobby|settings|carnage|setup|gametype|variant|selmap) pad b:500 5 ;;
    mainmenu) r=$(lit "$O/cl.png" 405 435 287 315 343 371 399)
      if [ "$r" = 3 ]; then pad a:500 6; elif [ "${r:--1}" -gt 3 ]; then pad up:450 3; else pad down:450 3; fi ;;
    *) sleep 4 ;;
    esac; done; return 1; }
# countdown: 1 when the lobby shows the start countdown ("Press X to cancel" line)
countdown() { python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');print(1 if sum(1 for x in im.crop((60,128,300,142)).tobytes() if x>150)>20 else 0)" "$1" 2>/dev/null; }
# tosetup: from the host lobby into Custom Game Setup. Right moves to Game Setup, but the lobby
# puts its focus back on Start Game every few seconds and a slow frame can swallow the press, so
# A is pressed only right after a screenshot shows Game Setup lit; a countdown it started anyway
# is cancelled with X
tosetup() { local i n d
  for i in $(seq 10); do read n d <<< "$(scr)"
    case $n in
    setup) return 0 ;;
    lobby) if [ "$(countdown "$O/cl.png")" = 1 ]; then pad x:500 3; continue; fi
      if [ "$(setuplit "$O/cl.png")" != 1 ]; then pad right:700 0.2; shot sl; fi
      if [ "$(setuplit "$O/sl.png")" = 1 ] || [ "$(setuplit "$O/cl.png")" = 1 ]; then pad a:500 4; fi ;;
    gametype|variant|selmap) pad b:500 5 ;;
    ingame) return 1 ;;
    *) sleep 4 ;;
    esac; done; return 1; }
# tolobby: back from setup screens to the lobby
tolobby() { local i n d; for i in $(seq 6); do read n d <<< "$(scr)"; [ $n = lobby ] && return 0
  case $n in setup|gametype|variant|selmap) pad b:500 5 ;; *) sleep 4 ;; esac; done; return 1; }
TYPES=(slayer koth oddball juggernaut ctf assault territories)
# host <map row> <type row>: A creates a game, sets game type (first variant) and map
host() {
  press a lobby || return 1; shot s_lobby0
  tosetup || return 2; rowto 1 90 300 132 154 176 198 || return 3; press a gametype || return 4
  rowto $2 110 320 153 175 197 219 241 263 285 || return 5; shot s_gametype; press a variant || return 6
  shot s_variant; press a lobby setup || return 7
  tosetup || return 8; rowto 0 90 300 132 154 176 198 || return 9; press a selmap || return 10
  rowto $1 95 300 110 132 154 176 198 220 242 264 286 308 330 352 || return 11; shot s_selmap
  press a lobby setup || return 12; tolobby || return 13; shot s_hosted
}
# join: B, on Available Games, picks the first listed game (down then A at once: the list puts its
# focus back on Create New Game every few seconds); a game it created by mistake is left again
join() { local i n d
  for i in $(seq 8); do read n d <<< "$(scr)"
    case $n in
    available) pad down:600 0.7; pad a:500 10 ;;
    ingame) cp "$O/cl.png" "$O/s_joined.png"; return 0 ;;   # joined a game in progress
    lobby) if [ "$(joinedq "$O/cl.png")" = 1 ]; then cp "$O/cl.png" "$O/s_joined.png"; return 0; fi
      cp "$O/cl.png" "$O/s_ownlobby$i.png"; pad b:500 6 ;;
    *) sleep 4 ;;
    esac; done; return 1; }
# teamswap: B moves to the other team (X opens its team picker; right, A)
teamswap() { pad x:500 4; pad right:500 2; pad a:500 4; shot s_team; }
# mpgo: A presses Start Game (left first: Start Game is the left button) until a map loads
mpgo() { local i n d
  for i in $(seq 6); do read n d <<< "$(scr)"
    case $n in
    lobby) [ "$(setuplit "$O/cl.png")" = 1 ] && pad left:500 0.6; pad a:500 15 ;;
    ingame) return 0 ;;
    setup|gametype|variant|selmap) pad b:500 5 ;;
    *) sleep 8 ;;
    esac; done; return 1; }
# ingame_wait: up to 5 minutes for the copy's map (in-game screen)
ingame_wait() { local i n d; for i in $(seq 40); do alive || return 1; read n d <<< "$(scr)"; [ $n = ingame ] && return 0; sleep 6; done; return 1; }
# fight <minutes>: wander and fight (rigs5's loop); stops early when the copy is back in a menu
fight() { local end=$((SECONDS + $1 * 60)) n d; while [ $SECONDS -lt $end ]; do alive || return 1
    pad lup:1800 2; pad rright:350 0.8; pad rt:1200 1.5; pad lup:1500 1.8; pad x:600 1; pad lt:300 1.2
    pad rleft:500 0.8; pad rt:800 1; pad b:300 1; pad y:300 1.2; pad lup:1200 1.5; pad a:300 1
    pad lthumb:300 0.8; pad rthumb:300 0.8; pad ldown:800 1; pad rup:300 0.6; pad rt:600 1; pad x:300 1
    pad white:300 0.8; pad lleft:900 1; pad black:300 0.8; pad lright:900 1
    read n d <<< "$(scr)"; case $n in lobby|carnage|available|setup|mainmenu) [ $d -le 15 ] && return 2 ;; esac
  done; }
# endgame: A ends the match from its game menu (End Game is the last row: up wraps to it; the
# confirmation's default is Cancel: up to End Game), then both copies page through the carnage report
endgame() { local i n d c k
  use A; for i in 1 2 3; do read n d <<< "$(scr)"
    case $n in
    ingame|none|black) pad start:500 4 ;;
    gamemenu) # End Game is the last row (4, or 5 with Change Teams): step down; a wrap back to
      # row 0 means the row before was the last (step up again); two presses that leave the row
      # where it is mean the end of a list that does not wrap. Never A on Leave Game (row 0)
      local r r2 st=0 Y="140 380 201 223 245 267 289 311"; shot s_gamemenu; r=$(lit "$O/s_gamemenu.png" $Y)
      for k in $(seq 14); do pad down:350 5; shot s_gamemenu; r2=$(lit "$O/s_gamemenu.png" $Y)
        if [ "${r2:--1}" = -1 ] || [ "$r2" = "$r" ]; then st=$((st + 1)); [ $st -ge 2 ] && [ "${r:--1}" -ge 4 ] && break; continue; fi
        st=0; if [ "$r2" -lt "$r" ] && [ "$r" -ge 4 ]; then
          for k in 1 2 3; do pad up:350 5; shot s_gamemenu; r2=$(lit "$O/s_gamemenu.png" $Y); [ "$r2" = "$r" ] && break; done; break; fi
        r=$r2; done
      [ "${r2:--1}" -ge 4 ] && [ "$r2" = "$r" ] || break
      pad a:500 4; shot s_endconfirm; pad up:400 3; pad a:500 10 ;;
    *) break ;;
    esac; done
  for c in A B; do use $c; for i in $(seq 10); do read n d <<< "$(scr)"; [ $n = carnage ] && break; sleep 4; done
    for i in 1 2 3 4; do shot s_carnage$i; pad right:500 3; done; done
}
if [ -n "$RIG_LIB" ]; then return 0 2>/dev/null || exit 0; fi

for s in "${SC[@]}"; do
  case $s in link[0-9]*_*) ;; *) echo "unknown scenario $s"; continue ;; esac
  m=${s#link}; ty=${m#*_}; m=${m%%_*}; tr=-1
  for i in "${!TYPES[@]}"; do [ "${TYPES[$i]}" = "$ty" ] && tr=$i; done
  [ $tr = -1 ] && { echo "$s: unknown game type $ty"; continue; }
  while userbusy; do sleep 60; done   # never run beside the owner's game
  setsc $s; kill_mine; t0=$SECONDS; st=""
  [ -n "$RESTORE" ] && { restore $DIRA; restore $DIRB; }
  # host first (two copies booting at once on a busy E: took 7 minutes to the title)
  if boot A; then use A; toavail 0 && host $m $tr; r=$?; [ $r = 0 ] && st+="hosted " || st+="host-fail($r) "; else st+="A-noboot "; fi
  if boot B; then use B; toavail 1 && join; r=$?; [ $r = 0 ] && st+="joined " || st+="join-fail($r) "; else st+="B-noboot "; fi
  use B; read n d <<< "$(scr)"; case $ty in slayer|oddball|juggernaut) ;; *) [ $n = lobby ] && teamswap ;; esac
  use A; shot s_prestart; mpgo && st+="started " || st+="start-fail "
  for c in A B; do use $c; ingame_wait && shot s_inmap || st+="$c-nomap "; done
  # both copies fight at once (each loop drives only its own pad file)
  # (wait only for the two fight loops: the games are children of this shell too)
  fp=(); for c in A B; do ( use $c; fight ${FIGHT_MIN:-6}; shot s_fightend ) & fp+=($!); done; wait "${fp[@]}"
  endgame; sleep 16   # one more fe.bin rewrite (every 15 s)
  kill_mine
  line="$s: $((SECONDS - t0)) s, $st"
  for c in A B; do O=${OD[$c]}; line+="| $c $(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*\] //'), fe.bin $(stat -c %s "$O/fe.bin" 2>/dev/null || echo missing), snaps $(grep -a -c '\[FE\] snap ' "$O/out.log") "; done
  echo "$line"
done
