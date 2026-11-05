"""tools/make_boxart.py on a made-up picture, so no art from the ad is needed."""
import pathlib
import sys
import unittest

from PIL import Image, ImageChops

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import make_boxart as mb  # noqa: E402


@unittest.skipUnless(pathlib.Path(mb.FONT_DEFAULT).exists(), "Noto Sans isn't installed")
class EnglishBoxArt(unittest.TestCase):
    def test_title_area_changes_and_nothing_else(self):
        art = Image.new("RGB", (551, 720), (90, 120, 170))
        out = mb.english(art.copy(), mb.FONT_DEFAULT)
        self.assertEqual(out.size, art.size)
        diff = ImageChops.difference(out, art).convert("L")
        box = diff.getbbox()
        self.assertIsNotNone(box)
        # Changes stay around the title area: the frost's soft edge reaches a little further.
        self.assertGreater(box[1], 0.10 * 720)
        self.assertLess(box[3], 0.50 * 720)
        # The bottom of the picture, where Ayumi is, is untouched.
        self.assertIsNone(diff.crop((0, 400, 551, 720)).getbbox())

    def test_fit_shrinks_to_width(self):
        font = mb.fit(mb.FONT_DEFAULT, mb.TITLE, 200, 90)
        self.assertLessEqual(font.getbbox(mb.TITLE)[2] - font.getbbox(mb.TITLE)[0], 200)


if __name__ == "__main__":
    unittest.main()
