#!/usr/bin/env python3
"""Build the game from your own copies of the episode packs.

usage: tools/setup.py [--packs FOLDER] [--ad IMAGE] [--no-build]

Verifies the three packs, builds their images, generates the
recompiled code and compiles it. Everything it makes stays on your machine: none of it
is in the repo, or may be shared. Needs Python 3, Rust, CMake and a C compiler: on Linux,
gcc or clang with Ninja; on Windows, Visual Studio 2022's C++ tools, run from its Developer
PowerShell. Recordings are optional, and are picked up at launch from inputs/ or from images/.
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
    run(python, "tools/regen.py", "--rom", "images/zen.sfc")
    if args.no_build:
        return
    if not shutil.which("cmake"):
        sys.exit("CMake is needed to build (on Windows, open the Developer PowerShell for VS 2022)")
    # Ninja where there is one, which Visual Studio's Developer PowerShell has too; otherwise
    # CMake's default, which on Windows is the Visual Studio generator.
    generator = ["-G", "Ninja"] if shutil.which("ninja") else []
    run("cmake", "-S", ".", "-B", "build", *generator, "-DCMAKE_BUILD_TYPE=Release")
    run("cmake", "--build", "build", "--config", "Release", "--parallel")
    exe = "BsTanteiClubSNESRecomp.exe" if sys.platform == "win32" else "BsTanteiClubSNESRecomp"
    found = next((p for p in (ROOT / "build" / exe, ROOT / "build" / "Release" / exe) if p.exists()), None)
    shown = found.relative_to(ROOT) if found else pathlib.Path("build") / exe
    print(f"Done. Run: {shown} --launcher --rom {pathlib.Path('images') / 'zen.sfc'}")


if __name__ == "__main__":
    main()
