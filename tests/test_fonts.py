"""The cut-down language fonts must draw every character their translations use."""
import pathlib
import sys
import tomllib
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import subset_font  # noqa: E402
from pvs import script as sc  # noqa: E402


class LanguageFontTest(unittest.TestCase):
    def test_fonts_cover_their_text(self):
        for meta_path in sorted((ROOT / "translation").glob("*/meta.toml")):
            meta = tomllib.loads(meta_path.read_text(encoding="utf-8"))
            if "font" not in meta:
                continue
            lang = meta_path.parent.name
            with self.subTest(lang=lang):
                have = set(sc.load_font(ROOT / meta["font"]))
                needed = subset_font.wanted(lang) - {c for b in subset_font.ALWAYS for c in b}
                lacking = sorted(chr(c) for c in needed - have if chr(c).isprintable() and not chr(c).isspace())
                self.assertEqual(lacking, [], f"run tools/subset_font.py --lang {lang}")


if __name__ == "__main__":
    unittest.main()
