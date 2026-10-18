"""Overnight work queue: todo/near game functions with snapshots, by fecheck verdict, split into lanes.

    python lanes.py [--tag _t] [--lanes 6] [--out <WORK>/queue]

Reads the fecheck results (<WORK>/results/<va><tag>.json) of the first pass over every
todo/near function. Functions already `equivalent` need no work. The rest are ordered:
near before todo, `differs` before `unbuildable`, then small before large. Lanes are cut by
source file (all functions of one .cpp go to one lane) so no two lanes edit one file.
Writes <out>/lane<N>.txt (va status verdict size source) and prints a census.
"""
import argparse
import collections
import json
import os

import common
import snapstore


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', default='_t')
    ap.add_argument('--lanes', type=int, default=6)
    ap.add_argument('--out', default=os.path.join(common.WORK, 'queue'))
    a = ap.parse_args()
    fn = common.functions()
    snaps = snapstore.counts()
    res_dir = os.path.join(common.WORK, 'results')
    rows = []
    c = collections.Counter()
    for va, r in fn.items():
        if r['owner'] != 'game' or r['status'] not in ('todo', 'near') or va not in snaps:
            continue
        p = os.path.join(res_dir, '%08x%s.json' % (va, a.tag))
        v = json.load(open(p)) if os.path.exists(p) else {'verdict': 'unchecked', 'detail': ''}
        c[(r['status'], v['verdict'])] += 1
        if v['verdict'].startswith('equivalent') or v['verdict'] == 'untestable':
            continue
        rank = (0 if r['status'] == 'near' else 1, {'differs': 0, 'unbuildable': 1}.get(v['verdict'], 2), r['size_i'])
        rows.append((rank, va, r, v))
    rows.sort(key=lambda x: x[0])
    by_src = collections.OrderedDict()
    for rank, va, r, v in rows:
        by_src.setdefault(r['source'] or ('none_%08x' % va), []).append((va, r, v))
    lanes = [[] for _ in range(a.lanes)]
    load = [0] * a.lanes
    for src, items in by_src.items():
        k = load.index(min(load))
        lanes[k] += items
        load[k] += sum(min(r['size_i'], 2000) for va, r, v in items)
    os.makedirs(a.out, exist_ok=True)
    for k, items in enumerate(lanes):
        with open(os.path.join(a.out, 'lane%d.txt' % k), 'w') as f:
            for va, r, v in items:
                f.write('%08x %s %s %d %s\n' % (va, r['status'], v['verdict'], r['size_i'], r['source'] or '-'))
    print('queue: %d functions to work in %d lanes (%s)' % (len(rows), a.lanes, ', '.join(str(len(l)) for l in lanes)))
    for k, n in sorted(c.items()):
        print('  %-5s %-14s %d' % (k[0], k[1], n))


if __name__ == '__main__':
    main()
