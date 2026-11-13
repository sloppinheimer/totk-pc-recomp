# Contraption sync v4: the specification

Status, 2026-11-12: second draft, written before any code. It turns [contraption-sync-v4.md](contraption-sync-v4.md)
(the design, D below) into data, messages and procedures for one new file, runtime/coop_ent.c. Where this file and D
disagree, D is corrected first.

The first draft was reviewed by three readers that did not write it (against the code, against the game facts, and
by walking sixteen play scenarios by hand; R/sync-v4/spec-review/, with triage.md). Every finding was accepted and
is in this draft. Points that wait on the second round of readings are marked **[R2.n]** (section 16); nothing
that depends on one is written before its answer is in [contraption-game-facts.md](contraption-game-facts.md).

Conventions. Addresses are TotK 1.4.3, main+offset. "The world" is the game's memory; "the table" is ours. An
incarnation is a game's 64-bit id for this session (`coop_ctr_me`). **Every timer in this file is wall-clock time**
(it must run in a paused game); only POSE.us and the drive use the world clock (`frame_step_world_us`). Functions
named without a file are in runtime/coop_ctn.c today. "Here" means in this game.

## 0. Scope of the first build

An **entity** is one of: a Zonai device or capsule (actor name `SpObj_*`), a part an Autobuild made, or any other
actor that an exact cause of this code's player entered (4.4: grabbed by Ultrahand, an end of a glue). The set is
frozen at entry: an entity stays one for the session.

Not entities, and left to the object mirror in coop_players.c exactly as today: pickable items, weapons, shields,
materials and fused gear, arrows, beams, the paraglider. They are not grabbed-and-glued in the cases this design
is for, and the mirror's pick-up and destroy notices already carry them. A weapon or material a player grabs with
Ultrahand becomes an entity at that moment and leaves the mirror.

## 1. Threads

- **Phase 0** (the hook on main+0xa72b00, GameDataMgr::calc, once a frame outside the parallel actor update):
  everything that walks the world's structures, every guest call, every change of the world, and the entity table.
  One function, `coop_ent_phase0(Cpu *c)`, in the fixed order of 1.1.
- **The tick** (`coop_ent_tick`, called from coop_spawn.c `coop_tick` beside `coop_ctr_tick`, on the pad poll, about
  30 a second; it runs with no Player actor and in menus): the transport half. It sets the heartbeat flags (1.2),
  drives copies (12.1) from a snapshot phase 0 publishes, and stands in for phase 0 on the ownership messages when
  phase 0 is not running (5.4).
- **Hooks** on any game thread: read the atomic lists of section 11, append to the event ring (10), and make the
  few writes named in 10 on the actor being updated. A hook never takes `ent_lock` and never walks the table.

One mutex, `ent_lock`, guards the table. Phase 0 holds it for its pass; the tick takes it for 5.4 and for the
snapshot. coop_ctr.c is already safe from any thread, so there are no inbox or outbox rings of our own: phase 0
and the tick read `coop_ctr_recv` under `ent_lock`, and a send that answers 0 (a full queue) is kept in a small
resend list and tried again each pass.

### 1.1 The order inside phase 0
1. the event ring (10); 2. liveness (3.1); 3. edges, islands and digests of marked and due islands (3.2 to 3.4);
4. received messages in arrival order (6); 5. this game's own causes: TAKE, bond operations (5.2, 8); 6. statements
of changed islands (6.2); 7. reconcile (7); 8. the published lists and the tick's snapshot (11); 9. the resend list.

### 1.2 Paused and loading
The transport's heartbeat flags are set by the tick through `coop_ctr_hb_set` (today nothing sets LOADING, and
PAUSED follows a clock that runs in menus):
- PAUSED: phase 0 has not run for 0.3 s. This is true by construction whether or not the game runs its calc in
  menus.
- LOADING: coop_loadwatch.c's load flag, or no resident Player actor. **[R2.1]** gives the flag that is set before
  the first delete of a teardown.

## 2. Data

```c
typedef struct { uint64_t inc; uint32_t serial; } Eid;   // inc 0: a map actor, serial = its stored key (4.3).
                                                         // hashed and compared field by field, never as bytes
enum { LIFE_LIVE = 0, LIFE_DEAD = 1, LIFE_GONE = 2, LIFE_FREE = 3 };  // FREE: a map entity nobody owns (5.8)
enum { CLS_IDLE = 0, CLS_HOLD = 1, CLS_RIDE = 2 };
enum { ENT_GREEN = 1, ENT_MAP = 2, ENT_PREVIEW = 4, ENT_COPY = 8 };   // MAP, PREVIEW, COPY are local, not sent

typedef struct {
    Eid id;
    uint64_t owner;         // incarnation that simulates it (0: nobody)
    uint32_t epoch;         // never falls, never restarts while the session lasts
    uint8_t life, death_kind, flags, asked;   // death_kind 0xff: a plain delete. asked: 5.2
    char name[48];
    float scale[3], radius; // radius: DisplayRadius + DeleteRadiusOffset, stated by the owner
    uint64_t actor;         // this game's actor (0: not here)
    uint32_t gen;           // actor +0x10 when taken
    uint8_t handle[0x40], link[0x40];   // our create request while a copy is being made (coop_game.c)
    int making;
    // an island of one that another game owns is kept here, not in a Stated:
    float root_m[12]; int64_t stated_ms, dead_ms;
} Ent;
#define ENT_N 768

typedef struct { Eid a, b; uint8_t sa, sb; float rel[12]; } Bond;   // a < b; rel: 3.2. Each (a,b,sa,sb) once

typedef struct {            // a statement of two or more entities, as last received
    Eid root; uint64_t from;            // root: smallest member the sender owns; from: the sender
    uint16_t n, nb; uint32_t digest;
    struct { Eid id; uint64_t owner; uint32_t epoch; uint8_t life, death_kind, green, pad;
             float rel_root[12], radius; } m[21];
    Bond bond[48];
    float root_m[12];                   // the root's world matrix when stated
    uint8_t rec[BP_SIZE]; uint8_t rec_n; Eid rec_member[21];   // the recipe (rec_n 0: none)
    int64_t ms; int passes; int64_t backoff_until; uint8_t state, why;   // state: 7.4
} Stated;
#define STATED_N 64
```

The game holds at most 21 parts in a group and refuses a glue at 22; every array is 21 wide. `Bond bond[48]`:
a 21-part island with more than 48 real edges is stated with its first 48 in ascending order and a flag, has no
recipe, and its digest covers the 48: logged, and a stated limit of the first build.

## 3. Reading the world (phase 0)

Due: every frame for an island that is held, asked, or moved in the last second; every 0.25 s otherwise; at once
for an island a ring event marked. A loose entity whose SPR has no group (SPR +0x28 is 0) has no edges and is not
passed through 3.2 at all.

3.1 **Liveness.** An entity's actor is alive when actor +0x10 is still `gen` and its state (+0x24) is 2 to 6
(`alive_as`). An owned entity that stopped being alive takes its life from the delete event of 10.3; with no
event it is GONE.

3.2 **Bonds.** For each live entity here in a group: its bodies are taken from the part's body list
(**[R2.4]**: [comps +0x288] +0x20, count +0x58, array +0x60, stride 0x88) and each body's slot is the key for
which main+0x44bba4 returns that body. For each body, main+0x27c1c0 lists its edges; an edge is a bond when edge
+0x10 bit 3 is set (registered) and bit 2 is clear, and edge +0xec or +0xed is set. The other end's actor is looked
up by pointer (4.5); an end that is no entity is entered by 4.4 ("an end of a real glue edge of an entity").
`rel` is taken from the bond's own rest frames, main+0x7a4410(edge, outA, outB) (**[R2.5]** gives the formula),
not from the bodies' present places. Two edges with the same (a, b, sa, sb): the bond is listed once and the newer
edge is broken (7.3's break).

3.3 **Islands.** Union-find over the bond set. Members in ascending Eid everywhere. All members of an island this
game owns carry one epoch: when the world merges two of its islands, all take the greater.

3.4 **Digest** (each game computes it from its own world): FNV-1a 32 over, for each member ascending: inc, serial,
life; then for each bond ascending by (a, b, sa, sb): those six values. Owner and epoch are not in it (ownership has
its own comparison, 6.11). Poses are not in it (3.5).

3.5 **Layout check.** For an island another game owns whose digest agrees here: each member's main-body pose
relative to the root against `rel_root`. More than 0.3 m or 15 degrees for 1 s is a layout difference (7.4 rung 2).
A member whose chain of bonds to the root passes a body slot other than 0 (a part glued to a spring's head or a
wheel's turning body) is not checked and not driven rigidly (12.1). **[R2.4]**

## 4. Identity

4.1 **A copy this code creates.** The entry exists first, flagged ENT_COPY, with `handle` and `link`; the create is
coop_game.c `request_create` (a variant that takes a scale); a thread flag `ent_creating` is set inside
`request_create` and around the batch maker's guest call, so every create this code asks is known as ours. At
status 3 the actor is claimed and bound. For the batch maker, made part i is bound to `rec_member[i]`.

4.2 **A run-time actor the game made here** is minted (inc = this game, serial = next; owner this game, epoch 1) by
the rules below, in order. The create hooks are main+0x599670 (placement record in x5) and the synchronous
main+0xa1bc30 (placement record in x3; it returns the actor), which needs a new entry in cpu.h's hook list and a
full regeneration.
- `ent_creating` is set: ours (4.1), not minted;
- a placement record is passed: a map actor, 4.3, not minted;
- the name is not `SpObj_*` and the return address is not the Autobuild part maker's: not an entity (0);
- return address main+0x1b7f8c8 (a capsule opening into its device): minted when the Player is this game's Link;
  for another Player actor it is counted and not minted. **[R2.2]** gives the path from the call to the Player;
- return address main+0x1d3bb98 (the Autobuild part maker): minted, ENT_PREVIEW; all parts of one maker run are one
  batch (4.6);
- any other `SpObj_*` create (a dispenser's capsule, a device an AI node makes): minted only under the object
  mirror's present guards, carried over by name from coop_players.c `prop_fn` / `report_props`: nearer this Link
  than any stand-in or unplaced, within 6 m at first sight, no copy pending near, not a preview, at most 20 new at
  once, not within the quiet time after a warp. Counted, with the return address in the log.
How the request becomes an actor: **[R2.2]** (the pool record behind x3, or a function every new actor passes).
Until read, nothing of 4.2 is written. Nothing is minted with no session (`coop_ctr_me()` is 0): the actor is
entered when the session starts. When this game's incarnation changes, every entity it owned is re-owned under the
new one at epoch + 1 and stated.

4.3 **A map actor.** "From the map" is: its placement record P = [actor +0x370] has (P +0x14 & 8) set and a hash at
P +0x88 that is neither 0 nor all ones. Eid = (0, key), flag ENT_MAP. The key is computed **once, at entry, and
stored**: FNV-1a 32 of (placement hash, name) once the first test session has logged equal hashes in both games for
the classes of section 16; until then `coop_actor_world_key(actor)` at entry. A game that receives a statement
naming a map key finds the actor with the object mirror's existing resolver (the all-actor scan of
`coop_obj_find`, at phase 0, at most once a second per missing key, with its cache). Two actors for one key are
logged. A map entity is never created and never deleted by this code. A placement that never loads here within
10 s in range (a save flag switched it off) is shown by a run-time copy, flagged and logged.

4.4 **Exact causes that enter an actor that is no entity** (owner nobody, epoch 0, until 5.1): this Link's
Ultrahand accepted a grab of it (main+0x1c0b83c, holder this Link); it is an end of a real glue edge of an entity
(3.2); it is an end of a glue made here (10.1); a hit toggled it and it has a device switch; an Autobuild pulled it
in. Nothing is entered by a scan, a distance or a time.

4.5 **Lookup.** By Eid and by actor pointer (checked against `gen`): two open-address hashes.

4.6 **An Autobuild's preview.** ENT_PREVIEW is cleared for all parts of one maker run together, when SPR +0x20 bit
51 is clear on all of them, every SPR's queued bond count (+0x2b0) is 0, and the edges of 3.2 have been listed once
(or 1 s has passed). A PREVIEW entity is in no statement, and its delete (a cancelled hologram, reason 0) removes
the entry and sends nothing. An existing entity the Autobuild pulls in keeps its statement throughout.

4.7 **Removal of entries.** A DEAD or GONE run-time entity is stated for 10 s by its owner, then dropped there;
a follower drops it 10 s after the last statement of it. An entry with epoch above 0 that is LIVE or FREE is never
dropped.

## 5. Ownership

5.0 **One rule for every path.** `apply_owner(e, owner, epoch)` is the only code that changes an entry's owner or
epoch from a message, and it takes the pair only if it is greater than the entry's by (epoch, then incarnation).
Every message that carries an owner goes through it: GRANT, REFUSE, ISLAND, POSE, INDEX. Whenever it takes from
this game an entity whose island this Link holds or rides, the hold is ended (5.5) or the ride goes inert (5.6).
Every GRANT and every claim uses epoch = 1 + the greatest epoch of every member involved, as the granter knows them.

5.1 **Claim of an unowned or FREE entity** (4.4, 5.8): owner this game, epoch + 1, stated at once. Two games
claiming at once differ in incarnation, so `apply_owner` gives one winner in both; the loser's hold is ended by 5.0.

5.2 **TAKE.** Sent by a game that does not own an island when its Link grabs a member (grab accept) or takes its
stick (`stick_rider` becomes this Link). It names the member touched, not the root. A request has a state: open,
granted, refused, expired. While open: the island is "asked" (out of the drive, gates open, as `touched` today),
TAKE is repeated every 0.3 s to the owner as now known, for as long as the cause lasts, at most 1.5 s per attempt;
then expired, and a new request opens after 1 s if the cause still lasts. A glue carries its take in the bond
request (8); its islands stay asked until the operation ends.

5.3 **The owner's answer**, in arrival order. The owner keeps (asker, request id) and its answer for 10 s and
gives the same answer to the same request again.
1. not the owner: REFUSE(NOT_OWNER, the owner and epoch it knows);
2. `epoch_seen` below the entity's epoch: REFUSE(STALE) and the statement again; above: the TAKE is kept one pass
   (a GRANT to this game is on its way) and then answered by rule 1 or 3;
3. this game's class for the island is HOLD or RIDE: REFUSE(HELD or RIDDEN);
4. an OFFER is open for the island (5.8): the stored digest is used in rule 5;
5. the asker's digest differs from this game's digest of the island: REFUSE(STALE) and the statement again;
6. otherwise GRANT to all (6.4), `apply_owner` here. The giver then repeats the GRANT every 0.5 s until it has a
   POSE or ISLAND from the new owner at that epoch or a higher one (at most 5 s; then INDEX settles it, 6.11).

5.4 **When phase 0 is not running** (PAUSED, 1.2), the tick answers TAKE, BOND_REQ and REQ under `ent_lock`, by the
whole of 5.3, from what phase 0 last stored per owned island (digest, class, member list, epochs). It answers only
when no older reliable message from that sender is unread. A BOND_REQ or REQ is answered as a take: GRANT of the
island to the asker and the positive answer, with no bond or toggle made here; the asker's world is then the truth
and this game applies it by reconcile when it resumes. Not while LOADING and not for an island with a delete event
waiting: those get no answer and fall to 5.7.

5.5 **At the asker.** A GRANT or REFUSE is taken only by an open request (a REFUSE for a granted or expired request
is dropped; a GRANT for an expired request is taken as ownership with class IDLE). A REFUSE(NOT_OWNER) naming the
asker itself at a greater epoch is a GRANT. On REFUSE or expiry the island goes back under the drive, and a hold on
it is ended the game's own way: the message 0x20510006 is queued on the channel of the actor the Ultrahand part
holds (part +0xf8, not any member) with main+0x2b76c34, addressed as the game's own refusal at main+0x1c0662c
addresses it (**[R2.6]** settles which word is the receiver). The part then runs its own end (main+0xb9b3a4). Read
back: if SPR +0x20 bit 16 and the holder handle still say "held by this Link" 10 frames later, the message is sent
again (the actor message ring can drop one), up to three times, then logged. The SPR side alone is never cleared.

5.6 **Refusing before it starts.**
- Grab: for a member of an island whose owner's class is HOLD or RIDE (the class from POSE, not older than 2 s),
  the hook on main+0x9fb7fc answers 1 when its kind is 1 and the return address is main+0x1c0c228. No hold starts
  (the accept never runs). What the part does next, and the feedback message 0x3dc6000N that main+0x1a45ba0 queues:
  **[R2.6]**; until read, main+0x1a45ba0 is made to do nothing for a refusal this hook caused.
- Stick: the seat claim main+0x502e60 answers "refused" for this Link on a stick of such an island. **[R2.6]**
  (every caller takes the refusal cleanly). Until read: the Link sits, the island is not asked, the stick's input
  is not sent, TAKE(RIDE) is repeated once a second, and a log line says so.

5.7 **Claims without a grant** (owner this game by 5.0's epoch rule, stated at once):
- the owner left the session (`coop_ctr_peer_event`): an exact cause of 5.2 here claims at once; after 2 s the
  lowest incarnation whose copy's digest agrees with the last statement claims (APPLIED goes to all, so every game
  knows every other's digest); after 5 s the lowest incarnation with any actor of the island claims, and its world
  is the truth;
- the owner's heartbeat says LOADING, or PAUSED with no answer to a TAKE for 1 s: an exact cause here claims.

5.8 **Unload at the owner.** An accepted delete (10.3) classed "unload" on an owned entity: that member's island is
frozen as it was the frame before (member list, digest, epochs stored), POSE carries the OFFER flag, and OFFER is
sent every 0.5 s for 2 s. A TAKE(OFFER) is compared with the stored digest and granted. No taker: **that member**
is stated GONE (a map entity: FREE); members still alive stay owned and are stated as the island they now are.
A follower's own unload of a copy is local: the entry keeps actor 0 and the `Stated` is "out of range" (7.4).

5.9 **Class.** The owner computes it each frame (HOLD: `coop_held_here` of a member; RIDE: `stick_rider` of a
member is this Link). It is in every POSE. A change of class sends the POSE at once and with the next two ticks.
A class older than 2 s counts as IDLE at a receiver.

## 6. Messages

All through runtime/coop_ctr.c; new types 40 to 59. The v3 types (1, 3, 10 to 35) go with their code; a type this
code does not know is dropped. Reliable control messages are taken with `coop_ctr_recv` in arrival order. ISLAND
pieces go on the bulk stream, so a long statement does not sit in front of a TAKE or GRANT; nothing relies on order
between the two streams (a GRANT carries its member list, and `apply_owner` settles the rest). ENT_DROP (13)
drops whole messages before they are queued, never pieces, and never one from the resend list.

6.1 **POSE** (40; unreliable, latest per (sender, root); 30 a second while a member moves, 1 a second at rest):
```c
typedef struct {
    uint64_t key;          // hash of root
    Eid root; uint32_t epoch, digest;
    int64_t us;            // the owner's world clock
    uint64_t payer;
    float m[12];           // the root's main body matrix
    uint8_t cls, n, flags, pad;              // flags: POSE_OFFER
    uint32_t sw, on, fw, fk, fb, fp, burn;   // bit r: member r in ascending Eid
    float pf[21][4];       // per member, as v3's StMsg
} PoseMsg;
```
A receiver passes (sender, epoch) through `apply_owner` for the root, uses `m`, `cls` and the flag always, and the
per-member arrays only when its own digest of the island equals `digest`. A POSE from a game that is not the
root's owner after that is dropped. The transport's latest-only table grows from 128 to 1024 slots with a hash
over its keys (it is scanned linearly today).

6.2 **ISLAND** (41; reliable, bulk stream, in pieces): header {sender's statement counter (it only joins pieces),
root, n, nb, digest, flags (more edges than 48), rec_n, root_m[12], total length, piece, pieces}; per member {Eid,
owner, epoch, life, death_kind, green, name[48], scale[3], radius, rel_root[12]}; per bond {a, b, sa, sb,
rel[12]}; when rec_n: the record and rec_member[]. **A game states the members it owns.** A member another game
owns appears only as the far end of a bond, with the (owner, epoch) the sender knows for it; a receiver passes
that pair through `apply_owner` like any other and never loses its own entity to such a row unless the pair is
greater. An island of one is stated by ISLAND with n = 1 (about 150 bytes). Sent on a change of digest, of a
member's owner, epoch or life; on NEED; to a game that joined; and by 7.5.
At a receiver: an ISLAND removes every `Stated` from the same sender that shares a member with it and is older.

6.3 **TAKE** (42): request id, the member touched, epoch_seen, reason (GRAB, RIDE, OFFER), the asker's digest.
6.4 **GRANT** (43, to all): asker, request id, n, member Eids, new owner, new epoch, sw, on, payer.
6.5 **REFUSE** (44, to the asker): request id, reason, owner, epoch.
6.6 **OFFER** (45, to all): root, epoch, digest.
6.7 **NEED** (46, to a game): an Eid, or (0, 0) for everything that game owns. Sent for a root when a POSE names a
root this game has no statement of for 0.5 s (at most once a second per root), and by 6.11.
6.8 **BOND_REQ** (47), **BOND_ACK** (48), **BOND_NAK** (49): section 8.
6.9 **REQ** (50, to the owner) {request id, Eid, kind, value}: TOGGLE (the wanted state, read in the hook),
EVENT (set on), PICKED (this game's Link took a copy into its pouch), BROKE (a copy was destroyed here by this
game's own play). **REQ_ANS** (54) {request id, done or refused}. Sent again every 0.5 s for 3 s.
6.10 **DEATH** (51, to all, from the owner): Eid, kind (0xff a plain delete). For an Eid a receiver does not know,
it is kept 2 s for a statement to arrive. The next ISLAND carries the life too.
6.11 **INDEX** (52; unreliable, latest per sender, every 2 s): count and FNV-1a over (Eid, epoch), ascending, of
every LIVE, not PREVIEW entity the sender owns. A receiver whose view of that sender hashes otherwise sends
NEED(0, 0); the answer is every island of the sender again and a reliable **INDEX_END** (55) {count, hash}, on which
the receiver marks GONE every entity it holds for that owner that was not named. And the other way: a receiver
that holds, for the sender, an entity at an epoch the sender's INDEX cannot contain (the sender's list is shorter)
states what it believes {Eid, owner, epoch} in a **BELIEF** (56); `apply_owner` at the sender takes it if greater.
This is what repairs an entity nobody states.
6.12 **APPLIED** (53; unreliable, latest per (sender, root), to all, once a second per foreign island here): root,
this game's digest of it, state and reason (7.4).

## 7. Reconcile (phase 0)

7.1 **When.** For each statement of another owner: on receipt, and once a second while this game's digest or
layout differs from it. Not for an island with an open bond operation here (8.2).

7.2 **Entities.** For each member the statement's sender owns:
- LIVE, no actor here, in range (this Link within `radius` less 10 m of the statement's root pose), not `making`:
  create. If **no** member of the island has an actor here and a recipe is present: the batch maker (9), placed
  from `root_m`. Otherwise a plain create of the one member at root * rel_root, and its bonds by 7.3;
- two actors here for one Eid: the newer ENT_COPY one is deleted;
- DEAD and an actor here: the death is replayed with its kind (`deaths_put`'s call, G4 lifted); if the request
  answers 0 on two passes, or the kind is 0xff, the actor is deleted (`part_delete`);
- GONE and an actor here that is ENT_COPY: deleted. A map actor or an actor this game made is never deleted for
  GONE: it becomes a claim of this game (5.1).

7.3 **Bonds**, for each member M the statement's sender owns, when every member is here:
- **break:** every real edge of M whose (a, b, sa, sb) the statement lacks, unless it is the edge of an open bond
  operation, or its other end is an entity whose own statement (same or greater epoch) lists that bond. This also
  applies a split, whose edge lies between two statements. The break is the game's message main+0x1cf54d4(owner
  actor of end A +0x378, edge, 0, withEd) with withEd 1 for an edge by +0xed alone;
- **make:** a stated bond with no real edge, by the direct bond call main+0x1c05a98. Preconditions, tested each
  frame until they hold (5 s, then a failed pass): the readiness test of the direct-bond reading on both actors
  (comps, physics set, bond manager, component +0x288, the slot's body not null); body +0x68 bit 24 clear on both
  (else the edge joins no graph node, is never listed, and would be made again every second until the game's
  512-edge pool is empty); the registry lookup main+0x29a5a08 returns each body; neither body has a glue entry in
  progress; **the part count**: max(the game's group count of each side, our own count over real edges, bonds made
  this pass and open operations) of both sides together is at most 21 (at 22 the game breaks every glue edge of the
  part, which would destroy the build). The two bodies must be within 5 cm and 5 degrees of `rel`; a loose,
  unheld island of one is placed first, one frame before the call. The matrices passed: **[R2.5]** (the code
  records parts thrown apart by one choice of frames). The new edge is taken from the bond maker's hook, not
  searched for. SPR +0x20 bit 18 is set on the SPR of end A. Edge +0xef (the lasting glue look) is **not** set on a
  copy's bond in the first build: **[R2.3]** (the game removes the oldest of more than 21 such strands, per game);
- the next pass reads the edges again; nothing is assumed.

7.4 **States and the ladder.** A `Stated` here is: agreeing; out of range (not counted, not a difference);
waiting for the maker (the player has a design selected, or stands where it goes: not counted); differing.
`passes` counts only a pass that acted and whose read-back still differs; a new statement, entering range, and
the maker becoming free reset `passes` and the back-off.
- rung 1, passes 1 to 3: 7.2 and 7.3;
- rung 2, a layout difference (3.5) with agreeing digest: the bonds of the member that is off are broken and made
  again by 7.3, once; a map member is placed by one SetTransform at root * rel_root first;
- rung 3, pass 4: every run-time actor of the island here that this game does not own is deleted, whoever made
  it, and the island is made whole from the recipe. Skipped when the island has no recipe, has a map member, or
  has a member this game owns;
- then "not shown here": retried after 2 s, doubling to 30 s; state and reason go out in APPLIED and one log line.
A follower's failure never changes the owner's world.

7.5 **The owner's side.** For each island it owns and each peer in range of it: while the peer's APPLIED digest
differs from the owner's for 1 s and its state is "differing", the ISLAND is sent to that peer again (at most once a
second) and `unconverged_ms[peer]` runs. "Out of range" and "waiting for the maker" do not run it.

7.6 **Strain and the glue cap belong to the owner.** The strain check main+0x190f034 is hooked and returns at
entry for a strand whose edge (strand +0x50) has an end body in `driven_body[]`: a copy's bond never breaks by
strain here, with no byte written and no first-frame window. When this game becomes the owner the strain is set to
0 once (strand +0x1d4). **[R2.3]** for the hook's arguments and for the cap.

## 8. A glue across owners (D 3.7)

```c
typedef struct { uint64_t key; uint32_t op; Eid x, t; uint8_t sx, st; uint32_t epoch_x; float rel[12]; } BondReq;
typedef struct { uint64_t key; uint32_t op; uint8_t reason; uint64_t who; uint32_t epoch; } BondAns;  // ACK: reason 0
```
`rel` runs from X's body to T's body: inverse(X) * T.

8.1 The bond maker wrap (return address main+0x1d677a8) posts to the ring only for a non-zero edge: the two
bodies, their world matrices before the call, the edge. Phase 0 resolves the entities (4.4). Both owned here:
nothing more. Otherwise, X being this game's end and T the other owner's: an operation {op, X, T, slots, rel,
since} opens and BOND_REQ goes to T's owner. The same holds for any real edge found by 3.2 between an owned entity
and a foreign one with no operation for 0.5 s (an Autobuild pull-in, a stake).

8.2 While an operation is open here: reconcile does not run on the islands of X or T, the edge is not broken, both
islands stay asked (5.2), and this game goes on stating X's island as its own, with T as a far end.

8.3 T's owner, in arrival order (or the tick, 5.4), first looking the op up in a memory of finished operations kept
30 s (a known op gets the same answer again):
1. not T's owner: NAK(NOT_OWNER, who, epoch);
2. T unknown, DEAD or GONE: NAK(GONE);
3. an edge between those two bodies exists already, or another operation for them is open: ACK, nothing made;
4. the part count of 7.3 would pass 21: NAK(FULL);
5. X has no actor here yet: the request waits (at most 3 s, then NAK(NOT_HERE));
6. X's copy is placed (when its island here is loose) and the bond is made by 7.3's make. If it cannot be within
   3 s: NAK(POSE);
7. then, in the same pass: if this game's class for T's island is IDLE, the merged island is granted to the asker;
   if it is HOLD or RIDE, X's island is taken from the asker (GRANT to all naming this game, epoch by 5.0 with
   `epoch_x`), and at the asker 5.0 ends its hold. BOND_ACK, and the merged island's statement.

8.4 The asker: ACK closes the operation. NAK(NOT_OWNER, who): who is the asker itself: an ACK; else the request is
sent once to who. Any other NAK: the edge is broken (7.3's break), read back, and asked again for 10 frames before
it counts as failed; the reason is logged in words. The operation also closes as an ACK the moment this game owns
both ends, and as a claim of T by 5.7 when T's owner left. No answer: sent again every 1 s; after 10 s it closes
as a NAK.

8.5 Counter "a bond in one game only": a closed operation after which, 5 s later, the two games' digests of that
island still differ. One count in a test session switches the design to take-first (D 3.7).

## 9. Making a copy

9.1 **The recipe at the owner.** For an island of 2 to 21 members whose member set equals its game group's member
set (a regrouping takes a frame or two; until then no recipe), the history writer main+0x1913694 is run as
`record_pass` does. `rec_member[i]` is rebuilt outside the writer by its order rule (by name, ties by the group's
member order, less the members it skips) and accepted only when the count equals the record's part count; else the
island is stated without a recipe. **[R2.8]** (the skip test, the order after a merge or split, bonds past 21).

9.2 **The maker at a follower** runs only for an island none of whose members has an actor here. It is
`make_pass`'s three stages (the build object at ability +0x51f0 filled from the record, the part maker
main+0x1d3b470 inside `ent_creating`, the claim of each part, the confirm main+0x1d3c0d4 with no cost, the SPR
marks, green set once the part is in its group). The build object's base matrix is chosen so that the record part
of the root lands on `root_m` (**[R2.8]** gives the formula). The fields of the build object and of the ability
that belong to the player's own Autobuild are saved before and put back after (**[R2.8]** gives the list). Its
waits stay and are states, not failures (7.4). One batch at a time; islands wait in order of arrival.

## 10. Hooks and the event ring

A ring of 1024 events, lock-free append. An overflow is counted, and phase 0 then re-reads every owned entity (3).
10.1 A hand glue's bond: the wrap of 8.1.
10.2 Structure changed: the bond maker and the graph's removeEdge main+0x2995024 (already hooked; one line to
raise the event). It only marks islands for 3.2; a missed event costs at most 0.25 s.
10.3 **Deletes.** One event per actor, on an accepted request only: the hook runs the delete request
main+0x63baa8 inside itself and posts when it answered 1 (the deferring wrapper main+0x63b6b0 is not hooked for
this: its real request follows). All deletes this code asks go through one helper that sets a thread flag `ours`.
Class: **destroy** only positively: reason 0 after an accepted death request of that actor (10.4), or reason 0 from
the return addresses listed in the delete-reasons reading; reasons 1 and 2 remove an entry that never had an
actor; **everything else is an unload**, and so is every delete while LOADING. **[R2.1]** (the teardown of a load).
10.4 Death request main+0x1953184: posted when accepted, with the kind as stored after the game's rewrite.
10.5 Creates (4.2). 10.6 Grab accept main+0x1c0b83c: {spr, holder}.
10.7 Device toggle and switch event (coop_autobuild.c `coop_phase0_hook`: DEVSWITCH_UPDATE, ONOFF_CALC): the hook
clears the toggle bit or the event byte on a copy as today (a write on the actor being updated) and posts {actor,
wanted state as read then, the gauge's answer}. `events_out`'s 50 ms wait and `tog_seen` are kept.

## 11. What phase 0 publishes

Atomic arrays behind small hash filters (the pattern of coop_life.c's veto), rebuilt each frame:
- `driven[]`, `driven_body[]`, `driven_body_of[]`: live entities another game owns that are not asked. `coop_g1`,
  the force-node and motor gates, the strain hook (7.6). The group sender gate asks "is any member of this group
  in `driven[]`" by member actor, not by group pointer (a regroup gives a new group record for a frame);
- `follower[]`: every live entity another game owns, asked or not, and the batch in the making: G4;
- `copy[]`: ENT_COPY actors: behind `coop_contraption_made` and `coop_is_copy` (the glue guards);
- `nocharge[]`: entities whose payer is not this game;
- `part[]`: members of islands of two or more: the stand-in veto, as today (stand-ins go on touching loose things);
- `ent[]`: every entity's actor: `coop_ent_is(actor)`, which the object mirror asks at each of its entry points
  (`prop_fn`, `report_props`, `arrow_copies`, `props_receive`, `destroy_watch`, `ride_objects`) to leave entities
  alone.
The names the other files call are kept and re-pointed (section 14 lists every one).

## 12. Drive, devices, logs

12.1 **Drive.** `follow_rigid`, `dev_follow` and `floats_follow` stay on the tick, at the cadence they were played
at, working from the snapshot (member actors, root samples, clock offset, masks). The rest band stays 0.10 m and
4 degrees for every island. A member behind a body slot other than 0 (3.5) is not written. `springs_phase0`,
`burning_phase0`, `dev_flush`, `deaths_put`, `events_put`, `rider_weight` stay at phase 0. All per-part masks are
indexed by the member's place in ascending Eid and used only under an agreeing digest (6.1). No sample for 1 s:
nothing is written. An error above 10 m: one SetTransform of the island, then the drive.

12.2 **Riders.** The player state's `ride_owner` / `ride_id` name an entity as (coop_ctr_route(inc), serial), with
0xfd for a map key and 0xfc for a minter that left (then the serial alone is searched).

12.3 **Logs.** Every line starts with wall-clock milliseconds. An Eid prints as the low 16 bits of inc, a dot, the
serial. At start: the settings. Every 10 s and on change: `ent: owned N (M islands) | copies N (M) | out of range K
| waiting K | not shown K | unconverged <peer> <root> <s> ...`. One line with Eids for every ownership change,
statement, reconcile action, operation and its answer, refusal, and counter.

## 13. Test settings (they stay in the code)
TOTK_COOP_ENT_DROP=5 drops every fifth reliable message; TOTK_COOP_ENT_BREAK=1 breaks one edge of a copy island
every 10 s; TOTK_COOP_ENT_DELAY=2000 holds one reliable message in ten back for that many ms.

## 14. What leaves the build, and what is re-pointed

Removed from coop_ctn.c: `reg[]`, `Ent`, cids, versions, `record_pass` but for the recipe (9.1), `in_place`,
`bond_queue`, `bond_pool_recycle`, orphans, `made_parts` and adopt / original, `abandon`, `parts_delete`,
`edits_pass`, `coop_ctn_glue_made`, `take_over`, `authority`, `retire_pass`, `wanted_*`, `receive`, `send_states`,
the lone-device code, `objs_*`, `clean_line`, `cptoggle_test`, `coop_ctn_first`. Kept and moved: `part_edges`,
`make_pass`'s stages, `follow_rigid`, `coop_g1`, `dev_*` and `setq`, the on/off and fuse node tables,
`gauge_blocked`, `floats_follow`, the spring helpers, `burning_phase0`, `deaths_put`, `events_put`, `events_out`,
`rider_weight`, `delete_radius`, `part_delete`, `died[]`, `ctn_on`.
Exported names kept and answered from section 11: coop_ctn_driven, coop_ctn_no_charge, coop_ctn_follows,
coop_contraption_follower, coop_ctn_part, coop_ctn_sender_entry, coop_contraption_made, coop_ctn_core, coop_g1,
coop_g1_counts, coop_held_here, coop_ctn_structure_event, coop_ctn_death_seen, coop_ctn_note_onoff,
coop_ctn_event_seen, coop_ctn_gauge_seen, coop_ctn_toggle_asked, coop_ctn_note_fuse, coop_ctn_emitter_of_followed
(without the lone list), coop_ctn_runs_part and coop_ctn_parts_all (the riders). Removed with their callers'
branches: coop_contraption_ident / _find / _owns.
coop_players.c: the mirror keeps all its code and skips entities (11); `obj_take_over`, `obj_handover_apply`,
`take_q` and the OBJS header word 2 go; `coop_obj_ident` / `coop_obj_find` lose their contraption branch.
coop_glue.c and coop_bonds.c leave the build with: `coop_glue_hook` and its two hooked addresses,
`coop_glue_tick`, `coop_glue_take`, `coop_bonds_tick`, `coop_bonds_take` (coop_world.c peer_tick), the test callers
in coop_autobuild.c, and the four `coop_bonds_*` readers in coop_players.c. coop_bonds.c has another session's
uncommitted change: that session is asked first. TOTK_COOP_GLUE, _BONDS, _DEVICE_SYNC go.
cpu.h: main+0xa1bc30, main+0x190f034, main+0x502e60, main+0x1a45ba0 are added to the hook list (one full
regeneration of the generated C).

## 15. Pass tests (D 4.4), by piece

| Piece | Passes when |
|---|---|
| Identity | Three fans from the pouch: three Eids, equal in both logs, unchanged through two hand-overs and a glue |
| Ownership | Twenty grabs back and forth of one fan: one Eid, epoch 21; both grabbing: one REFUSE(HELD), one hand lets go |
| Lost messages | ENT_DROP=5 through the whole script: no island without an owner for more than 5 s; "unconverged" under 3 s |
| Split and merge | A build pulled in two, each half then held by a different player: two islands in both games within 1 s |
| Glue across owners | Ten trials of each case of D 4.4, also with the owner in a menu: the counter of 8.5 stays 0 |
| Copy making | A 3-part build seen by the other game within 2 s of its last glue; an 8-part Autobuild likewise |
| The player's Autobuild | After a copy was made here, this player's own history and selected design are as before |
| Drive | A loose fan pushed by its owner: under 0.10 m at rest in the other game, no teleport line, no chatter on a slope |
| Lifetime | Walk 200 m away and back: OFFER, GRANT, the copy made again on return with no back-off wait |
| Menu | The owner in a menu for 10 s: the other player grabs, glues and toggles, each within 0.5 s |

## 16. The readings

First round (2026-11-12, R/sync-v4/, facts section 49): hold-end, edge-removal, direct-bond, create-callers,
device-origin, delete-reasons, grab-refuse. Second round, asked for by the review:

| # | Folder | Decides |
|---|---|---|
| R2.1 | load-teardown | 1.2 the load flag and its order against the deletes; 10.3; whether phase 0 runs in menus and loads |
| R2.2 | create-to-actor | 4.2: from a game-made create to its actor; from the capsule call to its Player; the callers of the second create |
| R2.3 | strand-cap | 7.6 the strain hook; 7.3 whether a copy's bond may carry the glue look; the cap of 21 |
| R2.4 | device-bodies | 3.2 body slots; 3.5 and 12.1 members behind a moving body |
| R2.5 | bond-frames | 3.2 `rel` from an edge; 7.3 the matrices of the direct call |
| R2.6 | seat-and-refusal | 5.5 the addressing of the hold-end message; 5.6 the refused grab and the seat |
| R2.8 | maker-side-effects | 9.1 the recipe's member order; 9.2 placing the batch and restoring the player's Autobuild |

Not read, by decision, each with its rule above: a dispenser's create and drops (4.2's guarded rule, counted);
what colours the aim highlight; who writes the devices' created-by-player flag; the physics step against phase 0
(the runs so far worked; counted). Settled by the first test session's log: map keys by placement hash (planks,
rocks, crates, map devices, stacked identical objects); how often the direct bond holds (per attempt: made, read
back, still there after 5 s); the counters of 4.2.
