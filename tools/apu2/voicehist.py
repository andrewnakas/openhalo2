"""voicehist.py <trace.bin> <voice> [max lines]

History of one voice: every method write while it is selected (or naming it),
and every descriptor word that changed since the last dump of that voice
(model write-backs show as changes in a 'pre' dump).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from apu_trace import load_full

us, off, size, wr, v, desc = load_full(sys.argv[1])
VO = int(sys.argv[2], 0)
mx = int(sys.argv[3]) if len(sys.argv) > 3 else 400
last = None
cur = None
n = 0
for i in range(len(us)):
    if n >= mx:
        break
    if i in desc:
        t, vo, d = desc[i]
        if vo != VO:
            continue
        if last is not None:
            ch = [(k, last[k], d[k]) for k in range(32) if last[k] != d[k]]
            if ch:
                print('%10.6f %s %s' % (us[i] / 1e6, 'wb  ' if t == 0x7FFFFFFE else 'set ',
                                        ' '.join('+%02X:%08X->%08X' % c for c in ch)))
                n += 1
        else:
            print('%10.6f init %s' % (us[i] / 1e6, ' '.join('%08X' % x for x in d)))
        last = d.copy()
        continue
    o = int(off[i])
    if not wr[i]:
        continue
    if o == 0x202F8:
        cur = int(v[i]) & 0xFFFF
        continue
    if 0x20100 <= o < 0x20400 and (cur == VO or (o in (0x20124, 0x20128, 0x20140) and (int(v[i]) & 0xFFFF) == VO)):
        if o != 0x202FC:
            print('%10.6f  W %05X %08X' % (us[i] / 1e6, o, v[i]))
            n += 1
