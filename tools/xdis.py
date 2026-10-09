"""Disassemble a range of the XBE: python xdis.py <va> <len> (hex)."""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
f = open('D:/n64work/halo2/dirty/game/default.xbe', 'rb').read()
base = struct.unpack_from('<I', f, 0x104)[0]
nsec = struct.unpack_from('<I', f, 0x11C)[0]
sh = struct.unpack_from('<I', f, 0x120)[0] - base
def rd(va, n):
    for i in range(nsec):
        fl, v, vs, ra, rs = struct.unpack_from('<5I', f, sh + i * 56)
        if v <= va < v + vs:
            return f[ra + va - v: ra + va - v + n]
    return b''
va = int(sys.argv[1], 16); n = int(sys.argv[2], 16)
for i in Cs(CS_ARCH_X86, CS_MODE_32).disasm(rd(va, n), va):
    print('%08X %-8s %s' % (i.address, i.mnemonic, i.op_str))
