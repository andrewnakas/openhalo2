"""Count MMX/SSE-integer mnemonics (any instruction with an mm/xmm operand) in a VA range of the XBE.
usage: python mmx_census.py <lo> <hi> (hex)"""
import struct, sys, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
f = open('D:/n64work/halo2/dirty/game/default.xbe', 'rb').read()
base = struct.unpack_from('<I', f, 0x104)[0]
nsec = struct.unpack_from('<I', f, 0x11C)[0]
sh = struct.unpack_from('<I', f, 0x120)[0] - base
lo = int(sys.argv[1], 16); hi = int(sys.argv[2], 16)
for i in range(nsec):
    fl, v, vs, ra, rs = struct.unpack_from('<5I', f, sh + i * 56)
    if v <= lo < v + vs:
        data = f[ra + lo - v: ra + min(hi, v + vs) - v]
c = collections.Counter(); ex = {}
md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
for i in md.disasm(data, lo):
    if 'mm' in i.op_str or i.mnemonic in ('emms',):
        k = i.mnemonic + (' [mm]' if ('mm' in i.op_str and 'xmm' not in i.op_str) else '')
        c[k] += 1; ex.setdefault(k, '%08X %s %s' % (i.address, i.mnemonic, i.op_str))
for k, n in c.most_common():
    print('%6d  %-22s %s' % (n, k, ex[k]))
