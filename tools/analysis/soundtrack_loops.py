#!/usr/bin/env python3
"""Find where a soundtrack album's track loops, for data/soundtrack/tracks.toml.

Game rips play a track's loop twice and then fade. The loop's length has to be exact to the
sample. Its start only has to be somewhere both times through cover, so a decoder that starts
a little earlier or later than ffmpeg still loops cleanly.

Some rips were recorded from the console and don't repeat sample for sample, though they sound
the same each time. Those get a short fade from the loop's end into what leads to its start.

    soundtrack_loops.py <file.mp3> ...

Prints, per file, the numbers for a track line in data/soundtrack/tracks.toml:
length in seconds, rate, loop start in seconds, loop length in samples, fade in seconds.
"""
import subprocess
import sys

import numpy as np

CHROMA_HOP = 0.1
SHORTEST_LOOP = 20.0
CHECK_SECONDS = 4.0
MARGIN_SECONDS = 1.0
SAME = 0.97
ALIKE = 0.9          # spectra this alike, for a rip that doesn't repeat exactly
FADE_SECONDS = 0.5


def decode(path):
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=sample_rate",
                            "-of", "csv=p=0", path], capture_output=True, text=True, check=True)
    rate = int(probe.stdout.strip())
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-ac", "1", "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64), rate


def envelope(x, rate):
    """Short-time spectra, coarse, to find the loop length roughly."""
    hop, size = int(CHROMA_HOP * rate), 4096
    frames = (len(x) - size) // hop
    index = np.arange(size)[None, :] + hop * np.arange(frames)[:, None]
    spectra = np.log1p(np.abs(np.fft.rfft(x[index] * np.hanning(size), axis=1))[:, 8:600])
    spectra -= spectra.mean(axis=1, keepdims=True)
    return spectra / (np.linalg.norm(spectra, axis=1, keepdims=True) + 1e-9)


def rough_length(spectra):
    best, best_lag = -1.0, 0
    for lag in range(int(SHORTEST_LOOP / CHROMA_HOP), len(spectra) - int(10 / CHROMA_HOP)):
        same = (spectra[:-lag] * spectra[lag:]).sum(axis=1)
        # The best stretch of 10 s at this lag.
        run = np.convolve(same, np.ones(100) / 100, "valid")
        if run.size and run.max() > best:
            best, best_lag = run.max(), lag
    return best_lag * CHROMA_HOP, best


def similarity(x, a, b, n):
    u, v = x[a:a + n], x[b:b + n]
    return float((u * v).sum() / (np.linalg.norm(u) * np.linalg.norm(v) + 1e-12))


def exact_length(x, rate, rough):
    """Refine on the waveform, a chunk well into the track against the same chunk a loop later."""
    n = int(CHECK_SECONDS * rate)
    at = int(min(len(x) - rough * rate - n - rate, max(rate * 5.0, (len(x) - rough * rate) / 2)))
    lag0, reach = int(rough * rate), int(0.3 * rate)
    search = x[at + lag0 - reach:at + lag0 + reach + n]
    corr = np.correlate(search, window, "valid")
    energy = np.sqrt(np.convolve(search ** 2, np.ones(n), "valid"))
    corr = corr / (energy * np.linalg.norm(window) + 1e-12)
    best = int(np.argmax(corr))
    return lag0 - reach + best, float(corr[best])


def loop_start(x, rate, length):
    """The first place that plays the same a loop later, plus a margin."""
    n, step = int(CHECK_SECONDS * rate), int(0.25 * rate)
    for at in range(0, len(x) - length - n, step):
        if similarity(x, at, at + length, n) > SAME:
            return at / rate + MARGIN_SECONDS
    return None


def lags(spectra):
    """Every loop length worth trying, each with how alike its spectra are along the track."""
    found = []
    for lag in range(int(SHORTEST_LOOP / CHROMA_HOP), len(spectra) - int(10 / CHROMA_HOP)):
        same = (spectra[:-lag] * spectra[lag:]).sum(axis=1)
        run = np.convolve(same, np.ones(40) / 40, "valid")
        found.append((lag, run))
    return found


def faded_loop(x, rate, spectra):
    """The longest loop whose spectra repeat, from as early as they do, with the length fitted to
    the waveform where the fade will be."""
    hop = int(CHROMA_HOP * rate)
    best = None
    for lag, run in lags(spectra):
        alike = np.flatnonzero(run > ALIKE)
        if not alike.size:
            continue
        start = alike[0] * CHROMA_HOP + MARGIN_SECONDS + FADE_SECONDS
        if start * rate + lag * hop + MARGIN_SECONDS * rate >= len(x) or run.max() < ALIKE + 0.04:
            continue
        if best is None or lag > best[0] + 5 or (abs(lag - best[0]) <= 5 and run.max() > best[2]):
            best = (lag, start, run.max())
    if best is None:
        return None
    lag, start, _ = best
    n, reach = int(FADE_SECONDS * rate), int(0.03 * rate)
    at = int((start - FADE_SECONDS) * rate)
    tries = [(similarity(x, at, at + lag * hop + d, n), lag * hop + d) for d in range(-reach, reach)]
    match, length = max(tries)
    return start, length, match


def main():
    for path in sys.argv[1:]:
        x, rate = decode(path)
        spectra = envelope(x, rate)
        rough, score = rough_length(spectra)
        length, match = exact_length(x, rate, rough)
        start = loop_start(x, rate, length) if match > SAME else None
        fade = 0.0
        if start is None or start + length / rate + MARGIN_SECONDS >= len(x) / rate:
            found = faded_loop(x, rate, spectra)
            if found is None:
                print(f"{path}: no loop found")
                continue
            start, length, match = found
            fade = FADE_SECONDS
        print(f"{path}: {len(x) / rate:.2f} {rate} {start:.3f} {length} {fade}  "
              f"(loop {length / rate:.3f} s, waveform match {match:.3f})")


if __name__ == "__main__":
    main()
