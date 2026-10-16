"""methodmap.py <capture dir> [vpv_pa hex] [window_lo hex]

For every voice method write (0x20300-0x203FC) followed by a RAM snapshot
before the same voice's method is written again, find which 32-bit word of
that voice's descriptor (VPV base + voice * 0x80) holds the written value.
Prints, per method, the descriptor offsets that matched and how often.
"""
import sys, os
from collections import defaultdict, Counter
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from apu_trace import load

d = sys.argv[1]
VPV = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x3608000
LO = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x3600000
us, off, size, wr, v = load(os.path.join(d, 'trace.bin'))
cur = None
pending = []   # (method, voice, value)
hits = defaultdict(Counter)
tries = Counter()
for i in range(len(us)):
    o = int(off[i]); val = int(v[i])
    if o == 0xFFFFFF and size[i] == 0x7F:
        sn = os.path.join(d, 'snap%04u.bin' % val)
        if not os.path.exists(sn):
            continue
        ram = np.fromfile(sn, dtype='<u4')
        for m, vo, x in pending:
            base = (VPV - LO + vo * 0x80) // 4
            desc = ram[base:base + 32]
            tries[m] += 1
            for k in np.nonzero(desc == x)[0]:
                hits[m][int(k) * 4] += 1
        pending = []
        continue
    if not wr[i]:
        continue
    if o == 0x202F8:
        cur = val & 0xFFFF
    elif 0x20300 <= o < 0x20400 and cur is not None:
        pending = [p for p in pending if not (p[0] == o and p[1] == cur)]
        if val not in (0, 0xFFFFFFFF):
            pending.append((o, cur, val))
for m in sorted(tries):
    print('%05X tries %3d: %s' % (m, tries[m], ' '.join('+%02X:%d' % kv for kv in hits[m].most_common(4))))
