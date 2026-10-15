"""blackscan.py <png|bmp> [...] [--sheet out.png] : black frames and black regions, one line each.

Whole-black: mean luma < 8. Black region: the largest 4-connected area of pixels
with luma < 6 on a 1/4-scale copy, as a share of the frame (> 2% is flagged).
Frames come out in the order given. --sheet writes a contact sheet of the flagged
frames with the regions tinted red.
"""
import sys

import numpy as np
from PIL import Image
from scipy import ndimage


def scan(path):
    im = Image.open(path).convert("RGB")
    a = np.asarray(im, np.float32)
    y = a @ np.array([0.299, 0.587, 0.114], np.float32)
    small = y[::4, ::4]
    lab, n = ndimage.label(small < 6)
    if n:
        sizes = ndimage.sum(np.ones_like(small), lab, range(1, n + 1))
        k = int(np.argmax(sizes)) + 1
        share = sizes[k - 1] / small.size
        mask = lab == k
    else:
        share, mask = 0.0, np.zeros_like(small, bool)
    return im, float(y.mean()), float(share), mask


def main():
    args = sys.argv[1:]
    sheet = None
    if "--sheet" in args:
        i = args.index("--sheet")
        sheet = args[i + 1]
        del args[i:i + 2]
    flagged = []
    for p in args:
        im, mean, share, mask = scan(p)
        tag = "BLACK FRAME" if mean < 8 else ("black region" if share > 0.02 else "ok")
        print(f"{p}: mean luma {mean:5.1f}, largest black area {share * 100:5.1f}%  {tag}")
        if tag != "ok":
            flagged.append((p, im, mask))
    print(f"{len(flagged)} of {len(args)} flagged")
    if sheet and flagged:
        tiles = []
        for p, im, mask in flagged[:12]:
            t = np.asarray(im.resize((320, 240)), np.uint8).copy()
            m = np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).resize((320, 240))) > 127
            t[m] = (t[m] * 0.3 + np.array([200, 0, 0]) * 0.7).astype(np.uint8)
            tiles.append(t)
        while len(tiles) % 3:
            tiles.append(np.zeros((240, 320, 3), np.uint8))
        rows = [np.concatenate(tiles[i:i + 3], 1) for i in range(0, len(tiles), 3)]
        Image.fromarray(np.concatenate(rows, 0)).save(sheet)
        print("sheet", sheet)


if __name__ == "__main__":
    main()
