"""Headless runs of the real game: the mouse and the title screen.

These need a local build and the player's images, so they skip anywhere else. Each run
takes some seconds.
"""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = ROOT / "build" / "BsTanteiClubSNESRecomp"
IMAGE = ROOT / "images" / "zen.sfc"
# The tutorial's pointer object, and where it keeps its place.
POINTER = 0x2600
POINTER_X, POINTER_Y = 0x1E, 0x20
WRAM_MINUTE, WRAM_SECOND, WRAM_CLOCK_FLAGS = 0x4183, 0x4184, 0x4186
CLOCK_SYNCED = 0x01


def run(script, env=None, timeout=120):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "run.script").write_text(script)
        environ = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", PVS_AUDIO="rom",
                       PVS_LANGUAGE="en", SNESRECOMP_DUMP_DIR=str(tmp), **(env or {}))
        subprocess.run([str(BINARY), "--no-launcher", "--rom", str(IMAGE), "--script", str(tmp / "run.script")],
                       env=environ, cwd=tmp, capture_output=True, timeout=timeout)
        return {p.name: p.read_bytes() for p in tmp.iterdir() if p.suffix in (".bin",)}


def word(ram, at):
    return ram[at] | ram[at + 1] << 8


@unittest.skipUnless(BINARY.exists() and IMAGE.exists(), "needs a local build and images")
class Native(unittest.TestCase):
    def test_mouse_puts_the_hand_under_the_pointer(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as moves:
            moves.write("1700 140 40 0\n1710 150 50 0\n")
        try:
            dumps = run("wait 1800\ndump here\nquit\n", {"PVS_TEST_MOUSE": moves.name})
        finally:
            os.unlink(moves.name)
        ram = dumps["here.wram.bin"]
        self.assertEqual((word(ram, POINTER + POINTER_X), word(ram, POINTER + POINTER_Y)), (150, 50))

    def test_title_holds_the_clock_until_a_press(self):
        held = run("wait 400\ndump held\nquit\n", {"PVS_TITLE": "1"})["held.wram.bin"]
        # Still waiting for the first time signal.
        self.assertFalse(held[WRAM_CLOCK_FLAGS] & CLOCK_SYNCED)
        going = run("wait 300\npress select 4\nwait 600\ndump going\nquit\n", {"PVS_TITLE": "1"})["going.wram.bin"]
        # Joined at 0:00 after the press, and running.
        self.assertEqual(going[WRAM_MINUTE], 0)
        self.assertTrue(1 <= going[WRAM_SECOND] <= 12, going[WRAM_SECOND])


if __name__ == "__main__":
    unittest.main()
