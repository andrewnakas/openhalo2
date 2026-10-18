"""fecheck: is decomp function F (retail VA A) equivalent to retail A on real game states?

    python fecheck.py <va> [<va> ...] [--decomp <tree>] [--src <file.cpp>] [--k N] [--tag T]

For each snapshot of A (snapstore.py) Unicorn runs retail A, recording every call it makes
(target, entry registers, stack words, the callee's return registers / x87 push / memory
writes). Then it runs F, compiled by VS2022 (dbuild.py) and linked into a blob whose external
references resolve to retail VAs (symmap.py): every call F makes out of the blob is a mock
that checks the arguments against retail's call and replays retail's recorded effects. Callees
retail inlined but F calls are linked into the blob from their own decomp source; CRT helpers
(_ftol, 64-bit arithmetic, memcpy, _chkstk) run for real in both. The entry convention of A
(LTCG) is searched: live-in registers of A x F's parameters.

Compared: the return value (by F's declared type), all guest memory outside the stack window
and the code, and the call sequence. Verdicts: equivalent / equivalent_fp (only float values
within 1e-5 relative differ) / differs / unbuildable / untestable.
Results: <WORK>/results/<va><tag>.json + .txt (one line, also printed). Nothing written holds
retail bytes beyond the few values of a first difference.
"""
import argparse
import itertools
import json
import os
import pickle
import struct
import time

import numpy as np
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone import x86 as CX
from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_PROT_READ, UC_PROT_WRITE,
                     UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED)
from unicorn import x86_const as X

import coff
import common
import dbuild
import snapstore
import symmap

BLOB = 0x70000000
BLOB_SIZE = 0x200000
STUB = 0x70800000          # sentinel + mock stubs + scratch
GDT = 0x70900000
SENT = STUB                # return address planted for the function under test
SCR = STUB + 0x800         # register + fnsave area
CODE_LO = STUB + 0x1000    # per-call stubs (fnsave/frstor at a retail callee's return, mocks), 64 bytes each,
CODE_N = 0x3F00            # never reused within a run: Unicorn keeps translated code for rewritten bytes
CAP = 30_000_000
BLOB_CAP = 8_000_000      # ours: a wrong entry mapping usually loops; keep it short
RESULTS = os.path.join(common.WORK, 'results')
RCACHE = os.path.join(common.WORK, 'rcache_v4')   # bump when retail recording changes
REGS = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi']
UREG = {r: getattr(X, 'UC_X86_REG_' + r.upper()) for r in REGS + ['esp', 'ebp', 'eip']}
TRANSPARENT_NAMES = ['__chkstk', '_memcpy', '__allmul', '__aulldiv', '__alldiv', '__ftol2', '__aullshr', '__allshr',
                     '__allrem', '__allshl']
ALIASES = {'__ftol2_sse': '__ftol2', '__ftol': '__ftol2', '__alloca_probe': '__chkstk'}
# mov [SCR],eax; mov [SCR+4],edx; fnsave [SCR+16]
SENT_CODE = bytes([0xA3]) + struct.pack('<I', SCR) + bytes([0x89, 0x15]) + struct.pack('<I', SCR + 4) + \
    bytes([0x0F, 0x11, 0x05]) + struct.pack('<I', SCR + 0x100) + \
    bytes([0xDD, 0x35]) + struct.pack('<I', SCR + 16)   # + movups [SCR+0x100], xmm0 (LTCG float returns)
SENT_END = SENT + len(SENT_CODE)

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
# CRT routines VS2022 calls where vc71 inlined them (no retail copy to call): our own cdecl bodies,
# linked into the blob as part of F
BUILTINS = {
    'memset': bytes.fromhex('578b7c24088b44240c8b4c241089faf3aa89d05fc3'),   # rep stosb
    'memcpy': bytes.fromhex('56578b7c240c8b7424108b4c241489f8f3a45f5ec3'),   # rep movsb
    '_CIsqrt': bytes.fromhex('d9fac3'),     # fsqrt
    '_CIsin': bytes.fromhex('d9fec3'),      # fsin
    '_CIcos': bytes.fromhex('d9ffc3'),      # fcos
    '_CIatan2': bytes.fromhex('d9f3c3'),    # fpatan: atan(st1 / st0)
    '_CItan': bytes.fromhex('d9f2ddd8c3'),  # fptan; fstp st0
    '_CIacos': bytes.fromhex('d9c0d8c8d9e8dee1d9fad9c9d9f3c3'),   # atan2(sqrt(1-x*x), x)
    '_CIasin': bytes.fromhex('d9c0d8c8d9e8dee1d9fad9f3c3'),       # atan2(x, sqrt(1-x*x))
    '_CIatan': bytes.fromhex('d9e8d9f3c3'),                        # atan2(x, 1)
    '_CIlog': bytes.fromhex('d9edd9c9d9f1c3'),                     # ln2 * log2(x)
    '_CIlog10': bytes.fromhex('d9ecd9c9d9f1c3'),
    # 2^t for t in st0: fld st0; frndint; fsub st1,st0; fxch; f2xm1; fld1; faddp; fscale; fstp st1
    '_CIexp': bytes.fromhex('d9eadec9' + 'd9c0d9fcdce9d9c9d9f0d9e8dec1d9fddd d9c3'.replace(' ', '')),
    '_CIpow': bytes.fromhex('d9c9d9f1' + 'd9c0d9fcdce9d9c9d9f0d9e8dec1d9fddd d9c3'.replace(' ', '')),
    '_CIfmod': bytes.fromhex('d9c9d9f8dfe09e7af9ddd9c3'),          # fxch; fprem loop; fstp st1
}


class Fail(Exception):
    pass


# ---------------------------------------------------------------- kernel imports
# The recomp gives kernel thunk slot i the address 0xFE000000 + 4*i (xboxrecomp kernel_bridge.c, MIT);
# its stdcall_args_for_ordinal() table (with export names in comments) gives each ordinal's argument
# bytes. A kernel call here returns 0 and pops its arguments: retail and ours both see what retail's
# run recorded, so the comparison stays fair.
KVA = 0xFE000000
KBRIDGE = 'D:/n64work/halo2/third_party/xboxrecomp/src/kernel/kernel_bridge.c'
THUNKS = 0x411520          # the XBE's kernel thunk table (header 0x158 ^ retail key)
_KERNEL = None


def kernel():
    """(slot -> (ordinal, arg bytes), export name -> thunk slot VA)."""
    global _KERNEL
    if _KERNEL is None:
        import re as _re
        args, names = {}, {}
        try:
            for m in _re.finditer(r'case\s+(\d+):\s*return\s+(\d+);\s*/\*\s*(\w+)', open(KBRIDGE, errors='replace').read()):
                args[int(m.group(1))] = int(m.group(2))
                names[m.group(3)] = int(m.group(1))
        except OSError:
            pass
        slots, i = {}, 0
        while True:
            b = common.xbe_read(THUNKS + 4 * i, 4)
            v = struct.unpack('<I', b)[0] if b else 0
            if not v:
                break
            slots[i] = (v & 0x7FFFFFFF, args.get(v & 0x7FFFFFFF, 0))
            i += 1
        by_ord = {o: THUNKS + 4 * i for i, (o, n) in slots.items()}
        if 156 in by_ord:   # XAPI GetTickCount = the KeTickCount data export (ordinal 156)
            BUILTINS['GetTickCount'] = bytes([0xA1]) + struct.pack('<I', by_ord[156]) + bytes([0x8B, 0x00, 0xC3])
        _KERNEL = (slots, {nm: by_ord[o] for nm, o in names.items() if o in by_ord})
    return _KERNEL


def kernel_hook(uc):
    slots = kernel()[0]
    st = {'last': None}

    def on_k(u, addr, size, d):
        esp = u.reg_read(X.UC_X86_REG_ESP)
        if st['last'] == (addr, esp):
            return
        o, n = slots.get((addr - KVA) // 4, (0, 0))
        ret = u32(u, esp)
        u.reg_write(X.UC_X86_REG_EAX, 0)
        u.reg_write(X.UC_X86_REG_ESP, esp + 4 + n)
        st['last'] = (addr, esp + 4 + n)
        u.reg_write(X.UC_X86_REG_EIP, ret)
    uc.mem_map(KVA, 0x1000)
    uc.mem_write(KVA, bytes([0xF4]) * 0x1000)
    uc.hook_add(UC_HOOK_CODE, on_k, begin=KVA, end=KVA + 0xFFF)


# ---------------------------------------------------------------- symbols


_TABLE = None
_SOURCES = None
_NEAR = None


def table():
    global _TABLE, _SOURCES
    if _TABLE is None:
        _TABLE = symmap.load()
        _SOURCES = {}
        for r in common.read_csv(os.path.join(common.RUNTIME, 'decomp_symbols.csv')):
            if r['source']:
                _SOURCES.setdefault(int(r['va'], 16), r['source'])
        for a, b in ALIASES.items():
            if b in _TABLE:
                _TABLE.setdefault(a, _TABLE[b])
    return _TABLE


def transparent():
    t = table()
    return {t[n] for n in TRANSPARENT_NAMES if n in t}


def resolve(name):
    t = table()
    pn = coff.plain_name(name)
    for k in (name, pn, ALIASES.get(name), ALIASES.get(pn)):
        if k and k in t:
            return t[k]
    m = symmap.HEXNAME.match(pn)
    return int(m.group(2), 16) if m else None


def nearest(va):
    global _NEAR
    if _NEAR is None:
        inv = {}
        for n, v in table().items():
            inv.setdefault(v, n)
        _NEAR = sorted(inv.items())
    import bisect
    k = bisect.bisect_right(_NEAR, (va, '￿')) - 1
    if k >= 0 and va - _NEAR[k][0] < 0x10000:
        v, n = _NEAR[k]
        return n if v == va else '%s+0x%x' % (n, va - v)
    return '%08x' % va


# ---------------------------------------------------------------- build + link


def obj_for(src, decomp, env, fresh=False, out=None):
    path = src if os.path.isabs(src) else os.path.join(decomp, src)
    stem = os.path.splitext(os.path.basename(path))[0]
    if os.path.basename(os.path.dirname(path)) == 'stubs':
        stem = 'stubs__' + stem
    obj = out or os.path.join(dbuild.objdir(decomp), stem + '.obj')
    if fresh or not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(path), dbuild.newest_header(decomp)):
        _, ok, errs, obj = dbuild.compile_one(path, decomp, env, out=out)
        if not ok:
            raise Fail('unbuildable: compile: ' + ' | '.join(errs[:3]))
    return obj


def retail_calls(va, size):
    """Direct call/jmp targets in retail A's bytes (outside A)."""
    body = common.xbe_read(va, size) or b''
    out = set()
    for ins in md.disasm(body, va):
        if ins.mnemonic in ('call', 'jmp') and ins.operands and ins.operands[0].type == CX.X86_OP_IMM:
            t = ins.operands[0].imm & 0xFFFFFFFF
            if not va <= t < va + size:
                out.add(t)
    return out


class Blob:
    def __init__(self):
        self.data = bytearray()
        self.placed = {}      # (obj path, section index) -> address
        self.callee_sig = {}  # retail VA -> Sig (F's declaration of the callee)
        self.callees = set()  # retail VAs F calls directly (mocked)
        self.linked = []      # names of functions linked in
        self.unlinked = set() # retail VAs retail never calls directly whose source does not build
        self.entry = None
        self.sig = None
        self.name = None


def _code_sym(o, s):
    return s.section > 0 and s.sclass in (2, 3) and bool(o.sections[s.section].flags & coff.SCN_CODE) and not s.naux


def link(objp, va, size, decomp, env):
    """Link F (the function at va in objp) into a Blob at BLOB."""
    objs = {}

    def get(p):
        if p not in objs:
            objs[p] = coff.Coff(p)
        return objs[p]
    o = get(objp)
    fsym = next((s for s in o.symbols if _code_sym(o, s) and resolve(s.name) == va), None)
    if fsym is None:
        raise Fail('unbuildable: no symbol for %08x in %s' % (va, os.path.basename(objp)))
    rc = retail_calls(va, size)
    transp = transparent()
    b = Blob()
    b.name = fsym.name
    b.sig = coff.signature(fsym.name)
    unresolved = set()
    relocs = []
    work = []

    def place(p, si):
        key = (p, si)
        if key in b.placed:
            return
        sec = get(p).sections[si]
        al = max(sec.align, 4)
        while len(b.data) % al:
            b.data.append(0xCC)
        b.placed[key] = BLOB + len(b.data)
        b.data += sec.data
        work.append(key)

    def code_def(name):
        """(obj path, section) of a code symbol compiled from its own decomp source."""
        v = resolve(name)
        src = _SOURCES.get(v) if v is not None else None
        if not src:
            return None
        try:
            p = obj_for(os.path.join('src', src), decomp, env)
        except Fail:
            return None
        oo = get(p)
        s = next((s for s in oo.symbols if _code_sym(oo, s) and resolve(s.name) == v), None)
        return (p, s.section) if s else None
    place(objp, fsym.section)
    done = set()
    while work:
        p, si = work.pop()
        if (p, si) in done:
            continue
        done.add((p, si))
        oo = get(p)
        sec = oo.sections[si]
        for off, symi, typ in sec.relocs:
            t = oo.by_index[symi]
            if typ in (coff.IMAGE_REL_I386_SECREL, coff.IMAGE_REL_I386_SECTION, coff.IMAGE_REL_I386_ABSOLUTE):
                continue
            target = None
            is_code = t.section > 0 and bool(oo.sections[t.section].flags & coff.SCN_CODE)
            v = None if (t.section > 0 and t.sclass == 3) else resolve(t.name)
            if v is not None and typ == coff.IMAGE_REL_I386_REL32 and v not in rc and v not in transp and v != va:
                cd = (p, t.section) if (t.section > 0 and is_code) else code_def(t.name)
                if not cd:
                    b.unlinked.add(v)
                if cd:   # retail inlined it (or reaches it another way): run our compile of it
                    place(*cd)
                    b.linked.append(coff.plain_name(t.name))
                    sym = next((s for s in get(cd[0]).symbols if s.section == cd[1] and _code_sym(get(cd[0]), s)), None)
                    target = ('sec', cd, sym.value if sym else 0)
            if target is None and v is not None:
                target = ('va', v)
                if typ == coff.IMAGE_REL_I386_REL32 and v not in transp:
                    b.callees.add(v)
                    b.callee_sig[v] = coff.signature(t.name)
            if target is None and t.section > 0:
                tsec = oo.sections[t.section]
                if tsec.flags & coff.SCN_WRITE and not (tsec.flags & coff.SCN_CODE):
                    raise Fail('unbuildable: writable data %s has no retail address' % coff.plain_name(t.name))
                place(p, t.section)
                target = ('sec', (p, t.section), t.value)
            if target is None and t.section <= 0 and t.name.startswith('__imp_'):
                nm = t.name[6:]
                bare = coff.plain_name(nm)
                kv = kernel()[1].get(bare)
                if kv is not None:            # a kernel export: its thunk slot (holds 0xFE000000 + 4*slot)
                    target = ('va', kv)
                else:
                    fv = resolve(nm)
                    if fv is None and bare in BUILTINS:
                        key = ('builtin', bare)
                        if key not in b.placed:
                            while len(b.data) % 16:
                                b.data.append(0xCC)
                            b.placed[key] = BLOB + len(b.data)
                            b.data += BUILTINS[bare]
                            b.linked.append(bare)
                        fv = b.placed[key]
                    if fv is not None:        # an XAPI routine: a pointer cell like the import table's
                        key = ('impcell', nm)
                        if key not in b.placed:
                            while len(b.data) % 4:
                                b.data.append(0)
                            b.placed[key] = BLOB + len(b.data)
                            b.data += struct.pack('<I', fv)
                        target = ('sec', key, 0)
            if target is None and coff.plain_name(t.name) in BUILTINS:
                bn = coff.plain_name(t.name)
                key = ('builtin', bn)
                if key not in b.placed:
                    while len(b.data) % 16:
                        b.data.append(0xCC)
                    b.placed[key] = BLOB + len(b.data)
                    b.data += BUILTINS[bn]
                    b.linked.append(bn)
                target = ('sec', key, 0)
            if target is None:
                unresolved.add(coff.plain_name(t.name))
                continue
            relocs.append(((p, si), off, typ, target))
    if unresolved:
        raise Fail('unbuildable: unresolved ' + ', '.join(sorted(unresolved)[:6]))
    if len(b.data) > BLOB_SIZE:
        raise Fail('unbuildable: blob too large')
    for key, off, typ, target in relocs:
        P = b.placed[key] + off
        S = target[1] if target[0] == 'va' else b.placed[target[1]] + target[2]
        o_ = P - BLOB
        add = struct.unpack_from('<i', b.data, o_)[0]
        if typ == coff.IMAGE_REL_I386_DIR32:
            struct.pack_into('<I', b.data, o_, (S + add) & 0xFFFFFFFF)
        elif typ == coff.IMAGE_REL_I386_REL32:
            struct.pack_into('<I', b.data, o_, (S + add - (P + 4)) & 0xFFFFFFFF)
        else:
            raise Fail('unbuildable: reloc type %d' % typ)
    b.entry = b.placed[(objp, fsym.section)] + fsym.value
    return b


# ---------------------------------------------------------------- convention


MASKS = {'i8': 0xFF, 'i16': 0xFFFF}
XREGS = ['xmm%d' % i for i in range(8)]   # LTCG passes some float arguments in SSE registers
PARTS = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx', 'dl': 'edx', 'dh': 'edx',
         'dx': 'edx', 'bl': 'ebx', 'bh': 'ebx', 'bx': 'ebx', 'si': 'esi', 'di': 'edi'}


def live_in(va, size):
    """Registers retail A reads before writing them on some path from its entry (its register
    arguments), in order of first read. Walks the control flow inside A."""
    body = common.xbe_read(va, size) or b''
    insns = {i.address: i for i in md.disasm(body, va)}
    out = []
    work = [(va, frozenset())]
    seen = set()
    steps = 0
    while work and steps < 3000:
        addr, written = work.pop()
        while addr in insns and steps < 3000:
            if (addr, written) in seen or len(seen) > 4000:
                break
            seen.add((addr, written))
            steps += 1
            ins = insns[addr]
            mn = ins.mnemonic
            if mn in ('ret', 'int3'):
                break
            try:
                rd, wr = ins.regs_access()
            except Exception:
                break
            rd = {PARTS.get(md.reg_name(r), md.reg_name(r)) for r in rd}
            wr = {PARTS.get(md.reg_name(r), md.reg_name(r)) for r in wr}
            ops = ins.operands
            if mn in ('xor', 'sub', 'sbb') and len(ops) == 2 and ops[0].type == ops[1].type == CX.X86_OP_REG and ops[0].reg == ops[1].reg:
                rd = set()
            if mn in ('or', 'and') and len(ops) == 2 and ops[1].type == CX.X86_OP_IMM and (ops[1].imm & 0xFFFFFFFF) in ((0xFFFFFFFF,) if mn == 'or' else (0,)):
                rd = set()
            if mn in ('mov', 'lea') and len(ops) == 2 and ops[0].type == ops[1].type == CX.X86_OP_REG and ops[0].reg == ops[1].reg:
                rd = set()   # mov edi, edi: padding
            if mn == 'push' and ops and ops[0].type == CX.X86_OP_REG and md.reg_name(ops[0].reg) in ('ebx', 'esi', 'edi', 'ebp'):
                rd = set()
            for r in REGS + XREGS:
                if r in rd and r not in written and r not in out:
                    out.append(r)
            written = written | (wr & set(REGS + XREGS))
            if mn == 'call':
                written = written | {'eax', 'ecx', 'edx'}
            nxt = addr + ins.size
            if mn.startswith('j'):
                op = ops[0] if ops else None
                tgt = op.imm & 0xFFFFFFFF if op is not None and op.type == CX.X86_OP_IMM else None
                if mn == 'jmp':
                    if tgt is None or not va <= tgt < va + size:
                        break
                    addr = tgt
                    continue
                if tgt is not None and va <= tgt < va + size:
                    work.append((tgt, written))
            addr = nxt
    return out


def candidates(sig, live, cap=150):
    """Mappings F param -> retail location: ('reg', name, size) / ('stk', slot index, size)."""
    loc = sig.locations() if sig else None
    if loc is None:
        return []
    out = []

    def assign(reg_of):
        m, slot = [], 0
        for i, (kind, where, sz, pt) in enumerate(loc):
            if i in reg_of:
                m.append(('reg', reg_of[i], sz))
            else:
                m.append(('stk', slot, sz))
                slot += (sz + 3) // 4
        return m
    out.append(assign({i: where for i, (kind, where, sz, pt) in enumerate(loc) if kind == 'reg'}))
    small = [i for i, l in enumerate(loc) if l[2] == 4 and l[3] != 'float']
    floats = [i for i, l in enumerate(loc) if l[3] == 'float']
    regs = [r for r in live if r in REGS]
    xregs = [r for r in live if r in XREGS]
    xvariants = [{}]
    for k in range(min(len(xregs), len(floats)), 0, -1):
        for chosen in itertools.combinations(floats, k):
            for rs in itertools.permutations(xregs, k):
                xvariants.insert(len(xvariants) - 1, dict(zip(chosen, rs)))
    for k in range(min(len(regs), len(small)), -1, -1):
        for chosen in itertools.combinations(small, k):
            for rs in itertools.permutations(regs, k):
                for xv in xvariants:
                    d_ = dict(zip(chosen, rs))
                    d_.update(xv)
                    m = assign(d_)
                    if m not in out:
                        out.append(m)
                    if len(out) >= cap:
                        return out
    return out


# ---------------------------------------------------------------- emulation


def desc(base, limit=0xFFFFF, access=0x92, flags=0xC):
    return (limit & 0xFFFF) | (base & 0xFFFFFF) << 16 | access << 40 | ((limit >> 16) & 0xF) << 48 | flags << 52 | \
        ((base >> 24) & 0xFF) << 56


_CODE = None


def code_ranges():
    global _CODE
    if _CODE is None:
        _CODE = [(v, raw, vs) for nm, v, vs, raw, ex in common.xbe_sections() if common.is_code(nm)]
    return _CODE


def machine(s):
    ram = s['ram'].copy()
    con = s['contig'].copy()
    for v, raw, vs in code_ranges():
        if v + len(raw) <= len(ram):
            ram[v:v + len(raw)] = np.frombuffer(raw, dtype=np.uint8)
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    sz = len(ram)
    uc.mem_map_ptr(0, sz, UC_PROT_ALL, ram.ctypes.data)
    for cb in (0x80000000, 0xF0000000):
        uc.mem_map_ptr(cb, len(con), UC_PROT_READ | UC_PROT_WRITE, con.ctypes.data)
    uc.mem_map(BLOB, BLOB_SIZE)
    uc.mem_map(STUB, 0x1000 + CODE_N * 64)
    uc.mem_map(GDT, 0x1000, UC_PROT_READ | UC_PROT_WRITE)
    mirrors = set()

    def unm(u, acc, addr, size, val, d):
        if addr >= 0x80000000 or addr < sz:
            return False
        bse = addr - addr % sz
        if bse in mirrors:
            return False
        mirrors.add(bse)
        u.mem_map_ptr(bse, sz, UC_PROT_READ | UC_PROT_WRITE, ram.ctypes.data)
        return True
    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, unm)
    uc.mem_write(SENT, SENT_CODE)
    kernel_hook(uc)
    g = [0, desc(0, access=0x9A), desc(0), desc(s['fs_base'] or 0)]
    uc.mem_write(GDT, b''.join(struct.pack('<Q', d) for d in g))
    uc.reg_write(X.UC_X86_REG_GDTR, (0, GDT, len(g) * 8 - 1, 0))
    # loading FS alone leaves SS unusable in Unicorn: load the flat data selector everywhere first
    for sr in (X.UC_X86_REG_SS, X.UC_X86_REG_DS, X.UC_X86_REG_ES, X.UC_X86_REG_GS):
        uc.reg_write(sr, 2 << 3)
    uc.reg_write(X.UC_X86_REG_FS, 3 << 3)
    for r in REGS + ['ebp', 'esp']:
        uc.reg_write(UREG[r], s[r])
    uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    uc.reg_write(X.UC_X86_REG_FPCW, (s['fpcw'] & 0xFFFF) or 0x27F)
    for i in range(8):
        uc.reg_write(getattr(X, 'UC_X86_REG_XMM%d' % i), int.from_bytes(s['xmm'][i], 'little'))
    uc._keep = (ram, con)
    return uc, ram, con


def stub_addr(uc, st):
    at = CODE_LO + 64 * (st['stub'] % CODE_N)
    st['stub'] += 1
    if st['stub'] > CODE_N:
        uc.ctl_remove_cache(at, at + 64)
    return at


def u32(uc, a):
    return struct.unpack('<I', uc.mem_read(a, 4))[0]


_DEC = {}


def decode(uc, addr, cache=True):
    ins = _DEC.get(addr) if cache else None
    if ins is None:
        ins = next(md.disasm(bytes(uc.mem_read(addr, 16)), addr), False)
        if cache:
            _DEC[addr] = ins
    return ins


def target_of(uc, ins):
    op = ins.operands[0]
    if op.type == CX.X86_OP_IMM:
        return op.imm & 0xFFFFFFFF
    if op.type == CX.X86_OP_REG:
        return uc.reg_read(UREG[md.reg_name(op.reg)])
    m = op.mem
    a = m.disp
    if m.base:
        a += uc.reg_read(UREG[md.reg_name(m.base)])
    if m.index:
        a += uc.reg_read(UREG[md.reg_name(m.index)]) * m.scale
    return u32(uc, a & 0xFFFFFFFF)


def fpsave(raw):
    """fnsave image -> (TOP, [st0..st7 as 10-byte values])."""
    sw = struct.unpack_from('<H', raw, 4)[0]
    return (sw >> 11) & 7, [bytes(raw[28 + 10 * i:38 + 10 * i]) for i in range(8)]


def ld80(b):
    m, e = struct.unpack('<QH', b)
    sign = -1.0 if e & 0x8000 else 1.0
    e &= 0x7FFF
    if e == 0 and m == 0:
        return 0.0 * sign
    if e == 0x7FFF:
        return float('nan') if m & 0x7FFFFFFFFFFFFFFF else sign * float('inf')
    try:
        return sign * (m / (1 << 63)) * 2.0 ** (e - 16383)
    except OverflowError:
        return sign * float('inf')


class Rec:
    __slots__ = ('target', 'regs', 'stack', 'ret', 'esp_call', 'tail', 'indirect', 'popped', 'eax', 'edx', 'push',
                 'st0', 'writes', 'top0', 'xmm0')

    def __getstate__(self):
        return {k: getattr(self, k, None) for k in self.__slots__}

    def __setstate__(self, d):
        for k, v in d.items():
            setattr(self, k, v)


def merge(ws):
    out = []
    for a, n in sorted(ws):
        if out and a <= out[-1][0] + out[-1][1]:
            lo = out[-1][0]
            out[-1] = (lo, max(lo + out[-1][1], a + n) - lo)
        else:
            out.append((a, n))
    return out


def run_retail(s, va, size):
    """Retail A from the snapshot: (result dict, call records)."""
    uc, ram, con = machine(s)
    esp0 = s['esp']
    uc.mem_write(esp0, struct.pack('<I', SENT))
    transp = transparent()
    recs, st = [], {'cur': None, 'w': [], 'fs': False, 'fsran': False, 'stub': 0}

    def on_w(u, acc, addr, size_, val, d):
        if st['cur'] is not None:
            st['w'].append((addr, size_))

    def finish(r):
        r.writes = [(a, bytes(uc.mem_read(a, n))) for a, n in merge(st['w']) if n < 0x100000]
        st['w'] = []
        st['cur'] = None
        recs.append(r)

    def on_code(u, addr, size_, d):
        r = st['cur']
        if r is not None:
            if addr != r.ret or u.reg_read(X.UC_X86_REG_ESP) <= r.esp_call - 4:
                return
            if st['fs'] and not st['fsran']:
                return   # the hook fires again on the instruction it redirected
            if not st['fs']:
                r.eax = u.reg_read(X.UC_X86_REG_EAX)
                r.edx = u.reg_read(X.UC_X86_REG_EDX)
                r.popped = u.reg_read(X.UC_X86_REG_ESP) - r.esp_call
                code = bytes([0x0F, 0x11, 0x05]) + struct.pack('<I', SCR + 0x140) + \
                    bytes([0xDD, 0x35]) + struct.pack('<I', SCR + 16) + bytes([0xDD, 0x25]) + struct.pack('<I', SCR + 16) + \
                    bytes([0x68]) + struct.pack('<I', addr) + bytes([0xC3])
                at = stub_addr(u, st)
                u.mem_write(at, code)
                st['fs'] = True
                st['fsran'] = False
                u.reg_write(X.UC_X86_REG_EIP, at)
                return
            st['fs'] = False
            top, sts = fpsave(u.mem_read(SCR + 16, 108))
            r.push = top == ((r.top0 - 1) & 7)
            r.st0 = sts[0] if r.push else None
            r.xmm0 = bytes(u.mem_read(SCR + 0x140, 16))
            finish(r)
        ins = decode(u, addr)
        if not ins or ins.mnemonic not in ('call', 'jmp'):
            return
        t = target_of(u, ins)
        if ins.mnemonic == 'jmp' and va <= t < va + size:
            return
        if t in transp:
            return
        r = Rec()
        r.target, r.tail = t, ins.mnemonic == 'jmp'
        r.indirect = ins.operands[0].type != CX.X86_OP_IMM
        r.regs = [u.reg_read(UREG[x]) for x in REGS]
        esp = u.reg_read(X.UC_X86_REG_ESP)
        r.esp_call = esp + (4 if r.tail else 0)   # the callee's first stack argument
        r.stack = list(struct.unpack('<32I', u.mem_read(r.esp_call, 128)))
        r.ret = SENT if r.tail else addr + ins.size
        r.top0 = (u.reg_read(X.UC_X86_REG_FPSW) >> 11) & 7
        r.writes = []
        st['cur'] = r
    uc.hook_add(UC_HOOK_CODE, on_code, begin=va, end=va + size - 1)
    uc.hook_add(UC_HOOK_CODE, lambda u, a, n, d: st.__setitem__('fsran', True), begin=CODE_LO, end=CODE_LO + CODE_N * 64 - 1)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_w)
    top0 = (uc.reg_read(X.UC_X86_REG_FPSW) >> 11) & 7
    try:
        uc.emu_start(va, SENT_END, timeout=120_000_000, count=CAP)
    except UcError as e:
        raise Fail('untestable: retail %s at %08x' % (e, uc.reg_read(X.UC_X86_REG_EIP)))
    if uc.reg_read(X.UC_X86_REG_EIP) != SENT_END:
        raise Fail('untestable: retail did not return (cap) at %08x' % uc.reg_read(X.UC_X86_REG_EIP))
    eax, edx = struct.unpack('<II', uc.mem_read(SCR, 8))
    top, sts = fpsave(uc.mem_read(SCR + 16, 108))
    r = st['cur']
    if r is not None:   # a tail call ran to the end
        r.eax, r.edx = eax, edx
        r.popped = uc.reg_read(X.UC_X86_REG_ESP) - r.esp_call
        r.push = top == ((r.top0 - 1) & 7)
        r.st0 = sts[0] if r.push else None
        r.xmm0 = bytes(uc.mem_read(SCR + 0x100, 16))
        finish(r)
    res = dict(eax=eax, edx=edx, push=top == ((top0 - 1) & 7), st0=sts[0], xmm0=bytes(uc.mem_read(SCR + 0x100, 16)))
    res['delta'] = delta(s, ram, con, esp0)
    return res, recs


def delta(s, ram, con, esp0):
    d = np.nonzero(ram != s['ram'])[0]
    for lo, hi in [(esp0 - 0x40000, esp0 + 4)] + [(v, v + vs) for v, raw, vs in code_ranges()]:
        d = d[(d < lo) | (d >= hi)]
    dc = np.nonzero(con != s['contig'])[0]
    return (d.astype(np.int64), ram[d].copy(), dc.astype(np.int64), con[dc].copy())


def near_float(a, b, ulps=8):
    """Two dwords are normal floats of one sign within a few ulps (x87 vs SSE / spill rounding)."""
    x, y = struct.unpack('<I', a)[0], struct.unpack('<I', b)[0]
    ex, ey = (x >> 23) & 0xFF, (y >> 23) & 0xFF
    return (x >> 31) == (y >> 31) and 0 < ex < 255 and 0 < ey < 255 and abs(x - y) <= ulps


def word_at(base, idx, vals, w):
    out = bytearray(base[w:w + 4].tobytes())
    k = int(np.searchsorted(idx, w))
    while k < len(idx) and idx[k] < w + 4:
        out[int(idx[k]) - w] = int(vals[k])
        k += 1
    return bytes(out)


def same_pointee(uc, v, cands, n=32):
    try:
        want = bytes(uc.mem_read(v, 64))
    except UcError:
        return False
    z = want.find(bytes(1))
    want = want[:z + 1] if z >= 0 else want[:16]   # a C string up to its NUL, else the first 16 bytes
    for w in cands:
        try:
            if bytes(uc.mem_read(w, len(want))) == want:
                return True
        except UcError:
            pass
    return False


def run_blob(s, b, mapping, rres, recs):
    uc, ram, con = machine(s)
    esp0 = s['esp']
    uc.mem_write(BLOB, bytes(b.data))
    new_esp = esp0 - 0x10000
    stk = bytearray()
    for (kind, where, sz, pt), (mk, mw, msz) in zip(b.sig.locations(), mapping):
        if mk == 'reg' and mw in XREGS:
            raw = bytes(s['xmm'][XREGS.index(mw)][:4])
        else:
            raw = struct.pack('<I', s[mw]) if mk == 'reg' else bytes(uc.mem_read(esp0 + 4 + 4 * mw, (sz + 3) & ~3))
        if kind == 'reg':
            uc.reg_write(UREG[where], struct.unpack('<I', raw[:4])[0])
        else:
            if len(stk) < where + len(raw):
                stk += bytes(where + len(raw) - len(stk))
            stk[where:where + len(raw)] = raw
    uc.mem_write(new_esp, struct.pack('<I', SENT) + bytes(stk) + bytes(64))
    bsl = (len(stk) + 3) // 4
    rsl = max([w + (sz + 3) // 4 for k, w, sz in mapping if k == 'stk'], default=0)
    uc.reg_write(X.UC_X86_REG_ESP, new_esp)
    transp = transparent()
    st = {'ri': 0, 'fail': None, 'n': 0, 'pend': None, 'stub': 0}
    lo_b, hi_b = BLOB, BLOB + len(b.data)

    def fptrs(values, lo, hi):
        return sorted({v for v in values if lo <= v < hi})

    def stop(msg):
        st['fail'] = msg
        uc.emu_stop()

    def on_code(u, addr, size_, d):
        if st['fail'] or st['pend'] == addr:
            return
        ins = decode(u, addr, cache=False)
        if not ins or ins.mnemonic not in ('call', 'jmp'):
            return
        t = target_of(u, ins)
        if lo_b <= t < hi_b or t in transp:
            return
        tail = ins.mnemonic == 'jmp'
        direct = ins.operands[0].type == CX.X86_OP_IMM
        st['n'] += 1
        ri = st['ri']
        while ri < len(recs) and recs[ri].target != t and not recs[ri].indirect and recs[ri].target not in b.callees:
            ri += 1   # retail calls F inlined
        if (ri >= len(recs) or recs[ri].target != t) and t in b.unlinked:
            return stop('UNLINKED %s: retail inlined it and its source does not build here' % nearest(t))
        if ri >= len(recs) or recs[ri].target != t:
            nxt = nearest(recs[ri].target) if ri < len(recs) else 'none'
            return stop('call %d to %s: retail makes no such call here (retail next: %s)' % (st['n'], nearest(t), nxt))
        r = recs[ri]
        st['ri'] = ri + 1
        esp = u.reg_read(X.UC_X86_REG_ESP) + (4 if tail else 0)
        sig = b.callee_sig.get(t) if direct else None
        cl = sig.locations() if sig else None
        fregs = {x: u.reg_read(UREG[x]) for x in REGS}
        args, masks = [], []
        if cl is not None:
            for kind, where, sz, pt in cl:
                if kind == 'reg':
                    args.append(fregs[where])
                    masks.append(MASKS.get(pt, 0xFFFFFFFF))
                else:
                    w_ = list(struct.unpack('<%dI' % ((sz + 3) // 4), u.mem_read(esp + where, (sz + 3) & ~3)))
                    args += w_
                    masks += [MASKS.get(pt, 0xFFFFFFFF)] * len(w_)
        else:
            n = min((r.popped or 0) // 4, len(r.stack))
            args = list(struct.unpack('<%dI' % n, u.mem_read(esp, 4 * n))) if n else []
        rset = set(r.regs) | set(r.stack)
        r_hi, b_hi = esp0 + 4 + 4 * rsl, new_esp + 4 + 4 * bsl   # frames incl. incoming argument slots
        r_fp = fptrs(r.regs + r.stack, r.esp_call, r_hi)
        b_fp = fptrs(args + list(fregs.values()), esp, b_hi)
        for i, v in enumerate(args):
            if v in rset or (esp <= v < b_hi and r_fp):
                continue
            mk = masks[i] if i < len(masks) else 0xFFFFFFFF
            if mk != 0xFFFFFFFF and any((v ^ x) & mk == 0 for x in rset):
                continue   # a bool/char/short: only its own width is the value
            if lo_b <= v < hi_b and same_pointee(u, v, r.regs + r.stack):
                continue   # our copy of a literal / constant: same bytes as what retail passed
            return stop('call %d to %s: argument %d = %08x, retail passed no such value (retail regs %s; stack %s)' % (
                st['n'], nearest(t), i, v, ' '.join('%08x' % x for x in r.regs), ' '.join('%08x' % x for x in r.stack[:6])))
        # frame pointers retail passed -> ours: by argument position when both sides pass the same
        # stack layout (and ecx/edx), else in address order
        pairs = {}
        if cl is not None and sig is not None and (r.popped or 0) == (sig.stack_bytes() or 0):
            for kind, where, sz, pt in cl:
                rv_ = r.regs[REGS.index(where)] if kind == 'reg' else r.stack[where // 4] if where // 4 < len(r.stack) else None
                bv_ = fregs[where] if kind == 'reg' else struct.unpack('<I', u.mem_read(esp + where, 4))[0]
                if rv_ is not None and r.esp_call <= rv_ < r_hi and esp <= bv_ < b_hi:
                    pairs[rv_] = bv_
        if not pairs:
            pairs = dict(zip(r_fp, b_fp))
        pk = sorted(pairs)
        for a, data in r.writes:
            if r.esp_call - 0x40000 <= a < r.esp_call:
                continue                       # the callee's own stack
            if r.esp_call <= a < r_hi:         # retail's frame -> ours
                p_ = max((p for p in pk if p <= a), default=None)
                if p_ is None:
                    continue
                a = a - p_ + pairs[p_]
            u.mem_write(a, data)
        if sig is not None and cl is not None:
            pops = sig.stack_bytes() if sig.callee_pops() else 0
            if pops != (r.popped or 0) and sig.ret == 'struct' and sig.callee_pops() and r.popped == pops + 4:
                pops += 4   # the hidden return-buffer pointer
        else:
            pops = r.popped or 0
        code = bytes([0xB8]) + struct.pack('<I', r.eax) + bytes([0xBA]) + struct.pack('<I', r.edx)
        xm = getattr(r, 'xmm0', None)
        if xm:
            u.mem_write(SCR + 0x240, xm)
            code += bytes([0x0F, 0x10, 0x05]) + struct.pack('<I', SCR + 0x240)  # movups xmm0, [..]
        if r.push:
            u.mem_write(SCR + 0x200, r.st0)
            code += bytes([0xDB, 0x2D]) + struct.pack('<I', SCR + 0x200)        # fld tbyte [..]
        elif xm and sig is not None and sig.ret in ('float', 'double'):
            code += bytes([0xD9, 0x05] if sig.ret == 'float' else [0xDD, 0x05]) + struct.pack('<I', SCR + 0x240)  # fld [xmm0]
        if tail:
            code += bytes([0xC2]) + struct.pack('<H', pops)                       # ret N
        else:
            code += bytes([0x81, 0xC4]) + struct.pack('<I', pops) + bytes([0x68]) + struct.pack('<I', addr + ins.size) + bytes([0xC3])
        at = stub_addr(u, st)
        u.mem_write(at, code)
        st['pend'] = addr
        u.reg_write(X.UC_X86_REG_EIP, at)
    uc.hook_add(UC_HOOK_CODE, on_code, begin=lo_b, end=hi_b - 1)
    uc.hook_add(UC_HOOK_CODE, lambda u, a, n, d: st.__setitem__('pend', None), begin=CODE_LO, end=CODE_LO + CODE_N * 64 - 1)
    top0 = (uc.reg_read(X.UC_X86_REG_FPSW) >> 11) & 7
    try:
        uc.emu_start(b.entry, SENT_END, timeout=60_000_000, count=rres.get('icount_cap', BLOB_CAP))
    except UcError as e:
        if not st['fail']:
            return 'differs', 'ours faults: %s at %08x' % (e, uc.reg_read(X.UC_X86_REG_EIP))
    if st['fail']:
        return 'differs', st['fail']
    if uc.reg_read(X.UC_X86_REG_EIP) != SENT_END:
        return 'differs', 'ours did not return (cap/loop) at %08x' % uc.reg_read(X.UC_X86_REG_EIP)
    left = [r for r in recs[st['ri']:] if r.indirect or r.target in b.callees]
    if left:
        return 'differs', 'retail calls %s next (call %d); ours made no more calls' % (nearest(left[0].target), st['n'] + 1)
    eax, edx = struct.unpack('<II', uc.mem_read(SCR, 8))
    top, sts = fpsave(uc.mem_read(SCR + 16, 108))
    fp = False
    rk = b.sig.ret
    if rk in ('float', 'double'):
        if top != ((top0 - 1) & 7):
            return 'differs', 'return: ours left no x87 result'
        if rres['push']:
            a = ld80(rres['st0'])
        elif rres.get('xmm0'):
            a = struct.unpack('<f' if rk == 'float' else '<d', rres['xmm0'][:4 if rk == 'float' else 8])[0]
        else:
            return 'differs', 'return: retail left no float result'
        c = ld80(sts[0])
        if rk == 'float':
            a, c = [struct.unpack('<f', struct.pack('<f', x))[0] if abs(x) < 3e38 else x for x in (a, c)]
        if not (a == c or (a != a and c != c)):
            if (rk == 'float' and near_float(struct.pack('<f', a), struct.pack('<f', c))) or                     (rk == 'double' and abs(a - c) <= 1e-12 * max(abs(a), abs(c))):
                fp = True
            else:
                return 'differs', 'return %s: retail %r, ours %r' % (rk, a, c)
    elif rk != 'void':
        mask = {'bool': 0xFF, 'char': 0xFF, 'short': 0xFFFF}.get(rk, 0xFFFFFFFF)
        if (eax ^ rres['eax']) & mask:
            return 'differs', 'return eax: retail %08x, ours %08x' % (rres['eax'] & mask, eax & mask)
        if rk == 'int64' and edx != rres['edx']:
            return 'differs', 'return edx: retail %08x, ours %08x' % (rres['edx'], edx)
    bd = delta(s, ram, con, esp0)
    rd = rres['delta']
    nsl = max([w + (sz + 3) // 4 for k, w, sz in mapping if k == 'stk'], default=0)
    for off, ri_, rv, bi, bv, base in ((0, rd[0], rd[1], bd[0], bd[1], s['ram']),
                                        (0x80000000, rd[2], rd[3], bd[2], bd[3], s['contig'])):
        if off == 0:   # A's incoming argument slots may be reused as scratch
            keep = (ri_ < esp0 + 4) | (ri_ >= esp0 + 4 + 4 * nsl)
            ri_, rv = ri_[keep], rv[keep]
            keep = (bi < esp0 + 4) | (bi >= esp0 + 4 + 4 * nsl)
            bi, bv = bi[keep], bv[keep]
        idx = np.union1d(ri_, bi)
        if not len(idx):
            continue
        rvals = base[idx].copy()
        rvals[np.searchsorted(idx, ri_)] = rv
        bvals = base[idx].copy()
        bvals[np.searchsorted(idx, bi)] = bv
        bad = idx[rvals != bvals]
        if not len(bad):
            continue
        words = sorted({int(x) & ~3 for x in bad})
        allok, anyfp = len(words) <= 4096, False
        for w in words if allok else []:
            rw, bw = word_at(base, ri_, rv, w), word_at(base, bi, bv, w)
            bp = struct.unpack('<I', bw)[0]
            if lo_b <= bp < hi_b and same_pointee(uc, bp, [struct.unpack('<I', rw)[0]]):
                continue     # a pointer to our copy of a literal that retail has at its own address
            if near_float(rw, bw):
                anyfp = True
                continue
            allok = False
            break
        if allok:
            fp = fp or anyfp
            continue
        a0 = int(bad[0])
        return 'differs', 'memory: %d bytes differ, first %08x (%s): retail %s ours %s; last %08x' % (
            len(bad), a0 + off, nearest(a0 + off), word_at(base, ri_, rv, a0 & ~3).hex(),
            word_at(base, bi, bv, a0 & ~3).hex(), int(bad[-1]) + off)
    return ('equivalent_fp' if fp else 'equivalent'), ''


# ---------------------------------------------------------------- driver


def retail_cached(path, root, va, size):
    os.makedirs(RCACHE, exist_ok=True)
    key = os.path.join(RCACHE, '%s__%s.pkl' % (os.path.relpath(root, common.SNAPS).replace(os.sep, '_').replace('/', '_'), os.path.basename(path)))
    if os.path.exists(key):
        try:
            return pickle.load(open(key, 'rb'))
        except Exception:
            pass
    s = snapstore.load(path, root)
    try:
        out = run_retail(s, va, size)
    except Fail as e:
        out = str(e)
    tmp = key + '.%d' % os.getpid()
    pickle.dump(out, open(tmp, 'wb'))
    os.replace(tmp, key)
    return out


def fmt_map(m):
    return ', '.join('p%d<-%s' % (i, w if k == 'reg' else 'stk%d' % w) for i, (k, w, sz) in enumerate(m))


def check(va, decomp, env, src=None, k=6, budget=240):
    fn = common.functions(decomp)
    r = fn.get(va)
    if r is None:
        return dict(va='%08x' % va, verdict='unbuildable', detail='not in functions.csv')
    size = r['size_i']
    t0 = time.time()
    res = dict(va='%08x' % va, status=r['status'], source=src or r['source'], verdict='', detail='', snaps=0, eq=0,
               mapping='', linked='')
    snaps = snapstore.index(va)[:k]
    res['snaps'] = len(snaps)
    try:
        if not (src or r['source']):
            raise Fail('unbuildable: no source')
        if src:
            out = os.path.join(common.WORK, 'tmpobj', '%08x_%d.obj' % (va, os.getpid()))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            objp = obj_for(os.path.abspath(src), decomp, env, fresh=True, out=out)
        else:
            objp = obj_for(r['source'], decomp, env)
        b = link(objp, va, size, decomp, env)
        res['linked'] = ' '.join(b.linked)
        if b.sig is None or b.sig.locations() is None:
            raise Fail('unbuildable: cannot read the signature of %s' % b.name)
        if not snaps:
            raise Fail('untestable: no snapshots (never reached in the rigs)')
        rr = []
        for p, root in snaps:
            x = retail_cached(p, root, va, size)
            rr.append((p, root, None if isinstance(x, str) else x, x if isinstance(x, str) else ''))
        good = [x for x in rr if x[2] is not None]
        if not good:
            raise Fail(rr[0][3])
        cands = candidates(b.sig, live_in(va, size))
        loaded = {}

        def snap(p, root):
            if p not in loaded:
                loaded[p] = snapstore.load(p, root)
            return loaded[p]
        first = chosen = None
        for m in cands:
            if first is not None and time.time() - t0 > budget:
                break
            verdicts, bad = [], None
            for p, root, x, why in good:   # a mapping must hold on every snapshot
                v, d = run_blob(snap(p, root), b, m, x[0], x[1])
                verdicts.append(v)
                if not v.startswith('equivalent'):
                    bad = (os.path.basename(p), d)
                    break
            if first is None or (bad and len(verdicts) > first[3]):
                first = (m, bad, verdicts, len(verdicts))
            if bad is None:
                chosen = (m, verdicts)
                break
        if chosen is None and first[1] and 'UNLINKED ' in first[1][1]:
            raise Fail('unbuildable: callee ' + first[1][1].split('UNLINKED ', 1)[1])
        if chosen is None:
            m, bad, verdicts, n = first
            res.update(verdict='differs', detail='%s: %s' % bad, mapping=fmt_map(m), tried=len(cands))
        else:
            m, verdicts = chosen
            res['mapping'] = fmt_map(m)
            res['eq'] = len(verdicts)
            res['verdict'] = 'equivalent_fp' if 'equivalent_fp' in verdicts else 'equivalent'
            res['detail'] = '%d/%d snapshots agree%s' % (res['eq'], len(rr), '' if len(good) == len(rr) else
                                                          ' (%d retail runs untestable)' % (len(rr) - len(good)))
    except Fail as e:
        res['verdict'], _, res['detail'] = str(e).partition(': ')
    res['t'] = round(time.time() - t0, 1)
    return res


def save(res):
    os.makedirs(RESULTS, exist_ok=True)
    tag = res['va'] + res.get('tag', '')
    json.dump(res, open(os.path.join(RESULTS, tag + '.json'), 'w'), indent=1)
    line = '%s %-13s %-7s %s  [%s] map(%s) %ss%s' % (res['va'], res['verdict'], res.get('status', ''), res['detail'],
                                                   res.get('source', ''), res.get('mapping', ''), res.get('t', ''),
                                                   (' linked: ' + res['linked']) if res.get('linked') else '')
    open(os.path.join(RESULTS, tag + '.txt'), 'w').write(line + '\n')
    return line


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vas', nargs='+')
    ap.add_argument('--decomp', default=common.DECOMP)
    ap.add_argument('--src')
    ap.add_argument('--k', type=int, default=6)
    ap.add_argument('--tag', default='')
    a = ap.parse_args()
    env = common.vcenv()
    for x in a.vas:
        res = check(int(x, 16), a.decomp, env, a.src, a.k)
        res['tag'] = a.tag
        print(save(res), flush=True)


if __name__ == '__main__':
    main()
