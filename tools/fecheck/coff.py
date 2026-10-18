"""Minimal i386 COFF object reader + MSVC name undecoration (dbghelp)."""
import ctypes
import re
import struct

IMAGE_REL_I386_ABSOLUTE = 0
IMAGE_REL_I386_DIR32 = 6
IMAGE_REL_I386_DIR32NB = 7
IMAGE_REL_I386_SECTION = 0xA
IMAGE_REL_I386_SECREL = 0xB
IMAGE_REL_I386_REL32 = 0x14

SCN_CODE = 0x20
SCN_INIT = 0x40
SCN_UNINIT = 0x80
SCN_LNK_INFO = 0x200
SCN_LNK_REMOVE = 0x800
SCN_COMDAT = 0x1000
SCN_EXEC = 0x20000000
SCN_READ = 0x40000000
SCN_WRITE = 0x80000000


class Section:
    __slots__ = ('index', 'name', 'flags', 'data', 'size', 'relocs', 'comdat_sel', 'assoc', 'align')


class Symbol:
    __slots__ = ('index', 'name', 'value', 'section', 'type', 'sclass', 'naux', 'aux')

    @property
    def is_func(self):
        return (self.type >> 4) == 2

    @property
    def external(self):
        return self.sclass == 2

    def __repr__(self):
        return 'Symbol(%s sec=%d val=%x cls=%d)' % (self.name, self.section, self.value, self.sclass)


class Coff:
    def __init__(self, path):
        b = open(path, 'rb').read()
        self.raw = b
        mach, nsec, _, psym, nsym, opt, _ = struct.unpack_from('<HHIIIHH', b, 0)
        if mach == 0 and nsec == 0xFFFF:
            raise ValueError('bigobj not supported')
        assert mach == 0x14C, 'not i386: %x' % mach
        strtab_off = psym + nsym * 18
        self.strtab = b[strtab_off:]

        def sname(raw8):
            if raw8[:4] == b'\0\0\0\0':
                off = struct.unpack_from('<I', raw8, 4)[0]
                return self._str(off)
            return raw8.rstrip(b'\0').decode('latin-1')
        self.sections = [None]
        o = 20 + opt
        for i in range(nsec):
            nm = b[o:o + 8]
            vsz, va, rsz, praw, prel, plin, nrel, nlin, fl = struct.unpack_from('<IIIIIIHHI', b, o + 8)
            s = Section()
            s.index = i + 1
            n = nm.rstrip(b'\0').decode('latin-1')
            if n.startswith('/'):
                n = self._str(int(n[1:]))
            s.name = n
            s.flags = fl
            s.size = rsz
            s.data = bytes(rsz) if (fl & SCN_UNINIT) else b[praw:praw + rsz]
            s.relocs = [struct.unpack_from('<IIH', b, prel + 10 * k) for k in range(nrel)]
            s.comdat_sel = 0
            s.assoc = 0
            al = (fl >> 20) & 0xF
            s.align = 1 << (al - 1) if al else 16
            self.sections.append(s)
            o += 40
        self.symbols = []
        self.by_index = {}
        k = 0
        while k < nsym:
            e = b[psym + k * 18: psym + k * 18 + 18]
            sym = Symbol()
            sym.index = k
            sym.name = sname(e[:8])
            sym.value, sym.section, sym.type, sym.sclass, sym.naux = struct.unpack_from('<IhHBB', e, 8)
            sym.aux = [b[psym + (k + 1 + j) * 18: psym + (k + 2 + j) * 18] for j in range(sym.naux)]
            if sym.sclass == 3 and sym.naux and sym.value == 0 and 0 < sym.section < len(self.sections):
                sec = self.sections[sym.section]
                if sec.flags & SCN_COMDAT and sym.name == sec.name:
                    ln, nr, nl, ck, num, sel = struct.unpack_from('<IHHIHB', sym.aux[0], 0)
                    if not sec.comdat_sel:
                        sec.comdat_sel = sel
                        sec.assoc = num
            self.symbols.append(sym)
            self.by_index[k] = sym
            k += 1 + sym.naux

    def _str(self, off):
        e = self.strtab.index(b'\0', off)
        return self.strtab[off:e].decode('latin-1')

    def defined(self):
        """External/static symbols defined in a section, by name."""
        out = {}
        for s in self.symbols:
            if s.section > 0 and s.sclass in (2, 3) and not (s.sclass == 3 and s.value == 0 and s.naux):
                out.setdefault(s.name, s)
        return out


_dbghelp = None


def undecorate(name, flags=0):
    """MSVC undecoration; flags 0 = complete, 0x1000 = name only."""
    global _dbghelp
    if not name.startswith('?'):
        return name
    if _dbghelp is None:
        _dbghelp = ctypes.WinDLL('dbghelp')
        _dbghelp.UnDecorateSymbolName.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
    buf = ctypes.create_string_buffer(4096)
    n = _dbghelp.UnDecorateSymbolName(name.encode('latin-1'), buf, 4096, flags)
    return buf.value.decode('latin-1') if n else name


def plain_name(dec):
    """Qualified name without decoration: '?f@c@@QAEXH@Z' -> 'c::f', '_f@8' -> 'f', '@f@8' -> 'f'."""
    if dec.startswith('?'):
        return undecorate(dec, 0x1000)
    m = re.match(r'^[_@]?([A-Za-z_$?][\w$?@]*?)(@\d+)?$', dec)
    return m.group(1) if m else dec


SIZES = {'char': 4, 'bool': 4, 'short': 4, 'int': 4, 'long': 4, 'wchar_t': 4, 'float': 4, 'double': 8,
         '__int64': 8, 'enum': 4}


class Sig:
    """A function's calling convention as the VS2022 compile sees it (standard conventions only)."""
    def __init__(self, cc, params, ret, is_method=False, varargs=False):
        self.cc, self.params, self.ret, self.is_method, self.varargs = cc, params, ret, is_method, varargs

    def __repr__(self):
        return 'Sig(%s %s -> %s)' % (self.cc, self.params, self.ret)

    def locations(self):
        """[(kind, where, size, ptype)] per parameter ('this' first for methods):
        kind 'reg' (where = register name) or 'stk' (where = byte offset from esp+4 at entry).
        None when a parameter's size is unknown (a structure passed by value)."""
        if self.params is None or any(sz == 0 for pt, sz in self.params):
            return None
        out, off = [], 0
        regs = ['ecx', 'edx'] if self.cc == 'fastcall' else []
        for i, (pt, sz) in enumerate(self.params):
            if self.cc == 'thiscall' and i == 0:
                out.append(('reg', 'ecx', 4, pt))
            elif regs and sz == 4 and pt in ('int', 'ptr'):
                out.append(('reg', regs.pop(0), 4, pt))
            else:
                out.append(('stk', off, sz, pt))
                off += (sz + 3) & ~3
        return out

    def stack_bytes(self):
        loc = self.locations()
        return None if loc is None else sum((l[2] + 3) & ~3 for l in loc if l[0] == 'stk')

    def callee_pops(self):
        return self.cc in ('stdcall', 'fastcall', 'thiscall') and not self.varargs


def _ptype(p):
    p = p.strip()
    if p.endswith('*') or p.endswith('&') or '*' in p or '&' in p:
        return ('ptr', 4)
    q = re.sub(r'\b(const|volatile|signed|unsigned)\b', '', p).strip()
    if q.startswith('enum'):
        return ('int', 4)
    if q in ('float',):
        return ('float', 4)
    if q in ('double', 'long double'):
        return ('double', 8)
    if q in ('__int64',):
        return ('int64', 8)
    if q in SIZES:
        return ('int', 4)
    if q.startswith('struct') or q.startswith('class') or q.startswith('union'):
        return ('struct ' + q.split()[-1], 0)
    return ('int', 4) if q in ('', 'long', 'int') else ('struct ' + q, 0)


def _split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<(':
            depth += 1
        elif ch in '>)':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def signature(dec):
    """Sig for a decorated function name, or None."""
    if not dec.startswith('?'):
        m = re.match(r'^(_|@)([\w$]+?)(?:@(\d+))?$', dec)
        if not m:
            return None
        n = int(m.group(3)) // 4 if m.group(3) else None
        cc = 'fastcall' if m.group(1) == '@' else ('stdcall' if m.group(3) else 'cdecl')
        if n is None:
            return Sig(cc, None, 'int')
        return Sig(cc, [('int', 4)] * n, 'int')
    full = undecorate(dec)
    m = re.search(r'__(cdecl|stdcall|fastcall|thiscall)\s+(.*?)\((.*)\)(\s*const)?\s*$', full)
    if not m:
        return None
    cc = m.group(1)
    pre = full[:m.start()]
    ret = re.sub(r'\b(public|private|protected|static|virtual):?', '', pre).strip()
    args = m.group(3).strip()
    params = []
    varargs = False
    if args and args != 'void':
        for p in _split_top(args):
            p = p.strip()
            if p == '...':
                varargs = True
                continue
            params.append(_ptype(p))
    is_method = cc == 'thiscall' or ('::' in m.group(2) and 'static' not in pre)
    if cc == 'thiscall':
        params = [('ptr', 4)] + params
    if varargs and cc == 'thiscall':
        cc = 'cdecl'
    rt = ret.split()[-1] if ret else 'void'
    if '*' in ret or '&' in ret:
        rk = 'ptr'
    elif rt in ('float',):
        rk = 'float'
    elif rt in ('double',):
        rk = 'double'
    elif rt == '__int64':
        rk = 'int64'
    elif rt == 'void':
        rk = 'void'
    elif rt == 'bool':
        rk = 'bool'
    elif rt == 'char':
        rk = 'char'
    elif rt == 'short':
        rk = 'short'
    elif rt in ('long', 'int') or rt in SIZES or ret.startswith('enum'):
        rk = 'int'
    else:
        rk = 'struct'
    return Sig(cc, params, rk, is_method, varargs)
