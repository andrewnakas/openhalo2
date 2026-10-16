"""shotlevel.py <run dir>...: gunfire level against the bed before it.

Reads <dir>/mix.wav and <dir>/fire.txt (seconds of shot start, end) from
gunfire.sh. Prints bed RMS (5 s before), shot-window RMS and peak, the
per-second peaks over the 12 pulls, and the ratio in dB."""
import sys, wave
import numpy as np

for d in sys.argv[1:]:
    try:
        w = wave.open(d + '/mix.wav', 'rb')
        t = [float(x) for x in open(d + '/fire.txt').read().split()]
    except Exception as e:
        print(d, 'missing:', e)
        continue
    sr = w.getframerate()
    a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2).astype(float)
    t0 = int(t[0] * sr)
    bed = a[max(0, t0 - 5 * sr):t0]
    sh = a[t0:t0 + 13 * sr]
    rms = lambda x: np.sqrt((x ** 2).mean()) if len(x) else 0.0
    peaks = [int(np.abs(sh[i * sr:(i + 1) * sr]).max()) for i in range(len(sh) // sr)]
    rb, rs = rms(bed), rms(sh)
    print('%s: bed rms %.0f | shots rms %.0f peak %d (%.1f dB over bed) | clipped %d'
          % (d, rb, rs, int(np.abs(sh).max()) if len(sh) else 0,
             20 * np.log10((rs + 1) / (rb + 1)), int((np.abs(sh) >= 32767).sum())))
    print('   per-second peaks:', peaks)
