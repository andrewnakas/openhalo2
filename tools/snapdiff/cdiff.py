"""Per-call differential: Unicorn runs the snapshot's function; at each entry of a lifted
function in the DLL (first K calls each), the DLL runs the lifted copy from Unicorn's exact
state; when Unicorn reaches that call's return the two memories and registers are compared.
usage: python cdiff.py <in.bin> <dll dir> [K] [only-va,..]"""
import sys, ctypes, struct, time, numpy as np, re
from snap import load, code_sections, is_code
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_HOOK_MEM_UNMAPPED, UC_HOOK_BLOCK
from unicorn import x86_const as X
s = load(sys.argv[1]); dlld = sys.argv[2]; K = int(sys.argv[3]) if len(sys.argv) > 3 else 3
only = {int(x, 16) for x in sys.argv[4].split(',')} if len(sys.argv) > 4 else None
sz = s['size']
src = open(dlld + '/lifted_dll.c').read()
funcs = sorted({int(h, 16) for h in re.findall(r'case 0x([0-9A-F]{8})u:', src)})
if only: funcs = [f for f in funcs if f in only]
dll = ctypes.CDLL(dlld + '/lifted.dll')
dll.h_init.restype = ctypes.c_uint64; dll.h_init.argtypes = [ctypes.c_uint64]
dll.h_run.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_double)]
lbase = dll.h_init(sz); assert lbase
lmem = np.ctypeslib.as_array((ctypes.c_uint8 * sz).from_address(lbase))
lcon = np.ctypeslib.as_array((ctypes.c_uint8 * (64 << 20)).from_address(lbase + 0x80000000))
buf = ctypes.create_string_buffer(bytes(s['ram']), sz)
cbuf = ctypes.create_string_buffer(bytes(s['contig']), 64 << 20)
ucon = np.ctypeslib.as_array((ctypes.c_uint8 * (64 << 20)).from_address(ctypes.addressof(cbuf)))
for nm, v, vs, raw, ex in code_sections():
    if is_code(nm): ctypes.memmove(ctypes.addressof(buf) + v, raw, len(raw))
umem = np.ctypeslib.as_array((ctypes.c_uint8 * sz).from_address(ctypes.addressof(buf)))
codeskip = [(v, v + vs) for nm, v, vs, raw, ex in code_sections() if is_code(nm)]
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map_ptr(0, sz, UC_PROT_ALL, ctypes.addressof(buf))
def unm(u, a, addr, size, v, d):
    b = addr - addr % sz; u.mem_map_ptr(b, sz, UC_PROT_ALL, ctypes.addressof(buf)); print('  mirror %08X' % b); return True
for cb in (0x80000000, 0xF0000000): uc.mem_map_ptr(cb, 64 << 20, UC_PROT_ALL, ctypes.addressof(cbuf))
uc.hook_add(UC_HOOK_MEM_UNMAPPED, unm)
R = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_EBX, X.UC_X86_REG_ESP, X.UC_X86_REG_EBP, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI]
for r, n in zip(R, ['eax','ecx','edx','ebx','esp','ebp','esi','edi']): uc.reg_write(r, s[n])
uc.reg_write(X.UC_X86_REG_FPCW, s['fpcw'])
ret0 = struct.unpack_from('<I', buf, s['esp'])[0]
fset = set(funcs); count = {}; pending = [None]; results = []
def mm_read():
    return [0]*8
def mmval(v): return v[0] if isinstance(v, tuple) else v
def on_block(u, addr, size, ud):
    p = pending[0]
    if p is not None:
        va, retaddr, esp0, lregs, lmm = p
        if addr == retaddr and u.reg_read(X.UC_X86_REG_ESP) > esp0:
            pending[0] = None
            esp1 = u.reg_read(X.UC_X86_REG_ESP)
            why = []
            names = ['eax','ecx','edx','ebx','esp','ebp','esi','edi']
            ur = [u.reg_read(r) for r in R]
            lr = [lregs[0], lregs[1], lregs[2], lregs[3], lregs[4], None, lregs[7], lregs[8]]
            for n, a, b in zip(names, ur, lr):
                if b is not None and a != b and n not in ('ecx', 'edx'): why.append('%s %08X/%08X' % (n, a, b))
            for n, a, b in zip(names, ur, lr):
                if b is not None and a != b and n in ('ecx', 'edx'): why.append('(%s %08X/%08X)' % (n, a, b))
            d = np.nonzero(umem != lmem)[0]
            for lo, hi in codeskip: d = d[(d < lo) | (d >= hi)]
            d = d[(d < esp1 - 0x4000) | (d >= esp1)]
            dc = np.nonzero(ucon != lcon)[0]
            if len(dc): why.append('contig %d bytes, first %08X u=%02X l=%02X, last %08X' % (len(dc), 0x80000000 + dc[0], ucon[dc[0]], lcon[dc[0]], 0x80000000 + dc[-1]))
            if len(d): why.append('mem %d bytes, first %08X u=%02X l=%02X, last %08X' % (len(d), d[0], umem[d[0]], lmem[d[0]], d[-1]))
            results.append((va, count[va], why))
            if why: print('MISMATCH sub_%08X call %d: %s' % (va, count[va], '; '.join(why[:6])), flush=True)
            return
        return
    if addr in fset and count.get(addr, 0) < K:
        count[addr] = count.get(addr, 0) + 1
        esp0 = u.reg_read(X.UC_X86_REG_ESP)
        retaddr = struct.unpack_from('<I', buf, esp0)[0]
        ctypes.memmove(lbase, ctypes.addressof(buf), sz)
        ctypes.memmove(lbase + 0x80000000, ctypes.addressof(cbuf), 64 << 20)
        ur = [u.reg_read(r) for r in R]
        regs = (ctypes.c_uint32 * 13)(ur[0], ur[1], ur[2], ur[3], ur[4], ur[5], ur[5], ur[6], ur[7], 0x202, 0, 0, s['fpcw'])
        mm = (ctypes.c_uint64 * 8)(*[mmval(v) & 0xFFFFFFFFFFFFFFFF for v in mm_read()])
        fp = (ctypes.c_double * 8)()
        rc = dll.h_run(addr, regs, mm, fp)
        if rc != 0:
            print('sub_%08X call %d: lifted rc=%d (fault/unresolved) fail_va %08X' % (addr, count[addr], rc, ctypes.c_uint32.in_dll(dll, 'g_fail_va').value), flush=True); return
        pending[0] = (addr, retaddr, esp0, list(regs), list(mm))
uc.hook_add(UC_HOOK_BLOCK, on_block)
t = time.time()
uc.emu_start(s['va'], ret0)
bad = [r for r in results if r[2]]
print('done %.0fs: %d calls compared, %d mismatched; per function: %s' % (time.time() - t, len(results), len(bad),
      ' '.join('%08X:%d/%d' % (v, sum(1 for r in bad if r[0] == v), sum(1 for r in results if r[0] == v)) for v in funcs)))
