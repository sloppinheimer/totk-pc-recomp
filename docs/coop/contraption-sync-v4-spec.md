# Contraption sync v4: the specification

Status, 2026-11-12: written before any code. It turns [contraption-sync-v4.md](contraption-sync-v4.md) (the design,
called D below) into data structures, messages and procedures for one new file, runtime/coop_ent.c. Where this file
and D disagree, D is wrong and is corrected first. Section 14 lists what waits on the readings of D 4.1; nothing
that depends on one of them is written before its answer is in
[contraption-game-facts.md](contraption-game-facts.md).

Conventions. Addresses are TotK 1.4.3, main+offset. "The world" is the game's memory; "the table" is ours. An
incarnation is a game's 64-bit id for this session (coop_ctr_me). Times are the world clock in microseconds
(frame_step_world_us) unless "wall" is said. Functions named without a file are in runtime/coop_ctn.c today.

## 1. Threads

- **Phase 0** (the hook on main+0xa72b00, GameDataMgr::calc, once a frame outside the parallel actor update): every
  read of the world that walks a structure, every guest call, every change of the world, and every change of the
  entity table. One function, `coop_ent_phase0(Cpu *c)`.
- **The co-op tick** (coop_ctn_tick's caller in coop_autobuild.c; it runs in menus and loads): transport only. It
  moves received messages into an inbox ring and sends what phase 0 left in an outbox ring. It answers TAKE when
  phase 0 has not run for 0.3 s (the game is in a menu): see 5.4.
- **Any game thread** (hooks): reads atomic lists that phase 0 publishes (section 11) and appends to a ring of
  events (section 10). A hook never takes the table's lock and never walks the table.

One mutex, `ent_lock`, guards the table; phase 0 holds it for its pass, the tick takes it only for 5.4.

## 2. Data

```c
typedef struct { uint64_t inc; uint32_t serial; } Eid;     // inc 0: a map-placed actor, serial = its key (4.3)

enum { LIFE_LIVE = 0, LIFE_DEAD = 1, LIFE_GONE = 2 };       // DEAD: destroyed, with its death kind; GONE: unloaded
enum { CLS_IDLE = 0, CLS_HOLD = 1, CLS_RIDE = 2 };          // what the owner's Link does with the island

typedef struct {
    Eid id;
    uint64_t owner;        // incarnation that simulates it
    uint32_t epoch;        // rises at every change of owner
    uint8_t life, death_kind, cls, flags;   // flags: ENT_GREEN, ENT_MAP, ENT_PREVIEW, ENT_COPY (we created it)
    char name[48];         // actor name
    float scale[3];
    uint64_t actor;        // this game's actor (0: not here)
    uint32_t gen;          // actor +0x10 when taken: a freed and reused actor has another
    uint64_t create_handle;// our create request while it is being made (0: none)
    uint32_t island;       // index of its island in this pass (derived, section 3)
    int64_t stated_us;     // when its owner last stated it
} Ent;
#define ENT_N 768          // 3 players x COOP_OBJS

typedef struct { Eid a, b; uint8_t sa, sb; float rel[12]; } Bond;   // a < b by (inc, serial); rel = inverse(A) * B
#define BOND_N 1024

typedef struct {           // an island of the statement of another owner, as last received
    Eid root;              // its smallest member
    uint64_t owner; uint32_t epoch;
    uint16_t n, nb;        // members, bonds
    Eid member[22]; float rel_root[22][12]; uint8_t life[22], death_kind[22];
    Bond bond[32];
    uint8_t rec[BP_SIZE]; uint8_t rec_n; Eid rec_member[21];   // the recipe (rec_n 0: none)
    uint32_t digest;       // the owner's digest of it
    int64_t us;            // when received
    int passes;            // reconcile passes that left a difference (the ladder, 7.4)
    int64_t backoff_until; // "not shown here"
    uint8_t why;           // the reason sent in the heartbeat when it cannot be shown
} Stated;
#define STATED_N 96
```

Limits are the game's: a group holds at most 21 parts through Autobuild and the glue refuses 22. An island of more
than 22 members is stated without a recipe and its copy is made part by part; that case is logged.

## 3. Reading the world (phase 0, every frame for held or moving islands, every 0.25 s for the rest)

3.1 **Liveness.** For each entity with an actor: it is alive when the actor's id (actor +0x10) is still `gen` and
its state is not a deleting one (`alive_as`). An owned entity that is no longer alive becomes DEAD or GONE by the
delete event that phase 0 took from the ring (10.3); with no event it is GONE.

3.2 **Bonds.** For each live entity this game has an actor for: `part_edges(actor)` lists its real glue edges (the
edge-list call main+0x27c1c0 per body slot 0..2, kept where edge +0xec or +0xed is set and edge +0x10 bit 2 is
clear). Each edge gives (other actor, slot, other slot). The other actor is looked up in the table by actor pointer;
an edge to an actor that is no entity enters that actor first (4.4). The bond's `rel` is inverse(world matrix of A's
body) * (world matrix of B's body), body matrix at body +0x98. The result is this game's bond set, sorted.

3.3 **Islands.** Union-find over the bond set. An island's root is its smallest member Eid. Order of members inside
an island, everywhere in this file: ascending Eid.

3.4 **Digest of an island** (the one number both games compute, each from its own world):
FNV-1a 32 over, in order: for each member ascending: inc, serial, life; then for each bond ascending by (a, b, sa,
sb): a, b, sa, sb. Poses, names, owner and epoch are not in it. An island whose members this game has no actor for
has the digest of what it has (so it differs, and that is the point).

3.5 **Layout check** (not in the digest): for an island this game does not own whose digest agrees, the pose of
each member relative to the root is compared with `rel_root` of the statement. More than 0.3 m or 15 degrees for 1 s
counts as "differs" for 7.1. This catches a bond that exists between bodies in the wrong place.

## 4. Identity

4.1 **A copy we create** (ENT_COPY). The entry exists before the create request; the request's handle is stored in
`create_handle`; when `coop_handle_status` gives 3 the actor is taken with `coop_claim` and written to the entry. For
the batch maker the entry of each record part is `rec_member[i]`, by the maker's list order (one made part per record
part, in record order).

4.2 **An actor this game's player or world made at run time.** Entered and minted (inc = this game, serial = next)
at the create hook (coop_spawn.c create_trace, main+0x599670) when the create is a player's. Which creates are a
player's: reading R/sync-v4/create-callers (section 14). Owner = this game, epoch 1.
An Autobuild's parts carry ENT_PREVIEW while SPR +0x20 bit 51 is set; a PREVIEW entity is in no statement.
The flag is cleared when bit 51 is clear and the SPR's queued bond count (+0x2b0) is 0.

4.3 **A map-placed actor.** Eid = (0, key) with key = FNV-1a 32 of (placement hash, name), flag ENT_MAP. It is
entered only at an exact cause in this game (4.4) or when a statement names it. Until the first test session has
logged equal hashes in both games for the classes of 14.6, the key is `coop_actor_world_key(actor)` as today.
A map entity is never deleted by reconcile and never created; a game that does not have its actor waits for its own
world to load it.

4.4 **Exact causes that enter an actor that is no entity yet** (owner = nobody, epoch 0, until claimed by 5.1):
this Link's Ultrahand accepted a grab of it (main+0x1c0b83c with the holder this Link); it is one end of a glue made
here (the bond maker wrap, 10.1); a hit toggled it (the DeviceSwitch hook); an Autobuild pulled it in. Nothing is
entered by a scan, a distance or a time.

4.5 **Lookup.** By Eid: open-address hash over ENT_N. By actor: a second hash keyed by actor pointer, checked
against `gen`. Both rebuilt incrementally in phase 0.

## 5. Ownership

5.1 **Claim of an unowned entity** (4.4): owner = this game, epoch 1, stated at once. Two games claiming the same
map entity in the same second both state it; the pair that is greater by (epoch, incarnation) wins in both (D 3.2),
and the loser drops its claim on receipt.

5.2 **TAKE.** Sent by a game that does not own an island when its Link: grabs a member (grab accept), mounts its
stick (the stick's rising edge, `stick_rider`), or completes a glue onto it (as the bond request, section 8, which
carries the take). Message 6.3. Sent to the owner, again every 0.3 s until answered or the cause ends.
From the cause's frame the island is "asked": the drive stops and the gates open for it (as `touched` today), for at
most 1.5 s.

5.3 **The owner's answer**, in phase 0, in arrival order:
- not the owner: REFUSE(NOT_OWNER, who it believes owns it, epoch);
- the TAKE's `epoch_seen` is lower than the entity's epoch: REFUSE(STALE) and the statement again;
- its own class for the island is HOLD or RIDE: REFUSE(HELD or RIDDEN);
- the TAKE's digest differs from the owner's digest of the island now: REFUSE(STALE) and the statement again;
- otherwise: the statement if it changed since last sent, then GRANT to all; owner := asker, epoch + 1; the island's
  device bits and payer ride in the GRANT.

5.4 **In a menu.** When phase 0 has not run for 0.3 s the tick answers a TAKE itself with GRANT, under `ent_lock`,
from the table as it stands (the world is frozen, so the last digest is still true), unless the class is HOLD or
RIDE. The heartbeat's PAUSED flag is set by coop_ctr.c as today.

5.5 **At the asker.** GRANT: owner and epoch set; the island stays out of the drive. REFUSE, or 1.5 s with no answer
from a live, unpaused owner: the island goes back under the drive and the gates close. If this Link still holds it,
the hold is ended (reading R/sync-v4/hold-end, section 14); until that is read the fallback is that `coop_g1` no
longer exempts a held part of that island, so the drive wins.

5.6 **Before the grab.** For an island whose last statement says class HOLD or RIDE, the hook on main+0x9fb7fc
answers 1 when called from the grab handler (return address inside main+0x1c0bf20), so the game refuses the grab
itself. Which other callers main+0x9fb7fc has, and what the refusal's side call main+0x1a45ba0 does: reading
R/sync-v4/grab-refuse.

5.7 **Claims without a grant** (owner := this game, epoch + 1, stated at once), only when this game's copy of the
island agrees with the last statement's digest:
- the owner left the session (coop_ctr_peer_event) and this game is the lowest incarnation that has it whole;
- the owner's heartbeat has said PAUSED or LOADING for 1 s and has not answered a TAKE (5.4 covers a menu; this
  covers a load), and this Link grabs, rides, glues onto, hits or stands on the island.

5.8 **Unload at the owner.** A delete event (10.3) classed "unload" on an owned entity: the whole island is frozen
(no statement of it from that frame) and OFFER is sent. A game whose copy agrees answers with TAKE(reason OFFER) and
is granted. No taker in 2 s: every member is stated GONE. A follower's own unload of a copy is local: the entity
keeps its entry with actor 0 and is made again by reconcile when in range (7.2).

5.9 **Class.** The owner computes it each frame: HOLD when `coop_held_here` is true of a member, RIDE when
`stick_rider` of a member is this Link, else IDLE. It is in every POSE.

## 6. Messages

All through runtime/coop_ctr.c. New type numbers 40 to 59; the v3 numbers 10 to 35 are removed with their code.
Reliable messages go on the control stream and are taken with `coop_ctr_recv` in arrival order (never
`coop_ctr_recv_type`). Every message starts with an 8-byte key (the latest-only slot key for unreliable ones).

6.1 **POSE** (40; unreliable, latest per (sender, root); 30 a second while any member moves, 1 a second at rest):
```c
typedef struct {
    uint64_t key;          // hash of root
    Eid root; uint32_t epoch; uint32_t digest;
    int64_t us;            // the owner's world clock
    uint64_t payer;
    float m[12];           // the root's main body matrix
    uint8_t cls, n, pad[2];
    uint32_t sw, on, fw, fk, fb, fp, burn;   // bit r: member r (ascending Eid) has a switch / is on / wheel / ...
    float pf[22][4];       // per member, as v3's StMsg: wheel spin and steer, stick floats, fuse and armed, spring
} PoseMsg;
```
The per-member arrays are used by a receiver only when its digest of the island equals `digest`; otherwise it uses
`m`, `cls`, `epoch` and nothing else. The transport's latest-only table is raised from 128 to 1024 slots.

6.2 **ISLAND** (41; reliable, in pieces as v3's VERSION is: header then chunks): the statement of one island.
Header: root, owner, epoch, n, nb, digest, rec_n, total length, piece index and count, a statement counter `sc`
(per sender, only to join pieces of one statement; it names nothing). Body: per member (Eid, name[48], scale,
flags, life, death_kind, rel_root[12]); per bond (a, b, sa, sb, rel[12]); when rec_n: the record (BP_SIZE) and
rec_member[rec_n]. Sent on a change of the island's digest, owner or member list; on NEED; to a game that joined.

6.3 **TAKE** (42; reliable): asker's request id, root as the asker has it, epoch_seen, reason (GRAB, RIDE, GLUE,
OFFER), the asker's digest of the island.
6.4 **GRANT** (43; reliable, to all): request id, root, n, member Eids, new owner, new epoch, sw, on, payer.
6.5 **REFUSE** (44; reliable, to the asker): request id, reason, owner, epoch.
6.6 **OFFER** (45; reliable, to all): root, epoch.
6.7 **NEED** (46; reliable, to the owner): root or one Eid.
6.8 **BOND_REQ** (47; reliable, to the owner of the target), **BOND_ACK** (48), **BOND_NAK** (49): section 8.
6.9 **REQ** (50; reliable, to the owner): Eid, kind (TOGGLE with the wanted state, EVENT set-on).
6.10 **DEATH** (51; reliable, to all, from the owner): Eid, death kind. Also carried by the next ISLAND as life.
6.11 **INDEX** (52; unreliable, latest per sender, every 2 s): count and FNV-1a over (Eid, epoch, life) of every
entity the sender owns. A receiver whose own view of that sender's entities hashes otherwise sends NEED with Eid
(0, 0) and gets every island of that sender again. This is what removes an entity whose last message was lost.
6.12 **The heartbeat** (coop_ctr.c CoopCtrHb) gains nothing. A follower's digests go in **APPLIED** (53; unreliable,
latest per (sender, root), once a second per island of another owner): root, the digest of this game's world for
it, a reason code when it is not shown here.

## 7. Reconcile (phase 0; the only code that applies structure from another game)

7.1 **When.** For each `Stated` island of another owner: on receipt, and once a second while this game's digest
(3.4) or layout (3.5) differs from the statement. Not while this game has a pending bond operation on it (8.2).

7.2 **Entities.** For each member of the statement:
- owner and epoch are taken if greater;
- LIVE, no actor here, in range (inside the part's delete radius less a margin, `delete_radius`): create. One
  missing member with no stated bond to another missing member: a plain create at root pose * rel_root (the create
  path the object mirror uses today, with the entry bound first, 4.1). Two or more missing members bonded to each
  other and a recipe present: the batch maker (section 9), all of them together;
- LIVE and two actors here for one Eid: the one flagged ENT_COPY most recently is deleted;
- DEAD and an actor here: the death is replayed with its kind (`deaths_put`'s call, the G4 refusal lifted);
- GONE and an actor here that is ENT_COPY: deleted (`part_delete`). A map actor is never deleted.

7.3 **Bonds**, after every stated member is here:
- a real edge between two members that the statement lacks: broken with the game's break message main+0x1cf54d4;
- a stated bond with no real edge: made with the direct bond call main+0x1c05a98(spr of A, sa, body of B slot sb,
  matA, matB), only when the two bodies are within 5 cm and 5 degrees of `rel` now. An island of one that is loose
  and not held is first moved into place (one SetTransform through the drive's exemption) and bonded next frame.
  What must be written on the edge afterwards to equal a hand glue, and which thread: reading R/sync-v4/direct-bond.
- nothing is assumed: the next pass reads the edges again.

7.4 **The ladder.** `passes` counts consecutive reconcile passes that end with a difference.
- passes 1 to 3: 7.2 and 7.3 as above;
- pass 4: every ENT_COPY actor of the island here is deleted and the island is made whole from the recipe
  (section 9). An island with a map member, or without a recipe, skips this rung;
- after that: "not shown here": `backoff_until` = now + 2 s, doubling to 30 s; the reason is sent in APPLIED and
  logged once. The owner's world is never changed because of a follower.

7.5 **The owner's side.** For each island it owns and each peer in range of it: when the peer's APPLIED digest has
differed from the owner's for 1 s, the ISLAND is sent to that peer again (at most once a second), and
`unconverged_us[peer]` runs. The log line of 12.2 prints it.

## 8. A glue across owners (D 3.7)

8.1 The bond maker wrap (the hook at main+0x1916830 with return address main+0x1d677a8, coop_life.c) gives phase 0,
through the ring: the two bodies, their world matrices before the call, the edge. Phase 0 resolves the two entities
(4.4 enters a missing one).
- Both owned here: nothing more. 3.2 sees the edge and the island's statement goes out.
- Otherwise, with X this game's end (taken over by the grab) and T the other owner's: a pending operation
  {op, X, sx, T, st, rel, since} is stored and BOND_REQ sent to T's owner.

8.2 While an operation is pending, reconcile does not run on the islands of X or T here, and the edge is not broken.

8.3 T's owner, phase 0, arrival order:
- T unknown, DEAD or GONE; or the merged island would exceed 21 parts; or it is not the owner (with who is):
  BOND_NAK(reason);
- X has no actor here yet (its copy is being made): the request waits, at most 3 s, then NAK(NOT_HERE);
- the bodies are not within 5 cm and 5 degrees of `rel`: X's island, when loose here, is moved into place first;
  when it cannot be (X's island is bonded to something else out of place): NAK(POSE);
- otherwise the bond is made with the direct call; then, unless this game's class for T's island is HOLD or RIDE,
  the merged island is granted to the asker (GRANT, epoch + 1); else the asker's island X is taken into this game's
  ownership (GRANT the other way). BOND_ACK(op), and the merged island's statement.

8.4 The asker: ACK removes the operation; the statement that follows already agrees with its world. NAK: its edge is
broken with main+0x1cf54d4, the reason logged in words, the operation removed. No answer in 3 s from a live owner:
the request is sent again with the same op; an owner that already made that bond answers ACK again.
A second glue between two bodies that already have an edge or a pending operation sends nothing.

8.5 Counter: "a bond in one game only" = an operation older than 5 s, or a NAK whose break did not remove the edge.
One count in a test session switches the design to take-first (D 3.7).

## 9. Making a copy (kept code, moved)

9.1 **The recipe at the owner.** For an island of 2 to 21 members the history writer main+0x1913694 is run on the
island's group as `record_pass` does today (the members in the writer's order, the skip rules of SPR +0x26 bit 1 and
+0x2e8 bit 0). `rec_member[i]` is the Eid of the actor the writer took as record part i. A member the writer skips
makes the island "without a recipe".

9.2 **The maker at a follower.** `make_pass`'s three stages, unchanged in what they do to the game: the build object
(ability +0x51f0) filled from the record, the part maker main+0x1d3b470, the claim of each made part, the confirm
main+0x1d3c0d4 with no cost, the SPR marks. Its inputs become (record, centre, rec_member[], green bits) and its
output is the actors bound to those entities. Its waits stay: not while this game's player has a design selected,
not while this Link stands where the copy goes.

9.3 Only one batch is being made at a time; islands wait in order of their statement's arrival.

## 10. Hooks and the event ring

A ring of 256 fixed-size events, written by hooks on any thread (a lock-free index), read by phase 0.
10.1 Bond made by a hand glue: {bodies, matrices, edge} (the wrap at LR main+0x1d677a8).
10.2 Structure changed: the bond maker, the edge removal main+0x2999fe8, removeScotch main+0x1c042bc, the break
message main+0x1cf54d4: {body or actor}. It only marks the entity's island to be read this frame (3.2).
10.3 Delete request main+0x63baa8 / 0x63b6b0 on an entity's actor: {actor, reason, return address, ours}. `ours` is
a thread flag set around every delete this code asks. Class "unload" or "destroy" from the reason: reading
R/sync-v4/delete-reasons. Death request main+0x1953184: {actor, kind}.
10.4 Create request main+0x599670: {handle, name, return address} for 4.2.
10.5 Grab accept main+0x1c0b83c: {spr, holder}. The stick's rising edge is read in phase 0 (`stick_rider`).
10.6 Device toggle and event, as coop_life.c does today, keyed by actor; phase 0 turns the actor into an Eid.

## 11. What phase 0 publishes for the gates (replaces lists_rebuild)

Atomic arrays, rebuilt each frame from the table:
- `driven[]`, `driven_body[]`, `driven_body_of[]`: actors and main bodies of live entities this game does not own,
  not asked (5.2). Read by `coop_g1`, the force-node and motor gates, G4, the sender gate.
- `nocharge[]`: actors of entities whose payer is not this game.
- `follow_group[]`: the group (SPR +0x28) of each island this game does not own.
- `part_list[]`: every entity's actor, for the stand-in veto (`coop_life_veto_rebuild`).
The functions the hooks call keep their names (coop_ctn_driven, coop_ctn_no_charge, coop_ctn_follows,
coop_contraption_follower, coop_ctn_part, coop_ctn_sender_entry), so coop_life.c and coop_autobuild.c do not change
where they only ask.

## 12. Drive, devices, logs

12.1 **Drive.** `follow_rigid` is kept, with its inputs changed from an `Ent` of the registry to: the island's
member actors in ascending Eid, `rel_root` of each, the last three POSE samples of the root, the clock offset. A
loose entity is an island of one with the rest band 0.03 m and 2 degrees. `floats_follow`, `springs_phase0`,
`burning_phase0`, `dev_follow`, `dev_flush`, `deaths_put`, `events_put`, `rider_weight` are kept with the same
change of input. The lone-device code is removed.

12.2 **Logs.** Every line starts with wall-clock milliseconds. An Eid prints as inc's low 16 bits, a dot, serial.
At start: the settings in use. Every 10 s and on change: `ent: owned N islands M | copies N islands M | not shown K
| unconverged: <peer> <root> <seconds> ...`. Every ownership change, every statement sent or applied, every
reconcile action and every NAK is one line with the Eids.

12.3 **Test settings** (they stay in the code): TOTK_COOP_ENT_DROP=5 drops every fifth reliable message before it is
queued; TOTK_COOP_ENT_BREAK=1 breaks one edge of a copy island every 10 s.

## 13. What leaves the build with this change

- coop_ctn.c: `reg[]` and `Ent`, cid, versions (`send_version`, VerMsg, in_v, vseen), `record_pass` except the
  history-writer call moved to 9.1, `in_place`, `bond_queue`, `bond_pool_recycle`, orphans, made / adopt / original,
  `edits_pass`, `coop_ctn_glue_made`, `take_over`, `authority`, `retire_pass`, `wanted_*`, `receive`, `send_states`,
  the lone-device code, `objs_publish` / `objs_ident` / `objs_list` / `objs_find`, `clean_line`.
- coop_players.c: for every actor that is an entity: `props`, `report_props`, `props_receive`, `force_to`,
  `place_copy`, `obj_take_over`, `obj_handover_apply`, `take_q`, `taken[]`, the OBJS packet's use for them. Kept:
  the effects list (arrows, beams, paraglider, fused gear, anything prop_skip or the entity filter leaves out), the
  riders' placing (`ride_*`), now asking coop_ent.c for the ridden entity.
- coop_glue.c, coop_bonds.c and their hook registrations; TOTK_COOP_GLUE, TOTK_COOP_BONDS, TOTK_COOP_DEVICE_SYNC.
- runtime/coop_bonds.c has another session's uncommitted change: it is removed only after that session is asked.

## 14. Waiting on the readings (D 4.1)

| # | Reading (R/sync-v4/) | What it decides here |
|---|---|---|
| 14.1 | hold-end | 5.5: how a hold is ended on REFUSE; whether the held body is moved only through G1's nine writers |
| 14.2 | edge-removal | 3.2 and 10.2: whether one hook sees every removal; 7.3: whether an immediate re-bond is safe; whether a copy's strain check is skipped |
| 14.3 | direct-bond | 7.3 and 8.3: the bytes to write after the direct call; the body slot rule; the thread |
| 14.4 | create-callers | 4.2: the rule for "a player's create"; 4.3: the placement hash field |
| 14.5 | delete-reasons | 10.3: unload or destroy by reason; paths that pass no hook |
| 14.6 | grab-refuse | 5.6: the return-address test; a native refusal of a glue target and of a stick |

Settled by the first test session's log, with the fallback written in: map keys by placement hash (4.3; logged for
planks, rocks, crates, map devices, stacked identical objects); how often the in-place bond of 7.3 holds (counted
per attempt: made, read back next pass, still there after 5 s).

## 15. Pass tests (D 4.4), by piece

| Piece | Passes when |
|---|---|
| Identity | Three fans spawned by one player: three Eids, the same in both logs, unchanged through two hand-overs and a glue |
| Ownership | Twenty grabs back and forth of one fan: one Eid, epoch 21, no REFUSE but HELD when both grab |
| Statement and reconcile | With ENT_DROP=5 and ENT_BREAK=1: "unconverged" never above 3 s |
| Glue across owners | Ten trials of each case of D 4.4: the counter of 8.5 stays 0 |
| Copy making | A 3-part build seen by the other game within 2 s of its last glue; an Autobuild of 8 parts likewise |
| Drive | A loose fan pushed by its owner: under 0.05 m error at rest in the other game; no teleport line |
| Lifetime | Walk 200 m away and back: OFFER, GRANT, and the copy made again on return, by the log |
