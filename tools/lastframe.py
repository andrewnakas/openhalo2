"""Convert the newest frame dump in a report directory to PNG and print its name.
usage: python lastframe.py <dir> [n_back]"""
import glob, sys
from PIL import Image
d = sys.argv[1]; back = int(sys.argv[2]) if len(sys.argv) > 2 else 0
fs = sorted(glob.glob(d + '/fb*.bmp'))
if len(fs) <= back + 1:
    print('no frame yet (%d)' % len(fs)); sys.exit(0)
f = fs[-2 - back]            # the newest may still be being written
Image.open(f).convert('RGB').save(d + '/last.png')
print(f.replace(chr(92), '/').split('/')[-1], 'of', len(fs))
