# Shared by the cleaning tools: the recording at 32 kHz, short-time transforms, and writing
# an MP3 or a picture to listen to or look at. Run from the repository root.
import subprocess
import numpy as np

R = 32000


def stft(x, n, hop, window):
    pad = np.concatenate([np.zeros(n), x, np.zeros(n)])
    frames = (len(pad) - n) // hop + 1
    idx = np.arange(n)[None, :] + hop * np.arange(frames)[:, None]
    return np.fft.rfft(pad[idx] * window, axis=1)


def side_percentile(mag, side, q, past):
    out = np.empty_like(mag)
    src = np.concatenate([np.repeat(mag[:1], side, 0), mag, np.repeat(mag[-1:], side, 0)])
    for a in range(0, len(mag), 3000):
        b = min(a + 3000, len(mag))
        lo = a if past else a + side + 1
        w = np.lib.stride_tricks.sliding_window_view(src[lo:lo + (b - a) + side - 1], side, axis=0)
        out[a:b] = np.percentile(w, q, axis=-1)
    return out


def read(part, start, secs):
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-ss', str(start), '-t', str(secs), '-i', f'audio/{part}.mp3',
                          '-ar', str(R), '-ac', '2', '-f', 'f32le', '-'], capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2).astype(np.float64)


def write(path, a):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'f32le', '-ar', str(R), '-ac', '2', '-i', '-', '-b:a', '192k',
                    str(path)], input=np.clip(a, -1, 1).astype(np.float32).tobytes())


def picture(path, a, b, seconds=10, top=8000):
    k = min(len(a), seconds * R)
    both = np.concatenate([a[:k], b[:k]]).astype(np.float32).tobytes()
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'f32le', '-ar', str(R), '-ac', '2', '-i', '-', '-filter_complex',
                    f'showspectrumpic=s=1600x700:legend=0:stop={top}:scale=log:fscale=lin', str(path)], input=both)
