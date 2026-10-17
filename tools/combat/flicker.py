"""flicker.py <outdir> [secs] [tag]: with the rig running (RECOMP_FB_DUMP_FLIPS=<outdir>/dumpflag),
dump every presented frame for <secs> seconds of a still camera, then find
pixels that flicker: bright/dark changes of more than 40 levels between
consecutive frames, counted per pixel. Writes <tag>_flicker.png (first frame,
flickering pixels in magenta) and prints the share of pixels that flickered in
more than a quarter of the frame pairs, and the frames' count. The BMP dumps
are deleted afterwards."""
import glob
import os
import sys
import time

import numpy as np
from PIL import Image

o = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
tag = sys.argv[3] if len(sys.argv) > 3 else 'f'
for f in glob.glob(os.path.join(o, 'fb*.bmp')):
    try:
        os.remove(f)
    except OSError:
        pass
open(os.path.join(o, 'dumpflag'), 'w').close()
time.sleep(secs)
os.remove(os.path.join(o, 'dumpflag'))
time.sleep(1.0)
fs = sorted(glob.glob(os.path.join(o, 'fb*.bmp')))
frames = []
for f in fs:
    try:
        frames.append(np.asarray(Image.open(f).convert('L'), dtype=np.int16))
    except OSError:
        pass
frames = [f for f in frames if f.shape == frames[0].shape]
n = len(frames)
if n < 3:
    print('too few frames', n)
    sys.exit(1)
flips = np.zeros(frames[0].shape, np.int32)
for a, b in zip(frames, frames[1:]):
    flips += np.abs(a - b) > 40
bad = flips > (n - 1) / 4
first = np.asarray(Image.open(fs[0]).convert('RGB')).copy()
first[bad] = (255, 0, 255)
Image.fromarray(first).save(os.path.join(o, tag + '_flicker.png'))
print('%s: %d frames, %.3f%% of pixels flicker (%d px)' % (tag, n, 100.0 * bad.mean(), bad.sum()))
for f in fs:
    try:
        os.remove(f)
    except OSError:
        pass
