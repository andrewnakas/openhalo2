"""voices.py <trace.bin> : per voice-on, the voice, list, format word, bin
selectors, volumes, pitch and HRTF entry as the descriptor stood (from the
descriptor dumps), tallied by kind. Our field decoding (docs/APU_FACTS.md)."""
import sys, os
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from apu_trace import load_full


def bins(d0, d1):
    s = [(d0 >> 0) & 31, (d0 >> 5) & 31, (d0 >> 10) & 31, (d0 >> 16) & 31, (d0 >> 21) & 31,
         (d0 >> 26) & 31, d1 & 31, (d1 >> 5) & 31]
    return s


def vols(a, b, c):
    v = [(a >> 4) & 0xFFF, a >> 20, (b >> 4) & 0xFFF, b >> 20, (c >> 4) & 0xFFF, c >> 20,
         (a & 15) | ((b & 15) << 4) | ((c & 15) << 8),
         ((a >> 16) & 15) | (((b >> 16) & 15) << 4) | (((c >> 16) & 15) << 8)]
    return v


us, off, size, wr, v, desc = load_full(sys.argv[1])
lastdesc = {}
ante = None
kinds = Counter()
examples = defaultdict(list)
for i in range(len(us)):
    if i in desc:
        t, vo, d = desc[i]
        lastdesc[vo] = d
        continue
    if not wr[i]:
        continue
    o = int(off[i])
    if o == 0x20120:
        ante = int(v[i])
    elif o == 0x20124:
        vo = int(v[i]) & 0xFFFF
        d = lastdesc.get(vo)
        if d is None:
            continue
        lst = ((ante or 0) >> 16) - 1
        fmt = int(d[1])
        b = bins(int(d[0]), fmt)
        vv = vols(int(d[0x18]), int(d[0x19]), int(d[0x1A]))
        key = 'list%d fmt%08X' % (lst, fmt & 0xFFFFFC00)
        kinds[key] += 1
        if len(examples[key]) < 3:
            examples[key].append('t=%.2f v%d on=%08X bins %s vols %s pitch %d hrtf %04X base %06X end %06X +18=%08X +74=%08X +78=%08X' % (
                us[i] / 1e6, vo, int(v[i]), b, ['%03X' % x for x in vv], ((int(d[0x1F]) >> 16) ^ 0x8000) - 0x8000,
                int(d[7]) & 0xFFFF, int(d[8]) & 0xFFFFFF, int(d[0x17]) & 0xFFFFFF, int(d[6]), int(d[0x1D]), int(d[0x1E])))
for k, n in kinds.most_common():
    print('%-28s x%d' % (k, n))
    for e in examples[k]:
        print('     ' + e)
