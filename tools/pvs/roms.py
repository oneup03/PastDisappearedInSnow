"""Locate and verify the player's episode packs.

Packs may be bare .bs files or the zipped No-Intro set. Only hashes live here;
the ROM data itself is never stored in the repo.
"""
import hashlib
import pathlib
import zipfile
import zlib
from dataclasses import dataclass


@dataclass(frozen=True)
class Episode:
    key: str
    name: str
    nointro: str
    crc32: str
    sha1: str


EPISODES = (
    Episode("zen", "Zenpen", "(Japan) (1998-12-29) (SoundLink)",
            "dc3354e8", "c082f21c81c0a3648d448e68c0fcf59af8e73a59"),
    Episode("chu", "Chuuhen", "(Japan) (1998-6-20) (SoundLink)",
            "67568b74", "e88c6f7c140c57d6388c18a4c2e9d241779800d1"),
    Episode("kou", "Kouhen", "(Japan) (Premiere) (1997-2-24) (SoundLink)",
            "834055a7", "d61bfb8118c647043525d7e848cc7c5464ce6c17"),
)

# Other No-Intro builds of this game: recognised so the error can name them.
KNOWN_UNSUPPORTED = {
    "e64ea557": "Chuuhen (1998-6-14)",
    "4ae1fdeb": "Kouhen (Premiere) (1997-2-23)",
    "78336cd9": "Kouhen (1997-5-1)",
    "b5fd446d": "Kouhen (1998-6-22)",
    "c8b081d9": "Kouhen (1998-6-25)",
}

EPISODE_BY_KEY = {e.key: e for e in EPISODES}
EPISODE_BY_SHA1 = {e.sha1: e for e in EPISODES}
PACK_SIZE = 0x100000


def _candidates(path):
    """Yield (label, bytes) for each possible pack at path."""
    path = pathlib.Path(path)
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as zf:
            for name in zf.namelist():
                if zf.getinfo(name).file_size == PACK_SIZE:
                    yield f"{path.name}:{name}", zf.read(name)
    elif path.is_file() and path.stat().st_size == PACK_SIZE:
        yield path.name, path.read_bytes()


def find_packs(search_dir="."):
    """Return ({key: bytes}, [messages]) for packs found in search_dir."""
    found, notes = {}, []
    root = pathlib.Path(search_dir)
    for path in sorted(root.iterdir()):
        if path.suffix.lower() not in (".zip", ".bs", ".sfc"):
            continue
        for label, data in _candidates(path):
            sha1 = hashlib.sha1(data).hexdigest()
            ep = EPISODE_BY_SHA1.get(sha1)
            if ep:
                found[ep.key] = data
                continue
            crc = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
            if crc in KNOWN_UNSUPPORTED:
                notes.append(f"{label}: {KNOWN_UNSUPPORTED[crc]} is a known build "
                             "but not supported yet")
    return found, notes


def load_pack(key, search_dir="."):
    """Return one verified pack, or raise with a message that says what's wrong."""
    found, notes = find_packs(search_dir)
    if key in found:
        return found[key]
    ep = EPISODE_BY_KEY[key]
    detail = "; ".join(notes)
    raise FileNotFoundError(
        f"{ep.name} {ep.nointro} (crc32 {ep.crc32}) not found in {search_dir}"
        + (f" ({detail})" if detail else ""))
