"""peekmem.py <guest va hex> <bytes> <out file> : read guest memory of the
running halo2_recomp.exe (host address = guest VA + 0x10000).
peekmem.py tex <guest va hex> <w> <h> <fmt> <out.png> : decode a swizzled
texture (fmt 1a = A8L8, 19 = A8, 00 = L8, 06 = A8R8G8B8) into a PNG; for A8L8
the picture is L on the left and A on the right."""
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

def swz(x, y, w, h):
    o = 0; bit = 0; sh = 0
    while (1 << bit) < max(w, h):
        m = 1 << bit
        if m < w: o |= ((x >> bit) & 1) << sh; sh += 1
        if m < h: o |= ((y >> bit) & 1) << sh; sh += 1
        bit += 1
    return o

if sys.argv[1] == 'tex':
    from PIL import Image
    va = int(sys.argv[2], 16); w = int(sys.argv[3]); h = int(sys.argv[4]); fmt = int(sys.argv[5], 16)
    bpp = {0x1a: 2, 0x19: 1, 0x00: 1, 0x01: 1, 0x06: 4, 0x07: 4}[fmt]
    d = read(va, w * h * bpp)
    if fmt == 0x1a:
        im = Image.new('L', (w * 2, h))
        for y in range(h):
            for x in range(w):
                i = swz(x, y, w, h) * 2
                im.putpixel((x, y), d[i]); im.putpixel((w + x, y), d[i + 1])
        ls = sorted(set(d[0::2])); print('L values: min %d max %d, %d distinct; corner L=%d A=%d' % (ls[0], ls[-1], len(ls), d[0], d[1]))
    elif bpp == 1:
        im = Image.new('L', (w, h))
        for y in range(h):
            for x in range(w): im.putpixel((x, y), d[swz(x, y, w, h)])
    else:
        im = Image.new('RGBA', (w, h))
        for y in range(h):
            for x in range(w):
                i = swz(x, y, w, h) * 4
                im.putpixel((x, y), (d[i + 2], d[i + 1], d[i], d[i + 3]))
    im.save(sys.argv[6]); print('wrote', sys.argv[6])
else:
    open(sys.argv[3], 'wb').write(read(int(sys.argv[1], 16), int(sys.argv[2])))
