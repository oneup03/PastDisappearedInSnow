#!/usr/bin/env python3
"""Cut a language's font down to the characters the port uses, so the repo carries a small
font rather than a whole one.

usage: tools/subset_font.py FULL.bdf [--lang zh-Hans]

Run it after a translation gains a character that `script.py lint --lang <lang>` reports as
having no glyph. FULL.bdf is the whole font the language's current one was cut from; for
zh-Hans that's fusion-pixel-12px-monospaced-zh_hans.bdf from the 12px monospaced BDF zip of
https://github.com/TakWolf/fusion-pixel-font/releases (SIL Open Font License, text in
assets/fonts/fusion-pixel-OFL.txt).

It keeps printable ASCII, the common punctuation blocks, every character in the language's
translation/<lang>/ files, and every UTF-8 string written into src/*.c, such as the title
screen's. The result replaces the file the language's meta.toml names.
"""
import argparse
import pathlib
import re
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ALWAYS = [range(0x20, 0x7F), range(0x2010, 0x2028), range(0x3000, 0x3040), range(0xFF01, 0xFF5F)]
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPE = re.compile(rb"\\(x[0-9a-fA-F]{1,2}|.)")


def literal_bytes(body):
    def one(match):
        code = match.group(1)
        if code[:1] == b"x":
            return bytes([int(code[1:], 16)])
        return {b"n": b"\n", b"t": b"\t", b"0": b"\0"}.get(code, code)
    return ESCAPE.sub(one, body.encode("utf-8"))


def source_characters():
    """Characters in src/*.c string literals that decode as UTF-8, joining literals that
    follow one another (a string split across lines) before decoding."""
    found = set()
    for path in sorted((ROOT / "src").glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        run, end = b"", None
        for match in LITERAL.finditer(text):
            if end is not None and text[end:match.start()].strip():
                found |= decode(run)
                run = b""
            run += literal_bytes(match.group(1))
            end = match.end()
        found |= decode(run)
    return found


def decode(data):
    try:
        return set(data.decode("utf-8"))
    except UnicodeDecodeError:
        return set()  # Shift-JIS, for the Japanese screens


def wanted(lang):
    codes = {code for block in ALWAYS for code in block}
    for path in (ROOT / "translation" / lang).iterdir():
        if path.suffix in (".tsv", ".txt", ".toml"):
            codes |= {ord(c) for c in path.read_text(encoding="utf-8")}
    codes |= {ord(c) for c in source_characters()}
    return codes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("full", help="the whole BDF font")
    ap.add_argument("--lang", default="zh-Hans")
    args = ap.parse_args()
    meta = tomllib.loads((ROOT / "translation" / args.lang / "meta.toml").read_text(encoding="utf-8"))
    if "font" not in meta:
        sys.exit(f"translation/{args.lang}/meta.toml names no font")
    out = ROOT / meta["font"]
    codes = wanted(args.lang)

    head, glyphs, default = [], [], None
    lines = pathlib.Path(args.full).read_text(encoding="utf-8").splitlines(keepends=True)
    current = None
    for line in lines:
        if line.startswith("DEFAULT_CHAR "):
            default = int(line.split()[1])
        if line.startswith("STARTCHAR"):
            current = [line]
        elif current is not None:
            current.append(line)
            if line.startswith("ENDCHAR"):
                glyphs.append(current)
                current = None
        elif not line.startswith(("CHARS ", "ENDFONT")):
            head.append(line)
    kept = []
    for glyph in glyphs:
        code = next(int(l.split()[1]) for l in glyph if l.startswith("ENCODING "))
        if code in codes or code == default:
            kept.append(glyph)
    have = {int(l.split()[1]) for g in kept for l in g if l.startswith("ENCODING ")}
    always = {code for block in ALWAYS for code in block}
    lacking = sorted(chr(c) for c in codes - have - always if chr(c).isprintable() and not chr(c).isspace())
    note = (f"COMMENT Cut down by tools/subset_font.py to the {len(kept)} characters this port "
            "uses, from the font named below.\n")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(head[0] + note + "".join(head[1:]))
        f.write(f"CHARS {len(kept)}\n")
        for glyph in kept:
            f.write("".join(glyph))
        f.write("ENDFONT\n")
    print(f"wrote {len(kept)} of {len(glyphs)} glyphs to {out.relative_to(ROOT)}")
    if lacking:
        print(f"the full font lacks: {''.join(lacking)}")


if __name__ == "__main__":
    main()
