#!/usr/bin/env python3
"""Rough BS-X dependency scan for a Satellaview memory-pack image.

Byte-pattern scan (not a real disassembly): lists candidate instructions that
touch the BS-X base unit ($2188-$219F), the BS-X cart MCC ($5000-$5FFF), or
JSL/JML out of the pack. Confirm hits with a Mesen2 trace before trusting them.

usage: python3 -I bsx_scan.py <pack.sfc> [--lorom|--hirom]
"""
import sys
from collections import Counter

# opcodes taking a 16-bit absolute operand that reads/writes memory
ABS = {0xAD, 0xAE, 0xAC, 0x8D, 0x8E, 0x8C, 0x9C, 0x2C, 0x2D, 0x0D, 0x4D,
       0x6D, 0xED, 0xCD, 0xEC, 0xCC, 0xEE, 0xCE, 0x0E, 0x4E, 0x2E, 0x6E,
       0x0C, 0x1C, 0xBD, 0xB9, 0x9D, 0x99, 0xBE, 0xBC, 0x9E, 0x3C, 0x3D,
       0x39, 0x1D, 0x19, 0x5D, 0x59, 0x7D, 0x79, 0xFD, 0xF9, 0xDD, 0xD9,
       0xFE, 0xDE, 0x1E, 0x5E, 0x3E, 0x7E}
# opcodes taking a 24-bit long operand
LONG = {0xAF, 0x8F, 0xBF, 0x9F, 0x0F, 0x1F, 0x2F, 0x3F, 0x4F, 0x5F,
        0x6F, 0x7F, 0xCF, 0xDF, 0xEF, 0xFF}
JSL, JML = 0x22, 0x5C


def classify(addr16, bank=None):
    lowbank = bank is None or (bank & 0x7F) < 0x40
    if lowbank and 0x2188 <= addr16 <= 0x219F:
        return "base-unit"
    if lowbank and 0x5000 <= addr16 <= 0x5FFF and (bank is None or (bank & 0x7F) < 0x10):
        return "mcc"
    return None


def pack_banks(size, hirom):
    if hirom:
        return {0xC0 + i for i in range(size // 0x10000)} | {0x40 + i for i in range(size // 0x10000)}
    n = size // 0x8000
    return {i for i in range(n)} | {0x80 + i for i in range(n)}


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    if len(data) % 0x8000 == 0x200:
        data = data[0x200:]
    hirom = "--hirom" in sys.argv
    if "--lorom" not in sys.argv and not hirom:
        # BS header map-mode byte sits at $xFD8 relative to $xFB0 header base
        hirom = len(data) > 0x10000 and (data[0xFFD8] & 0x01) == 1 and data[0x7FD8] & 0x01 == 0
    banks = pack_banks(len(data), hirom)
    hits, far = [], Counter()
    for i in range(len(data) - 3):
        op = data[i]
        if op in ABS:
            kind = classify(data[i + 1] | data[i + 2] << 8)
            if kind:
                hits.append((i, kind, f"{op:02X} ${data[i+2]:02X}{data[i+1]:02X}"))
        elif op in LONG or op in (JSL, JML):
            a16 = data[i + 1] | data[i + 2] << 8
            bank = data[i + 3]
            kind = classify(a16, bank)
            if kind:
                hits.append((i, kind, f"{op:02X} ${bank:02X}:{a16:04X}"))
            if op in (JSL, JML) and bank not in banks and a16 >= 0x8000:
                far[bank] += 1
    print(f"{path}: {len(data)} bytes, assuming {'HiROM' if hirom else 'LoROM'} layout")
    by = Counter(k for _, k, _ in hits)
    print("candidate hardware accesses:", dict(by) or "none")
    for off, kind, txt in hits[:200]:
        print(f"  file+{off:06X}  {kind:9s} {txt}")
    print("JSL/JML into banks outside the pack (possible BIOS calls):",
          ", ".join(f"${b:02X}x{c}" for b, c in sorted(far.items())) or "none")


if __name__ == "__main__":
    main()
