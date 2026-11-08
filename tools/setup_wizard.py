#!/usr/bin/env python3
"""What the launcher's first-run wizard runs in place of the framework's own generator.

The framework's wizard makes one game from one ROM; this game is three packs. The wizard
hands over the one file the player chose, so the packs are looked for beside it and in
inputs/, verified, built into images/ and generated from. Same command line and JSON progress
as `snesrecomp_cli.py generate`, as src/setup_wizard.c names this in its place.
"""
import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from pvs.roms import EPISODES, find_packs  # noqa: E402


def emit(event, **fields):
    print(json.dumps({"event": event, **fields}), flush=True)


def phase(name, pct, message):
    emit("phase", phase=name, pct=pct, message=message)


def fail(message, code=1):
    emit("error", message=message, code=code)
    print(message, file=sys.stderr, flush=True)
    sys.exit(code)


def run(*command):
    # Standard output is the launcher's progress stream, so the tools' chatter goes to stderr.
    return subprocess.run([sys.executable, *map(str, command)], cwd=ROOT, stdout=sys.stderr).returncode


def find_folder(chosen):
    """The folder holding all three packs, or None, and the parts missing from the last one."""
    missing = []
    for folder in (pathlib.Path(chosen).resolve().parent, ROOT / "inputs"):
        found, _ = find_packs(folder)
        missing = [e.name for e in EPISODES if e.key not in found]
        if not missing:
            return folder, []
    return None, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["generate"])
    ap.add_argument("--rom", required=True)
    # What the wizard passes for a one-ROM game. The layout here is fixed, so they're ignored.
    for name in ("--cfg-dir", "--out-dir", "--funcs-h", "--project-root", "--expected-crc32", "--expected-sha256"):
        ap.add_argument(name)
    ap.add_argument("--cfg-roots", action="store_true")
    ap.add_argument("--json-progress", action="store_true")
    args = ap.parse_args()

    phase("verify", 0.05, "Looking for the three episode packs")
    folder, missing = find_folder(args.rom)
    if folder:
        phase("verify", 0.1, "Building the game images")
        if run("tools/build_image.py", "--dir", folder, "--out", ROOT / "images"):
            fail("The packs were found, but their images could not be built. See the log.")
    elif not all((ROOT / "images" / f"{e.key}.sfc").exists() for e in EPISODES):
        fail(f"Missing {', '.join(missing)}. Extract all three episode packs (.bs files) "
             f"into the same folder, or into inputs/, then choose any one of them.", 3)
    phase("emit", 0.2, "Recompiling all three parts (this takes a few minutes)")
    if run("tools/regen.py", "--rom", ROOT / "images" / "zen.sfc"):
        fail("Generating the code failed. See the log.")
    phase("done", 1.0, "Generated")
    emit("result", ok=True, rom=str(ROOT / "images" / "zen.sfc"), out_dir=str(ROOT / "src" / "gen"))


if __name__ == "__main__":
    main()
