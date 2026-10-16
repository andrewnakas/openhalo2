"""pools.py [filter] : list the title's data arrays (signature 0x64407440) in the
running halo2_recomp.exe with max / live / high-water counts. Scans guest RAM
0..64 MB through peekmem.read. filter = substring of the pool name."""
import struct, sys
import ctypes, subprocess, sys
from ctypes import wintypes as W
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = W.HANDLE

def pid_of():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq halo2_recomp.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    return int(out.split(',')[1].strip('"'))

def read(va, n):
    h = k32.OpenProcess(0x10, False, pid_of())
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(W.HANDLE(h), ctypes.c_void_p(va + 0x10000), buf, n, ctypes.byref(got))
    k32.CloseHandle(W.HANDLE(h))
    if not ok: sys.exit('read failed at %X' % va)
    return buf.raw[:got.value]
SIG = struct.pack('<I', 0x64407440)
flt = sys.argv[1] if len(sys.argv) > 1 else ''
if flt == 'fx':
    # effect pools by their global pointers (decomp names, retail VAs)
    G = {'effect': 0x4ea93c, 'effect location': 0x4ea938, 'particle_system': 0x510c74,
         'particles': 0x51ec84, 'particle_emitter': 0x51ec88, 'particle_location': 0x51ec8c,
         'contrail': 0x4ea944, 'contrail point': 0x4ea940, 'decals': 0x4ea950}
    for n, g in G.items():
        p = struct.unpack('<I', read(g, 4))[0]
        try:
            b = read(p, 0x4c)
            mx, sz = struct.unpack_from('<ii', b, 0x20); ff, hw, live = struct.unpack_from('<iii', b, 0x34)
            print(f'{n:18s} @{p:08X} name {b[:32].split(bytes(1))[0].decode(errors="replace"):18s} max {mx:5d} live {live:5d} hw {hw:5d}')
        except Exception as e:
            print(f'{n:18s} @{p:08X} unreadable {e}')
    sys.exit()
CH = 1 << 20
for base in range(0, 64 << 20, CH):
    try: b = read(base, CH + 0x50)
    except Exception: continue
    i = b.find(SIG)
    while 0 <= i < CH:
        h = i - 0x2c
        if h >= 0:
            name = b[h:h+32].split(b'\0')[0]
            if name and all(32 <= c < 127 for c in name):
                mx, sz = struct.unpack_from('<ii', b, h + 0x20)
                ff, hw, live = struct.unpack_from('<iii', b, h + 0x34)
                n = name.decode()
                if flt in n:
                    print(f'{base+h:08X} {n:24s} max {mx:5d} size {sz:5d} live {live:5d} hw {hw:5d}')
        i = b.find(SIG, i + 4)
