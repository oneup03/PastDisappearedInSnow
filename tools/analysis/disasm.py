#!/usr/bin/env python3
"""Disassemble part of an episode pack.

usage: tools/analysis/disasm.py <zen|chu|kou> <hex offset> [count] [--m8] [--x8] [--all]
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from pvs.dis65816 import disassemble  # noqa: E402
from pvs.roms import INPUTS, load_pack  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("episode", choices=("zen", "chu", "kou"))
    ap.add_argument("offset", type=lambda s: int(s, 16))
    ap.add_argument("count", nargs="?", type=int, default=64)
    ap.add_argument("--m8", action="store_true", help="start with 8-bit accumulator")
    ap.add_argument("--x8", action="store_true", help="start with 8-bit index")
    ap.add_argument("--all", action="store_true", help="keep going past returns")
    ap.add_argument("--dir", default=INPUTS, help="folder holding the packs")
    args = ap.parse_args()
    data = load_pack(args.episode, args.dir)
    for line in disassemble(data, args.offset, args.count, int(args.m8), int(args.x8),
                            stop_at_return=not args.all):
        print(line)


if __name__ == "__main__":
    main()
