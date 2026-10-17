#!/bin/bash
# ol_wkill.sh [port]: stops the headless browser the Open Live web rig started
# (tools/ol_wdrive.py: the debugging port, default 9478, and the dev profile
# build/web/chrome_profile in its command line). The browser re-parents its
# processes, so its first process id is not enough; nothing else is touched.
P=${1:-9478}
powershell -NoProfile -c "Get-CimInstance Win32_Process | Where-Object { \$_.CommandLine -like '*--remote-debugging-port=$P*' -and \$_.CommandLine -like '*build/web/chrome_profile*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue; 'stopped ' + \$_.ProcessId }"
