"""scr.py <png> [refdir]: name of the nearest reference screen (link_refs/*.png) and its distance.
Frames are compared as 80x60 grayscale thumbnails (mean absolute difference, 0-255); the list
screens (Select Game / Select Variant / Select Map) look alike there, so among those the title
band (x 40-340, y 30-75) decides."""
import sys, os, glob
from PIL import Image
LISTS = ('gametype', 'variant', 'selmap')
def thumb(im): return im.resize((80, 60)).tobytes()
def head(im): return im.crop((40, 30, 340, 75)).resize((150, 22)).tobytes()
def dist(a, b): return sum(abs(x - y) for x, y in zip(a, b)) / len(a)
def main():
    d = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'link_refs')
    im = Image.open(sys.argv[1]).convert('L'); t, h = thumb(im), head(im)
    refs = []
    for r in glob.glob(os.path.join(d, '*.png')):
        ri = Image.open(r).convert('L')
        refs.append((dist(t, thumb(ri)), os.path.basename(r)[:-4].rstrip('0123456789'), head(ri)))
    refs.sort(key=lambda x: x[0])
    s, n, _ = refs[0]
    if n in LISTS:
        n = min((x for x in refs if x[1] in LISTS), key=lambda x: dist(h, x[2]))[1]
    print(n, int(s))
main()
