#!/bin/bash
# rigs_cov.sh <outroot> <scenario>... [-- extra env assignments]
# Coverage scenarios past plain multiplayer, for functions no recording has run yet
# (roadmap.py category 'reach'; reach_features.py sorts them by feature). Built on rigs_link.sh
# (sourced as a library: same copies, boot, screen reading, PID-only kills, owner check).
#
# One copy (A = DIRA, signed in as Halo0001):
#   menus        (MENUS=<parts> picks parts) blind crawl (cov_drive.py) of the front end: Campaign options, Xbox Live without
#                Live (its sign-in/error dialogs), Split Screen and System Link Custom Game Setup,
#                Settings (Player Profiles, Game Variants: editors several levels deep). The copy's
#                UDATA is put back from PRISTINE_GAME before and after (the crawl may edit or
#                delete profiles and variants).
#   pause<row>   campaign level <row> (Select Level row), fight, then crawl the pause menu.
#   coop<row>    split-screen co-op: two pads (RECOMP_USB_PADS=2, pad p2 joins), Campaign ->
#                Cooperative, level <row>, both players fight.
#   live         Open Live (RECOMP_OPEN_LIVE=1, local gamertag; the transport is the relay of
#                RECOMP_NET_RELAY, default the public one): sign in, crawl the Xbox Live menu
#                (Quickmatch / Optimatch playlists and their search, Create Party) and the Y
#                panel (friends, clan, recent players).
# Two copies (System Link, as rigs_link.sh link<map>_<type>; map/type rows as there):
#   dropB<map>_<type>     both play, then the joiner's process is killed (peer lost); A plays on
#   dropA<map>_<type>     both play, then the host's process is killed (host lost on B)
#   leaveB<map>_<type>    both play, then B leaves from its game menu (Leave Game + confirm)
#   lobby<map>_<type>     B joins the lobby, leaves it, joins again; then A leaves its own lobby
#                         with B in it (party disbanded on B)
#   linkcoop<row>         A switches Custom Game Setup to Co-Op, campaign level <row>; B joins;
#                         both play the level
# Reports <outroot>/<scenario>/{A,B}/, snapshots FE_SNAP_ROOT/{A,B}/<scenario> (as rigs_link.sh;
# link_measure.py reads both). One summary line per scenario.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RIG_LIB=1 source "$HERE/rigs_link.sh" "$@" || exit 1
PRISTINE_UDATA=${PRISTINE_UDATA:-$PRISTINE_GAME/UDATA}
FIGHT_MIN=${FIGHT_MIN:-4}

# has <anchor>: the menus scenario crawls that part (MENUS, default all: camp live0 split link settings)
has() { [[ " ${MENUS:-camp live0 split link settings} " == *" $1 "* ]]; }
# udata_reset <dir>: the copy's profiles back from the pristine source (read only)
udata_reset() { case "$1" in "$E"/h2link?) ;; *) echo "refusing udata_reset '$1'"; return 1;; esac
  [ -d "$PRISTINE_UDATA" ] || return 1; rm -rf "$1/game/UDATA" && cp -r "$PRISTINE_UDATA" "$1/game/UDATA"; }
# tomain <profile row> <main row>: from wherever the copy is in the front end to the main menu
# with row <main row> lit (Campaign 0, Xbox Live 1, Split Screen 2, System Link 3, Settings 4).
# LIVEACCT=1: on Live's sign-in list take the account (row 1) instead of Don't Sign In.
# Returns 2 when the copy is in a game (the caller kills it).
tomain() { local prof=$1 row=$2 i n d r
  for i in $(seq ${NAVMAX:-40}); do read n d <<< "$(scr)"; [ $d -gt 30 ] && n=other
    case $n in
    mainmenu) r=$(lit "$O/cl.png" 405 435 287 315 343 371 399)
      [ "$r" = $row ] && { cp "$O/cl.png" "$O/s_main$row.png"; return 0; }
      if [ "${r:--1}" -gt $row ]; then pad up:450 3; else pad down:450 3; fi ;;
    title) pad start:1500 6 ;;
    profiles) r=$(lit "$O/cl.png" 220 450 242 263 284 305)
      if [ "$r" = $((1 + prof)) ]; then pad a:500 6
      elif [ "${r:--1}" -gt $((1 + prof)) ]; then pad up:450 3; else pad down:450 3; fi ;;
    signin) if [ -n "$LIVEACCT" ]; then r=$(lit "$O/cl.png" 240 440 242 264 286)
        if [ "$r" = 1 ]; then pad a:500 15; elif [ "${r:--1}" -gt 1 ]; then pad up:450 3; else pad down:450 3; fi
      else pad a:500 6; fi ;;
    players) pad b:500 5 ;;   # (the screen after a main-menu choice: back)
    ingame) return 2 ;;
    black) sleep 4 ;;
    *) # a dialog or a screen the crawl left open: back out (every other look, so fades pass)
      [ $((i % 2)) = 0 ] && pad b:500 4 || sleep 3 ;;
    esac; done; return 1; }
# reboot_a: kill copy A and start it again (a crawl that ended in a game or a hang)
reboot_a() { rp_kill "${PID[A]}"; PID[A]=; sleep 5; boot A; use A; }
# anchor <main row>: to that main-menu row, rebooting once when the copy is lost
anchor() { tomain 0 $1 && return 0; reboot_a && tomain 0 $1; }
# crawl <depth> <tag> [SKIP list]: cov_drive.py from the current screen (screens c_<tag>_*.png)
crawl() { SKIP=$3 RIG_TOOLS=$T python "$HERE/cov_drive.py" crawl "$(rp_hpath "$O")" $1 ${ROWS:-8} $2 > /dev/null; local rc=$?
  st+="$2:$([ $rc = 0 ] && echo ok || echo lost) "; return $rc; }
# tolobby1: after A on Split Screen, A through the player screens until the lobby shows
tolobby1() { local i n d; for i in $(seq 6); do read n d <<< "$(scr)"; [ $d -le 30 ] && [ $n = lobby ] && return 0
  case $n in players|signin|profiles) pad a:500 6 ;; *) sleep 4 ;; esac; done; return 1; }
# cshot <label>: screenshot kept as s_<label>.png
cshot() { shot "s_$1"; }
# camp_go <row>: from the main menu (Campaign lit) through Campaign Options (Resume 0 / Select
# Level 1 / Cooperative 2 at y 192 213 235) -> Select Level row <row> (y 92 + 22n) -> the top
# difficulty
camp_go() { local ROWS15 n d
  # (a copy not yet signed in for the campaign gets Choose Profile (Halo0001, row 1) and then
  # Sign In To Xbox Live (A on its default, Don't Sign In) before Campaign Options)
  pad a:500 8
  for k in 1 2 3; do read n d <<< "$(scr)"; [ $d -le 30 ] || break
    case $n in
    profiles) rowto 1 220 450 242 263 284 305 || return 1; cshot campprofile; pad a:500 8 ;;
    signin) cshot campsignin; pad a:500 8 ;;
    *) break ;;
    esac; done
  rowto 1 100 300 192 213 235 || return 1; cshot campopt; pad a:500 8
  ROWS15=$(python -c "print(' '.join(str(92+22*i) for i in range(15)))")
  rowto $1 140 330 $ROWS15 || return 3; cshot level; pad a:500 8; cshot diff
  for i in 1 2 3; do pad up:300 2; done; pad a:500 8; cshot go; }
# hudlong: up to 15 minutes through loading and the opening cinematic until the campaign HUD's
# shield bar shows (x 50-130, y 344-360: blue with some red); A every 30 s skips cinematics
hudlong() { local i v; for i in $(seq 180); do alive || return 1; shot hud
    v=$(python -c "import sys;from PIL import Image;v=list(Image.open(sys.argv[1]).convert('RGB').crop((50,344,130,360)).getdata());r=sum(p[0] for p in v)/len(v);print('H' if r>20 and sum(p[2] for p in v)/len(v)>140 else '-')" "$O/hud.png" 2>/dev/null)
    [ "$v" = H ] && return 0; [ $((i % 6)) = 0 ] && pad a:300 1; sleep 5; done; return 1; }
# fight2 <minutes>: fight with both pads of a split-screen copy (pad 1 and p2- lines alternate)
fight2() { local end=$((SECONDS + $1 * 60)) k; while [ $SECONDS -lt $end ]; do alive || return 1
    for k in lup:1500 rright:350 rt:1000 x:500 lleft:800 rt:800 b:300 y:300 a:300 lt:300 ldown:800 rleft:400; do
      pad $k 0.6; pad p2-$k 0.6; done; done; }
# live_menu: 1 when the frame is Open Live's Xbox Live menu (header of the openlive2 reference)
OLREF=${OLREF:-$E/halo2/reports/openlive2/ref}
live_menu() { python -c "import sys;from PIL import Image
a=Image.open(sys.argv[1]).convert('L').crop((0,30,640,60)).tobytes();b=Image.open(sys.argv[2]).convert('L').crop((0,30,640,60)).tobytes()
print(1 if sum(abs(x-y) for x,y in zip(a,b))/len(a)<12 else 0)" "$1" "$OLREF/live.png" 2>/dev/null; }
# ingame_any: up to 5 minutes for a map: the MP motion tracker or the campaign shield bar
ingame_any() { local i n d; for i in $(seq 40); do alive || return 1; read n d <<< "$(scr)"; [ $n = ingame ] && return 0
  hudlong_once && return 0; sleep 6; done; return 1; }
hudlong_once() { [ "$(python -c "import sys;from PIL import Image;v=list(Image.open(sys.argv[1]).convert('RGB').crop((50,344,130,360)).getdata());r=sum(p[0] for p in v)/len(v);print('H' if r>20 and sum(p[2] for p in v)/len(v)>140 else '-')" "$O/cl.png" 2>/dev/null)" = H ]; }
# both_play <minutes>: both copies fight at once (as rigs_link.sh)
both_play() { local fp=() c; for c in A B; do ( use $c; fight $1; shot s_fight_$2 ) & fp+=($!); done; wait "${fp[@]}"; }
# link_up <map> <type row>: rigs_link.sh's host + join + start, both in the map
link_up() { local r n d
  if boot A; then use A; toavail 0 && host $1 $2; r=$?; [ $r = 0 ] && st+="hosted " || st+="host-fail($r) "; else st+="A-noboot "; return 1; fi
  if boot B; then use B; toavail 1 && join; r=$?; [ $r = 0 ] && st+="joined " || st+="join-fail($r) "; else st+="B-noboot "; return 1; fi
  use B; read n d <<< "$(scr)"; case $2 in 0|2|3) ;; *) [ $n = lobby ] && teamswap ;; esac
  use A; shot s_prestart; mpgo && st+="started " || st+="start-fail "
  for c in A B; do use $c; ingame_wait && shot s_inmap || st+="$c-nomap "; done; }
# watch <copy> <seconds> <label>: screenshots every 15 s; A on a dialog (no known screen)
watch() { local end=$((SECONDS + $2)) i=0 n d; use $1; while [ $SECONDS -lt $end ]; do alive || return 1
    i=$((i + 1)); read n d <<< "$(scr)"; cp "$O/cl.png" "$O/s_$3_$i.png" 2>/dev/null
    case $n in ingame) pad lup:1500 1; pad rt:800 1 ;; none|other) [ $d -gt 30 ] && pad a:500 1 ;; esac; sleep 12; done; }
# leave_game <copy>: Leave Game (row 0 of the game menu) and confirm (the confirmation's
# default is Cancel: up to the other button, as endgame does)
leave_game() { use $1; pad start:500 4; shot s_leavemenu; local r Y="140 380 201 223 245 267 289 311"
  for k in 1 2 3 4 5 6; do r=$(lit "$O/s_leavemenu.png" $Y); [ "$r" = 0 ] && break; pad up:350 4; shot s_leavemenu; done
  pad a:500 4; shot s_leaveconfirm; pad up:400 3; pad a:500 10; shot s_left; }
TYPESN=(slayer koth oddball juggernaut ctf assault territories)
typerow() { local i; for i in "${!TYPESN[@]}"; do [ "${TYPESN[$i]}" = "$1" ] && { echo $i; return; }; done; echo -1; }

for s in "${SC[@]}"; do
  while userbusy; do sleep 60; done   # never beside the owner's game
  setsc $s; kill_mine; t0=$SECONDS; st=""; SAVEXENV=("${XENV[@]}")
  case $s in
  menus*)
    udata_reset $DIRA || st+="udata-reset-fail "
    if boot A; then use A
      has camp && anchor 0 && { pad a:500 8; crawl 2 camp camp_0; }                          # Resume would load a level
      has live0 && anchor 1 && { pad a:500 10; crawl 2 live0; }                                # Xbox Live, no Open Live
      has split && anchor 2 && { pad a:500 8; tolobby1 && tosetup && crawl 2 split 'split_0_*'; }   # A on a map picks it
      has link && anchor 3 && { toavail 0 && press a lobby && tosetup && crawl 2 link 'link_0_*'; }
      has settings && anchor 4 && { pad a:500 8; CRAWL_MAX_STEPS=${SETTINGS_STEPS:-220} CRAWL_MAX_MIN=90 crawl 4 settings; }
      anchor 0; cshot end
    else st+="A-noboot "; fi
    kill_mine; udata_reset $DIRA ;;
  pause[0-9]*)
    if boot A; then use A; anchor 0 && camp_go ${s#pause} && hudlong && { st+="inlevel "; cshot c0; fight 3
        pad start:500 5; cshot pause; crawl 2 pause 'pause_0,pause_4,pause_1_*,pause_2_*'; cshot end; } || st+="nolevel "
    else st+="A-noboot "; fi ;;
  coop[0-9]*)
    # Campaign -> Cooperative opens Choose Profile (a quadrant per controller): pad 2 joins with
    # its own presses, pad 1 continues, then the Split Screen: Cooperative lobby (the level and
    # difficulty the save remembers; <row> = Game Setup's level row when COOP_LEVEL=1)
    XENV+=(RECOMP_USB_PADS=2)
    if boot A; then use A; anchor 0 && { pad a:500 8; rowto 2 100 300 192 213 235; cshot campopt; pad a:500 8; cshot choose
        for k in 1 2 3; do pad p2-start:500 4; pad p2-a:500 5; shot co_${k}a; pad p2-a:500 5; shot co_${k}b; pad a:500 6; shot co_${k}c
          read n d <<< "$(scr)"; [ $n = lobby ] && [ "$(party2 "$O/cl.png")" = 1 ] && break; done
        cshot coop_lobby; [ "$(party2 "$O/cl.png")" = 1 ] && st+="2players " || st+="1player "
        mpgo && st+="started "; hudlong && st+="hud " || st+="nohud "; cshot c0; fight2 ${FIGHT_MIN}; cshot fightend
        pad start:500 5; cshot pause; pad b:500 3; pad p2-start:500 5; cshot pause2; pad p2-b:500 3; } || st+="menu-fail "
    else st+="A-noboot "; fi ;;
  live)
    XENV+=(RECOMP_OPEN_LIVE=1 RECOMP_OL_GAMERTAG=${OL_TAG:-CovRig} RECOMP_OL_PROFILE="$(rp_gpath "$DIRA")/openlive_cov.json")
    if boot A; then use A
      LIVEACCT=1 anchor 1 && pad a:500 12
      # the first sign-in asks which profile goes with the account: A until the Live menu shows
      for i in $(seq 8); do shot lv; [ "$(live_menu "$O/lv.png")" = 1 ] && break; pad a:500 10; done
      if [ "$(live_menu "$O/lv.png")" = 1 ]; then st+="live "; cshot live
        CRAWL_MAX_STEPS=150 crawl 3 live
        for i in 1 2 3 4; do shot lv; [ "$(live_menu "$O/lv.png")" = 1 ] && break; pad b:500 5; done
        pad y:500 8; cshot friends; CRAWL_MAX_STEPS=80 crawl 3 friends
      else st+="no-live-menu "; fi
    else st+="A-noboot "; fi ;;
  dropB[0-9]*_*|dropA[0-9]*_*|leaveB[0-9]*_*)
    m=${s#*[AB]}; ty=${m#*_}; m=${m%%_*}; tr=$(typerow $ty); [ $tr = -1 ] && { echo "$s: unknown game type"; continue; }
    link_up $m $tr && both_play $FIGHT_MIN 1
    case $s in
    dropB*) rp_kill "${PID[B]}"; PID[B]=; st+="B-killed "; watch A 240 after ;;
    dropA*) rp_kill "${PID[A]}"; PID[A]=; st+="A-killed "; watch B 240 after ;;
    leaveB*) leave_game B; st+="B-left "; watch A 120 after; watch B 60 after ;;
    esac
    use A; alive && { endgame; } ;;
  lobby[0-9]*_*)
    m=${s#lobby}; ty=${m#*_}; m=${m%%_*}; tr=$(typerow $ty)
    if boot A; then use A; toavail 0 && host $m $tr && st+="hosted "; fi
    if boot B; then use B; toavail 1 && join && st+="joined "
      pad b:500 5; cshot leave1; pad a:500 5; cshot leave2   # leave the lobby (B, confirm)
      toavail 1 && join && st+="rejoined "
      use A; pad b:500 5; cshot disband1; pad a:500 6; cshot disband2   # the host leaves its lobby
      watch B 90 hostleft; watch A 30 hostleft
    fi ;;
  linkcoop[0-9]*)
    lv=${s#linkcoop}
    if boot A; then use A; toavail 0 && press a lobby && tosetup && { cshot setup
        rowto 3 90 300 132 154 176 198 && pad a:500 8; cshot coopsetup
        # the co-op setup: its first row opens the level list
        rowto 0 90 300 132 154 176 198; pad a:500 8; cshot levels
        ROWS15=$(python -c "print(' '.join(str(92+22*i) for i in range(15)))")
        rowto $lv 95 330 $ROWS15 || rowto $lv 140 330 $ROWS15; cshot level; pad a:500 8; cshot chosen
        tolobby && st+="coop-hosted "; } || st+="host-fail "
    fi
    if boot B; then use B; toavail 1 && join && st+="joined "; fi
    use A; mpgo; for c in A B; do use $c; ingame_any && cshot inmap || st+="$c-nomap "; done
    both_play $FIGHT_MIN 1 ;;
  *) echo "unknown scenario $s"; continue ;;
  esac
  sleep 16   # one more fe.bin rewrite (every 15 s)
  kill_mine; XENV=("${SAVEXENV[@]}")
  line="$s: $((SECONDS - t0)) s, $st"
  for c in A B; do O=${OD[$c]}; [ -s "$O/out.log" ] || continue
    line+="| $c $(grep -a 'fps (' "$O/out.log" | tail -1 | sed 's/.*\] //'), fe.bin $(stat -c %s "$O/fe.bin" 2>/dev/null || echo missing), snaps $(grep -a -c '\[FE\] snap ' "$O/out.log") "; done
  echo "$line"
done
