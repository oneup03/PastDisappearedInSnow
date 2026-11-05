# recomp/

Analysis input for BS Tantei Club, a folder per part: `zen/`, `chu/` and `kou/`.
`tools/regen.sh` reads `zen/` and writes `src/gen/zen/`, the stock module.
`tools/regen_parts.sh` reads `chu/` and `kou/` and writes `src/gen/chu/` and
`src/gen/kou/`.

| File | Owner | Notes |
|---|---|---|
| `bank*.cfg` | you, plus generated blocks | Per-bank analysis directives |
| `symbols.toml` | you | Progressive name map; the source of truth |
| `funcs.h` | generated | Re-synced by regeneration — do not hand-edit |

The block between `>>> BEGIN symbols.toml` and `<<< END symbols.toml` in
each bank cfg is rewritten from `symbols.toml` on every regen. Edit
`symbols.toml`, not the block.

Python 3.11+ includes the TOML reader. On older Python, install it with
`python -m pip install tomli`. Projects without `symbols.toml` do not need it.

Each `[[func]]` has a C identifier `name`, a 16-bit hexadecimal `addr`
(for example, `"8000"`), and an integer `bank` (for example, `0xc0`).
Missing bank configs are created automatically; hand-written directives outside
the generated blocks are preserved. Removing an entry also removes its generated
directives on the next regen.

- `emit = false` (the default) writes a `symbol` label and `force_lle` boundary:
  it stays interpreted, with no generated `func` declaration in `funcs.h`.
- `emit = true` writes a `func` declaration and requests analysis from that
  entry, even without `--cfg-roots`. Only variants the analyzer proves safe
  become AOT code; others still use the interpreter. Optional `entry_m` and
  `entry_x` select the initial register-width flags (0 or 1, both default to 1).

An unchanged cfg file is not by itself evidence of failed code generation:
`src/gen/zen/bank*_v2.c` contains code discovered from the ROM, while `bank*.cfg`
contains analysis inputs. Discovery does not copy every reached function back
into the configs.
