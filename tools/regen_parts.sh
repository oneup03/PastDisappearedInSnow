#!/usr/bin/env bash
# Generate the program modules for the parts after Zenpen, which tools/regen.sh
# generates as the stock module. Output is ROM-derived and stays local.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
PYTHON=${PYTHON:-python3}
export PYTHONPATH="snesrecomp/tools:snesrecomp/recompiler${PYTHONPATH:+:$PYTHONPATH}"
for part in chu kou; do
  echo "== Generating src/gen/$part =="
  "$PYTHON" -c "import pathlib, sync_symbols; sync_symbols.sync_symbols(pathlib.Path('recomp/$part'))"
  "$PYTHON" snesrecomp/tools/v2_sync_funcs_h.py --cfg-dir "recomp/$part" --out "recomp/$part/funcs.h"
  "$PYTHON" snesrecomp/tools/v2_emit.py --rom "images/$part.sfc" --cfg-dir "recomp/$part" \
      --out-dir "src/gen/$part" --module-id "$part" --module-prefix "$part" \
      --host-alias-header "recomp/$part/funcs.h"
done
