#!/bin/bash
# rigs.sh <outroot> <scenario>... [-- extra env assignments]
# Runs the instrumented build (XRDIR, default build/xr_fe; EXE, default h2fe.exe)
# through headless play scenarios, one process each, with RECOMP_FE_OUT=<outroot>/<scenario>/fe.bin.
# Scenarios: title menus armory cairo ivory heretic pause mplobby syslink. Extra env (after --) goes to every run,
# e.g. RECOMP_FE_SNAP=<list> RECOMP_FE_SNAP_DIR=... (the snapshot dir gets /<scenario> appended
# when FE_SNAP_ROOT=<dir> is set instead). Only EXE is ever killed. One line per scenario.
# pause/mplobby/syslink change remembered menu rows and settings: they back up the save
# (XRDIR/save partitions 0-2, preferences, savegame) to SAVEBAK and restore it afterwards.
# Each step's screenshot (<label>.png) shows whether the scenario reached its goal.
OUTR=${1:?outroot}; shift
case "$OUTR" in ""|/|/d|/d/|/c|/c/|/e|/e/) echo "refusing outroot '$OUTR'"; exit 1;; esac
SC=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do SC+=("$1"); shift; done; [ "$1" = "--" ] && shift
export XRDIR=${XRDIR:-/c/h2play} EXE=${EXE:-h2fe.exe}
T=/e/openhalo2/tools
kill_exe() { taskkill //F //IM "$EXE" > /dev/null 2>&1; sleep 3; }
alive() { tasklist 2>/dev/null | grep -q "$EXE"; }
pad() { echo "$1" >> "$O/pad.txt"; sleep ${2:-1}; }
shot() { bash $T/shot.sh "$O" "$1" > /dev/null; }
SAVEBAK=${SAVEBAK:-/e/halo2/build/xr_fe_savebak}
SAVEF="Partition0.img Partition1.img Partition2.img Cache/preferences.dat CacheY/savegame0.bin"
# save_bak: the first time, copy the save to SAVEBAK; later, put SAVEBAK back (unless this rig
# run just did), so every such scenario starts from the same remembered menu rows
save_bak() {
  if [ -s "$SAVEBAK/Partition1.img" ]; then [ -n "$CLEAN" ] || save_restore; return 0; fi
  mkdir -p "$SAVEBAK/Cache" "$SAVEBAK/CacheY"; for f in $SAVEF; do cp -p "$XRDIR/save/$f" "$SAVEBAK/$f" || { rm -f "$SAVEBAK/Partition1.img"; return 1; }; done; }
# (only the 1 MB blocks that differ are written back: whole copies of the 2.2 GB partition took
# ~10 minutes while other jobs loaded the disk)
save_restore() { for f in $SAVEF; do [ -s "$SAVEBAK/$f" ] && python -c "
import sys,os,time
a,b=sys.argv[1],sys.argv[2]; n=0
for t in range(30):
    try: fb=open(b,'r+b'); break
    except PermissionError: time.sleep(2)   # the killed game can hold the image for a while
with open(a,'rb') as fa, fb:
    o=0
    while True:
        x=fa.read(1<<20)
        if not x: break
        fb.seek(o); y=fb.read(len(x))
        if x!=y: fb.seek(o); fb.write(x); n+=1
        o+=len(x)
    fb.truncate(o)
" "$SAVEBAK/$f" "$XRDIR/save/$f"; done; }
# lit <png> <x0> <x1> <y>...: index of the brightest row band (the highlight bar); -1 when it is
# not LITM (default 1) times brighter than the next one
lit() { python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');x0,x1=int(sys.argv[2]),int(sys.argv[3]);v=[sum(im.crop((x0,y-4,x1,y+4)).get_flattened_data() if hasattr(im,'get_flattened_data') else im.crop((x0,y-4,x1,y+4)).getdata()) for y in map(int,sys.argv[4:])];m=float(__import__('os').environ.get('LITM','1'));w=sorted(v);print(v.index(max(v)) if w[-1]>=m*w[-2] else -1)" "$@" 2>/dev/null; }
# boot <watchdog>: start EXE headless with a live pad file and wait for the title
boot() {
  : > "$O/pad.txt"
  ( cd $XRDIR; env "${E[@]}" RECOMP_WATCHDOG_SECS=$1 RECOMP_PB_REPORT_MS=5000 RECOMP_PAD_LIVE=$O/pad.txt \
      RECOMP_FB_DUMP_FLIPS=$O/dumpflag RECOMP_FB_DUMP=$O/fb ./$EXE > "$O/out.log" 2>&1 & )
  sleep 5
  # (a busy machine can take minutes to the title; one failed tasklist is not a dead game)
  local d=0
  until [ "$(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*(\([0-9]*\) flips).*/\1/')" -ge 1100 ] 2>/dev/null; do
    sleep 3; if alive; then d=0; else d=$((d + 1)); fi; [ $d -ge 3 ] && return 1; done
  sleep 4
}
# mainrow <row>: Start, profile, don't sign in, then up until main-menu row <row> is lit
# (Campaign 0, Xbox Live 1, Split Screen 2, System Link 3, Settings 4; rows at y 287 + 28n)
logo() { python -c "import sys;from PIL import Image;print(sum(1 for x in Image.open(sys.argv[1]).convert('L').crop((180,195,460,240)).getdata() if x>200))" "$1" 2>/dev/null || echo 0; }
mainrow() {
  pad start:1500 8; pad a:300 8; pad a:300 8
  # wait for the main menu (the HALO 2 logo: >1000 bright pixels at 180-460 x 195-240); a
  # missed press leaves the sign-in dialog up: press A again
  for i in $(seq 12); do shot row
    [ "$(logo "$O/row.png")" -gt 1000 ] && break
    [ $i = 4 ] || [ $i = 8 ] && pad a:300 4; sleep 2
  done
  for i in 1 2 3 4 5 6; do shot row; [ "$(LITM=1.1 lit "$O/row.png" 405 435 287 315 343 371 399)" = $1 ] && break; pad up:150 3; done
}
# camp_level <n>: Campaign -> Campaign Options (Resume 0 / Select Level 1 / Cooperative 2 at
# y 192 213 235) -> Select Level row n (The Heretic 0, Armory 1, Cairo Station 2 at y 92 114 136;
# only these are unlocked on this save) -> difficulty; each move is checked on a screenshot
camp_level() {
  boot 900 || return 1; mainrow 0; pad a:300 8
  for i in 1 2 3; do shot k0; [ "$(logo "$O/k0.png")" -lt 1000 ] && break; pad a:300 6; done
  for i in 1 2 3 4; do shot k1; r=$(lit "$O/k1.png" 100 300 192 213 235); [ "$r" = 1 ] && break
    if [ "${r:-0}" -gt 1 ]; then pad up:150 3; else pad down:150 3; fi; done
  pad a:300 10; shot k2
  for i in 1 2 3 4 5; do shot k3; r=$(lit "$O/k3.png" 140 330 92 114 136); [ "$r" = $1 ] && break
    if [ "${r:-0}" -gt $1 ]; then pad up:150 3; else pad down:150 3; fi; done
  pad a:300 8; shot k4; pad a:300 8; shot k5
}
# lobby <main menu row>: Split Screen (2) or System Link (3) up to Custom Game Setup
# (the lobby's Game Setup button: its right arrow at x 382-392 is bright when lit)
lobby() {
  boot 900 || return 1; mainrow $1; pad a:500 8
  for i in 1 2 3; do shot l0; [ "$(logo "$O/l0.png")" -lt 1000 ] && break; pad a:500 6; done; pad a:500 8; shot l1
  [ "$(python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');print(1 if sum(im.crop((382,70,392,86)).getdata())>160*80 else 0)" "$O/l1.png" 2>/dev/null)" = 1 ] || pad right:300 3
  shot l2; pad a:500 6; shot l3
}
# hudwait: wait (up to 4 minutes) for the campaign HUD's shield bar (x 50-130, y 344-360: blue with
# some red) after a loading frame (no red there) was seen: menus pass the HUD test too, and a
# fixed wait was too short on a busy machine
hudwait() { local ld=0 v; for i in $(seq 48); do shot hud
    v=$(python -c "import sys;from PIL import Image;v=list(Image.open(sys.argv[1]).convert('RGB').crop((50,344,130,360)).getdata());r=sum(p[0] for p in v)/len(v);print('L' if r<5 else 'H' if r>20 and sum(p[2] for p in v)/len(v)>140 else '-')" "$O/hud.png" 2>/dev/null)
    [ "$v" = L ] && ld=1; [ "$v" = H ] && [ $ld = 1 ] && return 0; sleep 4; done; return 1; }
# tosetup: a game type or map choice returns to the lobby: if its Game Setup button is lit, enter it
tosetup() { shot ts; [ "$(python -c "import sys;from PIL import Image;im=Image.open(sys.argv[1]).convert('L');print(1 if sum(im.crop((382,70,392,86)).getdata())>160*80 else 0)" "$O/ts.png" 2>/dev/null)" = 1 ] && pad a:500 6; }
# setuprow <n>: on Custom Game Setup, up/down until row n is lit (Change Map 0, Change Rules 1, Quick Options 2)
setuprow() {
  for i in 1 2 3 4 5; do shot sr; r=$(lit "$O/sr.png" 90 300 132 154 176); [ "$r" = $1 ] && break
    if [ "${r:-0}" -gt $1 ]; then pad up:150 2; else pad down:150 2; fi; done
}
# mpstart: leave Custom Game Setup for the lobby, press Start Game, wait for a map (fps < 38)
mpstart() {
  pad b:500 4; shot lobby; pad left:300 2; pad a:500 10
  for i in 1 2 3 4 5 6 7 8; do
    f=$(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*\] \([0-9]*\)\..*/\1/'); [ "${f:-99}" -lt 38 ] && break
    pad a:500 12
  done; sleep 40; shot inmap
  for i in $(seq 12); do pad lup:1200 1.5; pad rt:1500 2; pad rright:300 1; pad lt:300 1.5; pad b:300 1; done
  pad y:300 2; pad x:300 2; pad back:2000 3; shot score; pad start:300 3; shot mppause; pad b:300 2
}

# camp_any <row>: like camp_level, but any of the 15 Select Level rows (y 92 + 22n; this save has
# them all unlocked)
camp_any() {
  boot 900 || return 1; mainrow 0; pad a:300 8
  for i in 1 2 3; do shot k0; [ "$(logo "$O/k0.png")" -lt 1000 ] && break; pad a:300 6; done
  for i in 1 2 3 4; do shot k1; r=$(lit "$O/k1.png" 100 300 192 213 235); [ "$r" = 1 ] && break
    if [ "${r:-0}" -gt 1 ]; then pad up:150 3; else pad down:150 3; fi; done
  pad a:300 10; shot k2
  ROWS=$(python -c "print(' '.join(str(92+22*i) for i in range(15)))")
  local ok=0
  for i in $(seq 20); do shot k3; r=$(lit "$O/k3.png" 140 330 $ROWS); [ "$r" = $1 ] && { ok=1; break; }
    if [ "${r:--1}" = -1 ]; then pad down:150 2; elif [ "$r" -gt $1 ]; then pad up:150 2; else pad down:150 2; fi; done
  [ $ok = 1 ] || return 1   # not on Select Level (the main menu row was misread): the caller retries
  pad a:300 8; shot k4; for i in 1 2 3; do pad up:150 2; done; pad a:300 8; shot k5
}
# hudlong: after a level is chosen, wait (up to 15 minutes) through loading and the opening
# cinematic until the shield bar shows
hudlong() { for i in $(seq 180); do alive || return 1; shot hud
    v=$(python -c "import sys;from PIL import Image;v=list(Image.open(sys.argv[1]).convert('RGB').crop((50,344,130,360)).getdata());r=sum(p[0] for p in v)/len(v);print('H' if r>20 and sum(p[2] for p in v)/len(v)>140 else '-')" "$O/hud.png" 2>/dev/null)
    [ "$v" = H ] && return 0; [ $((i % 6)) = 0 ] && pad a:300 1; sleep 5; done; return 1; }
# fight <minutes>: wander and fight: move and turn, fire, grenades, melee, reload/action, swap
# weapons, crouch, jump, zoom, flashlight
fight() { local end=$((SECONDS + $1 * 60)); while [ $SECONDS -lt $end ]; do alive || return 1
    pad lup:1800 2; pad rright:350 0.8; pad rt:1200 1.5; pad lup:1500 1.8; pad x:600 1; pad lt:300 1.2
    pad rleft:500 0.8; pad rt:800 1; pad b:300 1; pad y:300 1.2; pad lup:1200 1.5; pad a:300 1
    pad lthumb:300 0.8; pad rthumb:300 0.8; pad ldown:800 1; pad rup:300 0.6; pad rt:600 1; pad x:300 1
    pad white:300 0.8; pad lleft:900 1; pad black:300 0.8; pad lright:900 1; done; }
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
  heretic)
    # The Heretic from Select Level: the opening cinematic plays for minutes, then the Arbiter
    # in the gas mine
    save_bak || { echo "$s: save backup failed"; continue; }; RESTORE=1
    camp_level 0 || echo "$s: boot failed"
    for i in $(seq -w 1 16); do sleep 15; shot h$i; done
    for i in $(seq 10); do pad lup:1500 2; pad rright:300 1; pad rt:600 1; pad lt:300 1; pad b:300 1; done
    pad a:300 2; pad x:1000 2; pad y:300 2; pad lthumb:300 2; pad white:300 2
    sleep 10; shot end ;;
  pause)
    # Cairo: melee, jump, crouch, grenade, reload, zoom, flashlight; the pause menu rows and their
    # submenus; Save and Quit; then the main menu's Settings two levels deep
    save_bak || { echo "$s: save backup failed"; continue; }; RESTORE=1
    camp_level 2 || echo "$s: boot failed"; hudwait || echo "$s: no HUD"; sleep 5; shot c0
    for i in $(seq 8); do pad lup:1500 2; pad b:300 1; pad a:300 1; pad lthumb:300 1; pad lt:300 1; pad rleft:400 1; done
    pad x:300 2; pad y:300 2; pad rthumb:300 2; pad rthumb:300 2; pad white:300 2; pad black:300 2; shot play
    # pause menu (Continue 0, Revert to Last Checkpoint 1, Restart Level 2, Controller Settings 3,
    # Save and Quit 4; A on Continue resumes, so the menu is reopened for every row)
    pad start:300 4; shot p0; for i in 1 2 3; do pad down:150 2; done; pad a:300 4; shot p3
    for r in 1 2 3 4 5 6; do pad a:300 4; shot p3_$r; pad b:300 3; pad down:150 2; done
    pad b:300 3; pad b:300 3; sleep 2; shot p3x
    pad start:300 4; pad down:150 2; pad a:300 4; shot p1; pad up:150 2; pad a:300 4; hudwait; shot p1c
    pad start:300 4; pad down:150 2; pad down:150 2; pad a:300 4; shot p2; pad up:150 2; pad a:300 4; hudwait; shot p2c
    for i in $(seq 4); do pad lup:1500 2; pad b:300 1; pad lt:300 1; done
    pad start:300 4; shot p9
    # Save and Quit is the last row (up wraps to it), then confirm
    pad up:150 3; shot q0; pad a:300 4; shot q1; pad up:150 2; pad a:300 15; shot q2
    for i in 1 2 3 4 5 6; do pad up:150 2; done; for i in 1 2 3 4; do pad down:150 2; done; shot m0; pad a:300 5; shot m1
    for r in 1 2 3 4 5 6 7 8; do pad a:300 4; shot m${r}a; pad down:150 2; pad a:300 4; pad b:300 3; pad b:300 3; pad down:150 3; done
    pad b:300 3; pad b:300 3; sleep 5; shot end ;;
  mplobby)
    # Split Screen lobby: Change Rules (another game type) and Change Map, then play it
    save_bak || { echo "$s: save backup failed"; continue; }; RESTORE=1
    lobby 2 || echo "$s: boot failed"
    setuprow 1; pad a:500 5; shot r0; for i in 1 2 3; do pad down:150 2; done; shot r1; pad a:500 5; shot r2; pad a:500 5; shot r3
    tosetup; setuprow 0; pad a:500 5; shot c0; for i in 1 2 3 4; do pad down:150 2; done; shot c1; pad a:500 5; shot c2
    mpstart; sleep 5; shot end ;;
  syslink)
    # System Link: host a game alone, pick another map and game type, play it
    save_bak || { echo "$s: save backup failed"; continue; }; RESTORE=1
    lobby 3 || echo "$s: boot failed"
    setuprow 0; pad a:500 5; shot c0; for i in 1 2 3 4 5 6; do pad down:150 2; done; shot c1; pad a:500 5; shot c2
    tosetup; setuprow 1; pad a:500 5; shot r0; pad down:150 2; shot r1; pad a:500 5; shot r2; pad a:500 5; shot r3
    mpstart; sleep 5; shot end ;;
  camp[0-9]*)
    # any campaign level by its Select Level row, then fight through it
    save_bak || { echo "$s: save backup failed"; continue; }; RESTORE=1
    camp_any ${s#camp} || { echo "$s: menu missed, retrying"; kill_exe; camp_any ${s#camp}; } || echo "$s: boot failed"; sleep 60; hudlong || echo "$s: no HUD"; shot c0
    fight ${FIGHT_MIN:-8}; shot end ;;
  mp[0-9]*_[0-9]*)
    # Split Screen: map row (Change Map, down N), game type row (Change Rules, down M), play
    save_bak || { echo "$s: save backup failed"; continue; }; RESTORE=1
    n=${s#mp}; m=${n#*_}; n=${n%_*}
    lobby 2 || echo "$s: boot failed"
    setuprow 1; pad a:500 5; for i in $(seq $m); do pad down:150 2; done; shot r1; pad a:500 5; pad a:500 5; shot r3
    tosetup; setuprow 0; pad a:500 5; for i in $(seq $n); do pad down:150 2; done; shot c1; pad a:500 5; shot c2
    mpstart; fight ${FIGHT_MIN:-5}; shot end ;;
  map[0-9]*)
    # Split Screen with the default rules: only the map changes (Change Map, down N), then play
    save_bak || { echo "$s: save backup failed"; continue; }; RESTORE=1
    n=${s#map}
    lobby 2 || echo "$s: boot failed"
    setuprow 0; pad a:500 5; for i in $(seq $n); do pad down:150 2; done; shot c1; pad a:500 5; shot c2
    mpstart; fight ${FIGHT_MIN:-5}; shot end ;;
  *) echo "unknown scenario $s"; continue ;;
  esac
  sleep 16   # one more fe.bin rewrite (every 15 s)
  kill_exe
  if [ -n "$RESTORE" ]; then save_restore; RESTORE=; CLEAN=1; else CLEAN=; fi
  echo "$s: $((SECONDS - t0)) s, $(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*\] //'), fe.bin $(stat -c %s "$O/fe.bin" 2>/dev/null || echo missing), snaps $(grep -a -c '\[FE\] snap ' "$O/out.log")"
done
