#!/bin/bash
# web_pair.sh <dir> [max secs]: two tabs of one headless browser on the clean
# dev tree (port 8092), linked tab-to-tab (?link=bc), tab b on its own save
# folder (/save_b, made once with ?clonesave=save_b&clonesave_only=1).
# Drive with:  bash web_pair.sh --do <dir> "a key Space 300"   (see cdp_pair.py)
#              bash web_pair.sh --sheet <dir> <label>          (both canvases side by side)
if [ "$1" = "--do" ]; then echo "$3" >> "$2/cmd.txt"; exit 0; fi
if [ "$1" = "--sheet" ]; then
  echo "shot $3" >> "$2/cmd.txt"; sleep ${SHOTWAIT:-4}
  python - "$2" "$3" <<'PY'
import sys
from PIL import Image
o, l = sys.argv[1:3]
ims = [Image.open(f'{o}/{l}_{x}.png').convert('RGB') for x in 'ab']
w = Image.new('RGB', (1280, 480))
for i, im in enumerate(ims):
    w.paste(im.resize((640, 480)), (i * 640, 0))
w.save(f'{o}/{l}.png')
PY
  for x in a b; do echo "$x: $(grep -a 'frames a second' $2/console_$x.txt | tail -1 | cut -c7-60) | $(grep -a 'link: sent' $2/console_$x.txt | tail -1 | cut -c7-70)"; done
  exit 0
fi
O=${1:?dir}; mkdir -p $O
cd /d/n64work/halo2-cleanroom
curl -s -o /dev/null http://localhost:8092/index.html || { (python ports/wasm/serve.py /d/n64work/halo2/build/web/site_clean 8092 > /dev/null 2>&1 &); sleep 2; }
python ports/wasm/cdp_pair.py $O --url-a "http://localhost:8092/index.html?link=bc${QA}" \
  --url-b "http://localhost:8092/index.html?link=bc&save=save_b${QB}" \
  --profile D:/n64work/halo2/build/web/chrome_profile --max ${2:-1700} > $O/driver.log 2>&1
