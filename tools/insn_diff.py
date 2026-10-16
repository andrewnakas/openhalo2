"""insn_diff.py: differential test of the lifter, one instruction form at a time.

Every distinct instruction form in the game's code (mnemonic, prefix, operand
kinds) is taken from real instances, wrapped in a tiny function and run two
ways: the original bytes in Unicorn (a Pentium III, x87 and SSE) and the same
bytes lifted by xboxrecomp's translator into a 64-bit DLL. Registers, x87
stack and status, MMX and XMM registers and every dirtied memory page are
compared after each run.

Each test function is
    cmp dword ptr [FA], 0x40000000    ; incoming flags, varied per vector
    INSN                              ; the instance under test
    (lea esp, [esp +/- n])            ; only after push/pop, so ret works
    (fnstsw ax)                       ; only after an x87 compare
    ret
and, for each condition whose flags INSN leaves defined,
    cmp ...; INSN; jcc L; mov dword [RES], 1; ret; L: mov dword [RES], 2; ret

usage: python insn_diff.py gen <workdir> [per-form]
       python insn_diff.py run <workdir> [-k form-substring]
Build the DLL between the two with <workdir>/build.cmd.
"""
import ctypes, json, math, os, re, struct, sys, time

XR = "D:/n64work/halo2/third_party/xboxrecomp"
XBE = "D:/n64work/halo2/dirty/game/default.xbe"
FUNCS_H = "D:/n64work/openhalo2/port/xr/src/recomp/gen/recomp_funcs.h"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from havok_diff import PRELUDE   # same runtime stubs

MEM = 0x01000000
FUZZ_VA = 0x00800000
SCRATCH, SCRATCH_SIZE = 0x00C00000, 0x4000
STACK_TOP = 0x00E00000
TRAMP = 0x00F00000
DATA = TRAMP + 0x100
FA, RES = TRAMP + 0x200, TRAMP + 0x204
SP = STACK_TOP - 64
S = SCRATCH

# eax ecx edx ebx esp ebp esi edi  (esp is fixed)
REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
VECTORS = [
    dict(r=[S + 0x100, 3, 5, S + 0x200, 0, S + 0xC00, S + 0x400, S + 0x800], fa=0x40000000,
         st=[1.5, -2.25, 100.0, 0.1, 7.0, -0.75, 3.0e5]),
    dict(r=[S + 0x40, 1, 0x10, S + 0x1000, 0, S + 0x1C00, S + 0x1400, S + 0x1800], fa=0,
         st=[-0.5, 3.0, 1e10, -7.0, 0.125, 2.5, -1e-4]),
    dict(r=[0x80000000, 0x1F, 0x7FFFFFFF, 0xFFFFFFFF, 0, 0xFEDCBA98, 1, 0x12345678], fa=0x80000000,
         st=[2.5, -3.5, 0.0, 1e-3, -2.5, 3.5, 65536.75]),
    dict(r=[0x3F800000, 0x21, 0, 0xBF000000, 0, 0x40490FDB, 0x7F7FFFFF, 0x80], fa=0x50000000,
         st=[65536.75, -1.0, 0.75, 12.0, 0.5, -0.5, 1.0]),
    dict(r=[0xFFFFFF80, 0, 0xFFFFFFFF, 0x7F, 0, 2, 0x8000, 0xFFFF], fa=0xC0000001,
         st=[-0.0, 3.14159265358979, -100.5, 2.0, 1.0e-30, -4.0e20, 0.3]),
    dict(r=[S + 0x2000, S + 0x80, S + 0x300, S + 0x2400, 0, S + 0x2800, S + 0x2C00, S + 0x3000], fa=0x3FFFFFFF,
         st=[0.3333333333, 4.0, -8.5, 1e-10, 1.5e3, -0.001, 9.0]),
]
XMM_POOL = [1.0, -2.5, 0.0, 3.75, -0.125, 100.0, 1e-3, -7.0, 0.5, 65535.0, -1e6, 2.0,
            -0.0, 0.333, 4096.5, -12.0]
CWS_X87 = [0x027F, 0x007F, 0x0E7F]

SKIP_MN = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "hlt", "rdtsc",
           "cpuid", "wbinvd", "invd", "cli", "sti", "lgdt", "lidt", "sgdt", "sidt", "ltr",
           "wrmsr", "rdmsr", "int", "int3", "into", "ud2", "bound", "enter", "leave",
           "invlpg", "lldt", "sldt", "str", "lmsw", "smsw", "clts", "iretd", "retf",
           "lcall", "ljmp", "nop", "fwait", "wait", "prefetchnta", "prefetcht0",
           "prefetcht1", "prefetcht2", "sfence", "lfence", "mfence", "pause", "rdpmc",
           "fnsave", "fsave", "frstor", "fnstenv", "fstenv", "fldenv", "fxsave", "fxrstor",
           "ldmxcsr", "stmxcsr", "fninit", "finit", "fnclex", "fclex", "fldcw", "fnstcw",
           "fstcw", "emms", "femms", "lds", "les", "lfs", "lgs", "lss", "arpl", "lar", "lsl",
           "verr", "verw", "sysenter", "sysexit", "xlatb", "fdecstp", "fincstp", "ffree"}
PUSHES = {"push": 4, "pushal": 32, "pushfd": 4, "pushaw": 16, "pushf": 2}
POPS = {"pop": 4, "popal": 32, "popfd": 4, "popaw": 16, "popf": 2}
# condition -> (opcode, flags it reads)
CONDS = [("jo", 0x70, "OF"), ("jb", 0x72, "CF"), ("je", 0x74, "ZF"), ("jbe", 0x76, "CF ZF"),
         ("js", 0x78, "SF"), ("jp", 0x7A, "PF"), ("jl", 0x7C, "SF OF"), ("jle", 0x7E, "ZF SF OF")]


def _flag_state(insn):
    """{flag: 'undef'|'mod'|'none'} from Capstone's eflags table."""
    from capstone import x86_const as X
    ef = insn.eflags
    out = {}
    for f in ("OF", "SF", "ZF", "AF", "PF", "CF"):
        und = getattr(X, f"X86_EFLAGS_UNDEFINED_{f}", 0)
        mods = [getattr(X, f"X86_EFLAGS_{k}_{f}", 0) for k in ("MODIFY", "RESET", "SET")]
        if ef & und:
            out[f] = "undef"
        elif any(ef & m for m in mods):
            out[f] = "mod"
        else:
            out[f] = "none"
    return out


def form_key(insn):
    from capstone import x86_const as X
    ops = []
    for op in insn.operands:
        if op.type == X.X86_OP_REG:
            n = insn.reg_name(op.reg)
            if n.startswith("st"):
                ops.append(n.replace("(", "").replace(")", ""))
            elif n.startswith("xmm"):
                ops.append("x")
            elif n.startswith("mm"):
                ops.append("mm")
            elif n == "cl" and insn.mnemonic.startswith(("sh", "ro", "rc", "sa")):
                ops.append("cl")
            else:
                ops.append(f"r{op.size * 8}")
        elif op.type == X.X86_OP_MEM:
            ops.append(f"m{op.size * 8}" + ("abs" if op.mem.base == 0 and op.mem.index == 0 else ""))
        else:
            ops.append("i" if not (insn.mnemonic.startswith(("sh", "ro", "rc", "sa")) and op.imm == 1) else "1")
    pre = " ".join(p for p, b in (("rep", 0xF3), ("repne", 0xF2), ("lock", 0xF0))
                   if b in insn.prefix and not insn.mnemonic.startswith(("movs", "cvt", "rcp", "rsqrt",
                                                                         "sqrt", "add", "sub", "mul",
                                                                         "div", "min", "max", "cmp",
                                                                         "pause", "movd", "movq")))
    return (pre + " " if pre else "") + insn.mnemonic + " " + ",".join(ops)


def _usable(insn):
    from capstone import CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET, CS_GRP_PRIVILEGE
    from capstone import x86_const as X
    if any(insn.group(g) for g in (CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET,
                                   CS_GRP_PRIVILEGE)):
        return False
    if insn.mnemonic in SKIP_MN or insn.mnemonic.startswith(("loop", "j", "call", "ret")):
        return False
    if insn.prefix[1] in (0x64, 0x65, 0x2E, 0x26, 0x36, 0x3E) and insn.prefix[1] not in (0x2E, 0x3E):
        return False   # fs:/gs:/es:/ss: overrides
    for op in insn.operands:
        if op.type == X.X86_OP_REG and insn.reg_name(op.reg) in ("cs", "ds", "es", "fs", "gs", "ss",
                                                                 "cr0", "cr2", "cr3", "cr4", "dr7"):
            return False
    try:
        _, written = insn.regs_access()
    except Exception:
        return False
    if X.X86_REG_ESP in written and insn.mnemonic not in PUSHES and insn.mnemonic not in POPS:
        return False
    if insn.mnemonic in POPS:
        for op in insn.operands:
            if op.type == X.X86_OP_MEM:
                return False   # pop m32: esp-relative address rules differ, rare
    return True


def is_x87(insn):
    return insn.mnemonic.startswith("f")


def uses_mmx(insn):
    from capstone import x86_const as X
    return any(op.type == X.X86_OP_REG and insn.reg_name(op.reg).startswith("mm")
               for op in insn.operands)


def uses_xmm(insn):
    from capstone import x86_const as X
    return any(op.type == X.X86_OP_REG and insn.reg_name(op.reg).startswith("xmm")
               for op in insn.operands)


def collect(per_form):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    sys.path.insert(0, XR)
    from tools.conformance import xbe_run
    data, sections, _ = xbe_run.load(XBE)
    text = [s for s in sections if s.is_code]
    starts = sorted({int(h, 16) for h in re.findall(r"\bsub_([0-9A-Fa-f]{8})\s*\(void\)",
                                                    open(FUNCS_H).read())})
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = True
    forms = {}
    for i, va in enumerate(starts):
        sec = next((s for s in text if s.va <= va < s.va + s.raw_size), None)
        if not sec:
            continue
        end = min(starts[i + 1] if i + 1 < len(starts) else va + 0x4000, va + 0x4000,
                  sec.va + sec.raw_size)
        off = sec.raw_off + (va - sec.va)
        for insn in cs.disasm(data[off:off + (end - va)], va):
            if insn.bytes == b"\xCC":
                break
            if not _usable(insn):
                continue
            k = form_key(insn)
            lst = forms.setdefault(k, [])
            if len(lst) < per_form and all(b != insn.bytes for _, b, _ in lst):
                # prefer instances with different operand text
                if all(o != insn.op_str for _, _, o in lst) or len(lst) < 1:
                    lst.append((insn.address, bytes(insn.bytes), insn.op_str))
    return data, sections, forms


def build_tests(forms):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = True
    blob = bytearray()
    tests = []
    for k in sorted(forms):
        for src_va, b, op_str in forms[k]:
            insn = next(cs.disasm(b, src_va))
            fl = _flag_state(insn)
            kind = "mmx" if uses_mmx(insn) else ("x87" if is_x87(insn) else
                                                  ("sse" if uses_xmm(insn) else "int"))
            body = bytearray(b"\x81\x3D" + struct.pack("<II", FA, 0x40000000)) + b
            mn = insn.mnemonic
            if mn in PUSHES:
                body += b"\x8D\x64\x24" + bytes([PUSHES[mn]])
            elif mn in POPS:
                body += b"\x8D\x64\x24" + bytes([(-POPS[mn]) & 0xFF])
            fnstsw = mn.startswith(("fcom", "fucom", "ftst", "fxam", "fnstsw", "fstsw"))
            if mn.startswith(("fcom", "fucom", "ftst", "fxam")) and not mn.startswith(("fcomi", "fucomi")):
                body += b"\xDF\xE0"          # fnstsw ax
            variants = [("plain", body + b"\xC3")]
            touched = [f for f, s in fl.items() if s == "mod"]
            for cname, op, need in CONDS:
                need = need.split()
                if any(fl[f] == "undef" for f in need):
                    continue
                if not touched and cname not in ("jb", "je"):
                    continue        # pass-through check: two conditions are enough
                if mn in POPS and mn.startswith("popf"):
                    continue
                if kind in ("x87",) and not mn.startswith(("fcomi", "fucomi")) and not touched:
                    continue
                if kind in ("mmx", "sse") and not touched:
                    continue
                v = body + bytes([op, 0x0B]) + b"\xC7\x05" + struct.pack("<II", RES, 1) + b"\xC3" \
                    + b"\xC7\x05" + struct.pack("<II", RES, 2) + b"\xC3"
                variants.append((cname, v))
            for vname, code in variants:
                va = FUZZ_VA + len(blob)
                blob += code
                blob += b"\xCC" * ((-len(blob)) % 16)
                tests.append(dict(va=va, size=len(code), form=k, src=src_va, text=f"{mn} {op_str}",
                                  variant=vname, kind=kind, fnstsw=fnstsw))
    return bytes(blob), tests


TAIL = r'''
typedef struct { uint32_t r[8]; uint32_t top, cc, cw, df; double st[8];
                 RecompXmm x[8]; RecompMmx m[8]; } HState;
static uint8_t *g_base;
__declspec(dllexport) uint64_t h_init(void) {
    g_base = (uint8_t *)VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (!g_base) return 0;
    if (!VirtualAlloc(g_base, MEMSIZE, MEM_COMMIT, PAGE_READWRITE)) return 0;
    g_xbox_mem_offset = (ptrdiff_t)g_base;
    return (uint64_t)(uintptr_t)g_base;
}
/* Pages written since the last call (offsets), and reset. */
__declspec(dllexport) int h_dirty(uint32_t *out, int max) {
    static PVOID addrs[65536]; ULONG_PTR n = 65536; DWORD gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base, MEMSIZE, addrs, &n, &gran) != 0) return -1;
    int k = 0;
    for (ULONG_PTR i = 0; i < n && k < max; i++) out[k++] = (uint32_t)((uint8_t *)addrs[i] - g_base);
    return k;
}
__declspec(dllexport) int h_run2(uint32_t va, const HState *in, HState *out) {
    recomp_func_t f = lookup_lifted(va);
    if (!f) return -1;
    g_eax = in->r[0]; g_ecx = in->r[1]; g_edx = in->r[2]; g_ebx = in->r[3];
    g_esp = in->r[4]; g_ebp = g_seh_ebp = in->r[5]; g_esi = in->r[6]; g_edi = in->r[7];
    g_fp_top = (int)in->top; g_fp_cc = (uint16_t)in->cc; g_fp_cmp = 0;
    g_fp_control_word = (uint16_t)in->cw; g_df = (int)in->df;
    g_recomp_mxcsr = 0x1F80u; g_recomp_eflags = 0x202u;
    memcpy(g_fp_stack, in->st, sizeof g_fp_stack);
    g_xmm0 = in->x[0]; g_xmm1 = in->x[1]; g_xmm2 = in->x[2]; g_xmm3 = in->x[3];
    g_xmm4 = in->x[4]; g_xmm5 = in->x[5]; g_xmm6 = in->x[6]; g_xmm7 = in->x[7];
    g_mm0 = in->m[0]; g_mm1 = in->m[1]; g_mm2 = in->m[2]; g_mm3 = in->m[3];
    g_mm4 = in->m[4]; g_mm5 = in->m[5]; g_mm6 = in->m[6]; g_mm7 = in->m[7];
    __try { f(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode() == 0xE0000002u || GetExceptionCode() == 0xE0000001u ? 3 : 2;
    }
    out->r[0] = g_eax; out->r[1] = g_ecx; out->r[2] = g_edx; out->r[3] = g_ebx;
    out->r[4] = g_esp; out->r[5] = g_ebp; out->r[6] = g_esi; out->r[7] = g_edi;
    out->top = (uint32_t)g_fp_top & 7u; out->cc = g_fp_cc; out->cw = g_fp_control_word; out->df = (uint32_t)g_df;
    memcpy(out->st, g_fp_stack, sizeof g_fp_stack);
    out->x[0] = g_xmm0; out->x[1] = g_xmm1; out->x[2] = g_xmm2; out->x[3] = g_xmm3;
    out->x[4] = g_xmm4; out->x[5] = g_xmm5; out->x[6] = g_xmm6; out->x[7] = g_xmm7;
    out->m[0] = g_mm0; out->m[1] = g_mm1; out->m[2] = g_mm2; out->m[3] = g_mm3;
    out->m[4] = g_mm4; out->m[5] = g_mm5; out->m[6] = g_mm6; out->m[7] = g_mm7;
    return 0;
}
'''


def gen(wd, per_form=3):
    sys.path.insert(0, XR)
    from tools.conformance import xbe_run
    data, sections, forms = collect(per_form)
    blob, tests = build_tests(forms)
    Section = type(sections[0])
    raw = bytes(data) + blob
    fuzz = Section(".fuzz", FUZZ_VA, len(blob), len(data), len(blob), True)
    secs = list(sections) + [fuzz]
    lifted, rejected = xbe_run.lift(raw, secs, [(t["va"], t["size"], None) for t in tests])
    have = {va for va, _, _ in lifted}
    rej = {va: why for va, why in rejected}
    for t in tests:
        t["lifted"] = t["va"] in have
        if not t["lifted"]:
            t["why"] = (rej.get(t["va"]) or ["?"])[0]
    os.makedirs(wd, exist_ok=True)
    out = [PRELUDE, f"#define MEMSIZE 0x{MEM:X}ull"]
    subs = set()
    for _, _, body in lifted:
        subs |= {int(h, 16) for h in re.findall(r"\bsub_([0-9A-Fa-f]{8})\b", body)}
    for va in sorted(subs - have):
        out.append(f"static void sub_{va:08X}(void) {{ RaiseException(0xE0000001u, 0, 0, 0); }}")
    for va in sorted(subs & have):
        out.append(f"#define sub_{va:08X} lifted_{va:08X}")
    refd = set()
    for _, _, body in lifted:
        refd |= {int(h, 16) for h in re.findall(r"\blifted_([0-9A-Fa-f]{8})\b", body)}
    for va in sorted(have | refd):
        out.append(f"void lifted_{va:08X}(void);")
    for _, _, body in lifted:
        out.append(body)
    for va in sorted(refd - have):
        out.append(f"void lifted_{va:08X}(void) {{ RaiseException(0xE0000001u, 0, 0, 0); }}")
    out.append("static recomp_func_t lookup_lifted(uint32_t va) { switch (va) {")
    for va in sorted(have):
        out.append(f"    case 0x{va:08X}u: return lifted_{va:08X};")
    out.append("    default: return 0; } }")
    out.append(TAIL)
    open(os.path.join(wd, "lifted_dll.c"), "w").write("\n".join(out))
    open(os.path.join(wd, "fuzz.bin"), "wb").write(blob)
    json.dump({"sections": [[s.name, s.va, s.vsize, s.raw_off, s.raw_size] for s in sections],
               "tests": tests}, open(os.path.join(wd, "meta.json"), "w"))
    wdw = wd.replace("/", "\\")
    open(os.path.join(wd, "build.cmd"), "w").write(
        "@echo off\r\n"
        "set \"PATH=C:\\Windows\\System32;C:\\Windows;C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\"\r\n"
        "call \"C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul\r\n"
        f"cd /d {wdw}\r\n"
        f"cl /nologo /O1 /W0 /LD /bigobj /I\"{XR}/templates/runtime\" lifted_dll.c /Felifted.dll > build.log 2>&1\r\n"
        "echo BUILD_EXIT=%ERRORLEVEL%\r\n")
    nrej = sum(1 for t in tests if not t["lifted"])
    print(f"{len(forms)} forms, {len(tests)} tests, {len(lifted)} lifted, {nrej} rejected -> {wd}")
    whys = {}
    for t in tests:
        if not t["lifted"]:
            whys.setdefault(t["form"], t["why"])
    for k in sorted(whys)[:60]:
        print(f"  REJECTED {k}: {whys[k][:90]}")


def f80(mant, exp):
    sign = -1.0 if exp & 0x8000 else 1.0
    e = exp & 0x7FFF
    if e == 0x7FFF:
        return sign * math.inf if (mant << 1) & ((1 << 64) - 1) == 0 else math.nan
    if mant == 0:
        return sign * 0.0
    try:
        return sign * math.ldexp(mant, e - 16383 - 63)
    except OverflowError:
        return sign * math.inf


class HState(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 8), ("top", ctypes.c_uint32), ("cc", ctypes.c_uint32),
                ("cw", ctypes.c_uint32), ("df", ctypes.c_uint32), ("st", ctypes.c_double * 8),
                ("x", (ctypes.c_uint32 * 4) * 8), ("m", ctypes.c_uint64 * 8)]


def close(a, b, rel):
    if a != a and b != b:
        return True
    if math.isinf(a) or math.isinf(b):
        return a == b
    return abs(a - b) <= rel * max(abs(a), abs(b), 1e-300)


def run(wd, only=None):
    from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_HOOK_MEM_WRITE
    from unicorn import x86_const as U
    meta = json.load(open(os.path.join(wd, "meta.json")))
    tests = [t for t in meta["tests"] if t["lifted"]]
    if only:
        tests = [t for t in tests if only in t["form"]]
    dll = ctypes.CDLL(os.path.join(wd, "lifted.dll"))
    dll.h_init.restype = ctypes.c_uint64
    dll.h_run2.argtypes = [ctypes.c_uint32, ctypes.POINTER(HState), ctypes.POINTER(HState)]
    dll.h_dirty.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
    base = dll.h_init()
    assert base
    img = bytearray(MEM)
    raw = open(XBE, "rb").read()
    for name, va, vsize, roff, rsize in meta["sections"]:
        if va + rsize <= MEM:
            img[va:va + rsize] = raw[roff:roff + rsize]
    blob = open(os.path.join(wd, "fuzz.bin"), "rb").read()
    img[FUZZ_VA:FUZZ_VA + len(blob)] = blob
    for i in range(SCRATCH_SIZE // 4):
        w = struct.unpack("<I", struct.pack("<f", float((((i * 37) % 41) - 20) * 0.125)))[0]
        if i % 16 == 3:
            w = i % 5
        if i % 16 == 7:
            w = SCRATCH + (i * 52) % SCRATCH_SIZE
        struct.pack_into("<I", img, SCRATCH + i * 4, w)
    for i in range(0, 0x100000 // 4, 1):   # stack: recognisable words
        struct.pack_into("<I", img, 0x00D00000 + i * 4, (0x5A000000 | i) if i % 3 else i % 7)
    pristine = bytes(img)
    ctypes.memmove(base, pristine, MEM)
    pagebuf = (ctypes.c_uint32 * 65536)()
    dll.h_dirty(pagebuf, 65536)

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.ctl_set_cpu_model(U.UC_CPU_X86_PENTIUM3)
    uc.mem_map(0, MEM, UC_PROT_ALL)
    uc.reg_write(U.UC_X86_REG_CR4, uc.reg_read(U.UC_X86_REG_CR4) | 0x600)
    uc.reg_write(U.UC_X86_REG_CR0, (uc.reg_read(U.UC_X86_REG_CR0) & ~4) | 2)
    uc.mem_write(0, pristine)
    upages = set()

    def on_write(uc_, access, addr, size, value, ud):
        upages.add(addr & ~0xFFF)
        upages.add((addr + size - 1) & ~0xFFF)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

    def tramp(mmx):
        c = bytearray(b"\x9B\xDB\xE3")                                  # finit
        c += b"\xD9\x2D" + struct.pack("<I", DATA)                      # fldcw [DATA]
        if mmx:
            for i in range(8):                                          # movq mmi, [DATA+0x40+8i]
                c += b"\x0F\x6F" + bytes([0x05 | (i << 3)]) + struct.pack("<I", DATA + 0x40 + 8 * i)
        else:
            for i in range(6, -1, -1):                                  # fld qword [DATA+0x40+8i]
                c += b"\xDD\x05" + struct.pack("<I", DATA + 0x40 + 8 * i)
        c += b"\xFF\x15" + struct.pack("<I", DATA + 0x20)               # call [slot]
        return bytes(c)
    tr = {False: tramp(False), True: tramp(True)}
    TR_AT = {False: TRAMP, True: TRAMP + 0x80}
    uc.mem_write(TRAMP, tr[False])
    uc.mem_write(TRAMP + 0x80, tr[True])
    # pristine must include the trampolines for page restores
    img[TRAMP:TRAMP + len(tr[False])] = tr[False]
    img[TRAMP + 0x80:TRAMP + 0x80 + len(tr[True])] = tr[True]
    pristine = bytes(img)
    ctypes.memmove(base, pristine, MEM)
    dll.h_dirty(pagebuf, 65536)

    lmem = (ctypes.c_ubyte * MEM).from_address(base)
    fails, faults, rounding, skipped, total = {}, {}, {}, 0, 0
    t0 = time.time()
    xmm_regs = [getattr(U, f"UC_X86_REG_XMM{i}") for i in range(8)]
    fp_regs = [getattr(U, f"UC_X86_REG_FP{i}") for i in range(8)]
    gpr = [U.UC_X86_REG_EAX, U.UC_X86_REG_ECX, U.UC_X86_REG_EDX, U.UC_X86_REG_EBX,
           U.UC_X86_REG_ESP, U.UC_X86_REG_EBP, U.UC_X86_REG_ESI, U.UC_X86_REG_EDI]
    for ti, t in enumerate(tests):
        if ti % 1000 == 0:
            print(f"  .. {ti}/{len(tests)} {time.time() - t0:.0f} s", flush=True)
        mmx = t["kind"] == "mmx"
        cws = CWS_X87 if t["kind"] == "x87" else [0x027F]
        for cw in cws:
            for vi, vec in enumerate(VECTORS):
                regs = list(vec["r"])
                regs[4] = SP - 4
                xm = [[struct.unpack("<I", struct.pack("<f", XMM_POOL[(vi * 5 + i * 4 + j) % 16]))[0]
                       for j in range(4)] for i in range(8)]
                mm = [(0x0123456789ABCDEF * (i + 1 + vi * 8)) & ((1 << 64) - 1) for i in range(8)]
                patch = {}
                patch[DATA] = struct.pack("<H", cw)
                patch[DATA + 0x20] = struct.pack("<I", t["va"])
                patch[FA] = struct.pack("<I", vec["fa"])
                if mmx:
                    patch[DATA + 0x40] = struct.pack("<8Q", *mm)
                else:
                    patch[DATA + 0x40] = struct.pack("<7d", *vec["st"])
                ret_addr = TR_AT[mmx] + len(tr[mmx])
                patch[SP - 4] = struct.pack("<I", ret_addr)
                # ---- reference
                upages.clear()
                for a, b in patch.items():
                    uc.mem_write(a, b)
                for r, v in zip(gpr, regs):
                    uc.reg_write(r, v & 0xFFFFFFFF)
                uc.reg_write(U.UC_X86_REG_ESP, SP)
                uc.reg_write(U.UC_X86_REG_EFLAGS, 0x202)
                for i in range(8):
                    uc.reg_write(xmm_regs[i], xm[i][0] | xm[i][1] << 32 | xm[i][2] << 64 | xm[i][3] << 96)
                try:
                    uc.emu_start(TR_AT[mmx], ret_addr, timeout=500_000, count=200_000)
                    ok = uc.reg_read(U.UC_X86_REG_EIP) == ret_addr
                except UcError:
                    ok = False
                r_regs = [uc.reg_read(r) for r in gpr]
                fpsw = uc.reg_read(U.UC_X86_REG_FPSW)
                fps = [uc.reg_read(r) for r in fp_regs]
                r_xmm = [uc.reg_read(r) for r in xmm_regs]
                # ---- lifted
                for a, b in patch.items():
                    ctypes.memmove(base + a, b, len(b))
                hin, hout = HState(), HState()
                for i in range(8):
                    hin.r[i] = regs[i] & 0xFFFFFFFF
                hin.cw = cw
                if mmx:
                    hin.top = 0
                    for i in range(8):
                        hin.m[i] = mm[i]
                else:
                    hin.top = 1
                    for i in range(7):
                        hin.st[1 + i] = vec["st"][i]
                for i in range(8):
                    for j in range(4):
                        hin.x[i][j] = xm[i][j]
                rc = dll.h_run2(t["va"], ctypes.byref(hin), ctypes.byref(hout)) if ok else None
                n = dll.h_dirty(pagebuf, 65536)
                lpages = {pagebuf[i] for i in range(max(n, 0))}
                pages = upages | lpages | {DATA & ~0xFFF, FA & ~0xFFF, (SP - 4) & ~0xFFF}
                why = []
                if ok and rc == 0:
                    total += 1
                    for i in range(8):
                        a, b = r_regs[i], hout.r[i]
                        if t["fnstsw"] and i == 0:
                            a &= 0xFFFF7F00; b &= 0xFFFF7F00   # status bits the lifter models
                        if a != b:
                            why.append(f"{REGS[i]} {a:08X}/{b:08X}")
                    if t["kind"] == "x87":
                        top = (fpsw >> 11) & 7
                        if top != hout.top:
                            why.append(f"top {top}/{hout.top}")
                        else:
                            depth = (8 - top) % 8 or 8
                            rel = 1e-6 if (cw & 0x300) == 0 else 1e-12
                            for i in range(min(depth, 8)):
                                ph = (top + i) & 7
                                a = f80(*fps[ph]); b = hout.st[ph]
                                if not close(a, b, 0):
                                    tag = "rounding" if close(a, b, rel * 10) else ""
                                    why.append(f"st{i} {a!r}/{b!r}" + (f" ({tag})" if tag else ""))
                        if t["fnstsw"] is False and (fpsw & 0x4500) != (hout.cc & 0x4500) and \
                                t["form"].split()[0].startswith(("fcom", "fucom", "ftst", "fxam", "fprem")):
                            why.append(f"cc {fpsw & 0x4700:04X}/{hout.cc & 0x4700:04X}")
                    if mmx:
                        for i in range(8):
                            a = fps[i][0]; b = hout.m[i]
                            if a != b:
                                why.append(f"mm{i} {a:016X}/{b:016X}")
                    if t["kind"] == "sse" or "xmm" in t["text"]:
                        for i in range(8):
                            a = r_xmm[i]
                            b = sum(hout.x[i][j] << (32 * j) for j in range(4))
                            if a != b:
                                fa = [struct.unpack("<f", struct.pack("<I", (a >> 32 * j) & 0xFFFFFFFF))[0] for j in range(4)]
                                fb = [struct.unpack("<f", struct.pack("<I", (b >> 32 * j) & 0xFFFFFFFF))[0] for j in range(4)]
                                tag = " (rounding)" if all(close(x, y, 2e-3) for x, y in zip(fa, fb)) else ""
                                why.append(f"xmm{i} {a:032X}/{b:032X}{tag}")
                    for pg in sorted(pages):
                        if pg >= MEM:
                            continue
                        ra = bytes(uc.mem_read(pg, 0x1000))
                        la = bytes(lmem[pg:pg + 0x1000])
                        if ra != la:
                            i = next(i for i in range(0x1000) if ra[i] != la[i]) & ~3
                            rv, lv = struct.unpack_from("<I", ra, i)[0], struct.unpack_from("<I", la, i)[0]
                            rf, lf = struct.unpack("<f", ra[i:i + 4])[0], struct.unpack("<f", la[i:i + 4])[0]
                            tag = " (rounding)" if rf == rf and lf == lf and close(rf, lf, 1e-6) else ""
                            why.append(f"mem {pg + i:08X} {rv:08X}/{lv:08X}{tag}")
                            break
                elif ok and rc != 0:
                    faults.setdefault(t["va"], []).append((vi, cw, rc))
                else:
                    skipped += 1
                # restore pages on both sides
                for pg in pages:
                    if pg < MEM:
                        uc.mem_write(pg, pristine[pg:pg + 0x1000])
                        ctypes.memmove(base + pg, pristine[pg:pg + 0x1000], 0x1000)
                dll.h_dirty(pagebuf, 65536)
                if why:
                    bucket = rounding if all("rounding" in w for w in why) else fails
                    bucket.setdefault(t["va"], []).append((vi, cw, why))
    dt = time.time() - t0
    byform = {}
    for va, lst in fails.items():
        t = next(x for x in tests if x["va"] == va)
        byform.setdefault(t["form"], []).append((t, lst))
    print(f"{len(tests)} tests, {total} runs compared, {skipped} reference runs skipped, {dt:.0f} s")
    print(f"MISMATCH {len(byform)} forms ({len(fails)} tests) | lifted-only fault {len(faults)} | "
          f"rounding only {len(rounding)}")
    for k in sorted(byform):
        t, lst = byform[k][0]
        vi, cw, why = lst[0]
        print(f"  FAIL [{k}] {t['text']} @{t['src']:08X} {t['variant']} v{vi} cw{cw:04X} "
              f"({len(lst)} runs): {'; '.join(why)[:160]}")
    for va in sorted(faults)[:40]:
        t = next(x for x in tests if x["va"] == va)
        print(f"  LIFTED FAULT [{t['form']}] {t['text']} {t['variant']} {faults[va][:3]}")
    json.dump({"fails": {f"{k:08X}": v for k, v in fails.items()},
               "faults": {f"{k:08X}": v for k, v in faults.items()},
               "rounding": {f"{k:08X}": v for k, v in rounding.items()}},
              open(os.path.join(wd, "result.json"), "w"), indent=1)


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 3)
    else:
        run(sys.argv[2], sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == "-k" else None)
