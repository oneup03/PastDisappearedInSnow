#!/usr/bin/env bash
# Run the Mesen2 reference emulator headless with a Lua script. Dev tool only:
# it is external, never linked, and needs the BS-X BIOS, so its output stays in
# the git-ignored local/ folder.
#
# usage: tools/mesen.sh <script.lua> <rom> [timeout seconds]
#
# Preloading libstdc++ works around a C++ runtime clash in Mesen 2.1.1's bundled
# core on current distros: without it, MesenCore.so aborts with std::bad_cast
# while building a static regex, before emulation starts.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESEN_DIR="$ROOT/local/tools/mesen"
script=$(realpath "$1"); rom=$(realpath "$2"); limit=${3:-60}
cd "$MESEN_DIR"
export HOME="$MESEN_DIR/home" XDG_CONFIG_HOME="$MESEN_DIR/home/.config"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export LD_PRELOAD=/usr/lib64/libstdc++.so.6
exec timeout $((limit + 15)) ./Mesen --testRunner "$script" "$rom" --timeout="$limit"
