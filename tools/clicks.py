"""clicks.py <wav> [<wav> ...] : clicks per second in a 16-bit WAV, one line per file.

A click is a sample step much larger than the signal's local slope: |d[i]| above
6x the median |d| of the 65 samples around it plus 300 (on a 16-bit scale), with
the next step back in the opposite sense or the run smooth again (a jump, not a
loud transient). Also prints the most common spacing between clicks, in samples,
which names a block size when the clicks are periodic (Halo CE: 64 at 44.1 kHz,
i.e. ~70 at the 48 kHz output), and the click rate in loud vs quiet passages.
"""
import sys
import wave

import numpy as np
from scipy.ndimage import median_filter


def load(path):
    with wave.open(path) as w:
        ch, rate = w.getnchannels(), w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.float64)
    return x.reshape(-1, ch), rate


def clicks(x):
    d = np.diff(x)
    a = np.abs(d)
    local = median_filter(a, size=65, mode="nearest")
    cand = a > 6 * local + 300
    # a jump: the step after it does not continue the same way (not a steep edge)
    nxt = np.roll(d, -1)
    jump = cand & ((np.sign(nxt) != np.sign(d)) | (np.abs(nxt) < 2 * local + 100))
    idx = np.flatnonzero(jump)
    if len(idx):  # merge neighbours within 4 samples
        idx = idx[np.concatenate([[True], np.diff(idx) > 4])]
    return idx


def main():
    for p in sys.argv[1:]:
        x, rate = load(p)
        secs = len(x) / rate
        allidx = []
        for c in range(x.shape[1]):
            allidx.append(clicks(x[:, c]))
        idx = np.unique(np.concatenate(allidx))
        gaps = np.diff(idx)
        common = np.bincount(gaps[gaps < 5000]).argsort()[-3:][::-1] if len(gaps) > 2 else []
        env = median_filter(np.abs(x).max(1), size=rate // 10)
        loud = env > np.percentile(env, 50)
        nl = loud[idx].sum() if len(idx) else 0
        print(f"{p}: {secs:.1f} s, rms {np.sqrt((x ** 2).mean()):.0f}, clicks {len(idx)} "
              f"= {len(idx) / max(secs, 1e-9):.2f}/s (loud half {nl}, quiet half {len(idx) - nl}); "
              f"common spacings {list(map(int, common))}")


if __name__ == "__main__":
    main()
