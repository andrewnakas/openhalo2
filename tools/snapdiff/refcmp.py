"""For each snapshot: Unicorn-decode the frame, find which movie/frame it is (packet bytes
in the .bik), decode the same frame with PyAV, report Y-plane error and save a sheet."""
import sys, struct, bisect, numpy as np, av
from PIL import Image
from snap import load, unicorn_run
from planes import planes_rgb
from unicorn import x86_const as X, UC_HOOK_CODE
BINK = 'D:/n64work/halo2/dirty/game/bink/'
movies = {}
for n in ('intro_60.bik', 'attract_60.bik'):
    d = open(BINK + n, 'rb').read()
    nf = struct.unpack_from('<I', d, 8)[0]; na = struct.unpack_from('<I', d, 40)[0]
    offs = [x & ~1 for x in struct.unpack_from('<%dI' % (nf + 1), d, 44 + na * 12)]
    movies[n] = (d, offs)
refcache = {}
def ref_frames(name, upto):
    key = name
    if key not in refcache or len(refcache[key]) <= upto:
        c = av.open(BINK + name); out = []
        for i, fr in enumerate(c.decode(video=0)):
            out.append(fr.to_ndarray(format='yuv420p'))
            if i >= upto + 2: break
        refcache[key] = out
    return refcache[key]
rows = []
for path in sys.argv[1:]:
    s = load(path)
    args = []
    def h(uc, buf):
        def c(u, a, sz, d):
            e = u.reg_read(X.UC_X86_REG_ESP); args.append(struct.unpack('<10I', bytes(u.mem_read(e, 40))))
        uc.hook_add(UC_HOOK_CODE, c, begin=0x3E6AE0, end=0x3E6AE0)
    pk = struct.unpack_from('<I', bytes(s['ram'][s['esp'] + 40:s['esp'] + 44]))[0]
    c0 = bytes(s['contig'])
    pkt = c0[pk - 0x80000000:pk - 0x80000000 + 64]
    uc, buf, ret = unicorn_run(s, hooks=h)
    c1 = np.frombuffer(s['_cbuf'].raw, dtype=np.uint8)
    found = None
    for n, (d, offs) in movies.items():
        i = d.find(pkt)
        if i >= 0:
            found = (n, bisect.bisect_right(offs, i) - 1); break
    y, u, v = args[0][1], args[1][1], args[2][1]
    Y = c1[y - 0x80000000:][:640 * 480].reshape(480, 640)
    line = '%s packet %08X -> %s' % (path.split('/')[-1], pk, found)
    if found:
        ref = ref_frames(found[0], found[1])[found[1]]
        R = ref[:480]
        err = np.abs(Y.astype(int) - R.astype(int))
        bad = (err.reshape(60, 8, 80, 8).max(axis=(1, 3)) > 24).sum()
        line += '  Y mean abs err %.2f, 8x8 blocks off by >24: %d' % (err.mean(), bad)
        if len(sys.argv) == 2 or bad:
            im = planes_rgb(c1, y, u, v)
            Rimg = Image.fromarray(np.stack([R] * 3, -1))
            sheet = Image.new('RGB', (1280, 480)); sheet.paste(im, (0, 0)); sheet.paste(Image.fromarray(np.stack([Y]*3,-1)), (640, 0))
            sheet.save('sheet_%s.png' % path.split('_')[-2])
    print(line, flush=True)
