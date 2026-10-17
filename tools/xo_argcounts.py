"""Argument bytes of each Open Live entry point, read from the lifted C.

For a thunk (`ecx = MEM32(0x48480C); tail jmp impl`) the implementation's own
`ret N` is the API's stdcall argument size; a wrapper that pushes its
arguments again has the `ret N` itself. Prints `va name N` (N = bytes, or a
set of values when the implementation has several returns) for every function
the xo_hle_pass hooked.

usage: python tools/xo_argcounts.py [gen dir]
"""
import os
import re
import sys

GEN = sys.argv[1] if len(sys.argv) > 1 else "D:/n64work/openhalo2/port/xr/src/recomp/gen"
FN = re.compile(r"^void (sub_[0-9A-F]{8})(?:_lifted)?\(void\)\r?\n\{\r?\n(.*?)^\}\r?\n", re.M | re.S)
RET = re.compile(r"RECOMP_NOTE_RET\((\d+)u\)")
TAIL = re.compile(r"(sub_[0-9A-F]{8})\(\); return; /\* tail jmp")

bodies = {}
hooked = []
for n in sorted(os.listdir(GEN)):
    if not (n.startswith("recomp_") and n.endswith(".c")):
        continue
    s = open(os.path.join(GEN, n), encoding="utf-8").read()
    hooked += re.findall(r"xo_hook\(0x([0-9A-F]{8})u", s)
    for m in FN.finditer(s):
        if m.group(1) not in bodies or "_lifted" in m.group(0)[:30]:
            bodies[m.group(1)] = m.group(2)


def rets(name, depth=0):
    b = bodies.get(name, "")
    r = set(int(x) for x in RET.findall(b))
    if not r and depth < 3:
        t = TAIL.search(b)
        if t:
            return rets(t.group(1), depth + 1)
    return r


sys.path.insert(0, os.path.dirname(__file__))
for va in sorted(set(hooked)):
    r = rets("sub_" + va)
    print(f"{int(va, 16):06X} {','.join(str(x) for x in sorted(r)) or '?'}")
