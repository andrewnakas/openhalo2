"""Function-entry hooks for the decomp-assist bench (docs/DECOMP_ASSIST_PLAN.md, P1).

Puts `RECOMP_FN_ENTER(<idx>u, 0x<VA>u);` as the first statement of every
generated function in the game range (default 0x12000-0x2cb4e2), writes
`fe_table.c` (the VAs in index order) into the gen dir, and adds to its
recomp_types.h a guarded definition: empty unless RECOMP_FECHECK is defined,
then a call of recomp_fe_enter (port/xr/src/fecheck_rt.c). Run it on a
PRIVATE copy of the gen dir (CMake -DOPENHALO2_GEN_DIR=<copy>
-DOPENHALO2_FECHECK=ON); a build of an un-passed gen dir needs nothing.
Idempotent; prints one line.

usage: python fe_pass.py <gen dir> [--lo 0x12000] [--hi 0x2cb4e2]
"""
import os
import re
import sys

MACRO = '''
/* ---- fe_pass.py: function-entry hook (decomp-assist bench) ---- */
#ifndef RECOMP_FN_ENTER
#ifdef RECOMP_FECHECK
void recomp_fe_enter(uint32_t idx, uint32_t va);
#define RECOMP_FN_ENTER(idx, va) recomp_fe_enter((idx), (va))
#else
#define RECOMP_FN_ENTER(idx, va) ((void)0)
#endif
#endif
'''
HEAD = re.compile(r'^void sub_([0-9A-F]{8})\(void\)\n\{\n', re.M)


def main():
    args = sys.argv[1:]
    gen = args[0]
    lo = int(args[args.index('--lo') + 1], 16) if '--lo' in args else 0x12000
    hi = int(args[args.index('--hi') + 1], 16) if '--hi' in args else 0x2CB4E2
    files = sorted(f for f in os.listdir(gen) if re.match(r'recomp_\d{4}\.c$', f))
    vas = set()
    for fn in files:
        for m in HEAD.finditer(open(os.path.join(gen, fn), encoding='utf-8', errors='surrogateescape').read()):
            va = int(m.group(1), 16)
            if lo <= va < hi:
                vas.add(va)
    order = sorted(vas)
    idx = {v: i for i, v in enumerate(order)}
    changed = 0
    for fn in files:
        p = os.path.join(gen, fn)
        src = open(p, encoding='utf-8', errors='surrogateescape', newline='').read()
        nl = '\r\n' if '\r\n' in src[:4000] else '\n'
        s = src.replace('\r\n', '\n')

        def put(m):
            va = int(m.group(1), 16)
            if va not in idx:
                return m.group(0)
            tail = s[m.end():m.end() + 80]
            if tail.lstrip().startswith('RECOMP_FN_ENTER('):
                return m.group(0)
            return m.group(0) + '    RECOMP_FN_ENTER(%du, 0x%08Xu);\n' % (idx[va], va)
        out = HEAD.sub(put, s)
        if out != s:
            open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(out.replace('\n', nl))
            changed += 1
    t = os.path.join(gen, 'recomp_types.h')
    th = open(t, encoding='utf-8', errors='surrogateescape').read()
    if 'fe_pass.py: function-entry hook' not in th:
        k = th.rfind('#endif')   # inside the include guard
        th = th[:k] + MACRO + '\n' + th[k:]
        open(t, 'w', encoding='utf-8', errors='surrogateescape').write(th)
    with open(os.path.join(gen, 'fe_table.c'), 'w') as f:
        f.write('/* fe_pass.py: VA of each RECOMP_FN_ENTER index, ascending */\n#include <stdint.h>\n')
        f.write('const uint32_t g_fe_table_n = %du;\nconst uint32_t g_fe_table_va[%d] = {\n' % (len(order), max(1, len(order))))
        for i in range(0, len(order), 8):
            f.write('    ' + ', '.join('0x%08Xu' % v for v in order[i:i + 8]) + ',\n')
        f.write('};\n')
    print('fe_pass: %d functions in %06X-%06X hooked, %d files changed, fe_table.c written' % (len(order), lo, hi, changed))


if __name__ == '__main__':
    main()
