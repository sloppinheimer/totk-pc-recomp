# Co-op plan: enemies act, never decide

Written 2026-11-17. This page is the plan. If something is not on it, it is not the plan; the other co-op documents are
reference.

## Why this plan

The enemy sync is being built one visible thing at a time: effects, arming, throws, held effects, ragdolls, prompts,
each made again by hand on a copy whose AI does not run. The spec counts about 9,700 node instances of such acts
(the old enemy sync spec, B.38; facts/enemies.md has the reading). That list does not end, and it is why progress comes in tiny increments.

The fix is to let the game do the work. In the game that runs an enemy (its owner) nothing changes. In every other
game the enemy's copy keeps the part of the AI that performs a move and loses the part that chooses one:

- **Decides (off on a copy):** the brain, the utility graph, the scheduler and its action sequence. They pick the
  target and the move.
- **Acts (on for a copy):** the action slots. They perform the move the owner chose: its effects, sounds, attack
  arming, held items, physics on the copy's own body.
- **Blocked on a copy:** anything a move does to the shared world (an actor made or deleted, a drop, a throw, a
  pick-up, a shot, a death, loot, game data, noise for other actors). The owner's game does those for real and the
  sync carries them.
- **Local:** whether a copy's blow hits this game's own Link is decided in this game, from what it shows.

The cost of this design does not grow with the number of enemy kinds: it is the owner's requests, a few blocks at
engine functions, and the facts of the copy's own state an action reads.

History, so it is not repeated: a first form of this (2026-11-07, the "native model" in `coop_world.c`) kept one
request per enemy, so empty requests for slots 1 to 4 overwrote the Main one and copies ran mostly on their own AI.
It was replaced by copies that play and never think (2026-11-15) without a side-by-side test. The action layer
(`coop_enemy_act.c`, B.38 and B.41) is this design done properly, still off by default.

## Where it stands (read from the code and the spec, 2026-11-17)

- `TOTK_COOP_EN_ACT=1` turns it on. A copy's Main slot runs its owner's request, made when this game shows the owner
  at the request's time (`en_act_precalc`), with the owner's controller move state and velocity written first
  (`cct_write`).
- Blocking is by node type: 267 classes in 10 categories (`coop_enemy_act_deny.inc`), from an audit of 578
  outward-acting classes. About a fifth were placed by name only, so some may be in the wrong category.
- Animation still comes from the owner's stated slots; plays the action asks for are answered "played" and not made.
- Measured in 80 s scripted runs (B.41): a Yiga footsoldier, a Chuchu and three Bokoblins with every verb on; no
  crash, no leak, tables equal, the same command 95 to 100 % of frames. In a play session a copy's melee blow did
  hurt the other player (B.39).
- Never seen: a fight with every verb on (a throw, a drop, a death), any other family, a play session with the layer
  on.

## Rules for this work

1. **One front.** Enemies until step 5 is done. The horses, lightning and elements work waits.
2. **No design switch without a side-by-side test in play.**
3. **A test passes on what both games show, not on what the sync believes.** The mismatch checker (step 1) and
   paired screenshots from both games are the evidence. A report says what was checked, what was not, and the
   mismatch count; never "clean" without the list behind it.
4. **Read before building.** Every engine function blocked in step 2 is read first, callers included.
5. **The tester's time is for how it feels.** Scripted runs with the checker find desyncs; a play session is asked
   for, by name, at step 4.
6. **This page is kept current.** Each step's result is written here when it ends.

## Steps

### 0. The clean-up (2026-11-17, now `main`)

Done: every branch of the co-op work merged into one branch; the co-op code mapped; about 8,300 lines of code that
could not run, experiments, failed alternatives and unused tests removed (co-op runtime about 38,500 to 30,200
lines); eleven bugs found by reading fixed (hook addresses with an extra digit, contraption state restored from
uninitialised memory, a shield fuse scale never applied, a shared message number, and others); records and
measurements behind `TOTK_COOP_DEBUG=1`; the documents replaced by this plan, two how-it-works pages and facts/
(README.md). Two short two-game runs after the removals: no crash, stand-ins and an enemy copy shown in both games.

Not exercised by those runs (both Links stood still, nobody fought or built): the fixes that change behaviour. The
Shooter-sleep replay of a copy's prepared shot runs for the first time (`TOTK_COOP_EN_SHS=0` turns it off); a
stand-in's fused shield material is scaled to its owner's; the spawn sync leaves an enemy seen dead to the enemy
sync; game-data catch-up has its own flag (game-data version 4); failed create requests are released. Moving
stand-ins, contraptions, hits and the horse sync were not run either.

Seen in both runs and in a run before the clean-up (`fam-Enemy_Bokoblin_Junior`), cause unknown: a Bokoblin made by
`TOTK_COOP_TEST_SPAWN` beside the Links on the Great Sky Island platform leaves its owner's game about 5 s after it is
made, without dying (`coop_spawn_sync.c spawn_tick` then sends its gone notice). Step 1's deliberate-desync test and
step 4's fight need an enemy that stays: read why before relying on a test spawn there.

Before any run in the main checkout (`C:\Dev\totkCoop`): its build and stand-in pack date from 2026-11-15, before
the clean-up. Run `.venv\Scripts\python tools\coop\make_puppet_pack.py --version v143` (the pack changed: the
stand-in avoids lightning) and then a full `tools/build_game.py --version v143` (the hook list changed). The lifted
code is current (`recomp/` did not change).

The review before step 1 was done on 2026-11-17 (its report, `build/CODEBASE_REVIEW.md`, stays outside git: it lists
weak spots of a public repository; finding IDs below are its). Fixed, in the report's order (commits 7cb76921 to
6061ab3f; each built, none run in the game yet):
- SYNC-001: the contraption sync opens its reader of the transport's inbox; the enemy sync, on by default, had been
  taking and dropping all its reliable messages and join/leave events.
- SEC-003, SEC-010, SEC-011, SEC-016: hosting asks for a session code (the address players type is now
  `<address> <code>`; the standalone relay takes `--code`); the router's forward follows the relay. The packet version
  is 32 (31 for the code, 32 for a player state without the fields nothing read).
- BUG-001, BUG-002, BUG-003, SEC-002, SEC-008, SEC-009: what another game sends is checked before it is used (numbers,
  offsets, counts); a stack overflow in the contraption status line and a write through a stale actor address fixed.
- SEC-001, SEC-004, SEC-005, SEC-006, SEC-012, SEC-013: names and owners of what another game asks this one to make,
  move or delete are checked where received (README rule 7).
- REL-001, REL-002, SEC-014, SYNC-004, SYNC-005, SYNC-008, BUG-004, BUG-005: session and slot lifetimes (a lost LEAVE,
  copy requests released, tables that filled and never emptied).
- TEST-001 to TEST-006: the test tools judge the copies' real places, fail on any FATAL line and when nothing was
  compared, and no longer pass on the sync's own bookkeeping.
- SYNC-002, SYNC-003, RACE-001, RACE-002, RACE-003, SEC-015, SEC-017, REL-004, BUILD-001, ARCH-001, DUP-001, DOC-001,
  DOC-002: the enemy outbox and shot table, threads' shared state, shared-world stamps, the rotating ground's sender,
  test tracers, hooks and functions with no use, docs and comments.

Not run: none of these fixes has been exercised in the game. Hosting from the Co-op tab now needs the code: the first
run of step 1 that hosts from game A's tab tests it (`run_session.ps1` gives game A the code `TEST42`).

Left, and where it goes:
- With step 2 (engine reads): SYNC-006, whether one actor's calc can run inside another's (an enemy copy's
  thread-local marks stay set across other actors' calcs, and a held brain's grace may turn away another actor's
  target reset). Its change was taken back (4d217dc4): the calc is reached only through its vtable entry, but a call
  through that entry from inside another calc is not ruled out by reading. PERF-001 was settled by reading the calc's
  entry and fixed again (see its commit).
- A protocol change for contraption ownership (SEC-007 in the report; no step yet). The session code keeps it to
  invited players.
- With step 1's runs: SYNC-007, a contraption copy's device striking an enemy this game runs counts in both games.
  Leaving it to the device's game (tried in e9b9ddcf, taken back) loses the blow when that game has not loaded the
  enemy; the checker's life column shows which case happens.
- Replaced by step 1's checker: the enemy bench's own bookkeeping (tables equal, shot counts), the empty enemy list of
  the sync audit.
- `coop_hooks.h`: the enemy hooks 0x2314ab0, 0x42ec98, 0x1b87a9c, 0x195321c, 0xa70a1c, 0x4e603c, 0x4f59ec, 0xc448c8
  and 0x1b70a04 have no handler; step 2 is to use them and replaces 0x1bb16a0 and 0x1ba7710.
- Small, when the files are next touched: duplicated helpers (quaternion conversions, FNV hashes, main-body lookups,
  the equipment-slot walk, the "paused" tests, the report's consolidation table); comments that still name removed files or tests (fewer than the 40 listed before);
  `coop_cfg_names.inc` holds 7 co-op names; 48 input scripts in
  `tools/coop/` that no file names; the untracked `map_hold.txt` and `menu_hold.txt`; the
  parked branches `claude/coop-arrow-tail-wip` and `claude/quizzical-poincare-8711f5`.

### 1. The mismatch checker

Both games write, once a second, every actor within reach of either Link, read from the game's own memory: identity
(placement hash or the sync's run-time id), name, position, rotation, life, death state, the command in each
animation slot, what each equipment slot holds, whether it is drawn. A tool lines the two games' records up by time
and identity and lists every difference beyond a tolerance, including an actor that exists in one game only. It
replaces the sync audit's enemy check, which always wrote an empty list (`coop_world.c coop_audit_enemies`).

The scripted pair runs use the Python relay (`server/coop_relay.py`); players host from the Co-op tab with the
in-game relay (`coop_relay.c`). At least one checker run hosts from game A's tab, so the relay players use is tested.

Done when: it runs on a scripted pair, its report lists differences by actor and kind, and a deliberate desync
(an enemy moved in one game by a test key) shows up in it.

### 2. Block at engine functions

While a copy's action runs, this thread knows it (`tl_act_actor` in `coop_enemy_act.c`). A call made under that flag
into any of these is refused, whichever node made it:

- actor create (main+0x599670 by caller; main+0xaf100c for equipment)
- equipment mode (main+0x4f59ec)
- drop and throw records (main+0x1b738e4, main+0x1b73be4)
- remove (main+0x1b70a04), pick up (main+0x1d71dfc), lifter throw (its entry main+0x1ba796c)
- shoot and prepare (the Shooter's entries)
- the death request, the drop and kill-flag bits
- noise for other actors (ExecutePerceptionEmitNoise)

The full list, with arguments and sources, is facts/enemies.md section 4; read it before building. It corrects two
addresses this plan first had: main+0x1bb16a0 is the AI's memory of a weapon, not the pick-up, and main+0x1ba7710 is
the lifter's release setter, not the throw. `coop_hooks.h` still registers those two for this step; they are
replaced when it is built. Addresses no runtime code uses yet rest on the research notes alone and are read again
before use. For each function: what its callers do when it is refused (a node that expects an actor back
may crash). The 267-class list stays as a second net; a refusal at an engine function logs the node class, so a
class missing from the list is found.

Done when: each function is read and blocked, and a scripted run with every verb on logs the refusals with no crash.

### 3. All slots and interrupts, for the Bokoblin family

- Requests of all five slots, not only Main: the owner sends each slot's request (`en_act_owner` returns for any
  slot but 0 today), and a copy keeps one per slot (`actq` holds one).
- Interrupt requests (skipped today by `flags & 1`). They start reactions (flinch, stagger) that a copy now shows
  from stated animation and bones.
- The form key: which enemies have more than one, read before assuming the default.

Done when: a scripted camp fight of Bokoblins with all slots and interrupts on shows no crash and the checker's
difference list is read and explained.

### 4. One camp fight in play

Asked of the tester, by name, with the layer on for Bokoblins. The checker runs and screenshots are taken from both
games. This is the side-by-side test that settles the design.

Done when: the tester has played it and the checker's report and the tester's account agree on what was wrong.

### 5. Take out what the actions replace

The hand-made replays of B.17 to B.36 (effects by key, the arm replay, held effects, and the rest) are removed for
families whose copies run their actions, each only where the checker shows the action does the same.

Done when: those replays are gone for the Bokoblin family and the checker's list is no longer than before.

### 6. Widen family by family

Each family: every verb on, a scripted run with the checker, the facts its actions read that a copy lacks (as the
controller state was for a Chuchu) found from where its actions stall, fixed, run again. Then its replays come out.

## Open risks

- **Owner changes mid-fight.** Four in one short fight (B.39). A copy that becomes the owner starts its brain in the
  middle of an action; the crash of B.37 is guarded but not proven impossible.
- **Hits "inconsistent"** in the tester's play session (B.39): not explained.
- **Delay.** A copy starts each move one network delay after its owner.
- **Facts read from the copy's own surroundings** (its target, the ground) may differ from the owner's; position and
  rotation are eased to the owner's, and the checker shows what remains.
