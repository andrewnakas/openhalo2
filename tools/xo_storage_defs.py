"""The title-storage transfers Halo 2 knows (dirty room: reads default.xbe,
prints facts only).

Each transfer request (s_pending_message_header, unknown_08e1e0.cpp) points at
a definition {kind, wide file name, version, directory, retry, sent,
received} (unknown_080f30.cpp / unknown_08e1e0.cpp). This scans .data for
request headers whose first dword points at such a definition (a pointer to
a wide string ending in ".dat"), and prints kind, file, version, callbacks,
the request's transfer kind and buffer size.

usage: python tools/xo_storage_defs.py
"""
import struct

f = open('D:/n64work/halo2/dirty/game/default.xbe', 'rb').read()
base = struct.unpack_from('<I', f, 0x104)[0]
nsec = struct.unpack_from('<I', f, 0x11C)[0]
sh = struct.unpack_from('<I', f, 0x120)[0] - base
secs = []
for i in range(nsec):
    fl, v, vs, ra, rs = struct.unpack_from('<5I', f, sh + i * 56)
    secs.append((v, vs, ra, rs))


def rd(va, n):
    for v, vs, ra, rs in secs:
        if v <= va < v + vs:
            off = va - v
            b = f[ra + off: ra + min(off + n, rs)]
            return b + b'\0' * (n - len(b))
    return None


def wstr(va):
    b = rd(va, 256)
    if not b:
        return None
    s = b.decode('utf-16-le', 'replace').split('\0')[0]
    return s


defs = {}
for v, vs, ra, rs in secs:
    for off in range(0, vs - 32, 4):
        d = struct.unpack_from('<I', rd(v + off, 4))[0]
        b = rd(d, 28) if 0x10000 < d < 0x600000 else None
        if not b:
            continue
        kind, name, ver, dirp, retry, sent, recv = struct.unpack('<7I', b)
        if not (0 <= kind < 8 and 0x10000 < name < 0x600000):
            continue
        s = wstr(name)
        if not s or not s.endswith('.dat'):
            continue
        hdr = rd(v + off, 0x28)
        flags, u06, rkind, state = struct.unpack_from('<HhhH', hdr, 4)
        size = struct.unpack_from('<I', hdr, 0x24)[0]
        print(f"request @{v + off:08X}: def @{d:08X} kind={kind} file={s!r} version={ver} "
              f"dir={dirp:08X} retry={retry:08X} sent={sent:08X} received={recv:08X} "
              f"transfer_kind={rkind} size=0x{size:X}")
