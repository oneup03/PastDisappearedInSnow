"""Scene bytecode decoding, on a synthetic script. No ROM data involved."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
from pvs import script as sc  # noqa: E402

AIUEO = "あいう".encode("shift_jis")


def make_image(scenes):
    """Lay scenes out in $C2 behind a scene table."""
    image = bytearray(0x40000)
    off = sc.SCHEDULE_TABLE + 3 * (len(scenes) + 1)
    for i, scene in enumerate(scenes):
        table = sc.SCHEDULE_TABLE + i * 3
        image[table:table + 3] = (off & 0xFFFF).to_bytes(2, "little") + bytes([0xC0 | off >> 16])
        image[off:off + len(scene)] = scene
        off += len(scene)
    end = sc.SCHEDULE_TABLE + len(scenes) * 3
    image[end:end + 2] = b"\xFF\xFF"
    return bytes(image)


def op(code, *args):
    return bytes([code, *args])


SCENE0 = b"".join([
    op(sc.OP_STRCPY, 0x34, 0x7A, 0xC0), "あゆみ「".encode("shift_jis"), b"\0",
    op(sc.OP_END),
])
SCENE1 = b"".join([
    op(sc.OP_EVENT, 0, 0),
    op(sc.OP_SYNC_CLOCK, 0x2C, 0x01),
    op(sc.OP_SPEAKER, 0x34, 0x7A, 0xC0),
    AIUEO, op(sc.OP_NEWLINE), AIUEO, op(sc.OP_WAIT, 0x1E, 0x00), b"ABC", op(sc.OP_WAIT, 0xFF, 0xFF),
    op(sc.OP_CLEAR),
    op(sc.OP_DEADLINE, 0x10, 0x00),
    op(sc.OP_MENU, 1), bytes([1, 0x34, 0x12]), "はい".encode("shift_jis").ljust(16, b" "),
    op(sc.OP_SCENE, 2),
    op(sc.OP_END),
])


class WalkTest(unittest.TestCase):
    def test_schedule_gives_start_seconds_and_scene_zero_has_none(self):
        image = make_image([SCENE0, SCENE1])
        self.assertEqual([start for _, start in sc.schedule(image)], [None, 300])

    def test_walk_steps_over_operands_and_text(self):
        image = make_image([SCENE0, SCENE1])
        addr = sc.schedule(image)[1][0]
        items = list(sc.walk(image, addr))
        self.assertEqual(items[-1].op, sc.OP_END)
        self.assertEqual(items[-1].addr, addr + len(SCENE1) - 1)

    def test_half_width_katakana_is_one_byte(self):
        image = make_image([b"\xB1\xB2" + op(sc.OP_END)])
        items = list(sc.walk(image, sc.schedule(image)[0][0]))
        self.assertEqual(len(items[0].raw), 2)
        self.assertEqual(items[1].op, sc.OP_END)


class MessagesTest(unittest.TestCase):
    def setUp(self):
        self.rows = sc.messages(make_image([SCENE0, SCENE1]))

    def test_inline_ops_become_tags(self):
        text = [r for r in self.rows if r.kind == "text"][0]
        self.assertEqual(text.text, "あいう{n}あいう{w:30}ABC{w}")

    def test_speaker_name_comes_from_scene_zero(self):
        text = [r for r in self.rows if r.kind == "text"][0]
        self.assertEqual(text.speaker, "あゆみ")

    def test_menu_labels_and_strings_are_rows(self):
        self.assertIn(("string", "あゆみ「"), [(r.kind, r.text) for r in self.rows])
        self.assertIn(("menu", "はい"), [(r.kind, r.text) for r in self.rows])


class LayoutTest(unittest.TestCase):
    FONT = {ord(c): 6 for c in "abcdefghijklmnopqrstuvwxyz "}

    def test_short_text_fits_on_one_row(self):
        self.assertEqual(sc.layout_rows("hello world", self.FONT)[0], 1)

    def test_long_text_wraps_at_word_boundaries(self):
        rows, missing = sc.layout_rows(" ".join(["abcdefghij"] * 8), self.FONT)
        self.assertEqual(rows, 3)
        self.assertEqual(missing, set())

    def test_forced_break_and_tags_do_not_take_width(self):
        self.assertEqual(sc.layout_rows("ab{n}cd{w:30}", self.FONT)[0], 2)

    def test_clearing_starts_a_new_page(self):
        text = "{c}".join([" ".join(["abcdefghij"] * 8)] * 2)
        self.assertEqual(sc.layout_rows(text, self.FONT)[0], 3)

    def test_missing_glyphs_are_reported(self):
        self.assertEqual(sc.layout_rows("ab!", self.FONT)[1], {"!"})


class TagsTest(unittest.TestCase):
    def test_translation_own_tags_are_not_kept(self):
        self.assertEqual(sc.kept_tags("a{n}b{c}{w:30}c{w}"), ["{w:30}", "{w}"])


if __name__ == "__main__":
    unittest.main()
