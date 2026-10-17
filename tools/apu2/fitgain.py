"""fitgain.py <dir> [from_s] : least-squares fit, per 1 s window and channel,
oracle mix ~ a * dry.wav + b * hrtf.wav (two shadows of one run with
RECOMP_MA_ONLY=nohrtf / hrtf). Prints the median a and b in dB: how far each
part of the new model's mix is from the oracle's level."""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import apu_ab

d = sys.argv[1]
f0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0
o = apu_ab.load(os.path.join(d, 'mix.wav'))
dr = apu_ab.load(os.path.join(d, 'dry.wav'))
hr = apu_ab.load(os.path.join(d, 'hrtf.wav'))


def align(a, b):
    lag = apu_ab.best_lag(a[int(f0 * 48000):], b[int(f0 * 48000):])
    return lag


W = 48000
G = apu_ab.best_lag(o[int(f0 * W):], (dr + hr)[int(f0 * W):])
print('global lag', G)
res = {0: [], 1: []}
for s0 in range(int(f0 * W), len(o) - 2 * W, W):
    seg = o[s0:s0 + W]
    for ch in (0, 1):
        y = seg[:, ch]
        if y.std() < 30:
            continue
        best = None
        for which in (dr, hr):
            pass
        # local alignment of the components to the oracle window (+-600 samples)
        best = None
        for lag in range(-300, 301, 1):
            a = s0 + G + lag
            if a < 0 or a + W > min(len(dr), len(hr)):
                continue
            x1 = dr[a:a + W, ch]
            x2 = hr[a:a + W, ch]
            X = np.stack([x1, x2], 1)
            coef, r, *_ = np.linalg.lstsq(X, y, rcond=None)
            err = ((X @ coef - y) ** 2).sum() / (y ** 2).sum()
            if best is None or err < best[0]:
                best = (err, coef, lag)
            if best[0] < 0.002:
                break
        if best and best[0] < 0.1:
            res[ch].append((s0 / W, best[1][0], best[1][1], best[0], np.sqrt((hr[s0:s0 + W, ch] ** 2).mean())))
for ch in (0, 1):
    r = res[ch]
    if not r:
        continue
    a = np.array([x[1] for x in r]); b = np.array([x[2] for x in r]); h = np.array([x[4] for x in r])
    sig = h > 50
    print('ch %d: %d windows; dry gain median %.3f (%+.2f dB); hrtf gain median %.3f (%+.2f dB) over %d windows with hrtf content' % (
        ch, len(r), np.median(a), 20 * np.log10(abs(np.median(a))), np.median(b[sig]) if sig.any() else 0,
        20 * np.log10(abs(np.median(b[sig]))) if sig.any() else 0, sig.sum()))
