# Contraption sync v4: the current design

Status, 2026-11-12: a plan. Sections 3 and 4 are not written or run. Section 2 describes code that exists and has
been played. This file is the design to work from; it is kept short on purpose.

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
Every grab-able actor (an SPR, minus the names coop_players.c prop_skip leaves out) is one entity with one id for
life: (minting incarnation, serial). A loose fan and a fan in a build are the same thing. Arrows, beams, the
paraglider and fused gear stay a small effects list in the object mirror.
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
- While the owner holds or rides an island, the other game refuses the grab natively before it starts
  (main+0x9fb7fc answering 1 from the grab handler).
- From the grab frame the asker simulates its copy for at most 1.5 s. A REFUSE or silence puts it back under the
  drive.
- Owner and epoch are in every statement (3.4), so a lost GRANT is corrected by state.
- A claim without a grant: the owner left the session, or has been paused or loading for 1 s.
- Unload at the owner (a delete request this code did not make, reason 3, 4 or 6): the island is offered; with no
  taker in 2 s its entities are stated gone.

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

The alternative, held in reserve: the gluer takes the other island first, and the bond maker (main+0x1916830)
refuses a bond unless this game owns both ends. Then a glue can never exist in one world only, but a held or ridden
target refuses the glue and the player must press attach again. Step 3's trials decide: one bond seen in one game
only, and the design switches to this. (Chosen by the writer of this plan; the tester has not confirmed it.)

### 3.8 Devices, harm, threads, logs
- The device rules of 2.2, addressed by id. The lone-device subsystem goes.
- A contact counts only in the game that owns the victim. This replaces "drop every contact from a copy's device".
- Everything that reads or changes the game's world runs at actor phase 0 (main+0xa72b00). The co-op tick keeps
  transport and heartbeat.
- Every log line has a wall-clock time and entity ids. The settings in use are printed at start.

## 4. Order of work

Each step ends in one two-game session with the same script: spawn three fans, glue two, hand over, glue the third,
hand over, glue onto the other player's build, pull apart.

1. **See it; no behaviour change.** Time, settings and member ids in the log. A shadow entity table beside the
   present system: ids, the bond set from real edges, the digest in both games, exchanged in the heartbeat. Counters:
   seconds the digests differ; a build minted from members of a known build; a bond broken by an apply; a retire that
   deleted nothing; a TAKE unanswered for 1 s. Log lines that settle readings: the placement hash of each moved map
   object, body bits 24 and 26 of each copy, the reason and caller of every entity delete.
   *Passes when the counters fire where run 8 failed (v3 section 48.1). If they do not, the diagnosis is wrong and
   nothing more is built on it.*
2. **Stop the fork, in the present files.** Arrival-order receive. A taken loose object keeps its number, and the
   notice is reliable with an epoch. The old owner does not delete an actor its new owner lists as a part.
   record_pass never mints a build from members of a build another game runs. in_place asks again instead of giving
   up while a part is still listed by its owner. The shape hash is taken over sorted ids. The self-heal applies only
   the version the runner's STATE names. heard_us is stamped after the version test.
   *Passes: one build id from the first glue to the pulling apart, the second glue stays, step 1's counters at 0.*
3. **An answer for every request.** TAKE with GRANT or REFUSE for builds and loose objects alike; the native grab
   refusal; the bond request of 3.7.
   *Passes, ten trials each: a glue onto the other player's loose object, onto an idle build, onto a build its owner
   holds, with the owner in a menu; both players grabbing one fan, and one build. Never a bond in one game only.*
4. **The single model.** Statements and reconcile replace versions; islands of one under the drive; one thread.
   Deleted when their replacement has been played, not before: in coop_ctn.c the versions and their number rules,
   in_place's give-up ladder, edits_pass, orphans, the made / adopt / original flags, the self-heal by time windows,
   the lone-device subsystem; in coop_players.c the entity half of the mirror (obj_take_over's new number, the
   lower-id rule in props_receive, taken[], place_copy and force_to for entities); coop_glue.c and coop_bonds.c.
   *Passes with a switch that drops every fifth reliable message and one that breaks a follower's edge: equal worlds
   within 3 s, counted.*
5. **Autobuild and lifetime.** An Autobuild announced whole; offer, gone and join; fast travel away and back.
6. **Devices.** By id; the harm rule; further bodies from the pose sample.

## 5. Game facts this rests on

Read in review 3 (R/sync-v3/review3/critique_feasibility.md section 1):
- The bond maker main+0x1916830 has two direct callers, the glue tick and the direct call, so a hook there sees
  every bond.
- A bond maker that answers 0 sends the glue down the game's own abort exit; the game does not try again.
- The direct bond call main+0x1c05a98 needs no build object, descriptor pool or SPR flag and leaves a glue edge
  (edge +0xec set; +0xef, +0xf1 and the look points are not set, and what they mean is not read).
- A grab is refused by main+0x9fb7fc answering 1.

Not read, each with its fallback:

| Fact | Fallback if it cannot be relied on |
|---|---|
| How an established Ultrahand hold is ended | The held part's exemption from G1 is withdrawn, so the drive wins |
| The placement hash is equal in both games for objects that are not enemies | Map objects keep the present key |
| The values of the delete reason, in a run | The present time window |
| A refused edge removal is clean | It is allowed, and reconcile makes the bond again |
| The body slots of parts with several bodies | A wrong body is found by the digest and the island is remade |
| Which create callers are a player's | The present nearness gate, with a counter |

**Seen once only:** a bond made in place in a game that did not do the gluing. If that fails often, every repaired
glue shows as a remake of the copy. The design stays correct; section 7 then moves forward.

## 6. What this does not fix

- A copy is still a second live simulation behind an incomplete list of gates (about 45 live hooks).
- A new copy of two or more parts is still made with the receiving player's Autobuild object, which waits while
  that player has a design selected.
- Stake and hover state across a hand-over; a stake or an Autobuild pull-in across owners; a rider's weight and
  push; status effects.
- Three players are covered by the rules and have never been run.

Open tests carried over from v3, to run after step 4:
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
sections 3.1 to 3.5 changes. Run after step 4, or earlier if the in-place bond of section 5 fails often.
