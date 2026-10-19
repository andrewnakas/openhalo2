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
import collections
import itertools
import json
import os
import pickle
import re
import struct
import subprocess
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
CAP = int(os.environ.get("FECHECK_CAP", 30_000_000))
BLOB_CAP = 8_000_000      # ours: a wrong entry mapping usually loops; keep it short
RESULTS = os.path.join(common.WORK, 'results')
DEBUG = bool(os.environ.get('FECHECK_DEBUG'))
# bump when retail recording changes (v6: entries before v6 lack the xmm call arguments, Rec.xlo)
RCACHE = os.path.join(common.WORK, 'rcache_v6')
REGS = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi']
UREG = {r: getattr(X, 'UC_X86_REG_' + r.upper()) for r in REGS + ['esp', 'ebp', 'eip']}
TRANSPARENT_NAMES = ['__chkstk', '_memcpy', '__allmul', '__aulldiv', '__alldiv', '__ftol2', '__aullshr', '__allshr',
                     '__allrem', '__allshl',
                     # XDK push-buffer routines retail's LTCG inlines into some callers: both sides run
                     # retail's own out-of-line copy, so its writes are compared like the caller's own
                     'D3DDevice_SetRenderState_ZEnable', 'D3DDevice_SetRenderState_Simple',
                     'D3DDevice_SetVertexShaderConstant1', 'D3DDevice_SetVertexShaderConstant1Fast',
                     'D3DDevice_SetVertexShaderConstant4', 'D3DDevice_SetVertexShaderConstantFast_Dwords',
                     'D3DDevice_SetRenderState_FillMode', 'D3DDevice_SetRenderState_CullMode', 'D3DDevice_SetStateBeforeBegin',
                     # CRT qsort sorts in place through a comparator: both sides run retail's copy (which calls
                     # the comparator each side passes), so the sorted array is compared like any other write
                     '_qsort']
# transparent routines that clobber ecx/edx like any cdecl call (the helpers above leave them alone)
TRANSPARENT_CLOBBER = {'_qsort'}
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


def _libm_sse2(ci, two):
    """VS2022 /arch:SSE2 math (__libm_sse2_<f>_precise): double x in xmm0 (y in xmm1), result in
    xmm0. Wraps the _CI x87 body: spill to the stack, load onto x87, run it, reload xmm0."""
    code = bytes.fromhex('83ec10' 'f20f110424')        # sub esp,16; movsd [esp],xmm0
    if two:
        code += bytes.fromhex('f20f114c2408')         # movsd [esp+8],xmm1
    code += bytes.fromhex('dd0424')                     # fld qword [esp]
    if two:
        code += bytes.fromhex('dd442408')             # fld qword [esp+8]: st0 = y, st1 = x as for _CI
    return code + BUILTINS[ci][:-1] + bytes.fromhex('dd1c24' 'f20f100424' '83c410' 'c3')  # fstp; movsd xmm0; add esp; ret


for _f, _two in (('sqrt', 0), ('sin', 0), ('cos', 0), ('tan', 0), ('acos', 0), ('asin', 0), ('atan', 0),
                 ('log', 0), ('log10', 0), ('exp', 0), ('atan2', 1), ('pow', 1)):
    BUILTINS['_libm_sse2_%s_precise' % _f] = _libm_sse2('_CI' + _f, _two)


def builtin_code(name):
    """A builtin's bytes. __libm_sse2_pow_precise runs retail's own _CIpow (x87 with every IEEE
    edge case: pow(0,y>0)=0, pow(x,0)=1, negative bases with integer y, infinities) when the symbol
    map knows it; transparent, so it runs for real as on retail's side."""
    if name == '_libm_sse2_pow_precise':
        ci = table().get('_CIpow')
        if ci is not None:
            return (bytes.fromhex('83ec10' 'f20f110424' 'f20f114c2408' 'dd0424' 'dd442408')   # st1 = x, st0 = y
                    + bytes([0xB8]) + struct.pack('<I', ci) + bytes.fromhex('ffd0')        # mov eax, _CIpow; call eax
                    + bytes.fromhex('dd1c24' 'f20f100424' '83c410' 'c3'))
    return BUILTINS[name]


class Fail(Exception):
    pass


# ---------------------------------------------------------------- kernel imports
# The recomp gives kernel thunk slot i the address 0xFE000000 + 4*i (xboxrecomp kernel_bridge.c, MIT);
# its stdcall_args_for_ordinal() table (with export names in comments) gives each ordinal's argument
# bytes. A kernel call here returns 0 and pops its arguments: retail and ours both see what retail's
# run recorded, so the comparison stays fair.
KVA = 0xFE000000
KBRIDGE = common.HALO2 + '/third_party/xboxrecomp/src/kernel/kernel_bridge.c'
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
_SRCNAME = None   # retail VA -> C++ name of its @retail marker
_NEAR = None


def table():
    global _TABLE, _SOURCES, _SRCNAME
    if _TABLE is None:
        _TABLE = symmap.load()
        _SOURCES, _SRCNAME = {}, {}
        for r in common.read_csv(os.path.join(common.RUNTIME, 'decomp_symbols.csv')):
            if r['source']:
                _SOURCES.setdefault(int(r['va'], 16), r['source'])
                if r['kind'] in ('function', 'method'):
                    _SRCNAME.setdefault(int(r['va'], 16), r['name'])
        for a, b in ALIASES.items():
            if b in _TABLE:
                _TABLE.setdefault(a, _TABLE[b])
    return _TABLE


_TRANSP = None


def transparent():
    global _TRANSP
    if _TRANSP is None:
        t = table()
        _TRANSP = {t[n] for n in TRANSPARENT_NAMES if n in t} | {v for n, v in t.items() if re.match(r'^_{0,2}CI[a-z0-9]+$', n)}
    return _TRANSP


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


def keep_obj(src, name, va, decomp, env):
    """Object of a wrapper TU that #includes src and takes the address of `name`: an `inline`
    function (or one only used inline in its own file) has no out-of-line copy in src's own object,
    and the real decomp build emits it through its stand-in callers (tools/build.py tu_source); a
    global holding its address does the same here. None when it does not build."""
    path = os.path.join(decomp, src)
    od = dbuild.objdir(decomp)
    stem = os.path.splitext(os.path.basename(path))[0]
    cpp = os.path.join(od, 'keep', '%s__%08x.cpp' % (stem, va))
    obj = cpp[:-4] + '.obj'
    os.makedirs(os.path.dirname(cpp), exist_ok=True)
    if os.path.exists(obj) and os.path.getmtime(obj) >= max(os.path.getmtime(path), dbuild.newest_header(decomp)):
        return obj
    open(cpp, 'w').write('/* fecheck: emit the inline %s from %s */\n#include "%s"\n'
                         'auto fe_keep_%08x = &%s;\n' % (name, src, os.path.abspath(path).replace('\\', '/'), va, name))
    _, ok, errs, obj = dbuild.compile_one(cpp, decomp, env, out=obj, flags_src=path)
    return obj if ok else None


_SSIZE = None


def struct_size(tname, decomp, env, src=None):
    """sizeof a structure the decomp passes by value, from a one-line probe compile against the
    header that defines it (the compile error names the array size), else against the source file
    src (sources declare some structures themselves). Cached in <WORK>; 0 if unknown."""
    global _SSIZE
    path = os.path.join(common.WORK, 'struct_sizes.json')
    if _SSIZE is None:
        _SSIZE = json.load(open(path)) if os.path.exists(path) else {}
    name = tname.split()[-1]
    if _SSIZE.get(name):
        return _SSIZE[name]
    skey = '%s@%s' % (name, os.path.basename(src)) if src and os.path.exists(src) else None
    if name in _SSIZE and skey is None:
        return 0
    if skey in _SSIZE:
        return _SSIZE[skey]
    probe = os.path.join(common.WORK, 'tmpobj', 'probe_%d.cpp' % os.getpid())
    os.makedirs(os.path.dirname(probe), exist_ok=True)
    size = 0
    if name not in _SSIZE:
        pat = re.compile(r'^\s*(struct|class|union)\s+%s\b[^;]*$' % re.escape(name), re.M)
        hdr = next((os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(decomp, 'include')) for f in fs
                    if f.endswith('.h') and pat.search(open(os.path.join(dp, f), errors='replace').read())), None)
        if hdr:
            open(probe, 'w').write('#include <xtl.h>\n#include "%s"\nchar (*fe_probe)[sizeof(%s)] = 1;\n'
                                   % (hdr.replace('\\', '/'), name))
            r = subprocess.run([common.CL] + dbuild.BASE + ['/I', common.SHIM, '/I', os.path.join(decomp, 'include'),
                                '/Fo' + probe + '.obj', probe], capture_output=True, text=True, env=env, errors='replace')
            m = re.search(r"'char \(\*\)\[(\d+)\]'", r.stdout)
            size = int(m.group(1)) if m else 0
        _SSIZE[name] = size
    if not size and skey:
        # declared in the source file itself: probe with that file included (front end only)
        open(probe, 'w').write('#include "%s"\nchar (*fe_probe)[sizeof(%s)] = 1;\n'
                               % (os.path.abspath(src).replace('\\', '/'), name))
        r = subprocess.run([common.CL] + dbuild.BASE + ['/Zs', '/I', common.SHIM, '/I', os.path.join(decomp, 'include'),
                            '/I', os.path.dirname(os.path.abspath(src)), probe],
                           capture_output=True, text=True, env=env, errors='replace', cwd=decomp)
        m = re.search(r"'char \(\*\)\[(\d+)\]'", r.stdout)
        size = int(m.group(1)) if m else 0
        _SSIZE[skey] = size
    try:
        json.dump(_SSIZE, open(path, 'w'))
    except OSError:
        pass
    return size


def sized(sig, decomp, env, src=None):
    """sig with by-value structure parameters given their real size (when the header or the source
    file src that declares it knows it)."""
    if sig is None or not sig.params or all(sz for pt, sz in sig.params):
        return sig
    ps = [(pt, sz or (struct_size(pt, decomp, env, src) if pt.startswith('struct') else 0)) for pt, sz in sig.params]
    out = coff.Sig(sig.cc, ps, sig.ret, sig.is_method, sig.varargs)
    out.cptr = getattr(sig, "cptr", frozenset())
    return out


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


def with_retbuf(sig):
    """The signature as compiled when it returns a class by value: a hidden result pointer after `this`
    (thiscall) or first, and the function returns that pointer in eax."""
    if sig is None or sig.params is None:
        return None
    ps = list(sig.params)
    at = 1 if sig.cc == 'thiscall' else 0
    ps.insert(at, ('ptr', 4))
    out = coff.Sig(sig.cc, ps, 'ptr', sig.is_method, sig.varargs)
    out.cptr = frozenset(i + (i >= at) for i in getattr(sig, 'cptr', ()))
    return out


def _code_sym(o, s):
    return s.section > 0 and s.sclass in (2, 3) and bool(o.sections[s.section].flags & coff.SCN_CODE) and not s.naux


def link(objp, va, size, decomp, env, force=(), src=None):
    """Link F (the function at va in objp, compiled from src) into a Blob at BLOB."""
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
        except (Fail, OSError):
            return None
        oo = get(p)
        s = next((s for s in oo.symbols if _code_sym(oo, s) and resolve(s.name) == v), None)
        if s is None and _SRCNAME.get(v):
            # an inline function: emitted only where it is used, so make a use
            p = keep_obj(os.path.join('src', src), _SRCNAME[v], v, decomp, env)
            if p:
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
            v = (table().get(t.name) if t.sclass == 3 else None) if t.section > 0 else resolve(t.name)
            if v is None and not (t.section > 0 and t.sclass == 3) and t.section > 0:
                v = resolve(t.name)
            if v == va and typ == coff.IMAGE_REL_I386_REL32 and t.section > 0 and is_code:
                # a self call: retail runs into its own body (not recorded as a call), so ours does too.
                # VS2022 may also turn it into a jmp to the entry (tail-recursion elimination).
                place(p, t.section)
                relocs.append(((p, si), off, typ, ('sec', (p, t.section), t.value)))
                continue
            if v is not None and typ == coff.IMAGE_REL_I386_REL32 and (v not in rc or v in force) and v not in transp and v != va:
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
                    b.callee_sig[v] = sized(coff.signature(t.name), decomp, env, src if p == objp else None)
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
                            b.data += builtin_code(bare)
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
                    b.data += builtin_code(bn)
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


def live_in(va, size, through=False):
    """Registers retail A reads before writing them on some path from its entry (its register
    arguments), in order of first read. Walks the control flow inside A.
    through: also registers A hands untouched to a direct callee that reads them (an argument
    passed straight through), appended after the others."""
    body = common.xbe_read(va, size) or b''
    insns = {i.address: i for i in md.disasm(body, va)}
    out, late, thru = [], [], []

    def passed(tgt, written):
        if not through or tgt is None or va <= tgt < va + size:
            return
        for x in callee_regs(tgt):
            if x not in written and x not in thru:
                thru.append(x)
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
                if mn == 'ret' and 'eax' not in written and 'eax' not in out:
                    late.append('eax')     # returned untouched: an argument passed through
                break
            try:
                rd, wr = ins.regs_access()
            except Exception:
                break
            rd = {PARTS.get(md.reg_name(r), md.reg_name(r)) for r in rd}
            wr = {PARTS.get(md.reg_name(r), md.reg_name(r)) for r in wr}
            ops = ins.operands
            if mn in ('xor', 'sub', 'sbb', 'xorps', 'xorpd', 'pxor', 'subps') and len(ops) == 2 and ops[0].type == ops[1].type == CX.X86_OP_REG and ops[0].reg == ops[1].reg:
                rd = set()
            if mn in ('or', 'and') and len(ops) == 2 and ops[1].type == CX.X86_OP_IMM and (ops[1].imm & 0xFFFFFFFF) in ((0xFFFFFFFF,) if mn == 'or' else (0,)):
                rd = set()
            if mn in ('mov', 'lea') and len(ops) == 2 and ops[0].type == ops[1].type == CX.X86_OP_REG and ops[0].reg == ops[1].reg:
                rd = wr = set()   # mov edi, edi: padding (neither reads nor writes the value)
            if mn == 'push' and ops and ops[0].type == CX.X86_OP_REG and md.reg_name(ops[0].reg) in ('ebx', 'esi', 'edi', 'ebp'):
                rd = set()
            for r in REGS + XREGS:
                if r in rd and r not in written and r not in out:
                    out.append(r)
            written = written | (wr & set(REGS + XREGS))
            if mn == 'call':
                tgt_ = ops[0].imm & 0xFFFFFFFF if ops and ops[0].type == CX.X86_OP_IMM else None
                passed(tgt_, written)
                eax_only = tgt_ is not None and tgt_ in transparent() and tgt_ not in {table().get(n) for n in TRANSPARENT_CLOBBER}
                written = written | ({'eax'} if eax_only else {'eax', 'ecx', 'edx'})
            nxt = addr + ins.size
            if mn.startswith('j'):
                op = ops[0] if ops else None
                tgt = op.imm & 0xFFFFFFFF if op is not None and op.type == CX.X86_OP_IMM else None
                if mn == 'jmp':
                    if tgt is None and op is not None and op.type == CX.X86_OP_MEM and op.mem.scale == 4 and op.mem.disp:
                        for k in range(64):       # a jump table: dd case targets
                            w = common.xbe_read((op.mem.disp + 4 * k) & 0xFFFFFFFF, 4)
                            tv = struct.unpack('<I', w)[0] if w else 0
                            if not va <= tv < va + size:
                                break
                            work.append((tv, written))
                        break
                    if tgt is None or not va <= tgt < va + size:
                        passed(tgt, written)   # a tail call
                        break
                    addr = tgt
                    continue
                if tgt is not None and va <= tgt < va + size:
                    work.append((tgt, written))
            addr = nxt
    out = out + [x for x in late if x not in out]
    return out + [x for x in thru if x not in out]


def candidates(sig, live, cap=400):
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


def aligned_copy(a):
    """A page-aligned copy with a guard margin: Unicorn maps the host buffer directly."""
    raw = np.zeros(len(a) + 3 * 4096, dtype=np.uint8)
    off = (-raw.ctypes.data) % 4096
    out = raw[off:off + len(a)]
    out[:] = a
    return out


def machine(s):
    ram = aligned_copy(s['ram'])
    con = aligned_copy(s['contig'])
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
                 'st0', 'writes', 'top0', 'xmm0', 'xlo')

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
        if not ins:
            return
        mn = ins.mnemonic
        if mn not in ('call', 'jmp'):
            if not (mn in JCC and ins.operands and ins.operands[0].type == CX.X86_OP_IMM):
                return
            t = ins.operands[0].imm & 0xFFFFFFFF
            if va <= t < va + size or not jcc_taken(mn, u.reg_read(X.UC_X86_REG_EFLAGS), u.reg_read(X.UC_X86_REG_ECX)):
                return
        t = target_of(u, ins)
        if mn != 'call' and va <= t < va + size:
            return
        if t in transp or (t == va and mn == 'call'):
            return     # a self call runs on, recorded as A's own calls (ours runs its own compile)
        r = Rec()
        r.target, r.tail = t, mn != 'call'
        r.indirect = ins.operands[0].type != CX.X86_OP_IMM
        r.regs = [u.reg_read(UREG[x]) for x in REGS]
        r.xlo = [u.reg_read(getattr(X, 'UC_X86_REG_XMM%d' % i)) & 0xFFFFFFFF for i in range(8)]
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


_CREGS = {}


def callee_regs(va):
    """Register arguments of a retail function (its live-in registers), cached."""
    if va not in _CREGS:
        r = common.functions().get(va) if va < KVA else None
        _CREGS[va] = [x for x in live_in(va, r['size_i']) if x in REGS] if r else []
    return _CREGS[va]


_NARGS = {}


def callee_nargs(va):
    """Dwords of arguments retail function va can read: its live-in registers plus the stack bytes
    every one of its `ret N` pops. None when not provable (caller-cleaned, mixed or no ret N)."""
    if va not in _NARGS:
        r = common.functions().get(va) if va < KVA else None
        n = None
        if r:
            body = common.xbe_read(va, r['size_i']) or b''
            pops = {(i.operands[0].imm if i.operands else 0) for i in md.disasm(body, va) if i.mnemonic == 'ret'}
            if len(pops) == 1 and min(pops) > 0:
                n = len(callee_regs(va)) + min(pops) // 4
        _NARGS[va] = n
    return _NARGS[va]


JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp', 'jl', 'jge', 'jle', 'jg', 'jecxz'}


def jcc_taken(mn, fl, ecx):
    cf, pf, zf, sf, of = fl & 1, (fl >> 2) & 1, (fl >> 6) & 1, (fl >> 7) & 1, (fl >> 11) & 1
    return {'jo': of, 'jno': not of, 'jb': cf, 'jae': not cf, 'je': zf, 'jne': not zf, 'jbe': cf or zf,
            'ja': not (cf or zf), 'js': sf, 'jns': not sf, 'jp': pf, 'jnp': not pf, 'jl': sf != of,
            'jge': sf == of, 'jle': zf or sf != of, 'jg': not zf and sf == of, 'jecxz': (ecx & 0xFFFFFFFF) == 0}[mn]


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


PAIR_CAP = 24


def fp_pairings(u, r, t, direct, sig, cl, fregs, esp, r_hi, b_hi, args):
    """Frame-pointer pairings for one mocked call. Returns (sound, candidates, val) or None
    when no callee write (or returned pointer) lands in retail's frame.

    sound: retail's callee reads exactly the registers our declaration passes and pops the
    same stack bytes, so pairing by argument position is the truth (no alternatives).
    candidates: [(locmap, {retail fp: ours fp})], every injective assignment of the retail
    frame pointers the callee's writes are based on to the frame pointers ours passes, whose
    written extent fits before ours' next frame pointer. A locmap names argument locations
    (('r', reg) / ('s', retail stack index | ours byte offset)), so a choice made at one call
    site carries over to its other invocations and to other snapshots."""
    regs = callee_regs(t) if direct else []
    if not regs and not direct:
        regs = ['ecx']                       # a method call through a vtable: `this`
    ours_stk = [(where, sz) for kind, where, sz, pt in (cl or []) if kind == 'stk']
    n_r = max((r.popped or 0) // 4, max([(w + 4) // 4 for w, sz in ours_stk], default=0) if cl is not None
              else len(args), 0)
    rlocs = [('r', x) for x in regs if x in REGS] + [('s', i) for i in range(min(n_r, len(r.stack)))]
    ro = set()
    if cl is not None:
        olocs = [('r', w) if k == 'reg' else ('s', w) for k, w, sz, pt in cl if k == 'reg' and w in REGS or k == 'stk' and sz <= 4]
        ro = {('r', w) if k == 'reg' else ('s', w) for i, (k, w, sz, pt) in enumerate(cl) if i in getattr(sig, 'cptr', ())}
    else:
        olocs = [('s', 4 * i) for i in range(len(args))] + [('r', 'ecx'), ('r', 'edx')]

    oidx = {l: i for i, l in enumerate(olocs)}

    def rval(l):
        v = r.regs[REGS.index(l[1])] if l[0] == 'r' else r.stack[l[1]]
        return v if r.esp_call <= v < r_hi else None

    def oval(l):
        v = fregs[l[1]] if l[0] == 'r' else struct.unpack('<I', u.mem_read(esp + l[1], 4))[0]
        return v if esp <= v < b_hi else None

    def val(rl, ol):
        return rval(rl), oval(ol)
    rv = {}
    for l in rlocs:
        v = rval(l)
        if v is not None and v not in rv:
            rv[v] = l
    ov = {}
    for l in olocs:
        v = oval(l)
        if v is not None and v not in ov:
            ov[v] = l
    if not rv:
        return None
    rk = sorted(rv)
    ext = {}
    for a, data in r.writes:
        if r.esp_call <= a < r_hi:
            p_ = max((p for p in rk if p <= a), default=None)
            if p_ is not None:
                ext[p_] = max(ext.get(p_, 0), a + len(data) - p_)
    if r.esp_call <= r.eax < r_hi:
        p_ = max((p for p in rk if p <= r.eax), default=None)
        if p_ is not None:
            ext.setdefault(p_, 1)
    if not ext:
        return None
    sound = (direct and sig is not None and cl is not None and (r.popped or 0) == (sig.stack_bytes() or 0)
             and set(regs) & set(REGS) == {w for k, w, sz, pt in cl if k == 'reg'})
    need = sorted(ext)
    ok = sorted(ov)
    if DEBUG:
        print('    fp_pairings %s sound=%s need %s ours %s' % (nearest(t), sound, {rv[n]: (hex(n), hex(ext[n])) for n in need},
              {ov[o]: hex(o) for o in ok}))
    room = {v: min([w for w in ok if w > v] + [b_hi]) - v for v in ok}
    cands = []
    for perm in itertools.permutations(ok + [None] * max(0, len(need) - len(ok)), len(need)):
        if any(o is not None and (room[o] < ext[n] or ov[o] in ro) for n, o in zip(need, perm)):
            continue
        pm = {n: o for n, o in zip(need, perm) if o is not None}
        if not pm:
            continue
        lm = tuple((rv[n], ov[o]) for n, o in pm.items())
        # LTCG moves arguments into registers but never reorders the ones it leaves on the stack:
        # retail stack arguments keep our parameter order
        st_ = sorted((rl[1], oidx[ol]) for rl, ol in lm if rl[0] == 's')
        if any(st_[i][1] > st_[i + 1][1] for i in range(len(st_) - 1)):
            continue
        if all(c[0] != lm for c in cands):
            cands.append((lm, pm))
        if len(cands) >= PAIR_CAP:
            break
    return sound, cands, val


OBS = {}               # callee VA -> call-site observations (observe) for learn_callee


def observe(u, t, cl, r, fregs, esp, b_hi, r_hi, lo_b, hi_b, fmap):
    """What one mocked call tells about callee t's retail convention: per parameter, our value
    and whether it is one of our frame pointers; retail's registers, stack words and frame.
    Grouped by F's entry mapping (fmap): under a wrong one F passes wrong values."""
    obs = OBS.setdefault(t, {}).setdefault(fmap, [])
    if len(obs) >= 6 or len(OBS[t]) > 24:
        return
    vals = []
    for kind, where, sz, pt in cl:
        v = fregs[where] if kind == 'reg' else struct.unpack('<I', u.mem_read(esp + where, 4))[0]
        vals.append((v, esp <= v < b_hi, lo_b <= v < hi_b))
    key = (tuple(r.regs), tuple(r.stack[:16]), tuple(vals))
    if any(o['key'] == key for o in obs):
        return
    obs.append(dict(key=key, vals=vals, regs=list(r.regs), stack=list(r.stack), xlo=list(r.xlo or []),
                    lo=r.esp_call, hi=r_hi, popped=r.popped))


UNSURE = ('unsure',)   # info key: callees met at uncertain sites whose convention is not known yet
MAPS = {}              # callee VA -> [[(kind, where) per parameter]] retail conventions ([] = none found)


def known_pairings(t, cl, r, val):
    """Frame-pointer pairings from the callee's known retail convention: retail's value in the
    location of parameter i pairs with ours for parameter i. [(locmap, {retail fp: ours fp})] or
    None when no known convention fits this call."""
    out = []
    for km in MAPS.get(t) or []:
        if len(km) != len(cl):
            continue
        lm, pm = [], {}
        for e, (ok_, ow, sz, pt) in zip(km, cl):
            if e is None:
                continue
            rk, rw = e
            if sz > 4 or (rk == 'reg' and rw not in REGS) or (ok_ == 'reg' and ow not in REGS):
                continue
            if rk == 'stk' and rw >= len(r.stack):
                continue
            rl, ol = ('r', rw) if rk == 'reg' else ('s', rw), ('r', ow) if ok_ == 'reg' else ('s', ow)
            a, b_ = val(rl, ol)
            if a is None or b_ is None or a in pm:
                continue
            lm.append((rl, ol))
            pm[a] = b_
        if pm and all(c[0] != tuple(lm) for c in out):
            out.append((tuple(lm), pm))
    return out or None


def run_blob(s, b, mapping, rres, recs, alt=None, info=None):
    """alt: call site -> frame-pointer pairing to use there (see fp_pairings); info: dict that
    collects, per call site whose positional pairing is not certain, the alternative pairings."""
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
    # Run ours at retail's own stack pointer when its stack arguments sit where retail's do: then
    # uninitialised locals hold the same stale bytes in both runs (sources copy whole structures).
    # Otherwise give our frame a shifted copy of retail's stack.
    same = all((k == 'stk' and l[0] == 'stk' and l[1] == 4 * w) or (k == 'reg' and l[0] == 'reg')
               for (k, w, sz), l in zip(mapping, b.sig.locations()))
    if same:
        new_esp = esp0
        uc.mem_write(new_esp, struct.pack('<I', SENT))
    else:
        uc.mem_write(new_esp - 0x8000, bytes(uc.mem_read(esp0 - 0x8000, 0x8000)))
        uc.mem_write(new_esp, struct.pack('<I', SENT) + bytes(stk) + bytes(64))
    bsl = (len(stk) + 3) // 4
    rsl = max([w + (sz + 3) // 4 for k, w, sz in mapping if k == 'stk'], default=0)
    uc.reg_write(X.UC_X86_REG_ESP, new_esp)
    transp = transparent()
    st = {'ri': 0, 'fail': None, 'n': 0, 'pend': None, 'stub': 0, 'fp': False}
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
        if not ins:
            return
        mn = ins.mnemonic
        if mn not in ('call', 'jmp'):
            if not (mn in JCC and ins.operands and ins.operands[0].type == CX.X86_OP_IMM):
                return
            t = ins.operands[0].imm & 0xFFFFFFFF
            if lo_b <= t < hi_b or not jcc_taken(mn, u.reg_read(X.UC_X86_REG_EFLAGS), u.reg_read(X.UC_X86_REG_ECX)):
                return     # a conditional tail call (jne function) that is taken: handled like jmp
        t = target_of(u, ins)
        if lo_b <= t < hi_b or t in transp:
            return
        tail = mn != 'call'
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
        if sig is not None and sig.ret == 'struct':
            sr = with_retbuf(sig)
            # a method returning a class always takes the hidden result pointer in our compile
            # (retail's LTCG pops can't tell: it may pass it in a register)
            use = sr is not None and (sig.is_method or not sig.callee_pops()
                                      or (r.popped or 0) == (sr.stack_bytes() or 0)
                                      or (sr.stack_bytes() or 0) == (sig.stack_bytes() or 0))
            if sr is not None and not sig.is_method:
                # a small POD comes back in eax without a result pointer: when retail's pops only
                # agree by coincidence (another convention), the snapshots decide
                rbk = ('rb', addr - lo_b, t)
                if alt and alt.get(rbk):
                    use = not use
                elif info is not None and rbk not in info:
                    info[rbk] = [True]
            if use:
                sig = sr
        cl = sig.locations() if sig else None
        if cl is None and sig is not None and sig.params and sig.callee_pops():
            unk = [i for i, (pt, sz) in enumerate(sig.params) if sz == 0]
            if len(unk) == 1:
                ps = list(sig.params)
                ps[unk[0]] = (ps[unk[0]][0], 4)
                s4 = coff.Sig(sig.cc, ps, sig.ret, sig.is_method, sig.varargs)
                guess = 4 + (r.popped or 0) - (s4.stack_bytes() or 0)
                if 0 < guess <= 64 and guess % 4 == 0:
                    ps[unk[0]] = (ps[unk[0]][0], guess)
                    sig = coff.Sig(sig.cc, ps, sig.ret, sig.is_method, sig.varargs)
                    cl = sig.locations()
        fregs = {x: u.reg_read(UREG[x]) for x in REGS}
        args, masks, fidx = [], [], set()
        if cl is not None:
            for kind, where, sz, pt in cl:
                if pt == 'float' and sz == 4:
                    fidx.add(len(args))
                if kind == 'reg':
                    args.append(fregs[where])
                    masks.append(MASKS.get(pt, 0xFFFFFFFF))
                else:
                    w_ = list(struct.unpack('<%dI' % ((sz + 3) // 4), u.mem_read(esp + where, (sz + 3) & ~3)))
                    args += w_
                    if pt.startswith('struct') and sz < 4:
                        # a by-value structure smaller than a dword: only sizeof bytes are the value
                        # (retail pushes a register and stores the byte: the rest is garbage)
                        masks.append((1 << (8 * sz)) - 1)
                    else:
                        masks += [MASKS.get(pt, 0xFFFFFFFF)] * len(w_)
        else:
            # retail's pops are exact for callee-cleaned calls (thiscall/stdcall/fastcall); an
            # indirect call that popped nothing is a virtual call without stack arguments.
            # Only a direct cdecl-looking call (pops 0) keeps the 4-word guess.
            if r.popped:
                n = r.popped // 4
            elif not direct and r.popped == 0:
                n = 0
            else:
                n = 0 if direct and sig is not None else 4
            n = min(n, len(r.stack))
            args = list(struct.unpack('<%dI' % n, u.mem_read(esp, 4 * n))) if n else []
        rset = set(r.regs) | set(r.stack) | set(getattr(r, 'xlo', None) or [])
        r_hi, b_hi = esp0 + 4 + 4 * rsl, new_esp + 4 + 4 * bsl   # frames incl. incoming argument slots
        # frame pointers retail really passed: the callee's live-in registers and the stack bytes it popped
        r_args = [r.regs[REGS.index(x)] for x in callee_regs(t)] + r.stack[:max(0, (r.popped or 0) // 4)]
        r_fp = fptrs(r_args, r.esp_call, r_hi) or fptrs(r.regs + r.stack, r.esp_call, r_hi)
        b_fp = fptrs(args if cl is not None else args + [fregs['ecx'], fregs['edx']], esp, b_hi)
        # LTCG drops parameters retail passes as constants (a NULL file, line 0): when retail's callee
        # provably reads fewer argument dwords than ours passes, that many zero arguments need no match
        nr = callee_nargs(t) if direct and cl is not None else None
        spare = max(0, len(args) - nr) if nr is not None else 0
        for i, v in enumerate(args):
            if v in rset or (esp <= v < b_hi and r_fp):
                continue
            if v == 0 and spare:
                spare -= 1
                continue
            mk = masks[i] if i < len(masks) else 0xFFFFFFFF
            if mk != 0xFFFFFFFF and any((v ^ x) & mk == 0 for x in rset):
                continue   # a bool/char/short: only its own width is the value
            if lo_b <= v < hi_b and same_pointee(u, v, r.regs + r.stack):
                continue   # our copy of a literal / constant: same bytes as what retail passed
            if i in fidx and any(near_float(struct.pack('<I', v), struct.pack('<I', x)) for x in rset):
                st['fp'] = True   # a float argument a few ulps off (x87 vs SSE rounding): equivalent_fp at best
                continue
            return stop('call %d to %s: argument %d = %08x, retail passed no such value (retail regs %s; stack %s)' % (
                st['n'], nearest(t), i, v, ' '.join('%08x' % x for x in r.regs), ' '.join('%08x' % x for x in r.stack[:6])))
        kmatch = None
        if direct and cl is not None:
            # the callee's retail convention is known: each argument must be the value retail passed
            # in that parameter's place (the check above only asks for it somewhere)
            kms = [km for km in (MAPS.get(t) or entry_rows(t)) if len(km) == len(cl)]
            miss = None
            for km in kms:
                miss, ai = None, 0
                for i, (e, (kind, where, sz, pt)) in enumerate(zip(km, cl)):
                    k0, ai = ai, ai + (1 if kind == 'reg' else (sz + 3) // 4)
                    if e is None or sz > 4 or (e[0] == 'reg' and e[1] not in REGS) or (e[0] == 'stk' and e[1] >= len(r.stack)):
                        continue
                    v, rv = args[k0], r.regs[REGS.index(e[1])] if e[0] == 'reg' else r.stack[e[1]]
                    mk = masks[k0] if k0 < len(masks) else 0xFFFFFFFF
                    if (v ^ rv) & mk == 0 or (esp <= v < b_hi and r.esp_call <= rv < r_hi):
                        continue   # equal, or a frame pointer each side (paired below)
                    if lo_b <= v < hi_b and same_pointee(u, v, [rv]):
                        continue
                    if k0 in fidx and near_float(struct.pack('<I', v), struct.pack('<I', rv)):
                        st['fp'] = True
                        continue
                    miss = (i, v, rv, e)
                    break
                if miss is None:
                    kmatch = km
                    break
            if kms and miss is not None:
                i, v, rv, e = miss
                return stop('call %d to %s: argument %d = %08x, retail passed %08x there (%s, its retail convention)' % (
                    st['n'], nearest(t), i, v, rv, e[1] if e[0] == 'reg' else 'stk%d' % e[1]))
        # frame pointers retail passed -> ours: by argument position when both sides pass the same
        # stack layout (and ecx/edx), else in address order
        pairs = {}
        if kmatch is not None:
            # ... and pairs its frame pointers parameter by parameter (the truth, no alternatives)
            ai = 0
            for e, (kind, where, sz, pt) in zip(kmatch, cl):
                k0, ai = ai, ai + (1 if kind == 'reg' else (sz + 3) // 4)
                if e is None or sz > 4 or (e[0] == 'reg' and e[1] not in REGS) or (e[0] == 'stk' and e[1] >= len(r.stack)):
                    continue
                rv_ = r.regs[REGS.index(e[1])] if e[0] == 'reg' else r.stack[e[1]]
                if r.esp_call <= rv_ < r_hi and esp <= args[k0] < b_hi and rv_ not in pairs:
                    pairs[rv_] = args[k0]
        elif cl is not None and sig is not None and (r.popped or 0) == (sig.stack_bytes() or 0):
            for kind, where, sz, pt in cl:
                rv_ = r.regs[REGS.index(where)] if kind == 'reg' else r.stack[where // 4] if where // 4 < len(r.stack) else None
                bv_ = fregs[where] if kind == 'reg' else struct.unpack('<I', u.mem_read(esp + where, 4))[0]
                if rv_ is not None and r.esp_call <= rv_ < r_hi and esp <= bv_ < b_hi:
                    pairs[rv_] = bv_
        if not pairs:
            pairs = dict(zip(r_fp, b_fp))
        if (info is not None or alt) and not (kmatch is not None and pairs):
            site = (addr - lo_b, t)
            pr = fp_pairings(u, r, t, direct, sig, cl, fregs, esp, r_hi, b_hi, args)
            if pr is not None:
                sound, cands, val = pr
                if site in (alt or {}):
                    got = {}
                    for rl, ol in alt[site]:
                        rv_, bv_ = val(rl, ol)
                        if rv_ is not None and bv_ is not None and rv_ not in got:
                            got[rv_] = bv_
                    if got:
                        pairs = got
                elif info is not None and not sound:
                    kl = known_pairings(t, cl, r, val) if direct and cl is not None else None
                    if kl is None:
                        if direct and cl is not None and t not in MAPS:
                            info.setdefault(UNSURE, set()).add(t)   # its convention may be learnable
                            observe(u, t, cl, r, fregs, esp, b_hi, r_hi, lo_b, hi_b, tuple(mapping))
                        cs = [c for c in cands if {k: v for k, v in pairs.items() if k in c[1]} != c[1]]
                    else:
                        # the callee's retail convention is known: pair parameter by parameter, and
                        # only that way (an open search could hide a source that swaps outputs)
                        cs = [c for c in kl if {k: v for k, v in pairs.items() if k in c[1]} != c[1]]
                    if cs and site not in info:
                        info[site] = [c[0] for c in cs]
        pk = sorted(pairs)
        if DEBUG:
            print('  mock %d %s args %s r_fp %s b_fp %s pairs %s pops %s/%s' % (st['n'], nearest(t), [hex(x) for x in args],
                  [hex(x) for x in r_fp], [hex(x) for x in b_fp], {hex(k): hex(v) for k, v in pairs.items()}, r.popped,
                  sig.stack_bytes() if sig else None))
        for a, data in r.writes:
            if r.esp_call - 0x40000 <= a < r.esp_call:
                continue                       # the callee's own stack
            if r.esp_call <= a < r_hi:         # retail's frame -> ours
                # a merged write can span several paired pointers (adjacent out-variables):
                # each piece goes through the pointer it starts at or after
                cuts = [a] + [p for p in pk if a < p < a + len(data)] + [a + len(data)]
                for lo, hi in zip(cuts, cuts[1:]):
                    p_ = max((p for p in pk if p <= lo), default=None)
                    if p_ is not None:
                        u.mem_write(lo - p_ + pairs[p_], data[lo - a:hi - a])
                continue
            u.mem_write(a, data)
        if sig is not None and cl is not None:
            pops = sig.stack_bytes() if sig.callee_pops() else 0
            pass
        else:
            pops = r.popped or 0
        reax = r.eax
        # a bool/char/short result is only its low bits: the rest of eax is whatever retail's callee
        # left there, never a pointer into the frame to carry over to ours
        narrow = sig is not None and sig.ret in ('bool', 'char', 'short')
        if pk and not narrow and r.esp_call <= reax < r_hi:
            p_ = max((p for p in pk if p <= reax), default=None)
            if p_ is not None:
                reax = reax - p_ + pairs[p_]
        code = bytes([0xB8]) + struct.pack('<I', reax) + bytes([0xBA]) + struct.pack('<I', r.edx)
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
    fp = st['fp']
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
        badw = None
        for w in words if allok else []:
            rw, bw = word_at(base, ri_, rv, w), word_at(base, bi, bv, w)
            bp = struct.unpack('<I', bw)[0]
            if lo_b <= bp < hi_b and same_pointee(uc, bp, [struct.unpack('<I', rw)[0]]):
                continue     # a pointer to our copy of a literal that retail has at its own address
            if near_float(rw, bw):
                anyfp = True
                continue
            allok = False
            badw = w
            break
        if allok:
            fp = fp or anyfp
            continue
        a0 = int(bad[0]) if badw is None else badw   # the first word that is not a near-equal float
        return 'differs', 'memory: %d bytes differ, first %08x (%s): retail %s ours %s; last %08x' % (
            len(bad), a0 + off, nearest(a0 + off), word_at(base, ri_, rv, a0 & ~3).hex(),
            word_at(base, bi, bv, a0 & ~3).hex(), int(bad[-1]) + off)
    return ('equivalent_fp' if fp else 'equivalent'), ''


# ---------------------------------------------------------------- driver


RCACHE_FORMAT = 7   # 7: self calls are not recorded (they run on). bump with any change to what run_retail records


def rcache_format():
    """The recording format of a retail cache entry: the format number plus the transparent
    routines (they run for real and are never recorded as calls)."""
    import hashlib
    h = hashlib.sha1(repr((sorted(TRANSPARENT_NAMES), sorted(ALIASES.items()), sorted(transparent()))).encode()).hexdigest()[:12]
    return '%d:%s' % (RCACHE_FORMAT, h)


def retail_cached(path, root, va, size):
    os.makedirs(RCACHE, exist_ok=True)
    key = os.path.join(RCACHE, '%s__%s.pkl' % (os.path.relpath(root, common.SNAPS).replace(os.sep, '_').replace('/', '_'), os.path.basename(path)))
    # a store re-captured later reuses snapshot names: a cache entry older than its snapshot is stale
    if os.path.exists(key) and os.path.getmtime(key) >= os.path.getmtime(path):
        try:
            got = pickle.load(open(key, 'rb'))
            # an entry recorded under another recording format (or another set of routines that
            # run for real instead of being recorded as calls) is rerun
            if isinstance(got, tuple) and len(got) == 3 and got[0] == 'fecheck-rcache' and got[1] == rcache_format():
                return got[2]
        except Exception:
            pass
    s = snapstore.load(path, root)
    try:
        out = run_retail(s, va, size)
    except Fail as e:
        out = str(e)
    tmp = key + '.%d' % os.getpid()
    pickle.dump(('fecheck-rcache', rcache_format(), out), open(tmp, 'wb'))
    os.replace(tmp, key)
    return out


_KNOWN = None


def known_maps(va, sig):
    """Entry maps read off retail's disassembly (<WORK>/entry_maps.csv: va,map,evidence with map as
    fmt_map prints it). Tried before the generic search; the snapshots still decide."""
    global _KNOWN
    if _KNOWN is None:
        _KNOWN = collections.defaultdict(list)
        for row in common.read_csv(os.path.join(common.WORK, 'entry_maps.csv')):
            try:
                _KNOWN[int(row['va'], 16)].append(row['map'])
            except (KeyError, ValueError):
                pass
    loc = sig.locations() if sig else None
    out = []
    for text in _KNOWN.get(va, []):
        parts = [p.strip().split('<-') for p in text.split(',') if p.strip()]
        if loc is None or len(parts) != len(loc):
            continue
        m = []
        for (pi, w), (kind, where, sz, pt) in zip(parts, loc):
            m.append(('stk', int(w[3:]), sz) if w.startswith('stk') else ('reg', w, sz))
        out.append(m)
    return out


def parse_map(text):
    """'p0<-ecx, p1<-stk0' (fmt_map) -> [('reg', 'ecx'), ('stk', 0)]."""
    out = []
    for part in text.replace('+retbuf', '').split(','):
        part = part.strip()
        if not part:
            continue
        w = part.split('<-')[1].strip()
        out.append(('stk', int(w[3:])) if w.startswith('stk') else ('reg', w))
    return out


def map_pops(km, loc):
    """Stack bytes a callee with retail convention km (sizes from loc) pops."""
    return 4 * max([e[1] + (l[2] + 3) // 4 for e, l in zip(km, loc) if e and e[0] == 'stk'], default=0)


_RESMAPS = {}


def entry_rows(t):
    """t's maps in entry_maps.csv (read off retail's disassembly: trusted as the convention)."""
    known_maps(t, None)
    out = []
    for text in _KNOWN.get(t, []):
        km = parse_map(text)
        if km not in out:
            out.append(km)
    return out


def result_maps(t):
    """Entry maps of t that a check found equivalent (any tree). Only candidates: a check takes the
    first mapping that agrees, and a parameter its runs never tell apart may sit anywhere."""
    if t not in _RESMAPS:
        import glob
        seen = []
        for f in sorted(glob.glob(os.path.join(RESULTS, '%08x*.json' % t)), key=os.path.getmtime, reverse=True):
            try:
                d = json.load(open(f))
            except (OSError, ValueError):
                continue
            if d.get('va') == '%08x' % t and str(d.get('verdict', '')).startswith('equivalent') and d.get('mapping'):
                km = parse_map(d['mapping'])
                if km not in seen:
                    seen.append(km)
        _RESMAPS[t] = seen
    return _RESMAPS[t]


def call_states(s, va, size, t, n):
    """Machine states at the entry of retail's direct calls from A (va) to t, from retail A's run
    on snapshot s: snapshot-like dicts the callee can be checked on (at most n)."""
    uc, ram, con = machine(s)
    esp0 = s['esp']
    uc.mem_write(esp0, struct.pack('<I', SENT))
    out = []

    def on_t(u, addr, size_, d):
        esp = u.reg_read(X.UC_X86_REG_ESP)
        if not va <= u32(u, esp) < va + size:
            return                       # a nested call, not one A makes
        ds = {r: u.reg_read(UREG[r]) for r in REGS + ['ebp', 'esp']}
        ds['xmm'] = [u.reg_read(getattr(X, 'UC_X86_REG_XMM%d' % i)).to_bytes(16, 'little') for i in range(8)]
        ds['fpcw'] = u.reg_read(X.UC_X86_REG_FPCW)
        ds['fs_base'] = s['fs_base']
        ds['ram'] = np.array(ram[:len(s['ram'])])
        ds['contig'] = np.array(con[:len(s['contig'])])
        ds['path'] = '%s@%08x#%d' % (s.get('path', ''), t, len(out))
        out.append(ds)
        if len(out) >= n:
            u.emu_stop()
    uc.hook_add(UC_HOOK_CODE, on_t, begin=t, end=t)
    try:
        uc.emu_start(va, SENT_END, timeout=60_000_000, count=CAP)
    except UcError:
        pass
    return out


LEARN_DIR = os.path.join(common.WORK, 'callee_maps')
FAR = 24   # stack slot given to a parameter retail does not pass


def site_candidates(t, sig, P, live, cap=3000):
    """Retail conventions for callee t (sig = its declaration) consistent with the calls observed
    (OBS[t]): a parameter whose value is not one of our frame pointers sits where retail held the
    same value (its own width for bool/char/short), a frame pointer where retail held one of its
    frame pointers; stack parameters keep their order and fill exactly the P bytes retail's t pops
    (P None: unknown); registers among t's live-in ones. A parameter nothing fits is not passed
    (a far stack slot: it must not matter)."""
    loc = sig.locations()
    if loc is None:
        return []
    out = []
    for obs in (OBS.get(t) or {}).values():   # one entry mapping of F at a time
        for m in _site_candidates(obs, loc, P, live, cap):
            if m not in out:
                out.append(m)
            if len(out) >= cap:
                return out
    return out


def _site_candidates(obs, loc, P, live, cap):
    nslot = P // 4 if P else None
    allowed = []
    for i, (kind, where, sz, pt) in enumerate(loc):
        if sz > 4:
            allowed.append(None)          # stack only, the next slot
            continue
        mask = MASKS.get(pt, 0xFFFFFFFF)
        locs = [('reg', x) for x in live if x in REGS] + [('stk', k) for k in range(nslot if nslot is not None else 16)]
        if pt == 'float':
            locs += [('reg', x) for x in live if x in XREGS]
        ok = []
        for l in locs:
            good = True
            for o in obs:
                v, isfp, isblob = o['vals'][i]
                if isblob:
                    continue              # our copy of a literal: any place
                if l[0] == 'reg':
                    rv = o['regs'][REGS.index(l[1])] if l[1] in REGS else (o['xlo'][XREGS.index(l[1])] if o['xlo'] else None)
                else:
                    rv = o['stack'][l[1]] if l[1] < len(o['stack']) else None
                if rv is None:
                    good = False
                elif isfp:
                    good = o['lo'] <= rv < o['hi']
                else:
                    good = ((rv ^ v) & mask) == 0
                if not good:
                    break
            if good:
                ok.append(l)
        allowed.append(ok)
    out = []

    def rec(i, used, last_slot, m):
        if len(out) >= cap:
            return
        if i == len(loc):
            cover = []
            for k_, w_, sz_ in m:
                if k_ == 'stk' and w_ < FAR:
                    cover += list(range(w_, w_ + (sz_ + 3) // 4))
            if nslot is not None and sorted(cover) != list(range(nslot)):
                return
            if nslot is None and cover and sorted(cover) != list(range(len(cover))):
                return
            out.append(list(m))
            return
        sz = loc[i][2]
        opts = allowed[i]
        if opts is None:
            opts = [('stk', last_slot + 1)]
        for l in opts:
            if l in used:
                continue
            if l[0] == 'stk':
                if l[1] <= last_slot or (nslot is not None and l[1] + (sz + 3) // 4 > nslot):
                    continue
                rec(i + 1, used | {l}, l[1] + (sz + 3) // 4 - 1, m + [('stk', l[1], sz)])
            else:
                rec(i + 1, used | {l}, last_slot, m + [('reg', l[1], sz)])
        if not opts:
            rec(i + 1, used, last_slot, m + [('stk', FAR + i, sz)])   # not passed
    rec(0, frozenset(), -1, [])
    return out


def learn_callee(t, va, size, good, snap, decomp, env, deadline):
    """The retail convention of callee t, learned by checking t's own source against retail t on
    the states retail A reaches at its calls to t: every entry mapping that pops the bytes retail's
    t pops and under which ours agrees with retail on every such state. Kept only when they all
    agree on each pointer parameter's location (None elsewhere for parameters they do not pin)."""
    row = common.functions(decomp).get(t)
    if not row or not row['source'] or t in transparent():
        return None
    srcp = os.path.join(decomp, row['source'])
    import hashlib
    ck = os.path.join(LEARN_DIR, '%08x_%s.json' % (t, hashlib.sha1(('%s|%s|%s|%s|%08x|%r' % (
        os.path.abspath(decomp).lower(), os.path.getmtime(srcp) if os.path.exists(srcp) else 0,
        dbuild.newest_header(decomp), rcache_format(), va,
        sorted(repr([o['key'] for o in g]) for g in (OBS.get(t) or {}).values()))).encode()).hexdigest()[:12]))
    if os.path.exists(ck):
        try:
            v = json.load(open(ck))
            return [tuple(x) if x else None for x in v] if v else None
        except (OSError, ValueError):
            pass
    found, complete = None, True
    try:
        srct = os.path.join(decomp, row['source'])
        bc = link(obj_for(row['source'], decomp, env), t, row['size_i'], decomp, env, src=srct)
        if bc.sig is not None and bc.sig.params and bc.sig.locations() is None:
            bc.sig = sized(bc.sig, decomp, env, srct)
        pops = {r.popped for p, root, x, *_ in good for r in x[1] if r.target == t and not r.tail and r.popped is not None}
        P = pops.pop() if len(pops) == 1 else None
        states = []
        for p, root, x, *_ in good:
            if any(r.target == t for r in x[1]):
                states += call_states(snap(p, root), va, size, t, 3 - len(states))
            if len(states) >= 3:
                break
        uniq = {}
        for ds in states:   # retail often reaches the same call state from several snapshots
            k_ = (tuple(ds[r] for r in REGS + ['ebp', 'esp']), bytes(ds['ram'][ds['esp']:ds['esp'] + 0x800]))
            uniq.setdefault(k_, ds)
        states = list(uniq.values())
        runs = []
        for ds in states:
            try:
                runs.append((ds, run_retail(ds, t, row['size_i'])))
            except Fail:
                pass
        if runs and bc.sig is not None and bc.sig.locations() is not None:
            live, live2 = live_in(t, row['size_i']), live_in(t, row['size_i'], through=True)
            sigs = [bc.sig] + ([with_retbuf(bc.sig)] if bc.sig.ret == 'struct' and with_retbuf(bc.sig) else [])
            agree = []
            for sg in sigs:
                bc.sig = sg
                loc = sg.locations()
                ms = site_candidates(t, sg, P, live2)
                for m in ms:
                    if time.time() > deadline:
                        complete = False
                        break
                    km = [(k, w) for k, w, sz in m]
                    if all(run_blob(ds, bc, m, rr[0], rr[1])[0].startswith('equivalent') for ds, rr in runs):
                        agree.append(([(k, w) if k == 'reg' or w < FAR else None for k, w, sz in m],
                                      [l[3] for l in loc]))
                if agree or not complete:
                    break
            if DEBUG:
                print('    learn %s: %d states, %d runs, %d candidates tried, %d agree %s' % (
                    nearest(t), len(states), len(runs), len(ms), len(agree), [fmt_map([(k, w, 4) if (k, w) != (None, None) else ('stk', 99, 4) for k, w in [e or (None, None) for e in km]]) for km, _ in agree[:4]]))
                if not agree:
                    for m in ms[:3]:
                        print('      e.g. %s -> %s' % (fmt_map(m), [run_blob(ds, bc, m, rr[0], rr[1])[1][:200] for ds, rr in runs[:1]]))
            # every mapping that agrees must put each pointer parameter in the same place: when the
            # callee only hands its output pointers on to its own (mocked) callees, its runs cannot
            # tell them apart, and a first-agreeing guess could pair a caller's outputs wrongly
            if complete and agree and len({len(km) for km, pts in agree}) == 1:
                km0, pts = agree[0]
                out = []
                for i, pt in enumerate(pts):
                    where = {km[i] for km, _ in agree}
                    if len(where) == 1:
                        out.append(km0[i])
                    elif pt == 'ptr':
                        out = None
                        break
                    else:
                        out.append(None)   # an argument the runs do not pin down
                found = out
    except Fail:
        found = None
    if complete:   # a timeout is not a result
        try:
            os.makedirs(LEARN_DIR, exist_ok=True)
            json.dump(found, open(ck, 'w'))
        except OSError:
            pass
    return found


def ensure_maps(ts, b, va, size, good, snap, decomp, env, deadline):
    """MAPS[t] for each callee t: its entry_maps.csv rows that fit our declaration and the bytes
    retail's t popped at A's calls, else the one learn_callee finds."""
    for t in ts:
        if t in MAPS:
            continue
        sig = b.callee_sig.get(t)
        locs = [x.locations() for x in [sig] + ([with_retbuf(sig)] if sig is not None and sig.ret == 'struct' else [])
                if x is not None and x.locations() is not None]
        pops = {r.popped for p, root, x, *_ in good for r in x[1] if r.target == t and not r.tail and r.popped is not None}
        P = pops.pop() if len(pops) == 1 else None
        ok = [km for km in entry_rows(t) if any(len(km) == len(l) and (not P or map_pops(km, l) == P) for l in locs)]
        if not ok:
            km = learn_callee(t, va, size, good, snap, decomp, env, deadline)
            ok = [km] if km else []
        MAPS[t] = ok


def fmt_alts(alts):
    def loc(l, ours):
        return l[1] if l[0] == 'r' else ('stk+%x' % l[1] if ours else 'stk%d' % l[1])
    out = []
    for k, lm in sorted((alts or {}).items(), key=lambda kv: kv[0][-2:]):
        if k[0] == 'rb':
            out.append('%s@+%x: result pointer flipped' % (nearest(k[2]), k[1]))
        else:
            out.append('%s@+%x: %s' % (nearest(k[1]), k[0], ', '.join('%s->%s' % (loc(a, 0), loc(b_, 1)) for a, b_ in lm)))
    return '; '.join(out)


def fmt_map(m):
    return ', '.join('p%d<-%s' % (i, w if k == 'reg' else 'stk%d' % w) for i, (k, w, sz) in enumerate(m))


SAVE_TAG = None   # main() sets it: check() then saves a provisional verdict before risky retries


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
        srcp = os.path.abspath(src) if src else os.path.join(decomp, r['source'])
        b = link(objp, va, size, decomp, env, src=srcp)
        res['linked'] = ' '.join(b.linked)
        if b.sig is not None and b.sig.params and b.sig.locations() is None:
            b.sig = sized(b.sig, decomp, env, srcp)
        if b.sig is not None and b.sig.params and b.sig.locations() is None:
            # by-value structures of unknown size: try one dword each (the snapshots decide)
            ps = [(pt, sz or 4) for pt, sz in b.sig.params]
            b.sig = coff.Sig(b.sig.cc, ps, b.sig.ret, b.sig.is_method, b.sig.varargs)
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
        live = live_in(va, size)
        live2 = live_in(va, size, through=True)   # pass-through registers: tried after the others
        loaded = {}

        def snap(p, root):
            if p not in loaded:
                loaded[p] = snapstore.load(p, root)
            return loaded[p]

        def paired(p, root, x, m, b, alts, todo):
            """Run one snapshot again under alternative frame-pointer pairings at the call sites
            whose positional pairing is uncertain (todo: site -> locmaps). Combinations over the
            sites, fewest changed sites first, capped; the first whole run that agrees wins."""
            sites = [k for k in todo if k != UNSURE and k not in alts and todo[k]][:5]
            combos = []
            for nchg in range(1, len(sites) + 1):
                for ss in itertools.combinations(sites, nchg):
                    for pick in itertools.product(*[todo[k] for k in ss]):
                        combos.append(dict(zip(ss, pick)))
                        if len(combos) >= 2 * PAIR_CAP:
                            break
            for c in combos[:2 * PAIR_CAP]:
                if time.time() > lim['end']:
                    break
                a2 = {**alts, **c}
                v, d = run_blob(snap(p, root), b, m, x[0], x[1], alt=a2)
                if DEBUG:
                    print('  pairing %s -> %s %s' % (fmt_alts(a2), v, d[:160]))
                if v.startswith('equivalent'):
                    return v, a2
            return None, None

        def search(b, retry=False):
            """First entry mapping (over the signature variants) that holds on every snapshot.
            retry: also try alternative frame-pointer pairings (only mappings whose failing run
            met an uncertain pairing in the first pass, recorded in amb_seen)."""
            sigs = [b.sig] + ([with_retbuf(b.sig)] if b.sig.ret == 'struct' and with_retbuf(b.sig) else [])
            first = chosen = None
            for sg in sigs:
                b.sig = sg
                ms = known_maps(va, sg) + candidates(sg, live)
                if live2 != live:
                    ms += [m_ for m_ in candidates(sg, live2) if m_ not in ms]
                for m in ms:
                    if first is not None and time.time() > lim['end']:
                        break
                    mk = (id(b), fmt_map(m), len(sg.params or []))
                    if retry and mk not in amb_seen:
                        continue
                    verdicts, bad, alts, done = [], None, {}, []
                    for p, root, x, why in good:   # a mapping must hold on every snapshot
                        info = {}
                        v, d = run_blob(snap(p, root), b, m, x[0], x[1], alt=alts or None, info=info)
                        unsure_t.update(info.get(UNSURE, ()))
                        if not v.startswith('equivalent') and info:
                            if not retry:
                                amb_seen.add(mk)
                            else:
                                v2, a2 = paired(p, root, x, m, b, alts, info)
                                if v2:
                                    v, alts = v2, a2
                                    # every earlier snapshot must agree under the same pairings
                                    for p_, root_, x_ in done:
                                        v3, d3 = run_blob(snap(p_, root_), b, m, x_[0], x_[1], alt=alts)
                                        if not v3.startswith('equivalent'):
                                            v, d = v3, '%s (under the pairing %s)' % (d3, fmt_alts(alts))
                                            break
                        verdicts.append(v)
                        done.append((p, root, x))
                        if not v.startswith('equivalent'):
                            bad = (os.path.basename(p), d)
                            break
                    if first is None or (bad and len(verdicts) > first[3]):
                        first = (m, bad, verdicts, len(verdicts), sg)
                    if bad is None:
                        res['pairing'] = fmt_alts(alts)
                        return first, (m, verdicts, sg)
            return first, chosen

        amb_seen = set()
        unsure_t = set()
        lim = {'end': t0 + budget}

        def search2(b):
            first, chosen = search(b)
            if chosen is None and amb_seen:
                if SAVE_TAG is not None and first is not None and first[1]:
                    # the retry can drive Unicorn into a native crash: leave the first pass's verdict
                    pv = dict(res, verdict='differs', detail='%s: %s' % first[1], mapping=fmt_map(first[0]),
                              tag=SAVE_TAG, t=round(time.time() - t0, 1))
                    save(pv)
                new = [t_ for t_ in sorted(unsure_t) if t_ not in MAPS]
                if new:
                    # callees met at uncertain pairings: their retail conventions (known or learned)
                    ensure_maps(new, b, va, size, good, snap, decomp, env, time.time() + 150)
                    if any(MAPS.get(t_) for t_ in new):
                        lim['end'] = max(lim['end'], time.time() + 120)   # room for the pairings they give
                f2, chosen = search(b, retry=True)
            return first, chosen
        force = set()
        first, chosen = search2(b)
        # a callee retail inlines here but calls elsewhere: run our compile of it, then search again
        while chosen is None and first[1] and 'retail makes no such call here' in first[1][1] and len(force) < 4:
            nm = first[1][1].split(' to ', 1)[1].split(': retail makes', 1)[0]
            tv = resolve(nm) if nm in table() else None
            if tv is None or tv in force:
                break
            force.add(tv)
            try:
                b = link(objp, va, size, decomp, env, force=force, src=srcp)
            except Fail:
                break
            res['linked'] = ' '.join(b.linked)
            first, chosen = search2(b)
        if chosen is None and first[1] and 'UNLINKED ' in first[1][1]:
            raise Fail('unbuildable: callee ' + first[1][1].split('UNLINKED ', 1)[1])
        if chosen is None:
            m, bad, verdicts, n, sg = first
            res.update(verdict='differs', detail='%s: %s' % bad, mapping=fmt_map(m))
        else:
            m, verdicts, sg = chosen
            res['mapping'] = fmt_map(m) + (' +retbuf' if sg.params and len(sg.params) > len(coff.signature(b.name).params or []) else '')
            res['eq'] = len(verdicts)
            res['verdict'] = 'equivalent_fp' if 'equivalent_fp' in verdicts else 'equivalent'
            res['detail'] = '%d/%d snapshots agree%s' % (res['eq'], len(rr), '' if len(good) == len(rr) else
                                                          ' (%d retail runs untestable)' % (len(rr) - len(good)))
            if res.get('pairing'):
                res['detail'] += ' [frame pointers paired: %s]' % res['pairing']
        if unsure_t:
            res['callee_maps'] = '; '.join('%s: %s' % (nearest(t_), ' | '.join(
                ', '.join('p%d<-%s' % (i, '?' if e is None else e[1] if e[0] == 'reg' else 'stk%d' % e[1])
                          for i, e in enumerate(km)) for km in MAPS[t_]) or 'unknown')
                for t_ in sorted(unsure_t) if t_ in MAPS)
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
    if os.name == 'nt' and os.environ.get('FECHECK_PRIORITY', 'low') == 'low':
        import ctypes   # below normal: overnight rigs play the game beside the checks
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    ap = argparse.ArgumentParser()
    ap.add_argument('vas', nargs='+')
    ap.add_argument('--decomp', default=common.DECOMP)
    ap.add_argument('--src')
    ap.add_argument('--k', type=int, default=6)
    ap.add_argument('--tag', default='')
    a = ap.parse_args()
    global SAVE_TAG
    SAVE_TAG = a.tag
    if not os.environ.get('FECHECK_NO_REFRESH'):
        symmap.refresh(a.decomp)
    env = common.vcenv()
    for x in a.vas:
        res = check(int(x, 16), a.decomp, env, a.src, a.k)
        if dbuild.SSE2 and not res.get('verdict', '').startswith('equivalent'):
            # the x87 build of the same source: VS2022's SSE2 codegen sometimes defeats the
            # entry search where the x87 one does not; either compile is a fair rendering
            dbuild.SSE2 = False
            try:
                alt = check(int(x, 16), a.decomp, env, a.src, a.k, budget=120)
            finally:
                dbuild.SSE2 = True
            if alt.get('verdict', '').startswith('equivalent'):
                alt['detail'] = (alt.get('detail') or '') + ' [x87 build]'
                res = alt
        res['tag'] = a.tag
        print(save(res), flush=True)


if __name__ == '__main__':
    main()
