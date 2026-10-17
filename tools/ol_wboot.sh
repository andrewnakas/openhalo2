#!/bin/bash
# ol_wboot.sh <dir> <gamertag> [extra query]: one Open Live copy in headless
# Chrome (the dev tree, build/web/site on port 8091, its own private file
# system in build/web/chrome_profile), driven to the Xbox Live menu
# (tools/ol_drive.py reads its screen). tools/ol_wdrive.py runs the page:
# drive it with tools/ol_wkeys.sh <dir>; stop it with: echo quit >> <dir>/cmd.txt
# Stage the site first (tools/webboot.sh does; port/web/build_web.sh links).
O=${1:?dir}; TAG=${2:?gamertag}; Q=${3:-}
case "$O" in ""|/|/d|/d/) echo "refusing dir '$O'"; exit 1;; esac
bash /d/n64work/openhalo2/tools/ol_wkill.sh ${CDPPORT:-9478} > /dev/null
rm -rf "$O"; mkdir -p "$O"; : > "$O/cmd.txt"
cd /d/n64work/halo2-cleanroom
curl -s -o /dev/null http://localhost:8091/index.html || { (python ports/wasm/serve.py /d/n64work/halo2/build/web/site 8091 > /dev/null 2>&1 &); sleep 2; }
URL="http://localhost:8091/index.html?link=openlive&env=RECOMP_OPEN_LIVE=1~RECOMP_OL_GAMERTAG=$TAG~RECOMP_XO_TRACE=1$Q"
(python /d/n64work/openhalo2/tools/ol_wdrive.py "$O" --url "$URL" ${WEBGL:+--webgl} \
   --profile D:/n64work/halo2/build/web/chrome_profile --port ${CDPPORT:-9478} --max ${MAXS:-14400} > "$O/driver.log" 2>&1 &)
until grep -aq "page: .* frames a second" "$O/console_a.txt" 2>/dev/null; do sleep 3; done
sleep ${BOOTWAIT:-25}
python /d/n64work/openhalo2/tools/ol_drive.py tolive "$(dirname "$O")" "$(basename "$O")" | tail -1
