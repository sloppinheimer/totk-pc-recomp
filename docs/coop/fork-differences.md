# How this fork differs from upstream

This repository is a fork of [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) that adds
co-op. Upstream moves fast, so the fork keeps its footprint in upstream's files small: co-op code lives in co-op
files (`runtime/coop_*`, `runtime/ui/coop_*`, `runtime/shaders/coop_*`, `tools/coop/`, `docs/coop/`, `server/`), and an
upstream file carries at most a few one-line calls or `#include`s of them. This note lists every place an upstream file
touches co-op code, and the general changes the fork made to upstream files, so a merge from upstream can check each
point still holds.

Checked against upstream's `main` at the last merge base (6151ac4f, 2026-11-15) with
`git diff 6151ac4f HEAD -- . ':!runtime/coop_*' ':!runtime/ui/coop_*' ':!tools/coop' ':!docs/coop' ':!server'`.

## Behaviour that differs from upstream

- **Function-entry hook in every function.** Upstream's generator puts `FN_ENTER` only into the functions a patch
  table's `hook` lines name (on 1.4.3, one render-size function). The co-op hooks (`runtime/coop_hooks.h`
  `COOP_HOOKED`, `coop_spawn.c coop_hook`) and the co-op tracers run from `FN_ENTER` all over the game, so
  `recomp/patches-v143.txt` has no `hook` line: by upstream's own rule, every function then keeps `FN_ENTER`, the
  render-size function included. After a merge, check that the lift still emits it everywhere (an earlier count was
  about 228,000 `FN_ENTER` lines in `generated/v143/main`; not counted again for this note).
- **`FN_ENTER` itself.** `runtime/coop_hooks.h`, included by `runtime/cpu.h` right after upstream's `FN_ENTER`,
  redefines it: the co-op hook test first (`COOP_HOOKED(a)`, a constant per function, so it folds away where it does
  not match), then `coop_fnprof_hit` (`coop_trace.c`), which calls upstream's `fnprof_hit` and the co-op readers of the
  entry registers.
- **RomFS manifest.** `tools/coop/make_puppet_pack.py` adds the stand-in's actor pack and its RSDB rows to
  `build/<ver>/romfs-patches/manifest.tsv`. `tools/make_romfs_patches.py` rewrites that manifest, so the full build
  runs `make_puppet_pack.py` after it, or no stand-in can be made.

## Calls and includes of co-op code in upstream files

| Upstream file | What it carries | Why |
|---|---|---|
| `runtime/cpu.h` | `#include "coop_hooks.h"` | the co-op entry hooks replace `FN_ENTER` (above) |
| `runtime/cfg.c` | `#include "coop_cfg_names.inc"` in the list of known setting names | `totk.ini` keys of co-op settings are not logged as unknown. The list holds 7 names (`COOP_EDGES`, `COOP_HIDE_CONSOLE`, `COOP_JOIN`, `COOP_NAME`, `COOP_RELAY`, `COOP_TAGS`, `PAD_INDEX`); any other co-op key still works from `totk.ini`, with an "unknown key" log line |
| `tests/settings_doc_check.py` | reads the `.inc` files `cfg.c` includes | so the names in `coop_cfg_names.inc` are checked against `docs/settings.md` too |
| `runtime/ui_text.h`, `runtime/ui_text.c` | `#include "coop_text_ids.inc"` and one `coop_text_<lang>.inc` per language (en, ru, pt_br, zh_hans) | the Co-op tab's and the notices' texts |
| `runtime/ui/menu.cpp` | `#include "ui/coop_menu.h"`; `TAB_COOP` and `tab_coop()`; `coop_notices_glyphs()` in the CJK glyph ranges; `coop_menu_font()` after the font upload; `coop_overlay_wanted()`, `coop_overlay_draw()` and `coop_overlay_shot()` in `menu_overlay_frame` | the Co-op tab; place names in the notices in Chinese; the name tags' copy of the font atlas; name tags, map markers and notices drawn over the game with the menu closed; the co-op tests' screenshots of the overlay |
| `runtime/main.c` | `coop_hide_console()` right after `mem_init()`; `rt_lifted_function()`, a public wrapper of the static `lifted_function` | the console out of the way when several game windows run (`TOTK_COOP_HIDE_CONSOLE`); `coop_watch_win.c` names the lifted function that touched a watched range |
| `runtime/ov.h`, `runtime/ov_input.c` | `coop_tick(c)` at the top of `npad_states`; `coop_local_input(...)` after `input_poll` | `coop_tick` (`coop_spawn.c`) is the co-op net tick: the transport, the session state, the enemy and entity syncs' net halves, the players. It runs at every gamepad poll, also in menus and loads. `coop_local_input` gives this player's input to the relay |
| `runtime/vk_backend.c` | `vk_tags_frame()` in `present_image` | the name tags drawn into the game's frame |
| `runtime/vk_exec.c` | `vk_tags_pass_begin(ex)` in `vk_exec_pass_begin` | the tags learn where the HUD starts |
| `runtime/vk_exec_draw.c` | `vk_tags_draw(ex)` in `prepare_draw` | the tags drawn in front of the HUD's first draw |
| `runtime/gfx_present.c` | `vk_tags_release()` in `gfx_window_release_vk` | the tags' Vulkan objects freed with the window |
| `runtime/mods.c` | in `scan_cb`, a folder with a `coop-character-off` file starts off until the mods order names it | a character mod fetched for another player does not become this player's Link (`coop_chars.c`) |
| `runtime/romfs.c`, `runtime/fs.h` | `romfs_live_add` (files added while the game runs), `ROMFS_RESTBL_SPARES` and `restbl_spares` (16384 spare size-table entries), `romfs_read_whole`, `TOTK_TEST_RESTBL_SKIP` | a party member's character fetched in a session (`coop_chars.c restbl_guest_add`); `romfs_read_whole` is used by `coop_gamedata.c` and `coop_notices.c`. They use romfs.c's own tables, so they stay there |
| `runtime/restbl.c`, `runtime/restbl.h` | `restbl_hash`, `restbl_each`, `restbl_empty` | the size-table entries `coop_chars.c` adds to the game's copy |
| `runtime/boot.c` | `rt_mark_shot()` and its save in `rt_frame_presented` | the tester's mark (Home or numpad 0, `coop_ent_log.c`) saves a picture |
| `runtime/input.h`, `runtime/input_win.c` | `input_mark_key()` | the mark key, read in any window |
| `tools/build_game.py` | `-lws2_32` in both link lists | the in-game relay (`coop_relay.c`) and UPnP (`coop_upnp.c`) use Winsock |
| `tests/diff/harness.c` | a stub of `coop_fnprof_hit` | the differential tests link `cpu.h`'s `FN_ENTER` |
| `AGENTS.md` | a "Co-op" section of one line | points agents at `docs/coop/README.md`, where the co-op rules are |
| `recomp/patches-v143.txt` | no `hook` line, with a comment saying why | `FN_ENTER` in every function (above) |

`runtime/memory.c` carries no co-op code.

## Files the fork adds outside the co-op folders

- `runtime/vk_tags.c`, `runtime/vk_tags.h`: the co-op name tags drawn into the game's frame before its HUD, hidden by
  the scene's depth (`TOTK_COOP_TAGS_INFRAME=0`: the overlay draws them instead). Named like upstream's renderer files,
  but co-op only; `ui/coop_tags.cpp` lays them out. Shaders: `runtime/shaders/coop_tag_vs.vert`, `coop_tag_fs.frag`,
  compiled into `runtime/coop_tag_vs_spv.h`, `coop_tag_fs_spv.h`.
- `Play co-op (two pads).vbs` in the repository root: starts `tools\coop\play_two.ps1` with no window. With no options
  that script moves both games with one gamepad; `-TwoPads` gives each game its own.
- `server/coop_relay.py`: the standalone relay the test scripts and `play_two.ps1` use (the in-game Host uses
  `runtime/coop_relay.c`, the same protocol).
- `tools/input/load_save_3rd.txt` (title to the third save), and new timings in `tools/input/load_save.txt`.

## General changes in upstream files

Not co-op code, but made for it. Candidates for upstream pull requests.

- `runtime/input.c`, `runtime/input_win.c`, `runtime/input_sdl.c`, `runtime/input_wsys.h`:
  - `TOTK_PAD_INDEX`: the n-th connected gamepad, picked again when the list changes (one game per gamepad).
  - The pad focus mode: with one gamepad driving two games (`TOTK_ASSUME_FOCUS`), Minus with the right stick click, or
    F9 in any window (`ikb_f9_any`), switches between "every game" and "only the game in focus" (`pad_focus_gate`;
    `TOTK_PAD_FOCUS_ONLY=1` starts in it).
  - A test input script drives the menu as a pad does (`script_apply_to` with `menu`).
- `runtime/ui/menu.cpp`, `runtime/ui/menu.h`, `runtime/ui/menu_win.cpp`, `runtime/ui/menu_sdl.cpp`: typed text in the
  menu's text boxes (`menu_event_char`, WM_CHAR, SDL text input), Ctrl+A/C/V/X with the modifier keys ImGui reads,
  a page's own popup holding B and the tab keys (`menu_page_modal`), and Esc leaving a text box before the menu.
- `runtime/boot.c`: `TOTK_SHOTS_CROP=x,y,w,h` saves only part of each test screenshot.
- `runtime/ov_input.c`: logs the controller ids the game supports.
- `docs/settings.md`: `pad_index` and a "Co-op" table (`coop_name`, `coop_join`, `coop_relay`, `coop_tags`,
  `coop_hide_console`, `coop_edges`); `docs/controls.md`: one line.

## Leftovers to tidy

- `runtime/ov.h` calls `coop_tick` "co-op spawn test, from the gamepad poll"; it is the whole co-op net tick.

## Merging upstream

Merge often (weekly), never rebase (the fork is public). After a merge: the full build (`recomp/gen.py`,
`tools/make_romfs_patches.py`, `tools/coop/make_puppet_pack.py`, `tools/build_game.py`), the points above, and a
two-game check (`tools/coop/run_pair.ps1` with `tools/coop/idle.txt` for both games: each shows the other's stand-in).
