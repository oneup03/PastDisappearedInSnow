# Where the player's longer sounds (the alarm and the like) are still in the game's own renders:
# each loop's audio/loops/<part>_<n>.pcm, through the long-sound check, at a lower bar so that
# what's near it shows too. Times are seconds into the loop.
#   scan_long_sounds.py [<part> ...] [--bar 12]
import sys, pathlib, struct, subprocess
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import long_sounds as l

args = sys.argv[1:]
bar = 12.0
if '--bar' in args:
    i = args.index('--bar')
    bar = float(args[i + 1])
    del args[i:i + 2]
parts = args or ['zen', 'chu', 'kou']
l.STANDS = bar
for part in parts:
    for path in sorted(pathlib.Path('audio/loops').glob(f'{part}_*.pcm'), key=lambda p: int(p.stem.split('_')[1])):
        raw = path.read_bytes()
        magic, key, samples, rate = struct.unpack('<4sIII', raw[:16])
        if magic != b'PVSL':
            print(f'{path.name}: skipped, not a render')
            continue
        if rate != l.R:   # the check works at the tools' rate
            pcm = subprocess.run(['ffmpeg', '-loglevel', 'error', '-f', 's16le', '-ar', str(rate), '-ac', '2', '-i', '-',
                                  '-ar', str(l.R), '-f', 'f32le', '-'], input=raw[16:], capture_output=True, check=True).stdout
            X, rate = np.frombuffer(pcm, np.float32).reshape(-1, 2).astype(np.float64), l.R
        else:
            X = np.frombuffer(raw[16:], np.int16).reshape(-1, 2).astype(np.float64) / 32768
        found = []
        for a in range(0, len(X), 60 * rate):   # a minute at a time, with a little either side
            lo, hi = max(a - 2 * rate, 0), min(a + 62 * rate, len(X))
            rep = {}
            l.clean(X[lo:hi], part, rep)
            found += [(round(t + lo / rate, 2), name, sc) for t, name, sc in rep['events'] if a / rate <= t + lo / rate < a / rate + 60]
        listed = ', '.join(f'{t:.2f} {name} {sc:.0f}{" !" if sc >= 20 else ""}' for t, name, sc in found)
        print(f'{path.stem} ({len(X) / rate:.0f} s): {listed or "none"}', flush=True)
