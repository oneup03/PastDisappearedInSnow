#!/usr/bin/env bash
# Generate the program modules for Chuuhen and Kouhen; see tools/regen.py.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${PYTHON:-python3}" "$ROOT/tools/regen.py" --parts-only "$@"
