"""Shared paths and helpers for the decomp-assist bench (tools/fecheck)."""
import csv
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OPENHALO2 = os.path.normpath(os.path.join(HERE, '..', '..'))
# openhalo2, halo2-decompiled and halo2 sit side by side under one root
ROOT = os.environ.get('FECHECK_ROOT', os.path.dirname(OPENHALO2)).replace('\\', '/').rstrip('/')
HALO2 = ROOT + '/halo2'
DECOMP = os.environ.get('FECHECK_DECOMP', ROOT + '/halo2-decompiled')
WORK = os.environ.get('FECHECK_WORK', HALO2 + '/build/fecheck')
SNAPS = os.environ.get('FECHECK_SNAPS', HALO2 + '/snaps')
RUNTIME = os.path.join(OPENHALO2, 'runtime')
XBE = HALO2 + '/dirty/game/default.xbe'
GEN = os.path.join(OPENHALO2, 'port', 'xr', 'src', 'recomp', 'gen').replace('\\', '/')
SHIM = os.path.join(HERE, 'xdkshim')
VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
MSVC_VER, SDK_VER = '14.44.35207', '10.0.26100.0'
CL = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%s\bin\Hostx64\x86\cl.exe' % MSVC_VER
WINDOWS = sys.platform == 'win32'
if not WINDOWS:
    # Linux: the same cl.exe under Wine. FECHECK_MSVC holds a copy of a VS2022 BuildTools install:
    #   VC/Tools/MSVC/<MSVC_VER>/  and  Kits/10/{Include,Lib}/<SDK_VER>/
    # (from C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools and .../Windows Kits).
    MSVC = os.environ.get('FECHECK_MSVC', os.path.join(os.path.dirname(OPENHALO2), 'msvc'))
    CL_EXE = os.path.join(MSVC, 'VC', 'Tools', 'MSVC', MSVC_VER, 'bin', 'Hostx64', 'x86', 'cl.exe')
    CL = os.path.join(HERE, 'wine_cl.py')
    # coff.undecorate: Microsoft's 32-bit dbghelp.dll (C:/Windows/SysWOW64/dbghelp.dll)
    DBGHELP = os.environ.get('FECHECK_DBGHELP', os.path.join(MSVC, 'dbghelp.dll'))
GAME_LO, GAME_HI = 0x12000, 0x2CB4E2


def winpath(p):
    """A Unix path as Wine sees it (Z: is /)."""
    return 'Z:' + os.path.abspath(p).replace('/', '\\')


def run_exe():
    """Prefix that runs a built Windows program (wine off Windows)."""
    return [] if WINDOWS else ['wine']


def vcenv():
    """The environment of an amd64_x86 VS2022 developer prompt (cached)."""
    if not WINDOWS:
        vc = os.path.join(MSVC, 'VC', 'Tools', 'MSVC', MSVC_VER)
        kit = os.path.join(MSVC, 'Kits', '10')
        env = dict(os.environ)
        env.setdefault('WINEPREFIX', os.path.join(os.path.dirname(MSVC), 'wineprefix'))
        env.setdefault('WINEDEBUG', '-all')
        env.setdefault('WINEDLLOVERRIDES', 'mscoree,mshtml=')
        env['INCLUDE'] = ';'.join(winpath(p) for p in (
            os.path.join(vc, 'include'),
            *(os.path.join(kit, 'Include', SDK_VER, d) for d in ('ucrt', 'um', 'shared', 'winrt', 'cppwinrt'))))
        env['LIB'] = ';'.join(winpath(p) for p in (
            os.path.join(vc, 'lib', 'x86'),
            *(os.path.join(kit, 'Lib', SDK_VER, d, 'x86') for d in ('ucrt', 'um'))))
        return env
    os.makedirs(WORK, exist_ok=True)
    cache = os.path.join(WORK, 'vcenv.json')
    if os.path.exists(cache):
        return json.load(open(cache))
    bat = os.path.join(WORK, 'vce.cmd')
    open(bat, 'w').write('@call "%s" amd64_x86 >nul\n@set\n' % VCVARS)
    out = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True, errors='replace').stdout
    env = {}
    for line in out.splitlines():
        k, s, v = line.partition('=')
        if s:
            env[k] = v
    json.dump(env, open(cache, 'w'))
    return env


def functions(decomp=None):
    """config/functions.csv rows keyed by VA (int)."""
    p = os.path.join(decomp or DECOMP, 'config', 'functions.csv')
    out = {}
    for r in csv.DictReader(open(p, newline='')):
        r['va_i'] = int(r['va'], 16)
        r['size_i'] = int(r['size'])
        out[r['va_i']] = r
    return out


def decomp_tools(decomp=None):
    p = os.path.join(decomp or DECOMP, 'tools')
    if p not in sys.path:
        sys.path.insert(0, p)


_SECTIONS = None


def xbe_sections():
    """[(name, va, vsize, raw bytes, executable)] of the retail XBE."""
    global _SECTIONS
    if _SECTIONS is None:
        f = open(XBE, 'rb').read()
        base = struct.unpack_from('<I', f, 0x104)[0]
        n = struct.unpack_from('<I', f, 0x11C)[0]
        sh = struct.unpack_from('<I', f, 0x120)[0] - base
        out = []
        for i in range(n):
            fl, v, vs, ra, rs, na = struct.unpack_from('<6I', f, sh + i * 56)
            o = na - base
            nm = f[o:f.index(b'\0', o)].decode()
            out.append((nm, v, vs, f[ra:ra + rs], bool(fl & 4)))
        _SECTIONS = out
    return _SECTIONS


def xbe_read(va, n):
    for nm, v, vs, raw, ex in xbe_sections():
        if v <= va < v + vs:
            o = va - v
            b = raw[o:o + n]
            return b + bytes(max(0, min(n, vs - o) - len(b)))
    return None


CODE_NAMES = ('.text', 'DSOUND', 'WMADEC', 'XONLINE', 'XNET', 'D3D', 'XPP')


def is_code(nm):
    return nm in CODE_NAMES or (nm.startswith('BINK') and nm != 'BINKDATA')


def read_csv(path):
    if not os.path.exists(path):
        return []
    return list(csv.DictReader(open(path, newline='', encoding='utf-8')))


def write_csv(path, rows, fields):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader()
        for r in rows:
            w.writerow(r)
    os.replace(tmp, path)
