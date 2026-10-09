"""wavcheck.py <mix.wav> [out.png]: is this sound or noise, and is it whole?

Prints per-second peak/RMS, the share of energy below 4 kHz (programme
material sits there; white noise has a sixth of its energy there at 48 kHz),
spectral flatness (noise -> 1, music/speech -> near 0), left/right
correlation, clipping, and the gaps: runs of exact digital silence that
start and end inside sound (a dropout is a short run; a pause between
sounds is long and has a decay before it). With a PNG name, a spectrogram.
"""
import sys, wave
import numpy as np

w = wave.open(sys.argv[1], 'rb')
sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
a = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch).astype(np.float64)
print('%d Hz, %d ch, %.1f s' % (sr, ch, n / sr))
mono = a.mean(axis=1)
for s in range(0, n, sr * 5):
    seg = a[s:s + sr * 5]
    print('  %3d s: peak %5d rms %6.0f' % (s // sr, np.abs(seg).max(), np.sqrt((seg ** 2).mean())))
print('clipped samples (|x| >= 32767): %d' % int((np.abs(a) >= 32767).sum()))

# spectrum
N = 4096
frames = [mono[i:i + N] * np.hanning(N) for i in range(0, len(mono) - N, N)]
P = np.array([np.abs(np.fft.rfft(f)) ** 2 for f in frames]) + 1e-9
tot = P.sum(axis=1)
loud = tot > tot.max() * 1e-4
low = P[:, :int(4000 / (sr / N))].sum(axis=1) / tot
flat = np.exp(np.log(P).mean(axis=1)) / P.mean(axis=1)
print('energy below 4 kHz: median %.2f (white noise 0.17)' % np.median(low[loud]))
print('spectral flatness: median %.3f (white noise ~0.56, tonal/speech < 0.1)' % np.median(flat[loud]))
if ch == 2:
    l, r = a[:, 0], a[:, 1]
    print('left/right correlation: %.2f; identical channels: %s' % (
        np.corrcoef(l, r)[0, 1], bool((l == r).all())))

# gaps of exact silence
z = (np.abs(a).max(axis=1) == 0)
runs = []
i = 0
while i < n:
    if z[i]:
        j = i
        while j < n and z[j]:
            j += 1
        runs.append((i, j - i))
        i = j
    else:
        i += 1
inner = [(s, l) for s, l in runs if s > 0 and s + l < n and l >= 16]
short = [(s, l) for s, l in inner if l < sr // 10]
print('silent runs of 16+ samples inside the file: %d (under 100 ms: %d); total silence %.1f%%' % (
    len(inner), len(short), 100.0 * z.mean()))
hist = {}
for s, l in inner:
    k = (l // 32) * 32
    hist[k] = hist.get(k, 0) + 1
print('  run lengths (samples: count):', sorted(hist.items())[:12])
# A dropout is a jump into silence: the sample before the run is far from 0.
jumps = [abs(mono[s - 1]) for s, l in inner]
if jumps:
    print('  level of the sample before each silent run: median %.0f, max %.0f' % (np.median(jumps), max(jumps)))
# Discontinuities anywhere: sample-to-sample steps against the local level.
d = np.abs(np.diff(mono))
print('largest sample-to-sample step: %.0f; steps over 8000: %d' % (d.max(), int((d > 8000).sum())))

if len(sys.argv) > 2:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 1, figsize=(12, 6))
    ax[0].plot(np.arange(n) / sr, mono, lw=0.3)
    ax[0].set_ylabel('mix'); ax[0].set_xlim(0, n / sr)
    ax[1].specgram(mono, NFFT=1024, Fs=sr, noverlap=512, cmap='magma', vmin=-40)
    ax[1].set_ylabel('Hz'); ax[1].set_xlabel('s')
    fig.tight_layout(); fig.savefig(sys.argv[2], dpi=80)
    print('wrote', sys.argv[2])
