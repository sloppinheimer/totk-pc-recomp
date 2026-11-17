# Game facts for the co-op mod

What is known about *Tears of the Kingdom* 1.4.3 itself: addresses, structures, what a game function does, and
game behaviour measured in a run. Nothing here describes our co-op code, a plan, or a setting of ours. How the
co-op works is in [../enemy-sync.md](../enemy-sync.md), [../entity-sync.md](../entity-sync.md) and the code's own
header comments; what is planned is in [../PLAN.md](../PLAN.md).

## Files

| File | What it covers |
|---|---|
| [engine.md](engine.md) | Actors and the engine around them: actor layout and states, frame phases, creating and deleting actors, death requests, physics bodies and collision filtering, model, skeleton and animation, messages, blackboards, game data, world state (time, weather), input and UI, resources and mods |
| [player.md](player.md) | Link: life, EquipmentUser and gear, hits on Link, riding, how enemies perceive a player, an actor made from the Player's pack, arrows, beams, the paraglider |
| [zonai.md](zonai.md) | Ultrahand, glue, bonds and groups, Autobuild, Zonai devices and energy, harm on parts, lifetime and limits |
| [enemies.md](enemies.md) | Enemy components, AI runners, brain verbs and actions, enemy animation, perception and targets, hits and damage, ragdoll, death and drops, kill flags and the blood moon, equipment, Shooters, spawning, families |
| [elements.md](elements.md) | The chemical system, ground cells, element kinds and reactions, conditions, area effects, lightning |
| [horses-animals.md](horses-animals.md) | Horses and animals: census, scheduler, riding, tack, gait values, herds, the owned-horse manager |

Each file ends its sections with what was looked for and not found or not read.

## Conventions

- **Version.** Everything is TotK 1.4.3 unless a line names another version.
- **Addresses.** `main+0x...` is an offset in the main module. In Ghidra that is 0x7100000000 plus the offset; in
  the lifted C (`generated/v143/main/`) the function is `f_70` followed by the offset in 8 hex digits. Where Ghidra
  has no function at an address (many vtable-only functions), read the lifted C.
- **Pointers.** `[x+0x10]` is the 64-bit value at x+0x10; `[[actor+0x228]+0x2f8]` reads through two pointers.
  `comps` is short for `[actor+0x228]`, the actor's component table.
- **Call sites and return addresses.** A caller is named by the return address (the instruction after the call)
  unless the line says "call site". The two differ by 4.
- **Bits.** "Bit n" counts from 0 at the least significant bit. A mask is written in hex.
- **Names.** Names in code font that look like the game's (`SeqExecuteDeleteAllActor`, `ImaginaryAutoBuildedActor`)
  are strings found in the game. Other names (the death controller, the pool record R) are ours.

## How sure each fact is

Every fact carries one mark:

- **[read]**: read in the game's code (Ghidra or the lifted C) or in its data files.
- **[measured YYYY-MM-DD]**: seen in a run of the game on that date. A measured number is kept as it was seen.
- **[inferred]**: follows from what was read, but nobody read or saw it directly.
- **[open]**: sources disagree, or the question was asked and not answered.

Where a later reading corrected an earlier one, only the later fact is kept, with "(corrects ...)" when the old
claim may still be cited somewhere.

## Game content

This repository is public. Facts are written in our own words: no decompiled code, no pseudocode copied from the
research notes, no tables of game data, no long quotes. A fact may name a game string, an address, an offset or a
number. Files derived from the game stay in ignored folders (see [../../../AGENTS.md](../../../AGENTS.md)).

## Where the readings live

The readings behind these facts are notes outside the repository, in the research folder (a sibling checkout
named `totkCoop-research`). Its notes are derived from the game's code and are never copied in here; a fact cites
them by subfolder and file, as "research: zonai/physics". The subfolders:

- `zonai/`: physics, collision-filter, player-physics, ultrahand, glue, autobuild, devices and devices2 (per
  device kind), runtime (functions seen in runs), sync-v3 and sync-v4 (focused readings: create-callers,
  create-to-actor, delete-reasons, load-teardown, hold-end, edge-removal, strand-cap, bond-frames and others).
- `enemy/`: verify/00 to 09 (the re-check of the enemy decode), gaps/G1 to G8 and R1 to R22 (focused readings).
- `horses/`: H1 to H4.
- `elements/`: E1 to E3.

A fact that came from a run of the game, or that no research note holds, cites the old co-op document it came
from by name and section (re-notes, proxy-plan, character-mods, contraption-sync, contraption-sync-v3,
contraption-game-facts, enemy-system, enemy-sync-spec, enemy-sync-design). Those documents are retired; the
citation says where the fact was first written down, not where to read more.

## Adding a fact

Put it in the file and section of its subject, with its mark and its source. If it contradicts a fact already
here, keep the later reading, say which one it corrects, or mark the point [open] if neither settles it.

## Old names in sources and code comments

The co-op docs before 2026-11-17 (commit 2604efb8 and earlier) were plans and specs with facts mixed in; their facts
are here and the files are gone. Sources and code comments still name them and their sections:

| Old name | Its facts now | Its plan text |
|---|---|---|
| contraption-game-facts, "game-facts NN" | zonai.md, engine.md (headings say "was: game-facts NN") | none |
| contraption-sync, -v3 ("v3 NN"), -v4, -v4-spec ("S n.n" in coop_ent*.c) | zonai.md, engine.md ("was: v3 NN") | [../entity-sync.md](../entity-sync.md) is how it works now |
| enemy-system, enemy-sync-design, enemy-sync-spec ("spec B.NN", "S n.n" in coop_enemy*.c), the audit's how-it-works and punch list | enemies.md ("was: spec B.NN") | [../enemy-sync.md](../enemy-sync.md) |
| element-sync-design ("D n.n"), element-sync-spec ("S n.n" in coop_elem.c) | elements.md | none |
| horse-animal-sync-spec | horses-animals.md | none |
| re-notes, proxy-plan | engine.md, player.md | none |
| research notes R1.., G1.., verify/.. | each fact names its note under the research folder | |

Code comments were pointed at the new files by name; a section number in one is the old file's. Find it by its "was:"
tag here, or read the old file in git history (`git show 2604efb8:docs/coop/<name>.md`).
