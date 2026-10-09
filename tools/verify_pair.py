"""pair.py <dir> <out.png> <idx> [crop x0,y0,x1,y1] [scale]
software | GL | amplified difference for dump <idx> of a verify run"""
import sys, os
from PIL import Image, ImageChops, ImageDraw
d, out, i = sys.argv[1], sys.argv[2], int(sys.argv[3])
a = Image.open(os.path.join(d, "fb%05d.bmp" % i)).convert("RGB")
b = Image.open(os.path.join(d, "fb%05d_gl.bmp" % i)).convert("RGB")
if len(sys.argv) > 4 and sys.argv[4] != "-":
    box = tuple(int(v) for v in sys.argv[4].split(","))
    a, b = a.crop(box), b.crop(box)
sc = int(sys.argv[5]) if len(sys.argv) > 5 else 1
df = ImageChops.difference(a, b).point(lambda v: min(255, v * 8))
ims = [a, b, df]
if sc > 1:
    ims = [m.resize((m.size[0] * sc, m.size[1] * sc), Image.NEAREST) for m in ims]
w, h = ims[0].size
sheet = Image.new("RGB", (w * 3, h + 16), (30, 30, 30))
dr = ImageDraw.Draw(sheet)
for k, (m, name) in enumerate(zip(ims, ("software", "GPU", "difference x8"))):
    sheet.paste(m, (k * w, 16))
    dr.text((k * w + 4, 2), name, fill=(255, 255, 0))
sheet.save(out)
print("wrote", out, sheet.size)
