"""Read fecheck snapshots (fecheck_rt.c, RECOMP_FE_SNAP): <dir>/pages.dat + <dir>/s/<VA>_<hit>.fes.

    python snapstore.py <store dir> [VA]     # census, or the snapshots of one function

A snapshot = registers, x87/MMX/XMM and one page number per 4 KB page of guest RAM and then
of the 64 MB contiguous window (0xFFFFFFFF = zero page). Snapshots hold retail data: the store
lives outside every repository.
"""
import glob
import os
import struct
import sys

import numpy as np

import common

HDR = ['magic', 'va', 'size', 'csize', 'eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'seh_ebp', 'esi', 'edi',
       'eflags', 'df', 'fp_top', 'fpcw', 'fs_base', 'hit', 'ms', 'tid']
_PAGES = {}


def roots():
    """Every store directory (one per rig scenario) under common.SNAPS."""
    return sorted(os.path.dirname(p) for p in glob.glob(os.path.join(common.SNAPS, '*', 'pages.dat')) + glob.glob(os.path.join(common.SNAPS, '*', '*', 'pages.dat'))
                  + glob.glob(os.path.join(common.SNAPS, '*', '*', '*', 'pages.dat')))   # link rigs: <run>/<copy>/<scenario>


def index(va=None):
    """[(path, store dir)] of all snapshots (of one function when va is given)."""
    out = []
    for r in roots():
        pat = '%08X_*.fes' % va if va is not None else '*.fes'
        out += [(p, r) for p in sorted(glob.glob(os.path.join(r, 's', pat)))]
    return out


def counts():
    """{va: number of snapshots} over all stores."""
    c = {}
    for r in roots():
        for p in os.listdir(os.path.join(r, 's')):
            if p.endswith('.fes'):
                v = int(p.split('_')[0], 16)
                c[v] = c.get(v, 0) + 1
    return c


def _pages(root):
    p = os.path.join(root, 'pages.dat')
    n = os.path.getsize(p) // 4096
    m = _PAGES.get(root)
    if m is None or m.shape[0] != n:
        m = np.memmap(p, dtype=np.uint8, mode='r', shape=(n, 4096))
        _PAGES[root] = m
    return m


def load(path, root=None):
    """dict: header fields, fp (8 doubles), mm, xmm, ram (np uint8, writable), contig (np uint8)."""
    root = root or os.path.dirname(os.path.dirname(path))
    b = open(path, 'rb').read()
    h = struct.unpack_from('<32I', b, 0)
    assert h[0] == 0x31534546, 'not a FES1 snapshot: %s' % path
    s = dict(zip(HDR, h))
    o = 128
    s['fp'] = struct.unpack_from('<8d', b, o); o += 64
    s['mm'] = struct.unpack_from('<8Q', b, o); o += 64
    s['xmm'] = [b[o + 16 * i:o + 16 * i + 16] for i in range(8)]; o += 128
    sz, csz = s['size'], s['csize']
    np_ = (sz + csz) // 4096
    table = np.frombuffer(b, dtype=np.uint32, count=np_, offset=o)
    pages = _pages(root)
    zero = table == 0xFFFFFFFF
    idx = np.where(zero, 0, table).astype(np.int64)
    allp = np.array(pages[idx])
    allp[zero] = 0
    flat = allp.reshape(-1)
    s['ram'] = flat[:sz].copy()
    s['contig'] = flat[sz:sz + csz].copy()
    s['path'] = path
    return s


def main():
    if len(sys.argv) > 2:
        va = int(sys.argv[2], 16)
        for p, r in index(va):
            s = load(p, r)
            print('%s esp %08X ecx %08X edx %08X eax %08X ret %08X' % (os.path.basename(p), s['esp'], s['ecx'], s['edx'], s['eax'],
                  struct.unpack_from('<I', s['ram'], s['esp'])[0]))
        return
    c = counts()
    print('snapstore: %d stores, %d functions, %d snapshots' % (len(roots()), len(c), sum(c.values())))


if __name__ == '__main__':
    main()
