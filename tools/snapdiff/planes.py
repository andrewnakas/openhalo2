import sys, numpy as np
from PIL import Image
from snap import load
def planes_rgb(c, y, u, v, w=640, h=480):
    Y = c[y-0x80000000:][:w*h].reshape(h, w).astype(np.float32)
    U = c[u-0x80000000:][:w*h//4].reshape(h//2, w//2).repeat(2,0).repeat(2,1).astype(np.float32) - 128
    V = c[v-0x80000000:][:w*h//4].reshape(h//2, w//2).repeat(2,0).repeat(2,1).astype(np.float32) - 128
    Yc = (Y - 16) * 1.164
    rgb = np.stack([Yc + 1.596*V, Yc - 0.392*U - 0.813*V, Yc + 2.017*U], -1)
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
if __name__ == '__main__':
    s = load(sys.argv[1])
    planes_rgb(s['contig'], int(sys.argv[2],16), int(sys.argv[3],16), int(sys.argv[4],16)).save(sys.argv[5])
