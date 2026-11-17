# The entity sync (contraptions), as built

This page says what the contraption sync does now, read from the code on `main` (2026-11-17). The code
calls it the **entity sync**: what it keeps the same is any *entity* (a Zonai device, an Autobuild part, or anything
glued to one), and a contraption is one island of entities. Hence the page's name.

Its specification and the older design pages were removed on 2026-11-17 (their game facts are in
[facts/](facts/README.md); the pages are in git history, commit 2604efb8); an "S n.n" or "v3 NN" in a code comment
names one of their sections. Where anything and the code differ, the code is what runs. Code is cited as file and function. "Not confirmed" means the code says it but no run this page could check shows
it.

## In one paragraph

Every entity has one id for life and one **owner**, the game that simulates it. Entities joined by glue form an
**island**, which has one owner. The owner reads its own world every frame and **states** each island whole to every
other game (ISLAND), and sends a fast pose with the devices' state (POSE). Every other game holds a **copy** and
**reconciles** its world toward the statement: it makes what is missing, replays deaths, breaks glue the statement
lacks and makes glue it has. A copy is moved by a velocity **drive** toward the owner's pose, and **gates** stop game
code in the copying game from pushing it. Ownership moves only when the owner answers a request (TAKE, then GRANT or
REFUSE), or by a claim when the owner is gone, loading, paused and silent, or offers the island.

## Files

| File | What it holds |
|---|---|
| `runtime/coop_ent.h`, `coop_ent_msg.h` | Types (`Eid`, `Ent`, `Bond`, `Island`, `Stated`, `Grp`) and the wire structs, types 40 to 55. |
| `runtime/coop_ent.c` | The table, the event ring, reading the world into bonds and islands, the sets the hooks read, the group views, the order of a phase-0 pass. |
| `runtime/coop_ent_own.c` | Ownership and every message: statements, POSE, TAKE/GRANT/REFUSE, REQ, DEATH, INDEX, NEED, OFFER_ALL, the pause queue. |
| `runtime/coop_ent_rec.c` | Reconcile a copy to its statement; break and make one bond; glue across two owners. |
| `runtime/coop_ent_id.c` | The identity of new actors; the hooks' events (create, delete, death, glue, grab, toggle). |
| `runtime/coop_ctn.c`, `coop_ctn.h` | The code that touches the game: the drive, gate G1, devices, deaths replayed, burning, springs, a rider's weight, the recipe and the copy maker. |
| `runtime/coop_ent_hooks.c` | Where the sync runs from each frame, and the copy gates (`coop_phase0_hook`). |
| `runtime/coop_life.c` | Lifetime hooks used here: G4 (no local death for a copy's part), I16 (stand-ins never collide with parts), the bond maker's capture, the delete hook. |
| `runtime/coop_ent_log.c` | The session record and the tester's mark key. |
| `runtime/coop_ent_bench.c` | The per-second audit's entity part, and the test bench (`TOTK_COOP_BENCH`). |
| `runtime/coop_autobuild.c` | Autobuild research tests (`TOTK_COOP_TEST_ABLOG`); not part of the sync. |

The whole sync is on unless `TOTK_COOP_CONTRAPTION=0` (`coop_ctn.c ctn_on`, `coop_ent.c ent_on`).

## Threads

- **Phase 0**: `coop_ent_hooks.c coop_ent_seq_calc` -> `coop_ent.c coop_ent_phase0`, at GameDataMgr::calc
  (main+0xa72b00) outside the parallel actor update. Everything that walks or changes the table or the world. Holds
  `ent_lock`.
- **The co-op tick**, only while another player is in the session: `coop_ent_hooks.c coop_ent_tick` (from
  `coop_hitchan.c coop_hitchan_tick`): the object mirror's answers (`coop_ent_mirror_tick`), the drive
  (`coop_ent_drive` -> `coop_ctn.c ctn_drive`), a save's devices, the bench, the session record.
- **The net tick**: `coop_ent.c coop_ent_net` (from `coop_spawn.c coop_tick`): the heartbeat's PAUSED and LOADING flags,
  and while paused or loading the pause queue, GRANT and REFUSE, and TAKE answers.
- **Any game thread**: hooks read the published sets (`ent_set_has`) and post events to a ring (`ent_post`).

Reliable messages and join/leave events reach the sync through its own reader of the transport's inbox
(`coop_msg.c`, `COOP_MSG_ENT`), which `coop_ent.c ent_reader_open` opens at the first poll. The enemy sync reads the
inbox first in each poll; a reader that is not open has its messages dropped (with one log line).

The order of a phase-0 pass (`coop_ent_phase0`): peer events; the pause queue and the transport's messages; the latest
POSE, APPLIED and INDEX; the ring's events; each entity's liveness and main-body matrix (`world_caches`); creates and
the copy maker; bonds (every 250 ms, or at once after a change); islands; this Link's hold and ride; islands again;
timers; this game's grabs and rides, and glue operations; sending; reconcile (not in a load); the kept device code
(`ctn_phase0_kept`); the published sets and group views (`publish`); a status line.

## Identity

An entity's id is `Eid` (`coop_ent.h`): the incarnation of the game that made it and a serial, or incarnation 0 and
the map key for a placed map actor.

- **A device this player puts out.** The async create (main+0x599670) is hooked (`coop_ent_id.c
  coop_ent_create_hook`). It decides by the caller: the Autobuild part maker (lr main+0x1d3bb98) mints a **preview**
  per maker run; a capsule opening (lr main+0x1b7f8c8) mints only when this game's Link opened it; any other `SpObj_`
  create asks the object mirror. When the game marks the actor created (main+0x54cb00, `coop_ent_created_hook`) the
  decision becomes an event, and phase 0 mints the Eid (`ent_ring_events`).
- **Autobuild previews** are not sent. A maker run is confirmed together once each of its parts has shown the
  hologram bit (SPR +0x20 bit 51) and every bit is clear again, and no bond is queued (or a second has passed since the
  bits cleared). A run whose hologram never showed waits with no limit (`coop_ent_id.c ent_ring_events`).
- **Anything glued.** An actor that becomes an end of a real glue edge and is no entity yet is entered
  (`coop_ent_id.c ent_enter_glued`, from `coop_ent.c bonds_read`). The object mirror is asked its name on the co-op
  tick, and `entered` makes the Eid: a map actor by its key; an object the mirror has a pair for by (owner's
  incarnation, `0x80000000 | id`), so two games that enter one thing mint one entity; else a new serial.
- **Devices from a save** have no identity and stay in one game only, unless `TOTK_COOP_SAVED_DEVICES=1`: then the
  host's (relay id 1) are given one and every other game deletes its own (`coop_ent_saved_tick`). Off because "the
  host" is whoever connected first, and the rule takes any device without an identity.

## Ownership

`coop_ent_own.c ent_apply_owner` is the one rule for taking an owner from a message: a pair is taken only if its epoch
is greater, or equal with a greater incarnation. All members of an island share the greatest epoch.

- **Claim** (`own_claim`, epoch + 1): a grabbed map entity nobody owns; a request whose owner left, is loading, or is
  paused and silent for 1 s while not holding or riding it; an island whose owner left the session 2 s ago (not within
  3 s of this game's own reconnect); a switch asked of a paused, loading or gone owner.
- **TAKE** (`own_causes`, `own_grab`, `req_send`): this Link's Ultrahand takes hold of another game's entity
  (TAKE_GRAB), or this Link rides another game's steering stick (TAKE_RIDE). Sent again every 0.3 s.
- **The owner's answer** (`take_in`): GRANT at epoch + 1, to everyone, when the island is idle (not held or ridden
  there), not offered, and the asker's digest and layout agree with the owner's. Else REFUSE: not the owner (the
  request is forwarded once to the owner named), stale, held, or ridden. An island that is offered, or an owned
  entity with no stored island, gets no answer (`take_in`). A GRANT is sent again every 0.5 s until the new owner is
  heard or 5 s have passed.
- **A grab or ride that is not granted**: a refusal, or 1.5 s with no answer, ends this Link's hold with the game's own
  refusal message (0x20510006, main+0x2b76c34; read back and sent up to three times, `own_end_hold`) or its ride with
  the game's get-off (main+0xc448c8, `own_end_ride`). A grab of an island whose owner holds or rides it is ended at once,
  with no TAKE.
- **The seat claim** (main+0x502e60, `coop_ent_id.c coop_ent_seat_hook`) is refused on a stick of an island whose
  owner holds or rides it.
- **An unload** of an owned entity with no delete seen freezes its island and offers it for 2 s (POSE with the offer
  flag, `own_unload`). With no claim, each member whose actor went is stated GONE (a map entity: FREE, nobody's).
- **A load**: OFFER_ALL to everyone at once and every 0.5 s while loading (`own_net`).
- **Paused or loading** (no phase 0 for 300 ms, `ent_paused`): incoming messages wait in a pause queue of 1024; GRANT
  and REFUSE are still taken from the net tick, and TAKE is answered there while paused and not loading (`own_net`).
- An entity this game owns, alive by the table, with no actor here for 3 s, is stated GONE (FREE for a map entity).

## Statements and copies

**ISLAND** (`coop_ent_own.c stmt_build`, reliable, in pieces of one transport payload): the island's members (id,
owner, epoch, life, death kind, green, the mirror's name for it, actor name, scale, radius, its pose relative to the
root), its bonds (the two ends, their body keys, the relative pose), the far ends (other owners' entities glued to it),
and for an island of 2 to 21 members with no far end the **recipe**: the game's own record of the group, taken from its
history writer (main+0x1913694, `coop_ctn.c ctn_recipe`). A statement is owed to a peer when the island's digest
changes, and sent in order while the queue takes it.

**POSE** (latest per island): the root's main-body matrix and velocity, the owner's clock, the class (idle, held,
ridden), the offer flag, who pays, and per member the device bits and floats. 30 a second while a member moved in the
last second, once a second at rest, at once on a class or device change.

**APPLIED** (once a second, from each copy's game): the digest it has and its reconcile state. **INDEX** (every 2 s):
a count and hash of what the owner owns, to find what a copy still holds that its owner no longer has; NEED and
INDEX_END answer it.

**Reconcile** (`coop_ent_rec.c rec_one`, per statement):

1. Deaths and goings are replayed whatever the distance: a death with its kind (`ctn_death_put`), a plain delete, or a
   GONE copy deleted.
2. If members are missing and this Link is out of range (the smallest member radius less 10 m less the island's
   reach, at least 15 m, from the newest root pose), the copy is FAR and nothing is made.
3. If nothing of a 2+ island is here and a recipe came, the island is made whole by the **copy maker**: the game's own
   Autobuild part maker and confirm, run from the owner's record, at no cost to this player (`coop_ctn.c
   ctn_make_start`, `ctn_make_pass`; `coop_life.c coop_life_free_confirm`). It waits while another make runs, a design
   is selected or a preview is open (`ctn_make_waits`); where this Link stands in the way the copy is made that far
   above its place (`ctn_make_lift`). It is not used when this player has no Autobuild build object, when a member
   may be bound to an actor of the object mirror's, or when a member's name is one no statement makes.
4. Otherwise each missing member is made by a plain create (`coop_request_create`, or `_autobuilt` for a green part),
   or bound to the actor the object mirror has for it.
5. Bonds: an edge here that the statement lacks is broken (main+0x1cf54d4, `rec_break`); a stated bond with no edge
   is made (`rec_make`, below).
6. Layout: each member is compared with its stated pose relative to the root (0.3 m, 15 degrees). A member off for 1 s
   has its bonds broken and is placed again (rung 2).
7. A pass that acted and still differs counts. After four, every run-time actor of the island here is deleted and the
   island made whole from the recipe (rung 3), not with a map member or a member this game owns. Failing that, the
   island is **not shown** here, tried again after a back-off doubling from 2 s to 30 s.

What another game states is checked before it is made: a member named as a player, character, enemy, animal, horse,
chest, dungeon object, armor or one of the game's helpers is not made (`coop_game.c coop_create_refused`), nor one
whose place is not a finite number; a recipe's bonds must name parts it has, and each part's name must be its
member's and allowed. A far end never changes the owner of an entity this game owns. Ownership taken by a statement
or a GRANT is otherwise trusted from any player (claims of a paused or loading owner's entities use them; PLAN.md
step 0).

Copies are marked `ENT_COPY`. A green (Autobuilt) copy is made by the Autobuild create, so it looks green as the
owner's does.

## The drive

A copy's island is moved as one rigid body (`coop_ctn.c follow_rigid`, on the co-op tick). Each frame it gets one
linear and one angular velocity from the owner's newest root samples and sent velocity, predicted to the owner's
present, plus a bounded correction. Every part's main body is written `v = V + W x r`, so its bonds see no relative
motion. What a physics step takes off the written vertical velocity (gravity, damping) is measured and added back.

- When the owner rests and the copy sits where it should, nothing is written and the bodies may sleep.
- Out of place at rest for 2 s, the copy is put in place (SetTransform through `ctn_place`), at most 3 times per rest.
- More than 10 m off, it is teleported to the newest pose (`ctn_teleport`).
- Members behind a body other than the main one (on a joint) are not written.
- No pose for 2.5 s: the copy is not driven.
- An island under an open request or glue operation here is not driven.

## Gates on copies

- **G1** (`coop_ctn.c coop_g1`): a write to the main body of a driven copy is dropped, except the drive's own: the
  velocity setters main+0x567750, 0x25a1f0, 0x40e490, the impulses 0x44be18, 0x29ae114, 0x29ae7a8, the forces
  0x29ae7fc, 0x29a4afc, and SetTransform 0x25a450. A spring's two launches (returning to main+0x221a9c0 and 0x221aa14)
  and a part this game's Link holds pass. `TOTK_COOP_G1=0` turns it off.
- **Device nodes** (`coop_ent_hooks.c coop_phase0_hook`): on a driven copy a fan's push (main+0x2487104) does nothing;
  the device nodes that push their own part (rocket, sled, balloon, wing, hover stone, stabilizer, spring, stake,
  homing cart, steering stick) run with their body writes skipped; wheel and cart motors are skipped; a wheel's motion
  node runs once with its "inert" branch, which takes its motor constraints off, and is then skipped.
- **Toggles**: a hit's toggle on a copy's device (main+0x4a3500) is taken off and sent to the owner as a REQ
  (`coop_ctn_toggle_asked`). A jump attack's switch event is sent the same way (`coop_ctn_event_seen`).
- **Who pays**: a device another player pays for charges no gauge here (main+0x1fa4ca8, 0x1fa4ed4 skipped) and keeps
  its "can pay" bit set, so a watcher's empty gauge or distance does not switch the copy off.
- **G4** (`coop_life.c`): a part of a copy takes no death from this game's own causes. `TOTK_COOP_G4=0` off.
- **I16** (`coop_life.c`): a stand-in never collides with a contraption part. `TOTK_COOP_I16=0` off.
- **Strain and the 21-part cap** belong to the owner: on a strand with an end in another game's entity, the strain
  check (main+0x190f034), the glue manager's list add (main+0x191642c) and the cap's kill (main+0x190d924) are skipped
  (`coop_ent_rec.c coop_ent_strand_hook`).
- A copy's device striking an enemy the enemy sync shares (a copy here, or one this game owns) is left to the game
  that runs the device, which counts the blow on its own actor of that enemy (`coop_contact.c contacts_filter`, via
  `coop_ctn_emitter_of_followed`).

## Devices

At the owner, `coop_ctn.c ctn_owner_state` reads per member: whether it takes the switch and is on (the blackboard
bool `SpecialParts_IsOn`, else component 11), a wheel's spin and steer, a steering stick's axes, a time bomb's fuse
and armed state, a spring's extension, limit and enables, and whether it burns. POSE carries them.

At a copy whose statement agrees, `dev_follow` (on the co-op tick) and the phase-0 code set them the same:

- **On and off**: by the message the device's on/off node takes as "set on" or "set off" (0x6b510000 / 0x6b510001,
  through main+0x2b76c34), compared twice a second. Any part that takes the switch follows; the code does not list
  kinds. Whether beam emitters, batteries and hydrants follow was not confirmed.
- **Wheels and sticks**: their blackboard floats (`floats_follow`).
- **Time bombs**: the fuse.
- **Springs**: the joint's limit and enables by the game's own setters, and the head placed at the owner's extension
  (`springs_phase0`).
- **Burning**: a copy that differs is ignited or put out with the game's own requests (`burning_phase0`). Not shown by
  reading that this alone brings the copy to the same state (the file says so).
- A toggle asked of the owner sets every switched device of the island to the state wanted, and the asking player pays
  (`ctn_toggle_put`). Off for 2 s, whoever switches it on next pays.

**A rider's weight**: another player's stand-in on a build weighs nothing in the owner's game (I16), so the owner adds
its weight as a force at the stand-in's feet on the ridden part (`coop_ctn.c rider_weight`, main+0x29ae7a8).

## Glue

**Glue by hand, one owner.** A glue edge this game makes between its own entities is just a new bond: the next
`bonds_read` sees it, the island's digest changes, a new statement goes out, and the copies make the bond with the
game's direct bond (main+0x1c05a98) and set its glue look (edge +0xef, the part's bit 18; `coop_ent_rec.c rec_make`).
So hand glue shows in the other games. Breaking glue is a bond that leaves the next statement; copies break their edge.

**Glue across two owners** (S 8). A glue made here between an entity this game owns and another game's opens an
**operation** (`rec_edge_found`): BOND_REQ to the other owner. That owner (`rec_bond_req`, `serve_step`) places the
smaller fragment, makes the bond, and GRANTs the merged island to one owner: itself if its Link holds or rides it, else
the gluing game (`serve_finish`). A refusal (not the owner, gone, over 21 parts, not here, could not be placed) or no
answer breaks the gluing game's edge (`OP_BREAKING`).

**Making a bond on a copy**: the fragment is taken out of the drive and placed so the two bodies stand as stated
(`place_for_bond`); a frame later the bond's preconditions are checked and the direct bond called; 5 s for all of it.
At 22 parts the game would break every glue edge of the part, so a make that would pass 21 is refused.

## Riding

`coop_ent.c link_read` finds the stick this Link rides (`ctn_stick_rider`); the island's class is then RIDE. Riding
another game's stick opens TAKE_RIDE, so the rider's game runs the contraption once granted. While its owner holds or
rides it, the seat is refused here. The class a copy shows ages to idle after 2 s without a POSE, not while its owner
is paused.

## Death

A delete of an owned entity's actor (main+0x63baa8, hooked in `coop_life.c` -> `coop_ent_id.c coop_ent_delete_hook`)
that is a destroy, not an unload, states the entity DEAD. An accepted death request (`coop_ent_death_seen`) sends DEATH
with its kind. Copies replay it with the game's death request (main+0x1953184, G4 lifted for it; `ctn_death_put`), or
delete the part if the replay did not take it in a second. DEAD and GONE rows are dropped 10 s after the last
statement of them.

## Messages

Types 40 to 55 (`coop_ent_msg.h`), over `coop_ctr.c`; the transport keeps the latest of each unreliable type per
(sender, key).

| No. | Name | Kind | What |
|---|---|---|---|
| 40 | POSE | latest per island | root pose, velocity, class, offer, payer, devices |
| 41 | ISLAND | reliable, bulk, pieces | the whole statement |
| 42 | TAKE | reliable | a grab or ride asks for an island |
| 43 | GRANT | reliable, to all | new owner and epoch for listed members, with the devices' state |
| 44 | REFUSE | reliable | why not |
| 45 | OFFER_ALL | reliable | this game is loading: take what you can |
| 46 | NEED | reliable | ask for one statement, or everything |
| 47-49 | BOND_REQ, BOND_ACK, BOND_NAK | reliable | glue across owners |
| 50 | REQ | reliable | a toggle or a switch event asked of the owner |
| 51 | DEATH | reliable | an owned part died, with its kind |
| 52 | INDEX | latest | count and hash of what a game owns |
| 53 | APPLIED | latest per island | a copy's digest and state |
| 54 | REQ_ANS | reliable | done or refused |
| 55 | INDEX_END | reliable, bulk | end of a NEED-everything answer |

## Settings

| Setting | Default | What |
|---|---|---|
| `TOTK_COOP_CONTRAPTION` | on (`0` off) | the whole entity sync and every gate |
| `TOTK_COOP_G1`, `TOTK_COOP_G4`, `TOTK_COOP_I16` | on (`0` off) | single gates, for finding a fault |
| `TOTK_COOP_SAVED_DEVICES` | off (`1` on) | the host's save devices given an identity, others deleted |
| `TOTK_COOP_DEBUG` | off | the session record (`coop_ent_log.c`) and the copies' follow line |
| `TOTK_COOP_RECORD` | unset | when set, decides the session record alone |
| `TOTK_COOP_FOLLOW_TRACE` | off (`1` on) | one `coop: ftrace:` line per written copy per frame (the reports read it) |
| `TOTK_COOP_ENT_DROP=<n>` | 0 | test: drop every n-th reliable message, whole statements included |
| `TOTK_COOP_BENCH` | unset | test bench scenes (`coop_ent_bench.c`, `play_two.ps1 -Bench`) |
| `TOTK_COOP_TEST_ABLOG` and the `TOTK_COOP_TEST_AB*` tests | off | Autobuild research (`coop_autobuild.c`) |

## Leftovers in the code

- Comments that no longer match: `coop_ent_id.c coop_ent_saved_tick`'s block comment says `TOTK_COOP_SAVED_DEVICES=0`
  turns it off (it is off unless `1`); `coop_ent.h` says `rec_owned_now` zeroes the strain fields (the code leaves
  them); `coop_ent.h`'s `Ev.seq` comment describes a lock-free ring (the ring is under a mutex and `seq` is unused);
  `coop_autobuild.c`'s header says the sync is `coop_bonds.c` (removed); `coop_ctn.c` says the on/off node is noted
  from `coop_autobuild.c` (it is `coop_ent_hooks.c`); `coop_life.c`'s header names `TOTK_COOP_TEST_X15` and `_X6`,
  which nothing reads now.
- `coop_ent_own.c ent_apply_owner` has a statement that does nothing (`e->flags |= ... ? 0 : 0`).
- The switch-update log in `coop_phase0_hook` prints a constant 0 as "ours".
- `coop_ctn.c dev_follow` says "every 3 s" for a log made once a second.
