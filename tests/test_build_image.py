"""build_image must refuse wrong bytes and produce a valid HiROM header."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import build_image as bi  # noqa: E402

PACK_SIZE = 0x100000


def synthetic_recipes(expect="11 22", write="33 44"):
    return {
        "header": {"map_mode": 0x31, "cart_type": 2, "rom_size": 0x0A, "ram_size": 5,
                   "region": 0, "developer": 0x33},
        "episode": [{"key": "zen", "patch": [
            {"offset": 0x1000, "expect": expect, "write": write, "why": "test"}]}],
    }


def synthetic_pack():
    pack = bytearray(PACK_SIZE)
    pack[0x1000:0x1002] = b"\x11\x22"
    return bytes(pack)


class BuildImageTest(unittest.TestCase):
    def test_applies_patch_and_keeps_size(self):
        image = bi.build(synthetic_pack(), "zen", synthetic_recipes())
        self.assertEqual(len(image), PACK_SIZE)
        self.assertEqual(image[0x1000:0x1002], b"\x33\x44")

    def test_refuses_unexpected_bytes(self):
        with self.assertRaises(bi.RecipeError):
            bi.build(synthetic_pack(), "zen", synthetic_recipes(expect="99 99"))

    def test_refuses_length_change(self):
        with self.assertRaises(bi.RecipeError):
            bi.build(synthetic_pack(), "zen", synthetic_recipes(write="33 44 55"))

    def test_other_episodes_untouched(self):
        image = bi.build(synthetic_pack(), "kou", synthetic_recipes())
        self.assertEqual(image[0x1000:0x1002], b"\x11\x22")

    def test_header_and_checksum(self):
        image = bi.build(synthetic_pack(), "zen", synthetic_recipes())
        self.assertEqual(image[bi.MAP_MODE], 0x31)
        comp = int.from_bytes(image[bi.CHECKSUM_COMP:bi.CHECKSUM_COMP + 2], "little")
        check = int.from_bytes(image[bi.CHECKSUM:bi.CHECKSUM + 2], "little")
        self.assertEqual(comp ^ check, 0xFFFF)
        self.assertEqual(sum(image) & 0xFFFF, check)


class BiosFontTest(unittest.TestCase):
    def test_refuses_wrong_bios(self):
        with self.assertRaises(bi.RecipeError):
            bi.build(synthetic_pack(), "zen", synthetic_recipes(), bios=bytes(PACK_SIZE))


if __name__ == "__main__":
    unittest.main()
