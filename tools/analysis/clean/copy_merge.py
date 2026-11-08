# Clean a one-take track from copies of itself: where the music repeats within the recording,
# line the copy up to the sample and, pitch by pitch, read whichever is quieter where this one
# stands clearly above it (a sound over the music only adds). Then the v7 filter over the rest.
#   copy_merge.py <part> <start> <secs> <outdir> <tag> <lag> [<lag> ...] [similar <lag> ...]
# Lags in seconds, + or -: the copy of time t is at t + lag. Where a copy holds is measured.
# Lags after "similar" are the music's own bars: the backing repeats but what's over it may not,
# so those copies only say what level each pitch should be at. A point that stands 6 dB over
# their median, and only briefly as a typing hit or beep does, comes down to it.
import sys, subprocess, pathlib
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import one_take_filter as deblip3

R = deblip3.R
N, HOP = 2048, 512
STANDS_DB = 2.0
HOLD_WINDOW = 3.0     # seconds, for where a copy is the same music
HOLD_R = 0.7


def read(part, start, secs):
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', f'audio/{part}.mp3', '-ss', f'{start:.6f}', '-t', f'{secs:.6f}',
                          '-ar', str(R), '-ac', '2', '-f', 'f32le', '-'], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, 2).astype(np.float64)


def low(x):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / R)
    X[(f < 80) | (f > 1500)] = 0
    return np.fft.irfft(X, len(x))


def moving(x, w):
    # The same as np.convolve(x, np.ones(w), 'same'), by a running sum.
    c = np.concatenate([[0.0], np.cumsum(x)])
    i = np.arange(len(x)) - (w - 1) // 2
    return c[np.clip(i + w, 0, len(x))] - c[np.clip(i, 0, len(x))]


def stft(x):
    win = np.sqrt(np.hanning(N + 1)[:N])
    pad = np.concatenate([np.zeros(N), x, np.zeros(N)])
    frames = (len(pad) - N) // HOP + 1
    idx = np.arange(N)[None, :] + HOP * np.arange(frames)[:, None]
    return np.fft.rfft(pad[idx] * win, axis=1)


def istft(S, length):
    win = np.sqrt(np.hanning(N + 1)[:N])
    y = np.fft.irfft(S, N, axis=1) * win
    out = np.zeros(len(S) * HOP + N)
    norm = np.zeros_like(out)
    for i in range(len(S)):
        out[i * HOP:i * HOP + N] += y[i]
        norm[i * HOP:i * HOP + N] += win ** 2
    return (out / np.maximum(norm, 1e-9))[N:N + length]


part, start, secs, outdir, tag = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), pathlib.Path(sys.argv[4]), sys.argv[5]
rest = sys.argv[6:]
cut = rest.index('similar') if 'similar' in rest else len(rest)
lags = [float(v) for v in rest[:cut]]
similar = [float(v) for v in rest[cut + 1:]]
SIMILAR_HOLD_R = 0.4
SIMILAR_DB = 6.0
SIMILAR_LONGEST = int(0.15 * R / HOP)
reach = max(abs(l) for l in lags + similar) + 2
wide_start = max(start - reach, 0)
wide = read(part, wide_start, secs + 2 * reach)
at0 = int(round((start - wide_start) * R))
length = int(secs * R)
X = wide[at0:at0 + length]
mid = X.mean(1)
copies = []
lw = low(wide.mean(1))
lx = lw[at0:at0 + length]
for lag in lags + similar:
    is_similar = lag in similar and lag not in lags
    # To the sample, on the low band, around the given lag.
    shift = int(round(lag * R))
    best = None
    for d in range(-64, 65):
        a = at0 + shift + d
        if a < 0 or a + length > len(wide):
            continue
        y = lw[a:a + length]
        r = (lx * y).sum() / (np.linalg.norm(lx) * np.linalg.norm(y) + 1e-12)
        if best is None or r > best[0]:
            best = (r, d)
    if best is None:
        print(f'  lag {lag}: outside the recording'); continue
    a = at0 + shift + best[1]
    C = wide[a:a + length]
    # Where it holds: local correlation of the low band.
    w = int(HOLD_WINDOW * R) | 1  # odd, so the running sum centres as convolve does
    lc = lw[a:a + length]
    num = moving(lx * lc, w)
    den = np.sqrt(moving(lx ** 2, w) * moving(lc ** 2, w)) + 1e-12
    holds = num / den > (SIMILAR_HOLD_R if is_similar else HOLD_R)
    print(f'  {"similar" if is_similar else "copy"} lag {lag:+.3f} -> {lag + best[1] / R:+.5f} s, whole r {best[0]:.2f}, holds over {holds.mean() * 100:.0f}%')
    if is_similar:
        # Only a level map is needed from these, so keep that, small, and not the audio.
        hop_centres = np.minimum(np.arange((length + N) // HOP + 1) * HOP, length - 1)
        copies.append((np.abs(stft(C.mean(1))).astype(np.float32), holds[hop_centres], True))
    else:
        copies.append((C.astype(np.float32), holds, False))
    del C

S = [stft(X[:, c]) for c in (0, 1)]
M = np.abs(stft(mid))
frames = len(M)
centres = np.minimum(np.arange(frames) * HOP, length - 1)
choice = np.full(M.shape, -1)
best_mag = M.copy()
for k, (C, holds, is_similar) in enumerate(copies):
    if is_similar:
        continue
    Mc = np.abs(stft(C.mean(1)))
    ok = holds[centres][:, None]
    better = ok & (Mc < best_mag) & (M > Mc * 10 ** (STANDS_DB / 20))
    choice[better] = k
    best_mag = np.where(better, Mc, best_mag)
for c in (0, 1):
    for k, (C, holds, is_similar) in enumerate(copies):
        if is_similar:
            continue
        Sc = stft(C[:, c])
        S[c] = np.where(choice == k, Sc, S[c])
# The bar copies: a level for each pitch and moment, from as many as hold there.
sim = [(m[:frames], h[:frames]) for m, h, s in copies if s]
gain = np.ones(M.shape)
if sim:
    enough = len(sim) >= 2
    stack = np.stack([np.where(h[:, None], m, np.float32(np.nan)) for m, h in sim])
    sim.clear()
    have = (~np.isnan(stack)).sum(0)
    stack[:, have < 2] = np.nan
    med = np.nanmedian(stack, axis=0) if enough else np.full(M.shape, np.nan)
    del stack
    Mnow = np.abs(S[0] + S[1]) / 2
    over = (Mnow > med * 10 ** (SIMILAR_DB / 20)) & ~np.isnan(med)
    # Only brief: a run along time no longer than a typing hit or beep.
    f = np.vstack([np.zeros((1, over.shape[1]), bool), over, np.zeros((1, over.shape[1]), bool)]).astype(np.int8)
    d = np.diff(f, axis=0)
    brief = np.zeros_like(over)
    for b in range(over.shape[1]):
        s_, e_ = np.flatnonzero(d[:, b] == 1), np.flatnonzero(d[:, b] == -1)
        for a_, z_ in zip(s_, e_):
            if z_ - a_ <= SIMILAR_LONGEST:
                brief[max(a_ - 1, 0):z_ + 1, b] = True
    gain = np.where(brief & over | (brief & (Mnow > med)), np.minimum(1, med / (Mnow + 1e-12)), 1.0)
    gain = np.nan_to_num(gain, nan=1.0)
    print(f'  bar copies: {(gain < 1).mean() * 100:.1f}% of pitch-moments brought down')
for c in (0, 1):
    S[c] = S[c] * gain
Y = np.stack([istft(S[c], length) for c in (0, 1)], 1)
print(f'  read from an exact copy: {(choice >= 0).mean() * 100:.1f}% of pitch-moments, removed {10 * np.log10(((X - Y) ** 2).sum() / (X ** 2).sum()):.1f} dB')
outdir.mkdir(parents=True, exist_ok=True)
deblip3.write(outdir / f'{tag}_after_copies.mp3', Y)
# Then the v7 filter over what no copy covered.
CH, OV = 120 * R, 4 * R
Z = np.zeros_like(Y); wsum = np.zeros(len(Y))
starts = list(range(0, len(Y), CH))
if len(starts) > 1 and len(Y) - starts[-1] < CH // 4:
    starts.pop()
for i, a in enumerate(starts):
    lo = max(a - OV, 0); hi = len(Y) if i + 1 == len(starts) else min(a + CH + OV, len(Y))
    z = deblip3.deblip(Y[lo:hi], part, deblip3.P, {})
    ramp = np.ones(hi - lo)
    if lo > 0: ramp[:2 * OV] = np.linspace(0, 1, 2 * OV)
    if hi < len(Y): ramp[-2 * OV:] = np.minimum(ramp[-2 * OV:], np.linspace(1, 0, 2 * OV))
    Z[lo:hi] += z * ramp[:, None]; wsum[lo:hi] += ramp
Z /= np.maximum(wsum, 1e-9)[:, None]
deblip3.write(outdir / f'{tag}_after_copies_plus_filter.mp3', Z)
print(f'  then filtered: removed {10 * np.log10(((X - Z) ** 2).sum() / (X ** 2).sum()):.1f} dB in all')
