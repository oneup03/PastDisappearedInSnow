# Where does a track repeat within itself, to the sample? Each 4 s window of the low band
# (80-1500 Hz, at 8 kHz) against the whole track; peaks at r > 0.85 away from itself.
# A lag counts if it holds (within 2 ms) in at least two neighbouring windows.
#   self_similar.py <part> <start> <secs> <tag> [...]  (groups of four)
import subprocess, sys
import numpy as np

R = 8000
WIN, STEP = 4 * R, 2 * R
MIN_R = float(__import__("os").environ.get("MIN_R", "0.85"))


def read(part, start, secs):
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', f'audio/{part}.mp3', '-ss', f'{start:.3f}', '-t', f'{secs:.3f}',
                          '-ar', str(R), '-ac', '1', '-af', 'highpass=f=80,lowpass=f=1500', '-f', 'f32le', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).astype(np.float64)


args = sys.argv[1:]
for part, start, secs, tag in zip(args[::4], args[1::4], args[2::4], args[3::4]):
    x = read(part, float(start), float(secs))
    n = 1 << int(np.ceil(np.log2(len(x) + WIN)))
    X = np.fft.rfft(x, n)
    energy = np.sqrt(np.maximum(np.convolve(x ** 2, np.ones(WIN), 'valid'), 1e-12))
    found = []  # (window time, lag seconds, r)
    for at in range(0, len(x) - WIN, STEP):
        w = x[at:at + WIN]
        nw = np.linalg.norm(w)
        if nw < 1e-3:
            continue
        c = np.fft.irfft(X * np.conj(np.fft.rfft(w, n)), n)[:len(energy)] / (energy * nw)
        c[max(0, at - R):at + R] = 0  # not itself
        peaks = np.flatnonzero((c[1:-1] > MIN_R) & (c[1:-1] >= c[:-2]) & (c[1:-1] >= c[2:])) + 1
        # one per 0.5 s neighbourhood
        keep = []
        for p in peaks[np.argsort(c[peaks])[::-1]]:
            if all(abs(p - q) > R // 2 for q in keep):
                keep.append(p)
        for p in keep:
            found.append((at / R, (p - at) / R, c[p]))
    # Group by lag (2 ms), count windows.
    groups = {}
    for t, lag, r in found:
        key = round(lag * 500)  # 2 ms bins
        hit = next((k for k in (key - 1, key, key + 1) if k in groups), key)
        groups.setdefault(hit, []).append((t, lag, r))
    rows = []
    for k, v in groups.items():
        if len(v) < int(__import__("os").environ.get("MIN_WINDOWS", "2")):
            continue
        lag = float(np.median([l for _, l, _ in v]))
        if lag < 0:
            continue  # each pair once: the later copy from the earlier
        ts = sorted(t for t, _, _ in v)
        rows.append((len(v) * 2, lag, ts[0], ts[-1] + 4, float(np.median([r for _, _, r in v]))))
    rows.sort(key=lambda r: -r[0])
    cover = np.zeros(int(len(x) / R) + 1, bool)
    for secs_, lag, a, b, r in rows:
        cover[int(a):int(b)] = True; cover[int(a + lag):min(int(b + lag), len(cover))] = True
    print(f'{tag} ({part} {start}, {secs} s): {cover.mean() * 100:.0f}% of it has another copy within itself')
    for secs_, lag, a, b, r in rows[:12]:
        print(f'   lag {lag:8.3f} s: {secs_:4d} s matched, from {a:.0f} to {b:.0f} s in, median r {r:.2f}')
    sys.stdout.flush()
