#!/bin/bash
# ol_kill.sh <outdir>...: stops the copies tools/ol_boot.sh started there (by
# their Windows process id), leaving any other running copy alone.
for O in "$@"; do
  [ -s "$O/winpid" ] && taskkill //F //T //PID "$(cat "$O/winpid")" > /dev/null 2>&1 && echo "stopped $O"
done
