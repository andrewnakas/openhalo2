"""snapdiff: differential test of lifted functions from real in-game states.

Workflow (how the Bink specks were found, 2026-10-16):
 1. Hook: in a PRIVATE copy of the gen dir, rename `void sub_X(void)` to
    `static void sub_X__body(void)` and append bsnap_hook.c + `BSNAP_WRAP(X)`.
    Run with RECOMP_BSNAP_DIR=<dir> RECOMP_BSNAP_<X>=lo-hi [RECOMP_BSNAP_STEP=n]
    [RECOMP_BSNAP_OUT=1]: each hit call writes guest RAM + the 64 MB contiguous
    window + registers at entry (and exit) to bsnap_<X>_<n>_in.bin.
 2. mkdll.py <dir> VA..: the real generated bodies of those functions (and their
    callees) into lifted_dll.c; build_dll.cmd <dir> compiles it like the game.
 3. cdiff.py <in.bin> <dll dir> [K] [VA,..]: Unicorn runs the original code from
    the snapshot; at each entry of a listed function the DLL runs the lifted copy
    from Unicorn's exact state and the two are compared at that call's return.
    The deepest mismatching function is the culprit.
 4. refcmp.py <in.bin>..: decoded Bink planes vs PyAV's decoder (bit exactness).
python snap.py <in.bin> <out.bin> replays one call and diffs it against the
lifted run's own exit state (noisy: other guest threads keep running).
Snapshots and frames contain retail data: keep them out of the repos."""
import struct, sys, ctypes, numpy as np
XBE = 'D:/n64work/halo2/dirty/game/default.xbe'
REGN = ['eax','ecx','edx','ebx','esp','ebp','seh_ebp','esi','edi','eflags','df','fp_top','fpcw']
def load(path):
    b = open(path, 'rb').read()
    h = struct.unpack_from('<16I', b, 0)
    assert h[0] == 0x50534E42
    s = dict(va=h[1], size=h[2]); s.update(zip(REGN, h[3:16]))
    o = 64
    s['fp'] = struct.unpack_from('<8d', b, o); o += 64
    s['mm'] = struct.unpack_from('<8Q', b, o); o += 64
    s['xmm'] = [b[o + 16*i:o + 16*i + 16] for i in range(8)]; o += 128
    s['ram'] = np.frombuffer(b, dtype=np.uint8, count=h[2], offset=o)
    s['contig'] = np.frombuffer(b, dtype=np.uint8, count=64<<20, offset=o + h[2]) if len(b) >= o + h[2] + (64<<20) else None
    return s
def code_sections():
    f = open(XBE, 'rb').read()
    base = struct.unpack_from('<I', f, 0x104)[0]; n = struct.unpack_from('<I', f, 0x11C)[0]; sh = struct.unpack_from('<I', f, 0x120)[0] - base
    out = []
    for i in range(n):
        fl, v, vs, ra, rs, na = struct.unpack_from('<6I', f, sh + i*56)
        o = na - base; nm = f[o:f.index(b'\0', o)].decode()
        out.append((nm, v, vs, f[ra:ra+rs], bool(fl & 4)))   # flag 4 = executable
    return out

CODE_NAMES = ('.text', 'DSOUND', 'WMADEC', 'XONLINE', 'XNET', 'D3D', 'XPP')
def is_code(nm): return nm in CODE_NAMES or nm.startswith('BINK') and nm != 'BINKDATA'

def unicorn_run(s, until=None, hooks=None, overlay_code=True, max_insns=0):
    """Run s['va'] in Unicorn from the snapshot until it returns. Returns (uc, membuf)."""
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_HOOK_MEM_UNMAPPED
    from unicorn import x86_const as X
    sz = s['size']
    buf = ctypes.create_string_buffer(bytes(s['ram']), sz)
    if overlay_code:
        for nm, v, vs, raw, ex in code_sections():
            if is_code(nm): ctypes.memmove(ctypes.addressof(buf) + v, raw, len(raw))
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map_ptr(0, sz, UC_PROT_ALL, ctypes.addressof(buf))
    mapped = {0}
    cbuf = ctypes.create_string_buffer(bytes(s['contig']), 64 << 20)
    for cb in (0x80000000, 0xF0000000):
        uc.mem_map_ptr(cb, 64 << 20, UC_PROT_ALL, ctypes.addressof(cbuf)); mapped.add(cb)
    s['_cbuf'] = cbuf
    def unm(uc_, access, addr, size, value, ud):
        base = addr - (addr % sz)
        if base in mapped: return False
        uc_.mem_map_ptr(base, sz, UC_PROT_ALL, ctypes.addressof(buf)); mapped.add(base)
        print('  [uc] mirror mapped at %08X (access %08X)' % (base, addr))
        return True
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unm)
    for r, n in ((X.UC_X86_REG_EAX,'eax'),(X.UC_X86_REG_ECX,'ecx'),(X.UC_X86_REG_EDX,'edx'),(X.UC_X86_REG_EBX,'ebx'),
                 (X.UC_X86_REG_ESP,'esp'),(X.UC_X86_REG_EBP,'ebp'),(X.UC_X86_REG_ESI,'esi'),(X.UC_X86_REG_EDI,'edi')):
        uc.reg_write(r, s[n])
    uc.reg_write(X.UC_X86_REG_EFLAGS, (s['eflags'] & ~0x400) | (0x400 if s['df'] else 0) | 2)
    uc.reg_write(X.UC_X86_REG_FPCW, s['fpcw'])
    ret = struct.unpack_from('<I', buf, s['esp'])[0]
    if hooks: hooks(uc, buf)
    uc.emu_start(s['va'], until or ret, count=max_insns)
    return uc, buf, ret

def diff_ranges(a, b, skip=()):
    a = np.frombuffer(a, dtype=np.uint8) if not isinstance(a, np.ndarray) else a
    b = np.frombuffer(b, dtype=np.uint8) if not isinstance(b, np.ndarray) else b
    d = np.nonzero(a != b)[0]
    for lo, hi in skip: d = d[(d < lo) | (d >= hi)]
    if not len(d): return []
    out = []; start = prev = d[0]
    for x in d[1:]:
        if x - prev > 64: out.append((start, prev + 1)); start = x
        prev = x
    out.append((start, prev + 1)); return out, len(d)

if __name__ == '__main__':
    import time
    sin, sout = load(sys.argv[1]), load(sys.argv[2])
    print('entry va %08X esp %08X ebp %08X eax %08X ecx %08X edx %08X ebx %08X esi %08X edi %08X ret %08X' % (
        sin['va'], sin['esp'], sin['ebp'], sin['eax'], sin['ecx'], sin['edx'], sin['ebx'], sin['esi'], sin['edi'],
        struct.unpack_from('<I', bytes(sin['ram'][sin['esp']:sin['esp']+4]))[0]))
    t = time.time()
    uc, buf, ret = unicorn_run(sin)
    from unicorn import x86_const as X
    print('unicorn done %.1fs  eax %08X/%08X esp %08X/%08X' % (time.time()-t, uc.reg_read(X.UC_X86_REG_EAX), sout['eax'], uc.reg_read(X.UC_X86_REG_ESP), sout['esp']))
    ref = np.frombuffer(buf.raw, dtype=np.uint8)
    skip = [(v, v+vs) for nm, v, vs, raw, ex in code_sections() if is_code(nm)] + [(sout['esp'] - 0x10000, sout['esp'])]
    r = diff_ranges(ref, sout['ram'], skip)
    if not r: print('MATCH (outside code and dead stack)')
    else:
        rng, n = r
        print('DIFF bytes %d in %d ranges' % (n, len(rng)))
        for lo, hi in rng[:30]: print('  %08X..%08X (%d)' % (lo, hi, hi-lo))
    rc = diff_ranges(np.frombuffer(sin['_cbuf'].raw, dtype=np.uint8), sout['contig'])
    if not rc: print('contig MATCH')
    else:
        print('contig DIFF bytes %d in %d ranges' % (rc[1], len(rc[0])))
        for lo, hi in rc[0][:20]: print('  %08X..%08X (%d)' % (0x80000000 + lo, 0x80000000 + hi, hi - lo))
    r3 = diff_ranges(sin['contig'], sout['contig'])
    if r3: print('lifted changed contig %d bytes in %d ranges' % (r3[1], len(r3[0])))
    # what the function changed at all (in vs out), for scale
    r2 = diff_ranges(np.frombuffer(bytes(sin['ram']), dtype=np.uint8), sout['ram'], skip)
    if r2: print('lifted changed %d bytes in %d ranges, e.g. %s' % (r2[1], len(r2[0]), ['%08X..%08X' % x for x in r2[0][:6]]))
