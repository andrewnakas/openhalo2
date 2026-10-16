"""apu_ab.py <oracle.wav> <new.wav> [from_s] [to_s] : compare two APU mixes.

Both are 48 kHz stereo 16-bit (RECOMP_APU_WAV / RECOMP_MA_WAV of one shadow
run, or two separate runs). Finds the lag that best aligns them, then per
1 s window: RMS of each side in dBFS and the difference, and the correlation
of the aligned signals. Prints the acceptance numbers of
docs/APU_CLEANROOM_DESIGN.md 3d that a mix comparison can give:
  - windows above -60 dBFS whose RMS is within +-0.5 dB (per channel)
  - correlation (median, and share of windows >= 0.95 / 0.98)
  - clicks per second (tools/clicks.py rule) and periodic spacing
  - digital-silence runs inside sound, clipped samples
"""
import sys, wave
import numpy as np


def load(p):
    with wave.open(p) as w:
        n = w.getnframes()
        x = np.frombuffer(w.readframes(n), '<i2').astype(np.float64).reshape(-1, w.getnchannels())
    return x


def best_lag(a, b, maxlag=48000 * 2):
    """lag such that a[t] ~ b[t + lag]; on a mono, decimated envelope first,
    then refined on the raw signal."""
    n = min(len(a), len(b), 48000 * 60)
    am, bm = a[:n].mean(1), b[:n].mean(1)
    # coarse on full-rate signal with FFT
    N = 1
    while N < 2 * n:
        N *= 2
    A = np.fft.rfft(am, N)
    B = np.fft.rfft(bm, N)
    c = np.fft.irfft(np.conj(A) * B, N)
    lags = np.concatenate([np.arange(0, maxlag), np.arange(-maxlag, 0)])
    vals = np.concatenate([c[:maxlag], c[-maxlag:]])
    return int(lags[np.argmax(vals)])


def db(x):
    return 20 * np.log10(max(x, 1e-9) / 32768.0)


def clicks(x):
    from scipy.ndimage import median_filter
    d = np.diff(x)
    a = np.abs(d)
    local = median_filter(a, size=65, mode='nearest')
    cand = a > 6 * local + 300
    nxt = np.roll(d, -1)
    jump = cand & ((np.sign(nxt) != np.sign(d)) | (np.abs(nxt) < 0.5 * a))
    return np.nonzero(jump)[0]


def silent_runs(x):
    m = (np.abs(x).max(1) == 0)
    runs = []
    i = 0
    n = len(m)
    edges = np.diff(np.concatenate([[0], m.astype(np.int8), [0]]))
    starts = np.nonzero(edges == 1)[0]
    ends = np.nonzero(edges == -1)[0]
    for s, e in zip(starts, ends):
        if s > 0 and e < n:
            runs.append(e - s)
    return runs


def main():
    args = [x for x in sys.argv[1:] if not x.startswith('-')]
    a = load(args[0])
    b = load(args[1])
    f0 = float(args[2]) if len(args) > 2 else 0
    f1 = float(args[3]) if len(args) > 3 else 1e9
    lag = best_lag(a[int(f0 * 48000):], b[int(f0 * 48000):])
    print('lengths %.1f s / %.1f s, lag %d samples (%.1f ms: new = oracle shifted)' % (
        len(a) / 48000, len(b) / 48000, lag, lag / 48.0))
    if lag >= 0:
        b2 = b[lag:]
        a2 = a
    else:
        a2 = a[-lag:]
        b2 = b
    n = min(len(a2), len(b2))
    a2, b2 = a2[:n], b2[:n]
    s0, s1 = int(f0 * 48000), min(n, int(f1 * 48000))
    W = 48000
    rows = []
    for s in range(s0, s1 - W + 1, W):
        wa, wb = a2[s:s + W], b2[s:s + W]
        ra = np.sqrt((wa ** 2).mean(0))
        rb = np.sqrt((wb ** 2).mean(0))
        ma, mb = wa.mean(1), wb.mean(1)
        cc, lg = np.nan, 0
        if ma.std() > 1 and mb.std() > 1:
            # best local lag within +-50 ms (the two models' voice starts are
            # asynchronous to each other by up to a block)
            L = 2400
            N = 1
            while N < 2 * W + 2 * L:
                N *= 2
            ext = b2[max(0, s - L):s + W + L].mean(1)
            off0 = s - max(0, s - L)
            c = np.fft.irfft(np.conj(np.fft.rfft(ma, N)) * np.fft.rfft(ext, N), N)
            k = int(np.argmax(c[:len(ext) - W + 1])) if len(ext) >= W else off0
            seg = ext[k:k + W]
            if len(seg) == W:
                cc = np.corrcoef(ma, seg)[0, 1]
                lg = k - off0
        rows.append((s / 48000, [db(x) for x in ra], [db(x) for x in rb], cc, lg))
    loud = [r for r in rows if max(r[1]) > -60]
    within = 0
    tot = 0
    dl = []
    for r in loud:
        for c in range(2):
            if r[1][c] > -60:
                tot += 1
                d = r[2][c] - r[1][c]
                dl.append(d)
                if abs(d) <= 0.5:
                    within += 1
    ccs = np.array([r[3] for r in loud if not np.isnan(r[3])])
    print('windows %d (loud %d): RMS within 0.5 dB %d/%d (%.1f%%), diff median %+.2f dB, p10 %+.2f, p90 %+.2f' % (
        len(rows), len(loud), within, tot, 100.0 * within / max(tot, 1),
        np.median(dl) if dl else 0, np.percentile(dl, 10) if dl else 0, np.percentile(dl, 90) if dl else 0))
    if len(ccs):
        print('correlation: median %.3f, >=0.98 %.1f%%, >=0.95 %.1f%%' % (
            np.median(ccs), 100 * (ccs >= 0.98).mean(), 100 * (ccs >= 0.95).mean()))
    for name, x in (('oracle', a2[s0:s1]), ('new', b2[s0:s1])):
        cl = clicks(x.mean(1))
        sp = np.diff(cl)
        common = np.bincount(sp[sp < 200]).argmax() if len(sp[sp < 200]) else 0
        runs = silent_runs(x)
        short = [r for r in runs if r < 4800]
        print('%-6s clicks %.3f/s (common spacing %d), inner silent runs <100ms %d, clipped %d, rms %.1f dBFS' % (
            name, len(cl) / max((s1 - s0) / 48000, 1e-9), common, len(short),
            int((np.abs(x) >= 32767).sum()), db(np.sqrt((x ** 2).mean()))))
    if '-v' in sys.argv:
        for r in rows:
            print('  %6.1f s  oracle %6.1f %6.1f  new %6.1f %6.1f  corr %s lag %+d' % (
                r[0], r[1][0], r[1][1], r[2][0], r[2][1], '%.3f' % r[3] if not np.isnan(r[3]) else '-', r[4]))


if __name__ == '__main__':
    main()
