# Investigation music: cleaning status

Where each looping track stands, so any of them can be revisited. Times are in seconds of
that part's recording (`audio/<part>.mp3`). "Into the loop" times count from the loop's start.
Loop numbers are the order of `loop =` lines in `data/cues/<part>.toml`.

## How a track gets cleaned

All cleaning happens on the player's machine, from the numbers in `data/cues/`: the game renders
each loop once (`src/loop_render.c`, `src/loop_clean.c`). The tools below find those numbers, and
the ones in `tools/analysis/clean/` also write MP3s of their result, for listening.

| Method | When | Tool |
|---|---|---|
| **Repeats** | The track comes round again, in this recording or another. Each slice reads whichever pass has nothing over it. | `tools/analysis/match_effects.py` |
| **Suite** | The opening track: three sections in a different order in each part, rebuilt as one cycle. | `match_effects.py`, `suite =` line |
| **Two takes** | One pass of the track in each of two recordings. Cuts between them, and the render replaces each known player sound from the other take. | `tools/analysis/two_takes.py` |
| **Copies** | The music repeats within one take: its own exact repeat (`copies =`), or its bars (`bars =`). Pitch by pitch, a point that stands clearly above its copy reads the copy, or comes down to the bars' level if it's brief like a beep. | `clean/self_similar.py` finds the lags, `clean/copy_merge.py` |
| **Filter** | One take with the player's sounds on it (`clean = filter`). Each known sound's pitches are found where they jump out together, and brought down to the level around them. Typing runs are learnt and taken off as they go. | `clean/one_take_filter.py`, with the pitches in `data/cues/sounds.toml` from `clean/export_sounds.py` |
| **Long sounds** | The alarm and others too long for the filter's window (with the filter, or alone as `clean = long`). | `clean/long_sounds.py`; `clean/scan_long_sounds.py` lists where any are left in the renders |
| **One take** | Only one pass anywhere. A loop with a fade for holds. Once cleaned, the first time through plays the cleaned render too, up to the fade. | `tools/analysis/soft_loop.py` |

Dialogue is never cut. On repeating tracks the first time through plays the recording wherever
there is dialogue (`keep` ranges), and a hold plays a clean pass without it wherever another pass
has none. On one-take tracks the first time through plays the cleaned render up to its fade, so
the filter runs under any dialogue there, taking off only the player's sounds.

## Repeating tracks

| Part / loop | Start | Period | Method | Status |
|---|---|---|---|---|
| zen 0, chu 0, kou 0 | 187.9, 97.7, 192.0 | 118.5 | Suite | Better than before. Effects remain into the loop at 41 (faint typing), 49 (timer beep), 52 to 63, and from 106 on, where only one pass exists. A sound like an ending at 106.8 is in every pass. Kouhen: a rough transition at 33. |
| zen 1 | 404.7 | 144.3 | Repeats | Beeps and typing from 142 into the loop to its end. Soundtrack version: Utsugi Detective Agency. |
| zen 2 | 907.5 | 136.5 | Repeats | Beeps and typing at 6 to 13, 16, 19 into the loop. Percussion changes between recordings. |
| zen 3 | 1480.3 | 143.7 | Repeats | Beeps at 26, 54; typing and beeps at 75 to 77, 90 to 91. Voice in every pass at 0 to 27 and 75 to 79. Soundtrack version: Memories. |
| zen 7 | 3335.6 | 48.4 | Repeats | Approved. Voice in every pass at 0 to 27, so a hold repeats it. |
| chu 3 | 979.5 | 136.5 | Repeats | A beep at 32 into the loop. Percussion changes, not too bad. |
| chu 12 | 2777.7 | 144.3 | Repeats | Beeps and typing at 30 to 34 into the loop. Soundtrack version: Utsugi Detective Agency. |
| chu 13 | 3267.3 | 42.4 | Repeats | Approved. |
| kou 1 | 702.9 | 71.9 | Repeats | Typing and beeps at 60 to 63 into the loop. Soundtrack version: Memories. |
| kou 4 | 1598.5 | 136.5 | Repeats | A dip at 12, beeps at 58 to 60 and 112 into the loop. Percussion changes, not too bad. |
| kou 6 | 2598.7 | 144.3 | Repeats | A beep at 80 into the loop. Soundtrack version: Utsugi Detective Agency. |

## Tracks with fewer passes

| Part / loop | Start | Length | Method | Status |
|---|---|---|---|---|
| zen 4 (E) | 1849.6 | 241 | Bars (every 16.04 s), then filter | Accepted for now. |
| zen 5 (Z9) | 2283.4 | 334 | Copies, then filter | Repeats within itself every 142.24 s from 48 to 174 s in; that stretch reads from its copy, and the filter does the rest. Alarms at 16.5 and 326.8 taken off. Accepted: effects gone or much quieter. |
| zen 6 (Z11) | 3166.3 | 145 | Bars (every 28.87 s), then filter | Accepted for now. Same tune as Utsugi, but a different performance from the other parts' takes. Soundtrack version: Utsugi, in holds. |
| chu 1 (haze) | 559.7 | 132 | Bars (every 8.99 s), then filter | Accepted for now; the bars hold only briefly here, so it's mostly the filter. Earlier review heard beeps at 10, 16, 19, 48, 57, 70, 79 to 80, 83, 91, 94, 104 to 108, 114 to 116 in. Soundtrack version: Tension, in holds. |
| chu 2 (sparse) | 727.8 | 132 | Copies, then filter | Repeats within itself every 71.6 s over part of its length; reads from that copy, then the filter. Accepted. |
| chu 4 (C7) | 1265.1 | 83 | One take | Filtered. Still heard: a click-like typing at 10 to 12, beep at 52, typing at 32. The clicks are like the track's own percussion. |
| chu 5 (C9) | 1435.7 | 200 | Copies, then filter | Repeats every 114.63 s over its first 85 s; reads from that copy, then the filter. Accepted. |
| chu 6 (C10) | 1717.0 | 233 | Two takes (Kouhen 1280.09), then filter | Rebuilt from both recordings. Accepted. |
| chu 7 / kou 5 | 1981.6 / 1918.6 | 280 | Two takes, then long sounds | The track with birdsong over it. An alarm at 270 taken off. |
| chu 8 (C12) | 2133.6 | 152 | One take | Filtered; passes for now. |
| chu 9, 10 (C12 a, b) | 2616.0, 2639.0 | 12, 46 | One take | Filtered; passes for now. |
| chu 11 (C12 c) | 2697.8 | 76 | Bars (every 8.99 s), then filter | Accepted for now. Alarms at 20.7 and 65.4 taken off. Soundtrack version: Tension, in holds. |
| kou 2 (K5) | 1281.0 | 112 | Two takes (Chuuhen 1717.96), then filter | Rebuilt from both recordings. Accepted. |
| kou 3 (K6) | 1406.8 | 160 | Two takes (Chuuhen 1843.77) for its first 108 s, then one take; then filter | Rebuilt. Accepted. |

The same performance lies 436.929 s earlier in Kouhen than in Chuuhen across C10, K5 and K6.

`scan_long_sounds.py` over every render finds nothing else that stands as clear as the alarms
(20 dB or more). Weaker matches, 12 to 18 dB, are listed by it throughout and sound like the music
itself so far; any of them is worth checking by ear if an alarm is heard.
Tracks marked as matching another tune by ear or by pitch but not sample for sample (haze, C7,
C12 c, E, Z11) were checked and are different performances, so they can't be used to clean.

## Soundtrack versions

`data/soundtrack/tracks.toml` maps Utsugi Detective Agency, Memories and Tension to the loops
above, from the Super Famicom album (1998) or the Switch remakes (2021). "In holds" means the
first time through plays entirely as recorded, so the album version only plays during a hold.
Possible further matches by pitch, not confirmed: The Girl of the Legend (zen 5, chu 5) and
Investigation (chu 7, kou 5).
