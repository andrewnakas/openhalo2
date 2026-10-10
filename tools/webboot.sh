#!/bin/bash
# webboot.sh <report dir> [secs] [query] [keys]: rebuild what changed, link, boot the dev
# page in headless Chrome, print the end of its console. Dev server on 8091.
O=${1:?report dir}; S=${2:-25}; Q=${3:-}; K=${4:-}   # keys: cdp_shot --keys, e.g. "Enter@110,Space@135"
bash /d/n64work/openhalo2/port/web/build_web.sh | head -8
cp /d/n64work/openhalo2/port/web/dev.html /d/n64work/halo2/build/web/site/index.html
cp /d/n64work/openhalo2/port/web/input.js /d/n64work/openhalo2/port/web/audio.js /d/n64work/halo2/build/web/site/
# dev tree: site/game and site/save are junctions made by hand (see STATUS); the
# import list is rewritten each time
python /d/n64work/openhalo2/tools/web_manifest.py D:/n64work/halo2/build/web/site
cd /d/n64work/halo2-cleanroom
curl -s -o /dev/null http://localhost:8091/index.html || { (python ports/wasm/serve.py /d/n64work/halo2/build/web/site 8091 > /dev/null 2>&1 &); sleep 2; }
rm -f "$O/console.txt"
for try in 1 2 3; do   # the browser sometimes fails to come up right after the previous one closed
  python ports/wasm/cdp_shot.py "$O" --url "http://localhost:8091/index.html$Q" --secs $S ${K:+--keys "$K"} ${REALGPU:---webgl} --port 9477 --profile D:/n64work/halo2/build/web/chrome_profile 2>&1 | tail -1
  [ -s "$O/console.txt" ] && break; sleep 6
done
echo "console: $(wc -l < "$O/console.txt") lines"
grep -v "raw\[\|^\[log\] *$" "$O/console.txt" | cut -c1-${COLS:-230} | tail -${TAIL:-30}
