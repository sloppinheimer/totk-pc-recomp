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
