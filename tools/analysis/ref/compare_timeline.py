#!/usr/bin/env python3
"""Compare a native run's clock-named frames against a reference run's frames.

usage: tools/analysis/ref/compare_timeline.py <reference dir> <native dir> [tolerance seconds]

Reference frames are f<t>.png, with log.txt mapping some t to a clock second. Native
frames are c<mm>_<ss>_<ff>.ppm (PVS_TRACE_SHOTS). Pairs frames by clock and prints
how many pixels differ, worst last. Frames are local-only: they show the game.
"""
import pathlib
import re
import sys

from PIL import Image, ImageChops

FRAMES_PER_SECOND = 60
TOLERANCE_SECONDS = 2


def reference_clock(log):
    clocks = {}
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"t\+(\d+) .* clock (\d+):(\d+)", line)
        if m:
            clocks[int(m[1])] = int(m[2]) * 60 + int(m[3])
    return clocks


def differing_pixels(a, b):
    diff = ImageChops.difference(a, b).convert("L")
    return sum(1 for value in diff.tobytes() if value)


def main():
    ref_dir, native_dir = map(pathlib.Path, sys.argv[1:3])
    tolerance = int(sys.argv[3]) if len(sys.argv) > 3 else TOLERANCE_SECONDS
    clocks = reference_clock(ref_dir / "log.txt")
    native = {}
    for path in native_dir.glob("c*.ppm"):
        m = re.match(r"c(\d+)_(\d+)_(\d+)", path.stem)
        native[int(m[1]) * 60 + int(m[2])] = path
    rows = []
    for t, second in sorted(clocks.items()):
        png = ref_dir / f"f{t:07d}.png"
        if not png.exists():
            continue
        reference = Image.open(png).convert("RGB")
        best = None
        for candidate in range(second - tolerance, second + tolerance + 1):
            path = native.get(candidate)
            if path:
                image = Image.open(path).convert("RGB").resize(reference.size)
                count = differing_pixels(reference, image)
                if best is None or count < best[0]:
                    best = (count, path.name)
        rows.append((second, t, best))
    for second, t, best in rows:
        when = f"{second // 60:02d}:{second % 60:02d}"
        print(f"{when} t+{t:<7} " + (f"{best[0]:6d} differ vs {best[1]}" if best else "no native frame"))


if __name__ == "__main__":
    main()
