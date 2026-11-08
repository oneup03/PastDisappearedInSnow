#!/usr/bin/env python3
"""Build the game from your own copies of the episode packs.

usage: tools/setup.py [--packs FOLDER] [--ad IMAGE] [--no-build]

Verifies the three packs, builds their images, generates the
recompiled code and compiles it. Everything it makes stays on your machine: none of it
is in the repo, or may be shared. Needs Python 3, Rust, CMake, Ninja and a C compiler.
Recordings are optional, and are picked up at launch from inputs/ or from images/.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def run(*command):
    print("==", " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--packs", default="inputs", help="folder holding the three packs")
    ap.add_argument("--ad", help="a scan of the 1997 ad, for the launcher's box art")
    ap.add_argument("--no-build", action="store_true", help="generate the code, but don't compile it")
    args = ap.parse_args()
    python = sys.executable
    if not (ROOT / "snesrecomp" / "runner" / "runner.cmake").exists():
        run("git", "submodule", "update", "--init", "--recursive")
    run(python, "tools/build_image.py", "--dir", args.packs)
    if args.ad:
        run(python, "tools/make_boxart.py", args.ad)
    bash = shutil.which("bash")
    if not bash:
        sys.exit("bash is needed to generate the code (on Windows, Git Bash has it)")
    run(bash, "tools/regen.sh", "--rom", "images/zen.sfc")
    run(bash, "tools/regen_parts.sh")
    if args.no_build:
        return
    run("cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release")
    run("cmake", "--build", "build")
    print("Done. Run: build/BsTanteiClubSNESRecomp --rom images/zen.sfc")


if __name__ == "__main__":
    main()
