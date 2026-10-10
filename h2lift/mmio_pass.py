"""Checked device-register access for lifted hardware code.

The title's libraries (Direct3D, DirectSound, the USB stack) read and write
chip registers at 0xFD000000 and above. The 64-bit Windows build leaves those
pages unmapped and emulates each access from the page-fault context, which
needs an x64 instruction decoder and cannot exist at all in WebAssembly.

This pass rewrites the lifted C of every function inside the given address
ranges so that a memory access is an expression the runtime can route:

    eax = MEM32(X);          ->  eax = MEMR32(X);
    MEM32(X) = Y;            ->  MEMW32(X, Y);
    MEM32(X) |= Y;           ->  MEMW32(X, MEMR32(X) | (Y));
    MEM32(X)++;              ->  MEMW32(X, MEMR32(X) + 1);

MEMR*/MEMW* (recomp_mmio.h) test the address against RECOMP_MMIO_BASE and call
recomp_mmio_read / recomp_mmio_write above it; below it they are the plain
access. `&MEM32(X)` (an address taken) is left alone. Stack macros (PUSH32,
POP32) and the string helpers are not touched: neither is used on a register
page by these libraries; a miss shows up as a counted page fault on the
64-bit build (`[MMIO]` report line), which is how the ranges are validated.

The pass is idempotent (MEMR32 is not matched again) and prints one line.

usage: python mmio_pass.py <gen dir> [--ranges 0x379C60-0x387384,...] [--check]
"""
import os
import re
import sys

# DSOUND, XNET, D3D, XPP sections of the retail default.xbe (start-end of the section)
DEFAULT_RANGES = "0x379C60-0x387384,0x3CCFE0-0x3E0400,0x3F5240-0x4086A4,0x4086C0-0x41151C"

ACC = re.compile(r"\b(S?MEM(?:8|16|32|64)|MEMF|MEMD)\(")
FUNC = re.compile(r"^void sub_([0-9A-Fa-f]{8})\(void\)\s*$")
COMPOUND = ("<<=", ">>=", "+=", "-=", "|=", "&=", "^=", "*=", "/=", "%=")
PLAIN = re.compile(r"^(esp( [+-] (0x[0-9A-Fa-f]+|[0-9]+))?|0x[0-9A-Fa-f]+u?|[0-9]+)$")
INCLUDE = '#include "recomp_mmio.h"'


class PassError(Exception):
    pass


def close_paren(s, i):
    """s[i] is '('; index of its partner."""
    depth = 0
    for j in range(i, len(s)):
        c = s[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
    raise PassError("unbalanced: " + s[:120])


def rhs_end(s, i):
    """End of an assignment's right-hand side starting at i: the ';' or ','
    at depth 0, or the ')' that closes an enclosing group."""
    depth = 0
    for j in range(i, len(s)):
        c = s[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return j
            depth -= 1
        elif c in ";," and depth == 0:
            return j
    return len(s)


def is_mmio_const(arg):
    a = arg.strip().rstrip("u")
    return not a.startswith("esp") and int(a, 0) >= 0xFD000000


def reader(name):
    return name.replace("MEM", "MEMR", 1)


def writer(name):
    return name.lstrip("S").replace("MEM", "MEMW", 1)


def xform(s):
    out = []
    i = 0
    while True:
        m = ACC.search(s, i)
        if not m:
            out.append(s[i:])
            break
        head = s[i:m.start()]
        name = m.group(1)
        j = close_paren(s, m.end() - 1)
        arg = xform(s[m.end():j])
        if PLAIN.match(arg.strip()) and not is_mmio_const(arg):
            out.append(head + name + "(" + arg + ")")   # the stack, or a fixed address in RAM
            i = j + 1
            continue
        k = j + 1
        while k < len(s) and s[k] == " ":
            k += 1
        rest = s[k:]
        before = head.rstrip()
        if before.endswith("&") and not before.endswith("&&"):
            # address taken (or a bitwise AND: a read on its right-hand side)
            prev = before[:-1].rstrip()
            if not prev or prev[-1] in "(,=;{":
                out.append(head + name + "(" + arg + ")")
                i = j + 1
                continue
        if before.endswith("++") or before.endswith("--"):
            raise PassError("prefix increment of a memory operand: " + s.strip()[:120])
        op = next((c for c in COMPOUND if rest.startswith(c)), None)
        if op:
            e = rhs_end(s, k + len(op))
            rhs = xform(s[k + len(op):e]).strip()
            out.append(f"{head}{writer(name)}({arg}, {reader(name)}({arg}) {op[:-1]} ({rhs}))")
            i = e
        elif rest.startswith("=") and not rest.startswith("=="):
            e = rhs_end(s, k + 1)
            rhs = xform(s[k + 1:e]).strip()
            out.append(f"{head}{writer(name)}({arg}, {rhs})")
            i = e
        elif rest.startswith("++") or rest.startswith("--"):
            out.append(f"{head}{writer(name)}({arg}, {reader(name)}({arg}) {rest[0]} 1)")
            i = k + 2
        else:
            out.append(f"{head}{reader(name)}({arg})")
            i = j + 1
    return "".join(out)


def split_comment(line):
    """Code and trailing /* comment */ of one generated line (comments never nest
    and string literals in lifted code hold no comment markers)."""
    p = line.find("/*")
    q = line.find("//")
    cut = min([x for x in (p, q) if x >= 0], default=-1)
    return (line, "") if cut < 0 else (line[:cut], line[cut:])


def process(text, ranges):
    lines = text.split("\n")
    out = []
    inside = False
    funcs = reads = writes = 0
    for ln in lines:
        m = FUNC.match(ln)
        if m:
            va = int(m.group(1), 16)
            inside = any(a <= va < b for a, b in ranges)
            funcs += inside
        elif inside and ln.startswith("}"):
            inside = False
        elif inside and "MEM" in ln:
            code, comment = split_comment(ln)
            new = xform(code)
            if new != code:
                writes += new.count("MEMW") - code.count("MEMW")
                reads += new.count("MEMR") - code.count("MEMR")
                ln = new + comment
        out.append(ln)
    text2 = "\n".join(out)
    if (reads or writes) and INCLUDE not in text2:
        text2 = text2.replace('#include "recomp_funcs.h"', '#include "recomp_funcs.h"\n' + INCLUDE, 1)
    return text2, funcs, reads, writes


def selftest():
    cases = [
        ("    eax = MEM32(esi + 0x10);", "    eax = MEMR32(esi + 0x10);"),
        ("    MEM32(esi + 0x10) = eax;", "    MEMW32(esi + 0x10, eax);"),
        ("    MEM32(esp + 4) = MEM32(esi);", "    MEM32(esp + 4) = MEMR32(esi);"),
        ("    MEM32(0xFD003200) |= 0x100;", "    MEMW32(0xFD003200, MEMR32(0xFD003200) | (0x100));"),
        ("    MEM8(edi) = MEM8(esi); esi += 1;", "    MEMW8(edi, MEMR8(esi)); esi += 1;"),
        ("    if (MEM32(eax) == 0) goto a;", "    if (MEMR32(eax) == 0) goto a;"),
        ("    MEM32(MEM32(ebp - 4) + 8) = MEM32(ecx) + 1;", "    MEMW32(MEMR32(ebp - 4) + 8, MEMR32(ecx) + 1);"),
        ("    for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*_st) = eax;",
         "    for (_i = 0; _i < ecx; _i++) MEMW32(edi + _i*_st, eax);"),
        ("    MEMF(ecx) = (float)fp_top(); fp_pop();", "    MEMWF(ecx, (float)fp_top()); fp_pop();"),
        ("    eax = (uint32_t)SMEM16(ecx + 2);", "    eax = (uint32_t)SMEMR16(ecx + 2);"),
        ("    eax = MEM32(esp + 0x10); MEM32(0x485AB0) = eax;", "    eax = MEM32(esp + 0x10); MEM32(0x485AB0) = eax;"),
        ("    f(&MEM32(eax));", "    f(&MEM32(eax));"),
        ("    eax = ecx & MEM32(eax);", "    eax = ecx & MEMR32(eax);"),
        ("    MEM32(eax)++;", "    MEMW32(eax, MEMR32(eax) + 1);"),
        ("    MEMW32(esi, MEMR32(eax));", "    MEMW32(esi, MEMR32(eax));"),
    ]
    bad = 0
    for src, want in cases:
        got = xform(src)
        if got != want:
            bad += 1
            print("SELFTEST FAIL\n  in  ", src, "\n  got ", got, "\n  want", want)
    return bad


def main(argv):
    if selftest():
        return 1
    if len(argv) < 2:
        print(__doc__)
        return 1
    gen = argv[1]
    spec = argv[argv.index("--ranges") + 1] if "--ranges" in argv else DEFAULT_RANGES
    ranges = [tuple(int(x, 16) for x in r.split("-")) for r in spec.split(",")]
    check = "--check" in argv
    tot = [0, 0, 0, 0]
    for name in sorted(os.listdir(gen)):
        if not re.match(r"recomp_\d+\.c$", name):
            continue
        path = os.path.join(gen, name)
        text = open(path, encoding="utf-8", newline="").read()
        new, funcs, reads, writes = process(text, ranges)
        tot[0] += funcs
        tot[1] += reads
        tot[2] += writes
        if new != text:
            tot[3] += 1
            if not check:
                open(path, "w", encoding="utf-8", newline="").write(new)
    print(f"mmio_pass: {tot[0]} functions in {len(ranges)} ranges, {tot[1]} reads and {tot[2]} writes "
          f"made checked, {tot[3]} files {'would change' if check else 'changed'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
