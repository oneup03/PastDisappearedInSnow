#!/usr/bin/env python3
"""List what an episode pack touches outside itself, by tracing its code.

usage: tools/analysis/trace_pack.py <zen|chu|kou> [--dir FOLDER]

Traces reachable code from the vectors and every `PHP/PHD/TSC/TCD` prologue,
tracking m/x widths. Only long-addressed operands count: the engine runs with
data bank $7E, so absolute `$5xxx` accesses are WRAM, not the BS-X MCC.

Dispatch through RTS and indirect pointers is not followed, so treat the result
as a floor and confirm with a runtime trace.
"""
import argparse
import collections
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from pvs.dis65816 import decode  # noqa: E402
from pvs.roms import INPUTS, load_pack  # noqa: E402

PROLOGUE = b"\x08\x0B\x3B\x5B"
ROM_BANKS = range(0xC0, 0xD0)
BASE_UNIT = range(0x2188, 0x21A0)


def is_ordinary_data(bank, addr):
    """True for WRAM, standard hardware registers, and the pack itself."""
    if bank in (0x7E, 0x7F) or bank in ROM_BANKS:
        return True
    if bank == 0x00:
        if addr >= 0x8000 or 0x4000 <= addr < 0x4400:
            return True
        return 0x2100 <= addr < 0x2200 and addr not in BASE_UNIT
    return False


def trace(data):
    seeds = [(0xC0, 0xFF2C, 1, 1), (0xC0, 0xFF4A, 1, 1), (0xC0, 0xFF63, 1, 1)]
    for off in range(0x20000 - len(PROLOGUE)):
        if data[off:off + len(PROLOGUE)] == PROLOGUE:
            seeds.append((0xC0 + (off >> 16), off & 0xFFFF, 0, 0))

    seen, covered = set(), set()
    far_calls, far_data = collections.Counter(), collections.Counter()
    work = list(seeds)
    while work:
        bank, pc, m, x = work.pop()
        while bank in ROM_BANKS and (bank, pc, m, x) not in seen:
            seen.add((bank, pc, m, x))
            off = ((bank & 0x0F) << 16) | pc
            mn, mode, n, arg, new_m, new_x = decode(data, off, m, x)
            if mn in ("BRK", "COP", "WDM", "STP"):
                break
            covered.update(range(off, off + n))
            nxt = (pc + n) & 0xFFFF
            if mode in ("al", "alx"):
                tbank, taddr = arg >> 16, arg & 0xFFFF
                if mn in ("JSL", "JML"):
                    if tbank in ROM_BANKS:
                        work.append((tbank, taddr, new_m, new_x))
                    else:
                        far_calls[f"{mn} ${tbank:02X}:{taddr:04X}"] += 1
                    if mn == "JML":
                        break
                elif not is_ordinary_data(tbank, taddr):
                    far_data[f"${tbank:02X}:{taddr:04X}"] += 1
            elif mn == "JSR" and mode == "a":
                work.append((bank, arg, new_m, new_x))
            elif mn == "JMP" and mode == "a":
                work.append((bank, arg, new_m, new_x))
                break
            elif mn in ("JMP", "JML") and mode in ("ai", "axi", "ail"):
                break
            elif mn == "BRL":
                work.append((bank, (nxt + ((arg ^ 0x8000) - 0x8000)) & 0xFFFF, new_m, new_x))
                break
            elif mode == "r":
                target = (nxt + ((arg ^ 0x80) - 0x80)) & 0xFFFF
                work.append((bank, target, new_m, new_x))
                if mn == "BRA":
                    break
            elif mn in ("RTS", "RTL", "RTI"):
                break
            pc, m, x = nxt, new_m, new_x
    return covered, far_calls, far_data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("episode", choices=("zen", "chu", "kou"))
    ap.add_argument("--dir", default=INPUTS, help="folder holding the packs")
    args = ap.parse_args()
    data = load_pack(args.episode, args.dir)
    covered, far_calls, far_data = trace(data)
    print(f"{args.episode}: traced {len(covered)} code bytes")
    print("calls outside the pack:", dict(far_calls) or "none")
    print("data accesses outside WRAM, standard registers and the pack:")
    for name, count in sorted(far_data.items()):
        print(f"  {name} x{count}")


if __name__ == "__main__":
    main()
