"""fitparts.py <dir> <from_s> part.wav ... : least-squares fit, per 1 s window
and channel, oracle mix.wav ~ sum_k g_k * part_k (shadow taps of one run,
RECOMP_MA_ONLY=...). Prints each part's median gain in dB over the windows
where that part carries at least 10% of the energy."""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import apu_ab

d = sys.argv[1]
f0 = float(sys.argv[2])
names = sys.argv[3:]
o = apu_ab.load(os.path.join(d, 'mix.wav'))
parts = [apu_ab.load(os.path.join(d, n)) for n in names]
n = min(len(p) for p in parts)
tot = sum(p[:n] for p in parts)
W = 48000
G = apu_ab.best_lag(o[int(f0 * W):], tot[int(f0 * W):])
print('global lag', G)
gains = [[] for _ in parts]
errs = []
for s0 in range(int(f0 * W), len(o) - 2 * W, W):
    for ch in (0, 1):
        y = o[s0:s0 + W, ch]
        if y.std() < 30:
            continue
        best = None
        for lag in range(-300, 301):
            a = s0 + G + lag
            if a < 0 or a + W > n:
                continue
            X = np.stack([p[a:a + W, ch] for p in parts], 1)
            c, *_ = np.linalg.lstsq(X, y, rcond=None)
            err = ((X @ c - y) ** 2).sum() / (y ** 2).sum()
            if best is None or err < best[0]:
                best = (err, c, X)
            if err < 0.002:
                break
        if best is None:
            continue
        errs.append(best[0])
        if best[0] > 0.05:
            continue
        e = (best[2] ** 2).sum(0)
        for k in range(len(parts)):
            if e[k] > 0.1 * e.sum():
                gains[k].append(best[1][k])
for k, nm in enumerate(names):
    g = np.array(gains[k])
    if len(g):
        print('%-12s %3d windows: gain median %.3f (%+.2f dB), p10 %.3f, p90 %.3f' % (
            nm, len(g), np.median(g), 20 * np.log10(abs(np.median(g))), np.percentile(g, 10), np.percentile(g, 90)))
print('residual energy / oracle energy: median %.4f, mean %.4f over %d windows' % (np.median(errs), np.mean(errs), len(errs)))
