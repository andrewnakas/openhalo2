"""link_measure.py <reports root> <snaps root> <reach list> [out dir]

Measures a rigs_link.sh run: snapshots taken per scenario and copy, and how many functions of the
reach list (one hex VA per line: functions no earlier rig ever ran) now have a call count > 0 in
any of the run's fe.bin traces, and how many now have snapshots in the run's stores.
Writes <out dir>/reach_executed.txt and reach_snapshotted.txt (VA, calls, snapshots).
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import facts  # noqa: E402

rep, snaps, lst = sys.argv[1:4]
out = sys.argv[4] if len(sys.argv) > 4 else rep
reach = [int(x.strip(), 16) for x in open(lst) if x.strip()]
calls = {}
for fb in sorted(glob.glob(os.path.join(rep, '*', '*', 'fe.bin'))):
    try:
        t = facts.read_table(fb)
    except Exception as e:  # a copy killed while writing
        print('skip', fb, e)
        continue
    for va, r in t.items():
        if r['count']:
            calls[va] = calls.get(va, 0) + r['count']
snapn = {}
print('%-24s %6s %6s' % ('scenario', 'A', 'B'))
for sc in sorted({os.path.basename(p) for p in glob.glob(os.path.join(snaps, '*', '*'))}):
    row = []
    for c in 'AB':
        d = os.path.join(snaps, c, sc, 's')
        fs = glob.glob(os.path.join(d, '*.fes'))
        row.append(len(fs))
        for f in fs:
            v = int(os.path.basename(f).split('_')[0], 16)
            snapn[v] = snapn.get(v, 0) + 1
    print('%-24s %6d %6d' % (sc, row[0], row[1]))
ex = [v for v in reach if calls.get(v)]
sn = [v for v in reach if snapn.get(v)]
with open(os.path.join(out, 'reach_executed.txt'), 'w') as f:
    f.writelines('%08x %d %d\n' % (v, calls[v], snapn.get(v, 0)) for v in ex)
with open(os.path.join(out, 'reach_snapshotted.txt'), 'w') as f:
    f.writelines('%08x %d %d\n' % (v, calls.get(v, 0), snapn[v]) for v in sn)
print('reach list %d: executed %d, snapshotted %d (functions with snapshots in this run: %d)'
      % (len(reach), len(ex), len(sn), len(snapn)))
