# Co-op RE notes (TotK 1.4.3)

Addresses are offsets from `main`'s base (Ghidra shows `main` at 0x7100000000, so main+0x599670 is 0x7100599670).
Names are ours (Ghidra project names start with `coop_`). Confidence: confirmed (tested here), likely (strong static
evidence), guess.

## Player actor: getting it each frame
- Address: [main+0x3add238] -> +0x1e8 scene -> scene+0x58 components[13] = resident actor list; entries at
  +0x28, stride 0x70, entry+8 -> data, data+0x40 -> actor; actor+0x218 -> name, "Player" is Link.
- Evidence: runtime/frame_rate.c `player_pos` (from MacNativeGames world_survey.c); logged on 2026-11-03 with
  `trace = perf`, `speed_probe = 1`: position followed the walk on the Great Sky Island, 1-3.7 m/s walking.
- Confidence: confirmed. On the title screen the actor exists at (0, 0, 0).

## Player position / rotation
- actor+0x2b4: position, 3 floats (confirmed, same log). actor+0x2c0: rotation, 3x3 floats (likely: MacNativeGames
  game_version.h, "setTransform writes +0x2b4 / +0x2c0").

## coop_ForceSetMatrix
- Address: main+0x45ce70. (actor, const float m[12] rows of rotation|position, u32 0): teleports an actor.
- Evidence: MacNativeGames world_survey.c `force_at` / FORCE_MATRIX for 1.4.3. Confidence: likely (not tested here).

## coop_ActorMgr_requestCreateActor
- Address: main+0x599670 (in the recomp main is at 0x7000000000, layout.h MOD_MAIN_BASE). Args (as the AI node calls it): x0 = actor manager (*[main+0x395e008]), x1 = &name
  pointer, x2 = create argument (position at +0, rotation 3x3 at +0xc, ...), x3 = handle to fill, w4 = 1, rest 0.
  Create argument (copied by main+0x59a9f0): +0x00 position, +0x0c rotation 3x3, +0x30 scale, +0x40 a string
  pointer read without a null check (null crashes: test 2026-11-03), +0x58/+0x60 optional data, +0x90 flags (bit 0
  position, bit 1 rotation, bit 2 scale given). Stack args: [sp] byte, [sp+8] u32* result code, [sp+0x10] out.
  Returns 1 when the request was queued; the actor is created later (the handle reports when).
- What it does: builds "Pack/Actor/<name>.pack", loads it under the "ActorMgr" heap tag and queues the actor's
  creation. Fails with a code in *x9 (1..8) when the manager is closed, the name is empty, the pack fails, etc.
- Evidence: the AI node "ExecuteGenericRequestCreateActor" (name getter main+0x231fd48, vtable main+0x3770d88) calls
  it from its enter function main+0x231fa58 (`coop_NodeGenericRequestCreateActor_enter`); the strings "Pack/Actor/",
  ".pack", "ActorMgr" inside it. Confidence: likely (not called by us yet).

## Hooking from the recomp
- `rt_call(Cpu*, fn, n, ...)` (runtime/host.h): host code calls a guest function; needs the guest thread's Cpu.
- HOST_OVERRIDE / HOST_OBSERVER entries (runtime/host.h, hostfn.c): replace a guest function, or run host code and
  then the original (`rt_original`).

## Spawn test results (2026-11-03, runtime/coop_spawn.c)
- Request -> handle status 2 (loading) -> 3 (created, waiting) within a few frames; claiming it with main+0x1877160
  (handle, link out, 1, 0) puts the actor in the world. The link is 16 bytes; [0] stays 0, [1] holds a pointer.
- Item_Fruit_A (apple) and Npc_Kokiri007 (Korok): appear in front of Link. Confidence: confirmed (Sinnik).
- Player: created and claimed, then the next game frame crashes in main+0x4b46a4 reading +0x228 of a null actor.
  That function walks the resident actor list (scene component 13) for "ThrowBox" and takes an actor from the entry
  it finds (or a default entry). Cause and fix: next section.

## Actor dependencies (why the second Player crashes)
- coop_ActorLink_getActor main+0x2804d0 (reference, caller actor, out flag): on a worker thread it returns the target
  only when caller and target are connected by update-dependency edges (main+0x281168 walks parents, main+0x2808f0
  children); otherwise null. main+0x4b46a4 asks for the resident "ThrowBox" and uses the result without a null check.
- Resident list dump (2026-11-03): spawning Player#2 leaves the list unchanged (ThrowBox is the same actor, state 3
  before and after), so the null is the access rule, not a replaced entry. Confidence: likely.
- Edges: actor +0x1c0 u16 ring (+0x1c8 capacity, +0x1cc start, +0x1d0 count) of indices into
  [[main+0x395da68]] +0x2f8 (16-byte {parent, child} entries, +0x2f0 count).
- coop_SetDependency main+0x5b811c ([main+0x3ad6d80] actor system, parent, child): queues an edge request (queue at
  system +0x310, capacity +0x318, start +0x31c, count +0x320; mutex +0x348). The AI node
  "OneShotActorRequestSetDependency" (execute main+0x24f4f34) calls it through main+0x85f7d8 (IsParent) or
  main+0x4fc3f0. Confidence: likely.
- Fix (2026-11-03): after the claim, every edge of the real Player (17: Obj_OneTouch_Connection, WakeBoardRope,
  the equipped weapon/sheath/armour/quiver, Accessory_SupportDevice/Battery, PlayerRaulHand, Parasail,
  Dm_Npc_RevivalFairy, CarryBox, ThrowBox) is queued again with Player#2 as parent (runtime/coop_spawn.c
  copy_dependencies). Result: a second Link stands in the world and takes a combat stance at a Soldier Construct
  next to the first one; no crash. Confidence: confirmed (Sinnik, screenshot).
- Open: Player#2 is now parent of Player#1's own equipment; it needs its own, with only the shared helpers copied.

## Input (2026-11-03, unattended tests, tools/coop/run_test.ps1)
- Link#2 copies every input of Link#1 (Sinnik): both Players read controller 0.
- coop_getController main+0x37ccc0 (mgr = [[main+0x3960150]], index, &user name): index 0 -> [mgr+0xa0], 1..3 ->
  [mgr+0xf0 + 8 * (i - 1)], 4 -> [mgr+0xa8]; a registered "Custom_Controller_<name>" or flag bit 13 of +0x13c -> +0xc0.
  Per-frame callers: main+0xb012fc, main+0x37c980 / main+0x37c9b8 (UI), main+0x100d07c (Camera), main+0x5e197c (UI);
  "Player" asks only at load (main+0x4ced10). Neither Player stores the controller pointer (scan, 3 levels).
- Controller layout (controllers 0 and 1 share vtable main+0x394c210; diffs while the input script held buttons):
  +0x114 and +0x13c held mask (bit 0 A, 2 ZL, 3 X, 20 left stick up), +0x10 repeat mask, +0x30 + 4 * bit frames held
  (float), +0x120 / +0x124 left stick x / y (-1..1), +0x110 a running counter. +0x8 / +0xc pressed / released and
  +0x128 / +0x12c right stick are guesses (sead order).
- main+0x479c20 rewrites every controller from the Npad slots continuously, also while actors update; writing a
  controller's fields during a Player calc does not stick (tests BLIND=3, BLIND=2 + fill: no effect).
- coop_ActorCalc main+0x41bb20 (x0 = actor), called from the actor job main+0x2f175c: Link#2 then Link#1 on the same
  worker thread every frame (calc log). Found with the function profiler (cpu.h FN_ENTER, TOTK_COOP_PROF).
- Working: while Player#2's calc runs, [mgr+0xa0] points at a controller of our own (patch page +0x400: a copy of
  controller 0 with player 2's buttons and sticks) and is put back after (TOTK_COOP_BLIND=4, runtime/coop_spawn.c
  calc_wrap). Test: player 1 idle, player 2's test pattern walked Link#2 31 m forward then right; Link#1 did not
  move. Confidence: confirmed (log and screenshots).
- Reporting a second Npad (id 1) to the game stalls it: from the start the title never reaches the world; plugged in
  later the actor calcs stop (cause not traced). Player 2 reaches the game only through the controller swap.
- Single-player leftovers seen: the HUD shows Link#2's prompts (Jump / Let go while Link#2 climbed).
- Tools: coop_watch_win.c (software read watchpoint: page protection + trap flag), the entry hooks (COOP_HOOKED in
  runtime/cpu.h; a hook may run the function itself through rt_dispatch and return 1), TOTK_COOP_* settings
  (AUTOSPAWN, WATCH, PROF, BLIND, P2, P2_NPAD, CTRLDUMP).
- Controller bits (button-map test, tools/coop/button_map.txt): A 0, B 1, ZL 2, X 3, Y 4, ZR 5, right stick click 6,
  left stick click 7, L 13, R 14, d-pad up 16, down 17, left 18, right 19, left stick up / down / left / right 20-23,
  right stick 24-27. Plus / Minus not measured.
- Player 2 now: the second connected gamepad by default (TOTK_COOP_P2=pad; input.c input_pad2_read), through the
  private controller. Test 2026-11-03: player 1's script and player 2's test pattern moved the two Links on their own
  paths at the same time. Real second gamepad: works (Sinnik, 2026-11-03: player 1 DualSense, player 2 Xbox One Elite 2).

## Equipment, HUD, camera (2026-11-04)
- Equipment: Player#2 creates its own weapons, armor, quiver, Zonai devices, PlayerRaulHand, Obj_OneTouch_Connection
  and WakeBoardRope (different actors, its own update links; edge lists by address). Only the resident helpers
  (ThrowBox, CarryBox, Parasail, Dm_Npc_RevivalFairy) are one per game, so only edges to resident actors are copied
  now. Both build their gear from the same inventory (the save's), so they look alike. Confidence: confirmed (logs).
- HUD: no global in main's .data/.bss points at either Link (scan); the button guides ("Jump / Let go") seem to be
  pushed by whichever Player's update asks (guess). UI names: CtrlGuide (UICtrlGuideScreen, game__ui__CtrlGuideSet,
  HoldSensorHitInfoSend*CtrlGuide). Not fixed.
- Camera: PlayerCamera (resident) holds the followed Player at [cam+0x1a8]+0x160, [cam+0x858]+0x100,
  [cam+0xa20]+0x790, [cam+0x1fc0]+0x1c0 (scan for Link#1's actor). Writing Link#2 there each poll makes the camera
  follow Link#2 (walks and climbs, screenshots); Numpad 4 switches, TOTK_COOP_CAMAT=<s> for runs. Which of the four
  are needed is untested (TOTK_COOP_CAMMASK). Confidence: confirmed for all four together, in one run only.
- Correction (2026-11-04): not reliable and harmful. Switching stripped Link#1 of hair, Zonai arm and armor (Sinnik's
  screenshot). Later scans found other places (one run: a single Link#1 *reference* at [cam+0x3a0]+0x48, another at
  [cam+0x3590]+0x298), and with all four written the camera stayed on Link#1. The four were caches, at least one of
  them shared with Link#1's model. The switch is off by default now (TOTK_COOP_CAMMASK unset). Next: find how the
  game sets the camera's target (camera or event code that takes an actor), instead of patching memory.
