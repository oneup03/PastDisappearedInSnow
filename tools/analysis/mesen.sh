#!/usr/bin/env bash
# Run the Mesen2 reference emulator headless with a Lua script. Dev tool only:
# it is external, never linked, and needs the BS-X BIOS, so its output stays in
# the git-ignored local/ folder.
#
# usage: tools/analysis/mesen.sh <script.lua> <rom> [timeout seconds]
#
# PVS_BSX_TIME sets the broadcast clock at power-on (default 18:00:00). The game
# joins the broadcast wherever the clock says, so a later time reaches later scenes.
#
# Preloading libstdc++ works around a C++ runtime clash in Mesen 2.1.1's bundled
# core on current distros: without it, MesenCore.so aborts with std::bad_cast
# while building a static regex, before emulation starts.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MESEN_DIR="$ROOT/local/tools/mesen"
script=$(realpath "$1"); rom=$(realpath "$2"); limit=${3:-60}
CONFIG="$MESEN_DIR/home/.config/Mesen2"
mkdir -p "$CONFIG/Firmware"
[ -f "$CONFIG/Firmware/BS-X.bin" ] || cp "$ROOT/inputs/BS-X.bin" "$CONFIG/Firmware/BS-X.bin"
# A missing settings.json makes Mesen open its setup wizard and hang.
cat > "$CONFIG/settings.json" <<JSON
{
  "Snes": { "BsxUseCustomTime": true, "BsxCustomDate": "1997-02-09T00:00:00+00:00", "BsxCustomTime": "${PVS_BSX_TIME:-18:00:00}" },
  "Debug": { "ScriptWindow": { "AllowIoOsAccess": true, "ScriptTimeout": 5 } }
}
JSON
cd "$MESEN_DIR"
export HOME="$MESEN_DIR/home" XDG_CONFIG_HOME="$MESEN_DIR/home/.config"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export LD_PRELOAD=/usr/lib64/libstdc++.so.6
exec timeout $((limit + 15)) ./Mesen --testRunner "$script" "$rom" --timeout="$limit"
