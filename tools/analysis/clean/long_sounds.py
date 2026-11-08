# The player's longer chip sounds, like the alarm: the filter judges each pitch against 0.15 s
# either side, which sits inside a sound as long as these, so it takes them for a note starting.
# Here each long template's pitches are judged across the whole sound against the music just
# before and just after it, and where they all stand clear, those pitches come down to it.
#   long_sounds.py <part> <in.mp3|-> <start> <secs> <out.mp3>   (start/secs read audio/<part>.mp3 when in is -)
import sys, pathlib
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import one_take_filter as d
from common import stft

R = d.R
N, HOP = 1024, 128
SHORTEST, LONGEST = 0.2, 0.6     # seconds of core: shorter ones the filter already handles
SIDE, GAP = int(0.3 * R / HOP), 2
STANDS = 20.0                    # dB over the louder side, at the share of pitches below
SHARE = 0.5
GROW = 6.0
TOP_HZ = 12000.0


def long_sounds(part):
    z = np.load(f'local/rec/fx/{part}_templates.npz')
    out = {}
    for k in z.files:
        x = z[k].astype(np.float64)
        mono = x.mean(axis=1) if x.ndim > 1 else x
        env = np.sqrt(np.convolve(mono ** 2, np.ones(256) / 256, 'same'))
        core = (env > env.max() * 0.1).sum() / R
        if not SHORTEST <= core <= LONGEST or k.startswith('m'):   # m: the game's music cues
            continue
        ps = [(b, db) for b, db in d.partials(x, N) if d.P['lowest'] <= b * R / N <= TOP_HZ]
        strong = []
        for b, db in ps:
            if all(abs(b - o) > 3 for o in strong):
                strong.append(b)
        if len(strong) >= 2:   # one pitch alone is too like a held note
            out[k] = (strong, core)
    return out


def median_run(col, width):
    # The median of col[t : t + width] for each t; the end repeats the last value.
    pad = np.concatenate([col, np.repeat(col[-1:], width)])
    w = np.lib.stride_tricks.sliding_window_view(pad, width)[:len(col)]
    return np.median(w, axis=1)


def clean(X, part, report=None):
    sounds = long_sounds(part)
    sq = np.sqrt(0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N) / N))
    M = np.abs(stft(X.mean(axis=1), N, HOP, sq ** 2))
    frames, bins = M.shape
    L = 20 * np.log10(M + 1e-12)
    events = []
    for name, (strong, core) in sounds.items():
        D = max(int(core * R / HOP), 4)
        ex, steps = [], []
        for b in strong:
            col = L[:, max(b - 1, 1):min(b + 2, bins)].max(axis=1)
            inside = median_run(col, D)
            side = median_run(col, SIDE)
            before = np.concatenate([np.full(SIDE + GAP, np.inf), side])[:frames]       # col[t-GAP-SIDE : t-GAP]
            after = np.concatenate([side[D + GAP:], np.full(D + GAP, np.inf)])[:frames]  # col[t+D+GAP : ...]
            ex.append(inside - np.maximum(before, after))
        ex = np.stack(ex)
        main = ex[0]   # the sound's own strongest pitch has to be there too
        ex = np.sort(ex, axis=0)
        k = max(2, int(np.ceil(len(strong) * SHARE)))
        score = np.minimum(ex[-k], main)
        for t in np.flatnonzero(score >= STANDS):
            lo, hi = max(t - D, 0), min(t + D + 1, frames)
            if score[t] == score[lo:hi].max():
                events.append((int(t), name, float(score[t]), D))
    events.sort(key=lambda e: -e[2])
    taken = np.zeros(frames, bool)
    final = []
    for t, name, sc, D in events:
        if taken[t:t + D].any():
            continue
        taken[t:t + D] = True
        final.append((t, name, sc, D))
    gain = np.ones_like(M)
    for t, name, sc, D in final:
        strong = sounds[name][0]
        # The recording's pitch, between bins, from the main pitch over the sound.
        f0 = strong[0] if strong[0] * R / N < 4000 else min(strong)
        lo_f, hi_f = max(f0 - 3, 2), min(f0 + 4, bins - 2)
        col = M[t:t + D, lo_f:hi_f].max(axis=0)
        fa = lo_f + int(np.argmax(col))
        pitches = {b for b in strong}
        pitches |= {int(round(fa * h)) for h in range(1, 40) if fa * h * R / N <= TOP_HZ}
        a0, e0 = max(t - 3, 0), min(t + D + 3, frames - 1)
        for b in sorted(pitches):
            for c in range(max(b - 2, 1), min(b + 3, bins)):
                before = M[max(a0 - GAP - SIDE, 0):max(a0 - GAP, 1), c]
                after = M[min(e0 + GAP, frames - 1):min(e0 + GAP + SIDE, frames), c]
                l0 = np.percentile(before, 40) if len(before) else np.percentile(after, 40)
                l1 = np.percentile(after, 40) if len(after) else l0
                if 20 * np.log10(M[a0:e0 + 1, c].max() + 1e-12) - 20 * np.log10(max(l0, l1) + 1e-12) < GROW:
                    continue   # nothing of the sound at this pitch
                # Grow to where this pitch stands over the sides, as the recording's sound may run
                # a little longer than the chip's render.
                ref = 20 * np.log10(max(l0, l1) + 1e-12) + GROW
                a, e = a0, e0
                while a > max(t - D, 0) and L[a - 1, c] > ref:
                    a -= 1
                while e < min(t + 2 * D, frames - 1) and L[e + 1, c] > ref:
                    e += 1
                lvl = l0 + (l1 - l0) * (np.arange(a, e + 1) - a + 1) / (e - a + 2)
                gain[a:e + 1, c] = np.minimum(gain[a:e + 1, c], np.minimum(1.0, lvl / (M[a:e + 1, c] + 1e-12)))
    Y = np.zeros((len(X) + 2 * N, 2))
    for ch in (0, 1):
        y = np.fft.irfft(stft(X[:, ch], N, HOP, sq) * gain, N, axis=1) * sq
        for f in range(len(y)):
            Y[f * HOP:f * HOP + N, ch] += y[f]
    Y = Y[N:N + len(X)] / (N / HOP / 2)
    if report is not None:
        report['events'] = [(round(t * HOP / R - N / R, 3), name, round(sc, 1)) for t, name, sc, D in sorted(final)]
    return Y


if __name__ == '__main__':
    import subprocess
    part, src, start, secs, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
    if src == '-':
        X = d.read(part, start, secs)
    else:
        raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', src, '-ar', str(R), '-ac', '2', '-f', 'f32le', '-'],
                             capture_output=True, check=True).stdout
        X = np.frombuffer(raw, np.float32).reshape(-1, 2).astype(np.float64)
    rep = {}
    Y = clean(X, part, rep)
    print(f'{len(rep["events"])} long sounds:', rep['events'])
    if out != '-':
        d.write(pathlib.Path(out), Y)
