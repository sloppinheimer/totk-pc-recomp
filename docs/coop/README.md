# Co-op: where things are, and the rules for keeping them

Read this before any co-op work. It is short on purpose: one page says what is planned, two say how the code works
now, one folder holds what is known about the game.

## The documents

| Document | What it is | Who keeps it current |
|---|---|---|
| [PLAN.md](PLAN.md) | The plan: why, the rules for the work, the steps and where each stands. If something is not on it, it is not the plan. | Every change that starts, finishes or changes a step |
| [enemy-sync.md](enemy-sync.md) | How the enemy sync works now, from the code | Every change to `runtime/coop_enemy*.c` and what it calls |
| [entity-sync.md](entity-sync.md) | How the contraption (entity) sync works now, from the code | Every change to `runtime/coop_ent*.c`, `coop_ctn.c` |
| [facts/](facts/README.md) | What is known about the game itself: addresses, structures, measured behaviour | Every new reading or measurement of the game |
| [playing.md](playing.md) | How to play co-op | Every change a player would notice |
| [character-mods.md](character-mods.md) | Character mods in co-op | Changes to `coop_chars.c` and `tools/coop/charmod.py` |
| [fork-differences.md](fork-differences.md) | Every line the co-op adds to upstream files, and why | Every such line added or removed |

## Rules

1. **No new plan, spec, design, audit or punch-list files.** Plans go in PLAN.md as steps; how something works goes in
   the how-it-works page of its area, written from the code; facts about the game go in facts/. A new area of the
   sync gets its how-it-works page when its code is built, not before.
2. **The code is the truth.** A how-it-works page describes the code as it is; a change to the code changes the page
   in the same commit. When a page and the code disagree, the code wins and the page is fixed.
3. **Facts are about the game, marked and sourced** (facts/README.md). A reading that corrects an older one replaces
   it and says so.
4. **A test passes on what both games show**, not on what the sync logs about itself (PLAN.md, "Rules for this
   work"). A report says what was checked, what was not, and why.
5. **Co-op code stays in co-op files.** Upstream files get one-line calls or includes, each listed in
   fork-differences.md.
6. **No test settings without a script that uses them.** A `TOTK_COOP_TEST_*` setting is added with the script that
   sets it and removed when the script goes. Records and periodic measurements run only with `TOTK_COOP_DEBUG=1`.
