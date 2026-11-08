#!/usr/bin/env python3
"""Take each of the broadcast's looping tracks round the original player's sound effects.

usage: tools/analysis/match_effects.py <zen|chu|kou> [--preview DIR] [--write] [--only N] [--search]

The broadcast's music came down the satellite channel, while the effects came from the
player's own sound chip. A track repeats exactly, within a part and across the three parts,
and the effects land elsewhere each time. So if two passes cancel to their noise floor at
some moment, neither has anything on it then, as no two runs of the game make a sound at
the same instant to the sample. Where nothing agrees, the pass standing least above the
others is the best there is.

This reads the repeats tools/analysis/find_loops.py put in data/cues/<part>.toml, finds the same
track in all three recordings, and chooses which to read at every moment, cutting where the
two waveforms coincide. Only numbers go in the cue file. The result can only be judged by
ear, as numbers have fooled this before, and --preview writes the audio to listen to.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import wave

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from pvs import script as sc  # noqa: E402
from pvs.roms import load_pack  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
FX_DIR = ROOT / "local" / "rec" / "fx"

PARTS = ("zen", "chu", "kou")
RATE = 32000                       # analysis rate: every recording is decoded to this
LOW_FACTOR = 4                     # searching for a track doesn't need full rate
SEARCH_BLOCK = 1 << 19

ALIGN_WINDOW = 1.0
ALIGN_EVERY = 1.0
ALIGN_SEARCH = 1.0
ALIGN_SCORE = 0.8
ALIGN_AGREE_SECONDS = 12.0
SEAM_LOST_SECONDS = 4.0            # lock lost this long before the end means a restart
OUTLIER_SAMPLES = 3.0
LAG_JUMP_SAMPLES = 4.0
ELSEWHERE_ANCHOR_SECONDS = 6.0
ELSEWHERE_ANCHORS = 8
ELSEWHERE_SCORE = 0.6
ELSEWHERE_VOTES = 2
SCENE_OVERRUN_SECONDS = 10.0
PASS_MARGIN_SAMPLES = 1024         # the player's final render reads a little past the end
JOIN_SEARCH_SAMPLES = 800
JOIN_WINDOW_SECONDS = 0.5

SLICE_SECONDS = 0.01
EFFECT_MARGIN = 0.025              # MP3 smears a sound by about a granule each way
RISE_WINDOW_SECONDS = 0.008
RISE_BANDS = 12
RISE_BAND_RANGE = (500.0, 10000.0)
RISE_TOP_BANDS = 4                 # an effect is narrow: averaging every band dilutes it
AGREE_BANDS = ((800.0, 3200.0), (3200.0, 8000.0))  # a faint sound shows in one or the other
AGREE_HOP_SECONDS = 0.008
AGREE_DB = 8.0                     # within this of the floor counts as cancelling
AGREE_WORST_FLOOR_DB = -3.0        # unrelated audio cancels to +3 dB, and MP3 noise to about -8
ABSENT_SECONDS = 0.25              # this long well under the others: the music's ducked
ABSENT_DB = 3.0
ABSENT_BANDS = 7                   # the lower bands: the top of a lower bitrate has holes
COST_ABSENT = 5.0
LEVEL_BAND = (150.0, 5000.0)
TONE_EDGES = (60.0, 120.0, 240.0, 480.0, 960.0, 1920.0, 3840.0, 7680.0, 11000.0)  # see TONE_BANDS in src/loop_render.h
TONE_SECONDS = 0.128               # the window tone is measured in
TONE_SMALLEST_DB = 0.3             # nearer than this counts as the same
TONE_MOST_DB = 8.0
TONE_NOTHING_DB = -60.0            # above 11 kHz a lower bitrate simply has nothing
VOICE_MARGIN_SECONDS = 0.3         # a voice kept as it is, with this either side
DUCKED_DB = 3.0                    # a whole pass this far off has its music mixed under something
LEVEL_SNAP_DB = 0.15
VOICE_SECONDS = 1.5                # this long with next to no agreement is a voice or a cue
VOICE_AGREEMENT = 0.25             # typing leaves gaps where passes agree, and a voice doesn't
COST_DIRTY = 1.0
COST_LOUD = 0.5                    # per dB standing out, among passes that don't agree
COST_SWITCH = 3.0
COST_SWITCH_RECORDING = 40.0       # into another recording: its drums and top end differ, which shows
COST_INVALID = 1e6                 # a pass that isn't this track here
COST_RANK = 0.02                   # per slice, per step down in bitrate: only a tie-break
COST_BAD_CUT = 50.0
CUT_SEARCH_SECONDS = 0.02
MAX_REPEATS = 12
BY_HAND = "# by hand"              # a line in a cue file that writing a loop leaves alone


def read_recording(part):
    """A whole recording, decoded once to a local file at the analysis rate."""
    path = FX_DIR / f"{part}.f32"
    if not path.exists():
        FX_DIR.mkdir(parents=True, exist_ok=True)
        subprocess.run(["ffmpeg", "-v", "quiet", "-y", "-i", str(ROOT / "audio" / f"{part}.mp3"), "-ac", "2",
                        "-ar", str(RATE), "-f", "f32le", str(path)], check=True)
    return np.memmap(path, dtype=np.float32, mode="r").reshape(-1, 2)


def part_bitrate(part):
    return int(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=bit_rate", "-of", "csv=p=0",
         str(ROOT / "audio" / f"{part}.mp3")], stdout=subprocess.PIPE, check=True, text=True).stdout.strip())


class Sources:
    """All the recordings end to end, so a loop can read from any of them. A position in
    `stereo`, `mid` or (over LOW_FACTOR) `low` is a sample across all of them."""

    def __init__(self, part):
        self.rate = RATE
        self.part = part
        self.bitrates = {p: part_bitrate(p) for p in PARTS}
        self.offsets, at = {}, 0
        for name in PARTS:
            self.offsets[name] = at
            at += len(read_recording(name))
        self.total = at
        self.stereo = self.joined(FX_DIR / "all.stereo", 2)
        self.mid = self.joined(FX_DIR / "all.mid", 1)
        path = FX_DIR / "all.low"
        if not path.exists():
            with open(path, "wb") as out:
                for start in range(0, len(self.mid) // LOW_FACTOR * LOW_FACTOR, LOW_FACTOR << 18):
                    block = np.asarray(self.mid[start:start + (LOW_FACTOR << 18)])
                    usable = len(block) // LOW_FACTOR * LOW_FACTOR
                    out.write(block[:usable].reshape(-1, LOW_FACTOR).mean(axis=1, dtype=np.float32).tobytes())
        self.low = np.memmap(path, dtype=np.float32, mode="r")

    def joined(self, path, channels):
        if not path.exists():
            with open(path, "wb") as out:
                for name in PARTS:
                    source = read_recording(name)
                    for at in range(0, len(source), 1 << 20):
                        block = np.asarray(source[at:at + (1 << 20)])
                        out.write((block if channels == 2 else block.mean(axis=1, dtype=np.float32)).tobytes())
        data = np.memmap(path, dtype=np.float32, mode="r")
        return data.reshape(-1, 2) if channels == 2 else data

    def locate(self, position):
        """(part, sample within that part's recording)."""
        for name in reversed(PARTS):
            if position >= self.offsets[name]:
                return name, position - self.offsets[name]
        raise ValueError(position)

    def bitrate_at(self, position):
        return self.bitrates[self.locate(position)[0]]

    def find_everywhere(self, known, period):
        """Every other pass of this track in any recording, as positions of phase zero."""
        length = int(period * self.rate)
        window = int(ELSEWHERE_ANCHOR_SECONDS * self.rate) // LOW_FACTOR
        votes = []
        for share in np.linspace(0.1, 0.9, ELSEWHERE_ANCHORS):
            offset = int(length * share)
            place = (known[0] + offset) // LOW_FACTOR
            anchor = np.asarray(self.low[place:place + window], dtype=np.float64)
            for position in search(self.low, 0, len(self.low), anchor):
                votes.append(position * LOW_FACTOR - offset)
        if not votes:
            return []
        votes = np.sort(np.array(votes, dtype=np.int64))
        found = [int(np.median(group)) for group in np.split(votes, np.flatnonzero(np.diff(votes) > self.rate) + 1)
                 if len(group) >= ELSEWHERE_VOTES]
        merged = list(known)
        for position in found:
            if position >= 0 and all(abs(position - other) > length * 0.9 for other in merged):
                merged.append(position)
        return sorted(position for position in merged if position not in known)


def search(mid, low, high, anchor):
    """Everywhere in a stretch that a window of audio matches closely, in fixed blocks."""
    length = len(anchor)
    hop = SEARCH_BLOCK - length
    spectrum = np.conj(np.fft.rfft(anchor, SEARCH_BLOCK))
    power = (anchor ** 2).sum()
    hits = []
    for at in range(low, high - length, hop):
        block = np.asarray(mid[at:at + SEARCH_BLOCK], dtype=np.float64)
        if len(block) <= length:
            break
        product = np.fft.irfft(np.fft.rfft(block, SEARCH_BLOCK) * spectrum, SEARCH_BLOCK)[:min(hop, len(block) - length)]
        energy = np.cumsum(np.concatenate([[0.0], block ** 2]))
        window = energy[length:length + len(product)] - energy[:len(product)]
        score = product / np.sqrt(np.maximum(window, 1e-12) * power)
        good = np.flatnonzero(score > ELSEWHERE_SCORE)
        if len(good):
            for group in np.split(good, np.flatnonzero(np.diff(good) > len(anchor) // 4) + 1):
                hits.append(at + int(group[np.argmax(score[group])]))
    return hits


def correlate(fixed, moving):
    size = 1 << int(np.ceil(np.log2(len(fixed) + len(moving))))
    return np.fft.irfft(np.fft.rfft(fixed, size) * np.conj(np.fft.rfft(moving, size)), size)


class Repeat:
    """One pass of a loop, measured against the first. position(n) is where phase sample n
    is in the recording. Lag isn't one straight line: it drifts slowly, and jumps by a few
    samples where the broadcast's source restarted its track."""

    def __init__(self, start, at, lags):
        self.start = start
        points = [(at[0], lags[0])]
        for x, lag in zip(at[1:], lags[1:]):
            if abs(lag - points[-1][1]) > LAG_JUMP_SAMPLES:
                middle = (points[-1][0] + x) / 2
                points += [(middle - 1, points[-1][1]), (middle, lag)]
            points.append((x, lag))
        self.at, self.lags = zip(*points)

    def position(self, phase):
        return (self.start + phase + np.round(np.interp(phase, self.at, self.lags))).astype(np.int64)


def steady(at, lags, rate):
    """Which measurements another agrees with, nearby. A lone reading is a false match."""
    near = np.abs(at[:, None] - at[None, :]) < ALIGN_AGREE_SECONDS * rate
    same = np.abs(lags[:, None] - lags[None, :]) <= OUTLIER_SAMPLES
    return (near & same).sum(axis=1) > 1


def measure_lags(mid, first, start, length, rate):
    """Where a repeat lines up with the first, in short windows, so that a clean moment
    between a voice's words still counts. Returns phases and lags in samples."""
    window = int(ALIGN_WINDOW * rate)
    step = int(ALIGN_EVERY * rate)
    pad = int(ALIGN_SEARCH * rate)
    at, lags = [], []
    for place in range(0, length - window, step):
        anchor = first[place:place + window]
        low = start + place - pad
        if low < 0 or low + window + 2 * pad > len(mid):
            continue
        stretch = np.asarray(mid[low:low + window + 2 * pad], dtype=np.float64)
        product = correlate(stretch, anchor)[:2 * pad + 1]
        peak = int(np.argmax(product))
        norm = np.sqrt((anchor ** 2).sum() * (stretch[peak:peak + window] ** 2).sum()) + 1e-12
        if product[peak] / norm > ALIGN_SCORE:
            at.append(place + window / 2)
            lags.append(float(peak - pad))
    at, lags = np.array(at), np.array(lags)
    if len(lags) < 2:
        return at, lags
    good = steady(at, lags, rate)
    return at[good], lags[good]


def order_by_lock(mid, starts, length, rate):
    """The repeats with the one that matches the others best first, as everything's measured
    against it. A repeat that spills into another scene's voices would make a poor ruler."""
    if len(starts) < 3:
        return starts
    locks = np.zeros(len(starts))
    for i, start in enumerate(starts):
        first = np.asarray(mid[start:start + length], dtype=np.float64)
        for j, other in enumerate(starts):
            if j > i:
                count = len(measure_lags(mid, first, other, length, rate)[0])
                locks[i] += count
                locks[j] += count
    best = int(np.argmax(locks))
    return [starts[best]] + starts[:best] + starts[best + 1:]


def align_repeats(mid, starts, length, rate):
    """Measure each repeat against the first, at full rate, wherever they match."""
    first = np.asarray(mid[starts[0]:starts[0] + length], dtype=np.float64)
    repeats = [Repeat(starts[0], [0.0], [0.0])]
    for start in starts[1:]:
        at, lags = measure_lags(mid, first, start, length, rate)
        repeats.append(Repeat(start, list(at), list(lags)) if len(lags) >= 2 else None)
    return repeats


def find_seam(mid, starts, length, rate):
    """The phase where the broadcast's source restarted its track, if one shows.

    Repeats stay locked to each other for a whole pass, then a restart shifts them by a few
    samples, or loses the lock altogether. So it's the last moment before a jump or a loss.
    """
    first = np.asarray(mid[starts[0]:starts[0] + length], dtype=np.float64)
    seams = []
    for start in starts[1:]:
        at, lags = measure_lags(mid, first, start, length, rate)
        if len(lags) < 2:
            continue
        jumps = np.flatnonzero(np.abs(np.diff(lags)) > LAG_JUMP_SAMPLES)
        if len(jumps):
            seams.append((at[jumps[0]], lags[jumps[0] + 1]))
        elif at[-1] < length - SEAM_LOST_SECONDS * rate:
            seams.append((at[-1], None))
    if not seams:
        return None
    # Each repeat only shows the seam's bracket, so trust whichever stayed locked longest.
    return int(max(place for place, _ in seams) + ALIGN_WINDOW * rate / 2)


def envelope(signal, hop):
    usable = len(signal) // hop * hop
    return 20 * np.log10(np.maximum(np.sqrt((signal[:usable].reshape(-1, hop) ** 2).mean(axis=1)), 1e-6))


def runs_of(row):
    """(from, to) of each True stretch."""
    edges = np.flatnonzero(np.diff(np.concatenate([[0], row.astype(np.int8), [0]])))
    return list(zip(edges[::2], edges[1::2]))


def band_levels(signal, rate):
    """Level in dB per band, in short overlapping windows: band x frame."""
    window = int(RISE_WINDOW_SECONDS * rate)
    hop = window // 2
    frames = (len(signal) - window) // hop
    taper = np.hanning(window)
    bins = np.fft.rfftfreq(window, 1 / rate)
    edges = np.geomspace(*RISE_BAND_RANGE, RISE_BANDS + 1)
    masks = [(bins >= low) & (bins < high) for low, high in zip(edges, edges[1:])]
    out = np.zeros((RISE_BANDS, frames), dtype=np.float32)
    for start in range(0, frames, 8192):
        count = min(8192, frames - start)
        chunk = np.stack([signal[(start + i) * hop:(start + i) * hop + window] for i in range(count)]) * taper
        power = np.abs(np.fft.rfft(chunk, axis=1)) ** 2
        for band, mask in enumerate(masks):
            out[band, start:start + count] = power[:, mask].mean(axis=1) if mask.any() else 0
    smooth = np.ones(3) / 3
    return 10 * np.log10(np.stack([np.convolve(row, smooth, "same") for row in out]) + 1e-9)


def rise_scores(levels):
    """How far each repeat stands above the music, frame by frame: repeat x frame.

    The music is the same in every repeat, and an effect or a voice only adds to it. So in each
    band the second quietest of three or more is the music, even when most have something on
    top. The very quietest may be missing it, or have a hole from the MP3 encoder. Two repeats
    can only be compared with each other.
    """
    count = levels.shape[0]
    scores = np.zeros((count, levels.shape[2]), dtype=np.float32)
    if np.isnan(levels).any():
        # Passes that cover only part of the track: the second quietest of those that are there.
        filled = np.where(np.isnan(levels), np.inf, levels)
        ordered = np.sort(filled, axis=0)
        present = (~np.isnan(levels)).sum(axis=0)
        music = np.where(present > 2, ordered[1], ordered[0])
        music = np.where(np.isfinite(music), music, 0.0)
        for repeat in range(count):
            rise = np.nan_to_num(levels[repeat] - music, nan=0.0)
            # With only two there, the quieter one is the music: compare each with the other.
            scores[repeat] = np.clip(np.sort(rise, axis=0)[-RISE_TOP_BANDS:], 0, None).mean(axis=0)
        return scores
    music = np.partition(levels, 1, axis=0)[1] if count > 2 else None
    for repeat in range(count):
        rise = levels[repeat] - (music if count > 2 else levels[1 - repeat])
        scores[repeat] = np.clip(np.sort(rise, axis=0)[-RISE_TOP_BANDS:], 0, None).mean(axis=0)
    return scores


def to_slices(frames, rate, slices):
    """Flags per band_levels frame, as flags per slice of the loop."""
    per_slice = SLICE_SECONDS * rate / (int(RISE_WINDOW_SECONDS * rate) // 2)
    out = np.zeros(slices, dtype=bool)
    for start, end in runs_of(frames):
        out[int(start / per_slice):min(int(end / per_slice) + 1, slices)] = True
    return out


def absent_from(levels, rate):
    """Frames where a repeat stays under the others: its music is ducked or hasn't started.
    A quieter pass looks cleaner, so this has to count against it. Needs three to tell."""
    count = levels.shape[0]
    if count < 3:
        return np.zeros((count, levels.shape[2]), dtype=bool)
    span = int(ABSENT_SECONDS * rate / (int(RISE_WINDOW_SECONDS * rate) // 2))
    with np.errstate(all="ignore"):
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)
            fall = (np.nanmedian(levels, axis=0)[:ABSENT_BANDS] - levels[:, :ABSENT_BANDS]).mean(axis=1)
    fall = np.nan_to_num(fall, nan=0.0)
    return np.stack([np.convolve(row, np.ones(span) / span, "same") > ABSENT_DB for row in fall])


def level_match(takes, own, rate, valid=None):
    """Each pass's offset from the others, per channel, in dB: repeat x channel. The same track
    was mixed a little differently from one scene to another, and a cut would show it. Passes
    are measured a second at a time and the median taken, so effects and voices don't count."""
    count, length = takes.shape[:2]
    seconds = length // rate
    levels = np.zeros((count, 2, seconds))
    bins = np.fft.rfftfreq(length, 1 / rate)
    for repeat in range(count):
        for channel in range(2):
            spectrum = np.fft.rfft(takes[repeat, :, channel])
            spectrum[(bins < LEVEL_BAND[0]) | (bins >= LEVEL_BAND[1])] = 0
            band = np.fft.irfft(spectrum, length)[:seconds * rate].reshape(-1, rate)
            levels[repeat, channel] = 20 * np.log10(np.sqrt((band ** 2).mean(axis=1)) + 1e-6)
    if valid is not None:
        ok = np.stack([row[:seconds * rate].reshape(-1, rate).all(axis=1) for row in valid])
        levels = np.where(ok[:, None, :], levels, np.nan)
        # Each pass against those that share its seconds.
        offset = np.nan_to_num(np.nanmedian(levels - np.nanmedian(levels, axis=0), axis=2), nan=0.0)
    else:
        offset = np.median(levels - np.median(levels, axis=0), axis=2)
    mine = [i for i in range(count) if own[i]]
    return offset - (np.median(offset[mine], axis=0) if mine else 0.0)


def band_power(signal, rate):
    """Power by tone band in short windows: window x band, in dB."""
    window = int(TONE_SECONDS * rate)
    frames = len(signal) // window
    spectrum = np.abs(np.fft.rfft(signal[:frames * window].reshape(frames, window) * np.hanning(window), axis=1)) ** 2
    bins = np.fft.rfftfreq(window, 1 / rate)
    edges = (0.0,) + TONE_EDGES + (rate / 2 + 1,)
    return 10 * np.log10(np.stack([spectrum[:, (bins >= low) & (bins < high)].mean(axis=1) for low, high in zip(edges, edges[1:])], axis=1) + 1e-12)


def match_tone(takes, names, part, rate, valid=None):
    """How to filter each other recording's takes to sound like this part's own, in dB per tone
    band: {name: gains}. The same track came down a different chain each time, and a cut
    between two would show. Measured against this part's passes, wherever both are the track,
    and the median taken, so effects and voices don't count."""
    window = int(TONE_SECONDS * rate)
    powers = [band_power(take.mean(axis=1), rate) for take in takes]
    frames = powers[0].shape[0]
    usable = [np.ones(frames, dtype=bool) for _ in powers] if valid is None else [row[:frames * window].reshape(frames, window).all(axis=1) for row in valid]
    own = [i for i, name in enumerate(names) if name == part]
    tones = {}
    for name in sorted(set(names) - {part}):
        differences = []
        for i in (i for i, other in enumerate(names) if other == name):
            for j in own:
                both = usable[i] & usable[j]
                if both.any():
                    differences.append(powers[j][both] - powers[i][both])
        if not differences or not own:
            continue
        gains = np.median(np.concatenate(differences), axis=0)
        own_top = np.median(np.concatenate([powers[j][usable[j], -1] - powers[j][usable[j], -2] for j in own]))
        gains[:-1] = np.clip(gains[:-1], -TONE_MOST_DB, TONE_MOST_DB)
        gains[np.abs(gains) < TONE_SMALLEST_DB] = 0.0
        # Above 11 kHz: take it away where this part has none, and leave it where this part has.
        gains[-1] = -99.0 if own_top < TONE_NOTHING_DB else 0.0
        if np.any(gains != 0.0):
            tones[name] = [round(float(g), 1) for g in gains]
    return tones


def band_passed(take, rate):
    """A take split into bands: band x sample."""
    spectrum = np.fft.rfft(take)
    bins = np.fft.rfftfreq(len(take), 1 / rate)
    return [np.fft.irfft(spectrum * ((bins >= low) & (bins < high)), len(take)).astype(np.float32)
            for low, high in AGREE_BANDS]


def find_agreement(takes, rate, slices, valid=None):
    """For each repeat and slice of the loop: does another repeat cancel with it to the floor?

    If two do, neither has anything on it. An effect in either, however faint, or a voice,
    stops them cancelling in at least one band. Levels are against the music at that moment,
    as what's left after cancelling grows with it. Returns (agreed, voices), booleans,
    repeat x slice.
    """
    count = len(takes)
    hop = int(AGREE_HOP_SECONDS * rate)
    passed = [band_passed(take, rate) for take in takes]
    levels = [[envelope(band, hop) for band in row] for row in passed]
    agree = np.zeros((count, len(levels[0][0])), dtype=bool)
    usable = np.ones(agree.shape, dtype=bool) if valid is None else np.stack([row[:agree.shape[1] * hop].reshape(-1, hop).all(axis=1) for row in valid])
    for i in range(count):
        for j in range(i):
            both = usable[i] & usable[j]
            if not both.any():
                continue
            together = both.copy()
            for band in range(len(AGREE_BANDS)):
                relative = envelope(passed[i][band] - passed[j][band], hop) - np.minimum(levels[i][band], levels[j][band])
                floor = np.percentile(relative[both], 10)
                together &= floor <= AGREE_WORST_FLOOR_DB
                together &= relative < floor + AGREE_DB
            agree[i] |= together
            agree[j] |= together
    per_slice = SLICE_SECONDS / AGREE_HOP_SECONDS
    margin = int(np.ceil(EFFECT_MARGIN / SLICE_SECONDS))
    span = int(VOICE_SECONDS / AGREE_HOP_SECONDS)
    agreed = np.ones((count, slices), dtype=bool)
    voices = np.zeros((count, slices), dtype=bool)
    for repeat in range(count):
        # What isn't agreed, with a margin: MP3 smears a sound by about a granule each way.
        for start, end in runs_of(~agree[repeat]):
            agreed[repeat, max(int(start / per_slice) - margin, 0):min(int(end / per_slice) + margin + 1, slices)] = False
        share = np.convolve(agree[repeat], np.ones(span) / span, "same")
        for start, end in runs_of(share < VOICE_AGREEMENT):
            voices[repeat, max(int((start - span // 2) / per_slice), 0):min(int((end + span // 2) / per_slice) + 1, slices)] = True
    return agreed, voices


def find_dirt(takes, rate, valid=None):
    """For each repeat and slice of the loop: how much of a problem is reading from it?

    Returns (cost, unagreed, voices), repeat x slice: a cost to minimise, and booleans.
    """
    slices = takes.shape[1] // int(SLICE_SECONDS * rate)
    agreed, voices = find_agreement(takes, rate, slices, valid)
    levels = np.stack([band_levels(take, rate) for take in takes])
    frame_ok = None
    if valid is not None:
        # Where a pass isn't this track, it mustn't count as the music or against the others.
        hop = int(RISE_WINDOW_SECONDS * rate) // 2
        frame_ok = np.stack([row[:levels.shape[2] * hop].reshape(-1, hop).all(axis=1) for row in valid])
        levels = np.where(frame_ok[:, None, :], levels, np.nan)
    scores = rise_scores(levels)
    absent = np.stack([to_slices(row, rate, slices) for row in absent_from(levels, rate)])
    per_slice = SLICE_SECONDS * rate / (int(RISE_WINDOW_SECONDS * rate) // 2)
    starts = np.minimum((np.arange(slices) * per_slice).astype(int), scores.shape[1] - 1)
    loud = np.stack([np.maximum.reduceat(row, starts) for row in scores])
    voices = voices | absent
    unagreed = ~agreed | voices
    cost = unagreed * (COST_DIRTY + COST_LOUD * loud) + absent * COST_ABSENT
    if valid is not None:
        hop = int(SLICE_SECONDS * rate)
        slice_ok = np.stack([row[:slices * hop].reshape(-1, hop).all(axis=1) for row in valid])
        cost = np.where(slice_ok, cost, COST_INVALID)
        unagreed |= ~slice_ok
        voices &= slice_ok
    return cost, unagreed, voices


def choose_path(cost_per_slice, dirty, rank, recording=None):
    """The repeat to read in each slice: as little dirt as possible, then as few cuts, and
    fewest of all into another recording, whose sound differs."""
    count, slices = dirty.shape
    base = cost_per_slice + np.asarray(rank)[:, None] * COST_RANK
    cost = base[:, 0].copy()
    back = np.zeros((count, slices), dtype=np.int32)
    names = list(recording) if recording is not None else [0] * count
    switch = COST_SWITCH + COST_SWITCH_RECORDING * np.array([[a != b for b in names] for a in names], dtype=float)
    np.fill_diagonal(switch, 0.0)
    for t in range(1, slices):
        # Cutting into or out of something dirty would make a click of it.
        leave = cost + dirty[:, t] * COST_BAD_CUT
        arrive = leave[:, None] + switch + dirty[None, :, t] * COST_BAD_CUT  # from x to
        np.fill_diagonal(arrive, np.inf)
        best = np.argmin(arrive, axis=0)
        arrive_cost = arrive[best, np.arange(count)]
        stay = cost <= arrive_cost
        back[:, t] = np.where(stay, np.arange(count), best)
        cost = np.where(stay, cost, arrive_cost) + base[:, t]
    path = np.zeros(slices, dtype=np.int32)
    path[-1] = int(np.argmin(cost))
    for t in range(slices - 1, 0, -1):
        path[t - 1] = back[path[t], t]
    return path


def best_cut(stereo, playing, incoming, reach, playing_gain=(1.0, 1.0), incoming_gain=(1.0, 1.0)):
    """Of the samples around a cut, the offset where both channels of the two coincide."""
    reach = max(min(reach, playing, incoming, len(stereo) - 1 - playing, len(stereo) - 1 - incoming), 0)
    a = np.asarray(stereo[playing - reach:playing + reach + 1], dtype=np.float64) * playing_gain
    b = np.asarray(stereo[incoming - reach:incoming + reach + 1], dtype=np.float64) * incoming_gain
    step = np.abs(a - b).sum(axis=1)
    return int(np.argmin(step)) - reach, float(step.min())


def build_takes(path, repeats, stereo, rate, gains=None):
    """Turn slices into takes: [phase, where phase zero would be, gain per channel], cutting
    between samples that match. Each take lines up with the one before, whatever its lag has
    done since."""
    hop = int(SLICE_SECONDS * rate)
    reach = int(CUT_SEARCH_SECONDS * rate)
    gain = [(1.0, 1.0)] * len(repeats) if gains is None else gains
    takes = [[0, int(repeats[path[0]].position(np.array([0]))[0]), gain[path[0]]]]
    jumps = []
    for t in np.flatnonzero(np.diff(path)) + 1:
        phase = np.array([int(t * hop)])
        expected = repeats[path[t - 1]].position(phase)[0] - phase[0]
        slip = takes[-1][1] - expected
        incoming = int(repeats[path[t]].position(phase)[0]) + int(slip)
        offset, step = best_cut(stereo, takes[-1][1] + int(phase[0]), incoming, reach, takes[-1][2], gain[path[t]])
        jumps.append(step)
        takes.append([int(phase[0]) + offset, incoming - int(phase[0]), gain[path[t]]])
    return takes, jumps


def close_loop(takes, length, stereo, rate):
    """Where the last take should end so that it runs on into the first.

    Returns the true length of a pass, how far along to put the wrap so the samples either
    side of it match, and the step left there.
    """
    window = int(JOIN_WINDOW_SECONDS * rate)
    reach = int(CUT_SEARCH_SECONDS * rate)
    outgoing = takes[-1][1] + length
    incoming = takes[0][1]
    if outgoing + window + JOIN_SEARCH_SAMPLES >= len(stereo):
        return length, 0, 1.0  # the recording stops here, so there's nothing to line up with
    fixed = np.asarray(stereo[incoming:incoming + window]).mean(axis=1, dtype=np.float64)
    moving = np.asarray(stereo[outgoing - JOIN_SEARCH_SAMPLES:outgoing + window + JOIN_SEARCH_SAMPLES]).mean(axis=1, dtype=np.float64)
    product = correlate(moving, fixed)[:2 * JOIN_SEARCH_SAMPLES + 1]
    shift = int(np.argmax(product)) - JOIN_SEARCH_SAMPLES
    turn, step = best_cut(stereo, outgoing + shift, incoming, reach, takes[-1][2], takes[0][2])
    return length + shift, turn, step


def rotated(takes, turn):
    """The same takes, with phase zero moved along by some samples: each still reads the same
    place at the same moment, but the wrap falls elsewhere."""
    moved = [[max(take[0] - turn, 0), take[1] + turn, *take[2:]] for take in takes]
    moved[0][0] = 0
    return [take for index, take in enumerate(moved)
            if index + 1 == len(moved) or moved[index + 1][0] > take[0]]


def render(takes, length, source_array):
    """The clean pass through the loop, as a phase-indexed array."""
    out = np.zeros((length,) + source_array.shape[1:], dtype=np.float32)
    for index, take in enumerate(takes):
        phase, source = take[0], take[1]
        end = takes[index + 1][0] if index + 1 < len(takes) else length
        gain = np.asarray(take[2], dtype=np.float32) if len(take) > 2 else np.float32(1.0)
        if source_array.ndim == 1:
            gain = np.mean(gain)
        out[phase:end] = np.asarray(source_array[source + phase:source + end]) * gain
    return out


def keep_voices_whole(cost, voices, names, part):
    """A voice stays as it is, from this part's first pass that has one: no cuts in it, and no
    repair that would thin it. Only the player's sounds over it are for taking off. Returns the
    stretches, in slices, and makes reading anything else there cost too much."""
    mine = [i for i, name in enumerate(names) if name == part and voices[i].any()]
    if not mine:
        return []
    keep = mine[0]
    margin = int(VOICE_MARGIN_SECONDS / SLICE_SECONDS)
    # The first time through plays the recording, voice and all (its keep ranges). The render
    # is for holds, where a voice mustn't come round again: where some pass has none, the path
    # reads that. Only where every pass has a voice is one kept whole rather than cut up.
    usable = cost < COST_INVALID
    clear = (~voices & usable).any(axis=0)
    stretches = []
    for start, stop in runs_of(voices[keep]):
        low, high = max(start - margin, 0), min(stop + margin, cost.shape[1])
        if clear[start:stop].all():
            continue
        if stretches and low <= stretches[-1][1]:
            stretches[-1] = (stretches[-1][0], high)
        else:
            stretches.append((low, high))
    others = np.arange(len(names)) != keep
    for low, high in stretches:
        cost[others, low:high] += COST_INVALID
    return stretches


def clean_loop(stereo, mid, starts, period, rate, log=print, bitrate_at=None, is_own=None, names_at=None, part=None,
               keep_voices=True):
    length = int(round(period * rate))
    usable = [s for s in starts if s >= 0 and s + length <= len(mid)][:MAX_REPEATS]
    if len(usable) < 2:
        return None
    usable = order_by_lock(mid, usable, length, rate)
    seam = find_seam(mid, usable, length, rate)
    if seam is not None:
        # Phase zero goes on the restart, so a pass holds together and the wrap is natural.
        log(f"  track restarts at phase {seam / rate:.2f}")
        rough = align_repeats(mid, usable, length, rate)
        moved = [int(r.position(np.array([seam]))[0]) - length for r in rough if r is not None]
        usable = [s for s in moved if s >= 0 and s + length <= len(mid)]
        if len(usable) < 2:
            return None
    repeats = [r for r in align_repeats(mid, usable, length, rate) if r is not None]
    if len(repeats) < 2:
        return None
    phase = np.arange(length)
    takes_stereo = np.stack([np.asarray(stereo[r.position(phase)], dtype=np.float32) for r in repeats])
    own = [is_own(r.start) if is_own else True for r in repeats]
    offsets = level_match(takes_stereo, own, rate)
    ducked = [r for r, row in zip(repeats, offsets) if np.abs(row).mean() > DUCKED_DB]
    keep = [i for i, r in enumerate(repeats) if r not in ducked]
    if len(keep) < 2:
        return None
    repeats = [repeats[i] for i in keep]
    offsets = offsets[keep]
    takes_stereo = takes_stereo[keep]
    # To the level of this part's own passes, so that cuts don't show and nor do voices it keeps.
    level = [tuple(1.0 if abs(value) < LEVEL_SNAP_DB else float(10 ** (-value / 20)) for value in row) for row in offsets]
    aligned = np.stack([(take * np.asarray(gain, dtype=np.float32)).mean(axis=1) for take, gain in zip(takes_stereo, level)])
    names = [names_at(r.start) if names_at else None for r in repeats]
    tone = match_tone(takes_stereo * np.asarray(level, dtype=np.float32)[:, None, :], names, part, rate) if names_at else {}
    del takes_stereo
    cost, dirty, voices = find_dirt(aligned, rate)
    as_is = keep_voices_whole(cost, voices, names, part) if keep_voices else []
    # A lower bitrate has lost its top end, which would come and go with each cut.
    rates = [bitrate_at(r.start) if bitrate_at else 0 for r in repeats]
    rank = [sorted(set(rates), reverse=True).index(value) for value in rates]
    path = choose_path(cost, dirty, rank, names if names_at else None)
    takes, jumps = build_takes(path, repeats, stereo, rate, level)
    closed, turn, join = close_loop(takes, length, stereo, rate)
    chosen = dirty[path, np.arange(dirty.shape[1])]
    result = {
        "length": length, "closed": closed, "turn": turn, "join": join, "repeats": repeats, "takes": takes,
        "ducked": ducked, "level": level,
        "jumps": jumps, "aligned": aligned, "voices": voices, "path": path, "chosen": chosen,
        "dirty_after": float(chosen.mean()), "rate": rate, "tone": tone,
        "as_is": [(low * int(SLICE_SECONDS * rate), high * int(SLICE_SECONDS * rate)) for low, high in as_is],
    }
    if tone:
        log(f"  tone of other recordings, dB by band: {tone}")
    log(f"  repeats {[round(r.start / rate, 2) for r in repeats]}, pass length {closed - length:+d} samples, "
        f"left out as ducked {[round(r.start / rate, 2) for r in ducked]}")
    log(f"  gains dB {[[round(float(-v), 1) for v in row] for row in offsets]}")
    log(f"  unagreed {[round(float(v) * 100, 1) for v in dirty.mean(axis=1)]}% "
        f"(voice {[round(float(v) * 100, 1) for v in voices.mean(axis=1)]}), "
        f"read from {[round(float((path == i).mean()) * 100, 1) for i in range(len(repeats))]}%")
    log(f"  {len(takes)} takes, unagreed after {chosen.mean() * 100:.2f}%, nothing agrees at all "
        f"{dirty.all(axis=0).mean() * 100:.2f}%, worst step at a cut "
        f"{20 * np.log10(max(max(jumps, default=1e-6), 1e-6)):.1f} dB, at the join {20 * np.log10(max(join, 1e-6)):.1f} dB")
    return result


def clean_suite(sources, entries, period, log=print):
    """A track the broadcast played as sections in a varying order: one cycle of them, from
    passes that each hold only some of it. entries: (position of phase zero, valid from,
    valid to), positions across all recordings, phases in samples. The first is the ruler,
    so it has to share some of every other's stretch."""
    rate, stereo, mid = sources.rate, sources.stereo, sources.mid
    length = int(round(period * rate))
    phase = np.arange(length)
    ruler = entries[0]
    first = np.asarray(mid[ruler[0]:ruler[0] + length], dtype=np.float64)
    first[:ruler[1]] = 0
    first[ruler[2]:] = 0
    repeats, valid = [], []
    for index, (start, low, high) in enumerate(entries):
        if index == 0:
            repeat = Repeat(start, [0.0], [0.0])
        else:
            at, lags = measure_lags(mid, first, start, length, rate)
            keep = (at >= max(low, ruler[1])) & (at <= min(high, ruler[2]))
            at, lags = at[keep], lags[keep]
            repeat = Repeat(start, list(at), list(lags)) if len(lags) >= 2 else Repeat(start, [0.0], [0.0])
            log(f"  {sources.locate(start + low)[0]} {(sources.locate(start + low)[1] - low) / rate:.4f}: "
                f"lag {np.median(lags) if len(lags) else 0:+.0f} samples over {len(lags)} windows")
        repeats.append(repeat)
        valid.append((phase >= low) & (phase < high))
    valid = np.stack(valid)
    takes_stereo = np.stack([np.asarray(stereo[np.clip(r.position(phase), 0, len(stereo) - 1)], dtype=np.float32) for r in repeats])
    names = [sources.locate(start + low)[0] for start, low, _ in entries]
    own = [name == sources.part for name in names]
    offsets = level_match(takes_stereo, own, rate, valid)
    level = [tuple(1.0 if abs(value) < LEVEL_SNAP_DB else float(10 ** (-value / 20)) for value in row) for row in offsets]
    aligned = np.stack([(take * np.asarray(gain, dtype=np.float32)).mean(axis=1) for take, gain in zip(takes_stereo, level)])
    aligned[~valid] = 0
    tone = match_tone(takes_stereo * np.asarray(level, dtype=np.float32)[:, None, :], names, sources.part, rate, valid)
    log(f"  tone of other recordings, dB by band: {tone}")
    del takes_stereo
    cost, dirty, voices = find_dirt(aligned, rate, valid)
    as_is = keep_voices_whole(cost, voices, names, sources.part)
    rates = [sources.bitrates[name] for name in names]
    rank = [sorted(set(rates), reverse=True).index(value) for value in rates]
    path = choose_path(cost, dirty, rank, names)
    takes, jumps = build_takes(path, repeats, stereo, rate, level)
    closed, turn, join = close_loop(takes, length, stereo, rate)
    chosen = dirty[path, np.arange(dirty.shape[1])]
    hop = int(SLICE_SECONDS * rate)
    covered = valid[:, :dirty.shape[1] * hop].reshape(len(entries), -1, hop).all(axis=2)
    log(f"  gains dB {[[round(float(-v), 1) for v in row] for row in offsets]}")
    along = np.array([names[i] for i in path])
    log(f"  read from {[round(float((path == i).mean()) * 100, 1) for i in range(len(entries))]}%, "
        f"{len(takes)} takes, {int((along[1:] != along[:-1]).sum())} into another recording")
    log(f"  unagreed after {chosen.mean() * 100:.2f}%, only one pass there {(covered.sum(axis=0) < 2).mean() * 100:.1f}%, "
        f"worst step at a cut {20 * np.log10(max(max(jumps, default=1e-6), 1e-6)):.1f} dB, at the join {20 * np.log10(max(join, 1e-6)):.1f} dB")
    return {"length": length, "closed": closed, "turn": turn, "join": join, "repeats": repeats, "takes": takes,
            "ducked": [], "level": level, "jumps": jumps, "aligned": aligned, "voices": voices, "path": path,
            "chosen": chosen, "dirty_after": float(chosen.mean()), "rate": rate, "valid": valid, "names": names,
            "tone": tone, "as_is": [(low * hop, high * hop) for low, high in as_is]}


def parse_suite(text, sources):
    """"54.5 kou 192.0 0 107.5 zen 132.8664 54.8 118.4876 ...": how far into the cycle this
    part's loop starts, then for each pass its part, where the cycle's phase zero is in that
    part's recording, and the phases where it's this track, all in seconds. Returns the turn
    in samples and the passes."""
    tokens = text.split()
    turn = int(round(float(tokens[0]) * sources.rate))
    tokens = tokens[1:]
    entries = []
    for at in range(0, len(tokens), 4):
        name, zero, low, high = tokens[at], float(tokens[at + 1]), float(tokens[at + 2]), float(tokens[at + 3])
        entries.append((sources.offsets[name] + int(round(zero * sources.rate)),
                        int(round(low * sources.rate)), int(round(high * sources.rate))))
    return turn, entries


def turned(takes, turn, length):
    """The same takes with phase zero moved any distance round the cycle."""
    turn %= length
    out = []
    for index, take in enumerate(takes):
        start = take[0]
        stop = takes[index + 1][0] if index + 1 < len(takes) else length
        for low, high, wrap in ((start, min(stop, turn), length), (max(start, turn), stop, 0)):
            if high > low:
                out.append([low - turn + wrap, take[1] + turn - wrap, *take[2:]])
    out.sort(key=lambda take: take[0])
    return out


def save_wav(path, stereo, rate):
    pcm = np.clip(np.asarray(stereo) * 32767, -32768, 32767).astype(np.int16)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(pcm.tobytes())


def read_cue(part):
    """The cue file's lines, and for each loop (anchor, end, measured period, repeats, line
    number, passes in other parts). The loop line's own period is the one worked out here,
    so it isn't an input."""
    path = ROOT / "data" / "cues" / f"{part}.toml"
    lines = path.read_text().splitlines(True)
    loops = []
    for number, line in enumerate(lines):
        match = re.match(r"loop = (\S+) (\S+) (\S+)", line)
        if match:
            loops.append([float(match[1]), float(match[2]), float(match[3]), [], number, None, None, False])
        match = re.match(r"repeats = (.*)", line)
        if match and loops:
            values = [float(v) for v in match[1].split()]
            loops[-1][2], loops[-1][3] = values[0], values[1:]
        match = re.match(r"also = ?(.*)", line)
        if match and loops:
            loops[-1][5] = match[1]
        match = re.match(r"suite = (.*)", line)
        if match and loops:
            loops[-1][6] = match[1]
        if line.startswith("voices = keep") and loops:
            loops[-1][7] = True  # the default now: kept so older cue files still read
    return path, lines, loops


def scene_start_media(part, cue_text, end):
    """In recording seconds, the start of the scene that a loop ending here belongs to."""
    rate = float(re.search(r"rate = ([\d.]+)", cue_text)[1])
    offset = float(re.search(r"offset = (-?[\d.]+)", cue_text)[1])
    starts = [start for _, start in sc.schedule(load_pack(part)) if start is not None]
    media = [(start - offset) / rate for start in starts]
    # A loop can run on a few seconds into the next scene, as the music does.
    return max((m for m in media if m <= end - SCENE_OVERRUN_SECONDS), default=0.0)


def cue_lines(result, scene_start, end, period, starts, also, sources):
    """The numbers the player needs for one loop, all on the sample grid."""
    rate = sources.rate

    def seconds(samples):
        return f"{samples / rate:.6f}"

    closed = result["closed"]
    repeats = result["repeats"]
    turn = result["turn"]
    ducked = result["ducked"]
    valid = result.get("valid")
    if valid is not None:
        # A suite's phase zero is where its loop starts in this part, as the cue file says.
        # Where the cycle is at the loop's start, in this part's own pass that has it then.
        # Not the old loop line, which already has the last build's turn in it.
        shift = result["shift"]
        own = next(i for i, name in enumerate(result["names"]) if name == sources.part and valid[i][shift % len(valid[i])])
        first = sources.locate(int(repeats[own].position(np.array([shift]))[0]))[1] + turn
        turn += shift
        lines = [f"loop = {seconds(first)} {end:.4f} {seconds(closed)}\n"]
    else:
        own = sorted(sources.locate(r.start)[1] for r in repeats + ducked if sources.locate(r.start)[0] == sources.part)
        # Phase zero is a pass start: the one running as this scene starts, or the first after.
        # Before the scene starts, the recording plays.
        first = max((place for place in own if place / rate <= scene_start + 1.0), default=own[0]) + turn
        lines = [f"loop = {seconds(first)} {end:.4f} {seconds(closed)}\n",
                 f"repeats = {period:.4f} " + " ".join(f"{t:.4f}" for t in starts) + "\n",
                 f"also = {also}\n"]
    # Every pass of the track, lined up with the phase, for the player's own final render.
    frames = np.arange(closed + PASS_MARGIN_SAMPLES)
    for index, (repeat, gain) in enumerate(zip(repeats, result["level"])):
        offset = repeat.position(frames + turn) - frames
        if valid is not None:
            # Where a pass isn't this track, "nan": the render leaves it out there.
            row = valid[index]
            cycle = (frames + turn) % closed
            offset = repeat.position(cycle) - frames
            usable = row[np.clip(cycle, 0, len(row) - 1)] & (cycle < len(row))
            name = result["names"][index]
            marked = np.where(usable, offset, -1)
            changes = np.concatenate([[0], np.flatnonzero(np.diff(marked)) + 1])
            low = sources.offsets[name]
            pairs = " ".join(f"{seconds(int(at))} {seconds(int(offset[at]) - low) if usable[at] else 'nan'}" for at in changes)
        else:
            changes = np.concatenate([[0], np.flatnonzero(np.diff(offset)) + 1])
            name, _ = sources.locate(int(offset[0]))
            low = sources.offsets[name]
            pairs = " ".join(f"{seconds(int(at))} {seconds(int(offset[at]) - low)}" for at in changes)
        lines.append(f"pass = {name} {gain[0]:.4f} {gain[1]:.4f} {pairs}\n")
    for name, gains in sorted(result.get("tone", {}).items()):
        lines.append(f"tone = {name} " + " ".join(f"{g:.1f}" for g in gains) + "\n")
    for low, high in result.get("as_is", []):
        lines.append(f"asis = {seconds(max(low - turn, 0))} {seconds(max(high - turn, 0))}\n")
    takes = turned(result["takes"], turn, closed) if valid is not None else rotated(result["takes"], turn)
    for index, (phase, source, gain) in enumerate(takes):
        stop = takes[index + 1][0] if index + 1 < len(takes) else closed
        name, place = sources.locate(source + phase)
        place -= phase
        tag = "." if name == sources.part else name
        tail = "" if tag == "." and gain == (1.0, 1.0) else f" {tag}" if gain == (1.0, 1.0) else f" {tag} {gain[0]:.4f} {gain[1]:.4f}"
        lines.append(f"take = {seconds(phase)} {seconds(stop - phase)} {seconds(place)}{tail}\n")
    if valid is None and first / rate < scene_start - 1.0:
        lines.append(f"keep = {seconds(first)} {scene_start:.4f}\n")
    # A voice or a cue over the music plays as recorded. Holding skips these.
    for repeat in ducked:
        name, place = sources.locate(repeat.start)
        if name == sources.part and (place + closed) / rate > first / rate and place / rate < end:
            lines.append(f"keep = {seconds(place)} {seconds(place + closed)}\n")
    hop = int(SLICE_SECONDS * rate)
    for repeat, row in zip(repeats, result["voices"]):
        if sources.locate(repeat.start)[0] != sources.part:
            continue
        for start, stop in runs_of(row):
            media_from = sources.locate(int(repeat.position(np.array([start * hop]))[0]))[1] / rate
            media_to = sources.locate(int(repeat.position(np.array([stop * hop]))[0]))[1] / rate
            if media_to > first / rate and media_from < end:
                lines.append(f"keep = {media_from:.4f} {media_to:.4f}\n")
    return lines


def parse_also(text, sources):
    """More passes, from the cue file: "chu 1 2 3 zen 4 5"."""
    positions, name = [], None
    for token in text.split():
        if token in PARTS:
            name = token
        elif name in sources.offsets:
            positions.append(sources.offsets[name] + int(round(float(token) * sources.rate)))
    return positions


def format_also(positions, sources):
    by_part = {}
    for position in positions:
        name, place = sources.locate(position)
        by_part.setdefault(name, []).append(f"{place / sources.rate:.3f}")
    return " ".join(f"{name} {' '.join(places)}" for name, places in by_part.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("part", choices=PARTS)
    parser.add_argument("--preview", type=pathlib.Path)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--only", type=int, help="just this loop, by number in the cue file")
    parser.add_argument("--search", action="store_true", help="look for passes again, even if the cue file has some")
    args = parser.parse_args()
    sources = Sources(args.part)
    rate = sources.rate
    path, lines, loops = read_cue(args.part)
    text = "".join(lines)
    replaced = {}
    for number, (anchor, end, period, starts, line, also, suite, keep_voices) in enumerate(loops):
        if args.only is not None and number != args.only:
            continue
        if suite:
            shift, entries = parse_suite(suite, sources)
            print(f"loop {number}: a suite, period {period:.4f}, {len(entries)} passes", flush=True)
            result = clean_suite(sources, entries, period)
            result["shift"] = shift
            replaced[line] = cue_lines(result, anchor, end, period, [anchor], "", sources)
            if args.preview:
                args.preview.mkdir(parents=True, exist_ok=True)
                save_wav(args.preview / f"{args.part}_{number:02d}_after.wav",
                         render(turned(result["takes"], result["turn"] + shift, result["closed"]), result["closed"], sources.stereo), rate)
            continue
        own_low = sources.offsets[args.part]
        known = [own_low + int(round(s * rate)) for s in starts]
        more = parse_also(also, sources) if also is not None and not args.search else sources.find_everywhere(known, period)
        also_text = format_also(more, sources)
        print(f"loop {number}: period {period:.3f}, {len(starts)} repeats here, also: {also_text or 'none'}", flush=True)
        # The better recordings first, as there's a limit on how many get compared.
        ranked = sorted(known + more, key=lambda position: (-sources.bitrate_at(position), position))[:MAX_REPEATS]
        if not any(sources.locate(position)[0] == args.part for position in ranked):
            ranked[-1] = known[0]
        result = clean_loop(sources.stereo, sources.mid, sorted(ranked), period, rate, bitrate_at=sources.bitrate_at,
                            is_own=lambda position: sources.locate(position)[0] == args.part,
                            names_at=lambda position: sources.locate(position)[0], part=args.part)
        if result is None:
            print("  too few usable repeats")
            continue
        replaced[line] = cue_lines(result, scene_start_media(args.part, text, end), end, period, starts, also_text, sources)
        if args.preview:
            args.preview.mkdir(parents=True, exist_ok=True)
            own = next(r for r in result["repeats"] if sources.locate(r.start)[0] == args.part)
            stem = args.preview / f"{args.part}_{number:02d}"
            save_wav(f"{stem}_before.wav", sources.stereo[own.start:own.start + result["length"]], rate)
            save_wav(f"{stem}_after.wav", render(rotated(result["takes"], result["turn"]), result["closed"], sources.stereo), rate)
    if args.write:
        out, skipping = [], False
        for index, line_text in enumerate(lines):
            if index in replaced:
                out.extend(replaced[index])
                skipping = True
            elif line_text.startswith("loop = "):
                skipping = False
                out.append(line_text)
            elif not (skipping and line_text.startswith(("repeats = ", "also = ", "pass = ", "take = ", "keep = ", "tone = ", "asis = "))
                      and not line_text.rstrip().endswith(BY_HAND)):
                out.append(line_text)
        path.write_text("".join(out))
        print(f"wrote {path}")


if __name__ == "__main__":
    sys.exit(main())
