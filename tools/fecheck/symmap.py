"""M0: decomp symbol -> retail VA map.

    python symmap.py [--decomp <tree>] [--no-infer]

Sources of truth, in order:
  1. `// @retail 0x<va>` and `// @stub 0x<va>` markers (functions, methods,
     constructors, destructors; parsed with the decomp's own tools/build.py scan)
  2. names in config/functions.csv (decorated library names such as _XGetLanguage@0)
  3. the names themselves: function_<va>, unknown_<va>, g_<addr>
  4. storage globals (upstream's include/*_storage.h): `extern T g_x_storage;` with rows
     `#define g_<addr> (g_x_storage.<member path>)`; base = addr - (address of the member - address
     of g_x_storage), measured by a probe program built with fecheck's own VS2022 toolchain, every
     row of one global agreeing (storages() below; cached per include/ state)
  5. named globals with no address anywhere (async_globals, ...): inferred from
     matched functions: each function's data relocations (symbol + addend in our
     VS2022 object) against the absolute data addresses its retail bytes use;
     the address most functions agree on wins (needs dbuild.py --all first).
Writes runtime/decomp_symbols.csv (name, kind, va, how, source) and prints a summary.
"""
import collections
import glob
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

import common
import coff
import dbuild

HEXNAME = re.compile(r'^(?:.*::)?(function|unknown|g)_([0-9a-f]{5,8})$')


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


# storage globals: `#define g_<addr>[_suffix] (<expr naming the global>)` and `extern <type> <global>;`
SDEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(g_([0-9a-f]{5,8})(?:_\w+)?)[ \t]+(\(.*\))[ \t]*$', re.M)
SEXTERN = re.compile(r'^[ \t]*extern[ \t]+((?:struct[ \t]+|union[ \t]+)?[A-Za-z_][\w:]*)[ \t]+([A-Za-z_]\w*)[ \t]*;', re.M)


def _probe(decomp, env, hdr, rows, types, tmp):
    """{define: (global, offset)} for the rows of one header: a probe program defines each global and
    prints &(<define>) - &<global> (and sizeof) for every row; rows that do not compile are dropped."""
    names = sorted({g for _, g in rows})
    while rows:
        lines = ['#include <stddef.h>', '#include <stdio.h>', '#include "%s"' % hdr]
        for g in names:
            lines.append('%s %s;' % (types[g], g))
        lines += ['int main(void)', '{']
        first = len(lines) + 1   # line number of the first row
        for d, g in rows:
            lines.append('    printf("%%s %%s %%ld %%lu\\n", "%s", "%s", (long)((char *)&(%s) - (char *)&%s), (unsigned long)sizeof(%s));'
                         % (d, g, d, g, g))
        lines += ['    return 0;', '}', '']
        src = os.path.join(tmp, 'probe.cpp')
        exe = os.path.join(tmp, 'probe.exe')
        open(src, 'w').write('\n'.join(lines))
        if os.path.exists(exe):
            os.remove(exe)
        import dbuild
        cmd = [common.CL] + [f for f in dbuild.BASE if f != '/c'] + ['/I', common.SHIM, '/I', os.path.join(decomp, 'include'),
                                                                   '/Od', '/Fo' + tmp + os.sep, '/Fe' + exe, src]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, errors='replace', cwd=tmp)
        if r.returncode == 0 and os.path.exists(exe):
            out = subprocess.run([exe], capture_output=True, text=True, errors='replace').stdout
            res = {}
            for line in out.splitlines():
                d, g, off, size = line.split()
                off, size = int(off), int(size)
                if 0 <= off < size:   # a cast row whose value is not an address inside the global is not one
                    res[d] = (g, off)
            return res
        bad = set()
        for m in re.finditer(r'probe\.cpp\((\d+)\)', r.stdout):
            k = int(m.group(1)) - first
            if 0 <= k < len(rows):
                bad.add(k)
        if not bad:
            print('symmap: storage probe of %s does not build: %s' % (hdr, r.stdout.strip().splitlines()[-1:]), file=sys.stderr)
            return {}
        rows = [r_ for k, r_ in enumerate(rows) if k not in bad]
    return {}


def storages(decomp, env=None):
    """{global: va} for globals that upstream gathered into storage structures: each `#define g_<addr>
    (<global>.<member>)` row gives <global> = addr - offset of the member, the offset measured by the
    compiler. All rows of a global must agree. Cached per include/ contents in WORK/storage_bases.json."""
    inc = os.path.join(decomp, 'include')
    hdrs = sorted(glob.glob(os.path.join(inc, '*.h')))
    texts = {}
    types = {}
    h = hashlib.sha1()
    for p in hdrs:
        t = open(p, encoding='utf-8', errors='replace').read()
        h.update(os.path.basename(p).encode() + b'\0' + t.encode('utf-8', 'replace'))
        if '#' in t and 'define' in t:
            texts[p] = t
        for m in SEXTERN.finditer(t):
            types.setdefault(m.group(2), m.group(1))
    key = h.hexdigest()
    cache = os.path.join(common.WORK, 'storage_bases.json')
    try:
        c = json.load(open(cache))
        if c.get('key') == key:
            return {k: int(v, 16) for k, v in c['bases'].items()}
    except (OSError, ValueError, KeyError):
        pass
    env = env or common.vcenv()
    tmp = os.path.join(common.WORK, 'storage_probe')
    os.makedirs(tmp, exist_ok=True)
    votes = collections.defaultdict(collections.Counter)
    for p, t in texts.items():
        rows, addr = [], {}
        for m in SDEFINE.finditer(t):
            d, a, body = m.group(1), int(m.group(2), 16), m.group(3)
            gs = [g for g in re.findall(r'[A-Za-z_]\w*', body) if g in types and not HEXNAME.match(g)]
            if len(gs) != 1:
                continue
            rows.append((d, gs[0]))
            addr[d] = a
        if not rows:
            continue
        for d, (g, off) in _probe(decomp, env, os.path.basename(p), rows, types, tmp).items():
            votes[g][addr[d] - off] += 1
    bases = {}
    for g, cnt in sorted(votes.items()):
        (va, n), = cnt.most_common(1)
        if len(cnt) > 1:
            print('symmap: storage %s rows disagree (%s): kept none' % (g, ', '.join('%08x x%d' % kv for kv in cnt.most_common(4))), file=sys.stderr)
            continue
        bases[g] = va
    json.dump({'key': key, 'bases': {k: '%08x' % v for k, v in bases.items()}}, open(cache, 'w'), indent=0)
    return bases


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


TREE_FORMAT = '|storage1'   # sidecar suffix: files made before storage rows existed are stale


def _tree(decomp):
    return os.path.abspath(decomp).lower() + TREE_FORMAT


def stale(decomp):
    """True when runtime/decomp_symbols.csv was made from another tree (its .tree sidecar) or by an
    older symmap, or is older than this tree's sources, headers or config."""
    p = os.path.join(common.RUNTIME, 'decomp_symbols.csv')
    if not os.path.exists(p):
        return True
    try:
        if open(p + '.tree').read().strip() != _tree(decomp):
            return True
    except OSError:
        pass   # made before the sidecar existed: judge by time alone
    t = os.path.getmtime(p)
    files = glob.glob(os.path.join(decomp, 'src', '*.cpp')) + glob.glob(os.path.join(decomp, 'src', 'stubs', '*.cpp'))
    files += glob.glob(os.path.join(decomp, 'include', '*.h'))
    files.append(os.path.join(decomp, 'config', 'functions.csv'))
    return any(os.path.getmtime(f) > t for f in files if os.path.exists(f))


def refresh(decomp, quiet=False):
    """Rebuild runtime/decomp_symbols.csv from the current tree's markers and functions.csv when it is
    stale (an upstream rename otherwise keeps the old name and loses the new one). Inferred data rows
    of the old file are kept for names the markers do not give (the vote needs every object built;
    `python symmap.py` does the full rebuild)."""
    if not stale(decomp):
        return False
    p = os.path.join(common.RUNTIME, 'decomp_symbols.csv')
    old = [r for r in common.read_csv(p) if r.get('how', '').startswith('infer')] if os.path.exists(p) else []
    rows, table = [], {}

    def add(name, kind, va, how, src):
        if name in table:
            return
        table[name] = va
        rows.append(dict(name=name, kind=kind, va='%08x' % va, how=how, source=src))
    for m in markers(decomp):
        if m.kind in ('vcall', 'adjustor'):
            continue
        add(m.name, 'stub' if m.stub else m.kind, m.retail, 'marker', os.path.basename(m.path))
    for va, r in common.functions(decomp).items():
        if r['name']:
            add(r['name'], 'function', va, 'csv', '')
            add(coff.plain_name(r['name']), 'function', va, 'csv', '')
    for name, va in sorted(storages(decomp).items()):
        add(name, 'data', va, 'storage', '')
    if not old and os.path.exists(os.path.join(dbuild.objdir(decomp), 'build.json')):
        # a fresh runtime/ (a new worktree) has no inferred rows to keep: vote now over the objects
        # already built, else async_globals and friends leave every user unbuildable
        for name, (va, n) in sorted(infer(decomp, table, data_sections()).items()):
            for nm in (name, coff.plain_name(name)):
                add(nm, 'data', va, 'infer:%d' % n, '')
    for r in old:
        add(r['name'], r['kind'], int(r['va'], 16), r['how'], r['source'])
    common.write_csv(p, rows, ['name', 'kind', 'va', 'how', 'source'])
    open(p + '.tree', 'w').write(_tree(decomp))
    if not quiet:
        print('symmap: refreshed %d names from %s' % (len(rows), decomp), file=sys.stderr)
    return True


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
    for name, va in sorted(storages(a.decomp).items()):
        add(name, 'data', va, 'storage', '')
    n0 = len(rows)
    if not a.no_infer:
        for name, (va, n) in sorted(infer(a.decomp, table, data_sections()).items()):
            add(name, 'data', va, 'infer:%d' % n, '')
            pn = coff.plain_name(name)
            if pn != name:
                add(pn, 'data', va, 'infer:%d' % n, '')
    common.write_csv(os.path.join(common.RUNTIME, 'decomp_symbols.csv'), rows, ['name', 'kind', 'va', 'how', 'source'])
    open(os.path.join(common.RUNTIME, 'decomp_symbols.csv.tree'), 'w').write(_tree(a.decomp))
    c = collections.Counter(r['how'].split(':')[0] for r in rows)
    print('symmap: %d names (%s), %d inferred' % (len(rows), ', '.join('%s %d' % kv for kv in c.items()), len(rows) - n0))


if __name__ == '__main__':
    main()
