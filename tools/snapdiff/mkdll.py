"""Build lifted.dll from the real generated bodies of a closure of functions.
usage: python mkdll.py <outdir> VA [VA...]   (closure from functions.csv; extra VAs added)"""
import os, re, sys, glob, csv
GEN = os.environ.get('SNAPDIFF_GEN', 'D:/n64work/openhalo2/port/xr/src/recomp/gen')
out = sys.argv[1]; seeds = [int(x, 16) for x in sys.argv[2:]]
rows = [r for r in csv.reader(open('D:/n64work/halo2/third_party/halo2-decompiled/config/functions.csv')) if len(r) > 7 and r[0] != 'va']
calls = {int(r[0],16): [int(c,16) for c in r[7].split()] for r in rows}
want = set(); st = list(seeds)
while st:
    x = st.pop()
    if x in want: continue
    want.add(x); st += calls.get(x, [])
bodies = {}
for fn in sorted(glob.glob(GEN + '/recomp_*.c')):
    src = open(fn, errors='replace').read()
    for m in re.finditer(r'^(?:static )?void (sub_([0-9A-F]{8}))(__body)?\(void\)\n\{', src, re.M):
        va = int(m.group(2), 16)
        if va not in want: continue
        end = src.index('\n}\n', m.end()) + 3
        body = src[m.start():end]
        if m.group(3): body = body.replace('static void sub_%08X__body(void)' % va, 'void sub_%08X(void)' % va, 1)
        bodies[va] = body
missing = sorted(want - set(bodies))
print('closure %d, found %d, missing %s' % (len(want), len(bodies), ' '.join('%08X' % v for v in missing)))
refd = set()
for b in bodies.values(): refd |= {int(h, 16) for h in re.findall(r'\bsub_([0-9A-F]{8})\b', b)}
os.makedirs(out, exist_ok=True)
L = ['#define RECOMP_GENERATED_CODE', '#include <windows.h>', '#include "recomp_funcs.h"', '#include "recomp_mmio.h"', '#include <math.h>',
     '#include "dllrt.h"']
for va in sorted(refd - set(bodies)):
    L.append('void sub_%08X(void) { RaiseException(0xE0000001u, 0, 0, 0); }' % va)
L += [bodies[v] for v in sorted(bodies)]
L.append('recomp_func_t dll_lookup(uint32_t va) { switch (va) {')
L += ['    case 0x%08Xu: return sub_%08X;' % (v, v) for v in sorted(bodies)]
L.append('    default: return 0; } }')
open(os.path.join(out, 'lifted_dll.c'), 'w').write('\n'.join(L) + '\n')
print('wrote', os.path.join(out, 'lifted_dll.c'), 'stubs', len(refd - set(bodies)))
