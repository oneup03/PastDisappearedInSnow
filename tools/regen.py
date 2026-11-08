#!/usr/bin/env python3
"""Generate the recompiled code from your own verified images.

usage: tools/regen.py [--rom images/zen.sfc] [--no-verify] [--cfg-roots] [--zen-only | --parts-only]

Zenpen is the stock module, in src/gen/zen; Chuuhen and Kouhen are program modules with
their own prefixes, in src/gen/chu and src/gen/kou, from images/chu.sfc and images/kou.sfc.
All of it is derived from the ROMs, so it stays on your machine and is never committed.
Plain Python, so it runs the same on Windows without a Unix shell.
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FRAMEWORK = ROOT / "snesrecomp"
CLI = FRAMEWORK / "snesrecomp_cli.py"


def run(*command, env=None, capture=False):
    command = [str(part) for part in command]
    if not capture:
        print("==", " ".join(command), flush=True)
    done = subprocess.run(command, cwd=ROOT, check=True, env=env, text=capture,
                          stdout=subprocess.PIPE if capture else None)
    return done.stdout.strip() if capture else None


def identity(field):
    return run(sys.executable, FRAMEWORK / "tools" / "rom_identity.py", ROOT / "rom_identity.txt",
               "--get", field, capture=True)


def zen(rom, verify, cfg_roots):
    # The framework owns the search: --rom, then SNESRECOMP_ROM, then a known name at the
    # root, then rom.cfg (the path the setup wizard recorded).
    resolve = [sys.executable, CLI, "resolve-rom", "--project-root", ROOT,
               "--name", "zen.sfc", "--name", "bstanteiclub.sfc", "--name", "bstanteiclub.smc"]
    if rom:
        resolve += ["--rom", rom]
    try:
        rom = run(*resolve, capture=True)
    except subprocess.CalledProcessError:
        sys.exit("regen: pass --rom path/to/zen.sfc, set SNESRECOMP_ROM, or run tools/build_image.py first")
    checks = []
    if verify:
        crc32 = os.environ.get("SNESRECOMP_EXPECTED_CRC32") or identity("expected_crc32")
        sha256 = os.environ.get("SNESRECOMP_EXPECTED_SHA256") or identity("expected_sha256")
        checks = ["--expected-crc32", crc32, "--expected-sha256", sha256]
        run(sys.executable, CLI, "verify-rom", "--rom", rom, *checks)
    generate = [sys.executable, CLI, "generate", "--rom", rom, "--cfg-dir", "recomp/zen",
                "--out-dir", "src/gen/zen", "--funcs-h", "recomp/zen/funcs.h", "--project-root", ROOT]
    if cfg_roots:
        generate.append("--cfg-roots")
    run(*generate, *checks)


def parts():
    env = dict(os.environ)
    paths = [str(FRAMEWORK / "tools"), str(FRAMEWORK / "recompiler")]
    if env.get("PYTHONPATH"):
        paths.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = os.pathsep.join(paths)
    tools = FRAMEWORK / "tools"
    for part in ("chu", "kou"):
        cfg = f"recomp/{part}"
        run(sys.executable, "-c", f"import pathlib, sync_symbols; sync_symbols.sync_symbols(pathlib.Path('{cfg}'))",
            env=env)
        run(sys.executable, tools / "v2_sync_funcs_h.py", "--cfg-dir", cfg, "--out", f"{cfg}/funcs.h", env=env)
        run(sys.executable, tools / "v2_emit.py", "--rom", f"images/{part}.sfc", "--cfg-dir", cfg,
            "--out-dir", f"src/gen/{part}", "--module-id", part, "--module-prefix", part,
            "--host-alias-header", f"{cfg}/funcs.h", env=env)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", help="Zenpen's image (default: images/zen.sfc, found by the framework)")
    ap.add_argument("--no-verify", action="store_true", help="skip the digest check on Zenpen's image")
    ap.add_argument("--cfg-roots", action="store_true", help="seed analysis from every func in recomp/zen/*.cfg")
    which = ap.add_mutually_exclusive_group()
    which.add_argument("--zen-only", action="store_true")
    which.add_argument("--parts-only", action="store_true")
    args = ap.parse_args()
    if not CLI.exists():
        sys.exit("regen: snesrecomp is missing; run: git submodule update --init --recursive")
    if not args.parts_only:
        zen(args.rom, not args.no_verify, args.cfg_roots)
    if not args.zen_only:
        parts()


if __name__ == "__main__":
    main()
