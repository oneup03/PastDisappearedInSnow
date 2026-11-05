#!/usr/bin/env python3
"""Fit one straight line from the clock to a recording (development tool).

usage: tools/analysis/fit_map.py <align_video output> [--min-score 0.2]

The game's cutscenes run the same in every playthrough until a scene waits for a button.
A native run with no input waits there, and the original player didn't, so such windows
sit off the line until the next scene starts on the clock. The line is whatever the most
windows agree on: try lines through pairs of points, keep the one most of them lie near.
"""
import argparse
import itertools

import numpy as np

TOLERANCE = 0.25


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("alignment")
    ap.add_argument("--min-score", type=float, default=0.2)
    args = ap.parse_args()
    rows = [line.split() for line in open(args.alignment) if line.strip() and not line.startswith("#")]
    data = np.array([r for r in rows if len(r) == 4], dtype=float)
    data = data[data[:, 2] >= args.min_score]
    clock, media = data[:, 0], data[:, 1]
    best = (0, 1.0, 0.0)
    for i, j in itertools.combinations(range(len(clock)), 2):
        if clock[j] - clock[i] < 600:
            continue
        rate = (clock[j] - clock[i]) / (media[j] - media[i])
        if not 0.99 < rate < 1.01:
            continue
        offset = clock[i] - rate * media[i]
        inliers = int((np.abs(rate * media + offset - clock) < TOLERANCE).sum())
        if inliers > best[0]:
            best = (inliers, rate, offset)
    _, rate, offset = best
    for _ in range(4):
        keep = np.abs(rate * media + offset - clock) < TOLERANCE
        rate, offset = np.polyfit(media[keep], clock[keep], 1)
    residual = rate * media + offset - clock
    keep = np.abs(residual) < TOLERANCE
    print(f"{int(keep.sum())} of {len(clock)} windows on the line, rms {np.sqrt((residual[keep] ** 2).mean()) * 1000:.0f} ms")
    print(f"rate = {rate:.8f}")
    print(f"offset = {offset:.4f}")
    for lo in range(0, 3600, 600):
        band = (clock >= lo) & (clock < lo + 600) & keep
        if band.any():
            print(f"  {lo // 60:2d} min: {int(band.sum()):3d} windows, mean {residual[band].mean() * 1000:+5.0f} ms")


if __name__ == "__main__":
    main()
