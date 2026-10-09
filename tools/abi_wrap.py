"""Instrument lifted functions so each direct call inside them reports when it
behaves differently from its first call: a different esp change, or esi/ebx/edi
coming back changed.

    python abi_wrap.py <gen dir> <function VA hex> [<function VA hex> ...]
    python abi_wrap.py <gen dir> --clean        remove all instrumentation

Edits the generated C in place (regeneration removes it too). ebp is ignored:
the game was built with LTCG and uses it freely across calls. A call site's
first esp change is taken as its normal one; LTCG callees that return values in
esi/ebx/edi will report every time, so read the output with the disassembly.
"""
import glob, os, re, sys

BS = chr(92)
HEAD = '{ uint32_t s0_=g_esi, b0_=g_ebx, d0_=g_edi, p0_=g_esp; '
OLD_HEAD = '{ uint32_t s0_=g_esi, b0_=g_ebx, d0_=g_edi, e0_=g_ebp, p0_=g_esp; '
TAIL_MARK = ' /*ABIW*/ '

def unwrap(text):
    out, n = [], 0
    for line in text.split('\n'):
        s = line.lstrip()
        for head in (HEAD, OLD_HEAD):
            if s.startswith(head):
                rest = s[len(head):]
                cut = rest.find(TAIL_MARK)
                if cut < 0:
                    cut = rest.find(' if (g_esi != s0_')
                if cut >= 0:
                    line = '    ' + rest[:cut]
                    n += 1
                break
        out.append(line)
    return '\n'.join(out), n

def wrap_function(gen, va):
    name = 'void sub_%08X(void)' % va
    for path in sorted(glob.glob(os.path.join(gen, 'recomp_0*.c'))):
        text = open(path, 'rb').read().decode('utf-8')
        i = text.find(name)
        if i < 0:
            continue
        nl = '\r\n' if '\r\n' in text[:4000] else '\n'
        j = text.index(nl + '}' + nl, i)
        body, _ = unwrap(text[i:j])
        count = [0]
        def wrap(m):
            count[0] += 1
            tgt = m.group(1)
            return (HEAD + m.group(0).strip() + TAIL_MARK
                    + '{ static int first_ = 0x7fffffff; static unsigned k_; int d_ = (int)(g_esp - p0_);'
                    ' if (first_ == 0x7fffffff) first_ = d_;'
                    ' if ((d_ != first_ || g_esi != s0_ || g_ebx != b0_ || g_edi != d0_) && k_++ < 4) {'
                    ' fprintf(stderr, "  [ABIW] in %08X: call to ' + tgt
                    + ' esp %%+d (normally %%+d) esi %%08X->%%08X ebx %%08X->%%08X edi %%08X->%%08X'
                    + BS + 'n", d_, first_, s0_, g_esi, b0_, g_ebx, d0_, g_edi); fflush(stderr); } } }') % va
        pat = re.compile(r'[^\n]*RECOMP_ABI_CALL\(0x([0-9A-Fa-f]{8})u, sub_[0-9A-Fa-f]+\);[^\n]*')
        body2 = pat.sub(wrap, body)
        icalls = len(re.findall(r'RECOMP_ICALL', body))
        text = text[:i] + body2 + text[j:]
        if '#include <stdio.h>' not in text[:3000]:
            text = '#include <stdio.h>' + nl + text
        open(path, 'wb').write(text.encode('utf-8'))
        print('sub_%08X in %s: wrapped %d direct calls (%d indirect call sites not wrapped)'
              % (va, os.path.basename(path), count[0], icalls))
        return
    print('sub_%08X not found' % va)

def clean(gen):
    for path in sorted(glob.glob(os.path.join(gen, 'recomp_0*.c'))):
        text = open(path, 'rb').read().decode('utf-8')
        new, n = unwrap(text)
        if n:
            open(path, 'wb').write(new.encode('utf-8'))
            print('%s: removed %d' % (os.path.basename(path), n))

if __name__ == '__main__':
    if sys.argv[2:] == ['--clean']:
        clean(sys.argv[1])
    else:
        for arg in sys.argv[2:]:
            wrap_function(sys.argv[1], int(arg, 16))
