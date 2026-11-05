#!/usr/bin/env python3
"""Map the game's clock to a recording by comparing moving pictures (development tool).

usage: tools/analysis/align_video.py <recording.divx> <strip.bin> --box x,y,w,h --guess a,b

Takes a native run's strip (PVS_TRACE_STRIP): a tiny picture of each frame drawn, with its
clock. Over a window of a minute or so at a time, it slides the strip along the recording
to find where their changes line up: a cut, a fade, or a line of text typing out. Comparing
changes rather than pictures means static screens don't match anything falsely, and the
original player's own actions just lower the score. So cutscenes pin down the map, and this
never reads the on-screen timer, which can't be trusted early in a recording.

It prints clock = a * media + b for each window, with how sure it is. Only numbers get
committed.
"""
import argparse
import pathlib
import subprocess

import numpy as np

STRIP_SIZE = (32, 28)
FRAMES_PER_SECOND = 60.0988
SAMPLE_FPS = FRAMES_PER_SECOND / 16  # turbo draws one frame in 16
MINUTE_FRAMES = 3606
WINDOW_SECONDS = 45.0
STEP_SECONDS = 15.0
SEARCH_SECONDS = 25.0


def read_strip(path):
    record = 4 + STRIP_SIZE[0] * STRIP_SIZE[1]
    raw = np.fromfile(path, dtype=np.uint8)
    raw = raw[:len(raw) // record * record].reshape(-1, record)
    stamps = raw[:, :4].copy().view(np.uint32).reshape(-1).astype(np.float64)
    pictures = raw[:, 4:].astype(np.float32)
    # Keep the run after the clock has started, in order.
    keep = stamps < 59 * MINUTE_FRAMES
    return stamps[keep] / FRAMES_PER_SECOND, pictures[keep]


def read_recording(path, box, cache):
    if cache.exists():
        return np.load(cache)
    x, y, w, h = box
    cmd = ["ffmpeg", "-v", "quiet", "-i", path, "-vf",
           f"fps={SAMPLE_FPS:.6f},crop={w}:{h}:{x}:{y},scale={STRIP_SIZE[0]}:{STRIP_SIZE[1]}:flags=area,format=gray",
           "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
    frames = np.frombuffer(raw, dtype=np.uint8).reshape(-1, STRIP_SIZE[0] * STRIP_SIZE[1]).astype(np.float32)
    np.save(cache, frames)
    return frames


def align(times, native, video, a, b):
    """For each window of the strip, the offset that best lines up its changes."""
    n_change = np.diff(native, axis=0)
    v_change = np.diff(video, axis=0)
    results = []
    per_window = int(WINDOW_SECONDS * SAMPLE_FPS)
    per_search = int(SEARCH_SECONDS * SAMPLE_FPS)
    for start in np.arange(0, times[-1] - WINDOW_SECONDS, STEP_SECONDS):
        i0 = int(np.searchsorted(times, start))
        if i0 + per_window >= len(n_change):
            break
        change = n_change[i0:i0 + per_window]
        if np.abs(change).mean() < 0.15:
            continue  # nothing happening on screen in this window
        # Where each sample falls in the recording, at the guessed map.
        centre = ((times[i0:i0 + per_window] - b) / a * SAMPLE_FPS).astype(np.int64)
        scores = []
        for shift in range(-per_search, per_search + 1):
            where = centre + shift
            if where[0] < 0 or where[-1] >= len(v_change):
                scores.append(-1.0)
                continue
            other = v_change[where]
            scores.append(float((change * other).sum() / (np.sqrt((change ** 2).sum() * (other ** 2).sum()) + 1e-6)))
        scores = np.array(scores)
        best = int(np.argmax(scores))
        guard = int(1.5 * SAMPLE_FPS)
        others = np.concatenate([scores[:max(0, best - guard)], scores[best + guard + 1:]])
        lead = scores[best] - others.max() if len(others) else 0.0
        # A curve through the top three scores places the peak between samples.
        fine = float(best)
        if 0 < best < len(scores) - 1:
            lo, mid, hi = scores[best - 1:best + 2]
            bend = lo - 2 * mid + hi
            if bend < 0:
                fine += 0.5 * (lo - hi) / bend
        shift_seconds = (fine - per_search) / SAMPLE_FPS
        middle = float(times[i0 + per_window // 2])
        media = (middle - b) / a + shift_seconds
        results.append((middle, media, float(scores[best]), float(lead)))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("recording")
    ap.add_argument("strip")
    ap.add_argument("--box", required=True)
    ap.add_argument("--guess", default="0.9975,0.0")
    ap.add_argument("--cache", default="local/rec")
    args = ap.parse_args()
    box = tuple(int(v) for v in args.box.split(","))
    a, b = (float(v) for v in args.guess.split(","))
    cache = pathlib.Path(args.cache) / (pathlib.Path(args.recording).name[:7] + "_strip.npy")
    video = read_recording(args.recording, box, cache)
    times, native = read_strip(args.strip)
    print(f"# {len(video)} recording samples, {len(times)} strip samples, to clock {times[-1]:.0f}")
    print("# clock  media  score  lead")
    for clock, media, score, lead in align(times, native, video, a, b):
        print(f"{clock:8.2f} {media:8.2f} {score:.3f} {lead:.3f}")


if __name__ == "__main__":
    main()
