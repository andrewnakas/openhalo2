"""timeline.py <tl.bin> [min frame ms]: read a RECOMP_TIMELINE dump (see xbox_tl in the
toolkit's kernel_bridge.c) and print where the time between flips went.

  RECOMP_TIMELINE=<dir>/tl.bin in the environment arms it; `touch <dir>/tl.bin.req`
  writes the last ~4M events (20-200 s, depending on how busy the kernel is).
  Writing the dump stalls the game for a moment: do not measure across it.

Event types: 1/2 guest kernel call enter/return (a = ordinal, b = guest return
address, c = the word above it), 3 guest CPU taken back, 10/11 executor segment,
12 flip, 13/14 executor waits for a software interrupt, 16/17 periodic report,
20/21 vblank ISR, 22-24 SWI handler, 25-27 DPC, 34/35 occlusion query wait,
40/41 GL program build, 42/43 texture decode+upload (43: a = CPU decode us),
46/47 texture hash, 50/51 executor waits for the vertex worker threads.

Prints: totals per activity, texture upload cost by size (CPU decode vs driver),
frame-length histogram, then every frame over <min> ms (default 60) with the
executor's busy time, the largest silent gaps inside it, and the main guest
thread's longest kernel waits (ordinal@return address)."""
import collections, struct, sys
import numpy as np

DT = np.dtype([('t', '<i8'), ('tid', '<u4'), ('type', 'u1'), ('pad', 'u1'),
               ('a', '<u2'), ('b', '<u4'), ('c', '<u4')])
NAMES = {10: 'scanB', 11: 'scanE', 12: 'flip', 13: 'swiB', 14: 'swiE', 16: 'repB', 17: 'repE',
         34: 'qB', 35: 'qE', 40: 'progB', 41: 'progE', 42: 'decB', 43: 'decE', 46: 'hashB',
         47: 'hashE', 50: 'poolB', 51: 'poolE'}

f = sys.argv[1]
mn = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
fq = struct.unpack('<q', open(f, 'rb').read(8))[0]
ev = np.fromfile(f, DT, offset=8)
ev = ev[ev['t'] > 0]
ev.sort(order='t', kind='stable')
ty = ev['type']
ms = lambda x: x * 1000.0 / fq


def pairs(on, off, sel=None):
    st, out = None, []
    src = ev if sel is None else sel
    for e in src[(src['type'] == on) | (src['type'] == off)]:
        if e['type'] == on:
            st = e
        elif st is not None:
            out.append((st, e))
            st = None
    return out


exe_tid = collections.Counter(ev['tid'][ty == 10].tolist()).most_common(1)[0][0]
k1 = ev[ty == 1]
main_tid = collections.Counter(k1['tid'][np.isin(k1['a'], [129, 161, 301])].tolist()).most_common(1)[0][0]
span = ms(ev['t'][-1] - ev['t'][0]) / 1000.0
print('%s: %d events, %.1f s; executor tid %d, main guest tid %d' % (f, len(ev), span, exe_tid, main_tid))
for on, off, nm in ((10, 11, 'executor busy'), (50, 51, 'xf pool wait'), (46, 47, 'texture hash'),
                    (42, 43, 'texture upload'), (34, 35, 'query wait'), (40, 41, 'program build'),
                    (13, 14, 'swi wait'), (16, 17, 'report')):
    pp = pairs(on, off)
    d = np.array([ms(b['t'] - a['t']) for a, b in pp]) if pp else np.zeros(1)
    print('  %-15s n %8d  total %8.0f ms (%5.1f ms/s)  max %7.1f ms' % (nm, len(pp), d.sum(), d.sum() / max(span, 1e-9), d.max()))
per = collections.defaultdict(list)
for a, b in pairs(42, 43):
    per[(b['b'] & 0xFFFF, b['b'] >> 16)].append((ms(b['t'] - a['t']), b['a'] / 1000.0))
if per:
    print('  texture uploads by size (median ms: all / CPU decode / driver):')
    for k in sorted(per, key=lambda k: -len(per[k]))[:6]:
        v = per[k]
        print('    %4dx%-4d n %5d  %.3f / %.3f / %.3f' % (k[0], k[1], len(v), np.median([x for x, _ in v]),
              np.median([c for _, c in v]), np.median([x - c for x, c in v])))
flips = ev['t'][ty == 12]
fl = np.diff(flips) * 1000.0 / fq
if len(fl):
    print('  frames %d: mean %.1f ms; >40 ms %d, >60 ms %d, >100 ms %d, max %.0f ms' % (
        len(fl), fl.mean(), (fl > 40).sum(), (fl > 60).sum(), (fl > 100).sum(), fl.max()))
ex = ev[ev['tid'] == exe_tid]
mk = ev[(ev['tid'] == main_tid) & ((ty == 1) | (ty == 2))]
for i in range(len(flips) - 1):
    a, b = flips[i], flips[i + 1]
    if ms(b - a) < mn:
        continue
    m = ex[(ex['t'] >= a) & (ex['t'] <= b)]
    busy, st = 0, None
    for e in m:
        if e['type'] == 10:
            st = e['t']
        elif e['type'] == 11 and st is not None:
            busy += e['t'] - st
            st = None
    if st is not None:
        busy += b - st
    g = np.diff(m['t']) if len(m) > 1 else np.zeros(0)
    gi = np.argsort(g)[-3:][::-1] if len(g) else []
    gaps = ', '.join('%.0f ms after %s' % (ms(g[j]), NAMES.get(int(m['type'][j]), m['type'][j]))
                     for j in gi if ms(g[j]) > 5)
    km = mk[(mk['t'] >= a) & (mk['t'] <= b)]
    kw, s0, o = collections.Counter(), None, None
    for e in km:
        if e['type'] == 1:
            s0, o = e['t'], (int(e['a']), int(e['b']))
        elif s0 is not None:
            kw[o] += e['t'] - s0
            s0 = None
    kws = ', '.join('%d@%x %.0f ms' % (k[0], k[1], ms(v)) for k, v in kw.most_common(2) if ms(v) > 2)
    print('  flip %6d %6.0f ms: executor busy %5.0f | gaps: %s | main thread waits: %s' % (
        i, ms(b - a), ms(busy), gaps or '-', kws or '-'))
