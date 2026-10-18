"""P1 runtime facts: merge RECOMP_FE_OUT tables from rig runs into
runtime/functions_runtime.csv (+ runtime/functions_runtime.json per function).

    python facts.py <scenario>=<fe.bin> [<scenario>=<fe.bin> ...] [--out runtime/functions_runtime.csv]

Columns: va, size, status, owner, calls (sum), scenarios (where reached, with
counts), callers (caller function VAs seen, from the return addresses at
entry), call_sites, indirect (call sites that reach it through a register or
memory operand: vtable slots and function pointers), arg_shape (per stack
argument / register: the class of the sampled values), samples.

Value classes (no retail bytes, only shapes): 0, small int (<0x10000),
neg (negative int32), float (plausible IEEE single), code (inside .text),
rdata, data (writable XBE sections), heap (other RAM below 64 MB),
contig (0x80000000 window), stack (near the sampled esp), other.
"""
import bisect
import collections
import json
import os
import struct
import sys

import common

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi']


def read_table(path):
    b = open(path, 'rb').read()
    magic, n, ncall, nsamp = struct.unpack_from('<4I', b, 0)
    assert magic == 0x31544546, path
    o = 16
    recs = {}
    for _ in range(n):
        va, = struct.unpack_from('<I', b, o); o += 4
        cnt, = struct.unpack_from('<Q', b, o); o += 8
        callers = struct.unpack_from('<%dI' % ncall, b, o); o += 4 * ncall
        hits = struct.unpack_from('<%dI' % ncall, b, o); o += 4 * ncall
        ns, = struct.unpack_from('<I', b, o); o += 4
        samp = struct.unpack_from('<%dI' % (nsamp * 16), b, o); o += 4 * nsamp * 16
        snaps, = struct.unpack_from('<I', b, o); o += 4
        recs[va] = dict(count=cnt, callers=[(c, h) for c, h in zip(callers, hits) if c],
                        samples=[samp[16 * i:16 * i + 16] for i in range(min(ns, nsamp))], snaps=snaps)
    return recs


class Classifier:
    def __init__(self):
        self.secs = [(v, v + vs, nm, ex) for nm, v, vs, raw, ex in common.xbe_sections()]

    def __call__(self, v, esp):
        if v == 0:
            return '0'
        if esp - 0x20000 <= v <= esp + 0x2000:
            return 'stack'
        for lo, hi, nm, ex in self.secs:
            if lo <= v < hi:
                return 'code' if ex else ('rdata' if nm in ('.rdata', '$$XTIMAGE', '$$XSIMAGE') else 'data')
        if v < 0x10000:
            return 'small'
        if 0x80000000 <= v < 0x84000000:
            return 'contig'
        if v < 0x04000000:
            return 'heap'
        if v >= 0x80000000 and v > 0xFFFF0000:
            return 'neg'
        f = struct.unpack('<f', struct.pack('<I', v))[0]
        if 1e-6 < abs(f) < 1e7:
            return 'float'
        if v >= 0x80000000:
            return 'neg'
        return 'other'


def main():
    args = sys.argv[1:]
    out = os.path.join(common.RUNTIME, 'functions_runtime.csv')
    if '--out' in args:
        i = args.index('--out'); out = args[i + 1]; del args[i:i + 2]
    rows = common.functions()
    starts = sorted(rows)
    cls = Classifier()
    code = [(v, v + vs, raw) for nm, v, vs, raw, ex in common.xbe_sections() if ex]

    def func_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < starts[i] + rows[starts[i]]['size_i']:
            return starts[i]
        return None

    def site_kind(ret):
        """'direct', 'indirect' or '?' for the call instruction ending at ret."""
        for lo, hi, raw in code:
            if lo + 6 <= ret <= hi:
                b = raw[ret - lo - 6:ret - lo]
                if b[1] == 0xE8:
                    return 'direct'
                if b[4] == 0xFF and (b[5] & 0x38) == 0x10 and b[5] >= 0xC0:
                    return 'indirect'      # call reg
                for k in range(0, 5):      # call [mem] forms of 2..6 bytes ending at ret
                    if b[k] == 0xFF and ((b[k + 1] >> 3) & 7) == 2:
                        return 'indirect'
                return '?'
        return '?'
    merged = collections.defaultdict(lambda: dict(count=0, scen={}, callers=collections.Counter(), samples=[]))
    for a in args:
        scen, _, path = a.partition('=')
        for va, r in read_table(path).items():
            if not r['count']:
                continue
            m = merged[va]
            m['count'] += r['count']
            m['scen'][scen] = m['scen'].get(scen, 0) + r['count']
            for c, h in r['callers']:
                m['callers'][c] += h
            m['samples'] += r['samples']
    game = [v for v in rows if rows[v]['owner'] == 'game']
    out_rows, js = [], {}
    for va in sorted(set(game) | set(merged)):
        r = rows.get(va)
        m = merged.get(va)
        d = dict(va='%08x' % va, size=r['size'] if r else '', status=r['status'] if r else 'unlisted',
                 owner=r['owner'] if r else '', calls=0, scenarios='', callers='', call_sites='', indirect='',
                 arg_shape='', samples=0)
        if m:
            d['calls'] = m['count']
            d['scenarios'] = ' '.join('%s:%d' % kv for kv in sorted(m['scen'].items()))
            sites = m['callers'].most_common()
            d['call_sites'] = ' '.join('%08x' % s for s, _ in sites)
            d['callers'] = ' '.join(sorted({'%08x' % f for f in (func_of(s) for s, _ in sites) if f}))
            d['indirect'] = ' '.join('%08x' % s for s, _ in sites if site_kind(s) == 'indirect')
            shape = []
            for j, nm in enumerate(REGS + ['s%d' % k for k in range(8)]):
                col = j if j < 6 else 8 + (j - 6)
                cs = collections.Counter(cls(s[col], s[6]) for s in m['samples'])
                shape.append('%s=%s' % (nm, '/'.join(k for k, _ in cs.most_common(3))))
            d['arg_shape'] = ' '.join(shape)
            d['samples'] = len(m['samples'])
            js['%08x' % va] = dict(calls=m['count'], scenarios=m['scen'], call_sites=dict(('%08x' % s, h) for s, h in sites),
                                   shape=d['arg_shape'])
        out_rows.append(d)
    common.write_csv(out, out_rows, ['va', 'size', 'status', 'owner', 'calls', 'scenarios', 'callers', 'call_sites',
                                     'indirect', 'arg_shape', 'samples'])
    json.dump(js, open(os.path.splitext(out)[0] + '.json', 'w'), indent=0)
    reached = [v for v in game if v in merged]
    by = collections.Counter(rows[v]['status'] for v in reached)
    tot = collections.Counter(rows[v]['status'] for v in game)
    print('facts: %d game functions, %d reached (%.1f%%): %s' % (len(game), len(reached), 100.0 * len(reached) / len(game),
          ', '.join('%s %d/%d' % (k, by[k], tot[k]) for k in sorted(tot))))
    for scen in sorted({s for m in merged.values() for s in m['scen']}):
        n = sum(1 for v in game if v in merged and scen in merged[v]['scen'])
        print('  %-10s %5d game functions' % (scen, n))
    ind = sum(1 for r in out_rows if r['indirect'])
    print('  reached through an indirect call: %d functions; wrote %s' % (ind, out))


if __name__ == '__main__':
    main()
