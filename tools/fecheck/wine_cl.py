#!/usr/bin/env python3
"""cl.exe under Wine for fecheck on Linux: common.CL points here when sys.platform is not win32.

Runs the same VS2022 x86 cl.exe that fecheck uses on Windows (common.MSVC_VER), copied from a
VS2022 BuildTools install into $FECHECK_MSVC (see common.py). Unix absolute paths in the
arguments (`/I <dir>`, `/Fo<obj>`, `/Fe<exe>`, sources) become Z:\\ paths; cl would read a bare
`/home/...` as an option. INCLUDE/LIB/WINEPREFIX come from common.vcenv().
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common   # noqa: E402

# cl options whose argument is glued to them (/Fo<path>)
GLUED = ('/Fo', '/Fe', '/Fd', '/Fa', '/Fp', '/FI', '/Fi', '/Tp', '/Tc')


def is_unix_path(a):
    # a cl option never holds a second '/', and none is named like a root directory
    return a.startswith('/') and '/' in a[1:] and os.path.isdir('/' + a[1:].split('/', 1)[0])


def winarg(a):
    if is_unix_path(a):
        return common.winpath(a)
    for g in GLUED:
        if a.startswith(g) and is_unix_path(a[len(g):]):
            return g + common.winpath(a[len(g):])
    return a


def main():
    cmd = ['wine', common.CL_EXE] + [winarg(a) for a in sys.argv[1:]]
    r = subprocess.run(cmd, stdout=subprocess.PIPE)
    sys.stdout.buffer.write(r.stdout.replace(b'\r\n', b'\n'))
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
