#!/usr/bin/env bash
# Regenerate src/gen/zen from a verified image. tools/regen.py does the work, so Windows
# needs no shell; this stays for the framework's convention. Flags: --rom <path>,
# --no-verify, --cfg-roots. Generated C is ROM-derived and never committed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${PYTHON:-python3}" "$ROOT/tools/regen.py" --zen-only "$@"
