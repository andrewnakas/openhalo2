"""M0: decomp symbol -> retail VA map.

    python symmap.py [--decomp <tree>] [--no-infer]

Sources of truth, in order:
  1. `// @retail 0x<va>` and `// @stub 0x<va>` markers (functions, methods,
     constructors, destructors; parsed with the decomp's own tools/build.py scan)
  2. names in config/functions.csv (decorated library names such as _XGetLanguage@0)
  3. the names themselves: function_<va>, unknown_<va>, g_<addr>
  4. `#define g_<addr> (<symbol>.field_<off>)` lines in include/ (symbol = addr - off)
  5. named globals with no address anywhere (async_globals, ...): inferred from
     matched functions: each function's data relocations (symbol + addend in our
     VS2022 object) against the absolute data addresses its retail bytes use;
     the address most functions agree on wins (needs dbuild.py --all first).
Writes runtime/decomp_symbols.csv (name, kind, va, how, source) and prints a summary.
"""
import collections
import glob
import os
import re
import struct
import sys

import common
import coff
import dbuild

HEXNAME = re.compile(r'^(?:.*::)?(function|unknown|g)_([0-9a-f]{5,8})$')
DEFINE = re.compile(r'^\s*#\s*define\s+g_([0-9a-f]{5,8})\s+\(\s*([A-Za-z_]\w*)\.field_([0-9a-f]+)\s*\)', re.M)


def markers(decomp):
    common.decomp_tools(decomp)
    import build as dbuild_decomp   # the decomp's tools/build.py
    out = []
    files = sorted(glob.glob(os.path.join(decomp, 'src', '*.cpp')) + glob.glob(os.path.join(decomp, 'src', 'stubs', '*.cpp')))
    for f in files:
        text = open(f, encoding='utf-8', errors='replace').read()
        try:
            for m in dbuild_decomp.scan(text, f):
                out.append(m)
        except SystemExit as e:
            print('  scan:', e)
    return out


def data_sections():
    return [(v, v + vs) for nm, v, vs, raw, ex in common.xbe_sections() if not common.is_code(nm)]


def load(path=None):
    """{name: va} from runtime/decomp_symbols.csv (plain and raw names)."""
    p = path or os.path.join(common.RUNTIME, 'decomp_symbols.csv')
    out = {r['name']: int(r['va'], 16) for r in common.read_csv(p)}
    # hand-proven additions (name,va,evidence), e.g. by an overnight lane: they win
    extra = os.path.join(common.WORK, 'extra_symbols.csv')
    for r in common.read_csv(extra):
        try:
            out[r['name'].strip()] = int(r['va'], 16)
        except (KeyError, ValueError):
            pass
    return out


def resolve(raw, table):
    """Retail VA for a COFF symbol name, or None."""
    if raw in table:
        return table[raw]
    pn = coff.plain_name(raw)
    if pn in table:
        return table[pn]
    m = HEXNAME.match(pn)
    if m:
        return int(m.group(2), 16)
    return None


def infer(decomp, table, data):
    """Named globals without an address: vote over matched functions."""
    fn = common.functions(decomp)
    matched = {r['source']: [] for r in fn.values() if r['status'] == 'matched' and r['source']}
    for r in fn.values():
        if r['status'] == 'matched' and r['source']:
            matched[r['source']].append(r)
    od = dbuild.objdir(decomp)
    votes = collections.defaultdict(collections.Counter)
    inv = {}
    for va_name, va in table.items():
        inv.setdefault(va, va_name)
    for src, rows in matched.items():
        stem = os.path.splitext(os.path.basename(src))[0]
        if os.path.basename(os.path.dirname(src)) == 'stubs':
            stem = 'stubs__' + stem
        objp = os.path.join(od, stem + '.obj')
        if not os.path.exists(objp):
            continue
        try:
            o = coff.Coff(objp)
        except Exception:
            continue
        defs = o.defined()
        byva = {r['va_i']: r for r in rows}
        for sym in o.symbols:
            if sym.section <= 0 or sym.sclass != 2:
                continue
            sec = o.sections[sym.section]
            if not sec.flags & coff.SCN_CODE:
                continue
            va = resolve(sym.name, table)
            if va not in byva:
                continue
            body = common.xbe_read(va, byva[va]['size_i']) or b''
            addrs = set()
            for k in range(len(body) - 3):
                x = struct.unpack_from('<I', body, k)[0]
                if any(lo <= x < hi for lo, hi in data):
                    addrs.add(x)
            seen = set()
            for off, si, typ in sec.relocs:
                if typ != coff.IMAGE_REL_I386_DIR32:
                    continue
                t = o.by_index.get(si)
                if t is None or t.name in seen:
                    continue
                if resolve(t.name, table) is not None or (t.section > 0 and t.sclass == 3):
                    continue
                if t.section > 0 and not (o.sections[t.section].flags & coff.SCN_WRITE):
                    continue   # literals and constants (??_C@, __real@): each object has its own copy
                seen.add(t.name)
                add = struct.unpack_from('<I', sec.data, off)[0]
                for x in addrs:
                    votes[t.name][(x - add) & 0xFFFFFFFF] += 1
    out = {}
    for name, c in votes.items():
        (best, n), = c.most_common(1)
        second = c.most_common(2)[1][1] if len(c) > 1 else 0
        if n >= 2 and n > second:
            out[name] = (best, n)
    return out


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--decomp', default=common.DECOMP)
    ap.add_argument('--no-infer', action='store_true')
    a = ap.parse_args()
    rows, table = [], {}

    def add(name, kind, va, how, src):
        if name in table:
            return
        table[name] = va
        rows.append(dict(name=name, kind=kind, va='%08x' % va, how=how, source=src))
    for m in markers(a.decomp):
        if m.kind in ('vcall', 'adjustor'):
            continue
        add(m.name, 'stub' if m.stub else m.kind, m.retail, 'marker', os.path.basename(m.path))
    for va, r in common.functions(a.decomp).items():
        if r['name']:
            add(r['name'], 'function', va, 'csv', '')
            add(coff.plain_name(r['name']), 'function', va, 'csv', '')
    # (no rule from `#define g_<addr> (<sym>.field_<x>)`: field_<x> names an address suffix, not an offset;
    # such structures get their address from the inference below)
    n0 = len(rows)
    if not a.no_infer:
        for name, (va, n) in sorted(infer(a.decomp, table, data_sections()).items()):
            add(name, 'data', va, 'infer:%d' % n, '')
            pn = coff.plain_name(name)
            if pn != name:
                add(pn, 'data', va, 'infer:%d' % n, '')
    common.write_csv(os.path.join(common.RUNTIME, 'decomp_symbols.csv'), rows, ['name', 'kind', 'va', 'how', 'source'])
    c = collections.Counter(r['how'].split(':')[0] for r in rows)
    print('symmap: %d names (%s), %d inferred' % (len(rows), ', '.join('%s %d' % kv for kv in c.items()), len(rows) - n0))


if __name__ == '__main__':
    main()
