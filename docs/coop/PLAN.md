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
- Blocking is by node type: 276 classes in 10 categories (`coop_enemy_act_deny.inc`), 267 from an audit of 578
  outward-acting classes and nine that step 2's reading found reaching an engine function (facts/enemies.md 4.1).
  About a fifth were placed by name only, so some may be in the wrong category.
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

Seen in both runs and in a run before the clean-up (`fam-Enemy_Bokoblin_Junior`): a Bokoblin made by
`TOTK_COOP_TEST_SPAWN` beside the Links on the Great Sky Island platform leaves its owner's game a few seconds after it
is made, without dying (`coop_spawn_sync.c spawn_tick` then sends its gone notice). Found (2026-11-17, a 60 s run with
`TOTK_COOP_TEST_FOELOG`; facts/enemies.md 13.3): its brain finds no path home there, takes ForceReturnHome and its own
AI asks a far delete (reason 3) 8.5 s after it is made. Not our code. The test enemy alone is now kept from that one
delete (`coop_life.c coop_life_hook`); what it does after the refusal is not seen yet (it is likely to stay in
ForceReturnHome and not fight). Step 4's fight needs a place with a navigation path (a camp save).

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

Run once (2026-11-17, `run_session.ps1`, 90 s, both games standing still, no pictures taken): game A hosted with the
code, game B joined with `127.0.0.1 TEST42`, each game made the other's stand-in ("slot 0: Player ... in the world"
in both logs), the router was asked for the forward only after the relay had the port (no router answered here), no
FATAL line and no message dropped for an unopened reader. B's leave at 70 s fell after the run's end. Nothing else of
these fixes has been exercised: moving players, enemies, contraptions and hits wait for step 1's runs.

Left, and where it goes:
- With step 2 (engine reads): SYNC-006, whether one actor's calc can run inside another's (an enemy copy's
  thread-local marks stay set across other actors' calcs, and a held brain's grace may turn away another actor's
  target reset). Its change was taken back (4d217dc4): the calc is reached only through its vtable entry, but a call
  through that entry from inside another calc is not ruled out by reading. Step 2's part 3 keys the node tick and the
  target reset on the call's own actor, which holds either way; whether calcs nest is still not read (R26 names the
  run that decides). PERF-001 was settled by reading the calc's
  entry and fixed again (see its commit).
- A protocol change for contraption ownership (SEC-007 in the report; no step yet). The session code keeps it to
  invited players.
- With step 1's runs: SYNC-007, a contraption copy's device striking an enemy this game runs counts in both games.
  Leaving it to the device's game (tried in e9b9ddcf, taken back) loses the blow when that game has not loaded the
  enemy; the checker's life column shows which case happens.
- Replaced by step 1's checker: the enemy bench's own bookkeeping (tables equal, shot counts; en_bench does not run
  the checker yet). The sync audit's empty enemy list is removed.
- `coop_hooks.h`: the enemy hooks 0x2314ab0, 0x42ec98, 0x1b87a9c, 0x195321c, 0xa70a1c, 0x4e603c, 0x4f59ec, 0xc448c8
  and 0x1b70a04 have no handler; step 2 is to use them and replaces 0x1bb16a0 and 0x1ba7710.
- Small, when the files are next touched: duplicated helpers (quaternion conversions, FNV hashes, main-body lookups,
  the equipment-slot walk, the "paused" tests, the report's consolidation table); comments that still name removed files or tests (fewer than the 40 listed before);
  48 input scripts in
  `tools/coop/` that no file names; the untracked `map_hold.txt` and `menu_hold.txt`; the
  parked branches `claude/coop-arrow-tail-wip` and `claude/quizzical-poincare-8711f5`.

### 1. The mismatch checker

Both games write, once a second, every actor within reach of either Link, read from the game's own memory: identity
(placement hash or the sync's run-time id), name, position, rotation, life, death state, the command in each
animation slot, what each equipment slot holds, whether it is drawn. A tool lines the two games' records up by time
and identity and lists every difference beyond a tolerance, including an actor that exists in one game only. It
replaces the sync audit's enemy check, which always wrote an empty list.

The scripted pair runs use the Python relay (`server/coop_relay.py`); players host from the Co-op tab with the
in-game relay (`coop_relay.c`). At least one checker run hosts from game A's tab, so the relay players use is tested.

Movement and animation are judged from the first pass, per enemy copy against its owner, from each game's
per-frame record. Targets (set before the first measurement; a step is not done on state agreement alone):
- lag (the delay that best lines the copy's path up with the owner's, while it moves): at most 200 ms on one PC;
- place error at that lag: 95th percentile at most 0.3 m;
- stutter (the copy's frame-to-frame acceleration, RMS, over the owner's): at most 1.5 times;
- the owner's command in the first animation slot: at least 95 % of frames;
- the animation's phase (clock difference on the same command, in seconds of the owner's clock rate): 95th
  percentile at most 0.1 s;
- once a second: no difference that holds two seconds in place, rotation, life, death, ragdoll, drawn, slot commands
  or equipment, and no enemy within 60 m of a Link shown in one game only;
- added after the baseline (what an action does): attacks armed with the owner in at least 95 % of the owner's armed
  moments, and armed without the owner in at most 5 % of the copy's armed frames; effects and sounds started by key:
  at least 95 % of the owner's made in the copy's game too, at most 5 % of the copy's game's not the owner's; every
  blow between an enemy and a Link in both games, with the same outcome. A run with none of attacks armed, effects by
  key or blows recorded is "not judged".

Done when: it runs on a scripted pair, its report lists differences by actor and kind, and a deliberate desync
(an enemy moved in one game by a test key) shows up in it.

Built (2026-11-17): `runtime/coop_check.c` writes each game's `logs/check.jsonl` (with
`TOTK_COOP_DEBUG=1`), read from the game's memory with the enemy sync's readers of slots and equipment
(`en_read_anim`, `en_read_equip`, which tell no other game anything); `tools/coop/check.py` compares the two
(enemy-sync.md, "Checks"); `run_pair.ps1`, `run_session.ps1` and `play_two.ps1` run it at the end. The deliberate
desync is `run_pair.ps1 -Nudge <t>,<s>,<m>`: game B shows its first enemy copy m metres east of where its owner's
state puts it, through the copy's own shown place, so its pictures show it too. The sync audit's empty enemy list and
its enemy check are removed. Checked without a game: `check.py` on a made-up pair of records found a 120 ms lag, the
planted 5 m desync in its window, and an enemy in one game only.

First run (2026-11-17, `run_pair.ps1`, 60 s, both Links standing, a test Bokoblin at 20 s, pictures from both games):
the record was read in both games with no unreadable line; 7 enemy-seconds compared once a second; the copy against
its owner frame by frame: the same command 99 % of frames, phase p95 0.03 s, place error p95 0.58 m (it moved 1.8 s,
too little for lag and stutter, so not judged against the 0.3 m target). One difference listed: the new Bokoblin was in
game A only for its first 2 s (the spawn sync sends a new enemy about 2 s after it is made). The pictures agree: both
games showed it beside the Links, then running off, then gone. A commit review of the checker found ten ways
`check.py` could pass on too little or judge wrongly (a nudge asked for and never made, no copy that moved, two
owners, unreadable lines, duplicate identities, the owner per second, the lag match, phase per command); all fixed:
a run where no copy moved enough is now "not judged" (exit 2), not a pass.

Gate runs (2026-11-17, both Links standing, a test Bokoblin at 20 s, the planted desync asked of game B at 35 s for
8 s, pictures from both games):
- `run_pair.ps1` (the Python relay): PASS. The planted desync was listed on that Bokoblin for its 8 s and nothing else
  was; the Bokoblin stayed (its AI asked its far delete every 3.3 s, refused) and ran about, so movement was judged for
  the first time: lag 60 ms, place error p95 0.11 m, stutter 0.71x, the owner's command 100 % of frames, phase p95
  0.004 s, all within the targets. 27 enemy-seconds compared once a second.
- `run_session.ps1` (game A hosts from its Co-op tab, B joins with the code): FAIL, on a real desync. The Links stood at
  the platform's edge there and fell about 38 m at 26 s (the Bokoblin was at them); A handed the Bokoblin to B, whose
  Link was then nearer (26.5 m against 34.1 m: the rule working); in B it took ForceReturnHome and its actor went
  (B's log: "offered (this game's actor is gone)"; the keep is only in the game that spawned it, so very likely the same
  far delete, not logged in B); A took it back from its copy; and B never had it again: "missing", 26 s, until the
  end. The planted desync was not made (B had no copy at 35 s), and check.py says so. While B ran it, lag 60 ms and
  place error p95 0.11 m both ways.
- Found: **a run-time enemy whose actor is gone in one game while it lives on in another is never made again in the
  first** (the spawn sync makes copies only when an enemy is made). In play: a run-time enemy that one game unloads
  (its player walks off) and another takes over is gone for the first player when they come back. Goes to step 3's
  list (no change made yet).
- Pictures: `TOTK_SHOTS` times each game's pictures by its own world clock, and game B starts 3 to 4 s after A, so
  pictures of a moving enemy are not of one moment; they show it is there in both, not where. The checker's places are
  of one moment (the PC's clock).

The baseline before step 2 (2026-11-17, the camp save `build/profile-v143-camp`, restored from a worktree recycled in
the clean-up; `run_pair.ps1` with `camp_melee.txt` for A and `camp_watch.txt` for B, 110 s, seven Bokoblins, pictures
from both; the same run with the action layer off, as players have it, and on, `TOTK_COOP_EN_ACT=1` at its default
verbs). A fought in both (enemies' life went down; none died); B watched from about 30 m; no FATAL; 560 enemy-seconds
compared in each.

| | layer off | layer on |
|---|---|---|
| lag (the four that moved) | 70 ms each | 70 ms each |
| place error p95 | 0.03 m | 0.04 to 0.07 m |
| stutter | 1.19 to 1.23x | 1.18 to 1.27x |
| owner's command, worst copy | 90 % (three under 95 %) | 96 % |
| phase p95, worst | 0.084 s | 0.081 s |
| life differences | none | none |
| differences held two seconds | 6: a Guard Bokoblin run by neither game (each its own) for 19 s; a bow in slot 3 of a Guard Bokoblin in one game only, 40 s and 22 s; ragdoll 3 s twice; a command 4 s | 4: the bow in one game only, 36 s; a sword in one only, 3 s; ragdoll 2 s; a command 2 s |

So on what the checker measures, the action layer changes little: movement and life are the same, commands agree a
little better. That is expected: in both models the copy's animation is the owner's stated one. What the layer is for
(effects, sounds, arming, held things, physics an action does) the checker does not see yet; to measure step 2 onward
against this baseline, the checker needs those (an enemy's effects by key, its armed attacks, hits on each Link).
Lasting differences found in both runs, with or without the layer: a Guard Bokoblin's bow held in one game only, and
(one run) an enemy each game ran as its own. One run each: no repeat.

The baseline again with what an action does (2026-11-17, the same two runs after the checker learnt attacks armed,
effects by key and blows; no FATAL; pictures show A fighting in the camp in both games, B watching from 30 to 40 m):

| | layer off | layer on |
|---|---|---|
| attacks armed with the owner (four Bokoblins that fought) | 99, 93, 52, 100 % | 96, 86, 61, 89 % |
| attacks armed on the copy without the owner | 1, 3, 47, 0 % | 24, 3, 42, 12 % |
| blows between an enemy and A's Link in both games | 8 of 30 (22 in A only) | 10 of 28 (18 in A only) |
| effects and sounds by key | none recorded in either game | none recorded in either game |
| movement (lag, error p95, stutter) | 70 ms, 0.05 to 0.08 m, 1.10 to 1.51x | 70 ms, 0.03 to 0.04 m, 1.19 to 1.26x |

- Arming is the first measure where the two models differ: with the layer on, copies arm attacks their owner did
  not (up to 42 % of their armed frames; 24 % for the Dark Bokoblin, which agreed 99 % with the layer off). The
  action's own arming nodes and the arm replay may both be arming: step 2 reads this first.
- One Bokoblin (p5cb495b...) disagrees in both models (about half its armed frames): its attacks are armed on the copy
  at other times than on its owner. Not read yet.
- Blows: every blow listed in one game only is in A (where they happened) and not in B, whose Link was 30 to 40 m
  off: B did not register the copy's blows on A's stand-in, nor the stand-in's on the copies, most of the time.
  Whether that is what B shows (a blow that misses there) or the game not testing hits that far from its own Link is
  not known; a run with B close to the fight tells.
- Effects by key: the Bokoblins started none in this fight (the enemy sync logged none either), so the effect record
  is not shown to work yet; a Fire Wizzrobe (its `WeatherFlash` is started by key, measured 2026-11-17) shows it.
- One run each, no repeat.

Two runs to settle those (2026-11-17, layer off):
- Both Links fighting in the camp (`camp_melee.txt` for both, 110 s): an enemy's blow on a Link is in that Link's game
  only, in every case (A: 24 on its Link, none on B's stand-in; B: 15 on its Link, none on A's stand-in). That is the
  design (a stand-in takes no enemy contact; each game decides hits on its own Link from what it shows), so the check
  is now: a copy's blow on a Link must come while the enemy's owner had an attack armed. 3 did, 1 did not (a Bokoblin
  hit B's Link with nothing armed in A: a blow the owner never made). A Link's blow on an enemy was in both games 21
  times, in one only 2, with the same outcome every time. Some blows of an enemy on its own game's Link came with no
  arm call at all, so arming is not every attack. Attacks armed agreed worse than with B watching (22 to 93 % found
  per copy), not read yet.
- A Fire Wizzrobe beside A (90 s, the Great Sky Island platform): it floated by the Links in both games and never
  attacked (no shot, no weather, nothing armed), so effects by key are still not shown to work. Only AI nodes and a few
  engine paths start an effect by key (36 calls of main+0xa98cf8 in the game, read in Ghidra); clips start theirs
  otherwise. Effects by key are now "not judged" alone when none were started; the run is judged on the rest.

Step 1 is done by its "done when": the checker runs on a scripted pair, lists differences by actor and kind, finds the
planted desync, and one run hosted from the Co-op tab.

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
the lifter's release setter, not the throw; both are off `coop_hooks.h` now, replaced by the throw's entry.
Addresses no runtime code uses yet rest on the research notes alone and are read again before use. For each function: what its callers do when it is refused (a node that expects an actor back
may crash). The class list stays as a second net; a refusal at an engine function logs the node class, so a
class missing from the list is found.

Done when: each function is read and blocked, and a scripted run with every verb on logs the refusals with no crash.

Read (2026-11-17, research enemy/gaps/R24, R25, R26; facts/enemies.md 4.1 has each function's safe answer). The
design, built as one piece:
1. **Refusals.** At each function of facts/enemies.md 4.1, first thing in its hook: when `tl_act_actor` is set (and,
   where the function names its actor, that actor is the marked one), skip the body with the answer the table gives
   and log the function and return address once per pair. New hooks: the lifter throw main+0x1ba796c, drop now
   main+0x1b55b48 and reserve main+0x1b55bb4, the noise and terror producers main+0x1f40d34 and main+0x4cac5c,
   placementSetDead main+0x195321c's existing hook, the Bool64 setter main+0x2ab6dcc. main+0x1ba7710 and
   main+0x1bb16a0 come off the hook list when nothing else uses them. The mod's own replays run outside the mark and
   are untouched.
2. **Deny list.** The nine classes R24 found missing (facts 4.1) go into `coop_enemy_act_deny.inc`;
   OneShotEquipmentUserRequestChangeState stays off it.
3. **Marks by actor** (SYNC-006). Each decision keyed on a thread mark checks the call's own actor: the node tick under
   `tl_act_actor` compares the node's actor ([[[node+8]+8]+0x1f8]); the target reset main+0x24f0a9c compares [x0]
   with the held actor instead of testing that `tl_grace_bsa` is set (today it leaks into every later calc on the
   thread until the next enemy's, with no nesting needed). That holds whether or not one calc can run inside another,
   which reading did not settle (R26).
4. **The play limit** (both models, measured: most of the copies' extra and late arming, so late blows on the copy's
   game's Link). `coop_enemy_copy.c slots_apply` stamps its 12-frame limit at every play, so a command that follows
   another within 200 ms (Turn, then Attack) starts 200 ms late; the clock is then jumped to the owner's, which does
   not move the clip's events (facts 5.3). Stamp the limit only when a play did not take. Catching up without a jump
   across an event (main+0x1b569fc, read first) is left for after this step.

Built (2026-11-17), not run: the four parts above, in one branch. Part 1 is `coop_enemy_act.c en_act_refuse`, asked
first in `en_copy_hook`'s marked block, in `coop_spawn.c coop_hook` for the three creates and in `coop_life.c` for the
deaths and the Shooter's prepare; the six new hooks were checked in Ghidra as function entries of the shapes the
research gives, and the create's error-code pointer as the entry's [sp+8]. A death is refused under the mark alone
(R25), not only when it is about the marked copy: an action asks deaths of other actors too (its weapon by the remove,
a lifted actor, bullets). Part 2 adds the nine classes (276 now). Part 3 keys the node tick and the target reset on the
call's own actor (`tl_grace_actor`). Part 4 stamps `slot_try` only when the command read back after a play is not
the one asked. Each change was syntax-checked with clang; no game was built or launched.

Gate: the camp fight (`camp_melee.txt` for both Links) with the layer on and every verb on
(`TOTK_COOP_EN_ACT=1`, `TOTK_COOP_EN_ACT_VERBS=all`): no crash, each refusal logged by function and caller, and the
checker's list against the baseline above: attacks armed with and without the owner better than before (the play
limit), nothing new in the once-a-second list.

Gate run (2026-11-17, built from the step's four commits 0a88d075 to 7a54b8d7; 110 s, both Links fighting; pictures
from both games show the fight, each player's stand-in in it, the same guard effect in both):
- No crash, no FATAL. Refused in B (the copies' game), each logged once with its caller: a Shooter's prepare
  (from main+0x2452110), shoot (main+0x258e710) and sleep (main+0x2560898). No refusal "let through" for another actor.
- Attacks armed, much better (the play limit): with the owner 94, 100, 100, 100, 100, 100 and 90 % per copy (the
  baseline with both fighting: 22 to 93 %), without the owner 0 or 1 % (baseline up to 62 %).
- Worse: the copies game A showed of enemies game B ran moved with stutter 1.79, 1.97 and 4.74x and one place error
  p95 of 0.33 m (targets 1.5x and 0.3 m; the copies B showed of A's enemies, 0.22 to 1.31x; the baseline with the
  layer off, 0.91 to 1.29x). Whether the play limit or the layer with every verb
  causes it is not known: the same run with the layer off tells.
- Once a second, 9 differences held two seconds (the baseline, 8): new are two Bokoblins missing in B for 2 s and a
  rotation 27 degrees apart for 2 s; the bow held in one game only again (20 s).
- Blows: a Link's blow on an enemy in both games 17 times, in one only 7, another outcome once; one copy's blow on A's
  Link with its owner (B) not armed.
So the gate is not passed: no crash and the refusals work, arming is fixed, but movement got worse on one side.

The same fight with the layer off (2026-11-17, same build): stutter 1.01 to 1.24x on every copy that moved (place
error p95 0.05 to 0.12 m), attacks armed without the owner 0 to 4 %, no FATAL. So **the play limit is good and the
stutter is the layer's** (every verb on). Not read yet, a guess: what a copy's action does to its own body (its
controller's move, the physics an action asks) fights the placing of the copy at its owner's pose.

Read (2026-11-17, research enemy/gaps/R27): the guess was wrong. Nothing moves a copy off its placing (its place is
its owner's at the lag on almost every frame; "frames not at the pose asked" 0 throughout). The stutter figure was
the checker's: it set the jerk of a copy over the seconds its owner ran it against the owner game's whole run, which
includes the seconds that game showed the enemy as a copy. Fixed (`check.py` takes the owner's frames of the same
seconds); with it, the gate run's copies are 1.15 to 1.28x, under the target. What is real: in B, three enemies B
had run the layer on as copies and then took over **flew** after a blow (10 m up in 0.1 s, falling at 90 m/s,
sliding 30 to 66 m/s, at a partial ragdoll), and their copies in A showed it faithfully; nothing flew with the layer
off. Three causes stay possible: a jump request a copy's action leaves on its controller (OneShotPhysicsCctSetJumpHeight,
main+0x38011a8, makes the same request as the denied OneShotPhysicsCctJump, read; it is now on the deny list); the
controller's move state written by index each frame (`coop_enemy_act.c cct_write`), which skips the state change's
leave and edge flags; or chance (three cases). What the take-over does not undo: the controller's state and the
action's queue cell. Next: the same gate run (it decides the first), then, if flights remain, read `ChangeMoveState`
main+0x298f7a8, the jump module's vtable +0x60 and the result pass main+0x240440 before changing `cct_write`.

Gate run again with SetJumpHeight denied (2026-11-17, 5a0147cb): no FATAL; frames where an enemy moved more than
0.5 m: 58 (the first gate run 138, the layer off 0). Two of the three enemies that flew no longer do; the Dark
Bokoblin still flew in B (its owner then) after a take-over, and its copy in A showed it. So the jump request was one
cause and not the only one: the move state written by index (`cct_write`) or what a take-over leaves on the
controller stays, and the reading above comes next. Otherwise: stutter 0.82 to 1.48x on every copy measured (within
target), place error p95 under 0.3 m but the flier's 0.29; every copy's blow on a Link came while its owner had an
attack armed (5 of 5); a Link's blow on an enemy in both games 18 times, in one only 4; attacks armed with the owner
under 95 % for two Bokoblins game A ran; 8 differences held two seconds, as in the baseline (the bow held in one game
only, ragdoll states 2 to 5 s, short animation differences).

Read (2026-11-17, research enemy/gaps/R28): the flights are not the layer's. Every one (4 of 4, both gate runs) began
within 0.15 s of a partial ragdoll started on an enemy in a full one (or on the same frame), and the partial one was
the flinch our sync asks for another game's hit (`coop_enemy_hit.c`): a killing hit, or a hit while the enemy lay
ragdolled. It only met enemies game B owned, and B owned only enemies it had taken over (which had been layer copies
there), which is why it looked like the layer's; the layer-off run never met the pair. Fixed: no flinch for a
killing hit (`en_hit_phase0`) and none while the ragdoll state is 3 (`en_hit_precalc`). Built; not yet run.
SetJumpHeight stays denied (it is the same act as the denied jump).

**Gate passed** (2026-11-17, f931baa7; the same camp fight, layer on, every verb; 16 take-overs between the games):
- No crash, no FATAL. Refusals logged in both games: the Shooter's prepare, shoot and sleep, each with its caller.
- No flight: 4 single frames over 0.5 m in all (the first gate run 138, the second 58), none a launch.
- Movement within target on every copy measured: lag 70 ms, place error p95 0.04 to 0.08 m, stutter 0.94 to 1.09x.
- Attacks armed with the owner 97 to 100 % per copy but one Bokoblin at 72 %; without the owner 0 to 3 % (the
  baseline: 22 to 93 % and up to 62 %).
- Once a second, 8 differences held two seconds, the baseline's number and kinds: the Guard Bokoblin's bow in one
  game only (43 and 54 s), a Guard Bokoblin each game ran as its own (6 s), ragdoll states (5 s), two Bokoblins
  missing in one game for 2 s. The pictures show the fight in both games and nothing in the air.
- Blows: one copy's blow on a Link, with its owner armed; a Link's blow on an enemy in both games 18 times, in one
  only 6, with another outcome 2 (the layer-off run: 6 and 1).
Carried to step 3: the bow in one game only, the enemy both games run as their own, the run-time enemy not made again
(step 1), the Link's blows seen in one game only, and attacks armed late on one Bokoblin. The layer does help arming with the
owner: 94 to 100 % per copy with it, 81 and 43 % for two Bokoblins without it (a clip started at the owner's command
still arms by its own clock). Next: read what moves a copy's body while its action runs (`cct_write`, the
controller's move state and velocity, and the action's own movement requests) against `en_copy_precalc`'s placing,
before any change.

### 3. All slots and interrupts, for the Bokoblin family

The aim, from the start: requests of all five slots, interrupt requests, and the form key. Added from steps 1 and 2:
the five desyncs carried above. Read in research R29 (slots, interrupts, form), R30 (bow, two games running an enemy
as their own, the run-time enemy not made again) and R31 (a Link's blows in one game, late arming), 2026-11-17; their
code claims checked against `main` and Ghidra (main+0x3486a4) the same day.

What was found:
- **Slots** (R29 1). The action request (main+0x50ad74) takes its slot from the request; each slot has its own state,
  and only Main runs the move-control adjustment and the interrupt teardown. A Bokoblin uses Main for every action
  and Sub only for the lower body while riding a horse (verbs 24 Ride, 0 None, asked once at a state's enter and
  leave). It has no SP graphs. The camp fight has no horse, so its gate cannot show Sub working.
- **Interrupts** (R29 2). Today an interrupt request on the owner is told as a stop, so a hit sends its copies to
  Wait. Sent with the interrupt bit, a copy would tear down its own action and could stay in "interrupt requested".
  Sent with that bit cleared and the force bit kept, the light reactions (SmallHit, MediumHit, Repelled, Guard,
  GuardBreak, GuardJustRepelled, StealthHit, Paralyze, Combust, Fall, SightDown) enter the right root and act on
  nothing step 2 leaves open. The heavy ones (Die, Struggle, Thrown, HugeHit, CriticalHit, WeakPointHit, Smash,
  Frozen, BrokenArmor) switch the body to rigid-body mode and push it, R28's launch shape.
- **Form key** (R29 3). BSA+0x2628 is a pointer to text; the default builds no prefix. A Bokoblin's forms are
  `Sleep`, `HorseRide`, `RideSeat`, and the form decides which root a verb finds (`Sleep_InterruptSmallHit`). Nothing
  carries the owner's form today.
- **Bow** (R30 1). The copy drops its own bow: while the owner's Guard nocks an arrow its bow's attachment link holds
  the arrow, the copy's does not, and the held-things rule drops the bow "to make it again with the attachment". The
  two creates fail, the tries never reset, and a take-over by that game then deletes the real bow in the other.
- **Both games' own** (R30 2). Not a desync of what players see: the enemy rests because neither game runs it (parked
  by the game's battle LOD manager, or both Links far). The checker reads it as two owners.
- **Run-time enemy not made again** (R30 3). Nothing makes a copy again: the spawn sync makes copies only on the
  maker's one SPAWN, and the enemy sync binds only actors that appear. Also: the maker's SPAWN_GONE deletes the
  enemy everywhere even when another game owns and fights it.
- **A Link's blows in one game** (R31 1). The checker's: it asks for the stand-in's contact in the other game, which
  `contacts_filter` throws away by design. Life agreed in both games all run.
- **Late arming** (R31 2). Copies 20 m or more from their game's Link stop advancing their clip between the clocks
  `slots_apply` writes, so its events, the arm among them, never come. The game is not updating those copies'
  animation; what gates it is not read (main+0x348680 skips when bits 17 or 18 of the player's +0x124 are set).
  R29's copy calls answered 3 ("this slot already took a request this frame", 8 % and 33 % of calls) also show
  copies whose calc did not run between two of our calls [guess: the same throttle].

Design:
1. **Owner, every slot** (`coop_enemy_act.c en_act_owner`). Tell every slot, keeping the last verb, variation and form
   per slot. Mode 1 when any of the three changed, not only the verb. For slots 1 to 4, verb 0 (None) is a real
   request and is told. Light interrupts are told with an interrupt mark; heavy ones and every other unlisted
   request stay a stop, as today.
2. **Message** (`MAct`). Add the slot and the form text (32 bytes). Checked on receipt: slot below 5 (5 and over run
   on Main), verb in range, flags only bits 0 and 8, texts ended and printable, the form one of the family's known
   ones or empty.
3. **Copy** (`en_act_precalc`). One queue per copy and slot. Each frame: set the form if it changed (main+0x88d524,
   then main+0xd141fc, outside the action mark; the text kept in a table the mod never frees), then call Main, then
   the other slots that hold a request. The interrupt bit is cleared, the force bit kept. Stop per slot: Main asks
   Wait with mode 1, as today; slots 1 to 4 ask None the way the game's own fill does (mode 0, +0x24 4, +0x28 0x11c).
   The half-second rule ("nothing told: Wait") stays for Main only; other slots hold their request until told. A
   take-over leaves slots 1 to 4 with None.
4. **Bow** (`coop_enemy_copy.c`). `equip_read_more` reads no attachment for slot 3. `eq_tries` and `eq_att_tries` go
   back to 0 after 10 s, and a create's failure bit (eu+0x4600, bit 0x200000000000, research G8 3.2) is logged.
5. **Run-time enemy made again** (`coop_enemy.c` phase 0, a new `coop_spawn_sync.c coop_spawn_remake`). For an entry
   that is run-time, alive, owned by another game, has no actor here and has a name: when the owner's latest STATE
   place is within 60 m of this game's Link [guess: inside a Bokoblin's unload radius, facts 13.5 open] and no
   remake was asked in the last 10 s, make it by name at that place under the same key, through `spawn_take`'s
   create path. `cell_resolve` then binds it as a copy and `far_sweep` covers it. The maker sends SPAWN_GONE only when
   no other game owns a live entry of that key. The test foe's keep from its own far delete applies in every game,
   not only the maker's, so the test does not bounce between games.
6. **Checker.** `coop_check.c role()` writes "r" for an OWN cell this game does not run (a reader of `runs()` in
   `coop_enemy.c`); check.py lists "owner" only when no game has "o" and one has "l". A Link's blow on an enemy is
   judged in the striker's game; the other game's stand-in contact is listed, not judged. A per-frame line for each
   stand-in (its weapon attack byte +0x660 and weapon place) is recorded as information.
7. **Far copies' clips** (research R32, 2026-11-17). Bits 17 and 18 of the player's +0x124 are not the throttle
   ("graph not built", "stopped at clip end"); the other AS_update callers are not a slower path; the calc is entered
   every frame and AS_update inside it is skipped by one of four gates, one of which (the 0xc4 pause masks on
   actor+0x3fc) would also explain R29's "answered 3". The stall turns on and off per copy in blocks of 10 to 40 s,
   more often at 20 to 35 m but also at 5 to 15 m. Its writer is not found, so no switch can be held. The calc's
   per-slot status loop does run on a copy. The change: AS_update adds 1 to G+0xd8 on every run (main+0x432fa0, read
   in Ghidra). At the calc-entry hook, a copy within 40 m of any Link shown in its game whose G+0xd8 did not move
   since the previous calc gets one AS_update call from us (x0 = G, s0 = the calc's dt times [AC+0x98], as the calc
   does at main+0x41ca18..0x41ca30; main+0xb51440 is the game's own call outside the calc). A once-a-second line per
   copy logs which gate held (R32 5.4's fields), so the gate run both checks the change and names the gate. Unread:
   whether the arm call (main+0x421a7c) is reached on the pause-mask path; if the run shows events but no arming, that
   is next.

Done when: the camp fight with the layer on, every verb, all slots and the light interrupts runs with no crash; no
`lets go of its Weapon_Bow_004` line in either game; no "owner" finding for a resting enemy; every copy within 40 m
arms with its owner at 95 % or more; and the checker's difference list is read and explained. A session run
(`run_session.ps1`, the test foe) shows the run-time enemy made again in the game that lost it.

8. **A Link's hit effects in the other game** (added 2026-11-17 at the user's request). A Link's blow counts only in
   its player's game; the other game throws its stand-in's contact away (`contacts_filter`), so whether the other
   player sees the hit spark and hears the hit sound depends on where those start: from the contact, or from the
   enemy's reaction. Read first where a blow's spark and sound start; then the checker records them in both games and
   compares them, as it does effects started by key.

   Read in research R35 (2026-11-17; its call site checked in Ghidra): the spark and the hit sound are one "Hit"
   request to the game's reaction system (main+0x2b4f008 on [main+0x395f518]+0x30), made by the struck body's own calc
   for each entry of its received-hit list after the damage step (main+0x1b90cb4 from main+0x422a18, then
   main+0x1b91844, which picks the type, Hit, Guard, GuardJust, HitIneffectual and others, from the victim's results).
   So the striker's game shows them and **the other game shows neither**: its stand-in's contact is taken out of that
   list. The checker never saw this: the request does not pass ELink/SLink, and no run has recorded one "x" or "s"
   event at all. Design (a later batch, after the R34 batch passes; two new hooks, so a full rebuild):
   - the striker's game sends, per blow of its Link on an enemy, the request it posted (type, flags, properties, place
     and normal relative to the enemy) in a new message; the other game posts the same request with its own users
     (the stand-in's and its enemy's). Sent properties, not recomputed, so a guard plays as a guard;
   - the checker records each "Hit" request ("r": victim, attacker, attack id, type) in both games and compares: an
     "r" in the striker's game within 2 frames of each blow, and one for the same enemy in the other game within
     0.3 s with the same type;
   - read before building: main+0x1b946f4, which runs after the requester's post on the struck actor [guess: the
     hit-stop, which would also be R34 item 4's missing pause], and which user is the source of a sword blow.

Built (2026-11-17, merge 606f6f71: seven commits, reviewed, the review's eight findings fixed; no warning; a second
review's four findings fixed in 269c915d). Not checked in code: the request's +0x24 field from the other game, and the
known form keys leave out keys that come through graph links.

**First gate run: FAIL** (2026-11-17, 269c915d; the camp fight, layer on, every verb; records in `build/gate5`):
- Good: no crash. No bow difference and no "owner" finding. Light interrupts told as forced requests. Movement within
  target on every copy (lag 60 to 70 ms, error p95 0.03 to 0.28 m, stutter 1.04 to 1.20x).
- Bad: attacks armed with the owner fell for the copies of game A's enemies in game B: 9 % (p8e74ea9), 38 %
  (p5cb495b), 75 % (the Dark Bokoblin, also 17 % armed without its owner), against 97 to 100 % in step 2's gate. At the
  missed attacks the copy's clip advanced every frame, so it is not the clip stall. Not found yet. Signs:
  - game B logged copies' Main calls with the slot's graph never run for whole seconds ("60 calls, its graph run 0
    times");
  - each game logged ten (the log's cap) action requests from the other refused by the new message checks, with no
    reason given;
  - the logs cap most action lines at their first few dozen, so they cannot show which requests were lost.
- Part 7's record names the stall: while a copy's clip stood, the game ran its AS_update about 8 times a second (not
  never), with actor+0x3fc 0x22901 against 0x800 or 0x22800 when it ran fully, and LOD+0xb6 bit 7 clear (0x40, 0x48
  against 0xc0, 0xc8). No pause mask of gates B or C was set. The 30-calc wait added after the second review stops
  the mod's call for such a copy, since the game's 8 runs a second reset the count; the wait has to go or change.
- One copy's blow on a Link with its owner not armed (game A, paced24f).
- Still in the list: ragdoll states for 4 to 6 s (three enemies), one shield held in one game for 2 s, one Bokoblin
  missing in one game for 2 s after dying.

Cause (research R33, 2026-11-17; its two key reads checked in Ghidra): not the action layer. A copy whose
actor+0x3fc has bit 0 or 1 set takes the calc's reduced body (main+0x41bb8c), which reaches neither AS_update nor the
arm poll (main+0x42ec98, from main+0x421a7c), nor the slots' per-frame clear (hence "answered 3" and "graph run 0").
Every missed arm fell in seconds with bit 0 set; copies in the full body armed 96 %. Step 2's gate had the same zero
for a copy 20 to 25 m off, and passed only because its fight kept copies nearer. Separately, every Run (verb 4) request
was refused by the new checks [guess: the float size limit]. Fixed: part 7 now acts exactly when the reduced body is
taken (read at the entry), making AS_update and then the arm poll as the full body does; the 30-calc wait is gone; the
request checks no longer limit the numbers' size, and a refusal logs which check and the verb. What sets bits 0 and 1
is not read. Built; the gate is to be run again.

Second gate run (2026-11-17, d8beed7c; records in `build/gate6`): not a valid gate. The PC was busy with another game,
and both test games ran at 32 to 34 fps (1 % low 14 to 15) against 59 to 60 in the first run. Lag, stutter and the
command and phase shares fail on that and are not judged. What it does show: no crash; attacks armed with the owner
91 to 100 % per copy (from 9 to 75 %), the mod's update and arm poll rarely needed (most copies stayed in the full
body at that frame rate); no bow difference, no "owner" finding. The refusal log named the check: Run (verb 4) and
verb 1 requests carry flag bits 1 and 2 (0x2, 0x6), which "bits 0 and 8 only" refused; the number-size limit removed
before was not it. The check now passes every bit but the interrupt bit as asked, as step 2 did (their meaning is
not read). To be run again on a quiet PC.

Third gate run (2026-11-17, f543f837; records in `build/gate7`; both games 59 to 60 fps): FAIL, near. No crash, no
request refused, no bow difference, no "owner" finding. Movement within target on every copy measured (lag 60 to
70 ms, error p95 0.05 to 0.09 m, stutter 0.54 to 1.06x). Attacks armed with the owner 100 % on every copy but one
(p5cb495b's copy in A: 88 %, and 9 % armed without its owner). What fails:
- command same under 95 % on three copies in game A of enemies game B ran, each of which moved little (87 to 91 %);
- one copy's blow on a Link with its owner not armed (A, p8e74ea9 at 29293.2);
- once a second: a Guard Bokoblin's slot 0 "Run" in A against "Wait" in B for 6 s; ragdoll states for 2 to 6 s
  (three enemies); one Bokoblin in A only for 2 s and one with differing death words for 4 s, both around a death.
Not explained yet: the logs cap the action lines, and none of the Guard's lines in A show the 6 s.

Explained (research R34, 2026-11-17; its code claims checked):
- Checker artefacts: the command share of a copy that moved under 2 s is taken at lag 0, so every owner change counts
  the copy's normal 70 ms delay (at 70 ms the three pass, 95.6 to 99.1 %); the Guard's "Run against Wait" is six
  separate seconds whose samples fell on a command change; a copy's ragdoll state reads 0 by design while its owner's
  bones show the ragdoll; a take-over's second is judged against the old owner.
- Real, small: (5b) a hand-over is offered in the same pass that applies the striker's hit, so the flinch plays in the
  game that gave the enemy away and not in the striker's; (5c) a copy's death word takes a fallback second cause,
  since the DEATH message's word is not used; (5d) a copy still waiting at death phase 3 when its owner's actor goes
  is freed and runs its own Die and ragdoll 2 s longer, sliding 4 m.
- A timing limit: (4) after the other player's blow the owner's clip stands about 0.1 s (hit-stop) and the copy's does
  not, so it swings about 43 ms early; one episode in six, enough to fail 95 %.
- Open: (3) a blow on L1 named the Junior copy (nothing armed, 6.6 m off) while the Dark Bokoblin beside L1 had a rising
  horn blow armed. Which body struck is not in the record.

Fixes (one batch, one build):
1. check.py: a copy whose lag was not measured is compared at a nominal 70 ms; a take-over's second is left out of the
   per-frame comparison; an "anim" difference skips a second where either side's sampled clock is below the copy's
   delay; "ragdoll" counts only where the owner's state is 0 and the copy's is not. Shown on `build/gate7` (those
   findings go, 5b's stays) and on step 1's planted-desync run `build/gate1` (it still fails).
2. coop_enemy.c handover: no offer while the entry has an unapplied hit or the actor's ragdoll state is not 0 (5b).
3. coop_enemy_hit.c: ENM_DEATH keeps the owner's death word, and a copy's death request takes cause and second cause
   from it when no stated state has one yet (5c); ENM_DEATH_END gives a copy waiting at phase 3 the signal before its
   entry is freed (5d).
4. coop_enemy_copy.c slots_apply: when the copy is ahead, the rate bend may go down to 0, so an owner's stand-still
   holds the copy (4). Stutter is watched in the gate run.
5. Logs, before the next run: each contact on this game's Link from an enemy (attacker, striking object, type, level,
   power, attack id, attacker place and ragdoll state; item 3); the hand-over line with the unapplied hit and ragdoll
   state; the copy's death line with where its word came from, and the phase at DEATH_END; a setting that lifts the
   action lines' caps for one named enemy.
Item 3's own fix waits for its log line. Still to run for step 3's Done: the session run with the test foe (part 5).

Fourth gate run (2026-11-17, merge 7c0d0e6f; records in `build/gate8`; both games 59 to 60 fps): FAIL on one item.
No difference held two seconds; movement within target on every copy (lag 60 to 70 ms, error p95 0.04 to 0.09 m,
stutter 1.12 to 1.27x); command same 99 to 100 %; every copy blow on a Link with its owner armed; no crash. The fixes
fired: two hand-overs held for a hit or flinch, eight "became a copy" with nothing under way, three copy deaths from
the owner's DEATH word, three copies given the end signal. The pictures show the fight in both games, nothing in the
air. What fails: the Dark Bokoblin's copy in B armed with its owner just under 95 %. All 14 of the owner's arm windows
had a copy window, but from 32529 on the copy's windows ended up to 0.09 s before the owner's (in gate7 they ended at
the copy's lag after it). Part 7's record names the cause: in eight seconds the copy's every calc took the reduced
body and the mod made AS_update each frame, while the game also ran it 24 to 42 times a second by a path R33 did not
find; the clip stepped twice in those frames and ran ahead. Fixed: the mod's AS_update now fills only a frame the game
left without one (G+0xd8 unmoved since its previous entry, our own call not counted). Built; to be run again, with the
session run. Item 3 decided nothing this run (no blow without its owner armed).

Fifth gate run (2026-11-17, e4daee10; `build/gate9`; 59 fps): FAIL on two items. The double step is gone: the Dark's
copy now gets about 60 AS_update runs a second in every state, ours filling the reduced-body frames. Movement, commands
(99 to 100 %), copy blows (6 of 6 with the owner armed) and every other copy's arming (96 to 100 %) pass. Failing:
- the Dark Bokoblin's copy in B armed with its owner 91 %: every owner window has a copy window, but in the seconds its
  calc takes the reduced body (actor+0x3fc 0x2901, ours driving) the copy's windows start 8 to 34 ms after the
  owner's instead of the usual 60 to 90 ms and end 30 to 90 ms early: its clip runs a few frames ahead while the mod
  drives it. Not read yet;
- the Dark's shield (slot 4, Weapon_Shield_006) held by its owner in A and not by its copy in B for 15 s from 32915.
  Not read yet.

For part 5's session run, a test setting: `TOTK_COOP_TEST_FOE_DROP=<s>` makes a game delete its own actor of the test
enemy once, s seconds after it first holds it as a copy, since the test keep now refuses the far delete that lost it in
step 1's run (coop_spawn_sync.c foe_drop_test). Built; not yet run.

Session run (2026-11-17, be8e87d9; `build/session3`; game A made the test Bokoblin at 20 s, game B dropped its copy
10 s after it had one): FAIL. The drop worked and the re-make fired at once ("lives on in player 1's game and has no
actor here: made again here", 2.1 to 2.6 m from B's Link), but every create it asked stayed pending (handle status 2)
for its 10 s and was given up ("could not be made (2)"); the first copy, made from A's SPAWN, had been made at once.
The enemy was "missing" in B 39 s to the end. The re-make also logged "made again" once before the first copy was
bound, harmlessly (that create was already in flight). Why a later create of the same name stays pending is not read.

Explained (research R36, 2026-11-17; its key reads checked in Ghidra and the code), and fixed (built):
- The re-make's creates did not stay pending: each was given up in the tick it was asked. `spawn_tick` read `now`
  before `remake_tick`, the create was stamped after it, and the unsigned "given up after 10 s" test wrapped. Fixed:
  `now` is read again after `remake_tick`.
- The shield (and the Guard's bow since step 2): the copy's equipment create passed the holder's slot (4, 3) where the
  game wants an equipment kind (8 shield, 7 bow, 0 weapon; the game's own node at main+0x25237d8 / 0x25238d4).
  Accepted, never filled. Fixed: the slot is mapped to its kind, and slots with no kind read are not made.
- The Dark's early arms: the game's cheap animation step (main+0x348680, for an on-screen actor) advances the clip
  nodes' own times without raising events or counting G+0xd8, so part 7 saw "missed" and stepped the node times a
  second time; the events came at about twice the clock. Fixed [guess, to be confirmed by the run]: after the mod's
  AS_update, G+0xe0 is set to 1, which makes that cheap step run the full AS_update itself (events raised, counted),
  so the mod stays out.

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
