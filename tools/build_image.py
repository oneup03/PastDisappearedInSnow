#!/usr/bin/env python3
"""Turn verified episode packs into the images the recompiler and runner use.

usage: tools/build_image.py [--dir FOLDER] [--out images] [--dev-bios-font FILE]
                            [zen chu kou]

Applies data/image_recipes.toml to each pack and writes images/<key>.sfc. The
output is ROM-derived, so it stays local and git-ignored.
"""
import argparse
import hashlib
import pathlib
import sys
import tomllib

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from pvs.roms import EPISODES, load_pack  # noqa: E402

RECIPES = TOOLS.parent / "data" / "image_recipes.toml"
HEADER_BASE = 0xFFC0
MAP_MODE = HEADER_BASE + 0x15
CHECKSUM_COMP = HEADER_BASE + 0x1C
CHECKSUM = HEADER_BASE + 0x1E
ROM_SIZE = HEADER_BASE + 0x17

# Glyph table read by $C0:F7B1: entries of (first Shift-JIS code, pointer, bank).
FONT_TABLE = 0xF733
FONT_TABLE_FIRST = 2
FONT_TABLE_END = 0x5C
FONT_ENTRY_SIZE = 6

# The 12x12 kanji font lives in the BIOS at $89-$8F:8000-FFFF. It can stand in
# during development, until our own font renderer exists. Images built this way
# hold BIOS data, so like everything in images/ they must stay local.
BS_X_SHA1 = "4891d739a8a8b67923681bad4fb67edab2e90e50"
BIOS_FONT_FILE_OFFSET = 0x48000
BIOS_FONT_SIZE = 7 * 0x8000
BIOS_FONT_FIRST_BANK = 0x89
EXTENDED_SIZE = 0x200000
EXTENDED_ROM_SIZE_BYTE = 0x0B
FONT_IMAGE_OFFSET = 0x100000
FONT_IMAGE_BANK = 0xD0


class RecipeError(Exception):
    pass


def apply_patch(image, patch, key):
    offset = patch["offset"]
    expect = bytes.fromhex(patch["expect"])
    write = bytes.fromhex(patch["write"])
    if len(expect) != len(write):
        raise RecipeError(f"{key} +{offset:06X}: expect and write differ in length")
    actual = bytes(image[offset:offset + len(expect)])
    if actual != expect:
        raise RecipeError(f"{key} +{offset:06X} ({patch.get('why', 'patch')}): "
                          f"expected {expect.hex()}, found {actual.hex()}")
    image[offset:offset + len(write)] = write


def write_header(image, header):
    image[MAP_MODE] = header["map_mode"]
    image[MAP_MODE + 1] = header["cart_type"]
    image[MAP_MODE + 2] = header["rom_size"]
    image[MAP_MODE + 3] = header["ram_size"]
    image[MAP_MODE + 4] = header["region"]
    image[MAP_MODE + 5] = header["developer"]


def fix_checksum(image):
    image[CHECKSUM_COMP:CHECKSUM_COMP + 4] = b"\xFF\xFF\x00\x00"
    total = sum(image) & 0xFFFF
    image[CHECKSUM_COMP:CHECKSUM_COMP + 2] = (total ^ 0xFFFF).to_bytes(2, "little")
    image[CHECKSUM:CHECKSUM + 2] = total.to_bytes(2, "little")


def embed_bios_font(image, bios):
    """Append the BIOS font and repoint the glyph table at it."""
    if hashlib.sha1(bios).hexdigest() != BS_X_SHA1:
        raise RecipeError("BIOS file is not the expected BS-X (Rev.01) dump")
    image.extend(bios[BIOS_FONT_FILE_OFFSET:BIOS_FONT_FILE_OFFSET + BIOS_FONT_SIZE])
    image.extend(b"\xFF" * (EXTENDED_SIZE - len(image)))
    image[ROM_SIZE] = EXTENDED_ROM_SIZE_BYTE
    for x in range(FONT_TABLE_FIRST, FONT_TABLE_END, FONT_ENTRY_SIZE):
        pointer = int.from_bytes(image[FONT_TABLE + x + 2:FONT_TABLE + x + 4], "little")
        bank = image[FONT_TABLE + x + 4]
        if not BIOS_FONT_FIRST_BANK <= bank < BIOS_FONT_FIRST_BANK + 7:
            continue
        offset = ((bank - BIOS_FONT_FIRST_BANK) << 15) | (pointer & 0x7FFF)
        image[FONT_TABLE + x + 2:FONT_TABLE + x + 4] = (offset & 0xFFFF).to_bytes(2, "little")
        image[FONT_TABLE + x + 4] = FONT_IMAGE_BANK + (offset >> 16)


def build(pack, key, recipes, bios=None):
    image = bytearray(pack)
    for entry in recipes["episode"]:
        if entry["key"] == key:
            for patch in entry.get("patch", ()):
                apply_patch(image, patch, key)
    write_header(image, recipes["header"])
    if bios is not None:
        embed_bios_font(image, bios)
    fix_checksum(image)
    return bytes(image)


def load_recipes(path=RECIPES):
    with open(path, "rb") as f:
        return tomllib.load(f)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("episodes", nargs="*", default=[e.key for e in EPISODES])
    ap.add_argument("--dir", default=".", help="folder holding the packs")
    ap.add_argument("--out", default="images", help="output folder")
    ap.add_argument("--dev-bios-font", metavar="FILE",
                    help="embed the font from a BS-X BIOS dump (development only)")
    args = ap.parse_args()
    recipes = load_recipes()
    bios = pathlib.Path(args.dev_bios_font).read_bytes() if args.dev_bios_font else None
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for key in args.episodes:
        try:
            image = build(load_pack(key, args.dir), key, recipes, bios)
        except (FileNotFoundError, RecipeError) as e:
            sys.exit(f"build_image: {e}")
        path = out / f"{key}.sfc"
        path.write_bytes(image)
        print(f"{path}: {len(image)} bytes")


if __name__ == "__main__":
    main()
