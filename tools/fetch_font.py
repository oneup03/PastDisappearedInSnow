#!/usr/bin/env python3
"""Fetch the bitmap fonts: Shinonome for Japanese, Fusion Pixel for Chinese.

usage: tools/fetch_font.py [--archive shinonome-0.9.11p1.tar.bz2] [--fusion-archive FILE]

Shinonome is public domain. Fusion Pixel is under the SIL Open Font License, whose text
comes with it. Each comes from a published archive, checked against a pinned hash, and is
kept out of git as a dependency rather than copied in.
"""
import argparse
import hashlib
import io
import pathlib
import tarfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "fonts"
URL = "https://ftp.netbsd.org/pub/pkgsrc/distfiles/shinonome-0.9.11p1.tar.bz2"
SHA256 = "95663c95c92ba5765f63ccbdf033eb93b707be01812a989c548db943479c838f"
FILES = ("shnmk12.bdf", "shnm6x12a.bdf", "shnm6x12r.bdf")
FUSION_URL = ("https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.10.25/"
              "fusion-pixel-font-12px-monospaced-bdf-v2026.10.25.zip")
FUSION_SHA256 = "d8788cf5ffd6a2dabec397c33c878261195fb9146cdb67d8afd418fe73db15bd"
FUSION_FILES = {
    "fusion-pixel-12px-monospaced-zh_hans.bdf": "fusion-pixel-12px-monospaced-zh_hans.bdf",
    "OFL.txt": "fusion-pixel-OFL.txt",
}


def fetch(url, archive, sha256):
    if archive:
        data = pathlib.Path(archive).read_bytes()
    else:
        print(f"downloading {url}")
        with urllib.request.urlopen(url) as response:
            data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != sha256:
        raise SystemExit(f"archive hash is {digest}, expected {sha256}")
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--archive", help="use a copy of Shinonome you already have")
    ap.add_argument("--fusion-archive", help="use a copy of Fusion Pixel you already have")
    args = ap.parse_args()
    data = fetch(URL, args.archive, SHA256)
    OUT.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:bz2") as archive:
        for name in FILES:
            member = archive.extractfile(f"shinonome-0.9.11/bdf/{name}")
            (OUT / name).write_bytes(member.read())
            print(f"wrote {OUT / name}")
    data = fetch(FUSION_URL, args.fusion_archive, FUSION_SHA256)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for name, out in FUSION_FILES.items():
            (OUT / out).write_bytes(archive.read(name))
            print(f"wrote {OUT / out}")


if __name__ == "__main__":
    main()
