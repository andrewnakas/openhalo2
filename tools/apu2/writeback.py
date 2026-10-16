"""writeback.py <capture dir with snapNNNN.bin + trace.bin> [vpv hex] [lo hex]

Which descriptor words change between consecutive RAM snapshots for voices the
title did not touch (no voice-method write while selected) in that interval:
those are the model's own write-backs. Prints per word: how many voices
changed it and example values.
"""
import sys, os, glob
from collections import defaultdict, Counter
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from apu_trace import load

d = sys.argv[1]
VPV = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x3608000
LO = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x3600000
us, off, size, wr, v = load(os.path.join(d, 'trace.bin'))
marks = {}
touched = set()
cur = None
seq = []
for i in range(len(us)):
    o = int(off[i])
    if o == 0xFFFFFF and size[i] == 0x7F:
        seq.append((int(v[i]), touched))
        touched = set()
        continue
    if wr[i] and o == 0x202F8:
        cur = int(v[i]) & 0xFFFF
    elif wr[i] and 0x20100 <= o < 0x20400:
        if cur is not None:
            touched.add(cur)
        if o in (0x20124, 0x20128, 0x20140):
            touched.add(int(v[i]) & 0xFFFF)
chg = defaultdict(Counter)
ex = defaultdict(list)
prev = None
for sn, t in seq:
    p = os.path.join(d, 'snap%04u.bin' % sn)
    if not os.path.exists(p):
        prev = None
        continue
    r = np.fromfile(p, dtype='<u4')
    b = (VPV - LO) // 4
    desc = r[b:b + 256 * 32].reshape(256, 32)
    if prev is not None:
        for vo in range(256):
            if vo in t:
                continue
            dif = np.nonzero(desc[vo] != prev[vo])[0]
            for k in dif:
                chg[int(k) * 4][vo] += 1
                if len(ex[int(k) * 4]) < 6:
                    ex[int(k) * 4].append('v%d %08X->%08X' % (vo, prev[vo][k], desc[vo][k]))
    prev = desc
for k in sorted(chg):
    print('+%02X changed in %d voices (%d times): %s' % (k, len(chg[k]), sum(chg[k].values()), '; '.join(ex[k])))
