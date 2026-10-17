"""whiteprobe.py <outdir> [tries]: look for blown-out white figures in a running
game and probe one. Takes frames with shot.sh; in each, finds 4x4 cells of pure
white (all channels >= 250) away from the HUD corners; at the first frame with
such a cell writes "x,y" to <outdir>/probe (the RECOMP_GL_PROBE file of the
run) for about a second, so every batch over that pixel is listed in out.log
between [PROBE] ---- frame lines. Prints the coordinate and keeps the frame as
white_hit.png."""
import os
import subprocess
import sys
import time

from PIL import Image

o = sys.argv[1]
tries = int(sys.argv[2]) if len(sys.argv) > 2 else 20
shot = '/d/n64work/openhalo2/tools/shot.sh'
for t in range(tries):
    subprocess.run(['bash', shot, o, 'white_try'], stdout=subprocess.DEVNULL)
    p = os.path.join(o, 'white_try.png')
    if not os.path.exists(p):
        continue
    im = Image.open(p).convert('RGB')
    w, h = im.size
    px = im.load()
    best = None
    cells = set()
    for y in range(40, h - 40, 2):
        for x in range(0, w, 2):
            if (x < 160 and y > 330) or (y < 70):
                continue                    # radar and the top HUD row
            r, g, b = px[x, y]
            if r >= 250 and g >= 250 and b >= 250:
                n = sum(1 for dy in range(-2, 3) for dx in range(-2, 3)
                        if 0 <= x + dx < w and 0 <= y + dy < h
                        and min(px[x + dx, y + dy]) >= 250)
                if n >= 20:
                    cells.add((x // 24, y // 24))
                if n >= 20 and (best is None or n > best[0]):
                    best = (n, x, y)
    if best and len(cells) < int(os.environ.get('MIN_CELLS', '1')):
        best = None
    if best and os.environ.get('NO_FIRE'):
        warm = sum(1 for yy in range(0, h, 4) for xx in range(0, w, 4)
                   if px[xx, yy][0] > 200 and px[xx, yy][2] < 140)
        if warm > (w // 4) * (h // 4) // 200:
            best = None
    if best:
        _, x, y = best
        im.save(os.path.join(o, 'white_hit.png'))
        with open(os.path.join(o, 'probe'), 'w') as f:
            f.write('%d,%d' % (x, y))
        time.sleep(1.0)
        os.remove(os.path.join(o, 'probe'))
        print('probed', x, y, 'try', t)
        sys.exit(0)
    if os.environ.get('KEEP'):
        im.save(os.path.join(o, 'try%03d.png' % t))
    time.sleep(0.5)
print('no white figure seen')
