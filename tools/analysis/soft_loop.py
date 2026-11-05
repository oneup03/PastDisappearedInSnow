#!/usr/bin/env python3
"""A loop for a track that never truly repeats, so that a hold still has music.

Some tracks are through-composed, or have a layer that doesn't come round again. They can't be
cleaned or looped the way match_effects.py does it, as there's no second pass to compare. They
do keep a beat, so the end can cross into what leads up to the start: this finds where the
audio before the two moments matches best, so that the beat carries through the fade.

Only holds use it. The first time through plays as recorded, so nothing here changes the
ordinary run of the broadcast.

    soft_loop.py chu 1432:1438 1632:1638 --fade 2 --until 1638 --write
"""
import argparse
import pathlib
import re
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import match_effects as me  # noqa: E402

RATE = me.RATE // me.LOW_FACTOR
BAND = (60.0, 1200.0)      # above this, some tracks have a noise voice that never repeats
WINDOW_SECONDS = 2.0
LATER_STEP_SECONDS = 0.05


def band_passed(signal):
    spectrum = np.fft.rfft(signal - signal.mean())
    freq = np.fft.rfftfreq(len(signal), 1 / RATE)
    spectrum[(freq < BAND[0]) | (freq > BAND[1])] = 0
    return np.fft.irfft(spectrum, len(signal))


def best_wrap(low, base, earlier, later, window_seconds):
    """Which earlier and later moments have the most alike audio leading up to them."""
    window = int(window_seconds * RATE)
    first = int(earlier[0] * RATE) - window
    last = int(earlier[1] * RATE)
    stretch = band_passed(np.asarray(low[base + first:base + last], dtype=np.float64))
    later_from = int(later[0] * RATE) - window
    later_to = int(later[1] * RATE)
    moving = band_passed(np.asarray(low[base + later_from:base + later_to], dtype=np.float64))
    size = 1 << int(np.ceil(np.log2(len(stretch) + window)))
    spectrum = np.fft.rfft(stretch, size)
    squares = np.concatenate([[0.0], np.cumsum(stretch ** 2)])
    energy = squares[window:] - squares[:-window]
    best = (-1.0, 0, 0)
    for at in range(0, len(moving) - window, int(LATER_STEP_SECONDS * RATE)):
        anchor = moving[at:at + window]
        power = (anchor ** 2).sum()
        product = np.fft.irfft(spectrum * np.conj(np.fft.rfft(anchor, size)), size)[:len(energy)]
        score = product / np.sqrt(np.maximum(energy, 1e-9) * max(power, 1e-9))
        place = int(np.argmax(score))
        if score[place] > best[0]:
            best = (float(score[place]), first + place + window, later_from + at + window)
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("part", choices=me.PARTS)
    parser.add_argument("earlier", help="where the loop can start, as from:to in recording seconds")
    parser.add_argument("later", help="where it can go back from, as from:to")
    parser.add_argument("--fade", type=float, default=2.0, help="seconds to cross over")
    parser.add_argument("--until", type=float, help="the loop covers holds up to here: past the wrap, if need be")
    parser.add_argument("--from", dest="since", type=float, help="and holds from here: before the start, if need be")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    sources = me.Sources(args.part)
    base = sources.offsets[args.part] // me.LOW_FACTOR
    earlier = [float(v) for v in args.earlier.split(":")]
    later = [float(v) for v in args.later.split(":")]
    score, first, last = best_wrap(sources.low, base, earlier, later, max(WINDOW_SECONDS, args.fade))
    start = first * me.LOW_FACTOR / me.RATE
    period = (last - first) * me.LOW_FACTOR / me.RATE
    end = max(start + period, args.until or 0.0)
    # A phase is worked out from the loop's start, so going a period back keeps them all.
    origin = start
    while args.since is not None and origin > args.since:
        origin -= period
    since = start if args.since is None else min(args.since, start)
    print(f"{args.part}: {start:.3f} to {start + period:.3f}, period {period:.3f}, alike {score:.2f}")
    block = (f"loop = {origin:.6f} {end:.4f} {period:.6f}\n"
             f"fade = {args.fade:g}\n"
             f"take = 0.000000 {period:.6f} {start:.6f}\n"
             f"keep = {since:.4f} {end:.4f}\n")
    if not args.write:
        return
    path = me.ROOT / "data" / "cues" / f"{args.part}.toml"
    lines = path.read_text().splitlines(True)
    places = [(float(re.match(r"loop = (\S+)", line)[1]), number) for number, line in enumerate(lines) if line.startswith("loop = ")]
    after = [number for at, number in places if at > origin]
    where = after[0] if after else len(lines)
    lines[where:where] = block.splitlines(True)
    path.write_text("".join(lines))
    print(f"wrote {path}")


if __name__ == "__main__":
    sys.exit(main())
