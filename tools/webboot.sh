#!/bin/bash
# webboot.sh <report dir> [secs] [query] [keys]: rebuild what changed, link, boot the dev
# page in headless Chrome, print the end of its console.
#
# Two dev trees, each its own origin (so its own private file system in the one
# browser profile):
#   default       build/web/site        port 8091   game/ and save/ are junctions to
#                                                   dirty/game and build/xr/save
#   WEBSET=clean  build/web/site_clean  port 8092   junctions to clean/game and
#                                                   build/xr_clean/save
# The junctions are made by hand once (mklink /J); the import list is rewritten
# each time. First boot of a tree: add ?import=1 (or ?import=only) to the query.
# REALGPU=" " uses the machine's GPU instead of Chrome's software GL.
O=${1:?report dir}; S=${2:-25}; Q=${3:-}; K=${4:-}   # keys: cdp_shot --keys, e.g. "Enter@110,Space@135"
BUILT=/d/n64work/halo2/build/web/site
if [ "$WEBSET" = clean ]; then SITE=/d/n64work/halo2/build/web/site_clean; PORT=8092; else SITE=$BUILT; PORT=8091; fi
bash /d/n64work/openhalo2/port/web/build_web.sh | head -8
mkdir -p $SITE
[ "$SITE" != "$BUILT" ] && cp $BUILT/halo2.js $BUILT/halo2.wasm $SITE/
cp /d/n64work/openhalo2/port/web/dev.html $SITE/index.html
cp /d/n64work/openhalo2/port/web/input.js /d/n64work/openhalo2/port/web/audio.js /d/n64work/openhalo2/port/web/net.js $SITE/
python /d/n64work/openhalo2/tools/web_manifest.py "$(cygpath -m $SITE)"
cd /d/n64work/halo2-cleanroom
curl -s -o /dev/null http://localhost:$PORT/index.html || { (python ports/wasm/serve.py $SITE $PORT > /dev/null 2>&1 &); sleep 2; }
rm -f "$O/console.txt"
for try in 1 2 3; do   # the browser sometimes fails to come up right after the previous one closed
  python ports/wasm/cdp_shot.py "$O" --url "http://localhost:$PORT/index.html$Q" --secs $S ${K:+--keys "$K"} ${REALGPU:---webgl} --port 9477 --profile D:/n64work/halo2/build/web/chrome_profile 2>&1 | tail -1
  [ -s "$O/console.txt" ] && break; sleep 6
done
echo "console: $(wc -l < "$O/console.txt") lines"
grep -v "raw\[\|^\[log\] *$" "$O/console.txt" | cut -c1-${COLS:-230} | tail -${TAIL:-30}
