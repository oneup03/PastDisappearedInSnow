"""Decode the game's scene bytecode.

Scene scripts are plain Shift-JIS text mixed with opcodes below $20. The engine
walks each scene linearly when it starts ($C0:8444, table $C0:79F4) to find its
timed events and action labels, so the same linear walk decodes a whole scene.
Operand sizes below come from that table.
"""
import dataclasses
import pathlib
import re
import zlib

OP_RETURN = 0x00
OP_RETURN_MENU = 0x01
OP_END = 0x02
OP_DEADLINE = 0x03
OP_EVENT = 0x04
OP_LABEL = 0x05
OP_STRCPY = 0x06
OP_SYNC_CLOCK = 0x07
OP_NEWLINE = 0x0A
OP_CLEAR = 0x0B
OP_SPEED = 0x0C
OP_WAIT = 0x0E
OP_NEXT_EVENT = 0x0F
OP_SET = 0x10
OP_GOTO = 0x11
OP_IF_NE = 0x12
OP_IDLE = 0x13
OP_MENU = 0x14
OP_SCENE = 0x15

OPERAND_SIZE = {
    0x00: 0, 0x01: 0, 0x02: 0, 0x03: 2, 0x04: 2, 0x05: 3, 0x07: 2, 0x08: 0,
    0x09: 0, 0x0A: 0, 0x0B: 0, 0x0C: 2, 0x0D: 0, 0x0E: 2, 0x0F: 2, 0x10: 3,
    0x11: 2, 0x12: 5, 0x13: 0, 0x15: 1, 0x16: 4, 0x17: 6, 0x18: 6, 0x19: 3,
    0x1A: 4, 0x1B: 6, 0x1C: 1, 0x1D: 1, 0x1E: 0, 0x1F: 0,
}

MENU_ENTRY_SIZE = 19
SCHEDULE_TABLE = 0x2001C  # $C2:001C


@dataclasses.dataclass
class Op:
    addr: int  # file offset
    op: int
    args: bytes = b""


@dataclasses.dataclass
class Text:
    addr: int
    raw: bytes

    @property
    def text(self):
        return self.raw.decode("shift_jis", "replace")


def u16(data, off):
    return data[off] | data[off + 1] << 8


def walk(data, start):
    """Yield the Ops and Text runs of one scene, ending at OP_END."""
    pc = start
    while True:
        byte = data[pc]
        if byte >= 0x20:
            run = pc
            while data[pc] >= 0x20:
                pc += 2 if data[pc] >= 0x80 and not 0xA0 <= data[pc] < 0xE0 else 1
            yield Text(run, bytes(data[run:pc]))
            continue
        size = OPERAND_SIZE.get(byte)
        if byte == 0x06:
            end = data.index(0, pc + 4)
            size = end + 1 - (pc + 1)
        elif byte == OP_MENU:
            size = 1 + data[pc + 1] * MENU_ENTRY_SIZE
        yield Op(pc, byte, bytes(data[pc + 1:pc + 1 + size]))
        pc += 1 + size
        if byte == OP_END:
            return


def schedule(data):
    """Return (file offset, broadcast second) for each scene, by scene number.

    Scene 0 runs once at boot: it fills in the speaker names and holds the actions
    common to every scene. Its broadcast second is None.
    """
    scenes = []
    off = SCHEDULE_TABLE
    while u16(data, off) != 0xFFFF:
        target = (data[off] | data[off + 1] << 8 | data[off + 2] << 16) & 0x0FFFFF
        is_timed = data[target + 3] == OP_SYNC_CLOCK
        scenes.append((target, u16(data, target + 4) if is_timed else None))
        off += 3
    return scenes


@dataclasses.dataclass
class Message:
    addr: int  # file offset of the first text
    scene: int
    speaker: str
    kind: str  # "text", "menu" or "string"
    text: str


# Opcodes that can sit inside a message. Anything else ends it. Waits, speed and
# speaker changes stay in the script at their own addresses, so each shows up as a
# tag a translation has to keep. Newlines don't: a translation wraps as it likes.
OP_SPEAKER = 0x19
OP_SE = 0x1D
INLINE = {OP_NEWLINE, OP_WAIT, OP_SPEED, OP_SPEAKER, OP_SE, 0x1A}
WAIT_FOR_BUTTON = 0xFFFF


def tag(op):
    if op.op == OP_NEWLINE:
        return "{n}"
    if op.op == OP_WAIT:
        frames = u16(op.args, 0)
        return "{w}" if frames == WAIT_FOR_BUTTON else f"{{w:{frames}}}"
    if op.op == OP_SPEED:
        return f"{{s:{u16(op.args, 0)}}}"
    if op.op == OP_SE:
        return f"{{se:{op.args[0]}}}"
    if op.op == 0x1A:
        return "{x:" + op.args.hex() + "}"
    return ""


def speaker_ptr(op):
    return op.args[0] | op.args[1] << 8 | op.args[2] << 16


def speaker_names(data):
    """Map speaker record addresses to names, from the strings scene 0 copies."""
    names = {}
    for item in walk(data, schedule(data)[0][0]):
        if isinstance(item, Op) and item.op == OP_STRCPY:
            name = bytes(item.args[3:-1]).decode("shift_jis", "replace").strip("\u3000「")
            names[speaker_ptr(item)] = name
    return names


KEPT_TAG = re.compile(r"\{(?!n\}|c\})[^}]*\}")


def kept_tags(text):
    """The tags a translation must repeat, in order. {n} and {c} are its own business."""
    return KEPT_TAG.findall(text)


def source_hash(text):
    """Short hash of a source string, to spot a translation that no longer matches."""
    return f"{zlib.crc32(text.encode('utf-8')) & 0xFFFF:04x}"


# Text outside the scene scripts: status, time check, password, best score, signal
# error, then the credits pages. Found by hand; these screens' code uses their own
# window records. File offsets, by pack.
SCREEN_STRINGS = {
    "zen": (0x2CBDA, 0x2CC0F, 0x2D8CD, 0x2D8E7, 0x2D913),
    "chu": (0x295E8, 0x2961D, 0x2A2DB, 0x2A2F5, 0x2A321),
    "kou": (0x2D3DF, 0x2D414, 0x2E0D2, 0x2E0EC, 0x2E118),
}
CREDITS = {"zen": 0x2CCBB, "chu": 0x296C9, "kou": 0x2D4C2}
CREDIT_PAGES = 14


def screen_rows(data, episode):
    """Rows for the fixed-layout screens: these get translated in place."""
    rows = []
    addrs = list(SCREEN_STRINGS[episode])
    addr = CREDITS[episode]
    for _ in range(CREDIT_PAGES):
        addrs.append(addr)
        addr = data.index(0, addr) + 1
    for addr in addrs:
        raw = bytes(data[addr:data.index(0, addr)])
        # The last credits page has a logo in special tiles, $F0 and a tile number.
        raw = re.sub(rb"(?:\xF0[\x00-\x3F])+", lambda m: b"{x:" + m[0].hex().upper().encode() + b"}", raw)
        text = raw.decode("shift_jis", "replace").replace("\n", "{n}")
        text = re.sub(r"[\x00-\x1f\x7f-\x9f]", lambda m: f"\\x{ord(m[0]):02x}", text)
        rows.append(Message(addr, -1, "", "screen", text))
    return rows


def messages(data):
    """Return every translatable text in the scenes, in script order."""
    rows = []
    names = speaker_names(data)
    for number, (addr, _) in enumerate(schedule(data)):
        parts, first, speaker, has_text = [], None, "", False

        def flush():
            nonlocal parts, first, has_text
            if first is not None and has_text:
                rows.append(Message(first, number, speaker, "text", "".join(parts).rstrip("\u3000")))
            parts, first, has_text = [], None, False

        for item in walk(data, addr):
            if isinstance(item, Text):
                if first is None:
                    first = item.addr
                parts.append(item.text)
                has_text = True
            elif item.op in INLINE:
                if item.op == OP_SPEAKER:
                    ptr = speaker_ptr(item)
                    if ptr in names:
                        speaker = names[ptr]
                elif first is not None:
                    parts.append(tag(item))
            elif item.op == OP_STRCPY:
                flush()
                text = bytes(item.args[3:-1]).decode("shift_jis", "replace")
                rows.append(Message(item.addr + 4, number, "", "string", text))
                # A scene can rename a speaker, for those lines that follow it.
                names[speaker_ptr(item)] = text.strip("\u3000「")
            elif item.op == OP_MENU:
                flush()
                for i in range(item.args[0]):
                    entry = item.args[1 + i * MENU_ENTRY_SIZE:1 + (i + 1) * MENU_ENTRY_SIZE]
                    if entry[0] == 0xFF:
                        continue
                    label = bytes(entry[3:]).decode("shift_jis", "replace").rstrip("\x00 \u3000")
                    rows.append(Message(item.addr + 2 + i * MENU_ENTRY_SIZE + 3, number, "", "menu", label))
            else:
                flush()
        flush()
    return rows


# Text layout, as src/text.c does it.
CELL_PIXELS = 6
WINDOW_COLS = 38
WINDOW_ROWS = 3
NAME_MARGIN_PX = 12
MENU_LABEL_BYTES = 16
LETTER_GAP = 1


def load_font(path):
    """Read a font's glyph widths, by code point: an advance for the port's own text font,
    or (width, advance) for a BDF font, whose glyphs carry their own spacing."""
    path = pathlib.Path(path)
    if path.suffix == ".bdf":
        return load_bdf_widths(path)
    widths, code, width = {}, None, 0
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("U+"):
            if code is not None:
                widths[code] = width
            code, width = int(line[2:], 16), 0
        elif line and line[0] in "#." and code is not None:
            width = max(width, len(line))
    if code is not None:
        widths[code] = width
    return {c: w + LETTER_GAP for c, w in widths.items()}


def load_bdf_widths(path):
    widths, code = {}, None
    with open(path, encoding="latin-1") as f:
        for line in f:
            if line.startswith("ENCODING "):
                code = int(line.split()[1])
            elif line.startswith("DWIDTH ") and code is not None and code >= 0:
                advance = int(line.split()[1])
                widths[code] = (advance, advance)
    return widths


def metrics(font, ch):
    """(width, advance) of a character, falling back to "?" as the game does."""
    value = font.get(ord(ch), font.get(ord("?"), 5))
    return value if isinstance(value, tuple) else (value - LETTER_GAP, value)


# Line breaking for text with no spaces, such as Chinese: a row may break between two
# characters when either is wide, unless that would start a row with closing punctuation
# or end one with opening punctuation. src/text.c follows the same rules.
NO_ROW_START = set("，。、：；！？）」』”’…—·～〕】》〉%,.!?:;)]}")
NO_ROW_END = set("（「『“‘〔【《〈([{")


def is_wide(ch):
    return ord(ch) >= 0x2E80 or ch in "…—“”‘’·"


def can_break(before, after):
    """Whether a row may end between two characters that have no space between them."""
    if before in " \0" or after == " ":
        return False
    if after in NO_ROW_START or before in NO_ROW_END:
        return False
    return is_wide(before) or is_wide(after)


def layout_rows(text, font, first_px=NAME_MARGIN_PX, margin=NAME_MARGIN_PX, width=WINDOW_COLS * CELL_PIXELS):
    """How many window rows a translated message's longest page takes, and any characters
    with no glyph. This walks the text as src/text.c draws it."""
    if "{c}" in text:
        pages = [layout_rows(page, font, first_px, margin, width) for page in text.split("{c}")]
        return max(page[0] for page in pages), set().union(*(page[1] for page in pages))
    # Each stretch between kept tags is drawn as its own string, so each starts a word.
    text = KEPT_TAG.sub("\0", text).replace("{n}", "\n")
    missing = set(ch for ch in text if ch not in "\0\n" and ord(ch) not in font)

    def word_start(i):
        return i == 0 or text[i - 1] in " \0\n" or can_break(text[i - 1], text[i])

    def word_width(i):
        total = 0
        while i < len(text) and text[i] not in " \0\n":
            total += metrics(font, text[i])[1]
            i += 1
            if i < len(text) and can_break(text[i - 1], text[i]):
                break
        return total

    rows, px, wrapped = 1, first_px, False
    for i, ch in enumerate(text):
        if ch == "\0":
            continue
        if ch == "\n":
            rows, px, wrapped = rows + 1, margin, True
            continue
        if ch == " " and wrapped:
            continue
        if word_start(i) and ch != " " and not wrapped and px + word_width(i) > width:
            rows, px, wrapped = rows + 1, margin, True
        glyph_width, advance = metrics(font, ch)
        if ch == " " and px + glyph_width > width:
            rows, px, wrapped = rows + 1, margin, True
            continue
        if px + glyph_width > width:
            rows, px = rows + 1, margin
        px += advance
        wrapped = False
    return rows, missing
