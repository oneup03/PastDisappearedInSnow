#!/usr/bin/env python3
"""Fetch the Shinonome bitmap font, used to draw Japanese text.

usage: tools/fetch_font.py [--archive shinonome-0.9.11p1.tar.bz2]

Shinonome is public domain. It comes from a published archive, checked against a
pinned hash, and is kept out of git as a dependency rather than copied in.
"""
import argparse
import hashlib
import io
import pathlib
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "fonts"
URL = "https://ftp.netbsd.org/pub/pkgsrc/distfiles/shinonome-0.9.11p1.tar.bz2"
SHA256 = "95663c95c92ba5765f63ccbdf033eb93b707be01812a989c548db943479c838f"
FILES = ("shnmk12.bdf", "shnm6x12a.bdf", "shnm6x12r.bdf")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--archive", help="use a copy you already have")
    args = ap.parse_args()
    if args.archive:
        data = pathlib.Path(args.archive).read_bytes()
    else:
        print(f"downloading {URL}")
        with urllib.request.urlopen(URL) as response:
            data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SHA256:
        raise SystemExit(f"archive hash is {digest}, expected {SHA256}")
    OUT.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:bz2") as archive:
        for name in FILES:
            member = archive.extractfile(f"shinonome-0.9.11/bdf/{name}")
            (OUT / name).write_bytes(member.read())
            print(f"wrote {OUT / name}")


if __name__ == "__main__":
    main()
