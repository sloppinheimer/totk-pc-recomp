# Zonai: Ultrahand, glue, contraptions, devices

What the game does with Ultrahand, glue, bonds and contraption groups, Autobuild, and the Zonai devices (TotK 1.4.3).
Conventions and marks are in [README.md](README.md); actors, deletes, death requests, physics bodies, messages and
blackboards in general are in [engine.md](engine.md). Each section heading carries the old section it replaces
("was: game-facts NN" for contraption-game-facts.md, "was: v3 NN" for contraption-sync-v3.md), so a comment that
cites the old number can be re-pointed.

Most of this is static reading. The runs behind the measured facts used few builds: a 4-part Fanplane, a 7-part
Fanboat, hand-glued fans, a lone spring and a three-wheeled car.

## 1. Names and words (was: game-facts 32-50; v3 2)

**Components of a part** (index = offset in `comps` / 8) [read]

| Comp | Offset | Name / role | Words that matter |
|---|---|---|---|
| 10 | +0x50 | physics controller set | main body `[[comps+0x50]+0x20]+0x150` (engine.md) |
| 11 | +0x58 | (device) | +0x348 bit 0x2000 follows "on"; **not** the latch (section 8.2) |
| 28 | +0xe0 | (device) | +0x398: 10 while the device draws energy, 0 otherwise [measured 2026-11-05] |
| 36 | +0x120 | (device) | +0x420 == 4 exempts a part from the broken-off kill and makes it inert; meaning open |
| 48 | +0x180 | chemical component | element list (section 9) |
| 80 | +0x280 | (model look) | +0xb4 bit 0x80000 and +0x78 = 1.0 on autobuilt originals [measured 2026-11-05] |
| 81 | +0x288 | per-body request table; also the part's body list (+0x20: count +0x58, array +0x60, stride 0x88) | requests live one frame (section 8.6) |
| 107 | +0x358 | (device) | +0x398 0x40 / 0x80 written each frame from the on bool [measured 2026-11-05] |
| 129 | +0x408 | a stick's rider sources | player.md 5 |
| 144 | +0x480 | **SPR**, SpecialPowerReceiverComponent | below |
| 166 | +0x530 | **DeviceSwitch** | below |

A contraption is N separate actors, each with its own rigid bodies, joined by fixed constraints; there is no merged
body [read]. The group object shared by every part of one contraption is at SPR +0x28 (vtable main+0x361f1b0).
Source: research: zonai/README.md "Common names", zonai/physics; re-notes "Autobuild copies".

**SPR words** [read unless marked]

| Word | Bit / field | Meaning |
|---|---|---|
| +0x20 (u64) | 0 | a group was assigned (even "none") |
| | 12 (0x1000) | mid-glue: set on the target by the glue tick |
| | 16 | held by Ultrahand (section 6) |
| | 18 | one-shot "structure changed", taken by the SPR update main+0x271190 |
| | 28 | the autobuilt mark, by the reading of main+0x1c07114 (see the measured words below) |
| | 39-41, 45 (`+0x24 & 0x2380`) | refuse a glue as source, make devices inert, silence spring sounds |
| | 51 | preview (hologram) |
| | 53, 54 | grab-refusal lookup (section 6) |
| | 55 | gate of the unglue handler (section 4) |
| | 57 | a one-frame exemption from the Zonai-gear lapse, set by message 0x6b51000a; not a destroy mark |
| +0x26 | bit 1 | the history writer skips the member |
| +0x28 | | the group object, or 0 for a lone part |
| +0x30 | `[+0x30]+0x10` | the holder's handle while held |
| +0x150 | bit 0 | green (set only by main+0x1c07114) |
| | bit 4 | refuses being a glue target |
| | bits 6, 7 | choose 0x6b510000 / 0x6b510001 sent after a glue |
| +0x158, +0x170, +0x188 | lists | glue entries, strands (strand +0x50 the edge), temporary joints |
| +0x264, +0x268 | | time stamps of the mark |
| +0x27c | bit 0 | autobuilt / grace bit (main+0x1c07114, main+0x1c070c0); bit 10 set by the glue tick |
| +0x2a0, +0x2b0 | | the queued bond list and its count (section 3) |
| +0x2e6 | byte | from a parameter byte +0x62; a non-zero value kills a green part whatever its group |
| +0x2e7 | byte | a "rigid" switch for pull strengths |
| +0x2e8 | byte | the history writer skips the member |

Measured SPR +0x20 values of one Autobuild part (2026-11-14): 0x2 from the Autobuild screen on; 0xc014000000002 once
the design is picked (bit 51 and others); 0x80000000000202 after the confirm, with SPR +0x150 bit 0 set on a part made
from zonaite. A hand-made copy given nothing read 0x2. So the confirmed word has bits 1, 9 and 55 and not bit 28;
**open**: how this squares with the reading that the mark sets bit 28. Source: contraption-sync-v4-spec 21;
re-notes "Autobuild copies" (0x0080000000000202 on originals, 2026-11-08).

**DeviceSwitch (comp 166) words** [measured 2026-11-08, read]
- +0x60 set: the device has a switch. +0x64: the device kind number (section 8.1).
- +0x8d: bit 0 the energy gate ("this game's gauge can pay", rewritten every update, section 8.5); bit 3 the toggle
  request a hit sets; bit 4 the device's own "on" mark; bit 1 a step after bit 3; bit 6 (0x40) unknown (a device at
  0x21 sometimes ignored bit 3, at 0x61 took it, and the same kind came up either way from run to run).
- +0x28 goes 8 -> 4 -> 8 and +0x90 0 -> 1 at an accepted hit.
- +0x3c, +0x40, +0x44: the draw factors (section 8.5).
Source: re-notes "Device state on the copies"; contraption-game-facts 34 (energy).

**The group object** [read]: +0x58 member count; +0x60 member ActorLinks (a memory dump of 2026-11-08 saw ActorLinks to
every device from +0x78, 0x18 apart: how the two fit is open); +0x878 flag word (bit 0 read as held, bit 5 steering,
bit 22 rider or stick fallback, bit 28 a battery member, bits 10, 11, 21, 25-27, 33, 36 requests read by the
sender); +0x880 events (bit 0x11 a stick steering, bit 0x12 "everything off", bit 26 = 0x1a a hit); +0x88c mass;
+0x8a8..+0x8d4 the contraption frame; +0x928/+0x92c/+0x930 steering numbers; +0x994 hold state; +0x9e8/+0x9ec device
state and priority (the constructor sets {3, 9}); +0x9f0/+0x9f4 draw; +0xa04..+0xa1f per-kind counts (+0xa1e the
stabilizers). The group refresh main+0xd19a00 -> main+0xd19b90 recomputes them all, so outside writes do not stick.
Source: research: zonai/sync-v3/engine_reference.md, zonai/sync-v3/review3/facts.md A5.

**Bodies of a part** [read]
- Several bodies: small wheel (base, wheel on a slider), big wheel (axis, wheel, steering; the wheel motorised),
  spring (body and piston), stake, construct head (Neck, Look), portable pot (pan on a spring), mirror. One body:
  steering stick, carts, hover stone, sled, wing, balloon, cannon, rocket, fan, light, battery, time bomb.
- main+0xab69fc gives each body its key (0 to 6) and sets body +0x1e8 bits 1 to 4 one-hot for keys 0 to 3. Key 0 is
  the main body. main+0x44bba4(ctl, key) returns a body by key. That keys agree between games is inferred (they come
  from the device's static data).
- No device re-poses a secondary body each frame; they follow their joints.
Source: contraption-game-facts 50.4; research: zonai/sync-v4/device-bodies, zonai/player-physics.

## 2. Groups and the connection graph (was: game-facts 32 graph-groups; v3 26)

[read unless marked]
- Four objects: a **Set** (0xa0 bytes, one connected set of bodies; its collision group id at Set +0x8c is taken once
  when the Set object is built), a graph edge (0x70, at bond edge +0xc0), a graph node per bonded body (id at body
  +0x150), and the **group record** (0xa70 bytes) in the table at `[[main+0x395f6f8]]+0x28` (count +0x20, FIFO free
  list +0x38), which is what SPR +0x28 points to. The group id is index | generation << 16.
- The graph runs in a job each frame (main+0x181e40): pending edges join nodes, queued Sets are dissolved and rebuilt by
  union (smaller into larger, main+0x18b0b4 -> 0x18cfbc), and for each new Set in creation order a group record is
  made and bound (main+0xc16144 -> 0xc165c4 -> 0xc16b94), every member's SPR group written (main+0x1d93978), and the
  collision words written (main+0xacf904 / 0xacfa10: the bonded bodies ignore each other).
- **No group survives a change.** Adding or removing one bond dissolves the sets at both ends and rebuilds them; every
  member gets a new record, so SPR +0x28 and the group id change at every rebuild even with the same parts and bonds.
  A rebuilt Set takes the oldest freed record. Matching a build by its group pointer cannot hold.
- The only writer of SPR +0x28 is main+0x1d93978 (reached from the graph job through main+0x2999c94). It skips a body
  with `+0x68 & 0x1c` or +0x1e8 bit 5, or an actor with no SPR; otherwise it sets SPR +0x20 bit 0 and writes the group,
  or 0 for a lone part. A part left alone gets 0 last, in the same physics step.
- New members collide with each other from the bond maker's return until the graph job takes the bond: the
  push-apart seen when bonding touching parts.
- A hook on the record bind main+0xc16b94 sees every regrouping once, with the member list filled.
- Member order: the order of first appearance in the set; it changes when a bond is added.
- One table lists every contraption of a game: a game can scan its contraptions each tick [inferred].
- Group id / no group of a part: main+0x1c03a60 (-1 for none), main+0x1c038d8 (1 if no group).
- A copy becomes a group only after the graph job has run on all its bonds. Completion test used: every part's SPR
  +0x28 is one G and G +0x58 equals the part count, read twice.
- **Measured** 2026-11-09: a group of 4 parts formed the tick after their bonds; the group table holds 256 records and
  the bond pool 512 slots.
Source: contraption-game-facts 32 (graph-groups); research: zonai/sync-v3/m2/p4/graph-groups, zonai/sync-v3/review3/facts.md A3,
A4; contraption-sync-v3 23 (X15a).

**A measurement trap** [measured]: the write watch the early tests armed made a 4 KB page inaccessible, so reads of
that page failed and were taken as 0. The late or flickering group pointer, "3 of 4 parts", and a false adoption
seen in 2026-11-08/09 runs were artefacts; rerun without the watch (2026-11-09) every copy grouped whole. Source:
contraption-sync.md, closing note; contraption-sync-v3 23; research: zonai/sync-v3/fact_base.md part A.

## 3. Bonds (was: game-facts 32 bond-make, 49.3, 50.5; v3 48.6)

**The maker** [read]
- The bond maker main+0x1916830(bond manager, description) makes a bond edge (0x120 bytes, vtable main+0x392c2a0)
  wrapping a fixed constraint (main+0xa2cedc) from a pool slot; it returns 0 when the pool is empty and the caller
  drops the bond with no trace. A pool mutex is held around it. Its two callers both run in actor-update jobs: the
  direct call (main+0x1c05b84) and the glue tick (main+0x1d67060). A hook on the maker sees every bond.
- The description (0xc0 bytes): kind 0 (fixed, 6 degrees of freedom), body A +0x10, body B +0x18, frame A +0x20,
  frame B +0x50 (3x4 each, filled by main+0x8063ec), breakable +0x80 = 0, threshold +0x84 = 1000, soft linear and
  angular 0. The bond is rigid; the soft and breakable flags are 0.
- **Frames** (corrects contraption-sync.md "Phase 0, second round", which said the maker takes frames in each body's
  own space: C12). main+0x8063ec stores frameA = inverse(matA) * pivot and frameB = inverse(matB) * pivot. At rest the
  bond holds inverse(matA) * matB, wherever the bodies are; the pivot only places the anchor (main+0x299daec weights
  by mass, a static or keyframed body counting as infinite). The algebra was checked numerically. To hold two bodies
  as they stand: matA = WA, matB = WB. To hold B at rel from A: matA = WA, matB = WA * rel. The glue tick, the queue
  and the direct call agree in this.
- For an existing edge main+0x7a4410(edge, outA, outB) returns the two local rest frames; the held pose is outA *
  inverse(outB). main+0x4abc1c returns them in world space, where they coincide at rest.
- What the solver does with bodies off the pose: not read. The glue tick bonds at the bodies' present poses after its
  soft joint has pulled them together.
- A body torn down once carries body +0x68 bit 24, which nothing clears; it gets no graph node (main+0xa2d39c answers
  -1), so a bond on it is made and joins nothing [read]. (An older note that every copy's body had bit 24 was not seen
  again in later runs.)
Source: contraption-game-facts 32, 50.5; research: zonai/sync-v4/bond-frames, zonai/sync-v3/m2/p4/bond-make.

**The direct call** main+0x1c05a98(SPR, body index, other body, matA, matB) [read, measured]
- It needs no build object, descriptor pool or SPR flag; matA and matB are world matrices at the wanted pose and the
  bodies need not be there (the bond pulls them). It does not check a null body slot.
- Unlike the glue tick it does not refuse two groups of 22 or more parts; the SPR update then breaks the bonds.
- Measured 2026-11-08: two objects bonded this way formed one group at once.
- It leaves edge +0xec; it lacks +0xef, +0xf1 and the look points: such a bond differs from a hand glue in its look
  only.

**Edge words** [read]: +0x10 bit 3 (a bond edge) and bit 2 (removed); +0xc0 the graph edge; +0xc8 |= 1; +0xcc..+0xe0 the
strand's geometry (look points, written by the strand's own pass); +0xec a glue edge (breakable through the glue-break
path); +0xed the other kind the history writer takes; +0xef puts the strand on the glue manager's list and gives the
lasting glue look (set by the glue tick and the queue reader, never cleared); +0xf1 a look branch (glue tick only);
+0x100 / +0x108 the two ends; +0x118 registered with comp 81 (main+0x1b8dba8). Only the strand reads +0xef, +0xf1 and
+0xcc..+0xe0; breaking, strain, groups, the history writer and the save do not. An edge is a bond when +0x10 bit 3 is
set and bit 2 clear, and +0xec or +0xed is set.

**Body names and slots** [read]: a hand glue names bodies by global id, not slot. main+0x44bba4 maps key 0 to `[set
+0x150]`, others through a byte map at set +0x108. The history writer reads body +0x1e8 bits 1 to 4 as slots 0 to 3.
Listing a body's edges: main+0x27c1c0(body, list); ends through main+0x2993f98 (A) and main+0x2994078 (B).

**The queued way (Autobuild's)** [read, measured 2026-11-09]
- A BondEdgeDesc (0xc0 bytes, vtable main+0x3694e50; graph node ids at +0x28/+0x2c from main+0xa2d39c; +0x30 frame A,
  +0x60 frame B, +0x90 the pivot) is pushed on part A's SPR +0x2a0 by main+0x1c07604 (main+0x1c07618 appends).
- The reader main+0x46fe60 runs in each part's own update (the second SPR update, main+0x430000) and drains the whole
  queue in one pass, but only in a frame where SPR +0x27c bit 0 is set. Setting the bit alone runs the reader once and
  does not make the part green. Per entry it resolves the two bodies (main+0x29a5a08), fills a description
  (main+0x8063ec, stiffness 100/10, rigid), makes the bond, sets +0xec and +0xef, wakes the edge (main+0xa159e4) and
  registers it with comp 81.
- The reader also asks an active device on (request {1, priority 5}) and raises group flag bit 27.
- A descriptor whose bodies do not resolve, or whose maker returns 0, is dropped while the count still goes to 0.
- The queue needs no build object. The second SPR update is skipped while the actor's death component is dying.
- Measured: queued bonds were taken 0.8 s after a mark in one run (unexplained).
Source: contraption-sync.md "Phase 0, second round"; research: zonai/sync-v3/m2/p4/bond-make, review3/facts.md A10.

## 4. Breaking, strain and the cap of 21 (was: game-facts 32 bond-break, 49.2, 50.3; v3 24)

**Removal paths** [read]: every removal passes the graph's removeEdge main+0x2995024 (it takes the graph edge, bond edge
+0xc0); its callers are main+0x299454c (unregister) and main+0x18a1a4. The paths:
- the break message: main+0x1cf54d4(queue = actor +0x378, edge, wake, with-ed) checks the edge and posts message
  0x2051000c carrying the **edge pointer** to the owner of end A; its handler main+0x1c0bb54(SPR, edge) calls
  main+0x2999fe8(system, &{edge, 1}), which removes the edge from three containers. Used by a wiggle, a part's death,
  an SPR reset and AI nodes;
- strain (below), and a strand that fails its validity test;
- a body's teardown (main+0x18ac48: request bit 22 detaches every edge of the node, past main+0x2999fe8);
- the bond manager's bulk teardown; two physics-side paths (an edge whose end body has +0x68 bit 41 and edge +0x10 bit
  16, in main+0x181560; a force check main+0x299dff0);
- main+0x455e10, "break every glue edge of this actor": death procedure mode 6, message 0x20510009, and the confirm
  for an existing part. A plain part death does not go through it;
- AI nodes known by name only: ExecuteActorRemoveTBoxBondWhenConstraintYielded, OneShotActorRemoveAllBonds,
  CreateBondFromSoftBond, CreateRoughBond.
A group rebuild and an Autobuild pull-in remove nothing. Six families of code remove edges; only three pass the one
function a hook could watch (main+0x2999fe8 sees only the wiggle message and strain). After a removal the edge has
+0x10 bit 2 set and its ends cleared, both sets are rebuilt (a lone part gets SPR +0x28 = 0), and a new bond between
the same bodies is independent of the old. Source: contraption-game-facts 32, 49.2; research: zonai/sync-v4/edge-removal,
zonai/sync-v3/m2/p4/bond-break.

**Shake and unglue** [read]: Ultrahand sets part +0x17 bit 3 (the shake detector that writes it is not found);
main+0x1d58c88 sends 0x2051000b to the held actor; removeScotch main+0x1c042bc(SPR, sender id, 0x2051000a, 0) runs a
flood fill (main+0x1c04798) from the held actor's main body over **non-glue** edges only (+0xec == 0), never onto
static or keyframed bodies, at most 0x20 actors, and posts 0x2051000a to the actors found; each runs main+0x1c0aefc,
which does nothing unless SPR +0x20 bit 55 is set. So a neighbour joined only by glue is not asked. A plain part
breaks every glue edge on all its bodies. An autobuilt part with both autobuilt and plain neighbours breaks only its
bonds to the plain ones, otherwise every glue edge. Measured 2026-11-08: a part shaken off an Autobuild was deleted
by the game (state 11) and the rest became one group again under another group object. Source: contraption-game-facts 32;
research: zonai/sync-v3/m2/p4/bond-break; re-notes "Autobuild copies".

**Strain** [read, measured]
- The strand: made for any edge with +0xec or +0xed (hand glue or not), when the edge is linked into the graph after
  the maker returned; it hangs on an SPR's list at +0x170. Its strain is at +0x1d4.
- The strain check main+0x190f034(strand, actor, dt), one caller (the strand update main+0x190e044), measures how far
  the two rest frames have moved apart (distance and angle), not a force. Tolerance 0.3 * k (0.5 * k for simple or tight
  bonds), k from the bond's strength, 0.02 when the two SPRs differ in main+0x1b0f00c. Strain rises at
  max(1, c * over * m) a second, falls at 10 a second; at 1.0 the edge is removed through main+0x2999fe8.
- Its first line returns when strand +0x1e2 is set: such a bond never breaks by strain and breaks by every other path.
  Returning at its entry is the game's own path for +0x1e2.
- Measured 2026-11-09: a part made inside the ground strained its bonds to 1 in three frames (errors 1.5 to 2.4 m); a
  stick bond whose pivots stayed 0.33 to 0.80 m apart for about 1 s was removed.
Source: contraption-game-facts 49.2, 50.3; contraption-sync-v3 24 (X3); research: zonai/sync-v4/strand-cap, edge-removal.

**The cap of 21** (corrects contraption-game-facts 49.3, which said the oldest strands are only removed: C9) [read]:
main+0x1915600 walks the glue manager's list from the oldest and, at 22 or more, marks max(1, count - 21) strands;
the strand update releases each and posts the break message, so **the bond is broken**. The list is filled by
main+0x191642c(manager, strand), called only from the strand bind and only for an edge with +0xef (set by the glue
tick and main+0x46fe60). Not read: the thread and phase of the strand update and the cap; which end's SPR holds the
strand.

## 5. Hand glue (was: game-facts 49.6; v3 39.4, 44.4, 47.4)

**The chain** [read]
- Message 0x2051000e -> createScotch main+0x1c0b94c makes a glue entry (0x5f0 bytes; two actors, body ids, two frames)
  in SPR +0x158. Its tail main+0x1d61858 copies four look vectors into the entry (+0x51c, +0x528, +0x534, +0x540) and
  sets +0x554 bit 1 only when the request's vtable slot (+0x20) answers 1, as Ultrahand's derived request does; the
  base class answers 0.
- Message 0x2051000f -> main+0x1c0ba8c -> main+0x1d61a6c -> (tail call at main+0x1d61b04) doScotch main+0x1d66e34
  starts a soft temporary joint. doScotch's only caller is that tail call; the step, the capture and the component's
  message handler (main+0x1c0bf20, reached only through the component's message method, vtable slot main+0x367d560)
  are reached by direct branches only, so every glue (Ultrahand, Autobuild, the game's own) passes doScotch's entry.
- The glue request: base class constructor main+0x1c0c4ec, vtable main+0x367d5c8; +0x20 and +0x78 ActorLinks to the two
  actors, +0x38 and +0x90 their bodies' handles, +0x3c and +0x94 two 3x4 matrices. Ultrahand's own (main+0x1d5565c)
  is 0x1b8 bytes, zeroed, made with the base constructor; the pool for the component's requests is main+0x1c0c480.
- The glue tick main+0x1d67060 (called from main+0x1d61ca0, whose own per-frame caller is not found) ends a glue when
  the joint or either body is gone, B's link is dead, main+0x1c070a0 answers 1 (SPR(A) +0x24 & 0x2380), SPR(B) +0x150
  bit 4 is set, the two groups together have 22 or more parts, or after 1.5 s. Progress grows only while the rotation
  mismatch and distance are small; above 0.5, and when the one-glue-at-a-time turn (main+0x1915070 / 0x19150d4)
  allows, it builds a description from the bodies' current world matrices, sets the target's SPR +0x20 bit 12, calls
  the bond maker, and sets edge +0xec, +0xef, +0xf1 and the look points. The glue request 0x2051000f asks
  main+0x9fb7fc with kind 8. A glue whose bond maker returns 0 goes down the abort exit with no retry.
- The lasting bond of a hand glue is made at Ultrahand's release (main+0x1d67060 from main+0x1d61ca0, return address
  of the maker call main+0x1d677a8); the glue entry is only the joint while it is made and goes about 2 s later
  [measured 2026-11-08: "no longer glued" 2 s after each glue, while the parts stayed one group].
- A glue lands on the nearest touched body of the other part (main+0x1c09388 tries every body), so a part can be glued
  to a wheel's turning body, a spring's piston or a pot's pan.
- Measured 2026-11-08: the first hand glue of a play session (stick to wing) gave both parts one group, as an
  Autobuild does.
- Glue made by calling createScotch and doScotch from outside made an entry that was gone within 4 s and no group
  [measured 2026-11-08]; a scripted Ultrahand glue never took. Whether a glue can be replayed this way: open.
Source: re-notes "Glue by hand in the other game", "Device state on the copies"; proxy-plan "Ultrahand glue";
contraption-game-facts 49.6; research: zonai/glue, zonai/sync-v3/run5/attach.

**Upkeep and the teleport latch** [read, measured 2026-11-10]: the glue entry upkeep main+0x1d62f44 (each frame from the
SPR update) releases the entry, and its joint, unless both bodies exist, have body +0x68 bit 10 set (set when a body
is added to the world) and bit 26 clear. Bit 26 is a one-step "teleported" latch: only an applied transform request
(SetTransform, ForceSetMatrix) sets bit 25, which the body's post-step update copies to 26; velocity setters and
motion-type requests do not. Measured: glues onto an object teleported by outside code ended at once (bits 10/24/26 =
1/0/1 on the target); a glue onto the same kind of object no longer teleported held. Other readers of bit 26
(main+0x256b88, 0x4e5440, 0x1b9ea7c, 0x2376e94) take it as "pose not continuous" [inferred]. Source: contraption-sync-v3
47.4; research: zonai/sync-v3/run7/bit26.

**The glue's look** [read, measured 2026-11-08]: UltraHand_Connection_Effect2 is one effect actor per Link, made by the
glue-effect manager main+0x994048 (kept at manager +0x400, its EffectInfo given the manager), and draws every glue
joint from the manager's data while both entry actors are valid. A copy of it made by name has no manager and drew a
large green slab. A joint is drawn only from an entry in the array at `[[main+0x395f6f8]+0xa0]` with phase >= 1 and
flag bits 0|1. What draws a lasting joint after its entry is gone: not found. Source: re-notes "Glue by hand in the
other game"; research: zonai/ultrahand s5-s7.

## 6. Ultrahand holds (was: game-facts 34 stick-head, 49.1, 49.6, 50.6; v3 0.3)

**The part** [read]: one UltraHandPart per Link (0xd0b8 bytes, vtable main+0x3695798, at `[SpecialPowerSystem+0x80]`; a
derived vtable main+0x3695908 at +0x88 is guessed to be the Autobuild's). States at part +0x18: 0 off, 1 ready, 2
raising, 3 hand flying (UltraHandShootSpeed 27), 4 grab asked, 5 holding. The held actor is part +0xf8. The part
update is main+0x1d58b78; its hold control main+0x1d58e80 (class B: main+0x1d5dc7c). Source: research: zonai/ultrahand.

**Grab handshake** [read]
- Aiming: while in states 1-4 the part sends 0x20510002 to the aimed actor every frame; it sets flags only and never
  asks for a refusal. What colours the highlight: not found.
- Grab: the part sends 0x20510003 (payload the Link's handle). The SPR's handler main+0x1c0bf20 refuses with 0x20510006
  when main+0x9fb7fc(SPR, 1, 0) says so or the component lock refuses (main+0x1b65418, through `[comps+0x208]` from
  main+0x194f8dc: byte +0x508 set or the handle at +0x20 valid).
- main+0x9fb7fc(SPR, kind, out) has ten callers; the grab handler's (return address main+0x1c0c228) is the only one of
  kind 1. It answers 0 at once while SPR +0x20 bit 53 is clear; otherwise it asks a game-data flag "Owned" keyed by the
  actor's parameters (main+0x1a470b0, kinds 1 to 8). Answering 1 there makes the game send 0x20510006 and the part
  ends. The device switch asks it with kind 7, a glue with kind 8.
- Accept: main+0x9fb7cc (bits 53, then 54 set), then main+0x1c0b83c: holder handle := Link at `[SPR+0x30]+0x10`,
  SPR +0x20 bit 16 set, comp +0xb0 flags90 bit 6 set, a self message 0x20510021; reply 0x20510004. Bit 16 is set only
  by main+0x1c0b83c, from three message paths of a Player's Ultrahand part, so a hold by this game's Link can be told
  from any other. The Autobuild confirm sets no hold.
- After the accept the part runs main+0x85f7d8(&held, link) -> main+0x5b811c (taken to be the update-dependency
  request of engine.md [inferred]). It fails, and the part ends, when the target is in state 7 to 11 or the target's or the Link's actor
  +0x28 bit 17 is set.
- What a grab needs: a live actor, an SPR, the "Owned" gate open, the component lock free and a valid Link handle;
  nothing about motion type or group [inferred: none of those tests is in the handshake].
- Swap: 0x20510010 moves the hold to another actor of the same thing (old gets 0x20510011, new 0x20510012).
Source: research: zonai/ultrahand s3, zonai/sync-v4/grab-refuse; contraption-game-facts 34 (stick-head), 49.6.

**The hold** [read]: the part computes each frame a hold pose (part +0x1c, 3x4; rotation quaternion at +0x1f0) and a
0x59-byte block at +0x4c from the Link's yaw frame, the hold offset moved by stick and buttons, rotation in 45-degree
steps (slerp limit 270 degrees a second), reach and view-cone clamps, push-back from the held body's contacts (+0x25c)
and ground rays (main+0x1c04a68, 0x1c0494c, 0x1c0a260); its velocity target (+0x268) is capped at 3 m/s. A hold in
states 3-5 ends by itself outside the cancel box (UltraHandCancelFarRange, UpperRange, LowerRange; read for state 3,
likely for 5). One actor is held; the others follow through bonds. main+0x1c04b8c gives the contraption's box at a
pose; main+0x1c0a43c tests placement over the members (Ultrahand update only). **What turns the hold pose into the
body's motion was not found**: the part calls none of the nine body writers and makes no constraint; SPR-side writers
are main+0x1c07728, 0x1c0812c, 0x1c07f70 from the physics routine at main+0x46fe70. Whether a hold changes the motion
type, a preset (UltraHandLowGravity, 6, exists) or body flags: not read.

**Ending a hold** [read]
- The hold control never ends a hold: no distance test, no test of the SPR's holder or bit 16. A hold ends only
  through part_end main+0xb9b3a4.
- part_end runs on message 0x20510006 whose first word equals the held actor's id (+0x380), on a failed accept
  (main+0x85f7d8), and when a glue request's actor is dead.
- From outside: queue {word 0 = Link +0x380, id 0x20510006, 0} with main+0x2b76c34 on the held actor's channel
  (+0x378). Word 0 is the receiver and the channel's actor the sender, which is what the part compares with the actor
  it holds. The SPR itself does this at main+0x1c0662c, 0x466860 and 0xa99030. part_end then sends 0x20510005, and the
  SPR runs main+0x1c0b8a8 (holder cleared, bit 16 cleared, body woken through main+0x4bf998, flags90 bit 6 cleared).
- Clearing only the SPR side leaves the part in its hold state for good.
- **A grab refused before an accept** leaves the part in state 4 with no time-out: it ignores 0x20510006 while part
  +0xf8 is unset, and must be ended with part_end(part, link, link +0x378).
- main+0x1a45ba0 (on a refusal) queues a feedback message 0x3dc6000N once per refused grab; its handler
  main+0x1b19874 writes the kind into the receiver's AI parameters and fires an AI event (corrects contraption-game-facts 49.1,
  which said it changes no state: C10).
Source: contraption-game-facts 49.1, 50.6; research: zonai/sync-v4/hold-end, seat-and-refusal.

**Only the holder's game knows** [read]: SPR bit 16, flags90 bit 6 and the part's state exist only where the Link
holds. The device inert tests (section 8.6) fire on SPR bit 16 or group bit 0, so a hold makes the held contraption's
devices inert in the holder's game only.

## 7. Autobuild (was: game-facts 32 part-maker, 50.7; v3 23, 24)

**The ability** [read, measured 2026-11-09]: a game manager made at boot, `[main+0x3a62ef0]`; its build object at +0x51f0
(pooled: the same address in both games of a pair, run after run), the menu's at +0x51f8; its update main+0x9a2c00 is
a virtual (only reference: a vtable slot at main+0x3695238). It exists on every save: on a save with
IsGet.Obj_AutoBuilder 0 (read through main+0x3d01a0, hash 0x5d930d12) the ability, build object and update are all
there, and a build was made with it (measured 2026-11-09; settles the reading that a player without the ability
has no build object). The design selected is ability +0x51e8; the builder handle ability +0x52c8, a countdown
+0x52bc. Ghidra defines no function at main+0x9a2c00, 0x9a7260 or 0x1d3c0d4: read them in the lifted C.

**The record (CmbAct)** [read]
- Header: "CmbAct" at +0; +7 a reject byte; +8 version 2 (v1 is upgraded in place by main+0x1d3cdb0); +9 part count
  1..21; +0xa bond count 1..21 (a record with no bond is refused by the confirm; one with 2 parts and 1 bond passes).
- 21 part records of 0x118 bytes from +0xc: three 3x4 body matrices relative to the root (+0x00, +0x30, +0x60), scale
  +0x90, "is on" +0x94, actor name +0x98 (an autobuilt copy is saved under its original's name from actor +0x218),
  attachment name +0xd8 (passed as the create parameter Equipment_Attachment). Before the part maker, the ability
  clears each record's +0xd7 and +0x117 and fills a pool entry from +0x90, +0x94, +0x98 and +0xd8.
- 21 bond records of 12 bytes from +0x1704: body slot of A (0..3), body slot of B, part index A, part index B. Bonds
  are body-to-body graph edges, not glue entries. A record may hold a subset; no checksum; indexes refer to pool order.
- The record does not store: green, autobuilt or existing per part; device state other than "is on" (honoured at
  creation only for the spring, through the create parameter SpecialParts_InitOn); any identity.
- Kept in game data under the array key hash 0x4290322e. Ready-made schematics are Game/BluePrint/CombinedActorInfo/
  <name>.cai, loaded by the design menu main+0x2138ab4 into `[main+0x3a62ef0]`.
Source: research: zonai/autobuild s1, s5; re-notes "Running the game's own Autobuild".

**The history writer** main+0x1913694(table = `[[main+0x395f6f8]]`, ActorLink to any part, 3x3 alignment, record out,
matrix out) [read, measured 2026-11-09]
- While Ultrahand holds an actor (ability +0x6d10 != -1) the ability calls it on the held root, so any held build,
  hand-glued or not, becomes a history entry.
- It refuses a start member with no group (SPR +0x28 empty) and a group above 21. It skips a member with no SPR, SPR
  +0x26 bit 1 or SPR byte +0x2e8 set; a stable sort on the bytes at actor +0x218 orders the parts by name, equal names
  keeping the group's member order (not stable across bond changes). 21 parts and 21 bonds; with more than 21
  qualifying edges the writer **fails** (corrects contraption-game-facts 32's "dropped silently": C8). Both body slots are stored;
  an end whose part is not in the record leaves its index byte unwritten. The writer zeroes its buffer and leaves the
  header to its caller.
- The alignment is a 3x3 rotation: given identity, each record frame is the part's body matrix less the contraption's
  centre, and the matrix out is [I | -centre]. Measured: 4 parts and 3 bonds, places matching to the millimetre.
- Its first step, the link lookup main+0x45efd4, is the job system's access check (engine.md): during the parallel
  actor update (phase byte 2) it resolves only an actor the calling job may touch. Called at phase 0 from
  GameDataMgr::calc (main+0xa72b00) it works with nothing forced; inside one part's update it still returned 0
  (the other members are not that job's).
- An ActorLink made by main+0x155ec needs +0x10 = -1 and byte +0x14 set first.
Source: contraption-sync.md "Phase 0 results" 2, "The game's own Autobuild for a copy", "Phase 1, first step";
contraption-game-facts 32, 50.7.

**Making parts** [read, measured]
- Each aimed frame build_evaluate (main+0x1d3dd3c) sets a pool entry's state: 1 a loose actor of the right name being
  pulled in; 2 it arrived (within 0.2 m and about 5 degrees, plus main+0x1d42d20); 3 to be made from the currency item
  (default Item_Ore_L; price per part by actor name, cost record +0x48). The ready check main+0x9a83dc needs every
  entry 2 or 3 with a valid hologram handle, build object +0x3c70 == 0 and +0x3c58 not a live task.
- The part maker main+0x1d3b470 (called from main+0x9a2c00 at main+0x9a70ec, on a job thread) makes one
  requestCreateActor per part at the base matrix (build object +0x120) composed with the record frame (a part's pose is
  B * F), returning to main+0x1d3bb98. It reads only the part count and needs the blueprint's +9 non-zero (the build
  step zeroes it again). Its create argument: +0x48 0x0101010100000001, +0x58 a parameter list, +0x80 the Autobuild
  type object main+0x3a4e008 (vtable main+0x361d910), +0x90 7, x4 0, and a creation callback (vtable main+0x3694e18,
  call main+0x1d43dc8) that writes a byte into the actor's init values 0x11 and 0x13.
- Parts made by the part maker do not push each other apart; every part made one by one at its place (with or without
  hologram marks, ignore pairs or local frames) was thrown apart at 15 to 80 m/s before any bond held [measured
  2026-11-09]. What keeps the maker's parts from colliding is not read. The three preview marks (SPR bit 51, actor
  +0x5ac bit 2, bit 32 of the actor object's +0x20) are set only by the refresh loop main+0x1d3d038, and none was shown
  to switch collision off.
- The preview: a preview device carries the label "ImaginaryAutoBuildedActor" at actor +0x18 (compared by
  main+0x9e6af0 and main+0x1b37958; AI query QuerySpecialPowerIsImaginaryAutoBuildedActor); the label stays on the
  built device. What confirming changes on every device: actor +0x440 gains bit 0x10, +0x5d0 (NaN during the preview)
  gets a position, +0x454 goes 0 to 1 [measured 2026-11-08]. Built parts are shown 6-8 m from the Link before the
  confirm [measured 2026-11-10].
- Pooled CombinedActorIntegrator actors wait at the origin; the active one follows the contraption [measured
  2026-11-08].
- Selecting a design releases the build object: create requests are cancelled and made parts deleted.
Source: re-notes "Autobuild copies", "Running the game's own Autobuild", "The green tint" (earlier research);
contraption-sync.md "Phase 0, second round"; research: zonai/sync-v3/m2/p4/part-maker, zonai/autobuild s2.

**The confirm** main+0x1d3c0d4(build object) [read, measured 2026-11-09]
- In play it runs from main+0x9a2c00 at main+0x9a7260 when the builder task (ability +0x52c8, checked by
  main+0x1b2f20) is done and a byte +0x1f5 is set. It first checks build object +0x3c70 clear, each entry's task
  (+0x48) done, +0x3c58 not live, and the record's magic, +9 and +0xa.
- Per entry of state 3: the mark main+0x1c07114 (below), the currency charge main+0x209b754(item manager, &currency,
  cost, 1, 0xc, 0) only if the part has an SPR, materials restored. Per entry of state 2: the hologram deleted
  (reason 0), the existing actor kept, sent 0x6b510001, its glue edges broken (main+0x455e10(SPR, 1, 1)), marked
  existing by main+0x1c070c0 (+0x27c bit 0 and time stamps; not green, no cost).
- Per bond: both entries 2 or 3, both bodies resolved, world frames base x frame[slot], pivot main+0x299daec, a
  BondEdgeDesc pushed on part A's SPR +0x2a0. The confirm makes no bond itself. Then main+0x9a84c4 empties the pool and
  leaves +0x3c70 = 1.
- It accepts hand-filled entries (an existing actor at entry +0x28, a hologram at +0x48) [read, not run].
- Run from outside at phase 0 (GameDataMgr::calc) with parts claimed 0.03 s before it, every entry state 3, +0x3c70
  cleared and cost records +0x48 zeroed (main+0xae6e04 on `[[main+0x395f900]+8]`; that the confirm then charges
  nothing is read, not measured): 0.2 s later the copy was one group,
  green, every part within 0.01 m of the record's place, rigid as it landed; in a game that never built anything too.
  Claimed earlier, the parts fell apart; claimed in the confirm's tick, their SPR came back cleared. From the actor
  update (phase 2) the confirm returned 1 but reached no part. Measured 2026-11-09.
- The make touches only the build object; no ability field or save data is written (the history only by the hold
  block). It overwrites the player's working design in the build object: base matrix +0x120, kinds list +0x4d18 and
  its counter, the list at +0x4f48, +0x3c70..+0x3c72, the blueprint, the material cache, the pool. The bond descriptors
  at +0x3c88 cannot be restored.
Source: contraption-sync.md "The game's own Autobuild for a copy", "In game B"; contraption-game-facts 32 (part-maker), 50.7;
research: zonai/sync-v4/maker-side-effects, zonai/autobuild s2, s4.

**The mark and green** [read, measured]
- main+0x1c07114 (from the confirm at main+0x1d3c4e0, on each part's SPR, which main+0x194f69c returns): SPR +0x150
  |= 1 (green), +0x27c |= 1, +0x264 time, +0x268 = 0, +0x20 bit 28 set and bit 51 cleared, actor +0x5ac bit 2
  cleared, bit 32 of the actor object's +0x20 cleared, and a 1.0 then 0.0 blend at +0x280 (component 80 by the first
  reading; a slot of comp 81 by the later review: open).
- The tint parameters AutoBuilderBuildStateRealColorR/G/B and AutoBuilderRealStateAplha are read by main+0x1d398e4,
  which never runs in play; what applies the tint to a model is not found. The label, the create type object and the
  creation callback do not tint.
- At the confirm, words in components 14, 57, 75, 80 and 83 go 0.6 -> 1.0 together and a flag word 0x11249 -> 0x10009:
  the see-through look going solid [measured 2026-11-08].
- **The broken-off kill**: the SPR update main+0x271190 (from main+0x245030 at main+0x24808c), for a green part, at
  main+0x271eb4: unless comp 36 +0x420 == 4, it asks death kind 20 (ImaginaryAutoBuildedActorDeath) through
  main+0x1953184 (return main+0x271f00) when SPR +0x2e6 != 0, or when SPR +0x27c bit 0 is clear and SPR +0x28 == 0;
  with no death component it deletes with reason 0 (main+0x63b6b0 at main+0x272174). The grace bit +0x27c bit 0 is
  cleared by main+0x2f8b70 at the end of each run unless the global `[[main+0x395f700]]+0x44` bit 0 is set, so a
  freshly marked green part with no group lives about one frame. Hand-glued parts are not green and not under the
  rule.
- **Green late** [measured 2026-11-09]: parts made green only after they shared a group died. For the first frames they
  were not one contraption to the collision filter: a fan's spinning blade (a kinematic body) pressed the wing,
  the wing died of death kind 15 (PhysicsKinematicCollision, from the actor update at main+0x27b300), its glue edges
  broke as it died (main+0x455e10), and every other part then died of the broken-off rule (kind 20). Green at the
  confirm, as the game gives it, removed the chain. Parts marked green while no group could form (still in the
  build's held state) were destroyed by the same rule.
- Taking a part off an Autobuild deletes the part and dissolves no more than its bonds: the rest is one group again
  [measured 2026-11-08].
Source: re-notes "The green tint", "Autobuild copies"; contraption-sync-v3 24; research: zonai/sync-v3/review3/facts.md
A8, zonai/sync-v3/ghidra_answers.md Q2.

## 8. Devices (was: game-facts 34, 35, 40, 41; v3 0.3, 28)

### 8.1 The kinds

Kind numbers at DeviceSwitch +0x64, whether +0x60 (a switch) is set, and what setting +0x8d bit 3 did, measured
2026-11-08 on loose devices (re-notes "Zonai device kinds and the switch").

| Kind | Actor | +0x60 | The toggle bit | Notes |
|---|---|---|---|---|
| 1 | SpObj_Beamos_A_01 (beam) | 1 | on and off | |
| 2 | SpObj_FlameThrower_A_01 | 1 | on and off | only once awake |
| 3 | SpObj_SnowMachine_A_01 (frost) | 1 | on and off | only once awake |
| 4 | SpObj_BalloonEnvelope_A_01 | 0 | - | lit by fire, not powered |
| 5 | SpObj_LiftGeneratorWing_A_01 | 0 | - | |
| 6 | SpObj_Pile_A_01 (stake) | 0 | - | |
| 7 | SpObj_Chaser_A_01 (homing cart) | 1 | on and off | |
| 8 | SpObj_ElectricBoxGenerator (shock) | 1 | on and off | |
| 9 | SpObj_LightMirror_A_01 | 0 | - | |
| 10 | SpObj_Rocket_A_01 | 1 | on | burns out by itself |
| 11 | SpObj_FlashLight_A_01 | 1 | on and off | |
| 12 | SpObj_SlipBoard_A_01 (sled) | 0 | - | |
| 13 | SpObj_CookSetOnFire_A_01 (pot) | 0 | - | lit from the start |
| 14 | SpObj_Cart_A_01 | 0 | - | |
| 15 | SpObj_WindGenerator_A_01 (fan) | 1 | on and off | at rest, alone, did not take the bit |
| 16 | SpObj_ControlStick_A_01 | 0 | - | turns its contraption on by its rider |
| 17 | SpObj_FastWheel_A_01 | 1 | on | drove off before the second switch |
| 19 | SpObj_FloatingStone_A_01 (hover stone) | 1 | on and off | |
| 20 | SpObj_WaterPump_A_01 (hydrant) | 1 | not seen | made on the stairs it was usually deleted by the game |
| 21 | SpObj_Cannon_A_01 | 1 | on and off | |
| 22 | SpObj_SpringPiston_A_01 | 1 | on | fires once |
| 23 | SpObj_EnergyBank_A_01 (battery) | 1 | no | main+0x4a3500 skips kind 0x17: passive |
| 24 | SpObj_TimerBomb_A_01 | 1 | on | arms once |
| 25 | SpObj_GolemHead_A_01 (construct head) | 1 | on and off | |
| 26 | SpObj_TiltingDoll_A_01 (stabilizer) | 1 | no | |

The kinds' names are in a table at main+0x36a4600; 18 (LargeWheel) and 27 (others) were not in the sweep. An inventory device is a capsule actor
SpObj_<Device>_Capsule_A_01 that becomes its device in the Player attachment method main+0x1b7f2d8, which calls
requestCreateActor (return main+0x1b7f8c8, no parents, no placement record, the capsule's place); its Player is
`[(x3 - 0x3e88)+0x18]`. A device made by requestCreateActor can switch itself on at once (a rocket by
ThrustIsChangeStateWhenContact, a fan made on stairs moved off) [measured 2026-11-05]. The blackboard bool
ZonauGear_IsCreatedByPlayer (0xa0b06281) is read at main+0xa14a0c and 0x1c4d810; no writer found. Source: re-notes
"Zonai devices: what turns one on"; contraption-game-facts 49.4; research: zonai/sync-v4/device-origin.

### 8.2 On and off: the latch

(Corrects re-notes "Zonai devices" and "Device state on the copies", which took `[comp 11]+0x348` bit 0x2000 as "on":
C17. Corrects contraption-sync.md, which had the DeviceSwitch request switch a group: C18.) [read, measured 2026-11-09]
- A device's on state is a latch in its on/off node: node +0x9a bit 0. The node takes two set operations, messages
  0x6b510000 (on) and 0x6b510001 (off), each changing the latch only from the other state; the same message again
  changes nothing. They go through the node's event byte (node +0x40 bit 0 on, bit 1 off), handled by
  main+0x245c114, which also takes 0x6b510001..3.
- The off trigger is tested before every on trigger, so a set-off that reaches the node cannot fail inside it.
- Send: main+0x2b76c34(queue = actor +0x378, {word 0 = [actor+0x380], id, 0}, 0). Measured: sent together at actor
  phase 0 (GameDataMgr::calc), a build's set-offs switched it off and it stayed off in 3 of 3 runs; sent from a thread
  not lined up with the frame, 2 of 4 runs left the build on (one device off a frame early, then the one-state rule).
- The game reads the blackboard bool SpecialParts_IsOn (hash 0xbcd1015f), written on every accepted change by the
  setter main+0x9fb82c (from the device's behaviour graph: main+0x9fa920 <- 0x9f91d8 <- 0x1066bb8). The node's outputs:
  IsChangeState +0x220, IsOn +0x228, IsWorking +0x230, IsCombinedGolemHead +0x238, IsAllowedEmission +0x240.
  Writing IsOn or IsWorking from outside is overwritten by the node each tick; writing the bool or the energy words by
  hand switches nothing [measured 2026-11-05/08].
- The fan flips at once and IsWorking lags IsOn by about 1.5 s (AllowDelayBeforeWorking) [measured].
- Only the spring and the time bomb take separate on/off messages (IsSeparateOnOffMessage): the spring 0x2ca80000,
  0x2ca80002 and 0x6b510003, the time bomb 0x2ca80000 (arm) and 0x2ca80002 (detonate). The light also takes forced
  on/off 0x6b510004 / 0x6b510005, sent only by the contraption controller update main+0x1917ac0 to lights. The pot's
  flame goes by 0x6906000x; the construct head's look target by 0x6b510007. Messages 0x6b510001..0x6b510008 sent to
  fans with no hit turned none on [measured 2026-11-08].
- **One state per group**: while some devices of a group are on and some off, the group sends ON to all (the mixed
  rule). One member's request switches a whole group on but not off; off needs a request on every member that is on.
- The latch changes in each device's own node calc, on a job thread; in between the mixed rule can see one off and one
  on [inferred].
- Messages are still taken while an actor's update is skipped and applied at its next update, off before on.
Source: contraption-game-facts 34 (setoff-not-taking, spring-stake, emitters), 35 (message-order); contraption-sync-v3 28;
research: zonai/sync-v3/onoff.

**The message ring** [read]: messages queued with main+0x2b76c34 go into one ring, drained by main+0x2b772b0 in push
order, in one pass, under the ring's lock; both set-offs of a build land back to back in one frame. A full ring
(capacity 100 unless a parameter block says otherwise) drops a message silently. The node handler is reached through
the node's own message interface. Source: contraption-game-facts 35 (message-order); engine.md, messages.

**Reaching a node** [read]: no path from an actor to its AI nodes was found. A node is known only when its calc runs
(x0 = node; node -> actor is `[[node+8]+8]+0x1f8`), or from another node of the same actor through the graph (count
G+0x60, array G+0x68). The on/off event byte is also the blackboard entry 0xc9efe622, byte +8; writing it charges the
gauge. Source: contraption-game-facts 34 (engine-access).

### 8.3 Hits switch devices

[read, measured 2026-11-08]
- A hit sets two things and never looks at the on state: group +0x880 bit 26 (main+0x1c02a98) and DeviceSwitch +0x8d
  bit 3 (the toggle). Five functions do it: receiver copies main+0x18d2b48, one inlined in main+0x2d7c0,
  main+0x18ddd2c, and two attack-contact copies (main+0x303be0, 0x84a664) through the hit filter main+0x1d38a00.
- The DeviceSwitch update main+0x4a3500 (from the actor update, x19 = comp 166, x20 its actor): with +0x60 set, +0x64
  not 0x17 and +0x8d bit 3, it takes the branch at main+0x4a440c: the SPR is asked main+0x9fb7fc(comp, 7, &flag), then
  main+0x9fb7cc, and comp 166 is asked for state 4 (main+0x1c08774 with {1, 4}: its +0x20/+0x24 request). Measured:
  +0x8d 0x61 -> 0x69 (bit 3) -> 0x63 (bit 1) -> 0x61 within 0.05 s.
- Measured 2026-11-08: bit 3 set on one fan of an unpowered build switched all three fans on within 2 s; bit 3 on every
  on fan switched the build off; on one on fan only, it went off for 0.5 s and the others switched it on again. The
  fold turns the hit event, with group bit 22, into bit 27; the sender sends OFF (or ON with bit 21) to every member
  when bits 27, 10 and 26 are set.
- Two hits in one window give one toggle; in two frames, two toggles. A shockwave is no toggle (its damage type is
  rejected by the filter).
- A device's neighbours switch themselves on in their own updates by reading the contraption's state; no message goes
  to them [measured 2026-11-08].
- A hit's contact copied onto another device's damage component ran the whole hit reaction but did not switch it:
  what decides is the attacker's side [measured 2026-11-08]. The damage record a hit leaves on a device: section 9.
- **Who hit** (hit-attacker): the filter takes only the device and walks its contact records. A record names its
  attacker by +0x60 (the owner of the other body, the weapon) and, when byte +0x94 bit 0 is set, +0x78 the weapon's
  topmost owner (main+0x1d19380 follows weapon holder, shooter link, hold link). A sword has no affiliation of its own:
  a sword contact counts through its wielder. The filter passes weapon, shield and arrow contacts of an actor of
  affiliation Player. Three of the five toggle functions read no actor at all. An arrow made apart from a bow has no
  shooter link and very likely never passes.
- **The jump attack**: a jump or fall attack is a weapon contact plus the attribute ZonauGearChangeStateOn (bit 54 of
  the contact's 64-bit mask); no other player attack has it. Its one consumer, main+0x1b640f4 on the attacker's side,
  writes the device's switch event byte (blackboard 0xc9efe622, byte +8) bit 0, "switch on"; no toggle, no message. A
  plain swing goes through the same function too (bit 0 or 2), besides its toggle. A device already on ignores bit 0.
- The stick, cart, pot, mirror and stake are not switchable by a hit.
Source: re-notes "Device state on the copies" (the switch); contraption-game-facts 34 (hits-status), 35 (hit-attacker,
jump-attack); contraption-sync-v3 26; research: zonai/sync-v3/onoff/b_hit.

### 8.4 The group's state and its sender

[read; corrects the older reading that the group state is "on": C18]
- main+0x32f610(group) folds every live member's DeviceSwitch request (main+0x1c4da28: lower priority wins, start {3,
  9}) into group +0x9e8 (state), +0x9ec (priority) and +0x994; called by the group refresh main+0xd19b90 and by the
  group sender main+0x32eb90. A rider (group bit 22) or a hold falls back to {1, 4} or {on-or-off, 1}.
- Measured 2026-11-09: an idle Fanplane's group read state 1 with its fans off. The state is not "devices on".
- The sender main+0x32eb90 sends 0x6b510000/1 to members and, with group bit 36 (set by the constructor
  main+0xc16b94), 0x6b510002 carrying (MultipleBonusEnergyRate * (n - 1) + 1) / n, taken as the draw factor
  DeviceSwitch +0x44 [inferred].
- Its own gauge OFF (main+0x32f4e8..0x32f4fc) needs gauge manager +0xe8 bit 1, group +0x9e8 == 1 and no battery in the
  group (bit 28).
Source: contraption-sync.md "Who writes the group's device state"; contraption-sync-v3 26; research:
zonai/sync-v3/onoff/c_group, m1_device_off_and_filter.md.

### 8.5 Energy

[read, measured]
- The gauge is the EnergyGaugeMgr singleton (manager #0x1f in `[**main+0x395dc70]`, getter main+0x89f9b0, vtable
  main+0x36d6f58, 0x180 bytes): one per game, for its own player. The value is a pending-delta cell (mgr +0x18,
  pending int +0x30) shown at +0x54 / +0x58. On the late-game test save +0x54 read 48000; the code's default maximum is
  3000: the scale between the two is not read.
- Only three functions subtract: consume main+0x1fa4ca8 (from the device on/off writer main+0x9fad68, return
  main+0x9fb71c, with +0x3c * +0x40 * +0x44; and from the Link's own main+0x4d2fe0), consumeRate main+0x1fa4ed4 (from
  main+0x9f9088 and main+0x9fad68), flushPending main+0x1fa5178 (Player AI). A charge needs mgr +0xe8 bit 2 set and
  bit 3 clear; a positive amount also needs mgr +0xe8 bit 0 set and mgr +0x50 bit 0 clear.
- Measured 2026-11-09: three fans drained 312 a second (48000 -> 46128 in about 6 s). The node calls consume about 104
  per fan many times a second while the gauge falls about 104 a second per fan: a sum of calls is not the drain. A
  consume made outside a device node did not move the gauge.
- **The energy gate**: DeviceSwitch +0x8d bit 0 is rewritten at the end of every main+0x4a3500 from this game's own
  gauge manager: blocked = mgr +0x50 bit 0, or mgr +0xe8 bit 0 clear (the game flag Obj_Battery_Get). The node forces
  OFF (main+0x9fabbc) and refuses ON (main+0x9fb9f4) while it is clear, unless node +0x9a bit 4 or kind 9. It is also
  cleared for a device out of range of this game's Link (range 20 at mgr +0xc4, against mgr +0xa0..0xa8) while the
  group's bit 6 is clear, and for 6 s after a "substitute energy" event (mgr +0xf8). Nothing latches: the gate is worked
  out again every update. A loose device consults the gauge only once its own switch state is 1.
- With a battery in the group (bit 28) the on/off writer charges the Link nothing and only sets mgr +0xe9 bit 1.
- A battery's node only publishes current and maximum of its life cell; the driver main+0x9fa28c forces it off every
  tick when its life `[+8]` is below 1. The code that lowers the cell: not read.
- **A depleted battery breaks off** [measured 2026-11-14]: it leaves the group; its collision mask word goes from
  03f7ffff to 0 and its body flag 0x800 is set about a second before its death; the other parts get a new group of one
  fewer.
- Hover stone: CustomEnergyRange 50, tested against the watching game's Link by the energy reading; the review found
  no consumer of it. Open.
- Not read: the writer of mgr +0x50; the meaning of mgr +0xe8 bit 1 and +0xe9; the frame order of the DeviceSwitch
  update against the node calc.
Source: contraption-game-facts 34 (energy); contraption-sync.md "Phase 0 results" 5, "Phase 4, third step"; contraption-sync-v4-spec
24; research: zonai/devices2/energy, zonai/sync-v3/m2/p7/gate-stuck.

### 8.6 What moves a contraption, per kind

**All thrust and steering is written by AI-node calcs onto the device's own body or joints**, through the velocity,
impulse and force setters; the physics applies it in the next flush. A device force never acts on another part or a
hit body: the rest moves only through bonds. (Corrects contraption-sync.md "The model" 4, which had a fan push each body
it hits: C19. Loose things are blown by the fan's wind capsule actor.) [read]

| Device | Node and writes | Notes |
|---|---|---|
| Fan | main+0x2487104 (setters main+0x567750, 0x25a1f0, impulse 0x29ae114) | one Chemical_WindCapsule actor per fan per game (made by main+0x18fc35c, posed by 0x18fcb2c; wind box main+0x24e0f6c, message 0x08e50014 via 0x24e1548) |
| Rocket | main+0x24343f4 -> 0x24353c4 (gravity cancel), 0x24357ec (thrust); tumble 0x2435480 (global random); kick 0x25536d8 | below |
| Wheel | ExecuteFastWheelMotion, wrapper main+0x22fa1b0, body 0x22fa3fc; motors on constraints SideGrip, LinearMotor, ClampMotor | below |
| Sled | main+0x2458320; its own body velocities and side-edge constraints | inert test at return 0x2458428 |
| Cart | main+0x2251440; flywheel motor on LinearMotor, constraints only | inert at return 0x2251a8c |
| Homing cart | main+0x225c52c; three constraints and an impulse (0x29ae114 at 0x225cbcc); target search 75 m | inert at return 0x225c6c0 |
| Merged wheel | main+0x236d2e8; no body write, queues message 0x08e5001a to what it touches | |
| LargeWheel | main+0x246ba9c / 0x246cb88 / 0x246d930; hinge motor and steering | gated by IsActive only, no inert test |
| Balloon | main+0x222d694: impulse main+0x29ae7a8, two linear and two angular velocity writes | |
| Wing | main+0x221cd04: force 0x29ae7fc, linear velocity at two sites, angular at six (rail path included) | publishes Wing_LiftRate, Wing_IsOnRail; reads no on state |
| Hover stone | main+0x230a3fc, servo 0x230a53c: linear at 0x230aa08 and 0x230a530, angular 0x230aa30 | |
| Stabilizer | main+0x24730dc -> 0x2473354 (0x40e490, 0x29ae7fc) | |
| Spring | main+0x221a7dc: one-shot velocity on head and base | below |
| Stake | main+0x221420c; landing 0x2215040 | below |
| Steering stick | main+0x226abd4 -> 0x2269ca4 -> AddForceAtPoint main+0x29a4afc on its own body every frame while ridden | pushes the whole contraption itself |

Emitters (beam, flame, frost, shock, light, mirror, pot, hydrant) and the cannon push nothing. Source: contraption-game-facts 34
(wheels, lift-devices), 41; research: zonai/devices2, zonai/sync-v3/review3/facts.md F1.

**Inert tests** [read; corrects contraption-sync-v3 0.3 on the wheel: C15]: main+0x1c02990 (true when comp 36 +0x420 ==
4, or held (SPR bit 16 or group +0x878 bit 0) while comp 81 has no override slot, or main+0x42c054, or SPR +0x24 &
0x2380), main+0x1c02914 (the same without the override test), main+0x1c09284(SPR, 0) (SPR bit 16 or group bit 0). They
gate the fan, sled, wheel motor, rocket launch, stick filter, balloon, wing, stabilizer, hover servo and others; the
beam (call at main+0x22319d4), light (main+0x2309c0c) and sled (main+0x2458428) use main+0x1c02990. The wheel node at
main+0x22fc5f8 (inert call 0x22fc918) is a twin that no shipped graph uses; the running wheel's inert test is the call
at main+0x22fa5e4 (return main+0x22fa5e8). The two inert tests are pure reads of this game's own state.

**Steering stick** [read]
- While a rider steers, the stick raises group event 0x11 each frame and the group sends ON to the switchable
  devices. When the rider leaves, the stick's own node raises "everything off" (event 0x12, group +0x880) once a frame
  for 2.0 s, only in the game where the seat was occupied. The stick's take sends 0x6b510003 once
  (OneShotSendMessageRideOnCombinedControlStick, main+0x2578820, to members with SpecialPartsParam +0x80
  IsChangeableOnOffByAttackHit and +0x84 IsSeparateOnOffMessage); its rising edge is main+0x221913c.
- Its tilt: blackboard floats ControlStick_StickX/Y (0x6f1afb02, 0xde18d120) and ControlStick_ScaledStickX/Y
  (0x4b809ce9, 0x9479c43d); its animation reads the Scaled pair, the rider's pose node reads StickX/Y. On an
  unridden stick nothing writes them each tick; they are zeroed once when the seat empties. Its engaged bool is node
  +0x370.
- Group bools: ControlStick_ForceAllPartsOn 0xe7ee94d2, ForceAllPartsOff 0xd169fcb2, SpecialParts_IsLogicOrderOff
  0x0f548ac3, SpecialParts_IsChangeableOnOffNow 0x8d0ccacd.
- Measured 2026-11-08: the stick's on bool goes 0 -> 1 when a Link takes it, with actor +0x1d0 = 0x79_00000001 and comp
  166 +0x90 0 -> 3; holding the bool at 1 with nobody on it switches nothing. Getting off, the devices went off 1-2 s
  later by the game's own path (2026-11-09).
- Its force filter divides by log10 of the group inertia, using the group frame and the first member's body.
- Seat and rider: player.md 5.
Source: contraption-game-facts 34 (stick-head), 40 (stick-graph); re-notes "Device state on the copies"; research:
zonai/devices2/control.

**Wheels** [read]: a wheel has no notion of a rider. It sees IsWorking, the inert test (held, carried, just released)
and the group steering bit 5 (group +0x878) with the stick numbers at group +0x928/+0x92c/+0x930 (the stick's StickX,
StickY, ForwardBackMove, set to NaN at the start of every group pass). Steering exists only in a call that saw event
0x11 and a stick whose ForceAllPartsOn is true. Its disabled branch un-anchors and disables its three motor
constraints (main+0x5f9dd8(c, 0, 0), main+0xa14340(c)), writes its five model floats to 0 and publishes the stored
speed node +0x2c; the next working tick re-anchors. The visible spin is FastWheel_ModelRotateSpeed (0x4b7368f5),
integrated by the model node main+0x220b9d4; the steering pose FastWheel_ModelSteeringAngle (0xbc8c3de4); only the
motion node writes them. Wheels stay on until an OFF reaches them. Cart and wheel visuals are bone posing only.
Not read: the tuning values at `[main+0x395f3a0]`; command 0. Source: contraption-game-facts 34 (wheels).

**Spring** [read]: the head is a second body on a slider joint ("PistonSlider", limits 0 to about 1.3) with a one-way
motor (pulls the head in) and a spring (pushes it out). The value the node writes is the joint's upper limit, not a
target; its two flags are spring on and motor on. Idle, the motor is on and the head in. A launch switches the motor
off and gives the head and base one velocity each (head +v, base -v/3; the setter calls return to main+0x221a9c0 and
main+0x221aa14, call sites 4 bytes earlier: C31); after that the head is where physics leaves it, and the joint holds
no extension. The launch moves only the spring's own bodies, never the player. Nothing moves the head when the base is
placed from outside. The spring's state is the latch, the launch (the tick the latch changes), post-launch timers, and
the head's pose against the base. It is the only device whose saved "is on" survives an Autobuild. Source: game-facts
34 (spring-stake), 40 (spring-bodies).

**Stake** [read]: its hold is its own constraint "ToGround", re-pointed at the body it hit by main+0x2215364 and
disabled by main+0x2215118. Source: contraption-game-facts 34.

**Balloon, hover stone, stabilizer** [read]: the balloon's swell is node +0x20 (not +0x2c); it follows heat from the
balloon's own fire sensor ("LiftFireArea", query main+0x260ee68), so it swells by itself over a fire it sees; with no
fire it gives no lift, only a brake on falling. The hover stone's target pose is local to its node, captured from the
body at its working edge and replaced by the body's pose whenever out of tolerance or held. The stabilizer works
switched on or not and needs only the group's stabilizer count (+0xa1e). Source: contraption-game-facts 34 (lift-devices).

**Rocket** [read]: on by the set messages. After its working edge it waits 20 frames, probes for its own build in front
of the nozzle (a blocked launch fails with a tumble, drawing the global random generator twice), then thrusts each
tick. Its burn is its own life: MaxLife 90, minus 1 a frame once RocketEngine_IsSucceededLaunch is true; it ends by
death kind 16 and a plain delete with the vanishing effect, no blast. The rocket node asks no death itself. (An
earlier summary had a 20 s burn timer, "likely": open until measured.) Its "gravity cancel" is a comp 81 mass and
inertia override, not a force. Source: contraption-game-facts 34 (rocket-timebomb), 35 (comp81-consumer).

**Time bomb** [read]: ignores the set messages. Armed by 0x2ca80000; 0x2ca80002 detonates at once. Fuse at node +0xdc
(now) and +0xe0 (previous), 110 frames, stepped only while armed and not held or attached; a value written from outside
is honoured. At 0 the node asks death kind 8 and calls its blast virtual each tick; the blast is a chemistry explosion
(a Blast element) of size 20, not an actor. Its node calc is main+0x2474478. Source: contraption-game-facts 34.

**Cannon and beam** [read]: neither makes an actor when it fires. Each has a Shooter component that makes its
projectiles in advance in every game that has the part (cannon: up to 8 Obj_CannonBall_A_01; beam: one BeamosBeam;
the creator main+0x1d1d6e8, create returning to main+0x1d1dbd8). Firing is Shooter::shoot main+0x1bf8df4 (cannon:
return 0x224f538; beam: request via 0x2232d0c). The cannon the game runs is ExecuteCanonMotionPostCalc (calc
main+0x224e0b8; fire function main+0x224e690); the launch velocity is the ball's blackboard Obj_CannonBall_InitVel,
written before its first tick. The ball explodes through chemistry. The beam's attack: damage 12, impulse 1000,
DamageInterval 30, range 50 by default. Source: contraption-game-facts 34 (spawns); research: zonai/devices2/emitters.

**Emitters** [read]: beam, flame, frost, shock, light, mirror, hydrant and the wind capsule read nothing only one game
has: given the on and working state, a copy runs the same code and shows the same. Each game makes its own beam actor,
element pool (35 elements and 35 attack sensors per part, made lazily at main+0x1b37958) and wind capsule. Emitters hold
fire while their group is in hold state 11 and 0.5 s after. The pot's flame is a debounced "upright, not held" test.
The construct head has no brain in its graph: it turns toward a perception target by posing a constraint, applies no
force and spawns nothing. Source: contraption-game-facts 34 (emitters, stick-head).

**Comp 81 requests** (settles the open point of contraption-sync-v3 0.3: C32) [read]: the consumer is comp 81's own
update main+0x447df0 (from main+0x41bb20 at main+0x423550). The requests are physics properties of a body (mass,
inertia scale, the gravity and damping preset, five factors), live one frame, and never a force or velocity.
Source: contraption-game-facts 35 (comp81-consumer).

**Device creation** [read]: the actor-create function has 108 direct call sites and none is a device node. Ice from
the frost device was not traced. Source: contraption-game-facts 34 (spawns).

## 9. Hits and harm on parts (was: game-facts 34 hits-status, 35, 41 wood-status)

[read unless marked]
- **The hit record on a device** [measured 2026-11-05]: the damage component (`comps+0x2f8`) gets a record at
  +0x1a0..+0x2ff the frame the sword lands (+0x1a4 = 4, +0x1c0 the attacker's reference, +0x1c8 its id, +0x1d0 attack
  flags, +0x1d8 type, +0x1dc power, +0x1fc the place, then the direction; list links at +0x260..+0x270), taken the next
  frame. The hit reaches the device from the attacker's update: main+0x1b90cb4 builds it and hands it to
  main+0x1b91844; the device's hit handling runs main+0x42a23c -> 0x1b914e8 -> 0x1b91b84 -> 0x1b92cb0 / 0x1b93160 /
  0x1b9326c. A fan's hit body is its main body + 0x330.
- **Harm by emitters**: no emitter function harms. The damage of a beam or flame is the generic hit path in the game
  that runs the victim: contacts collected (main+0x1a56930), a 0x1c0-byte contact record built (main+0x1a56360), three
  hit builds (main+0x1cd497c), merged with the attacker set (main+0x1cd0c80 / 0x1cd263c, main+0x1cd18d0), into the
  receiver (main+0x31dbc0). The record's attacker is the beam actor or the pool sensor, and through the owner chain the
  device; the Link only when the Link holds the device. Attacker = rec +0x94 bit 0 ? rec +0x78 : rec +0x60.
- **Damage rates**: Zonai device parts take no damage from weapons or blasts (all their rates are 0) and cannot burn
  (stone). The parts that drain their own life are the rocket, battery, time bomb, spring, wing and balloon.
- **Burning wood**: the chemistry update asks death kind 9 every frame once an element is burned out (from
  main+0x30efa0, return main+0x3114bc, with RequestDeathIfBurned). Ignition damage lowers life, and the life writer
  main+0x4400b0 asks kind 16 itself at 0, straight on the death component. A wooden part burning alone loses no life;
  it ends when it burns out. Burning is the element state byte (`el+0x7d`: 2 burning, 3 burned out) in the chemical
  component's element list (comp 48); electrified is bit 9 of `el+0x94`.
- **Status by request**: no function writes a status into an element; reactions add into a per-frame accumulator, a
  tick advances the burn timer, and an evaluator derives the status (the store was not found). The game's own nodes
  ask: ignite main+0x1b34a44(comp, 2, -1); extinguish and burn out main+0xb2b804(comp, 0 or 2, -1). The flame, the
  blackboard flag and the sound follow the state. Every reaction into an element passes main+0x1906768. The chemical
  component's "request A" (main+0x1b2f944) is a lightning strike, not a status setter (corrects contraption-game-facts 35).
- **Blast pushes** on parts are main+0x312718 and message 0x08e5001a, both ending in the impulse call; impulses act
  only on dynamic bodies (main+0x44be18).
- Status in elements generally: elements.md.
Source: re-notes "Zonai devices: what turns one on"; contraption-game-facts 35 (emitter-harm, part-status-damage), 41
(wood-status); research: zonai/sync-v3/m2/p6, p8.

## 10. Lifetime, limits and off-screen (was: game-facts 32 death-delete, 40, 41, 49.4, 49.5; v3 23, 44.1, 46.1)

**Deaths of parts** [read]
- For a Zonai part every death kind but five ends in a plain delete inside the request call; Zonai part records refuse
  kinds 0, 2, 3, 4, 5. A death breaks no bond and leaves no group by itself; only the delete's teardown does, in a later
  physics flush, and neighbours that lose their last glued neighbour then die by the broken-off rule if green.
- The kind asked is rewritten before it is stored (15, kinematic collision, is stored as 16); a delete made at once
  leaves the death word untouched.
- Kinds that matter for parts (table main+0x366f8b0): 6 eaten, 7 picked up, 8 exploded, 9 burned, 15 kinematic
  collision (asked every frame once a counter reaches 9, inside main+0x276fd0), 16 life depleted, 20 the Autobuild
  part's own death. Death requests in general: engine.md.
- The lapse kill: kind 16 through main+0x2f8b70 (return main+0x2f8dcc, fallback delete return main+0x2f9264) and an
  inlined copy in main+0x2f48a0 at main+0x2f7510 that asks straight on the death component (main+0x1b4b738 takes the
  actor at component +0x18).
- AI nodes that delete by group: OneShotSpecialPowerReceiverDeleteAllCombinedActor, ...DeleteCombinedActorExcept,
  ...ExceptMyself.
Source: contraption-game-facts 32 (death-delete), 34 (rocket-timebomb); research: zonai/sync-v3/m2/p4/death-delete.

**Unload** [read]
- Nothing sweeps. Every actor tests itself each frame against its own radius from the game's observers (the player
  and camera slots; observer 0 at the radius, each further one 40 m more): DisplayRadius + DeleteRadiusOffset from its
  ActorInfo record (through `[[actor+0x370]+0x18]` +0xc8, +0xac, +0xa8). Out of range it asks its own delete with
  reason 3 (main+0x2f7f70; deferred inside its own calc through main+0x63b6b0 and issued by main+0x2fb9b0). The test
  applies only with actor +0x440 bit 21; an actor with no DisplayRadius is never far. A global
  (`[[main+0x395e008]]+0xd0` not 0) switches the far unload off for every actor.
- Radii: 60 m for a capsule's item form, a beam emitter, a cannon and the small battery; 80 m pot, shock, large
  battery, construct head; 90 m fan, wheel, rocket, homing cart, stick, stake, stabilizer, hydrant; 100 m cart, hover
  stone, sled; 110 m balloon; 130 m wing; 160 m time bomb; 210 m mirror; 310 m spring.
- So a build goes part by part, each at its own radius; no exemption for a held, ridden or glued actor (keep-near
  holders, actor +0x1b8 != 0 and a held lane 3 are the read exceptions; which apply to a held part: not read).
- A part made at run time (from the pouch, by Autobuild, glued by hand) is gone for good: no code that makes it again
  and no save record of placed parts was found. Only the Autobuild history designs persist.
- Fast travel has no "world unloaded" call for such parts: the observers jump and every part fails its range test in
  the next frames. Map actors go with reason 6 when their map unit unloads. A load's teardown asks reason 4 of every
  actor (engine.md).
- No "oldest part is removed" rule; at the run-time placement pool's limit a new actor is simply not made.
- **Measured** 2026-11-09: a plank teleported 150 to 1200 m away was never deleted in 35 s: moving the actor does not
  unload it; the observers must move. This disagrees with the read radius for that case and is not explained.
Source: contraption-game-facts 41 (unload-travel), 49.5; contraption-sync-v3 23 (X15a), 46.1; research: zonai/sync-v3/m2/p8/
unload-travel, zonai/sync-v4/delete-reasons.

**Identity of parts** [read]: every run-time actor (capsule devices, Autobuild parts, drops) has placement hash all ones;
nothing tells "made by a player" from "spawned by the game at run time". NPC and enemy vehicles are not made by the
bond maker or from blueprints (the blueprint loader has one referrer, the player's design menu); how their parts are
joined is not read. Placement records: engine.md. Source: contraption-game-facts 32 (placement-id); research:
zonai/sync-v3/m2/p4/placement-id.

**Capacities** [read, measured] (every overflow is silent)

| Pool | Size | At overflow |
|---|---|---|
| bond slots | 512 (measured 2026-11-09; from a config word) | the bond maker returns 0, the bond is skipped |
| group records | 256 (measured) | no record: members get SPR +0x28 = 0 and, green with no grace, die |
| actor message ring | 100 by default | the push stores nothing |
| Autobuild descriptor pool | 21 per build object (main+0x1d3ccf0) | 0 for the 22nd |
| glue | 21 parts per group; 22 strands with +0xef in the game | the glue aborts; the cap breaks bonds |
| flame / frost pools | 35 elements and 35 sensors per part | |

A 21-part build uses up to 21 bond slots and pushes up to 21 messages in one frame when it switches. The actor create
function answers an error code when a table, the heap or its queue is full. The number of connected sets and graph
node ids could not be read. Source: contraption-game-facts 41 (limits); contraption-sync-v3 23.

**Off screen** [read]: every actor update phase opens with one gate: a per-actor calc mask (+0x3fc; bits 0 and 1 skip
the whole update, 0xc4 its components, including the SPR update and the group sender) built from "reasons" code sets
(SetCalcMask main+0x2e90e0), and a blocked bit (+0x5ac bit 6). A skipped actor still gets one full update every 150
(or 30) counts; messages wait. A game in a menu stops its devices for that game only (a lone device read "blocked"
whenever its game was paused, in every logged case) [measured 2026-11-10]. What sets the mask from view or distance,
and which phase runs the AI nodes: not found. Source: contraption-game-facts 40 (offscreen); research: zonai/sync-v3/run4/blocked.

**Deletes refused** [read]: the delete request answers 0 for an actor going away (state 7 to 11), one already accepted
(actor +0x28 bit 17), or with lock lane 2 held; a second ask after an accepted one also answers 0. Who holds lane 2 on
a Zonai part is not settled. Deletes in general: engine.md. Source: contraption-game-facts 40 (delete-refused).

## 11. Collision of parts with players and each other (was: v3 26, 29.2)

[read, measured]
- The pair test main+0x40a240 decides whether two bodies collide, from the filter record of each body at
  `[body+0x140]+8` (or +0x148 for the second physics world; corrects contraption-sync-v3 26's "+0x148 is another class": C16), the group
  id and ignore bitset (the contraption mechanism), layer masks, a tag, a collision id pool, and owner masks at
  body +0x28. Collision filtering in general: engine.md.
- The bonded bodies of a group ignore each other once the graph job has taken their bond (section 2).
- The ignore-pair manager main+0x2ae6cf4 is an owner-to-held-actor relation set, not a collision filter (corrects
  contraption-sync.md "Phase 0, second round", where its asks answered 0).
- The player's character controller pushes what it touches through CharacterUpdatePushObject (main+0x1f591b8), whose
  query never runs the pair test: a veto in the pair test does not reach a controller's pushes.
- A Player's bodies sit in six places of its physics object and the game walks them all (main+0x3cc9b4 with
  main+0x2962c58): player.md and engine.md. Measured 2026-11-09: an actor made from the Player's pack, teleported into
  a build, shoved it 9 m; the bodies that pressed it were not the main body alone.
Source: contraption-sync-v3 26, 29.2, 29.3; contraption-game-facts 32 (part-maker); research: zonai/collision-filter,
zonai/player-physics.

## 12. Not read

- Ultrahand: what turns the hold pose into body motion, and whether a hold changes motion type, preset or body flags;
  the shake detector (part +0x17 bit 3); the glue tick's per-frame caller; the caller of the glue candidate search.
- Autobuild: what keeps the part maker's parts from colliding before the confirm and what makes them solid; the
  within-frame order of the confirm, the bond reader, the graph job, the broken-off kill and the grace clear.
- Meanings: comp 36 +0x420 == 4, SPR +0x2e6, the global `[[main+0x395f700]]+0x44` bit 0, the setters of SPR bits 53
  and 55, group +0x878 bit 0, DeviceSwitch +0x8d bit 6, the writer of comp 11 +0x348; how the group's links at +0x60
  and +0x78 relate; what triggers a Set rebuild when nothing structural changed.
- Energy: the writer of gauge manager +0x50; +0xe8 bit 1 and +0xe9; how node calls become the drain; what lowers a
  battery's life.
- Devices: the message ring's drain against the node calcs and the group sender; a toggle and a jump attack's set-on
  together on a device that is on; the rocket's real burn time and who subtracts its life; the time bomb's blast
  target and the senders of its two messages; the stick's animation float storage; a shot's creator link; a flame
  sensor's owner; who writes group hold state 11.
- Harm: the step that stores an element's status; a status replay API.
- When the objects the direct bond call needs exist on a new actor, and the call against the physics step.
- Which loads run the delete-all step; how NPC and enemy vehicles are joined; what Recall and Fuse do to parts.
Source: contraption-game-facts 32.2, 34.2 and the "not read" lines of 35-50; research: zonai/*/open-questions.md,
zonai/sync-v3/review3/facts.md G.
