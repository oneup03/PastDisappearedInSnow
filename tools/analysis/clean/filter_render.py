# The v7 one-take filter over a rendered loop: what two takes left behind. Writes only <tag>.mp3.
#   filter_render.py <part> <loop> <outdir> <tag>
import sys, struct, subprocess, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import numpy as np
import one_take_filter as deblip3
R = deblip3.R
part, n, outdir, tag = sys.argv[1], int(sys.argv[2]), pathlib.Path(sys.argv[3]), sys.argv[4]
b = open(f'audio/loops/{part}_{n}.pcm', 'rb').read(); rate = struct.unpack('<4sIII', b[:16])[3]
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-f', 's16le', '-ar', str(rate), '-ac', '2', '-i', '-', '-ar', str(R), '-f', 'f32le', '-'],
                     input=b[16:], capture_output=True, check=True).stdout
X = np.frombuffer(raw, np.float32).reshape(-1, 2).astype(np.float64)
CH, OV = 120 * R, 4 * R
Y = np.zeros_like(X); w = np.zeros(len(X))
starts = list(range(0, len(X), CH))
if len(starts) > 1 and len(X) - starts[-1] < CH // 4:
    starts.pop()
for i, a in enumerate(starts):
    lo = max(a - OV, 0); hi = len(X) if i + 1 == len(starts) else min(a + CH + OV, len(X))
    y = deblip3.deblip(X[lo:hi], part, deblip3.P, {})
    ramp = np.ones(hi - lo)
    if lo > 0: ramp[:2 * OV] = np.linspace(0, 1, 2 * OV)
    if hi < len(X): ramp[-2 * OV:] = np.minimum(ramp[-2 * OV:], np.linspace(1, 0, 2 * OV))
    Y[lo:hi] += y * ramp[:, None]; w[lo:hi] += ramp
Y /= np.maximum(w, 1e-9)[:, None]
deblip3.write(outdir / f'{tag}.mp3', Y)
print(tag, 'removed', round(float(10 * np.log10(((X - Y) ** 2).sum() / (X ** 2).sum())), 1), 'dB')
