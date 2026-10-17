"""binab.py <out.log> [scale] : per-bin energy, oracle ([APU] mixbin energy) vs
the new model ([MA] mixbin energy), paired by report index (both print every
~5 s from their own start). The two print in different units; scale is the
ratio new/oracle for identical signals (default: the median over all pairs
of bins 0 and 1). Prints, per bin, the share of periods within +-0.5 dB and
the median difference, for periods where the oracle's bin is above -60 dBFS."""
import sys, re
import numpy as np

a, n = [], []
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[(APU|MA)\] mixbin energy:(.*)', line)
    if not m:
        continue
    d = {int(b): float(e) for b, e in re.findall(r' (\d+):([0-9.]+(?:e[+-]?\d+)?)', m.group(2))}
    (a if m.group(1) == 'APU' else n).append(d)
k = min(len(a), len(n))
pairs = list(zip(a[:k], n[:k]))
if len(sys.argv) > 2:
    scale = float(sys.argv[2])
else:
    r = [p[1][b] / p[0][b] for p in pairs for b in (0, 1) if p[0].get(b, 0) > 1 and p[1].get(b, 0) > 0]
    scale = float(np.median(r)) if r else 1.0
print('%d periods, scale %.3f' % (k, scale))
# the new model's energy unit is mean square of 16-bit values / 1000
fs = 32768.0 ** 2 / 1000.0
bins = sorted(set(b for p in pairs for b in p[0]))
for b in bins:
    ds = []
    for o, w in pairs:
        if o.get(b, 0) * scale < fs * 1e-6:       # -60 dBFS
            continue
        ds.append(10 * np.log10(max(w.get(b, 1e-9), 1e-9) / (o[b] * scale)))
    if ds:
        ds = np.array(ds)
        print('bin %2d: %3d periods, within 0.5 dB %5.1f%%, median %+.2f dB, p10 %+.2f, p90 %+.2f' % (
            b, len(ds), 100 * (np.abs(ds) <= 0.5).mean(), np.median(ds), np.percentile(ds, 10), np.percentile(ds, 90)))
