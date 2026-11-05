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
