"""Effects in repeating music, on made-up audio, so no game data is needed."""
import pathlib
import sys
import unittest

try:
    import numpy as np
except ImportError:
    np = None

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools" / "analysis"))

RATE = 32000
PERIOD = 12
LEVEL = -0.7  # the recording chain inverts, as it does for real


def music(seconds, seed=1):
    """Noise shaped to be music-like: it never repeats by itself, but matches exactly."""
    noise = np.random.default_rng(seed).normal(size=seconds * RATE)
    return np.convolve(noise, np.ones(3) / 3, "same") * 0.1


def blip():
    t = np.arange(int(0.04 * RATE)) / RATE
    return np.sin(2 * np.pi * 1500 * t) * np.exp(-t * 60) * 0.3


def recording(places, passes=2):
    """Passes of the same music abutting, with a blip at each (pass, second)."""
    one = music(PERIOD)
    audio = np.concatenate([one] * passes)
    audio += np.random.default_rng(7).normal(size=audio.shape) * 1e-3  # as MP3 leaves it
    sound = blip()
    for number, second in places:
        at = int((number * PERIOD + second) * RATE)
        audio[at:at + len(sound)] += LEVEL * sound
    return one, audio


@unittest.skipUnless(np, "needs numpy")
class MatchEffects(unittest.TestCase):
    def setUp(self):
        import match_effects
        self.me = match_effects

    def test_clean_pass_avoids_every_effect(self):
        places = [(0, s) for s in (1, 3, 5, 7, 9)] + [(1, s) for s in (2, 4, 6, 8, 10)]
        one, audio = recording(places)
        stereo = np.stack([audio, audio], axis=1).astype(np.float32)
        mid = audio.astype(np.float32)
        result = self.me.clean_loop(stereo, mid, [0, PERIOD * RATE], PERIOD, RATE, log=lambda *_: None)
        self.assertLess(result["dirty_after"], 0.2)
        self.assertGreater(len(result["takes"]), 1)
        final = self.me.render(result["takes"], result["length"], mid)
        self.assertLess(np.abs(final - one.astype(np.float32)).max(), 0.02)

    def test_passes_that_cancel_are_clean_and_a_faint_sound_stops_them(self):
        one = music(PERIOD).astype(np.float32)
        takes = np.stack([one, one, one]).astype(np.float32)
        takes += np.random.default_rng(5).normal(size=takes.shape).astype(np.float32) * 1e-3  # MP3's noise
        sound = (blip() * 0.1).astype(np.float32)
        at = 5 * RATE
        takes[1, at:at + len(sound)] += sound
        slices = takes.shape[1] // int(self.me.SLICE_SECONDS * RATE)
        agreed, voices = self.me.find_agreement(takes, RATE, slices)
        slice_at = int(5 / self.me.SLICE_SECONDS)
        self.assertFalse(agreed[1, slice_at])
        self.assertTrue(agreed[0, slice_at])
        self.assertTrue(agreed[2, slice_at])
        self.assertGreater(agreed[:, :slice_at - 10].mean(), 0.95)
        self.assertFalse(voices.any())

    def test_a_long_stretch_nothing_agrees_with_is_a_voice(self):
        one = music(PERIOD).astype(np.float32)
        takes = np.stack([one, one, one]).astype(np.float32)
        takes += np.random.default_rng(5).normal(size=takes.shape).astype(np.float32) * 1e-3
        talk = music(3, seed=9).astype(np.float32)
        takes[2, 4 * RATE:7 * RATE] += talk
        slices = takes.shape[1] // int(self.me.SLICE_SECONDS * RATE)
        _, voices = self.me.find_agreement(takes, RATE, slices)
        self.assertTrue(voices[2, int(5.5 / self.me.SLICE_SECONDS)])
        self.assertFalse(voices[2, int(1 / self.me.SLICE_SECONDS)])
        self.assertFalse(voices[0].any())

    def test_rise_shows_which_pass_stands_out(self):
        # Three repeats, one with a burst.
        one = music(PERIOD)
        takes = np.stack([one, one, one]).astype(np.float32)
        burst = np.random.default_rng(2).normal(size=int(0.03 * RATE)) * 0.5
        at = int(5 * RATE)
        takes[1, at:at + len(burst)] += burst
        high = self.me.rise_scores(np.stack([self.me.band_levels(t, RATE) for t in takes])) > 6
        hop = int(self.me.RISE_WINDOW_SECONDS * RATE) // 2
        self.assertTrue(high[1, at // hop:(at + len(burst)) // hop].any())
        self.assertFalse(high[0].any())
        self.assertFalse(high[1, :at // hop - 4].any())

    def test_other_parts_passes_survive_the_cue_file(self):
        class Fake:
            rate = 1000
            offsets = {"zen": 0, "chu": 500000}

            def locate(self, position):
                return ("chu", position - 500000) if position >= 500000 else ("zen", position)

        sources = Fake()
        positions = [2000, 503000, 504500]
        text = self.me.format_also(positions, sources)
        self.assertEqual(text, "zen 2.000 chu 3.000 4.500")
        self.assertEqual(self.me.parse_also(text, sources), positions)
        self.assertEqual(self.me.parse_also("", sources), [])
        # A part at another sample rate isn't among the offsets, so it's left out.
        self.assertEqual(self.me.parse_also("kou 1.0 chu 3.0", sources), [503000])

    def test_search_finds_a_window_wherever_it_is(self):
        audio = music(40, seed=3).astype(np.float32)
        anchor = audio[int(25 * RATE):int(31 * RATE)].astype(np.float64)
        self.assertEqual(self.me.search(audio, 0, len(audio), anchor), [int(25 * RATE)])

    def test_rotating_keeps_each_sample_in_place(self):
        takes = [[0, 1000], [500, 9000], [900, 1000]]
        turned = self.me.rotated(takes, 40)
        self.assertEqual(turned[0], [0, 1040])
        self.assertEqual(turned[1][0] + turned[1][1], takes[1][0] + takes[1][1])
        self.assertEqual(turned[2][0] + turned[2][1], takes[2][0] + takes[2][1])
        self.assertEqual(self.me.rotated(takes, -40)[0][0], 0)
        self.assertEqual(len(self.me.rotated(takes, 600)), 2)

    def test_seam_is_where_lag_jumps(self):
        # The second pass restarts 30 samples late at second 8 of its phase.
        one = music(PERIOD)
        audio = np.concatenate([one, one[:int(8 * RATE)], np.zeros(30), one[int(8 * RATE):], one])
        seam = self.me.find_seam(audio.astype(np.float32), [0, PERIOD * RATE], PERIOD * RATE, RATE)
        self.assertIsNotNone(seam)
        self.assertLess(abs(seam / RATE - 8), 2.0)


if __name__ == "__main__":
    unittest.main()
