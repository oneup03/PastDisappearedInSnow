# The pitches of the player's sounds, for the game's own cleaning: data/cues/sounds.toml.
# The sounds themselves are local captures from the sound chip (local/rec/fx/<part>_templates.npz);
# only what the cleaning needs is written: each sound's length and its strongest pitches.
#   export_sounds.py   (from the repository root)
import sys, pathlib
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import one_take_filter as f
import long_sounds as l

N = f.P['n']


def hz(b):
    return b * f.R / N


lines = ['# The player\'s sounds, from the sound chip: how long each is heard (s), and its strongest\n',
         '# pitches in Hz with each one\'s level in dB under the strongest. Written by\n',
         '# tools/analysis/clean/export_sounds.py. "sound" lines feed the one-take filter;\n',
         '# "typing" marks the text sounds; "long" lines are sounds longer than its window.\n']
for part in ('zen', 'chu', 'kou'):
    for name, (ps, core) in f.load_sounds(part, N).items():
        kind = 'typing' if name[1:].isdigit() and 32 <= int(name[1:]) <= 48 else 'sound'
        lines.append(f'{kind} = {part} {name} {core:.8f} ' + ' '.join(f'{hz(b):g}:{db:.2f}' for b, db in ps) + '\n')
    for name, (strong, core) in l.long_sounds(part).items():
        lines.append(f'long = {part} {name} {core:.8f} ' + ' '.join(f'{hz(b):g}' for b in strong) + '\n')
out = pathlib.Path('data/cues/sounds.toml')
out.write_text(''.join(lines))
print(f'wrote {out}: {len(lines) - 4} sounds')
