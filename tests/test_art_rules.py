"""translation/<language>/art.txt: every rule can be read the way src/art.c reads it."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
RULE = re.compile(r"^art (\S+) (\d+) (-?\d+) (-?\d+) (\d+) (\d+) ([0-9a-f]{8}) (\S+) (.+)$")
PARTS = {"zen", "chu", "kou"}
STYLES = {"tiny", "plain", "big", "map", "over"}
TINY = re.compile(r"^[A-Z |]+$")


class ArtRules(unittest.TestCase):
    def rules(self):
        for path in sorted(ROOT.glob("translation/*/art.txt")):
            for number, line in enumerate(path.read_text().splitlines(), 1):
                if line.strip() and not line.startswith("#"):
                    yield f"{path.relative_to(ROOT)}:{number}", line

    def test_rules_parse(self):
        for where, line in self.rules():
            with self.subTest(where=where):
                m = RULE.match(line)
                self.assertTrue(m, line)
                parts, layer, x, y, w, h, _hash, style, text = m.groups()
                self.assertTrue(set(parts.split(",")) <= PARTS)
                self.assertIn(int(layer), (0, 1, 2))
                self.assertTrue(set(style.split(",")) <= STYLES or any(s in style for s in STYLES))
                if "map" not in style:
                    self.assertTrue(0 <= int(x) and int(x) + int(w) <= 256 and 0 <= int(y) and int(y) + int(h) <= 224)
                self.assertTrue(text.isascii(), "the art font is ASCII")
                self.assertLess(len(text), 96, "MAX_TEXT in src/art.c")
                if "tiny" in style:
                    self.assertRegex(text, TINY, "the small font has capitals only")


if __name__ == "__main__":
    unittest.main()
