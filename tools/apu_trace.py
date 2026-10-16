"""apu_trace.py <trace.bin> [mode] : read a RECOMP_APU_TRACE file.

modes:
  summary (default)  one line per offset: reads/writes, distinct values, first/last
  seq [from] [n]     the records in order (from = record index), one per line
  range lo hi [n]    only records with lo <= offset < hi (hex), in order
Record: u64 us, u32 offset|size<<24|write<<31, u32 value.
Special records (offset field): 0x7FFFFFFF snapshot marker (value = number);
0x7FFFFFFE / 0x7FFFFFFD descriptor of voice <value> before / after a voice
method write, followed by 128 raw bytes (RECOMP_APU_TRACE_DESC=1).
"""
import sys
from collections import Counter
import numpy as np

DT = np.dtype([('us', '<u8'), ('ow', '<u4'), ('v', '<u4')])


def load_full(p):
    """Returns (us, off, size, wr, v, desc) where desc maps a record index to
    (tag, voice, uint32[32]) for descriptor records (which keep their slot in
    the arrays with off = 0xFFFFFE / 0xFFFFFD)."""
    raw = np.fromfile(p, dtype=np.uint8)
    n = len(raw) // 16
    rec = raw[:n * 16].view(DT)
    ow = rec['ow']
    special = np.nonzero((ow == 0x7FFFFFFE) | (ow == 0x7FFFFFFD))[0]
    if len(special) == 0:
        keep = np.arange(n)
        desc = {}
    else:
        # walk: each descriptor header is followed by 8 payload slots
        skip = np.zeros(n, dtype=bool)
        desc = {}
        i = 0
        sp = set(special.tolist())
        idx = 0
        out_index = []
        while i < n:
            if i in sp and not skip[i]:
                payload = raw[(i + 1) * 16:(i + 9) * 16].view('<u4')
                desc[len(out_index)] = (int(ow[i]), int(rec['v'][i]), payload.copy())
                out_index.append(i)
                skip[i + 1:i + 9] = True
                i += 9
                continue
            out_index.append(i)
            i += 1
        keep = np.array(out_index)
    r = rec[keep]
    off = r['ow'] & 0xFFFFFF
    size = (r['ow'] >> 24) & 0x7F
    wr = (r['ow'] >> 31) & 1
    return r['us'], off, size, wr, r['v'], desc


def load(p):
    return load_full(p)[:5]


def name(o):
    return '%05X' % o


def main():
    us, off, size, wr, v, desc = load_full(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else 'summary'
    print('%d records, %.1f s, %d descriptor dumps' % (len(us), (us[-1] - us[0]) / 1e6 if len(us) else 0, len(desc)))
    real = off < 0xFFFFFD
    if mode == 'summary':
        keys = np.unique(off[real])
        for o in keys:
            m = off == o
            for w in (1, 0):
                mm = m & (wr == w)
                n = int(mm.sum())
                if not n:
                    continue
                vals = v[mm]
                c = Counter(vals.tolist())
                top = ' '.join('%X%s' % (k, '' if c[k] == 1 else 'x%d' % c[k]) for k, _ in c.most_common(6))
                print('%s %s n=%-7d sz=%d distinct=%-5d t=%.2f..%.2f  %s' % (
                    name(o), 'W' if w else 'R', n, int(size[mm][0]), len(c),
                    us[mm][0] / 1e6, us[mm][-1] / 1e6, top))
    elif mode == 'seq':
        a = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 200
        for i in range(a, min(a + n, len(us))):
            if i in desc:
                t, vo, d = desc[i]
                print('%7d %10.6f %s v%d %s' % (i, us[i] / 1e6, 'pre ' if t == 0x7FFFFFFE else 'post', vo,
                                               ' '.join('%08X' % x for x in d)))
            else:
                print('%7d %10.6f %s %s %08X' % (i, us[i] / 1e6, 'W' if wr[i] else 'R', name(off[i]), v[i]))
    elif mode == 'range':
        lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
        n = int(sys.argv[5]) if len(sys.argv) > 5 else 400
        idx = np.nonzero((off >= lo) & (off < hi))[0][:n]
        for i in idx:
            print('%7d %10.6f %s %s %08X' % (i, us[i] / 1e6, 'W' if wr[i] else 'R', name(off[i]), v[i]))


if __name__ == '__main__':
    main()
