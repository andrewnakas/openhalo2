"""Compile decomp sources with VS2022 x86 into COFF objects for fecheck blobs.

    python dbuild.py [--all | <src/x.cpp> ...] [--jobs 3] [--decomp <tree>] [--force]

Each source is compiled alone (no LTCG) with its own `// @flags` minus /GL,
plus: /Gy (one COMDAT per function, so the linker can take one function),
/Ob1 (only `inline` functions are inlined: a marked function stays a call, as
in retail), /arch:IA32 (x87 like vc71), /Zc:wchar_t- /Zc:forScope- (vc71
language), /Zc:threadSafeInit- (no TLS guard for local statics), /GS- /GR-.
<xtl.h> and friends come from tools/fecheck/xdkshim. Objects go to
<WORK>/obj/<tree-tag>/<stem>.obj; results in <WORK>/obj/<tree-tag>/build.json
({stem: {"ok": bool, "err": first errors}}). Prints a one-screen census.
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import common

FLAGS = re.compile(r'^\s*//\s*@flags\s+(.+?)\s*$', re.M)
BASE = ['/nologo', '/c', '/TP', '/Gy', '/GS-', '/arch:IA32', '/Zc:wchar_t-', '/Zc:forScope-',
        '/Zc:threadSafeInit-', '/GR-', '/W0', '/permissive', '/Zc:sizedDealloc-', '/D_XBOX', '/DXBOX', '/D_X86_']


def tree_tag(decomp):
    return hashlib.sha1(os.path.abspath(decomp).lower().encode()).hexdigest()[:8] + ('s2' if SSE2 else '')


def objdir(decomp):
    return os.path.join(common.WORK, 'obj', tree_tag(decomp))


SSE2 = os.environ.get('FECHECK_SSE2', '1') != '0'   # FECHECK_SSE2=0: plain x87 builds only


def src_flags(path, decomp):
    text = open(path, errors='replace').read()
    m = FLAGS.search('\n'.join(text.splitlines()[:30]))
    if m:
        fl = m.group(1).split()
    else:
        cfg = json.load(open(os.path.join(decomp, 'config', 'files.json')))
        rel = os.path.relpath(path, decomp).replace('\\', '/')
        v = cfg.get(rel) or cfg.get(os.path.basename(path))
        fl = (v if isinstance(v, str) else ' '.join(v) if v else '/O2 /Gr').split()
    fl = [f for f in fl if f not in ('/GL',)]
    if SSE2:
        # vc71 /arch:SSE does float math in scalar SSE (rounded per operation); VS2022's
        # /arch:SSE keeps it on x87 at extended precision. /arch:SSE2 is the VS2022 flag
        # that rounds float operations like retail.
        fl = ['/arch:SSE2' if f == '/arch:SSE' else f for f in fl]
    if '/Gr' not in fl:
        fl.append('/Gr')
    return fl


def compile_one(path, decomp, env, extra=(), out=None):
    stem = os.path.splitext(os.path.basename(path))[0]
    if os.path.basename(os.path.dirname(path)) == 'stubs':
        stem = 'stubs__' + stem
    od = objdir(decomp)
    os.makedirs(od, exist_ok=True)
    obj = out or os.path.join(od, stem + '.obj')
    fl = src_flags(path, decomp)
    if '/Ob0' not in fl:
        fl = [f for f in fl if not f.startswith('/Ob')] + ['/Ob1']
    cmd = [common.CL] + BASE + ['/I', common.SHIM, '/I', os.path.join(decomp, 'include'),
                                '/I', os.path.dirname(os.path.abspath(path))] + fl + list(extra) + ['/Fo' + obj, path]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, errors='replace', cwd=decomp)
    errs = [l.strip() for l in r.stdout.splitlines() if ': error' in l or 'fatal error' in l]
    return stem, r.returncode == 0 and os.path.exists(obj), errs[:6], obj


def newest_header(decomp):
    t = 0
    for p in glob.glob(os.path.join(decomp, 'include', '*.h')) + glob.glob(os.path.join(common.SHIM, '*')):
        t = max(t, os.path.getmtime(p))
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--decomp', default=common.DECOMP)
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    env = common.vcenv()
    files = a.files
    if a.all:
        files = sorted(glob.glob(os.path.join(a.decomp, 'src', '*.cpp')) + glob.glob(os.path.join(a.decomp, 'src', 'stubs', '*.cpp')))
    od = objdir(a.decomp)
    os.makedirs(od, exist_ok=True)
    state_p = os.path.join(od, 'build.json')
    state = json.load(open(state_p)) if os.path.exists(state_p) else {}
    hdr = newest_header(a.decomp)
    todo = []
    for f in files:
        stem = os.path.splitext(os.path.basename(f))[0]
        if os.path.basename(os.path.dirname(f)) == 'stubs':
            stem = 'stubs__' + stem
        obj = os.path.join(od, stem + '.obj')
        st = state.get(stem)
        if (not a.force and st and st.get('ok') and os.path.exists(obj)
                and os.path.getmtime(obj) > max(os.path.getmtime(f), hdr)):
            continue
        if not a.force and st and not st.get('ok') and st.get('mtime', 0) >= max(os.path.getmtime(f), hdr):
            continue
        todo.append(f)
    with ThreadPoolExecutor(a.jobs) as ex:
        for stem, ok, errs, obj in ex.map(lambda f: compile_one(f, a.decomp, env), todo):
            state[stem] = {'ok': ok, 'err': errs, 'mtime': os.path.getmtime(obj) if ok else max(hdr, 0)}
    json.dump(state, open(state_p, 'w'), indent=0)
    sel = [s for s in state if not files or any(os.path.splitext(os.path.basename(f))[0] in (s, s.replace('stubs__', '')) for f in files)] if not a.all else list(state)
    ok = sum(1 for s in sel if state[s]['ok'])
    print('dbuild: %d compiled now, %d/%d sources build' % (len(todo), ok, len(sel)))
    c = collections.Counter()
    for s in sel:
        for e in state[s]['err']:
            c[re.sub(r'^.*?: (fatal )?error', 'error', e)[:100]] += 1
    for k, v in c.most_common(25):
        print('%5d %s' % (v, k))


if __name__ == '__main__':
    main()
