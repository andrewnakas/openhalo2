#!/bin/bash
# abpairs.sh <outdir> <flag> <n> [prefix]: in a running rig (RECOMP_GPU_AB=<outdir>/ab),
# take n pairs of screenshots: <prefix>A<i> as is, then <prefix>B<i> with the A/B
# file <flag> present; a pair is ~1 s apart. Builds <prefix>pairs.png (A over B,
# up to 8 pairs per row band) for a quick look.
O=${1:?outdir}; F=${2:?flag}; N=${3:-8}; P=${4:-p}
T=/d/n64work/openhalo2/tools
mkdir -p "$O/ab"
for i in $(seq 1 $N); do
  bash $T/shot.sh "$O" "${P}A$i" > /dev/null
  touch "$O/ab/$F"; sleep 0.3
  bash $T/shot.sh "$O" "${P}B$i" > /dev/null
  rm -f "$O/ab/$F"; sleep 0.3
done
python - "$O" "$P" "$N" <<'EOF'
import sys
from PIL import Image, ImageDraw
o, p, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
W, H = 320, 240
cols = min(n, 4)
rows = (n + cols - 1) // cols
s = Image.new('RGB', (W * cols, H * 2 * rows), (30, 30, 30))
d = ImageDraw.Draw(s)
for i in range(n):
    for k, ab in enumerate('AB'):
        try:
            im = Image.open('%s/%s%s%d.png' % (o, p, ab, i + 1)).convert('RGB').resize((W, H))
        except OSError:
            continue
        x, y = (i % cols) * W, ((i // cols) * 2 + k) * H
        s.paste(im, (x, y))
        d.text((x + 4, y + 4), '%s%d' % (ab, i + 1), fill=(255, 255, 0))
s.save('%s/%spairs.png' % (o, p))
print('%s/%spairs.png' % (o, p))
EOF
