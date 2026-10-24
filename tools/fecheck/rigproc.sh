#!/bin/bash
# rigproc.sh: the process and path operations of the rigs (rigs_link.sh, rigs_cov.sh), for
# Windows (Git Bash / MSYS: tasklist, taskkill, powershell, cygpath) and Linux (the game under
# Wine: /proc, kill, renice). Sourced; defines rp_* functions and these variables:
#   RP_WIN   1 on Windows, empty on Linux
#   E        the E: drive as a bash path (/e on Windows; RIG_E on Linux, which must be a path
#            without spaces: the rigs leave many paths unquoted, so symlink the mount)
#   PY       the Python with Pillow (python on Windows; RIG_PY, default python3, on Linux)
#   RIG_LAUNCH  the command that runs the game exe (empty on Windows; "wine" on Linux, with
#            RIG_WINEPREFIX as its WINEPREFIX when set)
# Only PIDs the rig started are ever killed (rp_kill <pid>, with its children); there is no
# kill-by-name.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) RP_WIN=1 ;; *) RP_WIN= ;; esac
if [ -n "$RP_WIN" ]; then
  E=/e PY=${RIG_PY:-python} RIG_LAUNCH=
else
  E=${RIG_E:?set RIG_E to the E: drive mount on Linux (a path without spaces, e.g. a symlink)}
  case "$E" in *' '*) echo "RIG_E '$E' has a space: symlink the mount" >&2; exit 1 ;; esac
  PY=${RIG_PY:-python3} RIG_LAUNCH=${RIG_LAUNCH-wine}
  [ -n "$RIG_WINEPREFIX" ] && export WINEPREFIX=$RIG_WINEPREFIX
  export WINEDEBUG=${WINEDEBUG:--all}
fi
# shot.sh and the inline Pillow checks call `python`: the same interpreter in every child bash
python() { "$PY" "$@"; }
export -f python
export PY

# rp_gpath <path>: the path as the game sees it (an env var for the game: E:/x, Z:/home/x)
# rp_hpath <path>: the path as host tools (Python) take it (E:/x on Windows, /x on Linux)
# rp_upath <path>: the path as bash takes it (/e/x on Windows)
if [ -n "$RP_WIN" ]; then
  rp_gpath() { cygpath -m "$1"; }
  rp_hpath() { cygpath -m "$1"; }
  rp_upath() { cygpath -u "$1"; }
else
  rp_gpath() { local p; p=$(realpath -m "$1"); echo "Z:$p"; }
  rp_hpath() { realpath -m "$1"; }
  rp_upath() { echo "$1"; }
fi

# _rp_isexe <pid> <exe>: the process runs <exe> (Linux: an argv entry of a Wine process ends in it)
_rp_isexe() { { tr '\0' '\n' < "/proc/$1/cmdline"; } 2>/dev/null | head -3 | tr '\\' '/' | grep -qi "\(^\|/\)$2$"; }

# rp_alive <pid> <exe>: that PID is alive and is <exe>
rp_alive() {
  [ -n "$1" ] || return 1
  if [ -n "$RP_WIN" ]; then tasklist //FI "PID eq $1" 2>/dev/null | grep -qi "^$2"
  else _rp_isexe "$1" "$2"; fi
}

# rp_kill <pid>: kill that PID and its descendants (taskkill /T; a launcher stub that is still
# alive takes its game with it)
rp_kill() {
  [ -n "$1" ] || return 0
  if [ -n "$RP_WIN" ]; then taskkill //F //T //PID "$1" > /dev/null 2>&1; return 0; fi
  local all="$1" new="$1" p c
  while [ -n "$new" ]; do c=
    for p in $new; do c+=" $(pgrep -P "$p" | tr '\n' ' ')"; done
    new=$(echo $c); all+=" $new"; done
  kill -9 $all 2> /dev/null; return 0
}

# rp_pids <exe>: PIDs of every running <exe>
rp_pids() {
  if [ -n "$RP_WIN" ]; then
    # (with none running, tasklist prints an INFO line whose words are not PIDs)
    tasklist //FI "IMAGENAME eq $1" //FO CSV //NH 2>/dev/null | cut -d, -f2 | tr -d '"' | grep -E '^[0-9]+$'
    return 0
  fi
  local p; for p in $(pgrep -i -f -- "$1"); do _rp_isexe "$p" "$1" && echo "$p"; done; return 0
}

# rp_pids_in <dir> <exe>: PIDs of <exe> running from that folder (Linux: its working directory,
# the folder the rig starts it in)
rp_pids_in() {
  if [ -n "$RP_WIN" ]; then
    powershell -NoProfile -c "Get-Process ${2%.exe} -ea 0 | ?{\$_.Path -like '$(cygpath -w "$1")\\*'} | %{ \$_.Id }" | tr -d '\r'
    return 0
  fi
  local d p; d=$(realpath -m "$1")
  for p in $(rp_pids "$2"); do [ "$(readlink "/proc/$p/cwd" 2>/dev/null)" = "$d" ] && echo "$p"; done; return 0
}

# rp_prio <pid> <BelowNormal|Normal|Idle>: lower that process's priority
rp_prio() {
  if [ -n "$RP_WIN" ]; then powershell -NoProfile -c "(Get-Process -Id $1 -ea 0).PriorityClass='$2'" > /dev/null 2>&1
  else case "$2" in Idle) renice -n 19 -p "$1" ;; BelowNormal) renice -n 10 -p "$1" ;; esac > /dev/null 2>&1; fi
  return 0
}

# rp_owner_game: the owner is playing something (Windows: a ReSkate Skate.exe; Linux: a process
# whose name matches RIG_OWNER_GAMES, default ReSkate / Skate / GTA V recomp / a Steam game)
rp_owner_game() {
  if [ -n "$RP_WIN" ]; then tasklist 2>/dev/null | grep -qi "^skate.exe"; return; fi
  pgrep -i -x "${RIG_OWNER_GAMES:-skate|skate\.exe|reskate.*|gtav}" > /dev/null && return 0
  local p; for p in $(pgrep -x reaper); do tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q 'SteamLaunch AppId=' && return 0; done
  return 1
}

# rp_started <bgpid> <exe>: the PID of the game a `( cd ... && exec ... ) &` started, or empty
# after 60 s (then whatever was seen has been killed). Windows: the subshell's Windows PID changes
# while it execs env and env starts the game, so a PID is taken only once it is <exe> (or an <exe>
# child of a stub PID seen here). Linux: the exec chain (env, wine) keeps the PID.
rp_started() {
  local bp=$1 x=$2 p= q i seen=" "
  for i in $(seq 60); do sleep 1
    if [ -z "$RP_WIN" ]; then _rp_isexe "$bp" "$x" && { p=$bp; break; }
      kill -0 "$bp" 2> /dev/null || break; continue; fi
    q=$(cat /proc/$bp/winpid 2>/dev/null | tr -d '\r')
    if [ -n "$q" ]; then case "$seen" in *" $q "*) ;; *) seen+="$q " ;; esac
      tasklist //FI "PID eq $q" 2>/dev/null | grep -qi "^$x" && { p=$q; break; }; fi
    if [ $i -ge 10 ]; then p=$(powershell -NoProfile -c "\$s='$seen'.Trim() -split ' '; Get-CimInstance Win32_Process -Filter \"Name='$x'\" | ?{ \$s -contains [string]\$_.ParentProcessId } | %{ \$_.ProcessId } | select -First 1" 2>/dev/null | tr -d '\r')
      [ -n "$p" ] && break; fi
  done
  if [ -z "$p" ]; then
    if [ -n "$RP_WIN" ]; then for q in $seen; do rp_kill $q; done; else rp_kill $bp; fi
  fi
  echo "$p"
}
