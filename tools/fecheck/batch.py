"""Run fecheck over many functions in parallel worker processes.

    python batch.py <listfile|status:matched|status:todo,near> [--jobs 3] [--k 4] [--tag T] [--only-snapped]

One line per function goes to <WORK>/results/<va><tag>.txt (fecheck.save); prints a census at the end.
"""
import argparse
import collections
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import common
import snapstore
import symmap

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what')
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--k', type=int, default=4)
    ap.add_argument('--tag', default='')
    ap.add_argument('--decomp', default=common.DECOMP)
    ap.add_argument('--chunk', type=int, default=20)
    ap.add_argument('--skip-done', action='store_true')
    a = ap.parse_args()
    symmap.refresh(a.decomp)   # once here, not in each worker
    fn = common.functions(a.decomp)
    snapped = snapstore.counts()
    if a.what.startswith('status:'):
        want = a.what[7:].split(',')
        vas = sorted(v for v, r in fn.items() if r['status'] in want and r['owner'] == 'game' and v in snapped)
    else:
        vas = [int(l.split()[0], 16) for l in open(a.what) if l.strip()]
    res_dir = os.path.join(common.WORK, 'results')
    if a.skip_done:
        vas = [v for v in vas if not os.path.exists(os.path.join(res_dir, '%08x%s.txt' % (v, a.tag)))]
    chunks = [vas[i:i + a.chunk] for i in range(0, len(vas), a.chunk)]
    print('batch: %d functions in %d chunks, %d jobs' % (len(vas), len(chunks), a.jobs), flush=True)

    def run(ch):
        cmd = [sys.executable, os.path.join(HERE, 'fecheck.py')] + ['%08x' % v for v in ch] + \
            ['--k', str(a.k), '--decomp', a.decomp] + (['--tag', a.tag] if a.tag else [])
        subprocess.run(cmd, capture_output=True, text=True, cwd=HERE, env=dict(os.environ, FECHECK_NO_REFRESH='1'))
    with ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(run, chunks))
    # a native crash (Unicorn) kills the rest of its chunk: run the missing ones alone, then record the crash
    miss = [v for v in vas if not os.path.exists(os.path.join(res_dir, '%08x%s.txt' % (v, a.tag)))]
    if miss:
        with ThreadPoolExecutor(a.jobs) as ex:
            list(ex.map(run, [[v] for v in miss]))
    for v in vas:
        p = os.path.join(res_dir, '%08x%s.txt' % (v, a.tag))
        if not os.path.exists(p):
            open(p, 'w').write('%08x crashed       -       native crash in the emulator (Unicorn)\n' % v)
    c = collections.Counter()
    for v in vas:
        p = os.path.join(res_dir, '%08x%s.txt' % (v, a.tag))
        c[open(p).read().split()[1] if os.path.exists(p) else 'crashed'] += 1
    print('batch done: ' + ', '.join('%s %d' % kv for kv in c.most_common()))


if __name__ == '__main__':
    main()
