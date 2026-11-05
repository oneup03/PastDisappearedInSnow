#!/usr/bin/env python3
"""Find each scene's looping music, and a clean take of it (development tool).

usage: tools/analysis/find_loops.py <zen|chu|kou> [--write] [--preview DIR] [--audio audio] [--map data/cues]

The broadcast's investigation music repeats exactly, while the original player's sound
effects land in different places in each repeat. So stacking the repeats shows both. An
effect only ever adds energy, so a repeat is measured by how far it rises above the others
at that moment: above their median given three or more, above the quieter of two otherwise.
A brief rise is an effect, and a long one is a voice. For each slice of the loop this picks
the repeat that rises least, which makes a clean take as a list of numbers: where to read
from in the player's own recording. Nothing is re-encoded, and no audio goes in the repo.

Repeats are looked for across the whole part, not just the scene, as a track can come back.

--write adds the loops to the part's cue file, except where it already has one. --preview writes WAV files of a stretch of
each loop before and after, to judge by ear; those are ROM-free but are the broadcast's
audio, so they stay local.
"""
import argparse
import pathlib
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from pvs import script as sc  # noqa: E402
from pvs.roms import INPUTS, load_pack  # noqa: E402

RATE = 8000
HOP = 400                 # 50 ms
HOP_SECONDS = HOP / RATE
BANDS = 32
MIN_PERIOD = 8.0
MAX_PERIOD = 260.0
ANCHOR_SECONDS = 6.0      # a stretch to look for elsewhere
ANCHOR_EVERY = 12.0
MATCH_SCORE = 0.70        # samples this alike mean a true repeat
LAG_BIN = 16              # samples at 8 kHz: 2 ms
MIN_VOTES = 3
QUIET = 0.003
SPAN_SCORE = 0.45
STEP_SECONDS = 3.0
GAP_STEPS = 7             # a gap this many steps long doesn't end the music
MIN_RUN_SECONDS = 40.0
SLICE_SECONDS = 0.5       # how finely the clean take may switch between repeats
LONG_STRAY_SECONDS = 2.0  # longer than this isn't an effect: it's a voice or a cue
REPEAT_ANCHORS = 8
REPEAT_VOTES = 2
RISE_FLOOR = 0.25


def decode(path):
    cmd = ["ffmpeg", "-v", "quiet", "-i", str(path), "-ac", "1", "-ar", str(RATE), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


def features(audio):
    """Log band energies, 20 a second. They tolerate tiny timing differences; samples don't."""
    frames = (len(audio) - HOP * 2) // HOP
    window = np.hanning(HOP * 2)
    edges = np.geomspace(3, HOP, BANDS + 1).astype(int)
    out = np.zeros((frames, BANDS), dtype=np.float32)
    for start in range(0, frames, 4096):
        idx = np.arange(start, min(start + 4096, frames))
        chunk = np.stack([audio[i * HOP:i * HOP + HOP * 2] for i in idx]) * window
        spectra = np.abs(np.fft.rfft(chunk, axis=1))
        csum = np.cumsum(spectra, axis=1)
        out[idx] = np.stack([(csum[:, max(b - 1, a)] - csum[:, a]) / max(b - a, 1)
                             for a, b in zip(edges[:-1], edges[1:])], axis=1)
    return np.log(out + 1e-4)


class Matcher:
    """Finds where stretches of a given length occur in some audio, reusing its transform."""

    def __init__(self, audio, anchor_length):
        self.length = anchor_length
        self.count = len(audio) - anchor_length
        self.size = len(audio) + anchor_length
        self.spectrum = np.fft.rfft(audio, self.size)
        energy = np.cumsum(np.concatenate([[0.0], audio.astype(np.float64) ** 2]))
        self.window = energy[anchor_length:len(audio)] - energy[:self.count]

    def scores(self, anchor):
        """How well the anchor matches at each position, from -1 to 1."""
        anchor = anchor - anchor.mean()
        dot = np.fft.irfft(self.spectrum * np.conj(np.fft.rfft(anchor, self.size)), self.size)[:self.count]
        return dot / (np.sqrt(self.window * (anchor ** 2).sum()) + 1e-6)


def find_period(audio):
    """The period the most stretches of this audio agree on, to the sample.

    Music repeats to the sample, which voices and noise never do, so samples are the test.
    Each anchor votes for the distance to each place it matches closely.
    """
    anchor_length = int(ANCHOR_SECONDS * RATE)
    matcher = Matcher(audio, anchor_length)
    votes = {}
    anchors = 0
    for start in range(0, len(audio) - anchor_length, int(ANCHOR_EVERY * RATE)):
        anchor = audio[start:start + anchor_length]
        if np.sqrt((anchor ** 2).mean()) < QUIET:
            continue
        anchors += 1
        scores = matcher.scores(anchor)
        position = np.flatnonzero(scores > MATCH_SCORE)
        if not len(position):
            continue
        # Each cluster of positions is one match; take its peak.
        for group in np.split(position, np.flatnonzero(np.diff(position) > RATE) + 1):
            peak = group[np.argmax(scores[group])]
            lag = abs(int(peak) - start)
            if MIN_PERIOD * RATE <= lag <= MAX_PERIOD * RATE:
                key = round(lag / LAG_BIN)
                votes.setdefault(key, []).append(lag)
    if not votes:
        return 0.0, 0, anchors
    best = max(len(v) for v in votes.values())
    # The shortest lag that's nearly as popular is the period; others are its multiples.
    period_key = min(k for k, v in votes.items() if len(v) >= max(MIN_VOTES, best * 0.5)) if best >= MIN_VOTES else None
    if period_key is None:
        return 0.0, best, anchors
    return float(np.median(votes[period_key])) / RATE, len(votes[period_key]), anchors


def music_span(audio, period):
    """The longest stretch of the audio that matches itself one period later."""
    lag = int(round(period * RATE))
    a, b = audio[:-lag], audio[lag:]
    step = int(STEP_SECONDS * RATE)
    width = int(ANCHOR_SECONDS * RATE)
    flags = []
    for i in range(0, len(a) - width, step):
        x, y = a[i:i + width], b[i:i + width]
        x = x - x.mean()
        y = y - y.mean()
        flags.append(float((x * y).sum() / (np.sqrt((x * x).sum() * (y * y).sum()) + 1e-6)) > SPAN_SCORE)
    # Allow short gaps: an effect or two shouldn't end the music.
    flags = np.array(flags)
    filled = np.convolve(flags, np.ones(GAP_STEPS), "same") > 0
    best = (0, 0)
    start = None
    for i, value in enumerate(np.append(filled, False)):
        if value and start is None:
            start = i
        elif not value and start is not None:
            if i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    return best[0] * step, best[1] * step + width + lag


def find_repeats(whole, matcher, start, period):
    """Everywhere in the part this loop's music plays, as sample positions of its phase 0.

    Stretches at several phases each vote, so an effect over one of them doesn't lose a
    repeat. Returns positions in the whole part's audio, with drift between them measured
    rather than assumed.
    """
    period_samples = int(round(period * RATE))
    found = {}
    anchors = np.linspace(0, max(period_samples - matcher.length, 1), REPEAT_ANCHORS, endpoint=False).astype(int)
    for offset in anchors:
        anchor = whole[start + offset:start + offset + matcher.length]
        if len(anchor) < matcher.length or np.sqrt((anchor ** 2).mean()) < QUIET:
            continue
        scores = matcher.scores(anchor)
        position = np.flatnonzero(scores > MATCH_SCORE)
        if not len(position):
            continue
        for group in np.split(position, np.flatnonzero(np.diff(position) > RATE) + 1):
            peak = int(group[np.argmax(scores[group])])
            phase_zero = peak - int(offset)
            found.setdefault(round(phase_zero / (RATE / 2)), []).append(phase_zero)
    repeats = sorted(int(np.median(v)) for v in found.values() if len(v) >= REPEAT_VOTES)
    # Repeats closer than a period are the same one seen at a neighbouring phase.
    merged = []
    for position in repeats:
        if not merged or position - merged[-1] > period_samples * 0.9:
            merged.append(position)
    return merged


def analyse(all_features, whole, matcher, audio, base_sample):
    """Everything about one scene's music."""
    period, votes, anchors = find_period(audio)
    if not period:
        return {"anchors": anchors, "votes": votes}
    lo, hi = music_span(audio, period)
    if (hi - lo) / RATE / period < 1.5:
        return {"anchors": anchors, "votes": votes}
    repeats = find_repeats(whole, matcher, base_sample + lo, period)
    if len(repeats) < 2:
        return {"anchors": anchors, "votes": votes}
    # Stack the repeats, measuring how far each rises above the others at each moment.
    phase_hops = int(round(period / HOP_SECONDS))
    starts = [r // HOP for r in repeats if r // HOP + phase_hops <= len(all_features) and r >= 0]
    stack = np.stack([all_features[start:start + phase_hops] for start in starts])
    baseline = np.median(stack, axis=0) if len(starts) >= 3 else stack.min(axis=0)
    rise = np.clip(stack - baseline, 0, None).mean(axis=2)  # repeat x time
    threshold = max(float(np.percentile(rise, 75)) * 2.0, RISE_FLOOR)
    effects, voices = [], []
    for repeat, row in enumerate(rise > threshold):
        i = 0
        while i < len(row):
            if row[i]:
                j = i
                while j < len(row) and row[j]:
                    j += 1
                moment = (starts[repeat] + i) * HOP_SECONDS
                if (j - i) * HOP_SECONDS >= LONG_STRAY_SECONDS:
                    voices.append((moment, moment + (j - i) * HOP_SECONDS))
                else:
                    effects.append(moment)
                i = j
            else:
                i += 1
    # The clean take: for each slice of the loop, the repeat that rises least.
    slice_hops = int(SLICE_SECONDS / HOP_SECONDS)
    slices = [rise[:, i:i + slice_hops].mean(axis=1) for i in range(0, phase_hops, slice_hops)]
    picks = [int(np.argmin(level)) for level in slices]
    # Staying with one repeat as long as it's fine means fewer joins.
    fine = np.stack(slices, axis=1) < threshold
    current = picks[0]
    for number in range(len(picks)):
        if fine[current, number]:
            picks[number] = current
        else:
            current = picks[number]
    splices = []
    for number, pick in enumerate(picks):
        if splices and splices[-1][0] == pick:
            splices[-1][2] += SLICE_SECONDS
        else:
            splices.append([pick, number * SLICE_SECONDS, SLICE_SECONDS])
    take_rise = np.array([rise[pick, number * slice_hops:(number + 1) * slice_hops].mean() for number, pick in enumerate(picks)])
    return {"starts": [s * HOP_SECONDS for s in starts], "period": period, "votes": votes, "anchors": anchors,
            "effects": effects, "voices": voices, "splices": splices,
            "dirty_before": float((rise > threshold).mean()), "dirty_after": float((take_rise > threshold).mean()),
            "span": (base_sample + lo, base_sample + hi)}


def cue_lines(result):
    """The numbers the player needs for one loop."""
    span_start, span_end = result["span"][0] / RATE, result["span"][1] / RATE
    period, starts = result["period"], result["starts"]
    # Phase zero for this stretch: the latest repeat at or before it, or else one period back.
    before = [t for t in starts if t <= span_start + 1.0]
    anchor = before[-1] if before else starts[0]
    while anchor > span_start + 1.0:
        anchor -= period
    lines = [f"loop = {anchor:.4f} {span_end:.4f} {period:.4f}\n",
             f"repeats = {period:.4f} " + " ".join(f"{t:.4f}" for t in starts) + "\n"]
    for repeat, phase, length in result["splices"]:
        lines.append(f"take = {phase:.2f} {length:.2f} {starts[repeat]:.4f}\n")
    for start, end in result["voices"]:
        if span_start <= start < span_end:
            lines.append(f"keep = {start:.2f} {end:.2f}\n")
    return lines


def write_preview(folder, part, number, result, source):
    """A minute of this loop from the recording, and the same from the clean take."""
    folder.mkdir(parents=True, exist_ok=True)
    period, starts = result["period"], result["starts"]
    seconds = min(period, 60.0)
    rate = 32000

    def read(start, length):
        cmd = ["ffmpeg", "-v", "quiet", "-ss", f"{start:.4f}", "-t", f"{length:.4f}", "-i", str(source),
               "-ac", "2", "-ar", str(rate), "-f", "s16le", "-"]
        return subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout

    def save(name, pcm):
        path = folder / f"{part}_scene{number:02d}_{name}.wav"
        cmd = ["ffmpeg", "-v", "quiet", "-y", "-f", "s16le", "-ar", str(rate), "-ac", "2", "-i", "-", str(path)]
        subprocess.run(cmd, input=pcm, check=True)

    # The repeat with the most effects shows the difference best.
    moments = np.array(result["effects"]) if result["effects"] else np.array([starts[0]])
    busiest = max(starts, key=lambda t: int(((moments >= t) & (moments < t + seconds)).sum()))
    save("before", read(busiest, seconds))
    clean = b""
    for repeat, phase, length in result["splices"]:
        if phase >= seconds:
            break
        clean += read(starts[repeat] + phase, min(length, seconds - phase))
    save("after", clean)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("part", choices=("zen", "chu", "kou"))
    ap.add_argument("--audio", default="audio")
    ap.add_argument("--map", default="data/cues")
    ap.add_argument("--dir", default=INPUTS)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--preview", type=pathlib.Path)
    args = ap.parse_args()
    text = (pathlib.Path(args.map) / f"{args.part}.toml").read_text()
    rate = float(re.search(r"rate = ([\d.]+)", text)[1])
    offset = float(re.search(r"offset = (-?[\d.]+)", text)[1])
    data = load_pack(args.part, args.dir)
    audio = decode(pathlib.Path(args.audio) / f"{args.part}.mp3")
    all_features = features(audio)
    matcher = Matcher(audio, int(ANCHOR_SECONDS * RATE))
    scenes = sc.schedule(data)
    # Loops already in the file stay: some were found by hand, and all may have been cleaned.
    existing = [(float(start), float(end)) for start, end in re.findall(r"^loop = (\S+) (\S+) ", text, re.M)]
    cues = []
    timed = [(n, start) for n, (_, start) in enumerate(scenes) if start is not None]
    for index, (number, start) in enumerate(timed):
        end = timed[index + 1][1] if index + 1 < len(timed) else start + 300
        lo = (start - offset) / rate
        hi = (end - offset) / rate
        a, b = int(max(lo, 0) / HOP_SECONDS), int(hi / HOP_SECONDS)
        if b - a < int(MIN_RUN_SECONDS / HOP_SECONDS):
            print(f"scene {number:2d} {start // 60:2d}:{start % 60:02d} too short")
            continue
        result = analyse(all_features, audio, matcher, audio[a * HOP:b * HOP], a * HOP)
        if "period" not in result:
            print(f"scene {number:2d} {start // 60:2d}:{start % 60:02d} no loop in {hi - lo:5.0f}s "
                  f"({result['votes']} votes from {result['anchors']} anchors)")
            continue
        print(f"scene {number:2d} {start // 60:2d}:{start % 60:02d} loop {result['period']:8.3f}s, {len(result['starts'])} repeats at "
              + " ".join(f"{t:.0f}" for t in result["starts"][:8])
              + f"; {len(result['effects'])} effects, {len(result['voices'])} long, {len(result['splices'])} splices, "
              f"dirty {result['dirty_before'] * 100:.1f}% to {result['dirty_after'] * 100:.1f}%", flush=True)
        span = (result["span"][0] / RATE, result["span"][1] / RATE)
        if any(span[0] < end and start < span[1] for start, end in existing):
            print("    already has a loop in the cue file: keeping that one")
        else:
            cues.extend(cue_lines(result))
        if args.preview:
            write_preview(args.preview, args.part, number, result, pathlib.Path(args.audio) / f"{args.part}.mp3")
    if args.write:
        path = pathlib.Path(args.map) / f"{args.part}.toml"
        path.write_text(text + "".join(cues))
        print(f"wrote {len(cues)} lines to {path}")


if __name__ == "__main__":
    main()
