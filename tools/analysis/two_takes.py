#!/usr/bin/env python3
"""A clean pass of a track that has birds over it, from two recordings of that track.

Some scenes mix a birdsong tape over the music. The two run independently, so no two passes
of the music have the birds in the same place, and passes don't cancel where the birds are.
That's what match_effects.py goes by, so it finds nothing it can trust there. Three things
make it work here:

- The music still cancels. Typing is mostly under 1500 Hz and the birds are over it, so
  what stands out from the other recording down there is the player's. Button sounds are
  higher, but they have fixed lines, and it takes two lines at once to count.
- With the music cancelled, what's left higher up is a bird in one recording or the other.
- So cut between recordings only where neither has a bird sounding. The birds then jump by a
  fraction of a second at a cut, which doesn't matter, but no chirp gets cut in two.
- With the music cancelled, the game's own sounds match their templates at up to 0.99, so each
  one's place is known. Where a take still has one, the render replaces just its frequencies
  from the other take, so the birds over it aren't touched. The templates are local files
  (`local/rec/fx/*_templates.npz`), captured from the sound chip, and aren't needed to play.

    two_takes.py chu 1980:1992 2259.5:2262 kou --lag -63.0096 --fade 1.5 --write
"""
import argparse
import hashlib
import pathlib
import re
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import match_effects as me  # noqa: E402

RATE = me.RATE
SIZE = 1024
HOP = 256
BIN_HZ = RATE / SIZE
TOP_BIN = int(9000 / BIN_HZ)
TYPING_BINS = (int(200 / BIN_HZ), int(1500 / BIN_HZ))
BIRD_BINS = (int(2600 / BIN_HZ), int(7000 / BIN_HZ))  # over the typing's upper partials
BUTTON_LINES = ((1012, 2990), (1985, 5955), (3766, 7531), (1048, 3144), (1394, 3736), (2644, 5288), (3321, 6642))
STANDS_OUT_DB = 9.0
QUIETEST_DB = -14.0
TYPING_CELLS = 2
BIRD_LEFT_DB = 3.0
BIRD_CELLS = 3
TEMPLATE_SECONDS = 0.12
TEMPLATE_SHORTEST = 64
TEMPLATE_SCORE = 0.7
TEMPLATE_LINES = 8
TEMPLATE_LINE_SHARE = 0.03
SAME_MOMENT = RATE // 100
OWNER_DB = 3.0
PATCH_TOP_HZ = 2300.0          # as in src/loop_render.c: under here is all replaced
PATCH_BEFORE_SECONDS = 0.01
PATCH_AFTER_SECONDS = 0.025    # MP3 smears a sound
SWITCH_FRAMES = 12             # a cut has to save this much dirt, as a patch can do the rest
DIRT_MARGIN_FRAMES = 4         # a sound smears over the analysis window and the MP3's granules
BIRD_MARGIN_FRAMES = 3         # room for the cut to move to where the samples match
VOICE_SECONDS = 1.5            # dirt this long is a voice or a cue, for a first pass to keep
LAG_WINDOW_SECONDS = 4.0
LAG_REACH_SECONDS = 0.02
LAG_EVERY_SECONDS = 8.0
MIN_LOCK = 0.8
WRAP_WINDOW_SECONDS = 2.0
BIRD_AT_WRAP_COST = 2.0


def load_templates():
    """The game's sound effects, each once: (start of it, full length, lines above the low band)."""
    out, seen = {}, set()
    for pack in me.PARTS:
        with np.load(me.FX_DIR / f"{pack}_templates.npz") as stored:
            for key in stored.keys():
                if not key.startswith("e"):
                    continue
                sound = stored[key].astype(np.float64)
                sound = sound.mean(axis=1) if sound.ndim > 1 else sound
                head = sound[:int(TEMPLATE_SECONDS * RATE)]
                head = head - head.mean()
                digest = hashlib.md5(np.round(head[:256], 3).tobytes()).hexdigest()
                if digest in seen or len(head) < TEMPLATE_SHORTEST:
                    continue
                seen.add(digest)
                levels = np.abs(np.fft.rfft(head * np.hanning(len(head)), 4096))
                top, lines = levels.max(), []
                for _ in range(TEMPLATE_LINES):
                    peak = int(np.argmax(levels))
                    if levels[peak] < top * TEMPLATE_LINE_SHARE:
                        break
                    if peak * RATE / 4096 > PATCH_TOP_HZ:
                        lines.append(peak * RATE / 4096)
                    levels[max(0, peak - 16):peak + 16] = 0
                out[f"{pack}:{key}"] = (head, len(sound), lines)
    return out


def find_sounds(here, there, templates):
    """(sample, template name) of each sound, one per moment, with the music cancelled."""
    difference = here - there
    size = 1 << int(np.ceil(np.log2(len(difference) + TEMPLATE_SECONDS * RATE)))
    spectrum_ = np.fft.rfft(difference, size)
    squares = np.concatenate([[0.0], np.cumsum(difference ** 2)])
    hits = []
    for name, (head, _, _) in templates.items():
        product = np.fft.irfft(spectrum_ * np.conj(np.fft.rfft(head, size)), size)[:len(difference) - len(head)]
        energy = squares[len(head):len(head) + len(product)] - squares[:len(product)]
        score = np.abs(product) / np.sqrt(np.maximum(energy, 1e-12) * (head ** 2).sum())
        good = np.flatnonzero(score > TEMPLATE_SCORE)
        if len(good):
            for group in np.split(good, np.flatnonzero(np.diff(good) > SAME_MOMENT) + 1):
                place = int(group[np.argmax(score[group])])
                hits.append((place, float(score[place]), name))
    hits.sort()
    merged = []
    for hit in hits:
        if merged and hit[0] - merged[-1][0] < SAME_MOMENT:
            if hit[1] > merged[-1][1]:
                merged[-1] = hit
        else:
            merged.append(hit)
    return [(place, name) for place, _, name in merged]


def whose(here, there, place, head):
    """Which recording has a sound: 0, 1, or None if they can't be told apart. Looks where the
    sound is loudest, as some are nearly all in their first few milliseconds."""
    span = min(SIZE, len(head))
    squares = np.concatenate([[0.0], np.cumsum(head ** 2)])
    loudest = int(np.argmax(squares[span:] - squares[:-span])) if len(head) > span else 0
    middle = place + loudest + span // 2 - SIZE // 2
    if middle < 0 or middle + SIZE >= len(here):
        return None
    shape = np.hanning(SIZE)
    levels = np.abs(np.fft.rfft(head[loudest:loudest + span] * np.hanning(span), SIZE))
    bins = np.argsort(levels)[-6:]
    ours = (np.abs(np.fft.rfft(here[middle:middle + SIZE] * shape))[bins] ** 2).sum() + 1e-9
    theirs = (np.abs(np.fft.rfft(there[middle:middle + SIZE] * shape))[bins] ** 2).sum() + 1e-9
    ratio = 10 * np.log10(ours / theirs)
    return 0 if ratio > OWNER_DB else 1 if ratio < -OWNER_DB else None


def window():
    return np.hanning(SIZE).astype(np.float32)


def spectrum(signal):
    count = (len(signal) - SIZE) // HOP
    out = np.zeros((count, TOP_BIN), dtype=np.complex64)
    shape = window()
    for first in range(0, count, 4096):
        rows = np.arange(first, min(first + 4096, count))
        frames = np.stack([signal[row * HOP:row * HOP + SIZE] * shape for row in rows])
        out[rows] = np.fft.rfft(frames, axis=1)[:, :TOP_BIN]
    return out


def decibels(values):
    return 20 * np.log10(np.abs(values) + 1e-5)


def widened(levels, frames=2, bins=1):
    """The loudest nearby, so that a small slip in time or pitch doesn't count as a difference."""
    out = levels.copy()
    for step in range(1, frames + 1):
        out = np.maximum(out, np.maximum(np.roll(levels, step, axis=0), np.roll(levels, -step, axis=0)))
    across = out.copy()
    for step in range(1, bins + 1):
        out = np.maximum(out, np.maximum(np.roll(across, step, axis=1), np.roll(across, -step, axis=1)))
    return out


def stands_out(mine, other):
    """Frames with something on this recording that isn't on the other one."""
    over = (mine - widened(other) > STANDS_OUT_DB) & (mine > QUIETEST_DB)
    found = over[:, TYPING_BINS[0]:TYPING_BINS[1]].sum(axis=1) >= TYPING_CELLS
    for lines in BUTTON_LINES:
        together = np.ones(len(mine), dtype=bool)
        for hertz in lines:
            line = int(round(hertz / BIN_HZ))
            if line + 2 < mine.shape[1]:
                together &= over[:, line - 1:line + 2].any(axis=1)
        found |= together
    return found


def grown(frames, by):
    return np.convolve(frames.astype(np.int32), np.ones(2 * by + 1, dtype=np.int32), "same") > 0


def measure_lag(sources, part, other, start, end, guess):
    """To the sample, on the music alone, and whether it holds all the way along."""
    rate = RATE // me.LOW_FACTOR

    def low(name, at, count):
        place = sources.offsets[name] // me.LOW_FACTOR + int(at * rate)
        signal = np.asarray(sources.low[place:place + count], dtype=np.float64)
        spectrum_ = np.fft.rfft(signal - signal.mean())
        spectrum_[np.fft.rfftfreq(count, 1 / rate) > 800] = 0
        return np.fft.irfft(spectrum_, count)

    def full(name, at, count):
        place = sources.offsets[name] + int(round(at * RATE))
        return np.asarray(sources.mid[place:place + count], dtype=np.float64)

    window_samples = int(LAG_WINDOW_SECONDS * RATE)
    reach = int(LAG_REACH_SECONDS * RATE)
    lags, locks = [], []
    for at in np.arange(start, end - LAG_WINDOW_SECONDS, LAG_EVERY_SECONDS):
        count = int(LAG_WINDOW_SECONDS * rate)
        a, b = low(part, at, count), low(other, at + guess, count)
        locks.append(float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-12)))
        fixed = full(part, at, window_samples)
        moving = full(other, at + guess - reach / RATE, window_samples + 2 * reach)
        lags.append(int(np.argmax(me.correlate(moving, fixed)[:2 * reach + 1])) - reach)
    good = np.array(locks) > MIN_LOCK
    print(f"  music locks in {good.sum()} of {len(locks)} windows, lag spread {np.ptp(np.array(lags)[good])} samples")
    return guess + float(np.median(np.array(lags)[good])) / RATE


def find_wrap(sources, part, earlier, later, birds, range_start, fade):
    """Which moments lead up alike and have no bird through the fade."""
    rate = RATE // me.LOW_FACTOR
    length = max(WRAP_WINDOW_SECONDS, fade)
    window_samples = int(length * rate)
    base = sources.offsets[part] // me.LOW_FACTOR

    def passed(at, to):
        signal = np.asarray(sources.low[base + int(at * rate):base + int(to * rate)], dtype=np.float64)
        spectrum_ = np.fft.rfft(signal - signal.mean())
        bins = np.fft.rfftfreq(len(signal), 1 / rate)
        spectrum_[(bins < 60) | (bins > 1200)] = 0
        return np.fft.irfft(spectrum_, len(signal))

    def bird_share(at):
        first = max(int((at - fade - range_start) * RATE / HOP), 0)
        return float(birds[first:int((at - range_start) * RATE / HOP)].mean())

    early = passed(earlier[0] - length, earlier[1])
    size = 1 << int(np.ceil(np.log2(len(early) + window_samples)))
    spectrum_ = np.fft.rfft(early, size)
    squares = np.concatenate([[0.0], np.cumsum(early ** 2)])
    energy = squares[window_samples:] - squares[:-window_samples]
    penalty = np.array([bird_share(earlier[0] + i / rate) for i in range(0, len(energy), rate // 20)]) * BIRD_AT_WRAP_COST
    best = (-1e9, 0.0, 0.0, 0.0)
    for end in np.arange(later[0], later[1], 0.05):
        anchor = passed(end - length, end)[:window_samples]
        product = np.fft.irfft(spectrum_ * np.conj(np.fft.rfft(anchor, size)), size)[:len(energy)]
        alike = product / np.sqrt(np.maximum(energy, 1e-9) * ((anchor ** 2).sum() + 1e-9))
        score = alike - np.interp(np.arange(len(energy)), np.arange(len(penalty)) * (rate // 20), penalty) - bird_share(end) * BIRD_AT_WRAP_COST
        place = int(np.argmax(score))
        if score[place] > best[0]:
            best = (float(score[place]), earlier[0] + place / rate, float(end), float(alike[place]))
    return best[1], best[2], best[3]


def spans(frames):
    edges = np.flatnonzero(np.diff(np.concatenate([[0], frames.astype(np.int8), [0]])))
    return edges.reshape(-1, 2)


def choose_path(dirty, birds):
    """The recording to read in each frame: as little dirt as possible, then as few cuts. A cut
    on a bird isn't a cost to weigh up. It just can't happen."""
    frames = dirty.shape[1]
    never = np.inf
    cost = dirty[:, 0].astype(np.float64)
    back = np.zeros((2, frames), dtype=np.int8)
    for frame in range(1, frames):
        extra = never if birds[frame] else SWITCH_FRAMES + me.COST_BAD_CUT * (dirty[0, frame] or dirty[1, frame])
        crossed = cost[::-1] + extra
        stay = cost <= crossed
        back[:, frame] = np.where(stay, [0, 1], [1, 0])
        cost = np.where(stay, cost, crossed) + dirty[:, frame]
    path = np.zeros(frames, dtype=np.int32)
    path[-1] = int(np.argmin(cost))
    for frame in range(frames - 1, 0, -1):
        path[frame - 1] = back[path[frame], frame]
    return path


def build(sources, part, other, start, length, lag, dirty, birds, gain):
    """Takes along the pass, as (phase in samples, recording index), cut only between birds."""
    frames = dirty.shape[1]
    centre = SIZE / 2 / HOP
    birds = grown(birds, BIRD_MARGIN_FRAMES)
    path = choose_path(dirty, birds)
    places = [sources.offsets[part] + int(round(start * RATE)), sources.offsets[other] + int(round((start + lag) * RATE))]
    reach = int(me.CUT_SEARCH_SECONDS * RATE)
    takes = [(0, int(path[0]))]
    steps = []
    for frame in np.flatnonzero(np.diff(path)) + 1:
        phase = int((frame + centre) * HOP)
        came, went = int(path[frame - 1]), int(path[frame])
        offset, step = me.best_cut(sources.stereo, places[came] + phase, places[went] + phase, reach, gain[came], gain[went])
        steps.append(step)
        takes.append((phase + offset, went))
    still_dirty = dirty[path, np.arange(frames)]
    print(f"  {len(takes)} takes, {np.mean(path == 0) * 100:.0f}% from {part}, dirty {np.round(dirty.mean(axis=1) * 100, 1)} -> "
          f"{still_dirty.mean() * 100:.1f}%, birds {birds.mean() * 100:.0f}% of the time, worst step "
          f"{20 * np.log10(max(max(steps, default=1e-6), 1e-6)):.1f} dB")
    return takes, path


def cue_block(part, other, start, end, period, lag, takes, gain, fade, keep, patches):
    names = (part, other)
    places = (start, start + lag)
    lines = [f"loop = {start:.6f} {end:.4f} {period:.6f}\n", f"fade = {fade:g}\n"]
    for which in (0, 1):
        left, right = gain[which]
        lines.append(f"pass = {names[which]} {left:.4f} {right:.4f} 0.000000 {places[which]:.6f}\n")
    lines.extend(f"patch = {a:.4f} {b:.4f}{''.join(f' {hertz:.0f}' for hertz in high)}\n" for a, b, high in patches)
    for index, (phase, which) in enumerate(takes):
        stop = takes[index + 1][0] if index + 1 < len(takes) else int(round(period * RATE))
        left, right = gain[which]
        tag = "." if which == 0 else names[which]
        tail = "" if which == 0 and (left, right) == (1.0, 1.0) else f" {tag} {left:.4f} {right:.4f}"
        lines.append(f"take = {phase / RATE:.6f} {(stop - phase) / RATE:.6f} {places[which]:.6f}{tail}\n")
    lines.extend(f"keep = {a:.4f} {b:.4f}\n" for a, b in keep)
    return "".join(lines)


def insert_block(part, block):
    path = me.ROOT / "data" / "cues" / f"{part}.toml"
    lines = path.read_text().splitlines(True)
    start = float(re.match(r"loop = (\S+)", block)[1])
    after = [number for number, line in enumerate(lines) if line.startswith("loop = ") and float(line.split()[2]) > start]
    where = after[0] if after else len(lines)
    lines[where:where] = block.splitlines(True)
    path.write_text("".join(lines))
    print(f"wrote {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("part", choices=me.PARTS)
    parser.add_argument("earlier", help="where the loop can start, from:to in recording seconds")
    parser.add_argument("later", help="where it can go back from, from:to")
    parser.add_argument("other", choices=me.PARTS)
    parser.add_argument("--lag", type=float, required=True, help="other's time minus this part's, for the music")
    parser.add_argument("--fade", type=float, default=1.5)
    parser.add_argument("--preview", type=pathlib.Path)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    sources = me.Sources(args.part)
    earlier = [float(v) for v in args.earlier.split(":")]
    later = [float(v) for v in args.later.split(":")]
    begin = earlier[0] - max(args.fade, WRAP_WINDOW_SECONDS)
    lag = measure_lag(sources, args.part, args.other, begin, later[1], args.lag)
    print(f"  lag {lag:.5f}")

    def read(name, at, seconds):
        place = sources.offsets[name] + int(round(at * RATE))
        return np.asarray(sources.mid[place:place + int(seconds * RATE)], dtype=np.float32)

    seconds = later[1] - begin
    here, there = spectrum(read(args.part, begin, seconds)), spectrum(read(args.other, begin + lag, seconds))
    count = min(len(here), len(there))
    here, there = here[:count], there[:count]
    level_here, level_there = decibels(here), decibels(there)
    # What stands out for long is a voice, for the first time through to keep.
    voice_here = stands_out(level_here, level_there)
    voice_there = stands_out(level_there, level_here)
    birds = (decibels(here - there)[:, BIRD_BINS[0]:BIRD_BINS[1]] > BIRD_LEFT_DB).sum(axis=1) >= BIRD_CELLS
    del here, there
    start, stop, alike = find_wrap(sources, args.part, earlier, later, birds, begin, args.fade)
    period = round((stop - start) * RATE) / RATE
    print(f"  {start:.3f} to {start + period:.3f}, period {period:.3f}, alike {alike:.2f}")

    def stereo(name, at):
        place = sources.offsets[name] + int(round(at * RATE))
        return np.asarray(sources.stereo[place:place + int(period * RATE)], dtype=np.float32)

    offset = me.level_match(np.stack([stereo(args.part, start), stereo(args.other, start + lag)]), [True, False], RATE)
    gain = [tuple(1.0 if abs(v) < me.LEVEL_SNAP_DB else float(10 ** (-v / 20)) for v in row) for row in offset]
    print(f"  gains dB {[[round(float(-v), 1) for v in row] for row in offset]}")
    first = int((start - begin) * RATE / HOP)
    frames = int(period * RATE / HOP)
    templates = load_templates()
    ours, theirs = read(args.part, start, period).astype(np.float64), read(args.other, start + lag, period).astype(np.float64)
    sounds = find_sounds(ours, theirs, templates)
    owners = [whose(ours, theirs, place, templates[name][0]) for place, name in sounds]
    print(f"  {len(sounds)} sounds: {owners.count(0)} in {args.part}, {owners.count(1)} in {args.other}, {owners.count(None)} unclear")
    dirty = np.zeros((2, frames), dtype=bool)
    for (place, name), owner in zip(sounds, owners):
        length = templates[name][1]
        a, b = max(int(place / HOP - SIZE / 2 / HOP) - DIRT_MARGIN_FRAMES, 0), int((place + length) / HOP - SIZE / 2 / HOP) + DIRT_MARGIN_FRAMES
        for which in (0, 1) if owner is None else (owner,):
            dirty[which, a:b] = True
    for which, voice in enumerate((voice_here, voice_there)):
        for a, b in spans(grown(voice[first:first + frames], DIRT_MARGIN_FRAMES)):
            if (b - a) * HOP / RATE >= VOICE_SECONDS:
                dirty[which, a:b] = True
    takes, path = build(sources, args.part, args.other, start, period, lag, dirty, birds[first:first + frames], gain)
    patches = []
    centre = SIZE / 2 / HOP
    for (place, name), owner in zip(sounds, owners):
        frame = min(max(int(place / HOP - centre), 0), frames - 1)
        if owner is None or path[frame] == owner:
            patches.append((place / RATE - PATCH_BEFORE_SECONDS, (place + templates[name][1]) / RATE + PATCH_AFTER_SECONDS, templates[name][2]))
    print(f"  {len(patches)} of them left on the takes for the render to replace")
    # A long stretch that only one recording has is a voice: the first time through keeps it.
    voices = [(start + a * HOP / RATE, start + b * HOP / RATE) for a, b in spans(grown(voice_here[first:first + frames], DIRT_MARGIN_FRAMES)) if (b - a) * HOP / RATE >= VOICE_SECONDS]
    print(f"  voices kept: {[(round(a, 1), round(b, 1)) for a, b in voices]}")
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
        out = np.zeros((int(round(period * RATE)), 2), dtype=np.float32)
        places = (sources.offsets[args.part] + int(round(start * RATE)), sources.offsets[args.other] + int(round((start + lag) * RATE)))
        for index, (phase, which) in enumerate(takes):
            end = takes[index + 1][0] if index + 1 < len(takes) else len(out)
            out[phase:end] = np.asarray(sources.stereo[places[which] + phase:places[which] + end]) * np.asarray(gain[which], dtype=np.float32)
        me.save_wav(args.preview / f"{args.part}_{int(start)}_after.wav", out, RATE)
        me.save_wav(args.preview / f"{args.part}_{int(start)}_before.wav", stereo(args.part, start), RATE)
        me.save_wav(args.preview / f"{args.other}_{int(start + lag)}_before.wav", stereo(args.other, start + lag), RATE)
    if args.write:
        insert_block(args.part, cue_block(args.part, args.other, start, start + period, period, lag, takes, gain, args.fade, voices, patches))
        # The other part plays the same pass, to its own level.
        inverse = [gain[1], gain[0]]
        inverse = [tuple(a / b for a, b in zip(row, gain[1])) for row in gain]
        flipped = [(phase, 1 - which) for phase, which in takes]
        voices_there = [(start + lag + a * HOP / RATE, start + lag + b * HOP / RATE) for a, b in spans(grown(voice_there[first:first + frames], DIRT_MARGIN_FRAMES)) if (b - a) * HOP / RATE >= VOICE_SECONDS]
        insert_block(args.other, cue_block(args.other, args.part, start + lag, start + lag + period, period, -lag, flipped, [inverse[1], inverse[0]], args.fade, voices_there, patches))


if __name__ == "__main__":
    sys.exit(main())
