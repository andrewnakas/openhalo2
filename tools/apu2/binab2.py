"""binab2.py <dir> [from_s] : per-bin RMS, oracle vs new, over the oracle's own
report periods. The oracle prints '[APU] out: ... N of M blocks' and
'[APU] mixbin energy: b:e ...' per period; the new model dumps every block's
per-bin energy (RECOMP_MA_BINDUMP=<dir>/bins.f32). The two streams are
aligned with the lag between mix.wav and new.wav (tools/apu_ab.py). The
oracle's energy unit is fitted on bins 0/1/2/6/7 (one constant for the run).
Prints per bin: periods above -60 dBFS, share within +-0.5 dB, median diff."""
import sys, os, re
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import apu_ab

d = sys.argv[1]
f0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0
log = open(os.path.join(d, 'out.log'), errors='replace').read().split('\n')
periods = []          # (blocks in period, {bin: energy})
pend = None
for ln in log:
    m = re.search(r'\[APU\] out: .* (\d+) of (\d+) blocks', ln)
    if m:
        pend = int(m.group(2))
        continue
    m = re.search(r'\[APU\] mixbin energy:(.*)', ln)
    if m and pend is not None:
        e = {int(b): float(x) for b, x in re.findall(r' (\d+):([0-9.]+(?:e[+-]?\d+)?)', m.group(1))}
        periods.append((pend, e))
        pend = None
bins = np.fromfile(os.path.join(d, 'bins.f32'), dtype='<f4').reshape(-1, 32)
a = apu_ab.load(os.path.join(d, 'mix.wav'))
b = apu_ab.load(os.path.join(d, 'new.wav'))
lag = apu_ab.best_lag(a[int(f0 * 48000):], b[int(f0 * 48000):])
# the WAV taps start at the first block (FROM_START) and so does the dump
lagb = lag / 256.0
rows = []
start = 0
for nb, e in periods:
    s0 = start + lagb
    s1 = s0 + nb
    start += nb
    if s0 < f0 * 187.5 or s1 > len(bins) or s0 < 0:
        continue
    mine = bins[int(round(s0)):int(round(s1))].sum(0) / (nb * 256.0)   # mean square, 1.0 = FS
    rows.append((e, mine))
if not rows:
    print('no periods')
    sys.exit()
# oracle unit: mine_ms * K = oracle_e ; fit K on the main bins
ks = [r[0][k] / r[1][k] for r in rows for k in (0, 1, 2, 6, 7) if r[0].get(k, 0) > 0 and r[1][k] > 1e-7]
K = float(np.median(ks))
print('%d periods (lag %d samples), oracle unit = new mean square x %.1f' % (len(rows), lag, K))
for k in sorted(set(x for r in rows for x in r[0])):
    ds = []
    for e, mine in rows:
        if e.get(k, 0) / K < 1e-6:
            continue
        ds.append(10 * np.log10(max(mine[k], 1e-12) * K / e[k]))
    if ds:
        ds = np.array(ds)
        print('bin %2d: %3d periods, within 0.5 dB %5.1f%%, median %+.2f dB, p10 %+.2f, p90 %+.2f' % (
            k, len(ds), 100 * (np.abs(ds) <= 0.5).mean(), np.median(ds), np.percentile(ds, 10), np.percentile(ds, 90)))
