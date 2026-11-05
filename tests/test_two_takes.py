"""The rules for cleaning a track with birds over it, on synthetic data."""
import pathlib
import sys
import unittest

try:
    import numpy as np
except ImportError:
    np = None

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools" / "analysis"))

if np is not None:
    import two_takes as tt  # noqa: E402


@unittest.skipIf(np is None, "needs numpy")
class ChoosePath(unittest.TestCase):
    def test_moves_off_a_sound_when_it_can(self):
        dirty = np.zeros((2, 400), dtype=bool)
        dirty[0, 100:300] = True
        path = tt.choose_path(dirty, np.zeros(400, dtype=bool))
        self.assertFalse(dirty[path, np.arange(400)].any())

    def test_never_cuts_on_a_bird(self):
        dirty = np.zeros((2, 400), dtype=bool)
        dirty[0, 100:300] = True
        birds = np.zeros(400, dtype=bool)
        birds[50:350] = True
        path = tt.choose_path(dirty, birds)
        cuts = np.flatnonzero(np.diff(path)) + 1
        self.assertFalse(birds[cuts].any())

    def test_a_small_sound_is_not_worth_a_cut(self):
        dirty = np.zeros((2, 400), dtype=bool)
        dirty[0, 200:205] = True
        path = tt.choose_path(dirty, np.zeros(400, dtype=bool))
        self.assertEqual(len(np.flatnonzero(np.diff(path))), 0)


@unittest.skipIf(np is None, "needs numpy")
class FindSounds(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(1)
        time = np.arange(int(0.03 * tt.RATE)) / tt.RATE
        self.blip = np.sign(np.sin(2 * np.pi * 375 * time)) * np.hanning(len(time)) * 0.1
        self.music = np.sin(2 * np.pi * 220 * np.arange(tt.RATE * 2) / tt.RATE) * 0.3 + rng.normal(0, 0.0005, tt.RATE * 2)

    def test_finds_a_sound_with_the_music_cancelled(self):
        here, there = self.music.copy(), self.music.copy()
        here[20000:20000 + len(self.blip)] += self.blip
        found = tt.find_sounds(here, there, {"blip": (self.blip, len(self.blip), [])})
        self.assertEqual(len(found), 1)
        self.assertLess(abs(found[0][0] - 20000), 32)

    def test_says_whose_it_is(self):
        here, there = self.music.copy(), self.music.copy()
        here[20000:20000 + len(self.blip)] += self.blip
        self.assertEqual(tt.whose(here, there, 20000, self.blip), 0)
        self.assertEqual(tt.whose(there, here, 20000, self.blip), 1)


@unittest.skipIf(np is None, "needs numpy")
class Stretches(unittest.TestCase):
    def test_spans(self):
        frames = np.array([0, 1, 1, 0, 0, 1], dtype=bool)
        self.assertEqual(tt.spans(frames).tolist(), [[1, 3], [5, 6]])


if __name__ == "__main__":
    unittest.main()
