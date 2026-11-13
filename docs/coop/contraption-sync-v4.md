# Contraption sync v4: the current design

Status, 2026-11-13: sections 3 and 4.1 to 4.3 are implemented (runtime/coop_ent*.c, coop_ctn.c) and built; nothing
has been run. The specification's section 18 says where the code differs, section 19 lists the first tests.

- History, runs, reviews and play-test notes: [contraption-sync-v3.md](contraption-sync-v3.md) (an archive; code
  comments cite its section numbers). Its section 48 is the review that led to this design.
- What the game does (structure, devices, lifetime, read from its code):
  [contraption-game-facts.md](contraption-game-facts.md).
- R is the research folder outside this repository. The review behind this design is R/sync-v3/review3/ (six maps of
  the code and its history, three designs, two critiques). Addresses are TotK 1.4.3, main+offset.

Words: a *part* is one actor that Ultrahand can grab. A *build* is parts glued together. A *copy* is the actor
another game shows for a part it does not own. The *owner* of a thing is the one game that simulates it.

## 1. Why v3's bookkeeping is replaced

v3 syncs builds in one system (runtime/coop_ctn.c: a runner sends numbered versions of a build record, followers
apply them) and loose objects in another (the object mirror in runtime/coop_players.c). Its physics side works; its
bookkeeping does not converge. From the review (v3 section 48.2):

1. Two systems own the same actors, and a glue between two players' things has to move actors across that boundary
   in both games at once.
2. Nothing has an identity for life: a build is "the group the local recorder sees", a taken object gets a new
   number, messages address parts by index inside a version.
3. Requests have no answer and results are not checked, so a glue can exist in one game only.
4. Ownership is guessed from polled touch and time windows.
5. Nothing compares the two worlds, so a lost step stays lost until a special case notices it.

Of the play-test notes 15 to 26, 11 are in these classes. Follower physics was 45% of the problems of v1 and v2 and
is 9% of v3's play notes.

## 2. What is kept from v3 (it exists and has been played)

These are described as the code is (read in review 3, R/sync-v3/review3/core.md section 5 and hooks.md section 2).

### 2.1 The copy and its drive
- A copy of a build is a real actor with dynamic bodies, made whole by the game's own Autobuild part maker and
  confirm from the owner's record of the build (coop_ctn.c make_pass). The record is the game's own history writer
  output (main+0x1913694). A copy costs no Zonaite (main+0x209b754 answered 0 during the confirm).
- A copy is moved by velocity writes on each part's main body, every co-op tick, as one rigid field toward the
  owner's predicted root pose (coop_ctn.c follow_rigid: prediction from three samples, a proportional and integral
  correction clamped to 3 m/s and 2.5 rad/s, compensation for what the step took, a rest band). Only the root's pose
  is sent today; the other parts' places come from the record.
- Not kept: the object mirror's way of moving a loose copy (a speed from tick differences and a teleport about once
  a second: place_copy, force_to). Loose things go under the same drive (3.6).

### 2.2 The gates on a copy
A copy is simulated by the game, so the game's own rules are held off it. All are keyed by "a part this game does
not own" (coop_ctn_driven and the lists coop_ctn.c lists_rebuild fills):
- **G1** (coop_ctn.c coop_g1): the nine rigid-body writers (three velocity setters, three impulses, two forces,
  SetTransform) are dropped on a copy's main body, except the drive's own writes, the spring's two launches, and a
  part this game's Ultrahand holds.
- **Force nodes**: fan push skipped; rocket, sled, balloon, wing, hover stone, stabilizer, spring, stake, homing cart
  and steering stick nodes run but move nothing; cart and wheel motors off (the wheel through the game's own disabled
  branch, entered once); the stake never pins; the cannon fires no ball.
- **Devices**: the group's own on/off sender is silenced on a copy; a hit on a copy's device is sent to the owner
  as a request; a copy's device charges only the payer's gauge; a stand-in's swing is not a hit here.
- **G4** (coop_life.c): the game's death request is refused for a copy's part unless it replays the owner's death;
  three delete callers are refused; the engine's far unload passes.
- **The stand-in veto** (coop_life.c coop_life_veto, I16): a stand-in's bodies never collide with a contraption part,
  in any game.
- **Glue guards**: the glue tick hook clears body +0x68 bit 24 (no graph node) and the glue upkeep hook clears bit 26
  (the one-step "teleported" latch) on a copy's body, so a glue onto a copy is not cancelled.

Per device kind (what each device's node does, what is sent for it): contraption-game-facts.md sections 34 and 35.

### 2.3 Transport
runtime/coop_ctr.c: per-peer incarnations, reliable streams with selective acknowledgement, latest-only slots, a
30 Hz heartbeat with a PAUSED flag. The review found nothing in it that causes a desync. One change: messages are
taken in arrival order, not by type (3.3).

### 2.4 Decisions and the rule of work
- The tester's decisions D1 to D7 (v3 section 22) stand: who pays while the activator loads, is in a menu or leaves;
  a rider's weight as a force; quest effects per world.
- Either player may move any object. Map and NPC vehicles and spawns are outside this milestone.
- The rule of work (v3 section 29): nothing is written on a theory of the game; what is unknown is read (Sonnet
  readers, notes in R); no run to find something out; each run needs the tester's yes.

## 3. The design

### 3.1 Entity
An entity has one id for life: (minting incarnation, serial). A loose fan and a fan in a build are the same thing.
In the first build the entities are: Zonai devices (not their capsules), the parts an Autobuild made, and any other
actor a player glues (from that moment, for the rest of the session). Capsules, pickable items, weapons, materials,
logs, rocks, arrows, beams, the paraglider and fused gear stay in the object mirror as they are today, with its own
grab hand-over, until one is glued (decided in the specification's reviews: the mirror is one pipeline, it has been
played, and this keeps the hand-over between the two systems to the moment of a glue).
- A copy this code creates is bound to its id by the create handle.
- A run-time actor this game made is minted here.
- A map-placed actor is (0, hash of placement hash and name), only after one log line per game has shown the hashes
  equal for planks, rocks and map devices. Until then map objects keep the present key.
- The parts of an Autobuild are announced when the preview bit is clear and no bond is queued, so the build is
  announced whole.

### 3.2 Island and owner
An island is the set of entities connected by glue. It has no id of its own. Every entity carries (owner, epoch);
all entities of an island share them; a pair replaces another only if it is greater (epoch first, then incarnation).
A loose object is an island of one, and so is a lone device. No message carries a build id, a version number, a
shape hash or a part index.

### 3.3 Moving ownership
One reliable stream per sender, taken in arrival order.
- TAKE is sent on the exact causes: this Link's Ultrahand accepted a grab, the stick's rising edge, a glue. It is
  repeated until answered.
- The owner always answers: GRANT (to all, epoch + 1, with the device bits and the payer) or REFUSE with a reason
  (held, ridden, stale, "not the owner, it is X").
- A grant is given only to a game whose copy of the island equals the owner's world (the asker's digest is in the
  TAKE).
- While the owner holds an island, a grab of it by the other player is accepted and ended at once the game's own
  way (the refusal message that makes the Ultrahand part let go). In the first build it is not refused before it
  starts: that left the part in a state whose exit is not read. A stick the owner rides is refused at the seat.
- From the grab frame the asker simulates its copy for at most 1.5 s. A REFUSE or silence puts it back under the
  drive.
- Owner and epoch are in every statement (3.4), so a lost GRANT is corrected by state.
- A claim without a grant: the owner left the session; the owner is loading, or paused and silent for 1 s to a
  request (never a thing the paused player holds or rides); or the island is offered.
- Unload at the owner (an accepted delete that is not positively a destroy): the island is offered; a game that
  has it claims it; with no claim in 2 s the unloaded entity is stated gone. A load offers everything at once.

### 3.4 Structure is state, read from the owner's world
On a structure hook (bond maker, edge removal, death) and once a second, the owner lists the real glue edges of its
bodies and states the island: its entities (id, name, scale, green, owner, epoch, life) and its bonds (id A, slot A,
id B, slot B, relative pose). For an island of two or more parts the history writer's record rides along with an
index-to-id map, as the recipe for creating a new copy whole. The record is never the truth; the owner's world is.
Deaths, toggles and events are addressed by id.

### 3.5 Convergence: one mechanism
- Each game computes a digest of an island from its own world (not from its tables) once a second and sends it in
  the heartbeat.
- A follower whose digest differs asks for the statement and reconciles. An owner that sees a differing digest for
  1 s states the island again.
- reconcile(island) is the only way structure is applied. It creates what is missing (whole, with the game's part
  maker, when two or more bonded parts are missing), deletes a duplicate this game made, replays a death, breaks a
  real edge the statement lacks (main+0x1cf54d4), and makes a stated bond that has no edge with the game's direct
  bond call (main+0x1c05a98) when the two bodies are within 5 cm and 5 degrees of the stated pose. The result is read
  back on the next pass, never assumed.
- When an island still differs after three passes: this game's copy of it is deleted and made whole from the
  record. If that fails the island is "not shown here", retried with back-off, and the reason is sent.
- A follower's failure never changes the owner's world.
- "Seconds not converged", per island and peer, is the health line in the log.

### 3.6 The copy
As in 2.1 and 2.2. Islands of one use the same drive, with no per-tick placing and no periodic teleport. The poses of
a part's further bodies (a wheel, a spring's head) travel in the pose sample, which retires those side channels kind
by kind.

### 3.7 A glue across owners
The gluing game makes the glue natively, holds it as a pending operation, and sends a bond request (operation id,
the two entity ids and slots, the relative pose) to the owner of the other thing. That owner makes the bond with the
direct call and answers yes, giving the merged island to the gluer unless its own Link holds or rides it, or answers
no with a reason. A no breaks the gluer's edge and logs why. A pending operation protects the gluer's edge from its
own reconcile.

The alternative, held in reserve: the gluer takes the other island first, and a glue onto a thing this game does
not own is refused natively (the glue-source test main+0x1c070a0 answering 1 ends the glue on its first tick). Then a
glue can never exist in one world only, but a held or ridden
target refuses the glue and the player must press attach again. The trials of 4.4 decide: one bond seen in one game
only, and the design switches to this. (Chosen by the writer of this plan; the tester has not confirmed it.)

### 3.8 Devices, harm, threads, logs
- The device rules of 2.2, addressed by id. The lone-device subsystem goes.
- A contact counts only in the game that owns the victim. This replaces "drop every contact from a copy's device".
- Structure, ownership and reconcile run at actor phase 0 (main+0xa72b00). The co-op tick keeps transport, the
  heartbeat, the answers to ownership requests while the game is in a menu, and the drive of copies at the cadence
  it was played at.
- Every protocol timer is wall-clock time, so it runs in a paused game.
- Every log line has a wall-clock time and entity ids. The settings in use are printed at start.

## 4. Order of work

Decided with the tester on 2026-11-12: the cleanest route, done right or not at all. The new bookkeeping is written
whole and replaces the old in one change. There is no run of the new tables beside the old system, no repair of the
old bookkeeping, no setting that switches between the two, and no rule for giving up. This is the rule of work of
2.4: read, write everything, then test.

### 4.1 Read (beside 4.2; nothing is written against an unread fact)
The unread facts of section 5, each by a reader, each answer written into contraption-game-facts.md:
- how an established Ultrahand hold is ended from the SPR side, and what the Ultrahand part then does;
- what a refused edge removal leaves behind on later frames (the strain check);
- which body slot a bond names on parts with several bodies (wheel, spring, stick);
- which callers of the create request are a player's (capsule, pouch, Autobuild, dispenser);
- the delete request's reason values and their callers;
- what edge +0xef and +0xf1 and the look points do, which the direct bond call does not set.

Two facts cannot be read and are settled by the first test session's log, with the fallback of section 5 written in
from the start: whether the placement hash is equal in both games for objects that are not enemies, and how often a
bond made in place holds in a game that did not do the gluing.

### 4.2 Write the new bookkeeping, in a new file
runtime/coop_ent.c, written to section 3 and containing none of coop_ctn.c's registry logic:
1. The entity table: ids (3.1), the bond set read from real glue edges, islands as connected sets, the digest of an
   island taken from the world.
2. Owner and epoch per entity; TAKE, GRANT, REFUSE, the claims and the unload offer (3.3); the native grab refusal.
3. The statement and the pose sample (3.4, 3.6), sent in arrival-order reliable messages and latest-only slots.
4. reconcile and its ladder (3.5); the digest in the heartbeat; the "seconds not converged" line.
5. The bond request across owners (3.7).
6. Devices, deaths, toggles and events by id; the harm rule (3.8).
7. An Autobuild announced whole; join (a statement of everything owned, on request).
8. Two test settings that stay in the code: drop every fifth reliable message; break one edge of a copy.

Before any of it is written, each item gets a specification in this file's companion,
[contraption-sync-v4-spec.md](contraption-sync-v4-spec.md): the data structures, the message layouts, the function
of the game or of our code each step calls, the thread it runs on, and its pass test.

### 4.3 Wire it to what is kept, and remove what it replaces, in the same change
- Kept and called from the new file: the copy maker (make_pass's recipe), follow_rigid, coop_g1 and the gate lists,
  the device follow code by kind, the stand-in veto, the glue guards, coop_ctr.c. Where these live in coop_ctn.c
  beside registry code they are moved, not copied.
- Removed from the build (they stay in git): in coop_ctn.c the registry, versions and their number rules,
  record_pass, in_place, edits_pass, orphans, the made / adopt / original flags, the self-heal, the lone-device
  subsystem, TAKE / GRANT / RELINQUISH / PULL as they are; coop_glue.c and coop_bonds.c and their hook registrations.
  coop_players.c keeps all its code: the mirror skips entities and hands an object over when it is glued
  (specification 4.4 and 11).

### 4.4 Test, when all of 4.2 and 4.3 is in
Each run needs the tester's yes. The script: spawn three fans, glue two, hand over, glue the third, hand over, glue
onto the other player's build, pull apart. Then:
- ten trials each: a glue onto the other player's loose object, onto an idle build, onto a build its owner holds,
  with the owner in a menu; both players grabbing one fan, and one build. Never a bond in one game only (3.7);
- with each of the two test settings on: equal worlds within 3 s, counted by the "seconds not converged" line;
- an Autobuild near and 30 m away by each player; walking away and back; a fast travel away and back; a late join;
- fans, a wheeled build with a stick driven by each player in turn, a spring, a time bomb;
- the open tests of section 6.

A failure in a test is answered by reading and by a change to the design in this file, not by a special case in
the code.

## 5. Game facts this rests on

Read in review 3 (R/sync-v3/review3/critique_feasibility.md section 1):
- The bond maker main+0x1916830 has two direct callers, the glue tick and the direct call, so a hook there sees
  every bond.
- A bond maker that answers 0 sends the glue down the game's own abort exit; the game does not try again.
- The direct bond call main+0x1c05a98 needs no build object, descriptor pool or SPR flag and leaves a glue edge
  (edge +0xec set; +0xef, +0xf1 and the look points are not set, and what they mean is not read).
- A grab is refused by main+0x9fb7fc answering 1.

Read on 2026-11-12 for this design (contraption-game-facts.md section 49; static reading, nothing run):
- A hold is ended from outside by the game's own refusal message to the Link; Ultrahand never ends one by itself.
- Every removal of a glue edge passes the graph's removeEdge main+0x2995024. The strain check can be switched off
  per bond, and nothing else depends on it.
- A bond made by the direct call equals a hand glue but for its look (one byte), pulls its bodies into place, and
  checks neither the 22-part limit nor a missing body.
- A map actor is known at its create by its placement record; a device from a capsule and an Autobuild part by the
  return address of their create. A second create function exists beside the one hooked.
- The delete request's reason tells a destroy (0) from an unload (3, 5, 6, 7, 10). A run-time part that unloads is
  never made again by the game.

Still unread, each with its rule in the specification's section 14: a dispenser's create and drops (the present
nearness rule, counted); the teardown of a load or fast travel (every delete while loading is an unload); whether a
new copy is ready for a bond on its first frames (a wait of one frame and a read-back); the aim highlight.

**Settled only by a test session:** whether the placement hash is equal in both games for objects that are not
enemies (until then map objects keep the present key), and how often a bond made in place holds in a game that did
not do the gluing (it had been seen once before these readings). If it fails often, every repaired glue shows as a
remake of the copy. The design stays correct; section 7 then moves forward.

## 6. What this does not fix

- A copy is still a second live simulation behind an incomplete list of gates (about 45 live hooks).
- A new copy of two or more parts is still made with the receiving player's Autobuild object, which waits while
  that player has a design selected.
- Stake and hover state across a hand-over; a stake or an Autobuild pull-in across owners; a rider's weight and
  push; status effects.
- Three players are covered by the rules and have never been run.

Open tests carried over from v3, run in 4.4:
- A glue of one player's object onto the other's loose object, onto a build, and onto a build its owner holds.
- Three fans spawned with the other player's stand-in beside them: all three in both games.
- A menu opened in one game while the other player is near its build or loose device: the other game runs it after
  1 s, and a hit switches the device.
- A hit with an empty or recharging battery does not switch a device on, and the other battery does not pay.
- A player standing on and riding a build the other game owns: on the build in the other game, not trailing; a plank
  or balloon build dips under them; the stand-in's pose on a stick.
- Walking away and back: the build is made again only once well inside its parts' radius, and not deleted at once.
- Four or more builds switched at once: every device switches.
- An NPC vehicle, if one comes along.
- A game that leaves and joins again under a new incarnation (not written in v3).

## 7. The later experiment: a copy the game does not simulate

In one game, with no network: (E1) a bonded build with every body keyframed and moved by per-body velocity stays one
group; (E2) a Link is carried by it; (E3) plain create, keyframe, direct bond, then a switch to dynamic gives a sound
group. No keyframed bonded build has been run on purpose, and the accidental ones fell apart, so this is not on the
main path. If all three pass, the copy becomes keyframed with per-body poses, most gates of 2.2 go, and nothing in
sections 3.1 to 3.5 changes. Run after 4.4, or earlier if the in-place bond of section 5 fails often.
