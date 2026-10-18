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
    return [(v, v + vs) for nm,