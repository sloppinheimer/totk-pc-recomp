# Contraption sync v4: the specification

Status, 2026-11-13: implemented and built, not run. Section 18 says where the code differs from the text of
sections 0 to 17 (the fourth draft, written before the code); section 19 is the first session's tests. It turns
[contraption-sync-v4.md](contraption-sync-v4.md)
(the design, D below) into data, messages and procedures for one new file, runtime/coop_ent.c. Where this file and D
disagree, D is corrected first.

History: the first draft was reviewed by three readers that did not write it (against the code, against the game
facts, by walking play scenarios by hand; R/sync-v4/spec-review/), two rounds of game-code readings followed
([contraption-game-facts.md](contraption-game-facts.md) sections 49 and 50, cited as F), and the second draft was
reviewed the same way (R/sync-v4/spec-review2/, with triage.md there). A third, narrower review walked the third
draft by hand and checked it against itself (R/sync-v4/spec-review3/). Every finding of the three reviews is in this
draft, and so are the cuts the third review proposed: where two mechanisms did one job, one is gone. The reviews
found the ownership rules of section 5 sound under every interleaving tried, and no finding that needs a new
reading of the game.

Conventions. Addresses are TotK 1.4.3, main+offset. "The world" is the game's memory; "the table" is ours. An
incarnation is a game's 64-bit id for this session (`coop_ctr_me`). **Every timer is wall-clock time**; only POSE.us
and the drive use the world clock. A statement marked [inf] rests on an inference of a reading, [test] is settled
by the first test session's log (section 17). Functions named without a file are in runtime/coop_ctn.c today.
"Here" means in this game. "This Link" is `coop_player_actor()`, never "a Player actor": the other players'
stand-ins are Player actors too.

## 0. Scope of the first build

An **entity** is one of: a Zonai device (actor name `SpObj_*`, not a capsule), a part an Autobuild made, or any other
actor that is an end of a glue (4.4). The set is frozen
at entry: an entity stays one for the session.

Not entities, left to the object mirror in coop_players.c as today: capsules (`SpObj_*_Capsule_*`: pickable items,
with the mirror's pick-up notice), items, weapons, shields, materials and fused gear, arrows, beams, the paraglider.
One of those that a player glues leaves the mirror and becomes an entity by 4.4's hand-over. A bare Ultrahand grab
of one stays the mirror's business, with the mirror's own grab hand-over (`obj_take_over`, `obj_handover_apply`,
`take_q`), which is kept as it is: it has been played, and it keeps the hand-over of 4.4 to the moment of a glue.

Two things of the earlier drafts are **not** in the first build, by decision: a grab of a thing its owner holds is
not refused before it starts (it is accepted and ended at once by 5.5, whose every step is read; the native refusal
left the Ultrahand part in a state whose exit is not read); and the maker is not given part of a record. Cut after the
third review, each because another rule already does
its job: a message that states a belief about ownership (owner and epoch travel in every POSE, statement and
GRANT, and a stale asker is corrected by its REFUSE); the paused game's answers to bond and toggle requests (the
asker claims after 1 s, 5.7); taking an offered island by request and digest (it is claimed, 5.8); the three-step
ladder when a player leaves; a check of a bond between two owners (two owners last one round trip).

## 1. Threads

- **Phase 0** (`coop_ent_phase0`, the hook on main+0xa72b00, once a frame outside the parallel actor update): all
  walks of the world's structures, all guest calls but the drive's, all changes of the world, and the table.
- **The net tick** (`coop_ent_net`, called from coop_spawn.c `coop_tick` beside `coop_ctr_tick`; about 30 a second,
  also in menus, loads and with no Player): the heartbeat flags (1.2), the pause queue (1.3), repeats that only
  send (GRANT, OFFER_ALL), and the answer to a TAKE (5.4). It takes `ent_lock` for the queue and the answer.
- **The drive tick** (`coop_ent_drive`, called where `coop_ctn_tick` is called today, so with today's gates: a
  resident Player, a world role, a running world clock): `follow_rigid`, `dev_follow`, `floats_follow`, from the
  snapshot (12.1). Its guest calls are the one exception to "guest calls at phase 0".
- **Hooks** on any game thread: read the published sets of section 11, append to the event ring (10), and make the
  few writes named in 10 on the actor being updated. A hook never takes `ent_lock` and never walks the table.

`ent_lock` guards the table. Phase 0 holds it for its pass, guest calls included, so every other taker waits with
coop_spawn.c `coop_lock_wait` (the holder can give up its core inside a guest call), never `plat_mutex_lock`.

### 1.1 The order inside phase 0
1. the pause queue, then `coop_ctr_recv` until empty, in arrival order (6): table changes at once, world changes
   noted on the entry; then the latest-only slots of POSE, APPLIED and INDEX (the transport's listing call): POSE
   through `apply_owner`, the newest pose, the class and the OFFER flag; APPLIED into `Island.peer[]`; INDEX by 6.11;
2. the event ring (10);
3. the caches: this Link, the actor its Ultrahand holds, the stick it rides; each entity's liveness and main-body
   matrix (one read each; `main_body` cached per entity against `gen`);
4. creates that completed (4.1), the maker's stage (9.2);
5. edges, islands, digests and class of marked and due islands (3);
6. timers whose end changes the world or a request (5.2 expiry, 5.7, 5.8, 8.4, 7.4 back-off): never on a pass in
   which a message for that request or operation was read in step 1;
7. this game's own causes: TAKE, bond operations (5.2, 8);
8. statements owed (6.2); then POSE, APPLIED, INDEX and NEED (a paused game sends none of these);
9. reconcile (7), then the kept phase-0 functions (12.1);
10. the published sets and the drive's snapshot (11).

### 1.2 Paused and loading
Set by the net tick through `coop_ctr_hb_set` (nothing sets LOADING today, and today's PAUSED follows a clock that
runs in menus):
- PAUSED: phase 0 has not run for 0.3 s. True by construction, whether or not the game runs its calc in menus.
- LOADING: the game's teardown flag, [[main+0x395e008]] +0xe1 bit 1 (set when the delete-all-actors step starts,
  before its first delete, cleared after its last; F 50.1), or no resident Player actor. Which kinds of load run
  that step is [test].

### 1.3 The pause queue
The transport has one inbox of 256 for all types with no way to look without taking, and it stalls every reliable
stream when full. So while PAUSED or LOADING the net tick takes every message out of `coop_ctr_recv` into `pq`
(1024 entries of {from, type, len, payload}, order kept), and from it, in order:
- applies GRANT and REFUSE to the table through `apply_owner` and the request states only (no world change; a hold
  or ride to end is noted on the entry for phase 0);
- answers TAKE by 5.4 (not while LOADING);
- leaves every other message in `pq` for phase 0, which reads `pq` first. They block nothing.
When `pq` is full the tick stops taking (the transport stalls, as it would have), and logs once.

## 2. Data

Sizes: ENT_N 768, ISL_N 768, BOND_N 1024, STATED_N 128, REQ_N 64, OP_N 16, SERVE_N 8, ASK_N 16, ANS_N 64, PEND_N 64,
SNAP_N 256, EV_N 1024. Per-peer arrays are indexed by relay id - 1. Every
array of members is 21 wide (the game's group limit; a glue is refused at 22). A full table: the action that needed
the slot is not done, a counter rises, one log line per 10 s names the table. Nothing is evicted silently, except
as 4.7 and 6.2 say.

```c
typedef struct { uint64_t inc; uint32_t serial; } Eid;   // inc 0: a map actor, serial = its 31-bit key (4.3).
                                                         // Hashed and compared field by field, never as bytes.
enum { LIFE_LIVE, LIFE_DEAD, LIFE_GONE, LIFE_FREE };     // FREE: a map entity nobody owns
enum { CLS_IDLE, CLS_HOLD, CLS_RIDE };
enum { ENT_GREEN = 1, ENT_MAP = 2, ENT_PREVIEW = 4, ENT_COPY = 8 };   // only GREEN is sent

typedef struct {
    Eid id;
    uint64_t owner; uint32_t epoch;      // changed by apply_owner alone (5.0)
    uint8_t life, death_kind, flags;     // death_kind 0xff: a plain delete
    char name[48]; float scale[3], radius;
    uint64_t actor; uint32_t gen;        // this game's actor (0: not here) and actor +0x10 when taken
    uint64_t main_body;                  // cached, valid while gen matches
    uint8_t handle[0x40], link[0x40]; uint8_t making;   // our own create of a copy (4.1)
    uint8_t mir_owner; uint32_t mir_id;  // the object mirror's name for it at entry (4.4), 0: none
    uint8_t end_hold, end_ride, death_due;   // world changes noted by the tick or by step 1, done at phase 0
    int16_t isl;                         // owned: index of its Island; foreign in a build: index of its Stated; else -1
        // a foreign island of one keeps here what a Stated keeps for a build:
    float root_m[12]; int64_t pose_ms;   // the newest pose and when (6.1)
    uint8_t state, why, passes; int64_t backoff_until, backoff; uint32_t digest;   // 7.4
    uint8_t cls, offer; int64_t cls_ms;  // the owner's class and OFFER flag from POSE
    uint8_t named;                       // an ISLAND, GRANT or POSE named it since the last NEED(0, 0) to its owner
    int64_t stated_ms, dead_ms;
} Ent;

typedef struct { Eid a, b; uint8_t sa, sb; float rel[12]; uint64_t edge; } Bond;
// a < b by (inc, serial). sa, sb: body keys. rel = inverse(Wa) * Wb: b's body in the frame of a's body (3.2).
// edge: this world's edge, local. BOND_N entries, sorted; each (a, b, sa, sb) once.

typedef struct {            // an island this game owns (rebuilt by 3.3, matched to last frame's by its members)
    Eid root; uint16_t n; int16_t m[21];          // Ent indexes ascending by Eid
    uint32_t digest, epoch; uint8_t cls, has_far;   // has_far: a bond to a foreign entity
    uint32_t stated_digest; uint8_t owed[8];      // peers (by relay id) owed a statement
    struct { uint32_t digest; uint8_t state, why; int64_t ms, diff_since; } peer[8];   // their APPLIED
    int64_t moved_ms, class_ms; uint8_t class_sends;
    uint8_t frozen; int64_t offer_ms;              // 5.8
    uint8_t rec[BP_SIZE], rec_n; Eid rec_member[21]; uint32_t rec_digest;   // the recipe (9.1) and what it is of
} Island;

typedef struct {            // a statement of two or more members, or of one member with a bond, as last received,
                            // per (sender, root)
    uint64_t from; Eid root; uint32_t counter, digest; uint16_t n, nb, nf;
    uint8_t cls, offer; int64_t cls_ms;   // the owner's class and OFFER flag from POSE
    struct { Eid id; uint8_t life, death_kind, green; float rel_root[12], radius; } m[21];
    Bond bond[48];
    struct { Eid id; uint64_t owner; uint32_t epoch; } far[21];   // far ends of bonds: not members
    float root_m[12]; int64_t pose_ms;  // newest root pose: from the statement, then from every POSE (6.1)
    uint8_t rec[BP_SIZE], rec_n; Eid rec_member[21];   // a make copies these when it starts
    int64_t ms; uint8_t state, why, passes, rung2_done; int64_t backoff_until, backoff;   // 7.4
} Stated;
enum { ST_AGREE, ST_FAR, ST_WAIT_MAKER, ST_MAKING, ST_DIFFER, ST_NOT_SHOWN };
enum { WHY_NONE, WHY_CREATE_FAILED, WHY_BOND_NOT_READY, WHY_BOND_POSE, WHY_FULL22, WHY_NO_RECIPE, WHY_MAP_MEMBER,
       WHY_OWN_MEMBER, WHY_MAKER_FAILED, WHY_TABLE_FULL };

typedef struct {            // a TAKE this game sent (5.2). id: a counter of this game, never 0
    uint32_t id; uint8_t state, reason; Eid member; uint64_t to; int64_t opened, sent, closed;
} Req;
enum { RQ_OPEN, RQ_GRANTED, RQ_REFUSED, RQ_EXPIRED };

typedef struct {            // a bond operation this game opened (8). op: a counter of this game, never 0
    uint32_t op; uint8_t state; Eid x, t; uint8_t sx, st; float rel[12];
    uint64_t edge, to; int64_t opened, sent; uint8_t forwards, break_tries;
} Op;
enum { OP_WAIT_GRANT_X, OP_SENT, OP_BREAKING };   // WAIT_GRANT_X: X is held under an open request, nothing sent yet

typedef struct {            // a BOND_REQ this game is serving as T's owner (8.3 rules 4 and 5). SERVE_N
    uint64_t from; uint32_t op; Eid x, t; uint8_t sx, st; uint32_t epoch_x; float rel[12]; int64_t since;
} Serve;

typedef struct { uint32_t id; Eid e; uint8_t kind; uint32_t value; int64_t opened, sent; } Ask;   // a REQ sent (6.9)

typedef struct {            // the owner's memory of an answer, stored as fields and built again when sent
    uint64_t from; uint32_t id; uint8_t kind, type, reason, send, n; int64_t ms, until;
    uint64_t owner; uint32_t epoch; Eid m[21];
} Ans;
// kind TAKE (10 s), BOND (30 s: its GRANT, n may be 0, and its ACK or NAK), REQ (10 s). send with `until`: the
// GRANT repeat of 5.3. ANS_N, oldest overwritten.

typedef struct {            // per peer (relay id - 1)
    uint32_t sent_counter, taken_counter;   // statements sent to it; the highest counter taken from it
    uint8_t index_miss; int64_t need_ms; uint32_t need_number;   // 6.11
} Peer;

typedef struct { _Atomic uint64_t r; uint8_t decision; uint8_t batch; } Pend;   // 4.2: pool record -> decision
enum { DEC_OURS = 1, DEC_MINT, DEC_MINT_PREVIEW, DEC_ASK_MIRROR };   // MAP and NONE keep no row

typedef struct {            // the ring event (10): seq written last, so the reader sees whole events only
    _Atomic uint32_t seq; uint8_t kind; uint32_t gen, reason; uint64_t actor, a2, lr; float m[2][12]; uint8_t flag;
} Ev;

typedef struct {            // one row of the drive's snapshot per foreign island here (12.1)
    Eid root; uint64_t owner; uint8_t n, agree; uint64_t actor[21]; uint32_t gen[21]; uint32_t no_write;   // bit r
    float rel_root[21][12]; uint8_t teleport;   // teleport: set by the drive tick, done and cleared by phase 0
    int64_t stamp_ms;
} Snap;
```

## 3. Reading the world (phase 0, step 5)

Due: every frame for an island that is held, ridden, asked, has an open operation, or moved in the last second;
every 0.25 s otherwise; at once for an island a ring event marked. An entity whose SPR has no group (SPR +0x28 is 0)
has no edges and is not passed through 3.2.

3.1 **Liveness.** An entity's actor is alive when actor +0x10 is still `gen` and its state (+0x24) is 2 to 6. An
owned entity that stopped being alive takes its life from its delete event (10.3). With no event (a ring overflow):
GONE if the load flag was set at any time since the last pass, else it is read again next pass and then GONE.

3.2 **Bonds.** For each live entity here that has a group: its bodies from the part's body list ([comps +0x288]
+0x20: count +0x58, array +0x60, stride 0x88); a body's key from the one-hot bits 1 to 4 of the byte at body +0x1e8
(keys 0 to 3, written by main+0xab69fc), or, with none set, by asking main+0x44bba4 for keys 4 to 6. Key 0 is the
main body. That the keys are equal in two games is [inf] (they come from the device's static data).
main+0x27c1c0 lists a body's edges. An edge is a bond when edge +0x10 bit 3 is set and bit 2 clear, and +0xec or
+0xed is set. The other end's actor is looked up by pointer; one that is no entity is entered by 4.4.
The held pose comes from the bond's own rest frames: main+0x7a4410(edge, outA, outB) gives each end's frame in its
body's space, and held = outA * inverse(outB) is end B's body in end A's body frame. With entity `a` (the smaller
Eid) at end A: rel = held, sa and sb the two keys. With `a` at end B: rel = inverse(held), and the keys swapped.
Two edges for one (a, b, sa, sb): listed once, and the surplus is logged.

3.3 **Islands.** Union-find over the bond set, over all entities here. An **owned island** is the owned members of
a connected set, with `has_far` when a bond leads to a foreign entity. All owned members of a set carry one epoch:
when two of this game's islands merge, all take the greater. An `Island` row is matched to last frame's by its
greatest overlap of members, so `peer[]`, `owed[]` and the frozen state follow it through merges and splits.

3.4 **Digest** (each game from its own world): FNV-1a 32 over, for each member ascending: inc, serial, life; then
for each bond ascending by (a, b, sa, sb): those six values. For an owned island the members are its owned
members and the bonds are every bond with at least one end among them (so a bond to a far end is in it). A follower
computes the digest of a statement's island over the statement's members and the bonds that have an end among
them. Owner, epoch and poses are not in it.

3.5 **Layout.** For a statement whose digest agrees here: each member's main-body pose relative to the root
against `rel_root`; more than 0.3 m or 15 degrees for 1 s is a layout difference (7.4 rung 2). A member whose chain
of bonds to the root passes a body key other than 0 (a part glued to a spring's piston or a wheel's turning body;
a glue lands on the nearest body of the other part, F 50.4) is neither checked nor driven rigidly (12.1). The
devices with more than one body: small wheel, big wheel, spring, stake, construct head, portable pot, mirror; their
further bodies follow their joints, so only main bodies are driven. 

3.6 **Class**, from this Link's side, once a frame: the island of the actor this Link's Ultrahand holds (the entity
whose SPR +0x20 bit 16 is set with the holder this Link; found once, not per member) is HOLD; the island of the
stick this Link rides (`stick_rider`) is RIDE; every other owned island IDLE.

## 4. Identity

4.1 **A copy this code creates.** The entry exists first, ENT_COPY, with `handle` and `link`; the create is a
variant of coop_game.c `request_create` that takes a scale (and `coop_request_create_autobuilt` for a green
member). A thread flag `ent_creating` is set inside `request_create` and around the maker's guest call
main+0x1d3b470. At status 3 (phase 0 step 4) the actor is claimed (`coop_claim`) and bound. Status 4, or a request
answered 0 (every create is refused while the load flag is set): the handle is released, the pass counts as failed
with WHY_CREATE_FAILED, and it is not asked again before the back-off (a create asked every tick has taken the actor
manager down before).

4.2 **A run-time actor the game makes here.** The create hook on main+0x599670 runs the request inside itself
(the wrap coop_life.c uses for the bond maker: a thread-local `inner`, tested first in the hook, then
`rt_dispatch`; arguments copied before). If the request answered 0 or x3 is 0, nothing more. Else R = [x3], the
pool record, and the hook decides in the order below. It stores (R, decision)
in `pend[]` (atomic) for OURS, MINT, MINT_PREVIEW and ASK_MIRROR, and for MAP and NONE it clears any row for that
R, so a reused R never carries an old decision:
1. `ent_creating` is set: OURS (bound by 4.1; if inside a maker run, the batch number);
2. x5 (the placement record) is not 0: MAP;
3. the return address is main+0x1d3bb98 (the Autobuild part maker) and the name is `SpObj_*` or any part name:
   MINT_PREVIEW, with the batch number of this maker run;
4. the name is not `SpObj_*`, or is a capsule: NONE;
5. the return address is main+0x1b7f8c8 (a capsule opening into its device): the Player is [(x3 - 0x3e88) +0x18];
   this Link: MINT; another Player: NONE and a counter (whether a stand-in opens capsules is [test]);
6. any other `SpObj_*` create (a device an AI node or another creator makes): ASK_MIRROR.
The hook on main+0x54cb00(actor), the function in which the game marks an actor created, runs the original first
and then, when R = [actor +0x1b8] is not 0, its status is 3 and [R +8] is the actor, looks R up in `pend[]`,
clears the row and posts {actor, gen, decision, batch} to the ring. (After that pass the game frees R; it is never
read later.) Its branch sits at the top of `coop_hook`: how often the game calls it is not read.
At the ring (phase 0): MINT and MINT_PREVIEW enter the actor (inc = this game, serial = next; owner this game,
epoch 1). ASK_MIRROR is minted only if the object mirror would have taken it as made here: coop_players.c exports
one tick-side answer, `coop_obj_made_here(actor)` (the guards of `prop_fn` / `report_props` as they are: nearer
this Link than any stand-in or unplaced, within 6 m at first sight, no copy pending near, at most 20 new at once,
not within the quiet time after a warp), asked at phase 0 and read one tick later; counted, with the return address
in the log. A creator outside both hooks (the family at main+0xb06b28, F) is caught the same way: an `SpObj_*` actor
that 4.4 or the mirror meets and that has no entry is ASK_MIRROR.
The synchronous create main+0xa1bc30 (placement record in x3; returns the actor) gets an entry hook that only logs
an `SpObj_*` name: none of its callers read makes a device.
Nothing is minted with no session; the actor is entered when the session starts. When this game's incarnation
changes, every entity it owned is re-owned under the new one at epoch + 1 and stated.

4.3 **A map actor.** From the map: P = [actor +0x370] has (P +0x14 & 8) set and a hash at P +0x88 that is neither
0 nor all ones. Eid = (0, key), ENT_MAP, key 31 bits, **computed once at entry and stored**: the key the mirror
already holds for the actor (`props[].key`, `key_found[].key`) if it has one, else `coop_actor_world_key(actor)`;
after the first test session shows equal placement hashes in both games [test], FNV-1a of (placement hash, name).
A statement that names a map key this game has no actor for is resolved by the mirror's resolver, as an
ask-and-answer pair: phase 0 lists the missing keys, the tick resolves all of them in one scan a second
(`coop_obj_find`'s scan and cache) and answers. Two actors for one key are logged. A map entity is never created
and never deleted by this code. A FREE map entity keeps its row, so its epoch never restarts.

4.4 **An actor that is no entity is entered when it is an end of a glue**: an end of a real glue edge of an entity
(3.2), or of a glue made here (10.1). Nothing is entered by a grab, a scan, a distance or a time.
- **The mirror is asked first.** Phase 0 asks the object mirror through one ask-and-answer pair (the tick answers a
  tick later, as for `coop_obj_made_here`): `coop_obj_ident` for an actor being entered (the relay id of its mirror
  owner and its number, or the keyed flag and key), `coop_obj_find` for a pair received. **An entity being entered
  is not stated, and no operation is opened for it, until the mirror has answered.**
- **Its Eid.** A map actor: 4.3. An actor the mirror knows by (owner, number): Eid = (that owner's incarnation,
  0x80000000 | number), in every game, so two games that enter one thing at the same moment mint one entity; both
  claim it at epoch 1 and 5.0 picks one (the loser's hold is ended). Any other actor: inc = this game, the next
  serial. Serials a game mints itself stay below 0x80000000.
- **Leaving the mirror.** The pair goes into `mir_owner` / `mir_id` and travels in the member row (6.2). The
  tick-side call `coop_obj_entity(actor)` frees every mirror row of the actor (`props[]`, `mir[]`, `key_found[]`,
  `dwatch[]`) with no notice and no delete, un-freezes a frozen copy, and notes the pair in `taken[]` (with today's
  cap), so the mirror neither deletes a copy its sender stopped listing nor makes it again while its old owner
  still lists it.
- **At a receiver** a member row with a mirror pair binds the actor the mirror gives for that pair (ENT_COPY when it
  is the mirror's copy there, an original otherwise) and calls `coop_obj_entity` for it; a copy is created only
  when the pair resolves to nothing within 1 s. The mirror's pick-up and destroy notices for a pair that has become
  an entity are dropped at their receiver.
- **An entity that can be picked up** (a weapon, shield or material that was glued and pulled off again): at a
  follower the pick-up is refused, as G4 refuses every delete of a follower's actor; at its owner, any delete with
  reason 0 is a destroy (10.3). The pick-up's own delete is not read [test].

4.5 **Lookup.** By Eid and by actor pointer (checked against `gen`): two open-address hashes of 2048.

4.6 **An Autobuild's preview.** ENT_PREVIEW is cleared for all parts of one maker run (one batch) together, when
SPR +0x20 bit 51 is clear on all, every SPR's queued bond count (+0x2b0) is 0, and 3.2 has listed their edges once
(or 1 s has passed). A PREVIEW entity is in no statement; its delete (a cancelled hologram) removes the entry and
sends nothing. An existing entity that the Autobuild pulls in keeps its statement.

4.7 **Removal.** A DEAD or GONE run-time entity is stated for 10 s by its owner, then dropped there; a follower
drops it 10 s after the last statement of it. A LIVE or FREE entry with epoch above 0 is never dropped.

## 5. Ownership

5.0 **One rule.** `apply_owner(e, owner, epoch)` is the only code that changes an entry's owner or epoch from a
message; it takes the pair only if greater than the entry's by (epoch, then incarnation). GRANT, REFUSE, ISLAND
rows (members and far ends) and POSE all go through it; a GRANT is taken through it whatever the state of the
request it names. When it takes from this game an entity whose island
this Link holds or rides, `end_hold` or `end_ride` is noted and done at phase 0 (5.5, 5.6). After it, a `Stated`
none of whose members its sender owns any more is freed. Every GRANT and claim uses 1 + the greatest epoch of
every member involved, as the granter knows them.

5.1 **Claim of a FREE entity**: owner this game, epoch + 1, stated at once. Two claimers differ in incarnation,
so 5.0 gives one winner in both.

5.2 **TAKE.** Sent when this Link grabs a member of an island this game does not own (grab accept) or takes its
stick. It names the member touched and carries `epoch_seen`, the asker's digest of that member's island, and a bit
"layout agrees" (3.5). A `Req` is OPEN while the cause lasts, at most 1.5 s per attempt: the island is **asked**
(out of the drive, gates open, as `touched` today; "asked" is a property of the request's member and follows the
island that member is in), and TAKE is repeated every 0.3 s to the owner as now known. Expiry closes it (5.5); for
a ride a new request opens after 1 s while `stick_rider` is still this Link. A glue carries its take in the bond
request (8), and its islands stay asked while the operation is open.

5.3 **The owner's answer**, in arrival order. It first looks (asker, id) up in `ans[]` and gives a remembered
answer again without applying anything.
1. not the owner: REFUSE(NOT_OWNER, the owner and epoch it knows);
2. `epoch_seen` below the entity's epoch: REFUSE(STALE), and the statement is owed to the asker;
3. this game's class for the island is HOLD or RIDE: REFUSE(HELD or RIDDEN);
4. the island is frozen (5.8): no answer (it is claimed, not taken);
5. the asker's digest differs from this game's, or "layout agrees" is clear: REFUSE(STALE), statement owed;
6. GRANT to all (6.4); `apply_owner` here. The giver repeats the GRANT every 0.5 s (an `Ans` with `send`; the net
   tick sends it too) until it has a POSE or ISLAND from the new owner at that epoch or higher, at most 5 s.

5.4 **While PAUSED** the net tick answers a TAKE from the pause queue (1.3), under `ent_lock`, by the whole of 5.3,
from what phase 0 last stored per owned `Island` (digest, class, members, epoch). The stored class protects a
thing in the paused player's hand. An entity this game came to own while PAUSED has no stored island: no answer.
Not while LOADING. A bond request or a toggle request to a paused owner is not answered at all: its asker claims
after 1 s (5.7).

5.5 **At the asker.** A GRANT or REFUSE is taken only by an OPEN request (a REFUSE for a granted or expired one is
dropped; a GRANT for an expired one is taken as ownership, class IDLE, through 5.0). REFUSE(NOT_OWNER, who): who is
this game at a greater epoch: a GRANT; who is another game: the same request is sent once to who and stays open.
On any other REFUSE, or on expiry, the island goes back under the drive, and a hold on it is ended the game's own
way: main+0x2b76c34(held actor +0x378, {Link +0x380, 0x20510006, 0}, 0), the held actor being the one found by 3.6
(word 0 is the receiver, the channel's actor the sender, which is what the Ultrahand part compares with the actor
it holds; F 50.6). The part runs its own end, sends the let-go, and the SPR clears holder and bit 16. Read back:
still held 10 frames later, the message is sent again, three times at most, then logged (the actor message ring
drops silently when full). The SPR side alone is never cleared. A grab of a member whose owner's class is known as
HOLD or RIDE (POSE under 2 s old) is ended the same way at the next phase 0 without a TAKE.

5.6 **A stick.** The hook at the entry of the seat claim main+0x502e60(rider component, stick handle, mode, seat)
answers 0 when the rider's actor ([x0 +0x18]) is this Link and the handle's actor is in `refuse_stick[]` (11).
Every refusal of that function returns before any write (F 50.6); a ride started by the event node keeps that
node's preamble, which is logged and [test]. A ride that started and is refused or lost: the Link is taken off
with main+0xc448c8([[Link +0x228] +0x410], 1, 1). What the game does to a rider whose stick is destroyed is not
read.

5.7 **Claims without a grant** (owner this game, epoch by 5.0, stated at once). The claimer's world becomes the
truth; two claimers are settled by 5.0, and the loser's copy is reconciled.
- the owner left the session: a game with an actor of the island claims, at once on an exact cause of 5.2 or a
  glue, else after 2 s;
- the owner's heartbeat says LOADING, or PAUSED with no answer for 1 s to a TAKE, a BOND_REQ or a REQ of this
  game: this game claims the island asked for. For a BOND_REQ the operation closes as an ACK; for a REQ the asker
  applies its own value as the owner. **Not** an island whose owner's last class was HOLD or RIDE: a class does not
  age (6.1) while its owner's heartbeat says PAUSED;
- an offered island (5.8).

5.8 **Unload at the owner.** An accepted delete classed "unload" (10.3) on an owned entity: its `Island` is frozen
(nothing but POSE with POSE_OFFER is sent for it) for 2 s. Every game that has an actor of an offered island claims
it. With no claim in 2 s: **that member** is stated GONE (a map entity: FREE); members still alive stay owned and
are stated as the island they now are. While LOADING here nothing is frozen per island: the net tick sends one
OFFER_ALL when it sets LOADING, each other game claims every entity of that sender it has an actor for, and what
nobody claimed is stated GONE as its delete is accepted. A follower's own unload of a copy is local: actor 0, and
the statement is ST_FAR.

## 6. Messages

All through runtime/coop_ctr.c; new types 40 to 59. The v3 types (1, 3, 10 to 35) go with their code; an unknown
type is dropped. Control messages are reliable and in arrival order. Every reliable message is sent per peer (a send
to all reports
success when any one peer took it), and what one peer's queue refused is owed to that peer, a DEATH and an OFFER
too. ISLAND pieces and INDEX_END travel on the bulk
stream; nothing relies on order between the two streams. A reliable message is not kept by us when the transport's
queue is full (192 per peer and stream): what was to be said is marked owed (`Island.owed[]`, an open `Req` or `Op`
with `sent` 0, an `Ans` with its send flag) and said when a send succeeds. Every unreliable message starts with an
8-byte slot key, since the transport keeps the latest per (sender, type, first 8 bytes).

```c
typedef struct { uint64_t key; Eid root; uint32_t epoch, digest; int64_t us; uint64_t payer; float m[12];
                 uint8_t cls, n, flags, pad; uint32_t sw, on, fw, fk, fb, fp, burn; float pf[][4]; } PoseMsg;   // 40
typedef struct { uint32_t counter; Eid root; uint16_t n, nb, nf; uint32_t digest; uint8_t rec_n;
                 float root_m[12]; uint32_t total; uint16_t piece, pieces; } IslandHdr;                         // 41
typedef struct { Eid id; uint64_t owner; uint32_t epoch; uint8_t life, death_kind, green, mir_owner;
                 uint32_t mir_id; char name[48]; float scale[3], radius, rel_root[12]; } IslandMember;
typedef struct { Eid a, b; uint8_t sa, sb; float rel[12]; } IslandBond;
typedef struct { Eid id; uint64_t owner; uint32_t epoch; } IslandFar;
typedef struct { uint32_t id; Eid member; uint32_t epoch_seen, digest; uint8_t reason, layout_ok; } TakeMsg;    // 42
typedef struct { uint64_t asker; uint32_t id; uint64_t owner; uint32_t epoch; uint32_t sw, on; uint64_t payer;
                 uint8_t n; Eid m[21]; } GrantMsg;                                                              // 43
typedef struct { uint32_t id; uint8_t reason; uint64_t owner; uint32_t epoch; } RefuseMsg;                      // 44
typedef struct { uint8_t all; } OfferMsg;                                  // 45: OFFER_ALL (per island: POSE's flag)
typedef struct { Eid id; uint32_t number; } NeedMsg;                       // 46; id (0, 0): everything, with a number
typedef struct { uint32_t op; Eid x, t; uint8_t sx, st; uint32_t epoch_x; float rel[12]; } BondReq;             // 47
typedef struct { uint32_t op; uint8_t reason; uint64_t who; uint32_t epoch; } BondAns;                // 48 ACK, 49 NAK
typedef struct { uint32_t id; Eid e; uint8_t kind; uint32_t value; } ReqMsg;                                    // 50
typedef struct { Eid e; uint8_t kind; } DeathMsg;                                                               // 51
typedef struct { uint64_t key; uint32_t counter, count, hash; } IndexMsg;                 // 52; key: a constant
typedef struct { uint64_t key; Eid root; uint32_t digest; uint8_t state, why; } AppliedMsg;   // 53; key: hash of root
typedef struct { uint32_t id; uint8_t ans; } ReqAns;                       // 54; DONE, REFUSED
typedef struct { uint32_t number, count, hash; } IndexEnd;                 // 55, bulk stream

```

6.1 **POSE** (unreliable, latest per (sender, root); 30 a second while a member moves, 1 a second at rest; `pf` for
`n` members only). A receiver passes (sender, epoch) through `apply_owner` for the root; a POSE from a game that is
then not the root's owner is dropped. Every POSE taken writes `m` and the time into the newest root pose
(`Stated.root_m` / `pose_ms`, or the entry's for an island of one): **range and every placement use that newest
pose**, never the statement's alone. `cls` and the OFFER flag are used always; the per-member arrays only when this
game's digest of the island equals `digest`. A class change sends the POSE at once and with the next two ticks; a
class older than 2 s counts as IDLE. The transport's latest-only table grows from 128 to 2048 slots with a hash
over its keys, and gains a call that lists the slots of one type (for 6.7). An evicted slot comes again within a
second.

6.2 **ISLAND** (bulk, in pieces of the payload size; `counter` is the sender's statement counter and joins the pieces;
every piece carries the header; the body is the member rows, the bonds, the far rows, the recipe,
`rec_member`; a piece of a newer counter for the same root discards an unfinished older one, and a piece whose
counter is not above the last taken for that root is dropped; one reassembly buffer per sender). The counter is
per peer: the number of statements sent to that peer. An island with more than 48 bonds is stated with its first
48 in ascending order, has no recipe, and its digest covers those 48: logged, a stated limit. **A game states the
members it owns**: n member rows, `rel_root` from the root (the
smallest owned member), the recipe when it has one. Bonds: every bond with an end among those members. Far ends:
one row per foreign entity at the end of such a bond, with the (owner, epoch) the sender knows; a far row is not a
member. An island of one is an ISLAND with n = 1. Owed to a peer on: a change of digest, of a member's owner,
epoch or life; NEED; a join; 7.5.
At a receiver: member and far rows go through `apply_owner`; an ISLAND removes every `Stated` from the same sender
that shares a member with it and has a lower counter; an ISLAND whose sender does not own the member (after 5.0) is
dropped for that member. A full `Stated` table frees the ST_FAR one with the oldest `ms` (logged; NEED brings it
back). **A foreign member that no current statement names is not reconciled, and its edges are not touched.**

6.3 TAKE (reason GRAB or RIDE), 6.4 GRANT (to all; for 8.3 rule 6: asker = the new owner, id 0), 6.5 REFUSE (to
the asker): section 5.
6.6 **OFFER_ALL** (reliable, to all, from the net tick): every entity of the sender is offered (a load, 5.8). An
island's own offer is the flag in its POSE.
6.7 **NEED** (to a game): for a root, when a POSE slot names a root this game has no statement of for 0.5 s, at
most once a second per root; for everything, by 6.11.
6.8 BOND_REQ, BOND_ACK, BOND_NAK: section 8.
6.9 **REQ** (to the owner; again every 0.5 s for 3 s) and **REQ_ANS**: TOGGLE (value: the wanted state, read in
the hook) and EVENT (set on). An `Ask` row holds each until answered.
6.10 **DEATH** (to all, from the owner; from another game it is dropped): kind 0xff is a plain delete. For an Eid
a receiver does not know it is kept 2 s. The next ISLAND carries the life too.
6.11 **What a follower still holds that its owner no longer has.** INDEX (unreliable, per peer, every 2 s, not
while a statement is owed to that peer): the number of statements sent to that peer, and count and FNV-1a over
(Eid, epoch), ascending, of every LIVE, not PREVIEW entity the sender owns. A receiver compares only when it has
taken that sender's statements up to that counter and no GRANT naming that sender is under 1 s old. A mismatch on
two INDEX in a row: NEED(0, 0) with a number, at most once in 10 s per sender, and `named` is cleared on every
entity held for that sender. The answer is every island of the sender owed again, then INDEX_END with that number
**on the bulk stream after the last ISLAND**, {count, hash} taken when the answer was queued. On INDEX_END the
receiver hashes the entities of that sender that are `named`; only if that equals INDEX_END's hash does it mark
GONE the entities it still holds for that sender that are not `named`. Otherwise nothing is marked and the next
INDEX decides.
6.12 **APPLIED** (unreliable, to all, once a second per foreign island here): this game's digest of it, state, why.

## 7. Reconcile (phase 0, step 9)

7.1 **When.** For each `Stated`: on receipt, and once a second while this game's digest or layout differs. Not for
an island with an open operation of this game (8.2). While LOADING here neither reconcile nor the maker runs, no
pass is counted, and the handles of entries in the making are released.

7.2 **Entities.** In range: this Link within (the smallest member radius, less 10 m, less the greatest `rel_root`
distance) of the newest root pose; all members are made or none; a half-unloaded island is ST_FAR. In range, every
missing member is created; out of range nothing is, and an island with some actors left is ST_FAR.
A create waits when the newest pose is older than 2 s, a POSE has been seen for that root this session, and the
owner's heartbeat is not PAUSED. For each member the
sender owns:
- LIVE, no actor here, not `making`: a mirror pair first (4.4). Else, if **no** member of the island has an actor
  here and it has a recipe: the maker (9.2). Else a plain create of the member at newest root pose * rel_root;
- two actors for one Eid: the newer ENT_COPY one is deleted;
- DEAD with an actor here: the death is replayed with its kind (`deaths_put`'s call, G4 lifted); answered 0 on two
  passes, or kind 0xff: `part_delete`;
- GONE with an actor here: an ENT_COPY actor is deleted; a map actor or an actor this game made becomes this
  game's by 5.1.
All deletes this code asks go through one helper that sets the thread flag `ours` and first frees any mirror row
of the actor (`coop_obj_entity`), so the mirror tells nobody.

7.3 **Bonds**, for each member M the sender owns (the break also when other members are not here):
- **break** every real edge of M whose (a, b, sa, sb) the statement lacks, **unless**: it is the edge of an open
  operation; or its other end is an entity this game owns, or holds under an open request (such an edge is ended
  only by an operation's NAK, 8); or its other end is foreign and a statement from that end's owner lists the bond.
  This applies a split, whose edge lies between two statements. The break is main+0x1cf54d4(end A's owner actor
  +0x378, edge, 0, 1), read back the next frame and asked again for 10 frames before it counts as failed.
- **make** a stated bond with no real edge, **unless** its other end is an entity this game owns (then it waits
  for the BOND_REQ of the game that glued; after 2 s it is made and an operation opened from this side). Steps:
  1. both fragments leave the drive (as asked) until the read-back;
  2. the smaller fragment (fewer parts; not held, not ridden, with no member this game owns) is placed as one rigid
     set, one SetTransform per member's main body from the other side's body pose and the `rel` chain, exempt from
     G1 as the drive's writes are, and not if any of its bodies has a glue entry in progress. If the smaller
     fragment may not be placed, the other is, if it may; else the make waits. No make runs on a fragment whose
     island has an open request of this game;
  3. next frame, the preconditions, tested each frame for up to 5 s (then WHY_BOND_NOT_READY or WHY_BOND_POSE): the
     readiness test of the direct-bond reading on both actors (comps, physics set, bond manager, component +0x288,
     the body for each key not null); body +0x68 bit 24 clear on both (else the edge joins no graph node, is never
     listed, and would be made every second until the game's pool of 512 is empty); main+0x29a5a08 returns each
     body; the part count, max(the game's group count of each side, our own count with bonds made this pass and
     open operations) of both sides together, at most 21 (WHY_FULL22: at 22 the game breaks every glue edge of the
     part); the two bodies within 5 cm and 5 degrees of `rel`;
  4. the call main+0x1c05a98(SPR of a, sa, body of b for sb, Wa, Wb), the two world matrices as read then (five
     arguments: `call5`). The bond holds inverse(matA) * matB (F 50.5), so this holds the bodies as they stand,
     with no error for the solver, as the game's own glue does after it has pulled the parts together. What the
     solver does with an error, and what threw parts apart in an older run, are not established: each attempt logs
     the two bodies' relative speed over the next two steps [test];
  5. the new edge comes from the bond maker's hook (not a search); edge +0xef is set at once (the glue look: the
     strand is bound a step later and reads it then); SPR +0x20 bit 18 is set on a's SPR. A stated bond by +0xed
     alone is made as +0xec (a stated difference).
- The next pass reads the edges again; nothing is assumed.

7.4 **States and the ladder.** ST_FAR and ST_WAIT_MAKER (this player has a design selected, a preview batch of this
game is open, or this Link stands where the copy goes) are not differences and count nothing. `passes` counts a
pass that acted and whose read-back still differs. A new statement, coming into range, and the maker becoming free
reset `passes`, `rung2_done` and the back-off.
- rung 1, passes 1 to 3: 7.2 and 7.3;
- rung 2, a layout difference with an agreeing digest, once (`rung2_done`): the bonds of the member that is off
  are broken and made again by 7.3 (whose placing moves the fragment); a map member is placed by one SetTransform;
- rung 3, pass 4: every run-time actor of the island here that this game does not own is deleted and the island is
  made whole from the recipe. Skipped, with its reason, without a recipe, with a map member, or with a member this
  game owns;
- then ST_NOT_SHOWN: retried from rung 1 after 2 s, doubling to 30 s; state and reason in APPLIED and one log
  line.
A follower's failure never changes the owner's world.

7.5 **The owner's side.** Per owned island and peer: while the peer's APPLIED says ST_DIFFER or ST_NOT_SHOWN with
another digest for 1 s, the island is owed to that peer (once a second at most) and `diff_since` runs; the log's
"unconverged" is now minus `diff_since`. Not during an open operation on the island, and not for ST_FAR,
ST_WAIT_MAKER, ST_MAKING.

7.6 **Strain and the cap belong to the owner.** Keyed on `follower_body[]`, every body of every entity another
game owns, asked or not (11):
- the hook on the strain check main+0x190f034(strand, actor, dt) (already hooked as a logger) answers 1 at entry
  for a strand whose edge (strand +0x50) has an end body in that set: the game's own exempt path, no byte written;
- the hook on the list add main+0x191642c(manager, strand) skips the add for such a strand, and the hook on the
  kill main+0x190d924(strand) returns for one, so this game's cap of 21 glue looks (F 50.3: at 22 the oldest
  **bond** is broken) never breaks a copy's bond, whatever list the strand got into while it was owned here. The
  owner's game applies its own cap and the break arrives as state;
- when an island becomes owned or asked here: strand +0x1d4, +0x1d8, +0x1dc are set to 0 for its bonds (the
  strand is found by walking the list at SPR +0x170 of both ends for strand +0x50 = the edge).

## 8. A glue across owners (D 3.7)

8.1 The bond maker wrap (return address main+0x1d677a8) posts, for a non-zero edge only: the two bodies, their
world matrices before the call, the edge. Phase 0 resolves the two entities (4.4 enters one that is none), with X
the end this game owns or holds under an open request and T the other.
- both owned here: nothing more (3.2 sees the edge);
- X owned, T foreign: an `Op` opens at once and BOND_REQ goes to T's owner, `rel` = inverse(Wx) * Wt;
- one end held under an open request, the other owned or foreign: X is the asked end; the `Op` waits
  (OP_WAIT_GRANT_X) and sends nothing. When the GRANT of X comes it is sent if T is foreign, or closed if this
  game then owns both; if the request is refused or expires, the edge is broken;
- neither end is this game's (a stand-in's glue cannot happen: only this game's Ultrahand glues here): logged.
The same opens, at once, for any real edge 3.2 finds between an owned entity and a foreign one with no operation
(an Autobuild pull-in, a stake).

8.2 While an operation is open here: reconcile does not run on the islands of X or T; the edge is not broken (7.3);
both islands stay asked; this game states X's island as its own with T as a far end; 7.5 does not run for them.

8.3 T's owner, in arrival order, first by `ans[]` (kept 30 s). A request in service is a `Serve` row, and from the
make of rule 5 until rule 6 has run the pair counts as having an operation (8.1 opens none for it).
0. this game owns X as well, or the asker does not own X by this game's table and no grant of X to it is under way:
   ACK, nothing else;
1. not T's owner: NAK(NOT_OWNER, who, epoch);
2. T unknown, DEAD or GONE: NAK(GONE);
3. the part count of 7.3 would pass 21: NAK(FULL);
4. X has no actor here yet: the request waits (3 s, then NAK(NOT_HERE));
5. unless an edge for (x, t, sx, st) exists here already (made by reconcile, or by this game's own player gluing
   the same pair): the bond is made by 7.3's make, X's fragment being the one placed if it is loose here; if it
   cannot be within 3 s: NAK(POSE);
6. always after 5, in the same pass: one owner. If this game's class for T's island is IDLE, the merged island is
   granted to the asker; if HOLD or RIDE, X's island is taken (the new owner is this game; at the asker 5.0 ends
   its hold). Either way the GRANT names **every member of both islands**, X's as this game knows them, at 1 + the
   greatest epoch of all of them and `epoch_x`. Then BOND_ACK. The game that owns the island afterwards states it.
Two operations for one pair that cross: each side grants by rule 6 and 5.0 picks the owner; the surviving owner's
`rel` is the truth and the other side's rung 2 applies it.

8.4 The asker: ACK closes the operation. NAK(NOT_OWNER, who): who is this game: an ACK; else the request is sent
to who, twice at most, then it is a NAK. Any other NAK: the edge is broken (7.3's break with its read-back); the
reason is logged in words. The operation
also closes as an ACK the moment this game owns both ends, when this game no longer owns X, and by a claim (5.7). No
answer: sent again every 1 s;
after 10 s (tested in phase 0 step 6 only) it closes as a NAK.

8.5 Counter "a bond in one game only": a closed operation after which, 5 s later, the two games' digests of that
island differ. One count in a test session switches the design to take-first (D 3.7).

## 9. Making a copy

9.1 **The recipe at the owner**: for an owned island of 2 to 21 members with no far end, whose member set equals
its game group's member set (a regroup takes a frame or two). The history writer main+0x1913694 is run as
`record_pass` does, into a buffer it zeroes itself. `rec_member[i]` is rebuilt by the writer's rule: the group's
members, less those with no SPR, with SPR +0x26 bit 1, or with the byte at SPR +0x2e8 set, in a stable sort on the
bytes at actor +0x218; it is accepted only when the count equals the record's and each name matches the record's
(the stored name is not always that string). The writer fails for a group with more than 21 qualifying edges (a
build glued in loops): no recipe. A recipe is kept with the digest it was taken at and taken again when the
digest changes.

9.2 **The maker at a follower**, only for an island none of whose members has an actor here, and not while a
PREVIEW batch of this game is open: `make_pass`'s stages (the build object at ability +0x51f0 filled from the
record, the part maker inside `ent_creating`, the claim of each part bound to `rec_member[i]`, the confirm
main+0x1d3c0d4 with no cost, the SPR marks, green once grouped). The base matrix B (o +0x120) = newest root pose *
inverse(F of the root's record part), a part's world pose being B * F (a rotated B has not been run: the wanted
and made root poses are logged [test]). A make writes no save data, history or ability field (F 50.7); it
overwrites the player's working design in the build object, which is saved before and put back after by the table
of the maker-side-effects reading; not restorable, logged once: the player's last bond descriptors (o +0x3c88) and
the material cache. The abort test (ability +0x51e8, the object's flags & 7, o +4 and o +8 changed) runs each
frame and immediately before the confirm: the player chose a design and the game is cancelling our parts; the make
is dropped (its entries lose their actors, nothing is sent), and the island is ST_WAIT_MAKER. G4 lets the game's
own cancel of a batch in the making through. One batch at a time, in order of arrival.

## 10. Hooks and the event ring

Events are appended lock-free (a slot is claimed by an atomic counter and its `seq` written last). An overflow is
counted and phase 0 then re-reads every owned entity (3.1).
10.1 A hand glue's bond: the wrap of 8.1.
10.2 Structure changed: the bond maker and the graph's removeEdge main+0x2995024 (hooked today for a log; one line
to raise the event; it also fires on re-links). It only marks islands for 3.2.
10.3 **Deletes.** The hook on the delete request main+0x63baa8 posts only for an actor in `ent[]`, runs the
request inside itself (`inner` first; G4's refusal stays before the dispatch) and posts when it answered 1 (an
accepted request sets actor +0x28 bit 17 and later ones answer 0, so one event per actor), with the reason, the
return address and the load flag as read then. The deferring wrapper main+0x63b6b0 is not hooked for this. Class:
- **destroy**: reason 0 after an accepted death request of that actor (10.4), or reason 0 from the deferred runner
  (main+0x2fb9b0) or from the death procedures' return addresses main+0x1b4b8f4, 0x1b4be78, 0x1b4bf24, 0x1b4c17c,
  0x1b4c2fc; reason 2 on an entity that has an actor (DEATH 0xff);
- **no entity**: reasons 1 and 2 with no actor yet, reason 0 from main+0x54db94 (a refused creation);
- **unload**: everything else, and every delete with the load flag set. The teardown of a load is the game's
  delete-all-actors step, main+0x63baa8(actor, 4) from main+0x108c8c8 (F 50.1).
10.4 Death request main+0x1953184: posted when accepted, with the kind as stored after the game's rewrite.
10.5 Creates: 4.2. 10.6 Grab accept main+0x1c0b83c: {spr, holder}; taken only when the holder is this Link.
10.7 Device toggle and switch event (coop_autobuild.c `coop_phase0_hook`: DEVSWITCH_UPDATE, ONOFF_CALC): the hook
clears the toggle bit or event byte on a copy as today and posts {actor, the wanted state and the gauge's answer
as read then}. `events_out`'s 50 ms wait, `tog_seen` and `refused_on` are kept.

## 11. What phase 0 publishes

Open-address sets of 2048 slots, two buffers and an index each (the pattern of coop_life.c's veto), rebuilt each
frame, read lock-free:

| Set | Holds | Read by |
|---|---|---|
| `ent` | every entity's actor | `coop_ent_is`: the delete and death hooks, the mirror (below) |
| `owned` | actors of entities this game owns | `coop_ctn_runs_part` |
| `follower` | actors of live entities another game owns, asked or not, and the batch in the making | `coop_contraption_follower` (G4), `coop_ctn_follows` (a hit's toggle goes to the owner) |
| `driven`, `driven_body` | followers that are not asked, and their main bodies | `coop_ctn_driven` (force and motor gates), `coop_g1` |
| `follower_body` | every body of every follower | the strain, list-add and kill hooks (7.6) |
| `copy` | ENT_COPY actors | `coop_contraption_made`, `coop_is_copy` (the glue guards) |
| `nocharge` | actors whose payer is not this game | `coop_ctn_no_charge` |
| `part` | members of islands of two or more | `coop_ctn_part`, the stand-in veto (stand-ins go on touching loose things) |
| `refuse_stick` | sticks of islands whose owner's class is HOLD or RIDE (POSE under 2 s old) | the seat hook (5.6) |
| `shooter` | driven entities that have a Shooter | `coop_ctn_emitter_of_followed` |
| `follow_group` | the group (SPR +0x28) of each foreign island | `coop_ctn_sender_entry`: a hit is the fast path; on a miss the group's members are tested against `driven` (a regroup gives a new group record for a frame) |

The mirror: coop_players.c keeps all its code. `coop_obj_entity(actor)` (4.4) frees its rows; `coop_ent_is` at
`prop_fn`, the send loop of `report_props`, `arrow_copies`, `props_receive`, `props_hold`, `dwatch_fn` and its loop,
`destroy_apply` and `ride_objects` keeps new rows out and stops acting on old ones.

## 12. Drive, riders, logs

12.1 **Drive.** `follow_rigid`, `dev_follow` and `floats_follow` run on the drive tick at the cadence they were
played at, from `Snap` rows. A row is used only when its stamp is under 0.3 s old, not LOADING, and
`alive_as(actor, gen)` holds for each member written; `no_write` members (3.5) are not written. The three root
samples, the clock offset and `FollowState` are the tick's own, keyed by (owner, root) and reset when either
changes; the tick reads POSE for them (the part of today's `receive` that is kept). The rest band stays 0.10 m and
4 degrees, except that an island with a bond wanted (7.3 step 1) is out of the drive. No sample for 1 s: nothing
written. An error above 10 m: the drive tick sets `teleport` in the row and phase 0 does one SetTransform of the island
from `rel_root`; then the drive. All per-part masks are
by ascending Eid and used only under an agreeing digest. `springs_phase0`, `burning_phase0`, `dev_flush`,
`deaths_put`, `events_put`, `rider_weight` stay at phase 0.

12.2 **Riders.** `coop_ctn_parts_all` answers from `ent` (a lone wing or sled is ridden too). The player state's
`ride_owner` gets a bit that marks an entity; then `ride_id` is the serial and the low bits the minter's relay id
(0xfd: a map key; 0xfc: a minter that left, searched by serial). Two exports by that name map an actor to the pair
and back.

12.3 **Logs.** Every line starts with wall-clock milliseconds. An Eid prints as the low 16 bits of inc, a dot, the
serial. At start: the settings. Every 10 s and on change: `ent: owned N (M islands) | copies N (M) | far K | wait
K | not shown K | unconverged <peer> <root> <s> ... | full <table> ...`. One line with Eids for every ownership
change, statement, reconcile action, operation and answer, refusal, and counter.

## 13. Test settings (they stay in the code)
TOTK_COOP_ENT_DROP=5 drops every fifth reliable message before it is queued (a whole statement, never a piece);
TOTK_COOP_ENT_BREAK=1 breaks one edge of a copy island every 10 s; TOTK_COOP_ENT_DELAY=2000 holds one reliable
message in ten, and its stream behind it, back for that many ms.

## 14. What leaves the build, and what is re-pointed

Removed from coop_ctn.c: `reg[]`, the registry `Ent`, cids, versions, `record_pass` but for the recipe (9.1),
`in_place`, `bond_queue`, `bond_pool_recycle`, orphans, `made_parts` and adopt / original, `abandon`,
`parts_delete`, `edits_pass`, `coop_ctn_glue_made`, `take_over`, `authority`, `retire_pass`, `wanted_*`, `receive`
but for its POSE-sample part (12.1), `send_states`, the lone-device code, `objs_*`, `clean_line`, `cptoggle_test`,
`coop_ctn_first`, `coop_contraption_ident` / `_find` / `_owns`. Kept and moved: `part_edges` (with the bit 3
filter and the body list), `make_pass`'s stages, `follow_rigid`, `coop_g1`, `dev_*` and `setq`, the on/off and fuse
node tables, `gauge_blocked`, `floats_follow`, the spring helpers, `burning_phase0`, `deaths_put`, `events_put`,
`events_out`, `rider_weight`, `delete_radius`, `part_delete`, `died[]`, `ctn_on`.
Exported names kept and answered from section 11: coop_ctn_driven, coop_ctn_no_charge, coop_ctn_follows,
coop_contraption_follower, coop_ctn_part, coop_ctn_sender_entry, coop_contraption_made, coop_ctn_core, coop_g1,
coop_g1_counts, coop_held_here, coop_ctn_structure_event, coop_ctn_death_seen, coop_ctn_note_onoff,
coop_ctn_event_seen, coop_ctn_gauge_seen, coop_ctn_toggle_asked, coop_ctn_note_fuse,
coop_ctn_emitter_of_followed, coop_ctn_runs_part, coop_ctn_parts_all.
coop_players.c: `obj_take_over`, `obj_handover_apply`, `take_q` and the OBJS header word 2 stay, for what is not
an entity; `coop_obj_ident` / `coop_obj_find` lose their contraption branch; new exports `coop_obj_entity`,
`coop_obj_made_here`, and the ask-and-answer pairs (the mirror's name of an actor, the actor of a pair, the actors
of map keys).
coop_glue.c and coop_bonds.c leave the build with: `coop_glue_hook` and its two hooked addresses, `coop_glue_tick`,
`coop_glue_take`, `coop_bonds_tick`, `coop_bonds_take` (coop_world.c peer_tick), the test callers in
coop_autobuild.c, the four `coop_bonds_*` readers in coop_players.c. coop_bonds.c has another session's uncommitted
change: that session is asked first. TOTK_COOP_GLUE, _BONDS, _DEVICE_SYNC go.
cpu.h: new hooked entries main+0x54cb00, 0x191642c, 0x190d924, 0x502e60, 0xa1bc30 (0x190f034, 0x599670, 0x63baa8,
0x1953184, 0x1916830, 0x2995024, 0x1c0b83c, 0xa72b00 are hooked today). All are entries of lifted functions. The
cost is one recompile of every generated chunk, about 200 s at 24 jobs, not a new lift. coop_ctr.c: 2048 hashed
slots and the slot listing.

## 15. Pass tests (D 4.4), by piece

| Piece | Passes when |
|---|---|
| Identity | Three fans from the pouch: three Eids, equal in both logs, unchanged through two hand-overs and a glue |
| Mirror hand-over | A log and a dropped weapon, grabbed by each player in turn, then glued by each, then both gluing one log at once: one of each in both games throughout |
| Ownership | Twenty grabs back and forth of one fan: one Eid, epoch 21; both grabbing: one REFUSE(HELD), one hand lets go |
| Lost messages | ENT_DROP=5 and ENT_DELAY=2000 through the whole script: no island without an owner for 6 s, no NEED(0,0) more than once in 10 s, "unconverged" under 3 s |
| Split and merge | A build pulled in two, each half then held by a different player: two islands in both games within 1 s |
| Glue across owners | Ten trials of each case of D 4.4, both players attaching at once, and the owner in a menu: 8.5 stays 0, one owner afterwards |
| Copy making | A 3-part build seen by the other game within 2 s of its last glue; an 8-part Autobuild likewise |
| A moved build | A car driven 300 m by its owner while the other player follows 100 m behind: its copy is made where the car is |
| The player's Autobuild | After a copy was made here, this player's own history and selected design are as before |
| Drive | A loose fan pushed by its owner: under 0.10 m at rest in the other game, no teleport line, no chatter on a slope |
| Lifetime | Walk 200 m away and back: OFFER, GRANT, the copy made again on return with no back-off wait |
| Menu | The owner in a menu for 10 s: the other player grabs, glues and toggles, each within 1.5 s |
| Load | A fast travel with 50 owned loose devices near the other player: claimed by it or gone within 3 s, none destroyed, no ring overflow, and copies near the arrival made with no back-off |
| A paused owner | One player in a menu while the other walks up to its build: the copy is made; a build in the paused player's hand cannot be claimed |

## 16. What is not read, and its rule

| Not read | Rule here |
|---|---|
| Whether phase 0 runs in menus and cutscenes; which loads run the teardown step | PAUSED by construction (1.2); the flag is read, not the load's kind |
| What the solver does with a bond made off its pose; what threw parts in an older run | Place first, bond at the bodies' present poses, log the relative speed (7.3) |
| A dispenser's create, drops, other creators of devices | ASK_MIRROR with the mirror's own guards, counted (4.2) |
| Whether body keys are equal in two games | Assumed [inf]; a wrong key shows as a digest that never agrees: logged with both keys |
| What the game does to a rider whose stick is destroyed | Nothing extra; logged |
| The thread and phase of the strand update and the cap | The three hooks of 7.6 are keyed on a set, whatever the order |
| When a new actor's physics objects exist | The readiness test each frame (7.3) |
| The physics step against phase 0 | As today (the v3 runs worked); a crash here is the signal |
| What colours the aim highlight | A held thing may look grab-able; the grab is ended at once (5.5) |

## 17. Settled by the first test session's log

Each has its line: placement hashes of grabbed map objects in both games (planks, rocks, crates, map devices, a
pile of identical objects); body keys of both ends of every bond in both games; per direct bond: made, read back,
still there after 5 s, relative speed after the call; wanted against made root pose of every batch; whether a
stand-in's capsule create is ever seen; how many frames a create takes from request to `pend[]` hit; ring and
table high-water marks; the seat hook's refusals and what the event node did after one.

## 18. As built (2026-11-13): where the code differs from the sections above

The specification was implemented in full, built clean, reviewed against this file by four Sonnet readers
(R/sync-v4/code-review/: core.md, own.md, rec.md, kept.md) and then by four Opus 5.5 readers set against the code
as adversaries (R/sync-v4/code-review2/: threads.md, protocol.md, game.md, regress.md; one of them with the game's
code open in Ghidra). Every finding of both rounds is fixed, but for those named "left" below. Nothing has been run.
Where the code
had to differ from the text above, the code is what stands, and this section says how.

**Files** (section 14 named one). runtime/coop_ent.c: the table, reading the world, islands, the published sets,
the group views, the frame's pass. coop_ent_own.c: ownership and every message. coop_ent_rec.c: reconcile, bonds,
the glue across owners, the strain and cap hooks. coop_ent_id.c: identity, the create / delete / death / glue / grab
/ seat hooks, the ring's events, the mirror asked. coop_ent.h and coop_ent_msg.h: the data of section 2 and the
messages of section 6. coop_ctn.c keeps the kept code (the drive, the devices, springs, burning, deaths replayed,
the recipe and the maker, the edge reading, G1) and works from group views; its registry, versions, authority,
lone devices and messages are gone. coop_glue.c and coop_bonds.c stay in the tree and are not built
(tools/build_game.py).

**Data and threads.**
- The Snap of section 2 is the group view (`Grp`, `grps[]`, under `grp_lock`): one row per connected set of one
  owner here, rebuilt each frame; the kept code reads it where it read a v3 registry entry. Its drive state is
  allocated once per row and never freed.
- The event ring is a queue under a small lock (not lock-free). An overflow drops the new event, is counted, and
  the world is read again.
- Bonds are read again for every grouped entity on a structure event or every 250 ms, not per island (section 3's
  "due"). The set of follower bodies (7.6) is rebuilt four times a second.
- A part's bodies and their keys are found by asking main+0x44bba4 for keys 0 to 6 (not the body list, not body
  +0x1e8): the call the v3 code used.
- The held pose of an edge (3.2) is taken from main+0x7a4410 and checked against where the two bodies stand: a
  result more than 0.5 m off, or no number, is replaced by the bodies' present relation, and both cases are counted.
- The transport has 1024 slots, scanned linearly (6.1 said 2048, hashed).

**Identity.**
- An actor becomes an entity by 4.4 only after the mirror has answered on the co-op tick, so there is no
  "entered, not yet stated" state. A map actor's pair is COOP_OBJ_KEYED | key, and the other game finds its actor
  through the mirror's resolver with that pair.
- A device or Autobuild part minted at its create is handed to the mirror one tick later (`coop_obj_entity`), so
  the mirror forgets a row it may have made for it in the frame before.
- 4.2 rule 6 (another creator's device): `coop_obj_made_here` is the mirror's own "made" row, asked for up to 2 s.
- An actor made before a session exists is not entered later. 4.2's re-owning under a new incarnation is not
  written.
- The mirror keeps a copy of something that can be glued for one second after its owner stops listing it (the
  owner stops a moment before its statement arrives). A destroyed crate's copy therefore lingers up to a second.
  Things that can be picked up (items, weapons) and capsules are not held: they go at once, as before.
- Every actor bound to an entity (a copy made here, a part of the maker's, a minted device) is handed to the mirror
  on its next tick, which frees any row it has for it. A stand-in's swing is no hit on any entity (coop_life.c
  `synced_device`): the swinging player's own game asks the owner.
- 4.6 as built: a preview is cleared for the whole maker run when no part of it is a hologram any more and no SPR has
  a bond queued, or one second after the last hologram bit went.
- The pick-up refusal of 4.4's last bullet has no code of its own: G4 refuses every delete of a follower's actor
  from the three callers it knows; a pick-up's caller is not one of them [test].

**Ownership.**
- The held actor is found by the grab hook's note (main+0x1c0b83c, x0 taken as the SPR) and by a scan of the
  entities four times a second, whichever comes first.
- The payer carried in a GRANT is not applied by the new owner: whoever switches a device on next pays.
- A DEATH message means "replay this death on your copy". An entity's life changes only by its owner's statement,
  once the game there accepted the delete. A create of an entity is held 5 s after a death was replayed on it or a
  create of it failed. The death request of an owned entity is run inside its hook and told when the game accepted it
  (10.4), with the
kind as asked, not as the game rewrites it. The death component's path (the inlined lapse) is still told at entry.
- An entity whose owner left and that has no actor here is forgotten after 10 s. An entity this game owns that has
  had no actor here for 3 s is stated gone (a map entity: free).
- 10.3's class of a delete, as built: reason 0 or 2 is a destroy, but for reason 0 from main+0x54db94 (a refused
  creation) and for any delete while the load flag is set; every other reason is an unload. The return addresses
  10.3 lists were checked in Ghidra and mostly cannot occur (the deferred runner's calls return to main+0x2fc560
  and 0x2fd080; a death through the deferring wrapper arrives from main+0x63b6f4 or 0x63b830), so the class does
  not go by return address.
- An owned actor that went with no delete seen is offered like any unload (5.8), not stated gone at once.
- After this game's own incarnation changed (a reconnect) it claims nothing for 3 s. A message or a statement from
  a player who is no longer in its seat is dropped.
- A row whose owner changes loses the statement it had alone from its old owner.
- A change of a device (a switch, a time bomb, burning, a spring's extension) sends the island's POSE at once.

**Reconcile and bonds.**
- The make's precondition "main+0x29a5a08 returns each body" is not tested (the registry it takes was not at hand);
  the graph node (main+0xa2d39c) and bit 24 are. The game's own node look-up main+0xa2d39c is not called either: it
  writes its
answer through x8. The new edge is taken from the bond maker's hook while the direct call runs (the edge joins the
lists only a physics step later), and its glue look byte is set then. The call is made once per pair in a second.
- Strain fields are not set to 0 when a game becomes owner (7.6, last bullet): the strand of an edge is not found
  without a walk that was not read, and the reading calls the effect cosmetic.
- Rung 2 breaks only the bonds inside the sender's island; "a map member is placed by one SetTransform" is not
  written. With a member missing, a pass creates and does not yet break.
- The INDEX hash is a sum of per-entity hashes (order-free), not a hash in ascending order.
- "Two actors for one Eid" (7.2) cannot arise in the table (one actor per row) and has no code.

**The maker.**
- Saved and put back: the build object's base matrix, its record and the three bytes at +0x3c70. The kinds list
  and the part pool are left as the make leaves them (as in v3). The abort test reads the selected design
  (ability +0x51e8) only.
- No recipe is taken for a group with a member the game's writer skips; such an island is made part by part.
- A part the maker made whose entity went or changed hands meanwhile is deleted.

**Riders.** An entity is named between the games by a hash of its Eid with owner byte 0xfd (12.2 said the
serial and the minter's relay id).

**Left after the adversarial review** (each rare or small, none changed): a just-made bond across owners can be
broken once and made again when the bond request overtakes the other game's newer statement; the on/off state of a
device that is no entity (made before the session, or by a creator the mirror did not take as this game's) is no
longer sent; for up to 3 s while a member of an island unloads, a follower may apply device bits to the wrong
members; moving a fragment into place moves main bodies only, so a wheel's or a spring's other body is pulled
along by its joint; the pad-poll thread waits on the table's lock for the length of a phase-0 pass.

**Not built, as section 0 says:** the native refusal of a grab, the maker fed part of a record.

**Not written, though sections above ask for it:** the test settings TOTK_COOP_ENT_BREAK and TOTK_COOP_ENT_DELAY
(13; TOTK_COOP_ENT_DROP is); the settings printed at the start of the log (12.3); the release of a pending create's
handle when its entity's row is dropped; the removal of a `Stated` on a full table is as 6.2 says, the other
tables refuse and count.

## 19. The first session's tests

Two games on one PC. Each run needs the tester's yes. Read both logs after each block: every `coop: ent:` line
carries wall-clock milliseconds and Eids; the status line `coop: ent: <ms> owned ... | unconverged ...` is the
health of the session.

**Block A: does it start, and does identity hold (the gate for everything else).**
1. Both games reach the world with no crash; the status line appears in both logs.
2. P1 takes three fans from the pouch. Both logs: three "made here" lines at P1 with three Eids, three copies at
   P2 within a second, and the same three Eids in P2's log. The old "three fans, two seen" note.
3. P1 pushes a fan around. At P2: it follows, rests within 0.10 m, no teleport, no chatter on a slope.
4. Log lines that settle readings: "held poses of bonds read so far: N from the bond's rest frames, M from where
   the bodies stand" (if M is not 0, the rest-frame call's output layout is not as assumed); any "synchronous
   create" line; the stand-in capsule counter.

**Block B: ownership.**
5. P2 grabs P1's loose fan: a TAKE and a GRANT line, epoch 2, the fan follows P2's hand in both games. Back and
   forth twenty times: one Eid throughout, epoch 21.
6. Both grab one fan at once: one keeps it; the other's hand lets go (the "grab ... refused here" or
   "TAKE ... refused" line). What the player sees when a hold is ended this way is not known yet.
7. P1 holds a fan and P2 tries to grab it: P2's grab starts and drops at once.
8. P1 in a menu for ten seconds; P2 grabs P1's fan: granted "(answered while paused)". P1 holding a fan, then in a
   menu: P2 cannot take it.

**Block C: glue (the complaint this design is for).**
9. P1 glues two of its fans: at P2 the pair is one build within 2 s (a "being made here" line, or two bound
   copies and a "bond made here" line with its relative speed).
10. P2 grabs the pair, glues P1's third fan onto it: an "operation" line in P2's log, "bond ... in place" in P1's,
    one owner afterwards in both. Ten times. The counter to watch: any "a bond in one game only" line.
11. P2 glues its own fan onto a build P1 holds: the bond is made and P1 keeps the build (P2's hand lets go).
12. Both press attach on the same two parts at once.
13. P2 pulls a part off P1's idle build: two islands in both games within a second.
14. A log (felled tree) or a dropped weapon: grabbed by each player in turn (the old mirror's path), then glued by
    one player onto a fan: "is an entity now" in the gluer's log, one log in both games throughout, no twin.

**Block D: builds in motion.**
15. P1 builds a car (wheels, stick) and drives it; P2 watches, then stands on it, then rides while P1 drives.
16. P1 gets off; P2 takes the stick: TAKE (ride), GRANT. P2 tries the stick while P1 is on it: refused at the seat.
17. P1 drives 300 m with P2 following 100 m behind: P2's copy unloads and is made again where the car is.
18. Fans on a build switched by each player; a spring; a time bomb; a part of wood set alight.
19. P2 Autobuilds near P1 and 30 m away; P1 pulls a part off.

**Block E: lifetime.**
20. P1 walks 200 m from a build P2 stands by: "unloads here ... offered", P2 "claimed ... offered by its owner".
    P1 comes back: the copy is made with no wait.
21. P1 fast travels with a dozen loose devices near P2: P2 claims them; none is destroyed; no "ring overflowed".
22. A build is destroyed part by part at its owner (a time bomb): the same parts go at the other game.

**Block F: the two test settings**, once A to E pass: TOTK_COOP_ENT_DROP=5 through blocks B and C (no island
without an owner for more than 6 s; "unconverged" under 3 s), and TOTK_COOP_ENT_BREAK=1 (not written: the setting
has no code yet; a bond broken by hand at a follower with a hit is the stand-in).

**What a failure means.** A crash at start: the new hooks (the create wrap, main+0x54cb00, the delete wrap) are
the first suspects, each can be switched off by taking its address out of cpu.h. No "made here" line for a fan:
the create chain (4.2). Copies that never appear: the statement or the plain create (7.2); the status line's "far",
"wait", "differ", "not shown" counts say which. A glue that does not hold in the other game: the "bond made here"
line and its "edge listed / not listed yet" say whether the direct call took.
