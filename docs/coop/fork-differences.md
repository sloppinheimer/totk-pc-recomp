# How this fork differs from upstream

This repository is a fork of [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) that adds
co-op. Upstream moves fast, so the fork keeps its footprint in upstream's files small: co-op code lives in co-op
files (`runtime/coop_*`, `runtime/ui/coop_*`, `tools/coop/`, `docs/coop/`), and an upstream file carries at most a
one-line call or `#include` of one. This note lists what the fork changes in upstream's files and why, so a merge
from upstream can check each point still holds.

## Behaviour that differs from upstream

- **Function-entry hook in every function.** Upstream's generator puts `FN_ENTER` only into the functions a patch
  table's `hook` lines name (on 1.4.3, one render-size function). The co-op hooks (`runtime/coop_hooks.h`
  `COOP_HOOKED`, `coop_spawn.c` `coop_hook`) and the co-op tracers run from `FN_ENTER` all over the game, so
  `recomp/patches-v143.txt` has no `hook` line: by upstream's own rule, every function then keeps `FN_ENTER`.
  After a merge, check that the lift still emits it everywhere (about 228,000 `FN_ENTER` lines in `generated/v143/main`).
- **`FN_ENTER` itself.** `runtime/coop_hooks.h`, included by `runtime/cpu.h` right after upstream's `FN_ENTER`,
  redefines it: the co-op hook test first, then `coop_fnprof_hit` (`coop_trace.c`), which calls upstream's
  `fnprof_hit` and the co-op readers of the entry registers.
- **RomFS manifest.** `tools/coop/make_puppet_pack.py` adds the stand-in's actor pack and its RSDB rows to
  `build/<ver>/romfs-patches/manifest.tsv`. `tools/make_romfs_patches.py` rewrites that manifest, so the full build
  runs `make_puppet_pack.py` after it, or no stand-in can be made.

## One-line hooks into upstream's files

| Upstream file | What it carries |
|---|---|
| `runtime/cpu.h` | `#include "coop_hooks.h"` |
| `runtime/cfg.c` | `#include "coop_cfg_names.inc"` in the list of setting names |
| `runtime/ui_text.h`, `runtime/ui_text.c` | `#include "coop_text_ids.inc"` and one `coop_text_<lang>.inc` per language |
| `runtime/ui/menu.cpp` | `#include "ui/coop_menu.h"`, the Co-op tab's entry, and calls into `ui/coop_menu.cpp` |
| `runtime/main.c` | `coop_hide_console()`, and `rt_lifted_function` (it needs main.c's static `lifted_function`) |
| `runtime/ov_input.c` | `coop_local_input(...)`: this player's input for the relay |
| `runtime/vk_exec_draw.c`, `runtime/vk_exec.c`, `runtime/gfx_present.c` | the name tags' draw, include and release |
| `tests/diff/harness.c` | a stub of `coop_fnprof_hit` |

## Larger changes still in upstream's files

- `runtime/romfs.c`, `runtime/restbl.c`, `runtime/fs.h`: files added while the game runs (a party member's character
  mod) and spare size-table entries. They use romfs.c's own tables, so they stay there.
- `runtime/input.c`, `runtime/input_win.c`, `runtime/input_sdl.c`, `runtime/ui/menu_win.cpp`, `runtime/ui/menu_sdl.cpp`:
  `TOTK_PAD_INDEX` (one game per gamepad), a test script that drives the menu, typed text and Ctrl+V in the menu's text
  boxes. General features, candidates for upstream pull requests.
- `runtime/boot.c`: `TOTK_SHOTS_CROP` and the tester's mark screenshot. General, candidates for upstream.

## Merging upstream

Merge often (weekly), never rebase (the fork is public). After a merge: the full build (`recomp/gen.py`,
`tools/make_romfs_patches.py`, `tools/coop/make_puppet_pack.py`, `tools/build_game.py`), the points above, and a
two-game check (`tools/coop/run_pair.ps1` with `tools/coop/idle.txt` for both games: each shows the other's stand-in).
