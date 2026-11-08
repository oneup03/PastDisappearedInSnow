# Take the original player's typing and beeps off one recording.
# The game's own short sounds are known (templates rendered from the chip), so we know exactly
# which pitches each one has. Where the strongest of a sound's pitches all jump well above
# what that pitch holds just before and just after, together, that sound is there: bring those
# pitches down to the level around them, for as long as the sound lasts. Nothing else is touched.
#   one_take_filter.py <part> <start> <seconds> <outdir> <tag> [key=value ...]
import sys, subprocess, pathlib
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from common import stft, side_percentile, read, write, picture

R = 32000
P = dict(n=1024, hop=128, side=0.15, q=40, t2=9.0, t1=14.0, train=0.3, lone=4.0, grow=6.0,
         reach=2, maxlen=0.12, floor_db=-65.0, depth=-90.0, top=9000.0, rel=-22.0, share=0.5, slack=12, narrow=6.0, bass=450.0, bass_db=9.0, bass_depth=-12.0, lowest=200.0, longest_core=0.3, long_low=700.0, g_low=500.0, g_db=99.0, g_narrow=9.0, x_db=8.0, tq=20, exclusive=1, edge=1, trains=1, conf=15.0, hot_min=0.15, hot_ratio=3.0, gate_db=4.0, hot_top=4500.0, broad=0.2, drum=0.45, drum_extra=12.0, broad_extra=8.0, skip='e49', step_db=4.0, x_narrow=6.0, lowest_single=450.0, nbrel=1, main_pen=6.0, nb_floor=0.0, rgrow=6.0, rpad=0, rwide=25.0, guard=0, dbg_t=0.0, step_margin=20.0, with_beep=1, wb_narrow=15.0, wb_db=15.0, wb_after=15, wb_narrow2=12.0, dbg_names='e03,e10,e01', skirt=10, skirt_db=18.0, type_any=0, type_pen=8.0, rel_bonus=6.0, floor_mul=1.0)
SHORT = 0.12   # seconds: longer templates are music cues, not the player's sounds


def partials(x, n):
    """The pitches of a short sound, strongest first, as (bin, dB under the strongest)."""
    if x.ndim > 1:
        x = x.mean(axis=1)
    F = np.abs(np.fft.rfft(x * np.hanning(len(x)), 8 * n))
    top = F.max()
    best = {}
    for i in range(2, len(F) - 2):
        if F[i] == F[i - 2:i + 3].max() and F[i] >= top * 10 ** (P['rel'] / 20):
            b = int(round(i / 8))
            best[b] = max(best.get(b, 0), F[i])
    out = [(b, 20 * np.log10(v / top)) for b, v in best.items()]
    return sorted(out, key=lambda t: t[1], reverse=True)[:6]


def load_sounds(part, n):
    z = np.load(f'local/rec/fx/{part}_templates.npz')
    sounds = {}
    for k in z.files:
        x = z[k].astype(np.float64)
        mono = x.mean(axis=1) if x.ndim > 1 else x
        env = np.sqrt(np.convolve(mono ** 2, np.ones(256) / 256, 'same'))
        core = (env > env.max() * 0.1).sum() / R      # how long it's actually heard
        if core > P['longest_core'] or k in P['skip'].split(','):
            continue   # e49 is a burst of noise: it matches drums
        ps = [(b, db) for b, db in partials(x, n) if P['lowest'] <= b * R / n <= P['top']]
        if not ps or (core > SHORT and ps[0][0] * R / n < P['long_low']):
            continue   # a longer low sound is too like a note
        if len(ps) == 1 and ps[0][0] * R / n < P['lowest_single']:
            continue   # one low pitch alone looks just like a bass note
        if ps:
            sounds[k] = (ps, core)
    return sounds


def deblip(X, part, p=P, report=None):
    n, hop = p['n'], p['hop']
    sounds = load_sounds(part, n)
    sq = np.sqrt(0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n) / n))
    M = np.abs(stft(X.mean(axis=1), n, hop, sq ** 2))
    frames, bins = M.shape
    cols = list(range(1, min(bins, int(p['top'] * n / R) + 8)))
    side = int(p['side'] * R / hop)
    bg = np.full_like(M, np.inf)
    sub = M[:, cols]
    past, future = side_percentile(sub, side, p['q'], True), side_percentile(sub, side, p['q'], False)
    bg[:, cols] = np.maximum(past, future)
    # How far a pitch steps between before and after: a note starting or ending, not a blip.
    step = np.zeros_like(M)
    step[:, cols] = np.abs(20 * np.log10((future + 1e-9) / (past + 1e-9)))
    del past, future
    target = np.full_like(M, np.inf)
    past20, future20 = side_percentile(sub, side, p['tq'], True), side_percentile(sub, side, p['tq'], False)
    target[:, cols] = np.maximum(past20, future20)
    # A held note keeps even the quiet moments after it up; a train of beeps at one pitch doesn't.
    step[:, cols] = np.minimum(step[:, cols], np.abs(20 * np.log10((future20 + 1e-9) / (past20 + 1e-9))))
    del past20, future20
    with np.errstate(divide='ignore'):
        rdb = 20 * np.log10(M + 1e-12) - 20 * np.log10(bg + 1e-12)
    loud = M > 10 ** (p['floor_db'] / 20) * n / 4
    # What the neighbouring pitches do at the same moment. A drum or a hi-hat lifts them all;
    # a chip sound stands out from them. Judging each pitch against its neighbours replaces
    # the old all-or-nothing drum guard, which threw out beeps that landed on a hi-hat.
    if p['nbrel']:
        offs = (-12, -9, -6, -4, 4, 6, 9, 12)
        padded = np.pad(rdb, ((0, 0), (12, 12)), mode='edge')
        nb = np.median(np.stack([padded[:, 12 + d:12 + d + bins] for d in offs]), axis=0)
        rel = np.minimum(rdb, rdb - np.maximum(nb, p['nb_floor']) + p['rel_bonus'])
    else:
        rel = rdb
    # A drum or click lifts most pitches at once; a chip sound lifts a few.
    wb = slice(int(1000 * n / R), int(8000 * n / R))
    share_up = ((rdb[:, wb] > 6) & loud[:, wb]).mean(axis=1)
    share_up = np.maximum(share_up, np.maximum(np.roll(share_up, 1), np.roll(share_up, -1)))
    broad = share_up > p['broad']
    drum = share_up > p['drum']

    def near(b):  # excess at a pitch, allowing a bin either way
        lo, hi = max(b - 1, 1), min(b + 2, bins)
        return np.where(loud[:, lo:hi], rel[:, lo:hi], -99).max(axis=1)

    events = []   # (frame, name, score)
    for name, (ps, dur) in sounds.items():
        strong = []
        for b, db in ps:
            if all(abs(b - o) > 3 for o in strong):
                strong.append(b)
        rows = np.stack([near(b) for b in strong])
        thr = p['t2'] if len(strong) > 1 else p['t1']
        if len(strong) > 1:
            k = max(2, int(np.ceil(len(strong) * p['share'])))
            s = np.sort(rows, axis=0)[-k]
            s = np.maximum(s, rows[0] - p['main_pen'])   # a strong main pitch is enough
            if name[1:].isdigit() and 32 <= int(name[1:]) <= 48 and p['type_any']:
                # typing's fundamental sits under the bass; its third harmonic is often what's heard
                s = np.maximum(s, rows[1:].max(axis=0) - p['type_pen'])
        else:
            s = rows[0]
        steps = np.median(np.stack([step[:, b] for b in strong]), axis=0)
        # a note starting or ending steps the level about as far as it stands out; a blip stands out far more
        lo_m, hi_m = max(strong[0] - 1, 1), min(strong[0] + 2, bins)
        main_abs = np.where(loud[:, lo_m:hi_m], rdb[:, lo_m:hi_m], -99).max(axis=1)
        s = np.where((steps > p['step_db']) & (main_abs < steps + p['step_margin']), -99, s)
        s = np.maximum(s, np.maximum(np.roll(s, 1), np.roll(s, -1)))
        longest = int(dur * R / hop) + n // hop + p['slack']
        need = (p['t2'] if len(strong) > 1 else p['t1']) + (np.where(broad, p['broad_extra'], 0.0) + np.where(drum, 999.0 if len(strong) == 1 else p['drum_extra'], 0.0) if p['guard'] else 0.0)
        span = max(int(dur * R / hop), 1)
        cand = np.flatnonzero(s >= need)
        if p.get('dbg_t') and name in p['dbg_names'].split(','):
            f = int((p['dbg_t'] + n / R) * R / hop)
            for u in range(f - 3, f + 4):
                print('DBG', name, round(u * hop / R - n / R, 3), 'strong', [round(b * R / n) for b in strong], 'rows', np.round(rows[:, u], 1), 'step', round(float(steps[u]), 1), 's', round(float(s[u]), 1), 'rdb', [round(float(rdb[u, b]), 1) for b in strong])
        for t in cand:
            lo, hi = max(t - span, 0), min(t + span + 1, frames)
            if s[t] == s[lo:hi].max():
                # a sound this short can't stand out for longer than it lasts
                main = rows[int(np.argmax(rows[:, t]))]
                a = t
                while a > 0 and t - a <= longest and main[a - 1] > p['grow']:
                    a -= 1
                e = t
                while e < frames - 1 and e - a <= longest and main[e + 1] > p['grow']:
                    e += 1
                if e - a <= longest:
                    events.append((int(t), name, float(s[t])))
    # Every sound that fits gets taken off: telling close ones apart is unreliable, and a wrong
    # pick would leave the real one's pitches in.
    events.sort(key=lambda e: -e[2])
    taken = np.zeros(frames, bool); kept = []
    for t, name, sc in events:
        span = max(int(sounds[name][1] * R / hop) // 2, 2)
        if p['exclusive'] and taken[max(t - span, 0):t + span + 1].any():
            continue
        taken[max(t - span, 0):t + span + 1] = True
        kept.append((t, name, sc))
    kept.sort()
    typing = [e for e in kept if e[1][1:].isdigit() and 32 <= int(e[1][1:]) <= 48]
    tt = np.array([e[0] for e in typing])
    final = []
    win = int(p['train'] * R / hop)
    for t, name, sc in kept:
        is_typing = name[1:].isdigit() and 32 <= int(name[1:]) <= 48
        if is_typing:
            others = np.abs(tt - t)
            if not ((others > 0) & (others <= win)).any() and sc < p['t1'] + p['lone']:
                continue
        final.append((t, name, sc))
    gain = np.ones_like(M)
    # Never below what that pitch usually holds either side: dirt comes down to the music's
    # level, and music that was only a little up isn't dug out.
    floor_gain = np.minimum(1.0, p['floor_mul'] * bg / (M + 1e-12)) if p['floor_mul'] > 0 else np.zeros_like(M)
    maxlen = int(p['maxlen'] * R / hop) + n // hop

    def repair(t, pitches):
        for b in pitches:
            reach = p['reach'] + (1 if rdb[max(t - 2, 0):t + 3, b].max() > p['rwide'] else 0)
            for c in range(max(b - reach, 1), min(b + reach + 1, bins)):
                ex = rdb[:, c]
                near_t = range(max(t - 2, 0), min(t + 3, frames))
                if not any(ex[u] > p['grow'] for u in near_t):
                    continue   # nothing of the sound at this pitch
                t_c = max(near_t, key=lambda u: ex[u])
                a = t_c
                while a > 0 and t_c - a < maxlen and ex[a - 1] > p['rgrow']:
                    a -= 1
                e = t_c
                while e < frames - 1 and e - t_c < maxlen and ex[e + 1] > p['rgrow']:
                    e += 1
                a, e = max(a - p['rpad'], 0), min(e + p['rpad'], frames - 1)
                low = c * R / n < p['bass']
                if p['edge']:
                    # the level either side of the sound, straight across under it
                    l0 = np.mean(M[max(a - 3, 0):max(a - 1, 1), c]); l1 = np.mean(M[min(e + 2, frames - 1):min(e + 4, frames), c] if e + 2 < frames else M[e:e + 1, c])
                    lvl = l0 + (l1 - l0) * (np.arange(a, e + 1) - a + 1) / (e - a + 2)
                    g = np.minimum(1.0, np.maximum(lvl, target[a:e + 1, c]) / (M[a:e + 1, c] + 1e-12))
                else:
                    g = np.minimum(1.0, target[a:e + 1, c] / (M[a:e + 1, c] + 1e-12))
                if low:   # under the bass: only what clearly stands out, and not all the way
                    g = np.where(rdb[a:e + 1, c] > p['bass_db'], np.maximum(g, 10 ** (p['bass_depth'] / 20)), 1.0)
                g = np.maximum(g, 10 ** (p['depth'] / 20))
                gain[a:e + 1, c] = np.minimum(gain[a:e + 1, c], g)

    # What stands out narrowly, anywhere: for overtones of known sounds and for unknown ones.
    lo_b, hi_b = int(p['g_low'] * n / R), min(int(p['top'] * n / R), bins - 6)
    seg = rdb[:, lo_b:hi_b]
    Ms = M[:, lo_b:hi_b + 0]
    pk = np.zeros_like(seg, bool)
    pk[:, 1:-1] = (Ms[:, 1:-1] >= Ms[:, :-2]) & (Ms[:, 1:-1] >= Ms[:, 2:])
    around = np.median(np.stack([rdb[:, lo_b + d:hi_b + d] for d in (-5, -4, 4, 5)]), axis=0)
    hit = pk & (seg >= p['g_db']) & (seg - around >= p['g_narrow']) & loud[:, lo_b:hi_b]
    # A detected sound also brings its own overtones, which the recording has and the chip's
    # render may not: take whatever else stands out narrowly at that moment.
    xhit = pk & (seg >= p['x_db']) & (seg - around >= p['x_narrow']) & loud[:, lo_b:hi_b]
    for t, name, sc in final:
        f0 = sounds[name][0][0][0]
        # The recording's pitch can sit a little off the chip render's, which puts the upper
        # harmonics bins away: measure it, between bins, from the peak at the moment.
        lo_f, hi_f = max(f0 - 3, 2), min(f0 + 4, bins - 2)
        win_t = slice(max(t - 2, 0), min(t + 3, frames))
        col = M[win_t, lo_f:hi_f].max(axis=0)
        k0 = lo_f + int(np.argmax(col))
        a_, b_, c_ = np.log(M[win_t, k0 - 1].max() + 1e-12), np.log(M[win_t, k0].max() + 1e-12), np.log(M[win_t, k0 + 1].max() + 1e-12)
        den = a_ - 2 * b_ + c_
        fa = k0 + (0.5 * (a_ - c_) / den if den < 0 else 0.0)
        if abs(fa - f0) > 3:
            fa = f0
        harm = {int(round(fa * k)) + d for k in range(2, 40) for d in (-2, -1, 0, 1, 2) if fa * k < bins - 3}
        extra = {int(b) + lo_b for f in range(max(t - 2, 0), min(t + 3, frames)) for b in np.flatnonzero(xhit[f])
                 if int(b) + lo_b in harm}
        if p['with_beep'] and not (name[1:].isdigit() and 32 <= int(name[1:]) <= 48 and name != 'e48'):
            # Whatever else starts with a beep, stands out sharply and stops when it does is
            # part of the same chip sound: the recording's pitch envelope differs from the render's.
            slide = np.zeros(hi_b - lo_b, bool)
            for k in range(1, 6):
                l_, h_ = int(fa * k * 0.9) - lo_b, int(fa * k * 1.25) + 2 - lo_b
                slide[max(l_, 0):max(min(h_, hi_b - lo_b), 0)] = True
            for f in range(max(t - 3, 0), min(t + p['wb_after'], frames)):
                near_start = f <= t + 2
                ok = pk[f] & (seg[f] >= p['wb_db']) & (seg[f] - around[f] >= (p['wb_narrow'] if near_start else p['wb_narrow2']))
                if not near_start:
                    ok &= slide
                for b in np.flatnonzero(ok):
                    c = int(b) + lo_b
                    run = rdb[max(t - maxlen, 0):t + maxlen, c] > p['rgrow']
                    mid = t - max(t - maxlen, 0)
                    a = mid
                    while a > 0 and run[a - 1]: a -= 1
                    e = mid
                    while e < len(run) - 1 and run[e + 1]: e += 1
                    if a > 0 and e < len(run) - 1:   # it ends within reach: not a held note
                        extra.add(c)
            # A short beep's energy spreads either side of its pitch: take the skirt where it stands out.
            for k in range(1, 4):
                centre = int(round(fa * k))
                if centre >= bins - 3:
                    break
                for c in range(max(centre - p['skirt'], 2), min(centre + p['skirt'] + 1, bins - 1)):
                    if rdb[max(t - 3, 0):t + 4, c].max() > p['skirt_db']:
                        extra.add(c)
        repair(t, sorted({b for b, db in sounds[name][0]} | extra))
    # While the text types, its pitches are known: learn them from the clear cases, and take
    # off whatever stands out at those pitches for as long as the typing goes on.
    hot_bins, spans = [], []
    conf = [e for e in final if e[2] >= p['conf'] and e[1][1:].isdigit() and 32 <= int(e[1][1:]) <= 48]
    if p['trains'] and len(conf) >= 5:
        votes = np.zeros(bins)
        for t, name, sc in conf:
            for u in range(max(t - 1, 0), min(t + 2, frames)):
                row = rdb[u]
                good = np.flatnonzero((row > 8) & loud[u])
                good = good[(good > 2) & (good < bins - 3)]
                good = good[(M[u, good] >= M[u, good - 1]) & (M[u, good] >= M[u, good + 1])]
                votes[good] += 1
        # a typing pitch is a sharp peak among its neighbours' votes
        local = np.array([np.median(votes[max(b - 10, 0):b + 11]) for b in range(bins)])
        hot = (votes >= p['hot_min'] * len(conf)) & (votes > p['hot_ratio'] * (local + 1)) & (np.arange(bins) * R / n >= p['lowest']) & (np.arange(bins) * R / n <= p['hot_top'])
        hot_bins = list(np.flatnonzero(hot))
        tts = sorted(e[0] for e in final if e[1][1:].isdigit() and 32 <= int(e[1][1:]) <= 48)
        gapf = int(p['train'] * R / hop); pad = int(0.05 * R / hop)
        start = None
        for i, t in enumerate(tts):
            if start is None:
                start = t
            if i + 1 == len(tts) or tts[i + 1] - t > gapf:
                if i > 0 and t > start:
                    spans.append((max(start - pad, 0), min(t + pad + n // hop, frames - 1)))
                start = None
        for a0, e0 in spans:
            for b in hot_bins:
                for c in range(max(b - 1, 1), min(b + 2, bins)):
                    ex = rdb[a0:e0 + 1, c] > p['gate_db']
                    if not ex.any():
                        continue
                    on = np.flatnonzero(np.diff(np.r_[0, ex.astype(int), 0]) == 1)
                    off = np.flatnonzero(np.diff(np.r_[0, ex.astype(int), 0]) == -1) - 1
                    for r0, r1 in zip(on, off):
                        a, e = a0 + r0, a0 + r1
                        if e - a > maxlen:
                            continue
                        l0 = np.mean(M[max(a - 3, 0):max(a - 1, 1), c])
                        l1 = np.mean(M[min(e + 2, frames - 1):min(e + 4, frames), c]) if e + 2 < frames else M[e, c]
                        lvl = l0 + (l1 - l0) * (np.arange(a, e + 1) - a + 1) / (e - a + 2)
                        g = np.minimum(1.0, np.maximum(lvl, target[a:e + 1, c]) / (M[a:e + 1, c] + 1e-12))
                        if c * R / n < p['bass']:
                            g = np.maximum(g, 10 ** (p['bass_depth'] / 20))
                        gain[a:e + 1, c] = np.minimum(gain[a:e + 1, c], g)
    # Sounds with no template: loud, short, narrow, at two or more pitches at once.
    many = hit.sum(axis=1) >= 2
    generic = 0
    for start_, end_ in zip(np.flatnonzero(np.diff(np.r_[0, many.astype(int)]) == 1), np.flatnonzero(np.diff(np.r_[many.astype(int), 0]) == -1)):
        if end_ - start_ > maxlen:
            continue
        t = (start_ + end_) // 2
        pitches = sorted({int(b) + lo_b for f in range(start_, end_ + 1) for b in np.flatnonzero(hit[f])})
        repair(t, pitches)
        generic += 1
    gain = np.where(np.isfinite(floor_gain), np.maximum(gain, floor_gain), gain)
    Y = np.zeros((len(X) + 2 * n, 2))
    for c in (0, 1):
        y = np.fft.irfft(stft(X[:, c], n, hop, sq) * gain, n, axis=1) * sq
        for f in range(len(y)):
            Y[f * hop:f * hop + n, c] += y[f]
    Y = Y[n:n + len(X)] / (n / hop / 2)
    if report is not None:
        import collections
        report['final'] = [(t * hop / R - n / R, name, round(sc, 1)) for t, name, sc in final]
        report['gainmap'] = gain
        report.update(events=len(final), generic=generic, hot=[round(b * R / n) for b in hot_bins], spans=round(sum(e - a for a, e in spans) * hop / R, 1), kinds=collections.Counter(e[1] for e in final).most_common(8),
                      touched=float((gain < 1).mean()),
                      removed_db=float(10 * np.log10(((X - Y) ** 2).sum() / ((X ** 2).sum() + 1e-12) + 1e-12)))
    return Y


if __name__ == '__main__':
    part, start, secs, outdir, tag = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), pathlib.Path(sys.argv[4]), sys.argv[5]
    p = dict(P)
    for kv in sys.argv[6:]:
        k, v = kv.split('='); p[k] = type(P[k])(v)
    outdir.mkdir(parents=True, exist_ok=True)
    X = read(part, start, secs)
    rep = {}
    CH, OV = 120 * R, 4 * R
    if len(X) <= CH + OV:
        Y = deblip(X, part, p, rep)
    else:   # long tracks in pieces, to keep memory down, crossfaded where they overlap
        Y = np.zeros_like(X); w = np.zeros(len(X))
        starts = list(range(0, len(X), CH))
        if len(starts) > 1 and len(X) - starts[-1] < CH // 4:
            starts.pop()   # a short tail goes with the piece before it
        for i, a in enumerate(starts):
            lo = max(a - OV, 0)
            hi = len(X) if i + 1 == len(starts) else min(a + CH + OV, len(X))
            part_rep = {}
            y = deblip(X[lo:hi], part, p, part_rep)
            ramp = np.ones(hi - lo)
            if lo > 0: ramp[:2 * OV] = np.linspace(0, 1, 2 * OV)
            if hi < len(X): ramp[-2 * OV:] = np.minimum(ramp[-2 * OV:], np.linspace(1, 0, 2 * OV))
            Y[lo:hi] += y * ramp[:, None]; w[lo:hi] += ramp
            rep.setdefault('pieces', []).append({k: part_rep[k] for k in ('events', 'hot', 'touched')})
        Y /= np.maximum(w, 1e-9)[:, None]
        rep['removed_db'] = float(10 * np.log10(((X - Y) ** 2).sum() / (X ** 2).sum()))
    print(tag, {k: v for k, v in rep.items() if k not in ('final', 'gainmap')}, flush=True)
    for name, a in (('before', X), ('after', Y), ('removed', X - Y)):
        write(outdir / f'{tag}_{name}.mp3', a)
    picture(outdir / f'{tag}_10s.png', X, Y, 8, 6000)
