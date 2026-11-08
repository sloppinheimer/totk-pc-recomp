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

## Glue by hand in the other game (2026-11-08 evening, the tester's live session)
- the tester glued fan 3 to fan 1 by hand in game 1 (logs of build143 and build143play, 16:33): game 1 sent bond 1
  and "a contraption of mine (2 parts)"; its glue entry was "no longer glued here: taken off the list" a few seconds
  on, and game 2 then "parted here" (removeScotch), which undid coop_bonds' bond: "show no shared group here yet".
  Game 2 also made "a copy of object 4 (UltraHand_Connection_Effect2)", and the tester saw a large green slab between the
  glued copies and a green glow in a loose fan's copy.
- Ultrahand's release makes the lasting bond in main+0x1d67060 (called from main+0x1d61ca0): it takes both parts'
  SpecialPowerReceiverComponents, checks the two contraptions together stay under 22 parts, sets the target's +0x20 bit
  0x1000 and ends in the bond maker main+0x1916830, as the Autobuild does; the glue entry (createScotch / doScotch) is
  only the joint while it is made. So a glue by hand's parts share a group (as an autobuild's), and coop_bonds' bond
  (main+0x1c05a98 -> main+0x1916830) on the copies is the same kind.
- UltraHand_Connection_Effect2 is one actor per Link, made by its glue-effect manager (main+0x994048, which keeps it at
  manager+0x400 and gives its EffectInfo the manager): it draws every glue joint from the manager's data. A copy by
  name has no manager: hence the slab. It is no longer copied (prop_skip, and ignored if a sender of before sends it).
- The glue's look of the joint itself: createScotch's tail (main+0x1d61858) copies four vectors into the entry
  (+0x51c, +0x528, +0x534, +0x540) and sets +0x554 bit 1 when the request's +0x20 answers 1 (Ultrahand's does; ours, the
  base class, does not). They are now captured with the bond (GlueBond.look, pts) and written into the entry made in the
  other game.
- coop_glue now reads a bond as held while its entry is glued or its two objects share one group (same_group), on the
  maker's side (no removal sent when the entry goes) and on the other side (no glue made again once bonded).
- Not checked yet: the look with these changes (needs a glue by hand in the tester's next session), and world objects
  both games have (coop_bonds bonds copies only).

## A failed create request must be released (fixed 2026-11-08)
- a play session (main d84dcc4, game 2 from build143play): after a fast travel game 2 made no actor at all
  (copies, its own capsule devices: the item left the inventory and nothing came), and the ground was missing. Its
  log: "the copy of object N could not be made" 1498 times, for four objects only: keys f36511da, fdad097d, 8f82aab6,
  f1b2abdf, which game 1 logged as "I move MergedActorD100": the map's merged-mesh groups, moved as world objects;
  game 2 no longer had them after its fast travel and asked for copies by that name. No such actor can be made (handle
  status 4), and the copy was asked for again at once, every time.
- A request's handle holds a record of the actor manager's. The game's own create node (main+0x231fba0) on a failed
  one calls main+0x2ae4d2c(handle): main+0x5557e0(record, 0) (status 4 or 9: main+0x5559a0 frees it; 2, 3, 5..7:
  cancelled, then freed) and the handle cleared. A claim clears the handle itself (main+0x89fe48). Our code left failed
  records in their handles and zeroed them at the next request: each failure leaked one.
- Shown (TOTK_COOP_TEST_FAILSPAM=35,3000 in game B: a MergedActorD100 asked for again each time it failed;
  TOTK_COOP_TEST_DEVSWEEP=50 with 14 fans, one every 10 s): the old way (TOTK_COOP_TEST_NORELEASE=1) after 1491 failures
  requestCreateActor refused every request and none of the 14 fans was made (status 0); released, 2999 failures and all
  14 fans made.
- Fix: coop_game.c coop_handle_release (the game's main+0x5557e0), called on every failed copy and spawn-sync request,
  and by request_create on whatever its handle still holds; a copy that fails is asked for again after 2 s, 4, 8 ... up
  to 60 s (coop_players.c copy_failed / copy_wait), not every tick; MergedActor is never sent nor copied (prop_skip).
  Not reproduced: the status 4 of a fast travel itself (our test travel and copies deleted 2 km away were made again
  fine); the missing ground is taken to be the same exhaustion (not shown).

## Hooking from the recomp
- `rt_call(Cpu*, fn, n, ...)` (runtime/host.h): host code calls a guest function; needs the guest thread's Cpu.
- HOST_OVERRIDE / HOST_OBSERVER entries (runtime/host.h, hostfn.c): replace a guest function, or run host code and
  then the original (`rt_original`).

## Spawn test results (2026-11-03, runtime/coop_spawn.c)
- Request -> handle status 2 (loading) -> 3 (created, waiting) within a few frames; claiming it with main+0x1877160
  (handle, link out, 1, 0) puts the actor in the world. The link is 16 bytes; [0] stays 0, [1] holds a pointer.
- Item_Fruit_A (apple) and Npc_Kokiri007 (Korok): appear in front of Link. Confidence: confirmed (the tester).
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
  next to the first one; no crash. Confidence: confirmed (the tester, screenshot).
- Open: Player#2 is now parent of Player#1's own equipment; it needs its own, with only the shared helpers copied.

## Input (2026-11-03, unattended tests, tools/coop/run_test.ps1)
- Link#2 copies every input of Link#1 (the tester): both Players read controller 0.
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
  paths at the same time. Real second gamepad: works (the tester, 2026-11-03: player 1 DualSense, player 2 Xbox One Elite 2).

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
- Correction (2026-11-04): not reliable and harmful. Switching stripped Link#1 of hair, Zonai arm and armor (the tester's
  screenshot). Later scans found other places (one run: a single Link#1 *reference* at [cam+0x3a0]+0x48, another at
  [cam+0x3590]+0x298), and with all four written the camera stayed on Link#1. The four were caches, at least one of
  them shared with Link#1's model. The switch is off by default now (TOTK_COOP_CAMMASK unset). Next: find how the
  game sets the camera's target (camera or event code that takes an actor), instead of patching memory.
- Camera trace (2026-11-04): PlayerCamera's calls (main+0x41bb20, 0x2e27b0, 0x245030, 0x257310, 0x2f7f70, 0x19557fc,
  0x6c9ad0 with x0 = PlayerCamera; profiler TOTK_COOP_PROF=2) make no ActorLink getActor (main+0x2804d0) calls.
  Watch from inside them only (TOTK_COOP_WATCH=4/5): Link#1's reference read by main+0x1b0614 (ActorLink assign:
  (dst, src, flag)), main+0x1afbe4 (ActorLink lock-and-get: (out, link); a link is {+8 reference object, +0x10 its
  id}) and main+0x87509c, a few times a second; Link#1's actor read only at +0x228 (a pointer: the crash of
  main+0x4b46a4 read the same field) by main+0x87509c and main+0x876670, a camera component ([x0+0x38]+0x18 chain).
  Redirecting 0x1afbe4's link inside the camera changed nothing (0 hits on Link#1). Not solved.
- Design note: online, each PC's own Link is the original Player (camera, HUD, input as in single player) and the
  other player is the spawned second Player, so the camera follow switch only matters for one-screen play.

## Networking v1 (2026-11-04)
- Relay server/coop_relay.py (UDP 41900, up to 8 players, join / leave any time); client runtime/coop_net.c on its own
  thread (TOTK_COOP_RELAY, TOTK_COOP_NAME); 30 states a second: position, rotation, the camera's forward / right, the
  Switch buttons and sticks (NetState, 96 bytes).
- Player slots (runtime/coop_players.c, helpers in coop_game.c): a remote player's Link is spawned when their first
  state arrives; its calc runs with a controller of its own filled from their input, the stick turned from their
  camera into this PC's camera (PlayerCamera rotation: looks along -Z, X is its right); ForceSetMatrix (main+0x45ce70)
  puts it back when it drifts more than 3 m.
- Test (tools/coop/run_pair.ps1, two games on this PC): both joined, each spawned the other's Link; game A walked its
  script and game B's copy of A followed the same path within about 1 m (A 386.3,1320.8 -> 397.5,1351.9; B's copy
  386.6,1319.8 -> 398.4,1350.9). ForceSetMatrix's snap path did not trigger in it (untested). Confidence: confirmed
  for walking.
- Drift (the tester, 2026-11-04, two windows): input alone left a remote Link about 2 m off (other side of a staircase).
  Now put back past 1.5 m while its owner moves and past 0.25 m once they stand still, at most every 10 polls.
  Pair test: three put-backs of about 0.5 m when A stopped; final gap 0.1 m (A 392.06,1357.91, B's copy
  392.09,1357.80). ForceSetMatrix (main+0x45ce70) confirmed working from the input thread.

## Gear (2026-11-04)
- EquipmentUser slot build: main+0xc4a5f4 (eq, slot) reads the item from the save and creates it by name. Weapons go
  through main+0xaf100c (eq, slot, &name, 1 out / 2 put away, life -1, extra life -1, &attachment, 1, 0, 0, 0, -1, 0,
  -1), armor through main+0xc48ac0 (eq, slot, &name, dye -1, 0, 1). Create slots: 0 weapon, 7 bow, 8 shield,
  9 head, 10 body, 11 legs (also 3 sheath, 6 quiver, 15/16 extras, made along with them). &name: +0 char*, +8
  capacity 0x40. Evidence: Ghidra pseudocode of both, create traces. Confidence: confirmed (calls work).
- What an EquipmentUser holds: a table at +0x28, 0x18 a row; the row's first word is a reference whose +0x40 is the
  item actor. Seen rows: 0 weapon, 8 head, 9 body, 10 legs, 11 sheath, 14 quiver, 15/16 extras (row order is not the
  create slot order, so items are sorted by actor name). Removing the weapon from the menu clears row 0 and the
  sheath row. Evidence: memory scan while equipping/removing. Confidence: confirmed for weapon and armor rows; bow
  and shield rows not seen (this save has neither), found by name.
- main+0xaf100c clears the slot's handle (+0x1430 + 8 * slot) and destroys the old item first; an empty name leaves
  the slot empty. Used for "-" (nothing) on a remote Link.
- Test (run_pair, A reports Weapon_Sword_070, Weapon_Bow_001, Weapon_Shield_001): game B's copy of A wore all three
  (screenshot); B's save has none. A's menu Remove reached B as an empty weapon slot. Confidence: confirmed.
- The Decayed Master Sword cannot be equipped on this save (Great Sky Island start): the menu ignores A on it.

## Leaving and time of day (2026-11-04)
- Actor delete request: main+0x63baa8 (actor, reason). AI node OneShotGenericDeleteThisActor passes 3; returns at
  once for an actor already going away (state 7..11). Evidence: Ghidra (node vtable 0x71037ef048, its enter calls
  it). Test: B closed, A's copy of B and its gear gone two seconds later (state 7), no crash. Confidence: confirmed.
- World manager [main+0x3a83968]; +0x98 the time object: +0x28 the second of the day now (float 0..86400), +0x48 the
  last one asked for. main+0x21cd794 (world manager, s0 = second of the day) asks for a time, as AI node
  ExecuteWmSetTime does (hour * 3600 + minute * 60, then main+0xb6bff8 (time object, 1, -1)). Evidence: lifted code
  of the node, memory probe (35571 -> 37671 over the run; HUD 10:00 AM). Test: guest on 6:43 AM set to the host's
  9:55 AM in one request; HUD clocks agree. Confidence: confirmed.

## HUD control guide (2026-11-04)
- The bottom-right prompts ("Dive R", ...) live on one CtrlGuide screen, found through [main+0x3960158] by name hash
  0x5528d3d9. main+0x2192c50 (type) shows a prompt type (main+0x2147c64 on the screen), main+0x4cb540 (type) hides
  one (clears bit type of a mask at screen +0x1b8, rebuilds with main+0x2147df0); main+0x4cb3d8 hides all 42.
  Every Player's calc calls them, so a co-op Link's state reached this PC's HUD (the tester's screenshot: "Dive R" on
  the window whose own Link stood still). Evidence: profiler mode TOTK_COOP_PROF=3 saw the screen's guide stack
  main+0xd82334 entered inside the co-op Link's calc, from main+0x2147e4c; Ghidra for the callers.
- Fix: both do nothing inside a co-op Link's calc. Test: A glides (tools/coop/glide.txt), A's HUD shows "Dive R",
  B's shows none. Confidence: confirmed for this prompt; other HUD screens (big action prompts like "A Talk") not
  checked.

## Weather (2026-11-04)
- Weather manager [[main+0x395dc20]]. Weather at an actor: main+0x1c2fd1c (main+0x4f8d60 (actor)), after main+0x21cd370
  (manager) & 1 (ready), as AI query QueryWorldInfoCheckCurrentWeather. Force one: main+0x21d0798 (manager, type,
  flag): writes a request block at +0x940 (+0xd20 a buffer, index at +0x2380; type at +0xcd8), as flowchart node
  OneShotWmSetWeatherForFlowchart. Back to the forecast: main+0x21d0c18 (manager). Types seen: 0 clear, 2 rain (HUD
  raindrop), 4 snow (HUD snowflake). Evidence: lifted node code; pair test with TOTK_COOP_FAKEWEATHER. Confidence:
  confirmed.
- A guest shows the weather at the host's Link, wherever the guest is (players far apart see the host's sky).

## Enemies (2026-11-04)
- Every live actor passes the per-actor calc main+0x41bb20 each frame (x0 = actor): the live set.
- Placed enemies are created at the same spot in every game on a save (two Chuchus at 68.7/22.9, 1498, 1471, the
  same to 0.1 m in two games). No per-enemy value equal across games and different between enemies was found in
  the actor's first 0x2000 bytes (only position and matrices): the map's placement hash is elsewhere. Key used:
  name + first position (half metres). Confidence: works for placed enemies; random spawns not checked.
- Actor +0x228 is a component table: +0x2f8 life component, +0x518 world-info component (main+0x4f8d78 and
  main+0x4f8d60 return them). Life u32 at [life+0x1830] +8, max at [life+0x1838] +8 (AI query QueryActorLife; Link
  14 of 152 with 3.5 of 38 hearts; Chuchu Junior 10 of 10). Writing 0 kills: the game's own death follows and the
  enemy stays dead. The actor delete request (main+0x63baa8) on a placed enemy instead lets the map make a fresh
  one at once. Confidence: confirmed (pair test).

## Router (UPnP), 2026-11-04
- This PC's router: two devices answer the SSDP search; the one with WANIPConnection:2 is at the gateway (control
  /ctl/IPConn). AddPortMapping with a 3600 s lease works. DeletePortMapping with an empty NewRemoteHost answers 402
  Invalid Args (both service versions); with NewRemoteHost "*" it works. runtime/coop_upnp.c tries both.

## Enemy targets: not found (2026-11-04)
- A Bokoblin spawned 2 m from Link (TOTK_COOP_TEST_SPAWN) stands alert facing him. No pointer to Link or to its
  reference object in the actor's first 0x1000 bytes, in the objects those point to (0x400 each), or in its
  components (actor +0x228 table, 0x800 each) (TOTK_COOP_TRACE=target).
- Inside the enemy's per-actor calc (main+0x41bb20) no function is entered with x0 in Link or its reference
  (TOTK_COOP_PROF=4; the calc is bracketed and the profiler on): the AI's decisions run elsewhere.
- main+0x1a30670 (actor system [[main+0x395da68]], out link, &name) finds the one actor of a name in a tree at
  +0x38 (QueryTestGetPlayerActor asks it for "Player"); it is not called at all during play with the Bokoblin
  (TOTK_COOP_TRACE=player). Enemy AI finds the player another way: likely the sensor / awareness system.
- Perception (2026-11-04): an actor's perception component is [[actor+0x228] +0x378]; its data [comp+0x30], the id
  a u32 at data +0x10 (AI query QueryActorGetPerceptionID, main+0x1b0c67c). This PC's Link has id 4; a co-op Link
  has its own (0x2b, 0x29). Copying the resident Link's data flags (+0x14, +0x1c) to a co-op Link changes nothing.
- Five globals hold this Link's perception data and none a co-op Link's: [main+0x3a4e6a8]+0x98, [main+0x3a4e6e0]
  +0x370, [main+0x3a6ea80]+0x58, [main+0x3a83a30]+0x3b0, [main+0x3ad9830]+0x250 (pointer table: main+0x395dbd0 ->
  main+0x3a6ea80, main+0x395f110 -> main+0x3a4e6e0; main+0x3ad9820 is put in a registry by main+0xd65634).
- Swap test (TOTK_COOP_TEST_SLOT, the co-op Link's data put in one slot; A walks away from a Bokoblin next to B's
  Link): slots 1, 2, 4: the Bokoblin leaves B's Link and attacks A (as without a swap). Slot 3 ([main+0x3a6ea80]
  +0x58): A is not hit; B's Link loses life even 28 m from the Bokoblin (something else hurts "the player").
  Slot 5 ([main+0x3ad9830]+0x250): A is not hit, the Bokoblin stays 1-5 m from B's Link for ~30 s and B's Link
  loses 1 life. Lead: slot 5 is what enemies take as the player. Confidence: likely (one run each).
- Who reads this Link's perception data (read watch, 10 s with a Bokoblin): main+0x3020e0, 0x328d80, 0x41bb20,
  0x488258, 0x484b14, 0x4cbc28, 0x4c2bb0, 0x684e10 read both Links' data; main+0x45b930 and main+0x72b4e8 read
  only this Link's. main+0x45b930 is called from the emitter updates main+0x446a84 / main+0x49f5f8 (x19: the
  emitter component, +0x18 its actor, NPCs and insects; never a Link) and takes the player's data from the
  perception manager [main+0x3a6ea80] +0x58 (the same field as [main+0x3ad9830] +0x250: "slots" 3 and 5 are one).
- Putting the nearest co-op Link's data in that field for each enemy's call of main+0x45b930
  (TOTK_COOP_ENEMY_REDIRECT=1, about 2000 times in a run) does not change whom a Bokoblin attacks; the field itself
  is not read at all in 10 s of fighting (read watch). The choice is made from something cached elsewhere. Open.
- Static read of the readers (lifted C), none is the target choice: main+0x3020e0 / 0x328d80 publish an actor's own
  state into its perception data; main+0x4cbc28 builds the emitter's interest-type mask at data+0x1d8;
  main+0x484b14 (called only from main+0x45b930) is a yes/no check that drops a source whose perception ID is
  missing from the receiver's candidate list; main+0x488258 drops a receiver's own events; main+0x4c2bb0 is Link's
  state machine and queues its perception events in the ring at manager +0xda0 (through main+0x4cac5c).
  main+0x72b4e8 takes "the player" from the registry [main+0x3a75a40] (binary search for key 0x5d5d398d, entry
  +8 -> +0x150), a hard-wired local-player lookup; 45 places read that registry. Untested lead.
- An enemy's own perception data +0xf8/+0xfc and +0x138/+0x13c (IDs the per-actor calc resets to -1) stay -1 with
  a Bokoblin 1-4 m from both Links (TRACE=target "target slots"): not where it keeps its target.
- The registry [main+0x3a75a40] is small (27 entries); its key 0x5d5d398d entry and that object's +0x150 hold no
  pointer to either Link, its ref, or its perception component or data, even one level down (TRACE=target
  "registry"). It is a module or parameter object; main+0x72b4e8 only takes a scale factor from it. Not the player.
- Parked 2026-11-04. Resume at the consumer of the perception event ring (manager [main+0x3a6ea80] +0xda0 entries,
  +0xd98 capacity, +0xda8/+0xdac head/tail, filled by main+0x4cac5c): a grep for 0xda0/0xda8/0xdac in the lifted C
  gives dozens of hits (other objects use the same offsets), so a read watch on +0xdac during a fight is the faster way
  to find it. Untried idea: swapping the two Links' positions during an enemy's calc (risky: actor calcs run on
  several threads). Test caveat: with tools/coop/idle.txt and TOTK_COOP_TEST_SPAWN=Enemy_Bokoblin_Junior@8 the
  Bokoblin circles both Links but did not hit either in 80-150 s runs; a test needs a real fight.

## Instant tips from a co-op Link (2026-11-04)
- main+0x923d20 (game data [main+0x395dff0], 1, &key, value) sets a game-data enum (458 callers). A Player's calc
  calls it from main+0x207eba0 (key 0x57f52eb8, values from the table "AttackByWeapon", ...): this game's record of
  the player's last action. The instant tips ("To shield surf...") show from it, so a co-op Link's shield surf showed
  the tip in this game too. Found with TOTK_COOP_PROF=3 (functions in co-op Link calcs with a global's object) and
  tools/coop/surf.txt on game B: the only game-data write in the surf windows.
- Fix: coop_spawn.c skips main+0x923d20 inside a co-op Link's calc. Test: run_pair with B surfing, the tip shows in
  B's game only (screenshots, one run). Other game-data setters (bool, int) are not hooked; none showed up here.

## A co-op Link must not change this game (2026-11-04)
Each game runs the other players' Links as full Players, so their actions reached this game's shared systems: the
other player's shield surf showed the instant tip here, its fatal fall brought this game's Game Over. Swept by system
instead of by action:
- Update phases: an actor's vtable holds main+0x41bb20 (calc), main+0x245030 and main+0x194fd7c; a co-op Link's
  main+0x245030 now also counts as its calc (coop_players_phase). Found with TOTK_COOP_PROF=7 (functions entered with
  x0 = a co-op Link or one of its components outside its calc). main+0x2f9910 and main+0x194fd7c are hooked but
  never matched (x0 is not the actor there).
- Shared systems a co-op Link's update calls (TOTK_COOP_PROF=3 with an action tour, tools/coop/tour.txt; x0 named by
  the global that holds it): game data ([[main+0x395dff0]] = [main+0x3ad66b0]) is the one it writes. Its setters
  (return nothing, take a value): main+0x170210, 0x16fdcc, 0x170490, 0x282280, 0x2aabf84, 0x923d20; skipped inside
  a co-op Link's update. It wrote some every frame (from the player state code main+0x281d28 / 0x281d40 /
  0x282a0c and main+0x2b04c8 / 0x2b0568). Getters it calls run as before (list in coop_spawn.c).
- Instant tips: main+0x207ea50 (tips manager, tip id) queues a tip; skipped inside a co-op Link's update.
- Death: main+0x4400b0 (life component) applies damage each frame; for a co-op Link's component the life is
  lifted for the call and put back, so it takes no damage here and cannot die here (its own game has its life).
  Found with a write watch on its life (TOTK_COOP_TEST_WATCH5 + WATCHWHAT=cooplife, coop_watch_writes).
- Tests (run_pair, A idle, B scripted; tools/coop/tipcheck.py on every frame): before, A showed the tip (2-3 frames)
  and Game Over; after, the tour and the surf-and-fall runs show neither in A, while B keeps its own tip and Game Over.
- Tools: TOTK_COOP_PROF=6 (functions run for the first time after TOTK_COOP_PROF_AFTER s), =7 (above),
  TOTK_COOP_TRACE=gdata (game-data enum writes), TOTK_COOP_TEST_GDATA_KEEP / TEST_LIFE_KEEP (controls).
- Not covered: shared systems the tour did not reach (swimming, climbing, riding, abilities, cooking, enemies hitting
  a co-op Link); sound and effects were not looked at.
- Abilities tour (tools/coop/tour2.txt on the 100% save at In-isa Shrine: ability, wheel up/right/down/left, climbing,
  jumps, the quick item menu; A plays tools/coop/idle_3rd.txt): A's view took the red Ultrahand tint (4-5 frames;
  tipcheck.py now counts it) and its object highlight (main+0x585a30 from the model update main+0x580ff0, outside the
  co-op Link's calc). A co-op Link has no L now (coop_fill_controller): its abilities would run in this game on this
  game's objects, which are not shared. After: no tint in A (30 frames). No wheel, no ability change, no item menu
  showed in A before either. Also skipped now: game-data setters main+0x16ee70, 0x31d824, and the ability screen
  effects [main+0x3a5e878] (start main+0x2056f3c, end 0x205719c, focus 0x2057384) inside a co-op Link's update.
  Controls: TOTK_COOP_TEST_ABILITY_KEEP.
- Still not covered by a tour: swimming, riding, cooking, an enemy hitting a co-op Link, sound.
- Lookout Landing tour (the second save; tools/coop/tour3.txt, swim.txt; A plays idle_2nd.txt): the co-op Link's A
  next to Purah started Purah's talk in this game too, and this game stayed in it. A co-op Link has no A now unless ZL
  is held (coop_fill_controller; ZL + A starts a shield surf): talking, picking up, opening, checking, cooking and
  mounting all run on this game's NPCs and objects. After: no talk in A, B keeps its own. Sprinting, climbing a wall
  and the stamina wheel showed nothing in A. Control: TOTK_COOP_TEST_INTERACT_KEEP. tipcheck.py misreads night
  scenes and dialogue boxes (dark text, red); read those runs by eye.
- Still not covered: swimming (the run did not reach the moat), an enemy hitting a co-op Link, sound. Riding and
  cooking start with A, so they no longer run in other games; a riding player's Link is seen without its horse there.

## Puppet spike: a co-op Link shown without running it (2026-11-05)
- Freeze (TOTK_COOP_TEST_FREEZE=<s>): from then a co-op Link's calc (main+0x41bb20) and main+0x245030 do not run.
  It stays drawn in its last pose, the game runs on (60 s+), but ForceSetMatrix (main+0x45ce70) no longer moves it
  (the distance to its owner grows, tools/coop/freeze_walk.txt): the move is applied inside its update.
- Position copies: 13 near the actor (scan TOTK_COOP_TEST_MODELSCAN); writing all of them every tick
  (TOTK_COOP_TEST_FOLLOW) does not move the frozen model. Its draw matrix is elsewhere. Open.
- g3d: ModelObj::Initialize is main+0x1f38c8 (ModelObj +0x38 SkeletonObj; SkeletonObj::SetupBlockBuffer
  main+0x1f378c). SkeletonObj: +0 ResSkeleton, +0x18 world matrices (3x4, 0x30 each; again at +0x58), +0x20 local
  ones (0x40 each; +0x60), +0x48 u16 bone count. Link's body skeleton: actor +0x48, +0x2a8, +0x20, +0x20, +0,
  +0xf8, +0x58, +0xb0 (127 bones, per instance, root at 0,0,0: model space). Other Link skeletons have 95 bones
  (head / hair and others, paths in the scan); one 95-bone one is shared between both Links (a resource).
- Pose copy (TOTK_COOP_TEST_SKELCOPY=1 with FREEZE): this Link's 127 body bones (both world buffers and the local
  one) copied onto the frozen co-op Link every tick: it takes this Link's poses (shield up, crouch, arms, idle),
  facing its own frozen way; its head / hair (a separate skeleton, not copied) floats out of place. Verdict: a
  pose can be shown on a co-op Link whose update does not run. Not done: the other skeletons, the model's position
  and rotation while frozen, held items, the network.
- Moving the frozen Link (2026-11-05): the model object (component +0x8, +0x28) keeps its world matrix at +0x1f8
  (3x4 rows, absolute position) and scale at +0x228. main+0x24cc30 (from the update phase main+0x245030) pushes it to
  the model units only when its dirty bit (+0x240 bit 0) is set; main+0x25cf88 and main+0x269f70 read it to place
  attached items. Writing +0x1f8 alone (TOTK_COOP_TEST_FOLLOW), the actor position (MOVE=5) or the 4x4 array at
  +0x5b0 (MOVE=4) does not move it. With the calc frozen but main+0x245030 running
  (TOTK_COOP_TEST_FREEZE_PHASES=0) and MOVE=6 (owner position into +0x1f8, dirty bit set): head / hair and shield
  follow the owner; the body (the 127-bone skeleton's model) does not show at all. Open: what places the body.
- Idle body (2026-11-05): the co-op Link's update running, no input (TOTK_COOP_TEST_NOINPUT), this Link's skeleton
  copied onto it every tick (SKELCOPY): mostly its own idle pose shows, with a copied arm now and then. Its own
  animation (Havok pose to g3d, main+0x2e9c80; it runs outside the calc, also when frozen) overwrites the copy
  before the frame is drawn. Next: write the pose right after that function for the co-op Link's model.
- Pose timing (2026-11-05): main+0x2e9c80 is called per animation binding (vtable main+0x3927130); each Link has
  one, but the co-op Link's is not referenced from its actor or components (one level), so the test takes it by
  elimination. Its pose input is [binding+8] +0x38 (0x1c a bone, count at +0x30), only 27 bones: not the body.
  Copying the body skeleton right after main+0x2e9c80 does not stick either. Writers of the co-op Link's body world
  matrices while its update runs (write watch, TOTK_COOP_TEST_WATCHWHAT=coopskel, WATCHWRITES=1): main+0x2e9c80,
  main+0x283c750 (most), main+0x289620, main+0x2925e0, main+0x43e450, main+0x41bb20 (calc). Next: put the copy after
  the last of these for the co-op Link's skeleton (likely main+0x283c750 or the IK ones).
- Pose timing solved (2026-11-05): main+0x283c750 is called with x1 = a body skeleton (SkeletonObj) and is the
  writer that recomputes its world matrices after the animation step. Hooked: when x1 (or x0..x3) names the co-op
  Link's skeleton / buffers / model, the copied pose goes on right after it. Test (NOINPUT + SKELCOPY, A plays
  tools/coop/pose_walk.txt): the co-op Link, its update running and no input, holds this Link's poses (shield up,
  crouched swing, idle) at its own place; its head / hair (the separate 95-bone skeleton, not copied) sits off.
  The idle body + copied pose design works. Not done: the head skeleton, held items, sending the pose over the net
  (the body is 127 bones x 0x30 world + 0x40 local; the local matrices alone may do), finding the co-op Link's
  bindings without elimination.
- Puppet mode over the network (TOTK_COOP_PUPPET=1, 2026-11-05, in progress): main+0x283c750's x0 is the body's
  input pose: u16 bones at +0xa, [x0] 0x50 a bone: +0x00 scale, +0x10 translation, +0x20 / +0x30 the first two
  rotation rows, +0x40 other. ([SkeletonObj+0x18] is not 3x4 world matrices as first assumed: 0x60 records.)
  POSE packet (type 8, relays forward it; buffers 8 KB): u16 bones, u16 scaled count, 12 bytes a bone (smallest-
  three quaternion + translation at 1/4096 m), then u16 index + 3 floats for scales that are not 1: 1948 bytes for
  127 bones. Sent with the state packets (about 30 a second). The receiver runs main+0x283c750 for the co-op Link
  as usual, then again with the player's pose in the input (written before the first run it did not show).
  The co-op Link gets no input (no L, no A, nothing) and is teleported (ForceSetMatrix) every tick.
  State: poses go out, arrive (127 bones, 0-6 ms old) and are put on; the screenshots do not yet show the co-op Link
  where its player is (tools/coop/show.txt: B walks ahead, A does not see it there). Next: check what the per-tick
  ForceSetMatrix does to a no-input Link, then the pose on screen.

## Frame tracer (2026-11-05)
- Tool: `TOTK_COOP_TRACE_AT=8,14.6,30` (world seconds, the same clock as `@ms` script lines), `TOTK_COOP_TRACE_MS`
  (default 70). runtime/coop_trace.c records every guest call whose x0..x3 points into one of a Link's objects, with
  thread, time, caller and stack depth, and checks the actor position and rotation, model world matrix and dirty
  word, and skeleton bone records at each recorded call. Writes logs/trace_<n>_<s>s_<tid>.csv;
  `tools/coop/trace_report.py <csv>` writes .report.txt (value changes, functions run for one Link only, the display
  chain) and .timeline.txt (per thread, in order, indented by depth).
- Object map: actor (0x4000), component table [actor+0x228] and its entries, model [[[actor+0x228]+8]+0x28], the body
  skeleton path, every skeleton within 7 pointers (tagged by the actor that owns it), then 3 pointer levels of
  everything else. It doesn't walk into other actors (they get a label only). Objects main's globals hold are tagged
  "global-held"; objects both Links reach are "shared" and are left out of the diff. Each window costs about 50–110k
  records and a short hitch to build the map.
- Gaps: paths are first-found, so the two Links can label one object differently. The skeleton search is not
  deterministic (2–5 skeletons from one run to the next; head and hair are not always found). The bone translation
  values in the report read 0 (the record offsets are a guess); the changes themselves are real.
- Caution: a window stalls both games for a moment (building the map, the slow recording). In puppet mode that
  shows up as the puppet drifting away from its owner. That drift comes from the tracer: without tracing, the puppet
  stays within 0.3 m in the air (below). Don't measure positions across a trace window.
- Seen in the traces: the puppet's position is written inside its update phase main+0x245030, then main+0x257310, on
  its comp+0x18 (>+0x370>+0x150): the physics body sets the actor position each frame. ForceSetMatrix
  (main+0x45ce70) goes through [[actor+0x228]+0x50]+0x20 (the controller): it sets flags at +0x188, then on the body
  [ctrl+0x150] queues a transform request (main+0x25a450) or calls a vtable +0xc0 path.

## Puppet following its owner (2026-11-05)
- Measure: coop_players.c logs "error vs its owner at the same moment". It compares where the puppet was against
  each report's send time, so network lag counts as error.
- Where to call ForceSetMatrix (tour.txt in A, puppet mode in B): from our tick (the input thread) is best. Right
  before or right after the puppet's calc is worse: after the owner's long drop the puppet stays 29 m up.
- Prediction (on in puppet mode, TOTK_COOP_PUPPET_PREDICT=0 turns it off): the snap moves the owner's last report on
  by its age plus one tick at their measured velocity (at most 0.25 s ahead). While the owner moves the mean error
  roughly halves (for example 0.25 to 0.11 m).
- Still wrong, with prediction or without:
  - A 14 m spike for about a second when the owner lands after the long drop.
  - 0.98 m all the time while the owner climbs. The input-less puppet can't hold a climb, so it stands at the foot
    of the wall. Both need the puppet's physics off (it should follow the owner's transform, not simulate). Open.
- Placement (2026-11-05, TOTK_COOP_PUPPET_PLACE: bit 0 actor, bit 1 model, default 3, 0 off): main+0x257310
  (x0 = actor) is the step that copies the physics body's transform into the actor (+0x2b4 position, +0x2c0
  rotation; it writes only when they differ from the body's) and into the model's world matrix. Puppet mode lets it
  run, then writes the owner's predicted transform over the actor and over the model ([[[actor+0x228]+8]+0x28]
  +0x1f8, dirty bit +0x240) so the model units get it in the same phase. The body itself is still moved by snap()
  every tick, so physics can no longer put the visible puppet anywhere else.
- Checked on screen (tools/coop/show_jump.txt, B every 0.5 s): the puppet walks in, jumps clear of the deck and
  lands, with placement on (and in the control without it). Climbing and the long fall are not checked on screen:
  in tour.txt the owner is out of B's view by then. The tour runs to the end without a crash.
- With placement on, the "error vs its owner" log reads 0: it reads the actor's position, which placement writes.
  Measure physics drift with TOTK_COOP_PUPPET_PLACE=0.
- Mistake made while testing: a run whose screenshots started at 16 s (B's clock) missed the jumps, which happen at
  14–16 s there, and looked like a frozen puppet. Each game's world clock starts at its own load, so screenshot
  windows need a second or two of slack on each side.

## Armor dye (2026-11-05)
- The armor create main+0xc48ac0 / main+0xc4961c takes the dye as its 4th argument. The slot build main+0xc4a5f4
  reads it from the save (an enum mapped to 0..14, else -1 undyed). This PC's dyes are taken from its own armor
  creates (the gear hook on main+0xc4961c), sent in NetState.dye (COOP_NET_VERSION 6, 304 bytes), passed to a
  co-op Link's armor creates, and a dye change alone rebuilds that piece.
- Checked: on the 3rd save the tunic and trousers are dye 10 and go over as 10. With game A faking dye 3
  (run_pair -FakeDyeA ",,,,3,3", TOTK_COOP_FAKEDYE) game B shows A's Link in a darker tunic than its own white one.
  Not checked: dyeing at the shop during a session. That should go through the same create, but it's untested.

## Items in hand, puppet mode (2026-11-05)
- A puppet gets no input, so it never draws an item. The owner's state now carries weapon, bow and shield in hand
  (NetState.flags bits 8..10, from main+0x3e322c on their EquipmentUser). When one changes, the puppet's item is
  made again with the weapon create's "1 in hand / 2 put away" argument (main+0xaf100c).
- Checked: shield (1st save, show.txt). The log shows put away -> in hand -> put away around ZL, and game B's shot
  has the shield on the puppet's forearm. Not checked: sword and bow, because neither test save has one equipped.
  Each change rebuilds the item, which may show for a frame.
- Correction (2026-11-05, the tester's two-pad session): rebuilding the item "in hand" did not hold it. The puppet's own
  state put it straight back (main+0x3e322c read 0). What works: main+0x3e322c also reads the EquipmentUser's per-slot
  byte +0x4620 (0 weapon, 3 bow, 4 shield) bit 1 as "in hand". Puppet mode sets it while the owner holds the item
  and clears it otherwise. Checked with a faked Master Sword (run_pair -FakeGearA Weapon_Sword_070 -FakeHandA 1):
  without the flag the hilt sticks up over the shoulder, with it the sword is in the right hand. The other route,
  [[item+0x228]+0x208] +0x3d0 bit 0 / +0x3d4 == 1, is the item's own hold state and is left alone.
- Not checked: the owner's arm and lean while holding a real sword. the tester saw the puppet leaning back with its arm
  out, which may have been the puppet's own state (not holding) fighting the owner's holding pose. Bow and shield
  use the same flag.

## Puppet shake (2026-11-05)
- the tester's capture: a puppet standing on stairs dips about 7 cm on irregular frames. Its placed position doesn't
  move, and the poses sent and applied are smooth (TOTK_COOP_POSE_LOG), so the dip comes after both.
- Measured with TOTK_COOP_TOP_LOG (the highest bone in skeleton +0x20, every frame): standing still, the owner moves
  0.01 cm per frame and its puppet 0.1-0.55 cm (runs vary), up to about 2 cm.
- Likely cause: main+0x257310 writes the physics body's transform into the model before placement overwrites it.
  Anything reading it in between sees the body, and when the body was teleported every tick it was falling a few cm
  after each teleport.
- Change: the body is teleported only once it is 10 cm from the owner (TOTK_COOP_PUPPET_SNAP_M, 0 = every tick),
  measured on the body's own mLastTransform (body +0x98 translation; body = [[[actor+0x228]+0x50]+0x20]+0x150).
  Not proven against the 7 cm case: my pair runs don't reproduce it at that size.
- Skipping the step for a puppet (TOTK_COOP_PUPPET_SKIPPHYS=1) is worse: frame-to-frame jumps of up to 2.8 m.
- A jitter watch logs every frame where a puppet's highest bone moves more than 3 cm, plus a count every 1800
  frames. It also counts real animation (sword swings).
- Follow-up from a play session: game 2's puppet body stayed turned about 12 degrees from its owner (rotation off
  0.211), because the 10 cm rule never fired for a body that had only turned. That game logged 130 shake frames
  per 3600; game 1, whose body matched, logged 4. The body is now also teleported once any rotation element differs
  by more than 0.02. Checked with pose_walk.txt (the owner turns on the spot): rotation off 0.000 afterwards.

## Gear after loading another save (2026-11-05)
- a play session: after both games loaded another save, game 1 kept reporting the first save's default armor.
  Causes: (1) the new Link's gear is built while coop_player_actor() is still 0, so the gear hook dropped those
  creates; (2) an eq_owner entry could outlive its EquipmentUser.
- Now unmatched creates are kept along with their armor dyes. match_my_eq pairs them with this PC's Link on the next
  tick, and eq_of_actor drops an entry whose EquipmentUser no longer points at its actor.
- Not tested: loading another save in a pair run (no script does it yet).

## Proxy mode, stage 1 (2026-11-05)
`TOTK_COOP_PROXY=1` (docs/coop/proxy-plan.md stage 1; separate from `TOTK_COOP_PUPPET`, which it implies for the pose
packets and no input). Code: coop_players.c proxy_drive, coop_world.c coop_proxy_pack_mine / coop_proxy_put_records.
- Freeze: after 180 ticks LIVE (and once the input pose object of its body skeleton was seen) a remote Link's calc
  and update phases are skipped. In place of main+0x245030 the driver writes the owner's transform (60 ms behind,
  blended) into the actor (+0x2b4/+0x2c0) and the model (+0x1f8, dirty bit), writes the body pose, then calls
  main+0x24cc30 (model). That push sets each model unit's world matrix: units with no parent bone from +0x1f8, units
  hung on a bone (head, hair) from that bone's +0x20 matrix in the parent unit. Something outside the frozen update
  still turns the skeleton's +0x18 records into its +0x20 matrices every frame (body top changes in ~598 of 600
  frames), so writing the records is enough; main+0x283c750 need not run.
- Skeleton buffers, corrected: +0x18 local records 0x60 a bone (scale +0x10, the rotation's columns at +0x20 / +0x30
  / +0x40, translation +0x50; the rotation is stored transposed against the input pose's rows). +0x20 final matrices
  0x40 a bone: rows 0..2 rotation of a row-vector matrix, row 3 translation, in world space from an origin of the
  game's own: the numbers are metres off the actor position, by different amounts in two games, so only bone-to-bone
  differences compare across games. body_top now reads the highest bone above bone 0.
- The pose has to come from the owner's final matrices. The writer's input (what puppet mode sends) misses later
  steps: 30 bones differ between input and records even standing (TOTK_COOP_TEST_IKDIFF), and the final matrices
  carry more (feet on stairs: owner body top 1.55 m, proxy 1.60 m; a hand on the shrine wall: 1.90 against 1.60).
  The owner now sends, at its main+0x257310, locals made from its final matrices (each bone relative to its parent;
  the parents are found once by matching parent matrix x record against the bone's matrix: 126 of 126 within 2 mm).
  The bone count's top bit marks such a packet. TOTK_COOP_PROXY_POSE=input and TOTK_COOP_PROXY_FINAL=0 are controls.
- Pose timing: poses are kept (8 per player) and shown from the same delayed moment as the position, blended. With
  the newest pose put on as it came, it ran ~40 ms ahead of the position.
- Measuring: TOTK_COOP_TOP_LOG=<s>,<length> on both games, then tools/coop/top_compare.py (A log, B log). It lines the
  two clocks up by the position, then reports position error, body-top difference, and per frame how far the
  proxy's step differs from its owner's ("standing": the owner's position, body top and root still). Screenshot runs
  stall game B's clock, so take numbers from runs without screenshots.
- Results (final build, b0a3969):
  - In-isa stairs (A walk_up_3rd.txt, B idle_3rd.txt): standing 0 of 1569 frames over 1 cm (largest 0.53 cm);
    position error mean 0.5 cm; body top mean 0.7 cm (15.3 cm sending the records, 2.2 cm sending the writer's
    input). Max 30 cm in single frames where the owner stops running.
  - tour.txt: position mean 0.9 cm (max 39 cm at the fall and respawn), body top mean 1.1 cm; 4 "standing" frames
    over 1 cm, largest 2.7 cm, all at sword / shield moves where the owner's own step is momentarily zero.
  - Sword (show.txt, -FakeGearA Weapon_Sword_070 -FakeHandA 1): in the right hand in all 6 screenshots, swings too.
  - Far: drawn at 24 m (A at the top of the stairs, B walked down, walk_away_3rd.txt). A glide away from the Rito
    tower (glide_far.txt) was not seen from B in proxy mode nor in the default mode (control): that camera cannot
    frame it, so longer distances are untested.
  - The in-game jump watch ("proxy: ... standing jump") counts owner actions too (it knows no animation); use the
    comparer for shake.

## Proxy mode, gear (stage 2, 2026-11-05)
- Held items, test: A show.txt (shield up with ZL 18.5-20.5 s, jump 22.5 s), B idle.txt, screenshots w15-25:2.
  - With the in-hand flag alone (as in b0a3969): the shield went to the hand when raised, stayed there after the
    owner lowered it (still in the jump), then was gone from hand and back; the hold query main+0x3e322c read 0x4
    while the owner reported 0.
  - Failed 1: make the item again with the weapon create's 1 in hand / 2 put away on each change. It left the hand
    on time but never came back onto the back. Failed 2: empty the slot first, then make it. Same as no fix.
  - Works: a thaw. On each change of the owner's in-hand bits the proxy's own calc and update phases run for 30 ticks
    (PROXY_THAW_TICKS, about 0.5 s), the driver still placing and posing it after each phase. Screenshots: shield on
    the back, in hand while raised, on the back again for the jump and after; the hold query settles to 0 with the
    owner's 0. Cost on tour.txt: about 6 frames per change where the proxy steps 1-3 cm off its owner (largest
    4.3 cm), against 4 frames over 1 cm in the whole tour without thaws.
- Outfit and dye while frozen, test: A idle.txt with -FakeGearA ,,,Armor_001_Head,Armor_001_Upper,Armor_001_Lower
  -FakeDyeA ,,,,3,3 -FakeAtA 24 (the fakes apply from 24 s; TOTK_COOP_FAKE_AT), B idle.txt.
  - Frozen proxy: the gear creates run (logged) but the hood and tunic do not show. Control (default mode): they do.
  - Fixed with the same thaw on each gear change: before the switch the default outfit, after it the hood and the
    dye-3 tunic, as in the control; 0 standing frames over 1 cm in the 1200 frames around it (largest 0.21 cm).
- Loading a second save (tools/coop/load_3rd.txt in both games: Rito save, then System > Load > In-isa Shrine):
  reproduces the tester's bug. After the load neither game logs its own gear again, and each gives the other's new
  co-op Link the first save's armor ("gets Armor_Default_Upper ... this save has Armor_1043_Upper"). Not fixed.
  - Found: this game's Link keeps the same actor and the same EquipmentUser across the load; that EquipmentUser's
    gear table still names the first save's items (alive, state 4) while the Link visibly wears the new save's
    armor (screenshot). No armor create (main+0xc4961c / 0xaf100c) runs for this Link during the load (TOTK_COOP_
    TEST_EQLOG-style logging of every create: only the co-op Link's EquipmentUser gets Armor_1043). So the new
    armor is built by another path, and the table read here is not where the worn armor is listed after a load.
  - Failed 1: adopt an unplaced EquipmentUser that points at this Link even when one is known (none appears).
    Failed 2: re-read the EquipmentUser pointer from the Player's components each tick (it is in none of their first
    0x2000 bytes). Both removed.
  - Next: write-watch the worn upper armor's model or find what builds armor at a load (the frame tracer on the
    loading Link), or read the equipped items from the save's game data instead of the EquipmentUser table.

## Second-save gear, traced (2026-11-05, later)
- Control first: one game alone (no relay) with load_3rd.txt re-dresses this Link after the load: the creates run
  on the same EquipmentUser as before (it is not replaced at a load), Armor_999_Head, Armor_1043_Upper / Lower with
  dye 10, frame chain main+0xc4aba4 <- 0xd97e08 / 0xd97dfc <- 0xd97d90 <- 0x1fe4ed4 <- ... 0x244d48 (a Player's
  update). TOTK_COOP_TEST_EQLOG=1 logs every gear create with that chain; TOTK_COOP_TEST_ARMORPROBE=1 lists the armor
  actors on this Link and what points at them 20 s and 40 s after a load.
- In co-op the same three creates land on the new co-op Link's EquipmentUser (the co-op Link is made again after
  the load) and none on this Link's: this Link keeps the first save's armor actors (probe: Armor_999_Head,
  Armor_Default_Upper / Lower 40 s after the load, no Armor_1043 anywhere) and screenshots show it bare-chested.
  The earlier "new outfit" screenshot was the proxy standing on the same spot.
- Why: main+0xd97d70 rebuilds the gear of the player's EquipmentUser held at [[[main+0x39601e8]] +0x480]
  (main+0xd97dac: slots 0, 8, 7, 9, 10, 11 through main+0xc4a5f4), not the calling Link's. A newly made Player
  takes that field over (it went 0, then the new co-op Link's), so the load's rebuild went to the co-op Link.
- Fix 1 (coop_players.c keep_player_eq, every tick): when the field holds a co-op Link's EquipmentUser, write this
  Link's back. Game A: put back once, then "my gear slot 10 (Armor_1043_Upper) has dye 10", reported to B as
  Armor_1043_Upper / Lower dye 10; B's screenshots show A's proxy in the new save's tunic and trousers, dye 10
  (pink). Game B in the same run: put back too, but its own Link kept the old armor: there the field changed before
  the new co-op Link was claimed (logged as "another", not "a co-op Link's"), so the check let it pass, and the
  rebuild ran first.
- Fix 2: the same check also right before each co-op Link's calc and update phase. Same result in game B (the
  rebuild happens before the claim, when no slot names that actor yet). Stopped after two fixes.
- Fix 3 (from that trace): put back whenever the field holds an EquipmentUser that does not point at this Link (no
  co-op-Link test), also from the create hook (coop_players_gear), as the new Player's gear is built. Test:
  tools/coop/load_3rd_up1.txt in game A, load_3rd_up2.txt in game B (both load In-isa, then walk up the stairs so
  each is in the other's view). Proxy mode and default mode both: in each game "put back (1)", "my gear slot 10 is
  now Armor_1043_Upper" (dye 10) and from the other "owner's gear slot 10 is Armor_1043_Upper (dye 10)"; on both
  screens both Links wear the In-isa tunic and trousers in dye 10 (pink). Passed.

## Proxy mode in a play session (2026-11-05)
- Report: weapons showed on the other screen only once that game had equipped something in the same slot too; the
  proxy held its shield with the bow; fused weapons showed without their material.
- From the session's logs: every gear change arrived and was made on the proxy at once ("owner's gear slot 0 is
  Weapon_Sword_070", a thaw, "holds 0x1, owner 0x1"). Reproduced none of the first issue in four pair runs: a faked
  sword at 25 s, faked sword and bow after walking, and real menu equips (tools/coop/equip_3rd.txt,
  equip_bow_3rd.txt) all show on the proxy with the other game's own slots empty. Open: what differs in play.
- Shield with the bow: the session's log has "holds 0x4, owner 0" after sword and shield were put away together, and
  it stayed. Now a thaw is repeated (up to 4 times) while the proxy holds other items than its owner once a thaw is
  over. Test: with the put-away thaw skipped (TOTK_COOP_TEST_NOTHAW_PUTAWAY=1, TOTK_COOP_TEST_NORETRY=1) the shield
  stays out of place to the end; with the retry "holds 0, owner 0" after one re-run, shield on the back.
- Fused weapons: the weapon create's 7th argument (main+0xaf100c, a string object) is the material's actor name
  (AsbObj_RockParts_C_S_01 for the save's rock-fused stick; TOTK_COOP_TEST_EQLOG now logs it). This Link's are read
  from its own weapon creates and sent in NetState.fuse (protocol 7, 400 bytes); a proxy's weapon is made with it, and
  a change of material alone rebuilds it. Test: tools/coop/equip_fused_3rd.txt; B's view shows A's proxy with the
  rock-fused stick, as A's own Link.

## Proxy mode, gear while a game is in a menu (2026-11-05)
- the tester: both players in the equipment menu, one changes armor, both leave: the other screen keeps the old armor.
  Test: tools/coop/menu_3rd.txt in game B (inventory open 22-32 s), tools/coop/step_up_3rd.txt in game A with
  -FakeGearA ,,,Armor_001_Head,Armor_001_Upper,Armor_001_Lower -FakeAtA 26. Before: B's view after the menu shows
  A's proxy in the old outfit, although the changes arrived and were made during the menu.
- Three changes, all kept: the thaw counts frames in which the proxy's update ran, not ticks; every 0.5 s the proxy's
  EquipmentUser is compared with its owner's report, and a slot that differs is made again with a thaw (at most 4
  in a row; "gear slot 10 holds Armor_1043_Upper, owner Armor_001_Upper: made again"); no gear is made while this
  game seems to be in a menu (the clock and this Link both still), it waits. The first alone failed, the first two
  failed twice, all three passed twice (hood and new tunic on the proxy after the menu), plus the regression runs
  (equip_bow_3rd.txt: holds follow the owner; TOTK_COOP_PROXY=0: gear sync as before).

## Proxy mode, head and hair (stage 3, 2026-11-05)
- There is no separate head or hair skeleton on these Links: the model ([[actor+0x228]+8]+0x28) has one unit, the
  127-bone body (its +0x110 is the SkeletonObj itself, not +0x38 of it as coop_link_skeletons assumes, so that
  function finds nothing); the three armor actors in the EquipmentUser table hold no SkeletonObj within 6 pointer
  steps; the 95- and 113-bone skeletons the model scan finds hang off actor +0x140 / +0x178 (other actors, one shared).
- So head and hair are body bones (the highest bone is 83 or 84 in both games) and the proxy already gets them from
  the owner's final matrices (stage 1). In every proxy screenshot the head and hair sit in place and move with the
  body. Nothing else to do for stage 3.

## Animation by name (stage 4 research, 2026-11-05)
- TOTK_COOP_PROF=8 with TOTK_COOP_TRACE=calc (coop_spawn.c coop_fn_args): every function entered inside this PC's
  Link's calc with a name-like string in x1..x3, by function. Two layers show up:
  - Animation clips (Nml_Move_Run_St_F_L020, Sword_Guard_Wait, Equip_Shield_Off_Natural, Face_Default...):
    main+0x5baeb4, main+0x5bad18, main+0xe24484, main+0x2bb7460 / 0x2bb7570 (string compares inside).
  - AS (animation sequence) names (Wait, Move, Jump, Crouch, LockOnWait, WeaponEquipOn, SideStep, SurfingOn...):
    main+0x4dcc1c, main+0x4dd2a4, main+0x4e1bdc, main+0xac87e4, main+0xac718c / 0xac7184.
- main+0x4dcc1c (x0 the Player's AS manager, x1 the AS name as a string object, x2 a request: +0x28 slot, +0x2c an
  int it may overwrite from manager +0x148, +0x32 a flag byte, +0x34 blend time, negative = the manager's default
  +0x11c) is the request to play an AS in a slot: it calls main+0x4dd2a4 (manager, name, slot), then, unless the
  slot's entry (manager +0x20, 0x48 a slot; its [+0] +200 bit 0) blocks it, main+0x4e1bdc on that slot.
- TOTK_COOP_PROF=9 logs each call with the world time ("coop: as <s> <name> slot <n>"). On tour.txt it lines up with
  every scripted action, to the frame: Move at 14.0 s (stick), WeaponEquipNG* at 16.0 / 17.2 / 21.0 (Y with no
  weapon), LockOnWait + WeaponEquipOn at 18.5 (ZL), WeaponEquipOff* at 20.5 (ZL let go), Jump 24.5 and Land 24.9
  (X), Crouch / CrouchMove 25.5-26.8 (stick click), HorseCall 28.0 (whistle), LockOnWait 32.0, SideStep 33.0. Slots:
  0 the body, 1 the upper body, 2 and 3 weapon layers, 4 the face. All on one manager (19b496680 that run).
- Replay test (TOTK_COOP_TEST_ASPLAY=<name>,<slot>@<s>, coop_world.c as_play_test, with PROF=9 and TRACE=calc): this
  Link's AS manager is at [[[actor+0x228]+0x8]+0x378]+0x18; the co-op Link's at the same path. A copied request and
  name object with "HorseCall" in slot 1, sent to the co-op Link's manager (default mode, its own update running):
  main+0x4dcc1c returns 1 every time, once or every tick for 1.5 s (90 calls), but no screenshot (6 at 0.5 s) shows
  the whistle pose. Failed twice; stopped. Not known yet: whether the request is wrong (the name object, the
  request's other fields), or the Player's own state machine replaces it in the same frame.
- So a player's animation could go over the network as a few (slot, name) events a second instead of 127 bones 30
  times a second. Not tried: calling main+0x4dcc1c on a proxy's AS manager (where it is kept, a copied request),
  and whether a frozen proxy's AS then advances (its animation binding main+0x2e9c80 runs while frozen; the writer
  main+0x283c750 that turns it into a pose does not).

## Enemy targets, heap scan (stage 5, 2026-11-05)
- Tools (coop_world.c): TOTK_COOP_TEST_REFSCAN=<s>,<s>... logs every 8-byte word in guest memory 0x100000000-
  0x240000000 equal to this Link's actor or its reference object (0.5-0.7 s a scan; 314-316 words on the Rito save).
  TOTK_COOP_TEST_TARGETSWAP=<s0>,<s1> takes the words holding this Link's reference at s1 but not at s0, and from
  s1 writes the first co-op Link's reference over them whenever they hold this Link's (TOTK_COOP_TEST_TARGETSWAP_OFF=1:
  watch only); every 0.5 s it logs the nearest enemy's distance to both Links and this Link's life.
- Rito tower, Bokoblin spawned 2 m away at 20 s (TOTK_COOP_TEST_SPAWN=Enemy_Bokoblin_Junior@20): one new word, at
  1bce51cc8 in two runs (the same address, so a shared slot, not the enemy's own memory). Overwritten (2 writes
  needed): the enemy's distances to this Link were the same to 0.1 m as in the watch-only control (3.2, 1.0, 3.0,
  4.3, 3.3 m), and in both runs it left the live set at 32 s (it fell off the platform or was removed). No effect.
- In-isa stairs, the same spawn, B's Link 8 m up the stairs: three new words (11fffdbc0, 1a0123710, ...), which flip
  between this Link's reference and other values on their own. This Link took no damage (life 14 throughout), the
  Bokoblin wandered 12-16 m off and was gone by 39 s. No fight, so no test of the swap.
- Stopped after these two attempts. As the 2026-11-04 notes found, a spawned Bokoblin does not make a real fight in
  these places. Next: a save next to a placed camp (a fight that lowers this Link's life), then the same heap diff
  between "noticed" and "attacking", and the swap against a watch-only control.

## Arrows (protocol 8, 2026-11-05)
- A bow's arrow is an actor (NormalArrow; other types have "Arrow" in the name too). The first one is made at the
  bow on the ZR press, 1.1-1.4 m from Link; it flies on release (21 m in 0.12 s) and sticks; the next one is put
  on the bow 46 ticks after the release (TOTK_COOP_TEST_ARROWLOG, bow_shoot_3rd.txt).
- Each game also makes two arrows 0.45 m from every Link at load (the quiver, by the look). Taken by distance
  alone they were sent as shots; the 3 s window after ZR keeps them out. The EquipmentUser in-hand bit for the bow
  (+0x4620 + 3, bit 1) is no use here: on this Link it is set for a moment when the bow comes out, not while aiming.
- A fused arrow's material is an actor of its own: holding UP while drawing and taking Bomb Flower made a
  BombFruit 1.0 m from the arrow, which moved with it and was gone when the arrow hit (it blew up). It is sent as
  one more object; the receiver shows it on the arrow (bow_fuse_3rd.txt, screenshots on B at 23-25 s). The blast
  itself is not shown on the other screen.
- A copy is placed from the co-op tick, not from one of its own updates: the game stopped calling a frozen arrow's
  calc after two frames (the BombFruit's went on), and the Player's update phase main+0x245030 never runs for
  either (TOTK_COOP_TEST_COPYLOG).

## World objects (protocol 9, 2026-11-05)
- Only actors that run their per-actor calc (main+0x41bb20) are in the live list (coop_world.c seen[]): about 70 in
  the In-isa save, none of them the placed rocks, planks or plants around the Link. An object at rest runs no calc.
- Every actor's reference object sits in one pool, 0x60 bytes apart: +0x40 the actor, +0x48 its id, which is its
  entry number (the Player's reference 0x60 * 0x143 bytes from the start). About 800 actors in the In-isa save,
  the resting AsbObj_ rocks, trees, shrine parts and a sword on the ground among them (TOTK_COOP_TEST_REFPOOL;
  coop_world.c coop_actors_all). A game looks a moved object's key up there; it shows a copy only when none has it.
- Props test (TOTK_COOP_TEST_PROPS, both games idle on one spot): A lifts a resting AsbObj_RockParts_C_S_01 1.5 m,
  lets go, it rolls 10 m down; B finds its own in the pool and is within 0.1-0.2 m of A's while A sends. After A
  stops, B's slid on 1 m by its own physics with a 2 s hold; now held 5 s. With both Links on one spot neither
  claimed the rock until "nearer" allowed 1 m of slack.
- A dropped weapon is the equipped one's own actor (Weapon_Sword_070 kept its address from the equip on).
- Helpers that move with the Link were taken as moved objects: EventCamera, CameraXLinkControl.
- TOTK_COOP_TEST_PROPS=1 (the game that joined as player 1 makes a BombFruit 1 m above its Link at 24 s): "I made
  BombFruit" there, one copy in the other game, no copy churn, and an equipped weapon is not sent (equip_3rd.txt).
  (The moved-object half: see "Props test" above.)

## World objects in packets of their own (protocol 10, 2026-11-05)
- Objects left the 30-a-second state (24 of them made it 1840 bytes, over one internet packet) for OBJS packets
  (kind 9, both relays forward them): an object goes out when it moved 1 cm or turned, again once a second at
  rest, and once with an empty name when gone; up to 100 entries a tick, 20 a packet; a receiver forgets one not
  heard of for 2.5 s. Up to 256 a player. The state is 400 bytes again.
- Pair test with bow_fuse_3rd.txt and TOTK_COOP_TEST_PROPS: B has copies of all four arrows and the fused
  BombFruit, places arrow 1 through its flight, finds and moves the lifted rock, and shows a copy of an object it
  does not have.
- Moved objects are now sent while within 20 m once they moved, at rest too: a game without the object showed a
  copy that went away 1 s after it came to rest (the test BombFruit, made again and again). And they are taken by
  the game they move in, not the nearer Link: with B's Link nearer, A's lifted rock was sent by neither (B's was
  at rest there). Two games sending one object: the lower player id wins. Props test, both idle on one spot: B's
  rock ends 0.15 m from A's and stays; nothing else sent.

## Riding a co-op player's object (research, 2026-11-05)
- Zonai device actors: SpObj_LiftGeneratorWing_A_01 (glider), SpObj_WindGenerator_A_01 (fan), SpObj_Rocket_A_01,
  SpObj_Cart_A_01, SpObj_FastWheel_A_01, SpObj_BalloonEnvelope_A_0x, SpObj_FlameThrower_A_01, SpObj_SlipBoard_A_01
  (romfs Pack/Actor; 68 SpObj_ in all). A glider made by requestCreateActor makes several actors in one tick: the
  "a load makes many at once" guard for made objects is now 20, not 6.
- Object names reach 29+ characters (SpObj_LiftGeneratorWing_A_01): CoopObj.name is 48 now (protocol 11), 16
  objects a packet.
- What Link stands on: with the Link standing on a glider (0.45 m above its origin), no word in its first 0x4000
  bytes or 0x1000 into each block they point to holds the glider's actor or reference (TOTK_COOP_TEST_STAND; one
  run found it in a list at [Link+0x3b8]+0xd68 that turned out to be a list of actors that never changed). It is
  likely kept by the physics (a body, not the actor): not found.
- Carrying by the object's move each tick (ride_carry, TOTK_COOP_RIDE=1; the copy's body put with
  COOP_FORCE_MATRIX too): with the glider moved 1 m/s and up 0.3 m/s in A, B's Link on B's copy was carried but
  sank into it (0.4 m in 2 s) and fell behind (to 1 m in 2 s), and fell off; holding the boarding height took only
  now and then. Off by default. Next: how the game carries a Link on its own moving platforms, or a speed for the
  copy's body.
- Found (Ghidra, from COOP_FORCE_MATRIX main+0x45ce70): an actor's physics body is [[[actor+0x228] +0x50] +0x20]
  +0x150; main+0x25a450 (body, float m[12]) sets its transform, main+0x567750 (body, float v[3]) its linear
  velocity (checked as "next_linear_velocity", clamped to a max speed), main+0x25a1f0 (body, float w[3]) its angular
  velocity ("next_angular_velocity"). Each queues into a request block (+0x44 linear, +0x50 angular, +0x08 matrix;
  flags +0xd4: 0x80, 0x100, 0x40).
- A copy moved by velocity holds a Link standing on it (teleported each tick it sank through). The game does not
  carry the Link along a moving copy by itself (TOTK_COOP_TEST_NOCARRY): ride_carry moves it by the object's move.
- The players tick ran twice at one world time; the second ride_carry, reading the Link where it was before the
  game applied the first forced place, put it back: it fell behind at half the object's speed. Once a frame now.
- The rider's place in the other games comes from its offset in the object's frame (NetState ride_owner, ride_id,
  ride_off; protocol 12): at 5 m/s, A shows B on the glider within 0.08 m of where B's own game has it.
- Objects are sent within 20 m of any Link: a glider flown 20 m from its maker went away under the rider.
- Ride test at 5 m/s: B rode 50 m in 10 s, 0.1-0.2 m from its boarding place along the way (drifting 0.7 m across),
  and came down with the glider when A let it fall. Riding is on by default (TOTK_COOP_RIDE=0 off).

## Zonai devices: what turns one on (research, 2026-11-05)
- A device made by requestCreateActor can turn itself on at once: a SpObj_Rocket_A_01 made 1.6 m from the Link
  flew off 27 m in 3 s (its parameter ThrustIsChangeStateWhenContact, game__component__ZonauGearControlParam:
  contact turns it on); a SpObj_WindGenerator_A_01 (fan) made on the stairs moved 30 m in 6 s. Scripted sword
  hits (zonai_hit_3rd.txt, TOTK_COOP_TEST_ZONAI=<name>) never reached a device at rest: on the In-isa stairs it
  rolls away first.
- In the actor's first 0x3000 bytes only +0x404 / +0x418 (0 -> 1 or 2) changed between before and after; they
  changed when the rocket landed at the stairs' foot, so they are contact state, not "on".
- Ghidra: the device logic is in a component, game::component::ZonauGearControl (its parameter class
  ZonauGearControlParam). AI nodes with IsChangeState / IsOn / IsWorking (main+0x230b928 binds them) are a generic
  switch (WaitOn / WaitOff, SwitchOn), not the device. The state word was not found.
- Logger for a played run: TOTK_COOP_TEST_ZONAILOG=1 logs, for SpObj_ devices within 15 m, every word of the actor
  (0x3000 bytes) and of its first 128 components (0x400 bytes each) that goes between small values (0..3), with the
  device's place and speed ("coop: zonai: ..."). Next: a run where a device is turned on, left to stop, and again.
- Played dump test (TOTK_COOP_TEST_ZONAIDUMP, a fan hit on, off, on): three words follow the state:
  [comp11]+0x348 bit 0x2000 (on; 0x4000 when only the on bool is set), [comp28]+0x398 10 / 0 (energy drawn from the
  Link's battery: the battery is the Link's, not the device's), [comp107]+0x398 0x40 / 0x80 (written every frame by
  main+0x2f8b70 from the on bool). Writing any of them by hand turns nothing on.
- The on bool: component 0's +0x10 is a table of named bools (hash table +0x1c8 / +0x1d4 entries of {u32 name hash,
  u32 index}, bools +0x1a0, +0x198 of them, change counts +0x1f0 / +0x1e8); the device's on bool has hash 0xbcd1015f
  (coop_players.c dev_on_flag). Its setter main+0x9fb82c (handle x0 with [[[x0+8]+8]+0x200] the table and +0xc4 the
  index, w1 the value) writes it on every hit, called from the device's own behaviour graph (main+0x1066b20 runs the
  nodes; chain main+0x9fa920 <- 0x9f91d8 <- 0x1066bb8, TOTK_COOP_TEST_FLAGCALLER). Setting the bool (with its change
  count) on a fan from a capsule set 0xa000 but drew no energy and did not blow.
- A scripted sword hit turns a fan on (TOTK_COOP_TEST_FANSET=9, fan_hits_3rd.txt: the Link faces a fan 1 m away on
  the flat ground at 30.3 1514.5 1510.7; the hit at 34 s, after the first Y draws the sword). The hit reaches the fan
  from the Link's update: main+0x1b90cb4 (the attack contact, param_4 +0x18 the target) builds the hit and hands it
  to main+0x1b91844 (TOTK_COOP_TEST_FANHIT lists the functions entered with the fan at the hit). Next: hand a copy
  such a hit when the owner's device turns on (needs the contact and hit records decoded).
- The hit's mark on the device: its damage component ([actor+0x228]+0x2f8) gets a record at +0x1a0..+0x2ff the frame
  the sword lands (+0x1a4 = 4, +0x1c0 the attacker's reference (this Link's), +0x1c8 its id, +0x1d0 attack flags,
  +0x1d8 type, +0x1dc power, +0x1fc the hit place, then its direction; list links at +0x260..+0x270); it is taken the
  next frame (+0x1a4 back to 0). Written into a second fan (only the 65 words the hit changed, its own addresses moved
  over), it is taken the same way but does not turn the fan on; whole, it overwrote the fan's own links. So the hit
  also goes elsewhere (likely the attack object those links point to). A NormalArrow given a speed toward a fan moved
  0.5 m and stopped (an arrow flies only when a bow shoots it).
- The proxy's sword works (the tester's pick): when a player presses attack (Y), its proxy in the other games runs its
  own update for 40 frames with Y let through (the rest of its input stays off), so its sword swings there and the
  game's own hit lands on what it reaches. A Zonai device's copy now runs its own update (still placed at its owner's
  place every tick). Pair test (TOTK_COOP_TEST_FANSET=9 in A, fan_hits_3rd.txt): A's hit turns A's fan on (0xa000,
  energy 10) and B's proxy's swing turns B's copy on the same way 1.2 s later in B's clock. The copy draws on B's
  Link's battery (the game's rule), and the proxy's swing hits what else is there too. TOTK_COOP_PROXY_SWING=0 off.
- the tester's Ultrahand crash (both games): A crashed in a pool's free list (main+0x19164e0, read of -1, from the actor
  job, main+0x915a90's object +0xcf40) while holding its fan with Ultrahand; B's log stops just after it made a copy
  of "Parasail" (A's paraglider, sent as a made object). The paraglider is no longer sent. A's crash is not
  understood: scripted L / A (fan_ultra_3rd.txt) did not grab the fan, so it was not reproduced.
- The Ultrahand crash, likely cause: main+0x916860 (a Player's Ultrahand part, x0) draws the Ultrahand beam into one
  global pool of segments ([main+0x395f6f8]: +0x1f0 count, +0x1f4 capacity, +0x1f8 used, +0x200 free list), first
  giving every used segment back; the crash is its push (main+0x19164e0) reading a free-list link of -1. With a
  co-op Link there are two Players; both running it at once on two worker threads breaks the list. It is hooked
  now and run by one thread at a time (coop_spawn.c beam_hook, logs "drawn by two threads at once" when that
  happens). Not yet confirmed in play.
- the tester's autobuild test: (1) the autobuild preview's devices (real actors drawn as a hologram) were sent as moved
  objects and copied solid; (2) built devices are tinted green, copies by name are not; (3) the steering stick
  (SpObj_ControlStick) was never sent: "Control" was in the helper filter, now not for SpObj_; (5) a Link on a copy
  jiggled: the rider's forced height is not taken (the Link's own physics keeps its up and down), so holding it at
  its boarding height only jolted it now and then (a 0.25 m bob in the ride test); that is gone, and a placed object
  another player moves is now driven by speed (teleported only when 0.5 m off) instead of teleported each tick.
  Ride test at 5 m/s after: the rider stays 0.30 m above the glider within 0.01 m for 50 m. Open: (2), and a
  Recall-moved tree another game keeps merged (no actor of its own there) shows twice.
- (1), the preview: the game labels each preview device with the string "ImaginaryAutoBuildedActor" at actor +0x18
  and tests it by name (main+0x9e6af0 and main+0x1b37958 compare it; the AI query
  QuerySpecialPowerIsImaginaryAutoBuildedActor exists for it). coop_players.c is_imaginary skips such actors, so a
  preview is no longer sent; the built devices still are. Checked by build only, not yet in play.
- Correction: the label stays on a built device (a play session: the fans and stick of a build carried it from
  the moment they appeared, and none was sent). A memory diff over a build found what confirming does to every
  device at once: actor +0x440 gains bit 0x10, +0x5d0 (NaN during the preview) gets a position, +0x454 goes 0 to 1.
  coop_players.c is_preview uses the first two; built devices are sent again (CoopObj.flags says autobuilt).
- (2), the green tint, not solved. Tried on the copies and checked on screen: the label and +0x668 flag (no tint);
  then also component 80's +0xb4 bit 0x80000 and +0x78 = 1.0 (set on every original and no copy in a dump of four
  devices against their copies) with main+0x9d48dc run on it (no tint; that function marks model materials whose
  flags have bit 3, and the devices' one material has 0x80043 on originals and copies alike). Left to try: the
  originals are made with an autobuild creation record (the create argument's +0x78, whose type main+0x6898dc
  checks before setting the label; actor +0x3e8 holds main+0x3a4e008 on originals only). Making the copy through
  that same path, or finding what that record changes in the model at load, is the next step: hook main+0x6898dc
  to log the record during a build.

## Autobuild copies (one unit: done; green: open; 2026-11-08)
the tester: an autobuilt contraption with three fans turns all its fans on at one hit in the builder's game, one fan per
hit in the other game; the copies are not green. Pair test: tools/coop/autobuild_3rd.txt in game A (the 100% save
autobuilds the Fanplane, the first schematic: a wing, three fans; Autobuild is the wheel's down-right slot) with
`TOTK_COOP_TEST_ABLOG=1 TOTK_COOP_TEST_ABHIT=36` (runtime/coop_autobuild.c: A's Link is put by a fan at 36 s and the
script's second Y hits it; every live device's on state is logged in both games). It reproduces the report: in A all
three fans go on (on, energy 10) 0.4 s after the hit; in B only the copy fan the proxy's swing reaches.
- Not Ultrahand glue: the build calls none of createScotch, the glue step or doScotch, and every device's scotch list
  is empty. Ultrahand glue does not reach the copies either way: there is no bond to send. Glue made in B between the
  copies (coop_glue.c coop_glue_pair, `TOTK_COOP_TEST_ABGLUE=1`) holds, and changes nothing; glue made by our call on
  two new devices in one game (TOTK_COOP_TEST_GLUE) gives them no group either. Confidence: sure (logged).
- What makes the devices one unit: each device's SpecialPowerReceiverComponent ([[actor+0x228]+0x480]) +0x28 points
  to one group object shared by the whole contraption (vtable main+0x361f1b0; ActorLinks to every device from +0x78,
  0x18 apart), and its +0x20 flags read 0x0080000000000202 (B's copies: +0x28 0, +0x20 2). The group pointer is
  written by main+0x1d93978, a component virtual (x0+8 the actor; x2 a pooled physics object, vtable main+0x392c5e8,
  made by main+0x7a8784; [x2+0x90]+8 the group), reached from a queued actor message (lr main+0x2999d34, the job
  chain main+0x29a3060 <- 0xacf8dc) when the build is confirmed (A at 30 s). The group, the physics object and the
  holder sit at the same addresses run after run: they are pooled, so their constructors do not run at a build.
  Confidence: sure for the fields and the writer (write watch, hook); the group's class is a guess.
- The fans turned on by the hit's neighbour: their on bool (coop_players.c dev_on_flag) is set by each fan's own
  behaviour graph in its update (setter main+0x9fb82c from main+0x9fa920 <- 0x9f91d8 <- 0x1066bb8 <- 0x42e228 <-
  0x41fc08 <- 0x2f175c <- 0x2f152c <- 0x29a3060), the same path as a hit fan's: each reads the contraption's state,
  and a copy with no group has none. Earlier tests: writing the bool or the energy words by hand starts nothing.
- Pooled CombinedActorIntegrator actors: idle ones wait at the origin; the active one follows the contraption (A's
  at the Fanplane). A sent its active one as a moved object, and B took one of its own idle ones for it by key and
  moved it there. Now left out of the world objects (coop_players.c prop_skip). Its first 0x1000 bytes hold no pointer
  to the devices or the group.
- The autobuild type: requestCreateActor (main+0x599670) copies the create argument's +0x80 into the create
  information's +0x78 (its locals: the argument's +0x68/+0x70/+0x78/+0x80 go to information +0x60..+0x78). The
  originals carry main+0x3a4e008 there, a static object whose vtable (main+0x361d910) answers the constructor's type
  check: not a record. coop_game.c coop_request_create_autobuilt makes a copy so, and it is labelled like the original
  (`TOTK_COOP_AUTOBUILT=1`; off by default). Still not green: the tint is not the label. Strings for the tint:
  AutoBuilderBuildStateRealColorR/G/B and AutoBuilderRealStateAplha (an AutoBuilder parameter read by main+0x1d398e4,
  R at +0x44), the reader of which was not found.
- The schematic is a CombinedActorInfo blueprint (Game/BluePrint/CombinedActorInfo/%s.cai, loaded by the Autobuild
  screen, main+0x2138ab4, into the object at [main+0x3a62ef0]).
- Where the group comes from: a connection graph ([main+...] object at 0x1b98f1000 run after run, vtable
  main+0x392cae0), updated each frame in a job (main+0x181e40): its pending edges (+0xe8 list) join bodies' nodes,
  each connected set gets a pooled set object (vtable main+0x392c5e8), whose holder's group (main+0xc16144 ->
  main+0xc165c4 -> main+0xc16b94 fills the members, main+0x14f334 their links) is handed to every member's component
  by main+0x1d93978. An edge is added by main+0x18ba00 (graph, edge, node A, node B), from requests in a ring the job
  runs (main+0x185210 -> main+0x185770). The autobuild's edges (class main+0x392c2a0, made by main+0xa2cc20 from a
  description: +0x10 and +0x18 the two bodies, two pivot frames) come from main+0x1916830 (the bond maker), called
  by main+0x1c05b84 (component, body index, other body, world matrix, other's world matrix, world pivot) in an actor
  update (lr chain main+0x420da8 <- 0x43006c <- 0x4702cc). main+0x1c05a98 is its plain form: it takes the pivot from
  the two bodies itself (main+0x299daec, as doScotch does). Write watches (TOTK_COOP_TEST_ABWATCH) and hooks found
  each step; addresses of the pooled objects were the same in every run.
- Why Ultrahand glue made by our call gives no group: doScotch makes an edge too (main+0x5fa1a4 / main+0x5f9dd8,
  class main+0x392c4c0), but in game A its two bodies were joined and still no group came, and on a copy its body
  indices stayed -1: a body whose +0x68 has bit 24 set is left out of any edge (main+0xa2d39c, main+0x5f9dd8). Every
  copy's body has it set (0x104009400; the originals' 0x100009400); what sets it was not found (writers of the word:
  main+0x25a450, 0x182eb0, 0x188500, 0xa0faac, 0xa11af0).
- The fix (coop_bonds.c): the owner sends the groups its own objects form; the other game clears bit 24 on its
  copies' bodies and bonds each to the group's first copy with main+0x1c05a98. Pair test (autobuild_3rd.txt in A,
  autobuild_b_3rd.txt in B, `TOTK_COOP_TEST_ABLOG=1 TOTK_COOP_TEST_ABHIT=36 TOTK_COOP_TEST_ABHIT_B=46`): the copies
  join one group in B; A's hit turns all three fans on in A and, through the proxy's swing, all three copies in B;
  B's hit turns all of them off in both (B's copies first, then A's contraption through B's proxy in A). Not tested:
  a contraption made by hand with Ultrahand, one taken apart (the copy is made again, untested), more than one
  contraption, a contraption flown while the copies are bonded.
- More pair tests (2026-11-08, after the fix):
  - Taking a part off with Ultrahand (autobuild_part_3rd.txt, `TOTK_COOP_TEST_ABHIT=36` only to put the Link by a fan:
    the wheel's right slot, A grabs, the right stick shaken 2 s, B lets go): the game deletes the part (state 11, as
    for an autobuild part taken off; removeScotch on it, TOTK_COOP_TEST_ABPART, did the same) and makes the rest one
    group again, under another group object. The copy of the part went away in B with it and the others stayed
    bonded (pairs are kept by member; the group object changing parts nothing). Same picture on both screens.
  - Two contraptions (autobuild_two_3rd.txt, `TOTK_COOP_TEST_ABPLACE=34 TOTK_COOP_TEST_ABHIT=50`): each bonded as its
    own group in B; a hit on the first turns on its three fans only, in A (logged) and in B (logged for the first;
    the second was out of the log's reach and out of view in B).
  - Both ways again: B's hit missed once (its Link was put by a fan that was blowing); when it lands, both games end in
    the same state at the same moment.
  - Ultrahand's glue effect actor (UltraHand_Connection_Effect2) was sent as a made object and copied: now skipped.
  - A contraption glued by hand: not tested. Scripted Ultrahand grabs and shakes (above), but bringing one part to
    another to glue did not work by script (glue_hand_3rd.txt: no glue was made; two fans made stacked lost the top
    one, deleted by the game at once). Whether glue by hand makes the same group is not known: glue made by our own
    call (createScotch and doScotch) does not.
- Green, still open: none of the label, the glue component's flags (+0x20 0x0080000000000200 set by hand) or the
  shared group tints a copy. A dump of every component of an original fan and a copy (TOTK_COOP_TEST_ABDUMP) was not
  a clean diff (the copy lacked nine of the original's components at the dump), nor was one of their models (the two
  layouts were shifted against each other). Also tried on copies with no tint: actor +0x440 bit 0x200000 (set on the
  originals, not on copies). The AutoBuilder parameter reader main+0x1d398e4 is not run in play (hooked: never). The tint's parameters are
  AutoBuilderBuildStateRealColorR/G/B (the reader of the param struct, main+0x1d398e4, puts R at +0x44); who applies
  them to a model is not found.

## Device state on the copies, the steering stick, glue by hand (open, 2026-11-08)
the tester's reports on main 46b510c: flame emitters on in the builder's game and off in the other; the steering stick
works only for the game that made the contraption (held by player 2 nothing turns on anywhere, held by player 1 only
player 1's game); a contraption glued by hand shows no glue in the other game and one hit turns on one piece there.
TotK Online (side-session notes, totk-online-how-it-works.md 3 and 7): one owning game per glued group, handed to the
game whose player is nearest its middle every few seconds and claimed while a player holds it; copies run their own
update and are pulled toward the owner's place. It syncs position, life, collision, glue and enemy actions; no device
state.
What a device's "on" is, from pair tests with autobuild_3rd.txt (`TOTK_COOP_TEST_ABLOG=1 TOTK_COOP_TEST_ABHIT=36`):
- The contraption's group object (SpecialPowerReceiverComponent +0x28) does not change at all when a hit turns its
  fans on (every word logged against the last, every 0.5 s through the hit and the next 20 s).
- No actor message is sent to the other fans: the two message senders (main+0x2b76c34, the one the stick's AI node
  "OneShotSendMessageRideOnCombinedControlStick" uses with 0x6b510003 to actor +0x378, and main+0x1c0cc4c) were
  hooked through the hit; none went to a device's queue.
- A neighbouring fan turns itself on in its own update: every word of its actor (0x1000) and components (0x200 each)
  logged every tick through the hit (TOTK_COOP_TEST_ABTRACE) shows the on bool, components 16/17/21/22 +0x170/+0x130/
  +0xe8/+0x90 going 0x8000 -> 0xa000 and component 166 ([actor+0x228]+0x530, which the bool's writer main+0x9fa28c
  reads) changing in the same tick, and nothing changing before it in those ranges: what it reads is elsewhere.
- Writing the on bool with its change count on a bonded copy in the other game (TOTK_COOP_TEST_FANTOGGLE in game B
  only): once the copy was on (energy 10), it stayed on through every write; the bool alone does not switch a device.
So no call or write that turns a device on or off is known yet; device state from the owner, and a stick held in the
other game reaching the owner's devices, both wait on one. A hit does it (the proxy's swing), but cannot be aimed.
- The steering stick, pair tests (tools/coop/autobuild_stick_3rd.txt builds the Fanboat, the sixth schematic: five
  fans, a steering stick and wooden parts; `TOTK_COOP_TEST_ABLOG=1 TOTK_COOP_TEST_ABSTICK=50,<player>` puts that
  player's Link behind its stick, its script's A takes it; autobuild_stick_b_3rd.txt for game B): game A holding its
  stick turns its fans on in A only; game B holding the copy stick turns B's copies on (they are bonded, coop_bonds.c)
  and A's stay off. So the stick works in the game that holds it, and the other game's devices do not follow (no
  switch, above).
- Claims (coop_bonds.c, TotK Online's way): the game whose Link rides another player's contraption, 2 m nearer it than
  that player's Link, runs it: its copies are not placed and the owner moves its contraption to them as one body.
  What went wrong on the way, all in the stick pair test: unfrozen by the claim, a wooden plank's copy went away at once
  (state 7) and our "a copy went away here" rule made the owner delete its own; bonded copies are now never frozen,
  and one that goes away while claimed is made again without telling the owner. Each owner part pulled to the
  claimer's place of it, the owner's contraption came apart, and an autobuilt part pulled off is deleted by the game:
  the owner now moves only the root, by the rigid move from its own place to the claimer's, at most 0.4 m a tick (a
  force_to past 0.5 m teleports), and its bonds carry the rest. Standing by a fan the owner had just hit counted as
  riding it, and the claim pulled the owner's flying contraption back to the late copies: hence the 2 m rule.
  Result: B holding the copy stick runs B's fans and A's Fanboat follows B's place within 0.2 m, no part lost; A's
  fans stay off (their state is not sent). The plain hit test (autobuild_3rd.txt) makes no claim. Not tested: a
  contraption flown far by the claimer, a claim handed back while it moves, two claimers.
- Glue by hand: glue made again in the other game (coop_glue.c make_bond) uses the request's base class, whose
  vtable slot 4 answers 0, so createScotch's tail (main+0x1d61858) copies none of the request's four vectors into the
  entry (+0x51c..+0x548). Ultrahand's own request is the same class: main+0x1d5565c makes it 0x1b8 bytes, zeroed, with
  the base constructor main+0x1c0c4ec, and puts the base vtable back when it is done (decoded from the generated C;
  Ghidra was down), so that is not why no glue shows. UltraHand_Connection_Effect2, an effect actor Ultrahand makes
  on a contraption, was left out of the world objects that morning (prop_skip) and is copied again: it may be the
  glue's look (not checked). Whether glue by hand gives its pieces the shared group
  (which coop_bonds.c would then send) is not known: a scripted Ultrahand grab works on a contraption
  (autobuild_part_3rd.txt) but not on loose fans (glue_hand_3rd.txt: the grab only flashes; with the fans 2.5 m ahead
  of the camera, TOTK_COOP_TEST_GLUE=...,far, too: the camera turns after the Link is put and again in Ultrahand, so the
  reticle is not on them).
- Who calls into a device at a hit (TOTK_COOP_TEST_FANHIT, coop_spawn.c, now with the profiler on only from 3.5 s
  before the hit; aimed by hit_test at the build's second fan, or with TOTK_COOP_TEST_FANHITDEV=1 at the hit one):
  for a neighbouring fan only its own behaviour graph (main+0x1066bb8, from its update): the on bool's writer node
  main+0x9f97d0/0x9f97dc and its calls main+0x1d38db0 (a check: the device's component 166 state at +0x28, a named
  bool of hash 0xb835f400, the device's contraption via main+0x1861a1c), main+0x1952fb0 / main+0x1951350 (component
  getters, [actor+0x228]+0x530 and +0x120) and main+0x245c3c4 (in main+0x245c114, which also takes the 0x6b510001..3
  messages), and nodes main+0x2489xxx calling main+0x4dc9f8 / main+0x62e368. For the hit fan, the same plus its hit
  handling from its update (main+0x42a23c -> main+0x1b914e8 -> main+0x1b91b84 -> main+0x1b92cb0 / 0x1b93160 /
  0x1b9326c). Traced every tick, nothing in the neighbour (actor, components' first 0x200 bytes, component 166 to
  0xa00) changes before the tick it turns on: a device pulls its state from its contraption in its own update, and
  a hit is what starts it in one device. So a copy contraption needs one device started, not each; what starts one is
  the hit's reaction (main+0x1b914e8 on the damage record), which writing the record by hand does not (2026-11-05).
- Messages: 0x6b510001..0x6b510008 sent to each of the Fanplane's fans as the stick's node sends 0x6b510003
  (main+0x2b76c34 to actor +0x378, the sender this Link; TOTK_COOP_TEST_ABMSG, build/ab_noy.txt-style script with no
  hit) turned none on.
- Glue by hand, again: grabbing the Fanplane with Ultrahand works from beside it (autobuild_glue_3rd.txt with
  `TOTK_COOP_TEST_ABHIT=36 TOTK_COOP_TEST_ABLOOSE=1`: a loose fan made 1.6 m to the Link's right, the Fanplane grabbed
  and walked right), but it did not reach the loose fan, and A's group kept its four parts: still no glue by hand.
  On hand-glued copies and coop_bonds: it bonds only copies (coop_is_copy), so a world object glued by hand (a plank
  the other game has itself) is never bonded there, only devices made from capsules; and it sends a group only if
  the owner's game gives the glued parts one, which is not checked for glue by hand.
- The stick's own state: its on bool goes 0 -> 1 when a Link takes it, with actor +0x1d0 = 0x79_00000001 and component
  166 +0x90 0 -> 3 (TOTK_COOP_TEST_ABTRACE=SpObj_ControlStick_A_01,51.8 in game B). Holding the bool at 1 on the
  owner's stick with nobody on it (TOTK_COOP_TEST_ABSTICKON) turns nothing on: the bool is an effect of the Link
  holding it.
- A hit's contact replayed on a device (TOTK_COOP_TEST_DEVHIT=<s>,..., coop_world.c dev_contact / coop_dev_hit): the
  first contact a sword hit leaves on a device's damage component (its contact list, as coop_world.c replays a
  stand-in's hits on enemies: kind 2, power 30, by Weapon_Sword_070 / Player) is kept and copied, at a later hit
  update, onto another device's list, with the id bumped and the body moved to the one the real hit landed on (a
  fan's hit body is its main body + 0x330; the main body took the contact and nothing ran). The device then runs the
  whole hit reaction a real hit runs (TOTK_COOP_TEST_FANHIT: main+0x42a23c -> main+0x1b914e8 -> main+0x1b91b84 ->
  main+0x1b92cb0 / 0x1b93160 / 0x1b9326c, the same 13 functions), but its behaviour graph does not turn it on: of a
  real hit's functions only the graph's main+0x2489xxx nodes (main+0x62e368, main+0x4dc9f8: a light ramp, an effect of
  turning on) are missing. Tried in autobuild_two_3rd.txt (`TOTK_COOP_TEST_ABPLACE=34 TOTK_COOP_TEST_ABHIT=50`) on
  the second, unhit contraption's fan, 6 s and 14 s after the hit, and one frame after it while the swing is live
  (DEVHIT=53.5): it stays off; on the hit contraption's fan, already on, it does not turn it off either. So the
  contact plus the hit reaction is not what decides; the graph reads something else the real attack sets (the
  attacker's side, not the contact), not found. Steps after this one (turning off, sending the owner's on state in
  "CB", the sweep over device kinds) wait for a switch that works (2026-11-08).
- **The switch (found 2026-11-08 afternoon): component 166's byte +0x8d, bit 3.** A device's update runs main+0x4a3500
  (from the actor update main+0x41bb20) with x19 = component 166 ([actor+0x228]+0x530) and x20 = its actor (+0x18).
  When its byte +0x60 is set, its +0x64 is not 0x17 and its byte +0x8d has bit 3, it takes the branch at
  main+0x4a440c: the actor's SpecialPowerReceiverComponent (+0x480) is asked main+0x9fb7fc(comp, 7, &flag), then
  main+0x9fb7cc, and component 166 is asked for state 4 (main+0x1c08774 with {1, 4}: its +0x20/+0x24 request), after
  which the device and the rest of its contraption turn on. Found by tracing (TOTK_COOP_TEST_ABTRACE, every tick) a
  really hit fan against a replay's fan: the damage component's hit record is the same in both, but only the real
  hit's component 166 changes: +0x8d 0x61 -> 0x69 (bit 3) -> 0x63 (bit 1) -> 0x61 within 0.05 s, +0x28 8 -> 4 -> 8,
  +0x90 0 -> 1. The profiler with component 166 as the argument (TOTK_COOP_TEST_FANHITC166=1) showed main+0x1c08774
  from main+0x4a4f1c at the real hit only. Test (TOTK_COOP_TEST_DEV8D=08 with TOTK_COOP_TEST_DEVHIT=58,
  autobuild_two_3rd.txt, `TOTK_COOP_TEST_ABPLACE=34 TOTK_COOP_TEST_ABHIT=50`): bit 3 set on one fan of the unhit
  contraption (+0x8d 0x61 -> 0x69, +0x60 1) turned all three of its fans on (on, energy 10) within 2 s; the hit
  contraption was unchanged. On an on fan (+0x8d 0x71) bit 3 did not turn it off: off is not known yet (bit 1's
  branch, main+0x4a45ac, only clears bit 1). Off, found next:
- **Off is the same bit, set on every device that is on.** A contraption turning off (the build's fan hit a second
  time, build/ab_twohits.txt-style script: autobuild_3rd.txt with Y at 48 s and 50 s; traced with
  TOTK_COOP_TEST_ABTRACE=SpObj_WindGenerator_A_01,47.9) shows the same as on: component 166 asked for state 4 (+0x28
  8 -> 4 -> 8), and +0x8d's bit 4 (0x10) goes 0x71 -> 0x61 (bit 4 is the device's own on mark; bit 3 is set and taken
  within one tick, between two snapshots). Bit 3 set on one on fan turns it off for 0.5 s (energy 0), and the others,
  still on, turn it on again; set on all three at once (TOTK_COOP_TEST_DEV8DALL=1) they go off by 1 s and stay off,
  and set again on all they come back on (autobuild_3rd.txt, `TOTK_COOP_TEST_ABHIT=36`, DEVHIT=46,52). So bit 3 is a
  toggle a hit sets, and a contraption is switched by setting it on every device not yet as wanted.
- **Sync (coop_bonds.c dev_follow).** The owner's "CB" entries carry each member's state (flags: 2 a device with the
  switch, +0x60 set; 1 on: [comp 11]+0x348 bit 0x2000), sent at once when one changes (checked 10 times a second).
  The other game, once its bonded copies have differed from the owner for 0.8 s, sets bit 3 on each copy device not
  yet as wanted, then waits 2.5 s before it would again. A claimer sends its copies' state in "CL" (flags) and the
  owner's devices follow it the same way. TOTK_COOP_DEVICE_SYNC=0 turns it off; TOTK_COOP_TEST_DEVSYNCLOG=1 logs each
  followed group every 0.5 s. Pair tests (port 41991):
  - A holds its stick (autobuild_stick_3rd.txt with B at 62 s to let go, `TOTK_COOP_TEST_ABSTICK=50,1`, B idle): A's
    fans on at 52.5 s; B "player 1's contraption turned on here", its copies on; A lets go, A's fans off at 63.5 s, B
    "turned off here", copies off. Before, B's copies stayed off.
  - B holds A's copy stick (autobuild_stick_b_3rd.txt with B at 62 s, `TOTK_COOP_TEST_ABSTICK=50,2`): B claims it,
    its copies on; A "my contraption run by player 2 turned on here", A's fans on at 55.5 s; B lets go, A's fans off
    at 67.5 s.
  - A hits its fan on, off, on (Y at 40, 48 and 50 s): B's copies followed each time through the stand-in's swing and
    the sync switched nothing (no "turned" line): no double toggle.
  Not covered: a device alone (only groups of two or more are sent; its copy follows through the stand-in's swing
  only), and a copy device asleep (below).
- **Zonai device kinds and the switch** (TOTK_COOP_TEST_DEVSWEEP=30 with TOTK_COOP_TEST_DEVSWEEPNUDGE=1: each kind made
  loose by this Link, moved 5 cm by speed, bit 3 set, 3 s, set again, 3 s; one game). Component 166 sits at
  [actor+0x228]+0x530 on every kind; +0x64 is the kind's number. Bit 6 of +0x8d (0x40) reads as "awake": a device at
  rest with 0x21 there did not take the bit (the first run, without the nudge: fan, flame, frost), the nudge woke flame
  and frost. Only fans were tested as copies in the other game.

  | Kind | Actor | +0x64 | +0x60 | Switch on | Switch off | Notes |
  |---|---|---|---|---|---|---|
  | Fan | SpObj_WindGenerator_A_01 | 15 | 1 | yes | yes (all at once) | contraption and pair tests; alone at rest it did not take the bit |
  | Wheel | SpObj_FastWheel_A_01 | 17 | 1 | yes | not seen | drove off and was gone (state 11) before the second switch |
  | Rocket | SpObj_Rocket_A_01 | 10 | 1 | yes | off by itself | burns out on its own |
  | Flame emitter | SpObj_FlameThrower_A_01 | 2 | 1 | yes | yes | only once awake |
  | Frost emitter | SpObj_SnowMachine_A_01 | 3 | 1 | yes | yes | only once awake |
  | Shock emitter | SpObj_ElectricBoxGenerator | 8 | 1 | yes | yes | |
  | Beam emitter | SpObj_Beamos_A_01 | 1 | 1 | yes | yes | made 3 m ahead (+0x8d 0x61): both ways, twice (DEVSWEEPKINDS); at 0x21 it did not take the bit |
  | Light | SpObj_FlashLight_A_01 | 11 | 1 | yes | yes | |
  | Cannon | SpObj_Cannon_A_01 | 21 | 1 | yes | yes | |
  | Balloon | SpObj_BalloonEnvelope_A_01 | 4 | 0 | no switch | - | lit by fire, not a powered device |
  | Spring | SpObj_SpringPiston_A_01 | 22 | 1 | yes | - | fires once |
  | Sled | SpObj_SlipBoard_A_01 | 12 | 0 | no switch | - | |
  | Steering stick | SpObj_ControlStick_A_01 | 16 | 0 | no switch | - | turns its contraption on by its rider |
  | Battery | SpObj_EnergyBank_A_01 | 23 | 1 | no | - | +0x64 0x17 is the kind main+0x4a3500 skips; +0x8d 0x20 in three runs: passive |
  | Cart | SpObj_Cart_A_01 | 14 | 0 | no switch | - | |
  | Wing | SpObj_LiftGeneratorWing_A_01 | 5 | 0 | no switch | - | |
  | Hydrant | SpObj_WaterPump_A_01 | 20 | 1 | not seen | - | at 0x21 took the bit and stayed off; made at 0x61 it was deleted by the game (state 7 -> 11) in two runs |
  | Time bomb | SpObj_TimerBomb_A_01 | 24 | 1 | yes | - | arms once |

- What +0x8d bit 6 (0x40) means is not known. It is not "awake" after all: moving a device by speed did not set it
  (a beam emitter at 0x21 stayed so), and the same kind was 0x61 or 0x21 from one run to the next as it was made at
  other places (3 m ahead on the In-isa stairs: 0x61). Devices at 0x21 left the bit (fan, flame, frost in the first
  sweep), but a spring and a time bomb at 0x21 went on with it. dev_follow does not try to wake a device.
- **A device alone** (with the switch, in no group) is sent in "CB" too, group 0, one entry each, and its copy follows
  it the same way (coop_bonds.c single_state). Test: TOTK_COOP_TEST_GLUE=SpObj_WindGenerator_A_01,SpObj_WindGenerator_A_01
  with TOTK_COOP_TEST_GLUENONE=1 TOTK_COOP_TEST_GLUEON=1 (glue_pair_3rd.txt in A, idle B): the two fans not glued, the
  first switched on at 8 s, both switched at 14 s (the first off, the second on); B: "player 1's
  SpObj_WindGenerator_A_01 turned on here" for the first, then on for the second and off for the first.
- **Glue by hand and groups.** the tester's play log (build\v143play, player 2's game, which glued by hand) shows that
  the first glue by hand (stick to wing) gave both parts one group in the owner's game, which it sent ("a
  contraption of mine, sent: SpObj_LiftGeneratorWing_A_01 SpObj_ControlStick_A_01"). So glue by hand does make the
  group the packet and the switch work on. Its later parts (cannon, battery, fans, flame emitter) are not in that log:
  the line was printed for the first ten sends only. It is now printed whenever a group's parts change ("(N parts)"),
  so the next play log shows whether they joined.
- Glue by hand by script, again (TOTK_COOP_TEST_GLUENEAR=<s>,<m>: a loose fan made <m> past the held build's part
  farthest to the Link's right; build/glue_near*.txt-style scripts from autobuild_glue_3rd.txt, D-pad up to push the
  held build, A to let go; game screenshots with -ShotsA): the hold works in some runs ("Move forward/back" shown) and
  not in others (the reticle on the stand-in, which Ultrahand highlights). No glue was made in four runs. The green
  glow seen past the build was the stand-in, highlighted.
- The game's own glue call (coop_glue.c make_bond: createScotch and doScotch, as Ultrahand) on two made fans: the
  entry is made and gone within 4 s ("the edge 0" at 4 s), and no group comes; their bodies' +0x68 bit 24 is clear
  (0x100009400), so that is not the cause (TOTK_COOP_TEST_GLUECLEAR changes nothing). Glue by hand also loses its
  entry about 2 s on (the tester's log: "no longer glued here: taken off the list" after each glue) and yet makes a group:
  what turns a hand glue's entry into a lasting bond after the release is not found.
- The autobuild's bond on two of the owner's objects (TOTK_COOP_TEST_GLUEBOND=1, coop_bonds.c coop_bonds_bond:
  main+0x1c05a98, as coop_bonds bonds copies) gives them one group in the owner's game at once ("a contraption of
  mine (2 parts)"); B bonds its copies ("share a group here"); with TOTK_COOP_TEST_GLUEON=1 the owner's first fan
  switched on turns both on in A, and B "turned on here (2 of 2)"; both switched off at 14 s, B "turned off here",
  0 of 2 on. So a group that reaches "CB" is followed whatever made it; whether glue by hand's later parts join the
  owner's group is what the new log line will show.
- Not done: the glue's look in the other game (createScotch's request with Ultrahand's attachment points; no
  decoding of them found), and bonding world objects the other game has itself (coop_bonds bonds copies only: a
  plank from the map glued by hand would stay loose in the other game).
- Glue by hand by script, a fifth and sixth run (glue_near4-style, B's Link asked to stand aside with
  TOTK_COOP_TEST_ABPLACE in B, which did not move it): held, pushed and let go, the loose fan beside the build; no
  glue. Both the glue's look and B's own world objects need a real glue by hand first: one by the tester in a logged
  session (the "(N parts)" line and coop_glue's "made here") is the way to get it.

## Running the game's own Autobuild in the other game (feasibility, open, 2026-11-08)
the tester's idea: the other game builds the owner's blueprint itself, so the parts come out green and as one unit.
- Where the parts are made: the Autobuild ability's update (main+0x9a2c00, a large state machine, called through a
  vtable) fills per-part records and then calls main+0x1d3b470(obj) directly (at main+0x9a70ec, on a job thread:
  frame chain main+0x9a70ec <- 0x75d9c <- 0x74218 <- 0xacf8dc). main+0x1d3b470 makes each part with requestCreateActor
  (main+0x1d3bb98), with the create argument and callback in "The green tint" below.
- obj (0x1ec715548 in every run: a pooled object) holds the blueprint's path at +0x14,
  'Game/BluePrint/CombinedActorInfo/Obj_AutoBuilderDraft_00.cai' for the first schematic. [obj+0x3c50] points into
  it at the blueprint as loaded, a CombinedActorInfo in memory: magic "CmbAct", a byte 2 at +8, a byte at +9 that
  main+0x1d3b470 must find non-zero to build anything, and part records from +0xc, 0x118 bytes each, each with the
  part's transform (main+0x1d3b470 walks them with x27). So the layout is the .cai's (for the Autobuild screen's own
  schematics; history designs not looked at).
- The same path makes the screen's previews: more parts (a stick, fans, rockets) were made through main+0x1d3bb98
  while the script scrolled, some on another thread's stack.
- Calling it again (TOTK_COOP_TEST_REBUILD=40 with TOTK_COOP_TEST_CREATEDUMP=1, tools/coop/autobuild_noy_3rd.txt: autobuild_3rd.txt
  without its sword swings) on the kept obj, 10 s after the build: it returns at once and makes nothing; byte +9 reads
  0 after the build, and setting it to 1 or 4 first (TOTK_COOP_TEST_REBUILDN) makes nothing either. The caller sets
  more per part just before the call (bytes +0xd7 and +0x117 of each record cleared, words written to an output
  list), so main+0x1d3b470 alone is not the whole build.
- Not answered yet: the cost (where zonaite, battery or capsules are taken: in main+0x9a2c00 before the call, as far
  as can be seen), what is tied to the Link (obj is the ability's), mapping the made parts to the owner's numbers, and
  whether the parts made this way are green at all in the other game (the tint may be put on after main+0x1d3b470
  returns: the same create arguments on copies give none). Ghidra was down; this is read from the generated C.
- The preparation replayed (coop_autobuild.c rebuild_test, TOTK_COOP_TEST_REBUILD=40 with TOTK_COOP_TEST_CREATEDUMP=1
  and TOTK_COOP_TEST_ABLOG=1, tools/coop/autobuild_noy_3rd.txt): the ability update, before its call at main+0x9a70ec,
  checks the blueprint's +9 (parts) and +0xa (both 1..0x15), releases the build's entry pool (obj+0x4d10 counter
  stepped and its +0x220 cleared, the obj+0x4f48 used entries put back on the free list at obj+0x4f58), takes one entry
  per part from the pool at obj+0x1b0 (main+0x1d3cfc8) and fills it with the record's +0x98 and +0xd8 (addresses), its
  +0x90 and +0x94, clearing the record's +0xd7 and +0x117. With the blueprint as it was at the build put back (kept at
  the first part's create: +9 reads 0 afterwards) and that done from our tick, main+0x1d3b470 made all four parts again
  (a wing and three fans, ids 0x402..0x405, with the autobuild's tag and callback), 10 s after the first build.
- But they are not a finished build: they stand where the first build was first put (y 1516.7, 1 to 2 m above where it
  settled), do not fall, and nothing new is drawn (game screenshots before and after are the same). So the parts come
  out as the build's first stage, and the rest (the see-through preview turning solid, physics, the group and the
  green look) is done by main+0x9a2c00's later states, which this does not run. Their place is not in the records
  (relative) nor as three floats in obj (TOTK_COOP_TEST_REBUILDDX found none): probably a matrix, not found yet.
- The confirm, found with the profiler on the build's first fan from when it is made (TOTK_COOP_TEST_FANHITAB=1
  TOTK_COOP_TEST_FANHIT=29.97): main+0x9a2c00 at main+0x9a7260 calls main+0x1d3c0d4([ability+0x51f0], the build's
  object) when a task check on ability+0x52c8 (main+0x1b2f20) passes and a byte at +0x1f5 is set; the parts then see
  main+0x2af548c, main+0x194f8dc and main+0x1c071cc from it. main+0x1d3c0d4 checks first (main+0x9a83dc) that the
  object's byte +0x3c70 is clear, that each part entry's task (+0x48) is done and the object's task at +0x3c58 is not
  (main+0x1b2f20: a task handle, its id at +0x10), then the blueprint's "CmbAct" magic and its +9 (parts) and +0xa (3 for
  the Fanplane), which the build step zeroes again.
- Called from our tick on the second build (TOTK_COOP_TEST_REBUILDOK=42): it returned 0 every time, with the blueprint
  put back and +0x3c70 cleared (TOTK_COOP_TEST_REBUILDOK70=1) or not: the tasks it checks are the ability job's.
- The second build's parts made solid by hand (TOTK_COOP_TEST_REBUILDCLAIM=1: each part's create handle, its pool entry
  +0x60, put in the world with coop_claim): state 3 -> 4, +0x440 0x630300, +0x5d0 a place, they fall and settle, and
  the game draws them, solid, behind the first build (screenshots at 44-48 s). But they are not green (tan wing, grey
  fans, beside the green first build) and in no group (not one unit). So the green and the group come from the
  confirm (or after it), which does not run this way.
- Verdict: a second build is made and drawn from code, but neither green nor one unit; the confirm, which most likely
  gives both, needs the ability job's own tasks. Driving it means running it from the ability's job (main+0x9a2c00's
  state, ability+0x52c8 and +0x1f5) rather than from our tick: not done. Not tried in B; cost and Link ties open.
- Ghidra (project totk143, 2026-11-08 evening): no function is defined at main+0x9a2c00, main+0x9a7260 or
  main+0x1d3c0d4 in its program, so it does not decompile them; cross-references work: main+0x9a2c00's only reference
  is a vtable slot at main+0x3695238 (the slots around it: main+0x8a28d0, 0x92d864, 0x18b321c.., 0xe45b94, 0x1d5391c),
  so it is a virtual update, called by whoever holds that object (the Link's Autobuild, it seems).
- The confirm block in main+0x9a2c00 (generated C, from main+0x9a7164): it reads the build object's byte +0x3c71, the
  object at ability+0x5130 (its +0x68 bits 10 and 11 pick the branch), byte +0x1f5 of the object in x26, and runs the
  confirm once the task at ability+0x52c8 is done. All of it is the ability's own state while the Link is in Autobuild:
  after the first build the update no longer reaches it, and in game B it would need B's Link in its Autobuild (the
  screen, the hologram, the ability's state) with A's blueprint loaded, and the build's cost would be B's. Not tried.

## The green tint of autobuilt parts (done, 2026-11-08 evening)
- The lead (the tester's find, the "green_begone" mod, no licence: behaviour only; shared notes green-tint-lead.md): the
  mod blanks an atomic OR of bit 0 into +0x150 of its argument; in 1.4.3 the function is main+0x1c07114. The Autobuild's
  confirm calls it (main+0x1d3c0d4 at main+0x1d3c4e0) on [[actor+0x228]+0x480], each part's
  SpecialPowerReceiverComponent (main+0x194f69c returns it: read in the generated C; Ghidra headless decompiled
  main+0x1c07114 itself). It also ORs bit 0 into +0x27c, sets +0x264 / +0x268 and the +0x20 flags, clears bit 2 of the
  actor's +0x5ac and touches the actor's component +0x280.
- coop_bonds.c mark_autobuilt calls it once on each copy of an autobuilt part (the owner's COOP_OBJ_AUTOBUILT flag,
  coop_players.c coop_copy_autobuilt) when the copy is bonded. Pair test (autobuild_noy_3rd.txt in A, idle B, game
  screenshots in B at 40-44 s): B's copy Fanplane is green, as A's; the hit pair (autobuild_3rd.txt with Y at 48 and
  50 s) still shows B's copies on, off and on with A. TOTK_COOP_TEST_GREEN=0 leaves it out, =all marks every bonded copy.
- Taking a part off (2026-11-08 evening). An autobuilt contraption is meant to come apart whole when a piece is taken
  off (the tester), whoever takes it. Marked as soon as their bond was asked for, a copy was destroyed by the game before
  the graph had joined it (a fan copy gone in B with nobody near it; then the owner deleted its own): copies are now
  marked only once they share a group. Pair tests: B pulls the wing off its copy (autobuild_part_b_3rd.txt with
  `TOTK_COOP_TEST_ABHIT_B=46`): B's copy goes, B's other copies follow, the owner deletes its wing and its fans go too:
  both games empty. A shakes a fan off its own (autobuild_part_3rd.txt, `TOTK_COOP_TEST_ABHIT=36`): A destroys the fan,
  B drops its copy, B's other copies come apart (state 7) and tell A, which deletes the rest: both games empty. (A's own
  game had not destroyed its rest by then; the notices did.) The Ultrahand grab in autobuild_part_3rd.txt does not take
  every run (the Link ends up facing away).

### Earlier research (before the lead)
- What the autobuild passes when it makes a part (TOTK_COOP_TEST_CREATEDUMP=1, coop_spawn.c create_dump on
  requestCreateActor; the autobuild's call is main+0x1d3bb98 in main+0x1d3b470) against coop_game.c request_create:
  the create argument's +0x48 0x0101010100000001 (ours 0), +0x58 a parameter list (only "SpecialParts_InitOn", for
  spring pistons), +0x80 the autobuild type object (we pass it with TOTK_COOP_AUTOBUILT=1), +0x90 7 (ours 3), x4 0
  (ours 1), and x7 a creation callback: an object of vtable main+0x3694e18 with a pointer to a byte, whose call
  (main+0x1d43dc8) writes that byte to the new actor's init values 0x11 and 0x13 (main+0x63336c on x1+0x14). The byte
  was 0 for the first four parts made and 1 for six more on another thread (the schematic previews, it seems), and the
  built Fanplane is green, so it is not the tint.
- Passing all of it on B's copies (TOTK_COOP_AUTOBUILT=1 with TOTK_COOP_TEST_ABFLAG=1 and TOTK_COOP_TEST_ABARGS=1, in
  coop_game.c): B's copies are not green (game screenshots at 38-40 s, B against A at 36 s).
- A tick trace of an autobuilt fan through the build's confirm (TOTK_COOP_TEST_ABTRACE=SpObj_WindGenerator_A_01,29.9):
  at 30.02 s words in components 14, 57, 75, 80 and 83 go 0.6 -> 1.0 together, a flag word 0x11249 -> 0x10009: the
  preview's see-through look going solid. Nothing there stays green-only. A dump of all components at 38 s in both games
  (TOTK_COOP_TEST_COMPDUMP=38[,<comp>...], with each component's resource name, compname) is mostly pointers; the
  names are the same on both sides (ModelInfo twice, ELink, XLink, ...), and component slots 96, 107, 111, 116, 133,
  144, 194 and 197 were filled on A's fan and empty on B's copy in one run. Next: what those slots are (their vtables
  main+0x3675850, 0x3677088, 0x3677508, 0x3678550, 0x367c398, 0x367d4d0), and the model's material parameters, which
  no dump here reaches.
- A PlayerPuppet cannot take a steering stick: it has no Player component (make_puppet_pack.py), and the Link takes
  the stick by its A button in its own state machine, not by a call. In the stick pair (autobuild_stick_3rd.txt,
  `TOTK_COOP_TEST_ABSTICK=50,1`, game B idle) A's stand-in in B stands 0.7 m behind the copy stick, where A's Link
  holds A's, and the copy stick's on bool stays 0 and B's copies off.
- Hits through the stand-in: A's hit already reaches B's autobuilt copies (the stand-in's swing lands on one, and the
  bonded copies turn on as one; "Autobuild copies" above). For a hand-glued contraption the tester's play log (build  v143play, player 2 glued by hand) shows the first glue (stick to wing) gave a group that player 2's game sent, but
  bonds' log line stops after 10 sends, all before the cannon was glued, so whether the later parts joined is not in
  it, and player 1's log was not there.

## Proxy hair and clothes stay blown after a fan (open)
the tester (2026-11-05): after a fan's wind hits the proxy, its hair and clothes keep flapping as if in wind, on that
screen only, after the fan is off. Likely cause: the proxy runs none of its own update, and something in it sets the
wind its cloth feels; the proxy's swing thaw (40 frames of its own update) ran while the copy fan blew, and nothing
reset it after. Pair test fan_hits_3rd.txt with TOTK_COOP_TEST_FANSET=9 (the copy fan in B blows on A's proxy).
What was looked at, none of it holding the wind:
- Body bones (skeleton +0x20, 127 bones, which hold the hair): standing, the proxy's bones shake 0.2 mm a frame on
  both screens (this Link 0.06 mm), no fast flapping. The flapping is not in those matrices.
- The proxy's actor (0x3000 bytes) and 0x400 bytes of each of its first 128 components, dumped at the hit, 4 s later
  and 8 s after the fan went off: nothing that changed in the wind and stayed, besides positions.
- Havok cloth wind: TthclSimpleWindAction (vtable main+0x35c4888; base wind +0x20, gust +0x40 made by
  main+0x150dcb4 each frame). A search of all committed memory found no object of that class in the scene, so the
  Link's cloth does not use it.
Next: find what moves the hair on screen (a bone set after the pose write, or a shader wind), then what sets its
wind; a write watch (coop_watch_arm with coop_watch_writes) on it would name the setter.

## Arrow copies are not drawn (open)
the tester (2026-11-05): the other player's arrows do not show. The copies are made ("a copy of object N (NormalArrow)
is here") and placed, but sleep. TOTK_COOP_TEST_ARROWDUMP=1 with bow_shoot_3rd.txt in both games dumps this
game's own nocked arrow and a copy side by side; in both games the copy is at actor state 6 (+0x24) where the own
arrow is at 4, its model's +0x244 at 6 (own 5), and +0x28 had bit 0 set (own 0). The actor state machine is
main+0x54cb00 (2..6; 6 sleeps; from 6 it goes back to 4 only with +0x28 bit 0x80000 and low bits 0x10, through
vtable +0xf8 with a pointer to the state, which with 11 deletes).
Tried, all left the copy at 6: running the copy's own calc and phases (not frozen); vtable +0xf8 with 4 each tick;
clearing +0x28 bit 0 as well; setting +0x28 to 0x80010 (it stayed so: the state machine does not run on the copy).
Likely an arrow made on its own, not by a bow, is put to sleep by the arrow's own logic, and only a bow's shot or
nock keeps one awake. Next: find what the bow does to its arrow when it nocks it (the EquipmentUser's arrow, or
the shot call), and do the same to the copy; or show the copy by another actor that does not sleep.

## Map and minimap markers (research, 2026-11-07)
- One screen draws both the map and the HUD minimap: AppMap_00 (UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc).
  Its animation AppMap_00_MiniMap.bflan is the minimap pose: N_Cut_00..03 move to layout (765, -360), N_Cut_00
  shrinks from 2250x1080 to 249x249, Pa_Map_00, Pa_MapIcon_00 and N_IconArea_00 scale to 0.9, and the full map's
  cursor, stamp box and tabs are hidden. Confidence: sure for the files; that the HUD minimap is this pose is likely
  (the set-up below loads "MiniMap" first and no other layout holds a map).
- The map picture is the part Pa_Map_00 (PaMap_00.bflyt). Ground, depths and sky each have an SR/T pane pair
  (N_GroundSR_00/N_GroundT_00, N_UndergroundSR_00/T, N_SkySR_00/T) above 3000x3000 tile pictures, the first at
  (-16500, 13500): 36000x30000 units over Hyrule's 12000x10000 m, so 3 units a metre, y north (world -z). The game
  pans and zooms by those panes, so the T pane's global matrix maps (3 x, -3 z) to the layout. Confidence: likely
  (from the files; not yet checked against the game's own arrow, see the self check below).
- main+0x96c1ac: the AppMap screen's layout set-up (x0 the screen, [screen+0x28] its layout): looks up its
  animations ("MiniMap", "InArashi", ...), the part Pa_Map_00 (made into a 0x27758-byte map object at
  screen+0x208), Pa_Cursor_00, Pa_PlayerNavi_00, N_IconArea_00 (screen+0x2a8), Pa_North_00 (screen+0x2a0). It sits
  in a function table at main+0x372b3f0 that also points at "LayoutMsg/AppMap_00". Confidence: sure (code).
- Layout lookups: main+0x406760 (layout, "Pane" or "Part/Pane") and main+0x406acc (layout, "Part"): a layout's root
  pane is +0x18; its parts are a list at +0x48 (first node [+0x50], next [node+8], name inline at node-0x28, the part's
  layout [node+0x10]). Confidence: sure (code).
- nn::ui2d::Pane in 1.4.3 (type name N2nn4ui2d4PaneE, vtable main+0x357f280): +0x18 parent, +0x20 children list head
  (first [+0x28], next [node+8], child node-8; main+0x6070 FindPaneByName, which does not look inside parts),
  +0x30 translate, +0x3c rotate, +0x48 scale, +0x50 size, +0x58 flags (bit 0 visible), +0x59 alpha, +0x5a global
  alpha, +0x5b base position, +0x70 global matrix (3x4 rows; the constructor main+0x10d6080 sets identity), +0xb0
  name, +0xc9 user data. Confidence: sure (constructor, name setter main+0x402b30, the scale walk main+0x10d66d0).
- What was not used: the game's own pins and stamps (placing one writes the save's map data), and new panes in the
  layout (a RomFS patch, and the icons are made by code). Both need far more of the map object (0x27758 bytes)
  understood than drawing over it with the overlay.
- Pair test (2026-11-07): full map right. On the minimap the ground pane's matrix put this Link at (0, 0) with zoom
  0.252, not at N_Cut_00's (765, -360): the minimap is the map drawn centred and shown as a picture
  (P_MapCapture_00 in N_Cut_01). The markers now go through N_IconArea_00's matrix there, the live pane the game's
  icons sit in (likely; checked by the next run's "off").
- Self check for the transform: TOTK_COOP_TEST_MARKSELF=1 marks this PC's own Link too, and the log says where the
  transform puts it ("this Link ... at (X, Y), off d"): on the minimap the game keeps Link in the middle of N_Cut_00,
  so d near 0 there confirms the 3-units-a-metre mapping.

## The stand-in and enemies' perception (research, 2026-11-07)
- 1.4.3 main+0x1d1fc70 = 1.2.1 main+0x1736dec (TotK Online's "PlayerPuppet is a player" hook; same code). The actor
  from a link (main+0x1afbe4), its component +0x120, its name, looked up through main+0xd62b58 in
  [main+0x39601e8] (1, 0, 1). Its one caller is main+0x1d1f9e8, in the function that loads "IsAdditionalArrowBow".
  It is a bow check, not targeting. Confidence: sure (code).
- PerceptionEmitterParam: the reader main+0x1abe6e0 names SubLayerSensor, IsResidentEmitter, NoiseAndTerrorOnlyEmitter
  and NoEmitUntilConditionsMet (param +0x78.. +0x81). Player: resident, PerceptionPlayer; SageCommon: resident,
  PerceptionSage; EnemyCommon (enemies, horses): not resident, PerceptionEnemy. The sublayer is named only in the
  emitter param, not in the actor's physics. So the emitter makes its own sensor. Confidence: sure (files).
- Reading: a resident emitter is the manager's one player (or Sage) emitter that receivers check directly
  (main+0x45b930 takes the manager +0x58), so a second resident one is never seen. A non-resident one is sensed.
  Confidence: likely; the reader of the flag in code is not found (a scan for loads of +0x80 next to +0x78 found
  none).
- Tested (proxy-plan "Enemies and the stand-in"): with the emitter not resident and DefaultAffiliation Player in the
  stand-in's GameActorInfo row, a Bokoblin 3 m from it shows c882 c c 0 0 4 for about 3 s, a state it never shows for
  this game's Link. Then it fights this game's Link 30 m off (c082 c c 1 1). Either alone: no notice. Controls: the
  Link 3 m in front of a Bokoblin facing it: c082 within 2 s. So the perception half works and the target is
  then taken as "the player", this game's Link. The source of that is not found. main+0x45b930 (the receiver's check
  of the manager's player field) runs about once a run for it, so it is not that.
- GameActorInfo (RSDB) holds DefaultAffiliation for 1356 actors (Player: "Player"; 9 rows have it). A Bokoblin's
  PerceptionReceiverParam: DefaultFilter PerceptionFilterEnemy, IsReceiveFromSensorWorld true,
  IsReceiveResidentEmitter false. Awareness words (component +0x158 +0x24, +0x58, +0x60, +0x64, +0x70, +0x84):
  c182 12 12 0 0 28 unaware; "1 0 5c" / "1 0 f3" spells while it has not seen him; c0a2 c c; c082 c c 1 1 fighting
  (+0x84 the action).
