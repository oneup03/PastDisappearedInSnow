# Sample-level check of a suspected repeat: low band only (typing and beeps are mostly higher),
# 6 s windows along the overlap, best lag within +-0.6 s of the guess.
#   verify.py <part> <start> <secs> <part2> <start2> [...]
import subprocess, sys
import numpy as np
R = 16000


def read(part, start, secs):
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-ss', f'{max(start, 0):.3f}', '-t', f'{secs:.3f}', '-i', f'audio/{part}.mp3',
                          '-ar', str(R), '-ac', '1', '-af', 'highpass=f=80,lowpass=f=1500', '-f', 'f32le', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).astype(np.float64)


part, start, secs = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
a = read(part, start, secs)
args = sys.argv[4:]
for p2, s2 in zip(args[::2], args[1::2]):
    s2 = float(s2)
    b = read(p2, s2 - 1, secs + 2)
    W, reach = 6 * R, int(0.6 * R)
    rows = []
    for at in range(0, len(a) - W, W):
        w = a[at:at + W]
        if np.linalg.norm(w) < 1e-3:
            continue
        lo = at + R - reach
        seg = b[max(lo, 0):lo + 2 * reach + W]
        if len(seg) < W + 2 * reach:
            break
        n = 1 << int(np.ceil(np.log2(len(seg) + W)))
        c = np.fft.irfft(np.fft.rfft(seg, n) * np.conj(np.fft.rfft(w, n)), n)[:len(seg) - W + 1]
        e = np.sqrt(np.convolve(seg ** 2, np.ones(W), 'valid'))
        c = c / (e * np.linalg.norm(w) + 1e-12)
        i = int(np.argmax(c))
        rows.append((at / R, (i - reach) / R, c[i]))
    if not rows:
        print(f'{p2} {s2}: no overlap'); continue
    r = np.array(rows)
    good = r[r[:, 2] > 0.8]
    print(f'{part} {start} vs {p2} {s2}: {len(good)}/{len(r)} windows > 0.8, median r {np.median(r[:, 2]):.2f}, '
          f'lag {np.median(good[:, 1]) * 1000 if len(good) else float("nan"):+.1f} ms (spread {np.ptp(good[:, 1]) * 1000 if len(good) else 0:.1f} ms), '
          f'good from {good[0, 0] if len(good) else 0:.0f} to {good[-1, 0] + 6 if len(good) else 0:.0f} s')
