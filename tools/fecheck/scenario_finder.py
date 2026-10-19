"""What gameplay would reach the functions no recording has run yet.

    python scenario_finder.py [--decomp <tree>]

Reads WORK/roadmap/roadmap.csv (category 'reach') and groups those functions:
- by gateway: the nearest callers that DID run (from the in-game traces); code under one
  gateway shares one untaken branch, so one scenario reaches the group;
- by table: for functions with no direct caller, where the retail image stores their
  address (a vtable / callback table), and the functions next to them in that table that
  did run (same object type).
Writes WORK/roadmap/scenarios.txt.
"""
import argparse, collections, csv, os, struct
import common
from roadmap import traces


def xbe_sections(path):
    x = open(path, 'rb').read()
    base = struct.unpack_from('<I', x, 0x104)[0]
    n, hdr = struct.unpack_from('<II', x, 0x11c)
    secs = []
    for i in range(n):
        o = hdr - base + i * 56
        flags, va, vsz, raw, rsz = struct.unpack_from('<IIIII', x, o)
        secs.append((va, vsz, raw, rsz, flags))
    return x, secs


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--decomp', default=common.DECOMP); a = ap.parse_args()
    rows = {int(r['va'], 16): r for r in csv.DictReader(open(os.path.join(a.decomp, 'config', 'functions.csv')))}
    callers = collections.defaultdict(set)
    for v, r in rows.items():
        for c in r['calls'].split():
            callers[int(c, 16)].add(v)
    reach = [int(r['va'], 16) for r in csv.DictReader(open(os.path.join(common.WORK, 'roadmap', 'roadmap.csv'))) if r['category'] == 'reach']
    rs = set(reach)
    counts, where = traces()
    ran = lambda v: counts.get(v, 0) > 0
    label = lambda v: '%06x %s [%s]' % (v, rows.get(v, {}).get('name', '') or '', os.path.basename(rows.get(v, {}).get('source', '') or '-'))
    gate = collections.defaultdict(list); nocaller = []
    for v in reach:
        if not callers.get(v):
            nocaller.append(v); continue
        seen, frontier, found = {v}, [v], set()
        for _ in range(6):
            nxt = []
            for f in frontier:
                for p in callers.get(f, ()):
                    if p in seen: continue
                    seen.add(p)
                    (found.add(p) if ran(p) else nxt.append(p))
            if found: break
            frontier = nxt
        gate[tuple(sorted(found)) or ('no ran ancestor',)].append(v)
    # where retail stores the addresses of the no-caller ones: a table of function addresses
    # (vtable / callback table), or code that stores or pushes the address (a registration)
    x, secs = xbe_sections(common.XBE)
    import bisect
    starts = sorted(rows); 
    def owner(va):
        i = bisect.bisect_right(starts, va) - 1
        if i >= 0 and va < starts[i] + int(rows[starts[i]]['size']): return starts[i]
    def f2va(off):
        for va, vsz, raw, rsz, fl in secs:
            if raw <= off < raw + rsz: return va + off - raw, raw, va, rsz
    tables = collections.defaultdict(list)
    for v in nocaller:
        key = struct.pack('<I', v); i = x.find(key); placed = False
        while i >= 0 and not placed:
            r = f2va(i)
            if r:
                rva, raw, sva, rsz = r
                fn = owner(rva)
                if fn is not None:
                    tables['registered by code in %s' % label(fn)].append((v, [fn])); placed = True
                elif (i - raw) % 4 == 0:
                    lo = i
                    while lo - 4 >= raw and struct.unpack_from('<I', x, lo - 4)[0] in rows: lo -= 4
                    hi = i
                    while hi + 8 <= raw + rsz and struct.unpack_from('<I', x, hi + 4)[0] in rows: hi += 4
                    ents = [struct.unpack_from('<I', x, k)[0] for k in range(lo, hi + 4, 4)]
                    tables['table %08x (%d entries, %d ran)' % (sva + lo - raw, len(ents), sum(ran(e) for e in ents))].append((v, ents)); placed = True
            i = x.find(key, i + 1)
        if not placed:
            tables['(address not found in the image)'].append((v, None))
    out = []
    out.append('NEVER-RUN GAME FUNCTIONS: %d (%d with callers, %d with no direct caller)' % (len(reach), len(reach) - len(nocaller), len(nocaller)))
    out.append('')
    out.append('== GATEWAYS (functions that ran but never called into these; one untaken branch each), biggest first')
    for g, vs in sorted(gate.items(), key=lambda t: -sum(int(rows[v]['size']) for v in t[1])):
        sz = sum(int(rows[v]['size']) for v in vs)
        out.append('%5d B %3d fns  via %s' % (sz, len(vs), ' | '.join(label(p) if isinstance(p, int) else p for p in g[:3])))
        if isinstance(g[0], int):
            out.append('          gateway ran in: %s' % ' '.join(sorted(where.get(g[0], ())))[:150])
        out.append('          ' + ' '.join('%06x' % v for v in vs[:12]) + (' ...' if len(vs) > 12 else ''))
    out.append('')
    out.append('== NO DIRECT CALLER: where retail stores the address')
    for t, vs in sorted(tables.items(), key=lambda t: -len(t[1])):
        out.append('%3d fns  %s' % (len(vs), t))
        ents = vs[0][1]
        if ents:
            ranents = [e for e in ents if ran(e)][:3]
            out.append('          entries that ran: %s' % (' | '.join(label(e) for e in ranents) or 'none: the whole object type never ran'))
        out.append('          ' + ' '.join('%06x' % v for v, _ in vs[:12]))
    path = os.path.join(common.WORK, 'roadmap', 'scenarios.txt')
    open(path, 'w').write('\n'.join(out) + '\n')
    print('\n'.join(out[:60])); print('... full list:', path)


if __name__ == '__main__':
    main()
