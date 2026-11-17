# Engine: actors and the engine around them

TotK 1.4.3. Conventions and marks: [README.md](README.md). Enemy-, Player- and Zonai-specific parts of the same
structures are in [enemies.md](enemies.md), [player.md](player.md) and [zonai.md](zonai.md).

## 1. Actors

### 1.1 The actor object

| Offset | What | Mark, source |
|---|---|---|
| +0x10 | the actor's id, used as a generation check (a new actor at the same address has another) | [read] v4-spec 3.1 |
| +0x18 | a label string; an Autobuild preview device carries `ImaginaryAutoBuildedActor` here, compared by main+0x9e6af0 and main+0x1b37958, and keeps it after it is built | [read], [measured 2026-11-05] re-notes "Zonai devices" |
| +0x24 | state (1.3) | [read] research: zonai/sync-v4/delete-reasons |
| +0x28 | request flags: bit 17 delete accepted, bit 23 create cancelled, bit 25 pool record to be released; the low 11 bits are sleep requests | [read] delete-reasons |
| +0x1a8 | lock object; its +0x50 holds one byte lane per delete reason group (4.1) | [read] delete-reasons |
| +0x1b0 | latch count; an accepted delete waits for it to reach 0 | [read] contraption-game-facts 40 |
| +0x1b8 | the create pool record while the actor is being made (3.3) | [read] research: zonai/sync-v4/create-to-actor |
| +0x1c0 | ring of u16 indices of its update-dependency edges (+0x1c8 capacity, +0x1cc start, +0x1d0 count) | [read] re-notes "Actor dependencies" |
| +0x218 | name (a pointer to the string; "Player" is Link) | [read], [measured 2026-11-03] re-notes "Player actor" |
| +0x228 | component table, `comps` (1.2) | [read] |
| +0x280 | per-actor time rate (hit-stop, slow motion, bow aim) | [read] research: enemy/verify/09 |
| +0x2b4 | position, 3 floats | [measured 2026-11-03] re-notes "Player position" |
| +0x2c0 | rotation, 3x3 floats | [read] |
| +0x2e4 | scale; a Player stays drawn at full size when it is set small every frame | [measured 2026-11-08] character-mods |
| +0x320, +0x32c | linear and angular velocity, written from the body each frame (6.4) | [read] enemy-system "Movement" |
| +0x370 | placement record P (3.4) | [read] |
| +0x378, +0x380 | message channel, and the id messages are addressed to (8) | [read] contraption-game-facts 49.1, 50.6 |
| +0x3fc | calc mask (2.4) | [read] |
| +0x400 | map of named pause masks (2.4) | [read] enemy-system "AI framework" |
| +0x440 | flags: bit 20 persistent (survives a load's delete-all, skips the far test), bit 18 (a delete then leaves the placement marks alone), bit 21 far test on, bits 26 and 29 keep-near; bit 4 (0x10) appears when an Autobuild confirms a device | [read] delete-reasons; [measured 2026-11-08] re-notes "Zonai devices" |
| +0x444 | a deferred delete's reason (4 bits) | [read] delete-reasons |
| +0x448 | frame stamp of the last far test | [read] delete-reasons |
| +0x4b0 | a byte whose low 3 bits must be non-zero before a pause mask applies to a new actor | [read] enemy-system "AI framework" |
| +0x5ac | flags: bit 6 blocked (2.4), bit 10 keep near, bit 2 cleared by an Autobuild confirm | [read] contraption-game-facts 40; delete-reasons |

Every actor class shares one vtable layout; the base vtable is main+0x3633078 [read]. Slots used below: +0xe0 the
delete handler (generic main+0xb2d470), +0xe8 a refused delete, +0xf0 sleep, +0xf8 a state change, +0x238 the
reason passed on to components (generic main+0xd180e4), +0x298 "is this a kill" (generic main+0x1955f90),
+0x2b0 the far test (main+0x265928) [read] research: zonai/sync-v4/delete-reasons.

### 1.2 The component table

`comps = [actor+0x228]`. Each entry is a pointer to a component, or 0 when the actor's pack has none. Offsets seen:

| comps+ | Component | Detail in |
|---|---|---|
| +0x0 | the first component; its +0x10 is the actor blackboard (9) | 9 |
| +0x8 | model holder: `[comps+8]+0x28` is the model object (7.1) | 7 |
| +0x40 | animation: `[[[comps+0x40]+0x18]+0x18]` is the animation graph G (main+0xba8ed0) | 7 |
| +0x50 | physics: `[[comps+0x50]+0x20]` is the controller set (6.1) | 6 |
| +0x68 | link user, used to emit effects that follow the actor | player.md |
| +0x158 | BSA (brain, actions, AI) | enemies.md |
| +0x1a8, +0x1c8 | condition; death controller | elements.md, enemies.md |
| +0x230 | EquipmentUser | player.md |
| +0x2e0 | twelve stop timers | enemies.md |
| +0x2f8 | life (main+0x4f8d78 returns it) | player.md, enemies.md |
| +0x378, +0x380 | perception emitter; perception receiver (main+0x4f8d90) | enemies.md |
| +0x3e8 | ragdoll | enemies.md |
| +0x410 | Rider | player.md, horses-animals.md |
| +0x4a8 | SwordBlur | 7.5 |
| +0x4b0 | target holder | enemies.md |
| +0x518 | world info (main+0x4f8d60 returns it) | 11 |

[read] enemy-system "Big picture"; re-notes "Enemies". `[comps+0x378]` is the perception **emitter** and the
receiver is +0x380 (corrects re-notes "Enemy targets: not found", which called +0x378 the perception component)
[read] main+0x4f8d90 returns +0x380.

### 1.3 Actor states

The state word is actor+0x24 [read] research: zonai/sync-v4/delete-reasons, create-to-actor.

- 0 and 1 while it is made; 2 to 6 alive in the world; 4 is the usual running state; 6 asleep; 7 to 11 going away
  (11 destroyed) [read].
- main+0x54cb00 is the actor manager's per-actor step in its serial phase. It finishes creation (status 3, see 3.3),
  later releases the pool record, moves an actor between its live states, and is the only caller of the step that
  enters state 7 (main+0x552bb4) [read] create-to-actor, delete-reasons. (Reconciles re-notes "Arrow copies are not
  drawn", which saw only its live-state part and called it the state machine.)
- Out of sleep: an actor in state 6 goes back to 4 only with +0x28 bit 19 (0x80000) and the low bits 0x10, through
  vtable +0xf8 [read] re-notes "Arrow copies". Sleep is asked with main+0x55251c(actor, bit), which sets a request
  bit and calls vtable +0xf0; it is not a delete [read] delete-reasons.
- An arrow made by a create request, not shot by a bow, goes to state 6 and stays there; setting +0x28 by hand did
  not wake it, because the step above does not run for it then [measured 2026-11-05] re-notes "Arrow copies".

### 1.4 Finding actors

- **Resident actors.** [main+0x3add238] +0x1e8 is the scene; scene +0x58 is its component list, entry 13 the
  resident actor list. Entries start at +0x28, 0x70 apart; entry +8 is a record whose +0x40 is the actor [read],
  [measured 2026-11-03] re-notes "Player actor". The Player is the entry named "Player"; on the title screen it
  exists at (0, 0, 0) [measured 2026-11-03].
- The resident helpers ThrowBox, CarryBox, Parasail and Dm_Npc_RevivalFairy exist once per game [measured
  2026-11-04] re-notes "Equipment, HUD, camera".
- main+0x1fc9f6c reads the same layout (array +0x28, index +0x20, 0x70 a record) and returns the last entry. Whether
  its seven callers mean "the local Link" depends on the holder each passes [open] research: enemy/verify/00
  section 2. A fast travel's SetPlayerPos (main+0xebb96c) and SearchSafetyPlace (main+0x977a38) also take the last
  entry of the resident list [read] re-notes "Game 2 stuck in a load".
- main+0x4b46a4 asks for the resident "ThrowBox" and uses the answer without a null check (2.3) [read].
- Every live actor passes the per-actor calc main+0x41bb20 each frame, with x0 the actor [read].

### 1.5 Actor links

- An ActorLink is {+8 reference object, +0x10 id}. main+0x1b0614 assigns one (dst, src, flag); main+0x1afbe4 locks
  and gets the actor (out, link) [read] re-notes "Equipment, HUD, camera".
- main+0x155ec fills a link from an actor; it does nothing unless link +0x10 is -1 and byte +0x14 is set first
  [measured 2026-11-09] contraption-sync "Phase 0 results".
- main+0x2804d0 (reference, caller actor, out flag) returns the target only when caller and target are joined by
  update-dependency edges while on a worker thread; otherwise 0 (2.3) [read].

## 2. Frame order and phases

### 2.1 An actor's update slots

The actor vtable's slots run in this order each frame [read] research: enemy/verify/00 section 1:

| Slot | Function | What it runs |
|---|---|---|
| 48 | main+0x2e27b0 | folds the named pause masks into the calc mask |
| 49 (+0x188) | main+0x41bb20, the calc | BSA tick, AI, animation update, life update, death tick |
| 50 | main+0x245030 | the physics result back into the actor (main+0x257310), the model push, the ragdoll read-back |
| 51 | main+0x194fd7c, "phase A" | SensorHit calc, the attacker pick, damage calc, the action runner |
| 52 | main+0x2fb9b0 | runs a deferred delete |

Damage worked out in slot 51 is applied by the life update in the next frame's calc [read]. Slot 50 is the
function earlier notes called "vtable +0x98": that offset came from another vtable base (corrects enemy-system
"Big picture") [read] research: enemy/verify/00. The calc is called from the actor job main+0x2f175c [read]
re-notes "Input". The life update is main+0x4400b0 [read].

### 2.2 The phase byte and the job access check

- [[main+0x395da68]]+0x424 is the actor manager's phase byte: 2 during the parallel actor update, 4 in its serial
  phase (main+0x2ae55e0 -> main+0x54c6c0 -> main+0x54cb00), 0 outside both [read], [measured 2026-11-09]
  contraption-sync "Phase 0 results", create-to-actor.
- In phase 2 a link lookup (main+0x45efd4) resolves only to an actor the calling job may touch: one whose
  per-phase slot (actor+0x30) is empty, or one joined to the job's own actor by dependency edges [read]
  contraption-sync "Phase 0 results". A Player's update and any hook run inside the actors' jobs see only phase 2
  [measured 2026-11-09].
- GameDataMgr::calc (main+0xa72b00) runs once a frame at phase 0, outside the parallel update; so do
  getController and setEventBool [measured 2026-11-09] contraption-sync "The game's own Autobuild". Its only caller
  (main+0xa72abc) sits in a vtable with no static reference, so whether it runs in menus, loads or cutscenes is
  not read [open] contraption-game-facts 50.1.

### 2.3 Update dependencies

- Edges live in [[main+0x395da68]]+0x2f8 (16-byte {parent, child} entries, count +0x2f0); an actor's ring at +0x1c0
  indexes them [read] re-notes "Actor dependencies". main+0x281168 walks parents, main+0x2808f0 children.
- main+0x5b811c(actor system [main+0x3ad6d80], parent, child) queues a new edge (queue at +0x310, capacity +0x318,
  start +0x31c, count +0x320, mutex +0x348); it refuses when either actor is dying or both are the same. The AI node
  OneShotActorRequestSetDependency reaches it [read]. main+0x2ae6cf4 queues the removal [read] research:
  zonai/collision-filter section 5.
- The same edges are the game's owner-to-held relation: equipment is registered under a name `Equip_<owner>_<item>`,
  and the Parasail and the other resident helpers likewise [read] collision-filter section 5. They are not a
  collision filter: no pair test reads them [read].
- A Player has 17 such edges: its weapons, sheath, armour and quiver, Obj_OneTouch_Connection, WakeBoardRope,
  Accessory_SupportDevice and its battery, PlayerRaulHand, Parasail, Dm_Npc_RevivalFairy, CarryBox, ThrowBox
  [measured 2026-11-03] re-notes "Actor dependencies". A second Player made without them crashes the next frame in
  main+0x4b46a4, whose ThrowBox lookup comes back empty under the access rule [measured 2026-11-03].

### 2.4 Calc mask, blocked bit and pause masks

- Every update phase of an actor opens with one gate: the calc mask actor+0x3fc (bits 0 and 1 skip the whole
  update, 0xc4 its components) and the blocked bit actor+0x5ac bit 6 [read] contraption-game-facts 40.
- A skipped actor still gets one full update every 150 (or 30) counts. Messages sent to it meanwhile are taken and
  applied at its next update, off before on [read].
- main+0x2e90e0(actor, name hash, mask) adds a named mask to the map at actor+0x400; slot 48 ORs them into +0x3fc;
  main+0x672540 removes one. Names seen: IsAI 0x80, IsPreCalc 0x4, IsAS 0x40000. Any of 0xc4 stops the BSA tick, the
  AI, the target holder and the animation update [read] enemy-system "AI framework".
- A game menu pauses every actor; single devices also showed a calc mask 0x2e041 that came and went, setter not read
  [measured 2026-11-10] contraption-sync-v3 44.1. What sets the mask from view or distance was not found [open].
- A distance gate inside the calc (main+0x41c670) exists; the LOD "calc skip" AI nodes do not stop the AI [read]
  enemy-system "AI framework".

## 3. Creating an actor

### 3.1 The create request

main+0x599670 is requestCreateActor [read] re-notes (its requestCreateActor section), contraption-game-facts 49.4:

- x0 the actor manager `[[main+0x395e008]]` (the word at main+0x395e008 holds the address of main+0x3ad7450), x1 a
  pointer to the name pointer, x2 the create argument, x3 the handle to fill, w4 1, x5 a placement record (0 for a
  run-time actor). On the stack: a byte, a u32* for an error code, an out pointer.
- Create argument (copied by main+0x59a9f0): +0x00 position, +0x0c rotation 3x3, +0x30 scale, +0x40 a string pointer
  read without a null check (0 crashes), +0x58 a parameter list, +0x68 to +0x78 up to three parent actors, +0x90
  flags (bit 0 position, bit 1 rotation, bit 2 scale given) [read], [measured 2026-11-03].
- It builds `Pack/Actor/<name>.pack`, loads it under the heap tag `ActorMgr` and queues the creation. It returns 1
  when queued; the actor comes later [read]. It fails with a code 1 to 8 (manager closed, empty name, pack failed, a
  table, the heap or its queue full) [read].
- Nothing in the request says why the actor is made [read] 49.4. About 75 functions call it from 108 sites
  [read] research: enemy/verify/00 section 1, contraption-game-facts 34.
- It refuses at once, with no side effect, while bit 1 of the manager's byte +0xe1 is set (a load's delete-all, 4.4).
  With the pack not loaded yet it registers the resource (main+0x2c35f8, once per name) and returns 0 [read] re-notes
  "Game 2 stuck in a load"; research: zonai/sync-v4/load-teardown.
- The AI node ExecuteGenericRequestCreateActor (enter main+0x231fa58) is the game's own generic caller [read].

### 3.2 Callers that matter, by return address

| Return address | Caller | Mark |
|---|---|---|
| main+0x1b7f8c8 | a Zonai capsule turning into its device (Player attachment method main+0x1b7f2d8); its Player is [(x3 - 0x3e88)+0x18] | [read] contraption-game-facts 49.4, 50.2 |
| main+0x1d3bb98 | the Autobuild part maker | [read] |
| main+0x1d1dbd8 | a Shooter making its projectiles | [read] |
| through main+0xaf100c | equipment (weapon create) | [read] player.md |

main+0xa1bc30 is a synchronous create that returns the actor in x0 at once and does not pass main+0x599670 (the
map's create with a request byte, a fish node, the figure creator, effect and rider actors; one caller's name table
not read) [read] research: zonai/sync-v4/create-to-actor.

### 3.3 The handle and the pool record

- main+0x599670 writes only [x3] = R, a 0x30-byte pool record. Status in the low 16 bits of R+0: 2 queued, 3
  created, 4 failed (also 5 to 9 for cancel steps); R+8 the actor. R has no id and is reused at once after it is
  freed [read] create-to-actor.
- The actor is made on a request worker; main+0x3d40f0 returns it and sets actor+0x1b8 = R. main+0x54cb00, in the
  serial phase, sets status 3 and R+8 at its first pass after the actor's init; a later pass frees R and zeroes
  actor+0x1b8 [read].
- Claiming: main+0x1877160(handle, link out, 1, 0) puts a created actor into the world; the link is 16 bytes
  [measured 2026-11-03] re-notes "Spawn test results". The game's own claim (main+0x89fe48 -> main+0x50d6bc)
  zeroes the caller's handle; reading R or the handle after it is unsafe [read].
- Releasing a failed request: the game's create node calls main+0x2ae4d2c(handle), which calls
  main+0x5557e0(record, 0): status 4 or 9 frees it (main+0x5559a0), 2, 3 and 5 to 7 cancel and then free [read]
  re-notes "A failed create request must be released". Records left unreleased run out: after 1491 failures kept
  in their handles every further request was refused [measured 2026-11-08].
- Status 2 to 3 takes a few frames [measured 2026-11-03].

### 3.4 The placement record

P = [actor+0x370], copied from the creation job built by main+0x59c870 [read] enemy-system "Spawn":

- +0x14 bit 3 set for a map actor; +0x18 leads to the actor's ActorInfo row: `[[P+0x18]+0xc8]` (4.3) [read]
  v4-spec 4.3, delete-reasons.
- +0x88 the u64 placement hash, copied from the map entity's `Hash`; all ones for a run-time actor. The same in
  every game on the same map data; the game keys saved flags by it [read] contraption-game-facts 32, research:
  enemy/verify/07.
- +0x98 flags: bit 7 kill mark, bit 9 destroyed mark, bit 11 far, bit 29 turns a far delete into reason 5, bit 5
  skipped by the reset delete [read] delete-reasons. +0xa4 the banc slot (-1 at run time) [read].
- Nothing tells an actor a player made from one the game spawned at run time [read] contraption-game-facts 32.
- Placed enemies are made at the same spot in every game on a save, to 0.1 m [measured 2026-11-04] re-notes
  "Enemies".

## 4. Deleting and unloading

### 4.1 The delete request

- main+0x63baa8(actor, reason) is the request; main+0x63b6b0 is a wrapper that may defer it, keeping the reason in
  actor+0x444 [read] research: zonai/sync-v4/delete-reasons. Every actor removal found passes main+0x63baa8 [read].
- It answers 0 when the actor is already going (state 7 to 11), when a delete was already accepted (+0x28 bit 17),
  or when a lane of the actor's lock object is held for reasons 0, 3, 4, 6 and 8 (lanes by a shift table at
  main+0x2d88b98); then it calls vtable +0xe8 [read]. A second ask after an accepted one answers 0 [read]
  contraption-game-facts 40.
- On acceptance it sets bit 17 under the manager's mutex, queues the actor and calls the delete handler [read]. An
  accepted delete then waits, with no time limit, for lane 0 to be free and the latch (+0x1b0) to be 0 [read] 40.
- The request reads the actor's lock object first; that object is dropped once the actor passes state 7, so a
  request on a stale actor pointer can fault [read] contraption-game-facts 32.
- The generic handler marks the placement record: bit 9 for every reason but 3 and 4, bit 7 when vtable +0x298
  says "kill" (all reasons but 10 and, mostly, 3); reason 3 sets the far bit and clears bit 9 [read]. So bit 7 does
  not tell a destroy from an unload.

### 4.2 Reasons

[read] research: zonai/sync-v4/delete-reasons (later and fuller than contraption-game-facts 49.5):

| Reason | Meaning | Class |
|---|---|---|
| 0 | destroy: every death procedure, the AI node OneShotGenericDeleteThisActor, lapse and broken-off fallbacks, the Autobuild preview drop | destroy |
| 1, 2 | the create failed; the creation ticket was cancelled (2 also sets +0x28 bit 23) | creation bookkeeping |
| 3 | far delete: the per-actor far test, the AI node OneShotGenericFarDeleteThisActor, holders passing on a far delete | unload |
| 4 | engine-wide reset: scene-sequence delete-all nodes (4.4), "reset gimmick" | reset |
| 5 | far variant: the far test gives 5 when P+0x98 bit 29 is set; one AI node (main+0x2519c2c) asks it | unload |
| 6 | the actor's map unit unloaded (map actors only) | unload |
| 7, 10 | far variants, not re-read | unload [inferred] |
| 8, 9 | a holder or reference deleting the actor it owns | destroy [inferred] |

- The AI node OneShotGenericDeleteThisActor passes 0, not 3 (corrects re-notes "Leaving and time of day") [read].
- Reason 5 is a far delete marked by a placement flag or asked by one node; the generic far-delete node passes 3
  (settles the difference between contraption-game-facts 49.5 and the delete-reasons note) [read].
- A creation refused at the first placement check deletes with reason 0 from main+0x54cb00 (return main+0x54db94)
  [read].
- A delete cannot be classed by return address: of the return addresses once listed for destroys, most cannot occur.
  The deferred runner's deletes return to main+0x2fc560 and main+0x2fd080, a death through the wrapper to
  main+0x63b6f4 or main+0x63b830 [read] v4-spec 18. Class by reason.
- A placed enemy deleted by request is made again by the map at once; one whose life is set to 0 dies by the game's
  own death and stays dead [measured 2026-11-04] re-notes "Enemies".

### 4.3 The far unload

- Nothing sweeps. Each actor tests itself every frame in its own lifetime function (main+0x2f48a0; copies in
  main+0x245030, main+0x9550f0, main+0x634680, main+0x2afb464) and asks its own delete with reason 3 when out of
  range [read] contraption-game-facts 41, delete-reasons section 5.
- Radius: the ActorInfo row R = [[P+0x18]+0xc8], DisplayRadius (R+0xac) plus DeleteRadiusOffset (R+0xa8). With either
  negative or NaN the actor is never far. A scale term (R+0x88, ApplyScaleType) and a per-class bonus apply. The
  metric comes from R+0xb0 (DisplayRadiusDistanceType: XZ only, Manhattan, Chebyshev or Euclidean) [read].
- The distance is from the actor's position to observer points in the actor manager V = [[main+0x395e008]]: six
  pairs at V+0x10, the mask V+0xa0 picks one of each pair; pair 0 at the radius, the others at radius + 40 m. One
  pair in range keeps the actor. Who writes the observer points was not found [open].
- V+0xd0 non-zero turns the far test off for every actor (written by AI node main+0x1fda5b8) [read].
- No exemption for a held, ridden or glued actor was found. The per-actor exemptions: +0x440 bit 20 set, bit 21
  clear, or bit 29 set without bit 26 together with +0x5ac bit 10; a non-zero +0x1b8; a holder holding lane 3 [read].
  The radii of Zonai parts are in [zonai.md](zonai.md).
- The far deletes arrive through main+0x63b6b0 from main+0x249174 and main+0x636994, asked again every frame while
  deferred [measured 2026-11-09] contraption-sync-v3 23. A plank teleported 150, 300, 600 and 1200 m away was not
  deleted in 35 s: moving the actor does not unload it; why the test held is not read [measured 2026-11-09].
- An actor made at run time (from the pouch, by Autobuild, glued by hand) is never made again after it unloads: no
  code was found that does it, and no save record of placed parts [read, absence]. Map actors come back from their
  placement record [read]. At the run-time pool's limit a new actor is simply not made; no oldest-first eviction
  was found [read] contraption-game-facts 41.

### 4.4 A load's teardown

- A travel that changes the area runs the Sequence graphs' ChangeBanc steps (SleepResidentActor, UnloadBanc,
  LoadBanc, AfterChangeBanc) [read] re-notes "Game 2 stuck in a load". Which loads run it (fast travel, save load,
  shrine, title) is not read [open].
- Its node SeqExecuteDeleteAllActor (vtable main+0x36dc240; enter main+0x1fd9750, update main+0xea8aec) sets bit 1
  of the actor manager's byte +0xe1 on entry: creates are refused from then. Only SeqOneShotAfterDeleteAllActor
  (main+0x1fe7824) clears it [read] research: zonai/sync-v4/load-teardown.
- Each frame the update asks every actor of state 2 or more that is not going and not persistent (+0x440 bit 20)
  to delete with reason 4 (return main+0x108c8c8); a refused delete is asked again the next frame [read].
- It ends when the resource system [[main+0x3ad74b8]+8] has no unfinished job (and its +0x600 bit 5 is set), the
  actor system's delete lists, going-away list (+0x398, drained each frame by main+0x6ecce4) and creation counters
  ([+0x430]+0xe0) are empty, and every remaining actor is persistent [read]. The delete lists were read at
  +0x3c0/+0x3d8/+0x3f0 in one note and +0x3d0/+0x3e8/+0x400 in another; list head against count is likely
  [open].
- Two other signals change over the same span: the load object's +0x600 bit 2 and a nibble at
  [[main+0x3ad74a8]]+0x11c [read] load-teardown.
- Map actors whose unit unloads go with reason 6 [read]. A fast travel has no "world unloaded" call for run-time
  parts: the observers jump and each fails its own range test [read] contraption-game-facts 41.

## 5. Death requests

- main+0x1953184(actor, kind, 0) is the death request; it leads to RequestDeath main+0x1b4b738(D, cause, second,
  flag), where D is the death controller `[comps+0x1c8]` (state word +0x20). RequestDeath only starts a death; the
  death tick main+0x1b4badc, run from the calc, carries it through its phases [read] research: enemy/verify/06.
- Kinds by number: 0 AI, 1 AttachmentUseEffect, 2 AncientWeapon, 3 ChemCritFire, 4 ChemCritIce, 5 Drowned, 6 Eaten,
  7 PickedUp, 8 Exploded, 9 Burned, 10 Frozen, 11 Electrified, 12 Unequiped, 13 FallIntoAbyss, 14 Boiled,
  15 PhysKinematicColl, 16 LifeDepleted, 17 LifeCondition, 18 Melt, 19 Crushed, 20 ImaginaryAutoBuildedActorDeath
  [read] research: zonai/sync-v4/delete-reasons section 3. Each kind has a record (procedure, delay) per actor class.
- The kind asked is rewritten before it is stored (15 to 16; 16 to 9, 10 or 11; several to 20 when D+0x2c bits 5
  and 9 are set) [read] contraption-game-facts 32. A replay of a death must use the stored kind.
- D+0x2c holds switches: bit 0 no drops, bit 1 no kill flag, bit 7 refuses the request [read] research:
  enemy/verify/06. More on enemy deaths: [enemies.md](enemies.md).
- Every death ends in a delete with reason 0: inside the request when the record's procedure is 2 (delete) or 3
  (fade out, through the wrapper) and the actor is not in state 4 (return main+0x1b4b8f4, death word not written);
  otherwise from the death tick (returns main+0x1b4be78, main+0x1b4bf24) [read] delete-reasons.
- A death breaks no bond and leaves no group by itself; only the delete's teardown does, in a later physics flush
  [read] contraption-game-facts 32.
- Death kind 15 comes from the actor update main+0x27b300 when a kinematic body presses the actor [measured
  2026-11-09] contraption-sync-v3 24.

## 6. Physics bodies

The engine is Havok hknp, wrapped by the game's phive layer [read] research: zonai/physics.

### 6.1 Where an actor's bodies are

- The controller set Ctl = `[[[actor+0x228]+0x50]+0x20]`. Its main body is [Ctl+0x150] (main+0x2b08920 reads this
  path) [read] research: zonai/player-physics.
- Ctl holds bodies in several places: the main body; a named holder list ([Ctl+0x10]: count +0x10, array +0x18);
  entity body sets (count Ctl+0x18, array Ctl+0x20); sensor body sets (count Ctl+0x38, array Ctl+0x40, each set
  record starting with a sensor type 0 to 8: Tgt, Atk, Perception, GeneralSensor, Chemical, InDoor, AreaCheck,
  CameraCheck, Bond); an extra holder at Ctl+0x160; a provider at Ctl+0x128 that adds sets of a parent. The
  game's own walk over all of them is main+0x3cc9b4 with main+0x2962c58 [read]. [Ctl+0x158] is not a body [read].
- main+0x44bba4(Ctl, key) returns a body by small key; key 0 is the main body [read]. The part bodies' keys are
  in [zonai.md](zonai.md).
- The character controller, when there is one, is P = [[Ctl+0x10]+8]; P+0x18 its body (the main body for a
  Player), P+0x20 an object that takes the position at +0x1f8 [read]. A Player's bodies are in
  [player.md](player.md).

### 6.2 The body object

| Offset | What | Mark |
|---|---|---|
| +0x28 | an owner object the pair filter may consult (6.6) | [read] collision-filter |
| +0x60 | the request block (6.3); +0x80 a pending motion type while +0xd4 bit 2 is set | [read] contraption-sync-v3 2 |
| +0x68 | flag word (below) | [read] |
| +0x70 | 0 once the body has left the physics world (the best supported test) | [inferred] contraption-game-facts 32 |
| +0x98 | last transform, 3x4 rows | [read] proxy-plan "What is proven" |
| +0xf8 | linear velocity | [measured 2026-11-09] contraption-sync-v3 23 |
| +0x140, +0x148 | collision filter record holder, in world 0 or world 1 (6.6) | [read] |
| +0x17c | index of the speed limit class (default 5000 m/s) | [read] research: zonai/physics |
| +0x1e8 | bits 1 to 4 one-hot for body keys 0 to 3 (written by main+0xab69fc) | [read] zonai.md |

Flag word body+0x68 [read unless marked] research: zonai/physics, collision-filter; contraption-sync-v3 47.4:

- bit 5: the body lives in the second physics world.
- bits 6 and 7: the applied motion type (6.3).
- bit 8 or 29: the collision-id setter leaves the body alone.
- bit 10: must be set, with bit 26 clear, on both bodies for a glue to be kept (zonai.md).
- bit 11: collision off; the body then collides with nothing.
- bit 24: a mark left by a teardown that nothing clears; a bond on such a body is dropped, and the connection graph
  leaves it out.
- bit 25, then 26: an applied transform request sets 25; the post-step update copies it to 26, a one-step
  "teleported" latch.
- bits 30 and 31: the linear and the angular velocity setter do nothing.
- bit 41: a physics-side edge removal (zonai.md).
- bits 44 and 45: the flush, applying a velocity, flags the request so that the next flush writes zero velocity.
  Not reproduced: a resting plank given a velocity once with bit 44 set behaved as without it [open, C14]
  contraption-sync-v3 0.3 against 23 (X1).

### 6.3 Writing a body

- Setters queue into the request block, never act at once: SetTransform main+0x25a450 (body, m[12]), SetLinearVelocity
  main+0x567750 (body, v[3]), SetAngularVelocity main+0x25a1f0 (body, w[3]), requestChangeMotionType main+0xae4300
  (body, type). Block fields: +0x08 matrix, +0x44 linear, +0x50 angular; flags +0xd4: 0x4 motion type, 0x40 matrix,
  0x80 linear, 0x100 angular. Getters read the pending block first [read] research: zonai/physics.
- The flush before the Havok step (main+0x182410 -> main+0x182eb0) applies the block [read]. A velocity written
  lands one frame later: a resting plank given 3 m/s up read 2.52 m/s the next frame, then lost 0.48 m/s a frame
  [measured 2026-11-09] contraption-sync-v3 23.
- SetLinearVelocity ignores a value within 1e-4 of the current one unless bit 44 is set, and clamps the speed to
  the body's class limit [read].
- A teleport gives the body velocity 0, so nothing resting on it is carried (6.5) [read].
- **Motion types: 0 static, 1 keyframed, 2 dynamic.** main+0xae4300's apply sets body+0x68 bit 6 for 0 and bit 7
  for 1; neither bit means 2 [read]. (Corrects contraption-sync's early sections, which called 2 "keyframed"; the
  research note zonai/physics names 1 and 2 the other way as a guess from Havok's naming, and the runs below agree
  with this reading.)
  - Impulses, thrust and the stick's force act only on type 2 bodies [read] contraption-sync-v3 2.
  - An actor whose body was made type 1 sank 3.3 m through the floor [measured by 2026-11-15, run 6] enemy-sync-design 5.
  - A type asked once did not stay; asked every frame it held [measured 2026-11-09] contraption-sync "Phase 0".
- The character controller's result pass (main+0x240440) rewrites its body's next velocity every physics step,
  whether or not the actor updates; SetLinearVelocity on a controller body is overwritten [read] research:
  enemy/verify/00 "Movement".
- The controller's vtable: +0x48 direction, +0x68 speed, +0x70 a velocity split into direction and speed, +0xc0
  position and rotation [read] enemy-system "Movement". Its data holds forward direction (+0x1a8) and speed
  (+0x1c0). The move state (ground, air, fly) comes from contacts; ChangeMoveState is main+0x298f7a8 [read].
- ApplyLinearImpulse is main+0x29ae114 [read] research: zonai/physics section 7.

### 6.4 Placing an actor

- ForceSetMatrix main+0x45ce70(actor, m[12] rows of rotation and position, 0) teleports an actor. It goes through
  the controller set: sets flags at +0x188, then queues a transform request on the main body (main+0x25a450) or
  takes a vtable +0xc0 path. A NaN matrix is dropped; velocity is not touched [read] re-notes "Frame tracer";
  research: enemy/verify/00.
- The move is applied inside the actor's update: an actor whose calc and slot 50 do not run is not moved by it
  [measured 2026-11-05] re-notes "Puppet spike".
- main+0x257310(actor), from slot 50, copies the main body's transform into actor +0x2b4/+0x2c0 (only where they
  differ), the velocities into +0x320/+0x32c, and the model's world matrix. A pose jump becomes a velocity at
  +0x320 [read]. Skipping it for an actor makes its skeleton jump up to 2.8 m between frames: it does more than the
  copy [measured 2026-11-05] proxy-plan "What is proven".
- So writing actor+0x2b4 alone moves nothing for long; place an actor through its body or ForceSetMatrix [read].
- AI movement never writes the position: nodes push a direction and speed into the controller [read].

### 6.5 Carrying a character

- A character is carried by the velocity of what it stands on, not by its transform. The controller update
  main+0x298c9f8 is run by the physics system for every controller (directly or as jobs), not from the actor's
  update; it calls each extension in the controller's list [read] research: zonai/physics sections 3 and 7.
- The extension CharacterUpdateAttachGround (main+0x1f554f8) casts a ray 4 m down from 1 m above the character and,
  on a body, adds that body's velocity at the hit point (main+0x29a9398) to the character's move. It stays armed 5 s
  after the last hit [read].
- CharacterUpdatePushObject (main+0x1f591b8) pushes what the character meets with ApplyLinearImpulse and
  SetAngularVelocity; these are not body contacts, so the pair filter does not see them [read].
- The character state MovingPlatformConstraint exists by name, but its factory (main+0x1f42d04) returns 0: unused
  [read].
- What a character stands on is in no field; it can be read from the controller's contact list, or at the ground
  ray's call (main+0x29a9398 with return address main+0x1f556c4) [read] contraption-game-facts 41.
- A plank moved by velocity carried Link with 0.01 to 0.1 m of drift at 3 m/s over 4 s; the same plank teleported
  each frame left him behind at once (12 m by the end) [measured 2026-11-08] contraption-sync "Phase 0 results".
- Contact mass: a per-contact pair of mass factors applies only between two dynamic bodies and, for a Link, only
  when it pushes (factor 0.1, as 1 kg); standing, a Link weighs its real 10 kg on what it stands on [read]
  contraption-sync-v3 45.1.

### 6.6 Collision filtering

[read] research: zonai/collision-filter unless marked.

- A body's 0x30-byte filter record is at holder+8, where the holder is [body+0x140] in world 0 or [body+0x148] in
  world 1 (body+0x68 bit 5). Each world has its own filter object (corrects contraption-sync-v3 26, which took
  +0x148 for another class) [read]; the world-0 path was confirmed in a run [measured 2026-11-09] contraption-sync-v3 26.
- The pair test main+0x40a240 compares, in order: a tag (record +0x2a; two different non-zero tags never collide);
  the group word and ignore bitset (same non-zero group: the ignore bit of the other's index decides); a collision
  id with a nibble; the layer matrix and both layer masks; the sub-layer masks. Layers are taken modulo 32 for the
  mask tests [read].
- World 0's single-pair path first tests the two blockable masks; the batch path does not [read]. After the pair
  test, an owner object at body+0x28 may refuse by its own layer masks; its class is not identified [open].
- The group word and ignore bits are written by main+0xacf904 and main+0xacfa10, called by the connection-graph job
  main+0x181e40 for groups it is told to process: the bonded parts of one contraption ignore each other this way
  [read]. Always on the world-0 holder [read].
- The collision id is set per body (main+0x953eac) or on every body of a physics component at once
  (main+0x953c68); ids are counted in a pool [read].
- Layers seen: 3 Player; 2, 8, 0xa and 0xb ground kinds (GroundObject, GroundHighRes, GroundHighResRough,
  GroundStaticTree) [read] research: zonai/physics section 9.
- A Player's sword, shield and bow are actors of their own with one solid body each [read] contraption-sync-v3 29.2.

## 7. Model, skeleton and animation

### 7.1 The model object

- The model is `[[comps+8]+0x28]`. World matrix at +0x1f8 (3x4 rows, absolute position), scale +0x228, dirty bit
  +0x240 bit 0 [read], [measured 2026-11-05] re-notes "Puppet spike".
- main+0x24cc30, called in slot 50, pushes the matrix to the model's units only when the dirty bit is set. A unit
  with no parent bone takes +0x1f8; a unit hung on a bone takes that bone's final matrix [read], [measured
  2026-11-05] re-notes "Proxy mode, stage 1". Writing +0x1f8 without the dirty bit moves nothing [measured].
- main+0x25cf88 and main+0x269f70 read the model's matrix to place attached items [read].
- The model's animation slots are at model+0x40 (count +0x38, 0xb0 bytes a slot: clip u16 at +2, weight +0x10,
  frame +0x28) [read] proxy-plan "the stand-in as TotK Online's".
- ModelObj::Initialize is main+0x1f38c8; SkeletonObj::SetupBlockBuffer main+0x1f378c [read] re-notes "Puppet spike".
- The Player's model has one unit, its 127-bone body; model+0x110 is the SkeletonObj itself [read], [measured
  2026-11-05] re-notes "Proxy mode, head and hair".

### 7.2 The skeleton

- SkeletonObj: +0x00 ResSkeleton; +0x18 parent-local records, 0x60 a bone (scale +0x10, the rotation's columns at
  +0x20/+0x30/+0x40, translation +0x50); +0x20 final matrices, 0x40 a bone (rows 0 to 2 rotation of a row-vector
  matrix, row 3 translation); +0x48 u16 bone count [read], [measured 2026-11-05] re-notes "Proxy mode, stage 1".
  (Corrects re-notes "Puppet spike", which took +0x18 for 3x4 world matrices.)
- The final matrices are in world space from an origin of the game's own, metres off the actor's position by
  different amounts in two games: only bone-to-bone differences compare across games [measured 2026-11-05].
- The records are turned into the final matrices every frame by something outside the actor's calc and slot 50
  (the body top changed in 598 of 600 frames of an actor whose update was stopped) [measured 2026-11-05].
- Head and hair are bones of the body skeleton, not another skeleton (corrects proxy-plan "What is proven"). The
  95- and 113-bone skeletons near a Player hang off actor +0x140 and +0x178 and belong to other actors, one of them
  shared [measured 2026-11-05] re-notes "Proxy mode, head and hair".
- A Player's body skeleton path: actor +0x48, +0x2a8, +0x20, +0x20, +0, +0xf8, +0x58, +0xb0 [measured 2026-11-05].

### 7.3 Writing a pose

- main+0x283c750(x0 the input pose, x1 the SkeletonObj) builds the body's parent-local records after the animation
  step. Input pose: u16 bone count at +0xa; 0x50 bytes a bone (+0x00 scale, +0x10 translation, +0x20 and +0x30 the
  first two rotation rows) [read], [measured 2026-11-05] re-notes "Puppet spike".
- Other writers of a Player's body matrices during its update: main+0x2e9c80 (one per animation binding, vtable
  main+0x3927130; Havok pose to the model, runs even while the actor's calc is stopped), main+0x289620 (pose to
  parent-local records), main+0x2925e0, main+0x43e450, and the calc [measured 2026-11-05].
- A pose written after main+0x283c750 for that skeleton is what shows [measured 2026-11-05]. The input pose lacks
  later steps (IK): 30 bones differ between input and records even standing, and the final matrices carry more
  (feet on stairs, a hand on a wall) [measured 2026-11-05].
- The ragdoll's bodies are read back into the pose in the model phase (main+0x2752c8 inside main+0x245030) [read]
  enemy-sync-spec B.7.

### 7.4 Animation

- The animation graph is G = `[[[comps+0x40]+0x18]+0x18]` (main+0xba8ed0). Its player Q = [G+0x28]; Q's vtable +0x30,
  main+0x1b569fc, sets a model slot's clip, weight and absolute frame [read] proxy-plan, research: enemy/verify/09.
- **Starting an animation (AS).** Every start ends in main+0x4dca24; main+0x4dc628, which graph nodes and native
  modules call, is one of its 23 callers (the others not read) [read] enemy-sync-spec 7.1. (Refines enemy-sync-design
  6.2, which named main+0x4dc628 as the one path.)
- The Player's request to play an AS by name is main+0x4dcc1c (x0 the AS manager, x1 the name as a string object,
  x2 a request: +0x28 slot, +0x2c an int it may overwrite from manager +0x148, +0x32 a flag byte, +0x34 blend time,
  negative for the manager's default at +0x11c). It calls main+0x4dd2a4 and then, unless the slot's entry blocks it,
  main+0x4e1bdc on that slot [read] re-notes "Animation by name". The Player's AS manager is at
  `[[comps+8]+0x378]+0x18`, slots at manager +0x20, 0x48 a slot [measured 2026-11-05].
- Player slots: 0 body, 1 upper body, 2 and 3 weapon layers, 4 face [measured 2026-11-05]. A request sent to
  another Player's manager returned 1 but no pose showed; whether the request was incomplete or the Player's own
  state machine replaced it is not known [open].
- Clip names are compared in main+0x5baeb4, main+0x5bad18, main+0xe24484 and main+0x2bb7460; AS names in
  main+0x4dcc1c, main+0x4dd2a4, main+0x4e1bdc and main+0xac87e4 [measured 2026-11-05].
- **Slot state.** S = G+0x20+i*0x48: rate S+0xd0, blend controllers from *(S+0x50)+0x28, current index +0x95; a
  controller context's clock +0x68 and weight +0x6c [read] enemy-sync-design 6.2. AS_update is main+0x432fa0
  (from the calc, gated by the pause mask); it turns commands into clips and overwrites the model's slots every
  call (main+0x441970). Animation events (attack windows, sounds, trails) are applied inside it [read] research:
  enemy/verify/09.
- The clock counts 30ths of a second (0.5 a frame at 60 frames) [measured by 2026-11-15, run 8] enemy-sync-design 6.3. A clock
  written with the "do not advance this update" flag (context +0xa0 bit 10, seen in main+0x433c24) reads the
  written value after that frame's update and counts on from there [measured].
- Each slot holds a random value (S+0x1e4) that is a new number in [0, 1) every frame; what reads it is not known
  [measured by 2026-11-15, run 8] enemy-sync-design 6.4.
- Time: actor+0x280 scales the actor's animation time; times are also at [comps+0x40]+0x98 [read] research:
  enemy/verify/09.
- **Body masks.** Each model slot holds its mask (bind) at +0x08. A bind sits at +0xc0 of a layer object the graph
  points to: [G+0x1a8] whole body, [G+0x1f0], [G+0x2c8] upper body, seen on an idle, walking and bow-drawing Link
  [measured 2026-11-07] proxy-plan. A mask's state: u16 +0x1c, byte flags at [+0x10] (count +8), word flags at
  [+0x28] (count +0x20), a change count +0x18 that makes the play bind it again [measured 2026-11-07]. Two actors
  with the same layer objects may set different flags in them (a Link's whole-body mask and a stand-in's
  differed) [measured 2026-11-07].
- The animation blackboard is *(G+0x40), separate from the actor blackboard (9) [read] enemy-sync-design 6.2.

### 7.5 Animation events and effects

- ASB event kind names are listed at main+0x3713400 (AtCollision, Noise, AwarenessScale and others). Kind 17 is
  WeaponBlur (game::WeaponBlurType names the hand) [read] proxy-plan "the stand-in's sword trail".
- The SwordBlur component ([comps+0x4a8], vtable main+0x367e028) runs main+0x275954 every frame: it looks through the
  animation graph's layer nodes for an active WeaponBlur event and, while one is on, emits the weapon's
  `Blur_Default` effect on the weapon in that hand; effect handle at +0xc8, the event at +0xe0, +0x2a is 1 while the
  effect lives (about 0.17 s a swing) [read], [measured 2026-11-07]. So a sword trail comes from the animation
  sequence's nodes; an actor whose clips are played straight onto its model slots never raises it.
- Effect handles are an effect pool index and serial (group bases [main+0x395f388], sizes [main+0x395f390], serial
  at +0x20): meaningless in another game [read].
- An emit through main+0x4f4e40 (x0 the link user [comps+0x68], x1 a name, x2 2) returns two handles; an arrow's
  `ArrowTail` is emitted this way once when its flight starts [read] proxy-plan "arrow trails" (player.md).
- Havok cloth wind: the class TthclSimpleWindAction (vtable main+0x35c4888) has no instance in the scene, so a
  Player's cloth does not use it; what moves hair and clothes in wind was not found [measured 2026-11-05] re-notes
  "Proxy hair and clothes stay blown" [open].

## 8. Messages between actors

- main+0x2b76c34(channel, {word 0, id, flag}) queues a message. The channel is the sender's actor+0x378; word 0 is
  the receiver's id (its actor+0x380); the drain delivers the sender's id in word 0 [read] contraption-game-facts
  50.6, 49.1.
- All queued messages go into one ring, drained by main+0x2b772b0 in push order, in one pass, under the ring's
  lock [read] contraption-game-facts 35.
- The ring holds 100 messages unless a parameter block says otherwise. A full ring drops a message silently [read]
  35, 41.
- Messages to a skipped actor are applied at its next update, off before on (2.4) [read].
- Message ids used by the game appear in [zonai.md](zonai.md) (hold end 0x20510006, device set 0x6b51000N, refusal
  feedback 0x3dc6000N).

## 9. Blackboards

- The actor blackboard is bb = `[[comps+0]+0x10]` [read] contraption-game-facts 34; enemy-system.
- Bool section at bb+0x198: values bb+0x1a0, hash table bb+0x1c8, table size bb+0x1d4. Float section at bb+0x120:
  values bb+0x128, table bb+0x150, size bb+0x15c. A slot is hash modulo size holding {u32 name hash, u32 index},
  probed linearly, hash 0 ends [read].
- Keys are murmur3_32 of the name with seed 0 [read].
- The game's bool write raises a change counter (bb+0x1f0; a second at bb+0x1e8) and does nothing while the lock
  byte bb+0x1e0 is set [read] contraption-game-facts 34; enemy-sync-spec B.2.
- Setters: main+0x740b68 writes a bool for an actor; main+0x9fb82c writes one through a node's handle (x0 with
  [[[x0+8]+8]+0x200] the table and +0xc4 the index, w1 the value); main+0xc3e260 writes a float [read] re-notes
  "Zonai devices", contraption-game-facts 40.1.
- A value a node sets each frame is overwritten by that node; setting it from outside sticks only if the node
  stops writing it [read] contraption-game-facts 34.
- A graph's memory lives here (for example a Wizzrobe's IsMagicWeather); key lists per family are in
  [enemies.md](enemies.md), device keys in [zonai.md](zonai.md).

## 10. Game data and progress

- The game data manager is `[[main+0x395dff0]]` (the same object as [main+0x3ad66b0]) [read] re-notes "A co-op Link
  must not change this game". GameDataMgr::calc is main+0xa72b00 (2.2).
- **Setters** that take a value and return nothing: main+0x170210, main+0x16fdcc, main+0x170490, main+0x282280,
  main+0x2aabf84, main+0x16ee70, main+0x31d824, and main+0x923d20 (game data, 1, &key, value), which sets an enum and
  has 458 callers [read], [measured 2026-11-04]. A Player's update calls several every frame (from main+0x281d28,
  main+0x281d40, main+0x282a0c, main+0x2b04c8, main+0x2b0568) [measured 2026-11-04].
- A Player's calc writes key 0x57f52eb8 (from main+0x207eba0, values from the table "AttackByWeapon" and others): the
  record of the player's last action, from which instant tips show [measured 2026-11-04] re-notes "Instant tips".
- main+0x3d01a0 reads a bool by hash (for example IsGet.Obj_AutoBuilder, 0x5d930d12) [read] contraption-sync-v3 23.
- **setEventBool** is main+0xc6a9f0 (1.2.1 main+0x11616bc: its caller's caller looks up the same hash 0x2b05cbb9).
  It writes its table directly, not through a queue [read] proxy-plan "Shared world progress".
- **Queues** read in calc [read] proxy-plan "Shared world progress":
  - bools: manager +0x30 capacity, +0x38 entries {u8 value, u32 hash}, +0x40 a word whose low 20 bits count them;
  - Bool64bitKey flags: +0xc68, +0xc70 entries {u8 value, i32 index}, +0xc78; the keys at +0xc60, 0x10 bytes each
    {u64 key, u32 reset type, u8 value at +0xc};
  - value type t's block at manager + t*0x60 (2 Int, 4 Float, 6 Enum): entries 0x28, 0x28 and 0x38 bytes with the
    value at +0x1c (checked in calc's compare, not for every entry) [inferred]; queues of {value, hash, op}, op 1
    adds;
  - String64Array entries of 0x58 bytes with their own text buffer; an IntArray queue {value, index, hash, op}.
- GameDataMgr::calc is the same code at the same offsets as 1.2.1's main+0xee9704 [read].
- **What is saved where**: GameDataList.Product.140 gives each entry a SaveFileIndex: 0 the progress save, 1 the
  options save, -1 never saved [read]. Reset types group entries for resets (the blood moon is type 3; see
  [enemies.md](enemies.md)).
- The Player's own entries are named PlayerStatus.* and families named by actor (EquipTime.*,
  EquipmentPouchTime.*, EquipmentDeathCount.*, RecipeCard.*, PictureBookData.*, OwnedParasailPattern.*) [read].
  Shrine states are Enums DungeonState.DungeonNNN [read].
- Loading a save writes its values through the queues after the Player appears and before the world clock starts:
  1300 to 1500 quest steps, challenge states and counts in one test [measured 2026-11-07] proxy-plan.
- Key 0x90b81999, by slot, is this game's player's item in an equipment slot; held-item placement reads it
  (player.md) [read] proxy-plan.

## 11. World

### 11.1 Time of day

- The world manager is [main+0x3a83968]; its +0x98 is the time object: +0x28 the second of the day now (float,
  0 to 86400), +0x48 the last one asked for [read], [measured 2026-11-04] re-notes "Leaving and time of day".
- main+0x21cd794(world manager, s0 = second of the day) asks for a time, as the AI node ExecuteWmSetTime does (hour
  * 3600 + minute * 60, then main+0xb6bff8(time object, 1, -1)); the game applies it later. One request moved a
  game from 6:43 AM to 9:55 AM [measured 2026-11-04].

### 11.2 Weather

- The weather manager is [[main+0x395dc20]]. main+0x21cd370(manager) & 1 says it is ready; the weather at an actor
  is main+0x1c2fd1c(main+0x4f8d60(actor)), as the AI query QueryWorldInfoCheckCurrentWeather does [read] re-notes
  "Weather".
- main+0x21d0798(manager, type, flag) forces a weather, as the flowchart node OneShotWmSetWeatherForFlowchart does:
  it writes a request block at +0x940 (a buffer at +0xd20, index at +0x2380, type at +0xcd8). main+0x21d0c18(manager)
  goes back to the forecast [read], [measured 2026-11-04].
- Types seen: 0 clear, 2 rain, 4 snow (the HUD shows a raindrop or snowflake) [measured 2026-11-04].
- Lightning strikes are in [elements.md](elements.md).

### 11.3 Rotating ground

- A turn helper (main+0x298a20c) holds +0x8 top speed, +0xc acceleration, +0x10 current speed, +0x14 the angle in
  radians kept in 0 to 2 pi, +0x18 the ground control's index [read] proxy-plan "rotating ground".
- Two nodes drive it: the actor's logic node ActorLogicGroundControlRotate (enter main+0x1cf0160, update
  main+0x1cf0260; helper at node+0x28, so the angle at node+0x3c, the same place as in 1.2.1) and the AI node
  ExecuteGroundControlRotate (update main+0x2329170, helper at node+0x20) [read].
- Each update passes two switch bytes, turn and turn the other way; the logic node reads them from the actor's
  blackboard (RotateAxisX, RotateAxisXInv; main+0x1b13c9c). With neither on the speed falls to 0, and the turn
  (main+0x2988330) is set only while the speed is not 0 [read].
- A GroundControlActor_Rotate near H-5 ran its helper once a frame and never its AI node, and did not turn on the
  save tried [measured 2026-11-07].

### 11.4 Places and travel

- Place names come from the map locations: `Banc/MainField/LocationArea/MainField.locationarea.byml` and MinusField's
  (1256 and 303 points, each naming a `Work/Location/<label>.game__location__Location.gyml`). Names are in
  `Mals/<lang>.Product.<n>.sarc.zs`: `LocationMsg/Location.msbt` by label; shrines (`Dungeon<nnn>`) in
  `LocationMsg/Dungeon.msbt` under label `0<nnn>`. Some languages keep a name's grammatical forms in tag group 0xc9
  type 10 [read] proxy-plan "Notices".
- The location banner (layout `Pa_LocationName`) is opened by the sequence node SeqOneShotRequestOpenLocation (name
  getter main+0x1ff46f4); it takes a location, not a text [read].
- **No warp call was found.** Looked at: the sequence's `WarpMgr` (name getter main+0x1fd1bf4), `Game/WarpInfoTable`
  and its warp events `DmF_SY_WarpOut` and `DmF_SY_WarpIn`, and the node SeqQueryGetCurrentLocation [read, absence]
  proxy-plan "Notices". A fast travel shows only by its effects: the Player moves hundreds of metres in one tick
  and the area's ChangeBanc runs (4.4) [inferred].
- A travel's load took about 2 s in one game [measured 2026-11-10] re-notes "Game 2 stuck in a load".

## 12. Input and UI

### 12.1 Controllers

- getController main+0x37ccc0(manager [[main+0x3960150]], index, &user name): index 0 is [mgr+0xa0], 1 to 3 are
  [mgr+0xf0 + 8*(i-1)], 4 is [mgr+0xa8]; a registered `Custom_Controller_<name>`, or bit 13 of mgr+0x13c, gives
  [mgr+0xc0] [read] re-notes "Input".
- Its per-frame callers are main+0xb012fc, main+0x37c980 and main+0x37c9b8 (UI), main+0x100d07c (camera),
  main+0x5e197c (UI); the Player asks only at load (main+0x4ced10) and keeps no controller pointer. Every Player reads
  controller 0 [measured 2026-11-03].
- Controller layout (controllers 0 and 1 share vtable main+0x394c210): +0x114 and +0x13c held mask, +0x10 repeat
  mask, +0x30 + 4*bit the frames each button has been held (float), +0x120 and +0x124 left stick x and y (-1 to 1),
  +0x110 a running counter [measured 2026-11-03]. +0x8 and +0xc pressed and released, +0x128 and +0x12c the right
  stick, are guesses from sead's order [inferred].
- Button bits: A 0, B 1, ZL 2, X 3, Y 4, ZR 5, right stick click 6, left stick click 7, L 13, R 14, d-pad up 16, down
  17, left 18, right 19, left stick up, down, left, right 20 to 23, right stick 24 to 27. Plus and Minus not
  measured [measured 2026-11-03].
- main+0x479c20 rewrites every controller from the Npad slots continuously, also while actors update: a write to a
  controller's fields during an actor's calc does not stick [measured 2026-11-03].
- A second Npad (id 1) reported to the game stalls it: from the start it never reaches the world; plugged in later,
  the actor calcs stop. Cause not traced [measured 2026-11-03] [open].

### 12.2 HUD and prompts

- The button prompts at the bottom right ("Dive R") are one CtrlGuide screen, found through [main+0x3960158] by
  name hash 0x5528d3d9 (UICtrlGuideScreen, game__ui__CtrlGuideSet) [read] re-notes "HUD control guide".
- main+0x2192c50(type) shows a prompt type (main+0x2147c64 on the screen); main+0x4cb540(type) hides one (clears that
  bit of a mask at screen +0x1b8 and rebuilds with main+0x2147df0); main+0x4cb3d8 hides all 42 [read]. The screen's
  guide stack is main+0xd82334, entered from main+0x2147e4c [measured 2026-11-04].
- Every Player's calc calls them, so any Player's state reaches the HUD [measured 2026-11-04]. Other HUD screens
  (large action prompts such as "A Talk") were not checked [open].
- Instant tips: main+0x207ea50(tips manager, tip id) queues one [read] re-notes "A co-op Link must not change this
  game".
- The ability screen effects: [main+0x3a5e878], start main+0x2056f3c, end main+0x205719c, focus main+0x2057384
  [measured 2026-11-04].
- The Ultrahand red tint and object highlight come from main+0x585a30, called from the model update main+0x580ff0,
  outside the Player's calc [measured 2026-11-04].
- No global in main's data points at a Player [measured 2026-11-04].

### 12.3 Camera

- PlayerCamera is resident. It looks along -Z, with X its right [measured 2026-11-04] re-notes "Networking v1".
- It keeps the followed Player in several caches ([cam+0x1a8]+0x160, [cam+0x858]+0x100, [cam+0xa20]+0x790,
  [cam+0x1fc0]+0x1c0, and references at [cam+0x3a0]+0x48 and [cam+0x3590]+0x298). Writing another Player into them
  is unreliable and harmful: at least one is shared with the Player's model, and it stripped that Player of hair,
  arm and armour [measured 2026-11-04] re-notes "Equipment, HUD, camera". How the game sets the camera's target was
  not found [open].
- Its calls (main+0x41bb20, 0x2e27b0, 0x245030, 0x257310, 0x2f7f70, 0x19557fc, 0x6c9ad0) make no ActorLink getActor
  call; it reads the Player through ActorLinks (main+0x1b0614, main+0x1afbe4, main+0x87509c) and reads only the
  Player's +0x228 (main+0x87509c, main+0x876670, a camera component) [measured 2026-11-04].

### 12.4 Map and minimap

- One screen draws both the map and the HUD minimap: AppMap_00 (in UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc).
  Its animation AppMap_00_MiniMap.bflan is the minimap pose: N_Cut_00 to 03 move to layout (765, -360), N_Cut_00
  shrinks from 2250x1080 to 249x249, Pa_Map_00, Pa_MapIcon_00 and N_IconArea_00 scale to 0.9, and the full map's
  cursor, stamp box and tabs hide [read] re-notes "Map and minimap markers". That the HUD minimap is this pose is
  likely [inferred].
- The map picture is the part Pa_Map_00 (PaMap_00.bflyt): ground, depths and sky each have an SR and a T pane
  (N_GroundSR_00/N_GroundT_00, N_UndergroundSR_00/T, N_SkySR_00/T) over 3000x3000 tile pictures, the first at
  (-16500, 13500). That is 36000x30000 layout units over 12000x10000 m: 3 units a metre, y north (world -z). The
  game pans and zooms by those panes [read]; the mapping is not checked against the game's own arrow [open].
- On the minimap the ground pane's matrix puts the Player at (0, 0) at zoom 0.252: the minimap is the map drawn
  centred and shown as a picture (P_MapCapture_00 in N_Cut_01). The game's icons sit in N_IconArea_00 [measured
  2026-11-07].
- main+0x96c1ac sets up the AppMap screen's layout (x0 the screen, [screen+0x28] its layout): it looks up its
  animations ("MiniMap", "InArashi", ...), Pa_Map_00 (made into a 0x27758-byte map object at screen+0x208),
  Pa_Cursor_00, Pa_PlayerNavi_00, N_IconArea_00 (screen+0x2a8), Pa_North_00 (screen+0x2a0). It sits in a function
  table at main+0x372b3f0 that also names "LayoutMsg/AppMap_00" [read].
- Layout lookups: main+0x406760(layout, "Pane" or "Part/Pane") and main+0x406acc(layout, "Part"). A layout's root
  pane is +0x18; its parts are a list at +0x48 (first [+0x50], next [node+8], name inline at node-0x28, the part's
  layout [node+0x10]) [read].
- nn::ui2d::Pane (vtable main+0x357f280): +0x18 parent, +0x20 children list head (first [+0x28], next [node+8], child
  node-8), +0x30 translate, +0x3c rotate, +0x48 scale, +0x50 size, +0x58 flags (bit 0 visible), +0x59 alpha, +0x5a
  global alpha, +0x5b base position, +0x70 global matrix (3x4 rows), +0xb0 name, +0xc9 user data. main+0x6070 finds a
  pane by name (not inside parts); the constructor is main+0x10d6080, the name setter main+0x402b30, the scale walk
  main+0x10d66d0 [read].
- Placing the game's own pins or stamps writes the save's map data [read].

## 13. Resources and mods

### 13.1 The resource size table

- The game loads a file into a buffer sized from its entry in the resource size table. A file with no entry crashed
  it on load; an entry too small made it refuse the actor [measured 2026-11-08] character-mods "Loading a party
  member's character".
- The game keeps the table in its heap exactly as the file is laid out (one `RESTBL` header found in the heap) and
  reads it on every load [measured 2026-11-08].
- Files inside packs have entries of their own. A mod's grown pack members overflow their 1.4.3 entries: the
  allocation comes back null and the parameter reader writes into it. Crash sites seen: main+0x10584a0 (on the
  actor-creation path main+0x2110f8 <- 0x8928ec <- 0x3d4410, from a grown ActorParam naming a new AIInfo) and
  main+0x173130 (the parameter reader, ppPropBuffer, from a grown PointLightParam) [measured 2026-11-04]
  character-mods "Test on 1.4.3". Raising every changed member's entry made the same mod load [measured].
- The 1.4.3 RomFS names its tables `RSDB/*.Product.143...`; a mod that ships Product.140 to 142 tables is not
  seen by 1.4.3 [read] character-mods "Version problem on 1.4.3".
- RSDB ActorInfo and GameActorInfo are paired by row position: a row added to one crashed the load; renaming an
  unused row in place works [measured 2026-11-07] proxy-plan.

### 13.2 Models and textures

- A model's `.bfres.mc` is "MCPK", two version bytes, a word, then a zstd frame without its magic holding the BFRES
  (names, materials, texture names), then the mesh data (`FMSH`). A same-length edit of the BFRES strings,
  recompressed with the mesh data kept, loads; dropping the mesh data made the game refuse the actor [measured
  2026-11-08] character-mods "Textures".
- A BFRES dictionary (ResDic: a count, then nodes {bit, left, right, key}) finds a name by testing bits counted from
  its end [read].
- Materials are kept by name across models: a second model with a material named like a loaded one but with other
  textures was refused [measured 2026-11-08].
- A material renamed with another capital first letter and the same second letter (`Lt_`, `Xt_`) was accepted;
  changing the second letter (`Ln_`, `LN_`, `Mn_`) was refused, though every dictionary lookup still worked. Some
  lookup tests bit 3 of the second character [inferred from those runs].
- Textures are found by name in `TexToGo/`, so one game holds one version of each name [read]. The texture resolver
  main+0x52ddd0 builds `TexToGo/%s.txtg` and falls back to `DummyTextureForEvent`; a missing texture never refused
  an actor [read], [measured 2026-11-08].
- Material and eye animations are looked up by the model's project name (`Model/<project>.anim.bfres`,
  `AnimationEvent/Animation/<project>.anim.baev`), and `Phive/EyeTextureScroll/Player_Link...` names the eye
  materials [read] character-mods "What does not work".
- The game makes a memory pool for each model file it loads, whose storage is the file's own page-aligned copy: the
  pool starts at the file's `FRES` header, and the header's file name names the pool. A draw's index buffer lies in
  its model's pool [measured 2026-11-08] character-mods "This PC's character in place". An armour model is shared by
  every actor that wears that armour, and a weapon model with the same weapon lying in the world [inferred].

### 13.3 Loading and caching

- The Player actor is made once, before the title screen: its pack (`Pack/Actor/Player.pack.zs`), body model
  (`Model/Link.Link_RaulSkin.bfres.mc`), animation set (`AS/Player.root.asb.zs`) and skin texture are opened about
  3 s after start. Loading another save keeps the same Player (same address, moved); To Title Screen and Continue
  keep it too. Nothing in a run makes the Player again or reads its files again [measured 2026-11-08]
  character-mods.
- An actor made again under a name the game loaded before opens none of its files: the game keeps what it loaded
  under the name, also after the actor is deleted [measured 2026-11-08].
- No flag that hides the Player's model was found; the scope view hides Link, but every state byte it changes,
  held at its scope value, left Link drawn [measured 2026-11-08] character-mods [open].
- A create request that the game refuses reports status 4; this is how a broken model or material shows [measured
  2026-11-08] character-mods "Bisect".
