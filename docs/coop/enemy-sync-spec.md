# Enemy sync: the specification

Status, 2026-11-15: first draft, not reviewed, nothing of it built. It turns [enemy-sync-design.md](enemy-sync-design.md)
(the design, D below) into data, messages and procedures for new files `runtime/coop_enemy*.c`. Where this file and D
disagree, D is corrected first. Before any code it is reviewed by readers that did not write it: against the code it
builds on, against the game facts, and by walking play scenarios by hand, as the contraption specification was.

Conventions. Addresses are TotK 1.4.3, main+offset. "The world" is the game's memory; "the table" is ours. An
incarnation is a game's 64-bit id for this session (`coop_ctr_me`). Every timer is wall-clock time; only state times
use the world clock. [m] marks a statement measured in a test run (D sections 4, 5.1, 6.3 name the runs), [inf] an
inference of a reading, [open] something not read, with the rule that holds until it is. "Here" means in this game.
"This Link" is `coop_player_actor()`; the other players' stand-ins are Player actors too. `comps(a)` is [a+0x228];
`BSA(a)` is [comps+0x158].

## 0. Scope and order of building

Everything in D is in scope. The order of building, each step ending in a bench pass (section 15) before the next:

1. the record and the bench (section 14, 15), with the old enemy sync off;
2. identity, declaration and ownership of placed and spawned enemies (3, 4), copies made and let go (10.1);
3. the copy switch, motion and animation (5, 6, 7): a copy walks, runs and idles as its owner's enemy does;
4. hits on an enemy, its reactions, ragdoll, life and death, loot and kill flags (8, 9, 11);
5. an enemy's attacks on players, their outcome, projectiles (8.4 to 8.6), targets and awareness (9.4, 12);
6. what an enemy holds (10.2);
7. run-time spawns, the spawner, the blood moon (10.3 to 10.5);
8. the families of D section 12, one at a time, each with its own section added to this file when its reading is
   done: riders, flyers and swimmers, odd bodies, swarms and summoners, multi-part giants, dungeon bosses, dragons.

Steps 1 to 7 are specified below for enemies of the Bokoblin shape: one actor with a character controller, graphs as
D section 4 lists, attacks armed from animation events or by a stated arming call. An enemy kind outside that shape
is **not synced** until its section exists: each game keeps its own, as with no enemy sync at all, and the record
says so once per kind. The list of kinds in the shape is data (`coop_enemy_kinds.inc`, from the census), not a guess
at run time.

Out of the old code (`runtime/coop_world.c`): its enemy logic is off from step 1 (one setting, default off) and is
deleted when step 5 passes. What other code needs from that file moves first, unchanged (section 16).

## 1. Threads and moments

- **Phase 0** (`coop_enemy_phase0`, from the hook on main+0xa72b00 after the entity sync's): once a frame, outside the
  parallel actor update. All of the table, all reliable messages, ownership, declarations, making and letting go of
  copies, the world calls that need every link to resolve.
- **Pre-calc** (`coop_enemy_precalc`, at the entry of the actor's calc main+0x41bb20, x0 the actor, on that actor's
  update thread): for a copy, everything that puts the owner's state into the actor before the game's own update
  reads it (5.3, 6.3, 7.3); for an owned enemy, the read of last frame's finished state (6.2, 7.2). An actor is
  found by a published map (actor -> table index) that phase 0 writes and pre-calc only reads.
- **Hooks** at the function entries of `coop_hooks.h` `COOP_HOOKED_EN`, on any game thread: they read the published
  map, turn a runner away or capture an event into the event ring (1024 entries, lock-free, as the entity sync's),
  and never touch the table.
- **The net tick** (beside `coop_ctr_tick`, about 30 a second, also in menus and loads): heartbeat flags, answers to
  TAKE while paused, repeats that only send.

`en_lock` guards the table; phase 0 holds it for its pass. Pre-calc and hooks never take it: what they need is in
the published map and in per-enemy **mailboxes** (one for each direction: phase 0 -> pre-calc "apply", pre-calc ->
phase 0 "captured"), each a sequence-numbered copy that is written whole and read whole.

### 1.1 The order inside phase 0
1. reliable messages, in arrival order; then the latest-only slots (STATE, BONES);
2. the event ring: captures of this game's owned enemies become messages owed; captures on copies become requests
   to their owners (HIT, OUTCOME);
3. the actors: liveness of every table entry's actor (state byte, generation), new enemy actors seen by the calc
   hook since the last pass (4.1);
4. ownership: timers, offers, claims (4);
5. declarations and statements owed, then STATE for every owned enemy that is due (6.1);
6. copies: make, adopt, let go (10.1);
7. the apply mailboxes for pre-calc; the published map.

### 1.2 Paused and loading
As the entity sync: PAUSED when phase 0 has not run for 0.3 s; LOADING from the game's teardown flag or with no
resident Player. While LOADING here nothing is made, adopted or taken over; every owned enemy is offered once
(OFFER_ALL), the table is kept, actors are let go as the game deletes them.

## 2. Data

Sizes: EN_N 256 (64 synced enemies in range is the largest placed case, D 12; the rest is room for spawns, parts
and the dying), KIND_N from the census, SNAP_N 32 states per enemy, SLOT_N 6 animation slots, CTRL_N 4 blend
controllers per slot, BB_N 24 blackboard values, EQ_N 8 equipment slots, PROJ_N 128, EV_N 1024. A full table: the
action that needed the slot is not done, a counter rises, one log line per 10 s names the table.

```c
typedef struct { uint64_t a; uint32_t b; } Enid;   // a placed enemy: a = placement hash, b = its life number (>= 1);
                                                    // a run-time one: a = its maker's incarnation, b = serial | 0x80000000
enum { EN_FREE, EN_LIVE, EN_DYING, EN_DEAD, EN_GONE };          // life of the enemy as its owner states it
enum { CP_NONE, CP_WANTED, CP_MAKING, CP_HERE, CP_LETGO };      // this game's actor for a foreign enemy
typedef struct { int64_t us; float pos[3], quat[4], vel[3]; uint8_t move, flags; uint32_t ground; float gpos[3], gquat[4]; } EnSnap;
    // flags: 1 teleport, 2 landed, 4 left ground, 8 relative to `ground` (then gpos/gquat are in its frame)
typedef struct { uint8_t cur, n; uint16_t node[CTRL_N]; float clock[CTRL_N], weight[CTRL_N], rate; uint32_t cmd; } EnSlot;
typedef struct {
    Enid id; uint64_t owner; uint32_t epoch; uint8_t life, copy, kind_i, pack_n;
    char name[48]; uint64_t hash;                 // the actor name its owner states; the placement hash, 0 run-time
    Enid pack;                                    // the pack's root id (its own when alone)
    uint64_t actor; uint32_t gen;                 // the actor here, 0 none; the generation it was bound at
    int32_t hp; uint32_t verb; char form[24];     // life points, brain verb, form key
    EnSnap snap[SNAP_N]; uint8_t snap_w;          // a ring, newest at snap_w - 1
    EnSlot slot[SLOT_N]; float bb[BB_N]; float time_rate;
    uint8_t attn[64]; uint8_t rag;                // attention reason bytes; ragdoll state
    uint64_t target_inc; float look[3]; uint8_t look_on;   // battle target (a player's incarnation, 0 none), look-at
    int64_t stated_ms, seen_ms;                   // last declaration sent / last state taken
    uint32_t decl_no;                             // counts this enemy's declarations by its owner
} En;
```

Hashing and comparing of `Enid` is field by field. The command of a slot travels as a 32-bit FNV of its name; the
name itself travels once per (sender, kind, hash) in the play event that first uses it.

## 3. Identity

3.1 **Placed.** `hash` = [[actor+0x370]+0x88] (0 or -1: not placed). The life number is 1 for the first actor a game
sees at a hash in a session and is raised by the owner at every later actor at that hash (a respawn after a blood
moon or a reload). Two games that both see a first actor claim it with life number 1; the claim rule (4.2) gives one
owner, whose number counts from then on. A game whose own actor is at a lower life number than the owner states lets
its actor go and makes the stated one (it holds the enemy that died).

3.2 **Run-time.** Minted by the game that creates it, at the create's capture (10.3): (incarnation, serial with the
top bit set). Never derived from a name or a place.

3.3 **Kind and name.** The owner states the actor name. A game holds an enemy as a copy only with an actor of that
name; 10.1 says how it gets one.

3.4 **Actors come and go.** An entry is bound to an actor with the actor's generation ([actor+0x20] word the entity
sync uses); every use of `actor` checks state and generation first. Actor addresses are reused within a second.

## 4. Ownership

4.0 **One rule.** `en_apply_owner(e, owner, epoch)` is the only code that changes owner or epoch from a message; it
takes the pair only if greater than the entry's by (epoch, then incarnation). Every message that names an owner goes
through it. Ownership is per pack: the rule is applied to every member, and a message for one member is taken as
for the pack.

4.1 **Seeing an enemy.** The calc hook publishes every actor it sees whose name is in the kind list. At phase 0 a
new one is: matched to an entry by hash and life number (a copy adopted, 10.1, or an own actor of an owned enemy
after a reload); else, if an enemy with that hash is stated by another game at the same life number, held as that
game's copy (the switch on from its next calc, 5); else claimed: owner this game, epoch 1, declared at once.
An actor whose owner is not yet known (a declaration may be on its way) is held unthinking for at most 0.5 s before
the claim: a foreign owner known within that time wins without a fight over two half-run brains.

4.2 **Two claimers** are settled by 4.0. The loser's actor becomes a copy in the frame it takes the winner's
declaration: switch on, its own state overwritten by the winner's.

4.3 **The pack.** Members: the enemy; actors whose holder chain ends at it (main+0x1d19380), which are 10.2's and
have no entry of their own; its children made through its actor-holder, shooter (limbs) or summon, each with an
entry and `pack` = the root; the members of its camp (the camp actor's link list, main+0x2af3d48). The root of a
camp's pack is the camp's member with the smallest id. A pack is recomputed when a member is declared or goes.

4.4 **Hand-over.** Causes, and nothing else:
- the owner's game stops updating the root (its calc has not run for 0.5 s while the game runs) or is told the
  actor is being unloaded (the delete request with the unload reason): the owner sends OFFER for the pack. A game
  whose calc runs the root's actor claims (epoch + 1). With no claim in 2 s the pack is stated GONE and the owner
  keeps each entry's last state for 10 minutes: a game that later sees the actor claims it and starts from that
  state when the entry is still there, from the placement otherwise;
- the owner's heartbeat says LOADING, or PAUSED for 1 s, or the owner left: a game whose calc runs the root claims;
- TAKE: a game may ask (reason 1: this game's player is the battle target and the owner's player is beyond 60 m of
  the enemy for 3 s). The owner answers GRANT or REFUSE(busy) by 4.5. This is the only cause that follows a player,
  it exists so that a fight left to one player is that player's, and it is off until the bench of step 5 shows it
  is wanted.

4.5 **Not in the middle.** An offer or grant by choice (TAKE) waits while any member is in an interrupt action
(main slot status byte BSA+0x26a8 is 1 or 2), in ragdoll, or dying. A forced one (the owner cannot go on) does not
wait.

4.6 **Becoming the owner of an actor that was a copy**, in pre-calc of the first frame: the body dynamic again, the
switch off with what its writes overwrote put back (5.2), the brain verb as last stated, with force; target slot 0
as last stated; the game's own BSA reset (main+0xa12a48) [open: its argument and whether it is needed; rule until
read: not called, the brain's next burst starts from the stated verb]. Then the first STATE and a declaration at
once.

4.7 **Becoming a copy of an actor that was owned**: in the frame the grant is sent: the switch on, the body static,
the mailboxes cleared.

## 5. The copy switch

5.1 **Hooks** (D section 4; all [m] on a Bokoblin for 30 s: no node ticked, the enemy stood and animated). For an
actor that the published map says is a copy, at the entry of:

| Function | Actor | Does |
| --- | --- | --- |
| main+0x1920 (node tick) | [[[x0+8]+8]+0x1f8] | returns; counted as a leak |
| main+0x50b404, main+0x42e0c8, main+0x2beb80 | [x0+0x1f8] | returns |
| main+0x44ff30 (scheduler) | [x0+0xb70] | returns |
| main+0x50ad74 (action request) | [x0+0x18] | returns 2 |

The node tick's test is three loads and a lookup in a 256-entry open-addressed set of copy actors, and is skipped by
one global flag while this game holds no copy.

5.2 **Writes in pre-calc**, every frame for a copy, with ctrl = [[BSA+0x2618]+0x18] and inst = [[ctrl+0x20]]: on the
first frame the four values are saved in the entry; then [ctrl+0x19c] = -1, (i16)[inst+0xde] = -1, [inst+0x90] = 0,
(u16)[inst+0xd8] bit 0 clear. Restored at 4.6 (queue count and the bit word; [m] the interrupt graph then runs
again). Also held every frame: BSA+0x5c = 0, BSA+0x64 = 0, BSA+0x74 = 1, the five slot status bytes BSA+0x26a8.. = 0,
the verb id of action slots 0 and 1 ([BSA+0x1f0+8k]+0x28) as the owner states them, bit 0 of each slot's +0x50
clear, no stop timer ([comps+0x2e0]+0x20, +0x24 = 0).

5.3 **Stated by the owner and applied in pre-calc on change:** the brain verb (main+0xad9844(BSA, verb, 1)); the
form key (the string at BSA+0x2628, by the game's setter [open: its address; rule until read: the form key is not
applied and the record says which copies differ]).

5.4 **Gates that are not graphs.** On a copy these return without acting, each at its entry, by the actor it is
for: equipment re-create main+0x1b2bf1c and unequip replacement main+0xb299bc (10.2), the drop manager's drop
main+0x1c77af8 and the defeat counters main+0xb28394 (9.3), the actor-holder create main+0x1b11d38 (10.3). The death
request main+0x1b4b738 on a copy is refused unless the mod makes it (9.2).

5.5 **The proof.** Per copy and frame: entries turned away, by hook; node ticks and node enters (main+0x1bb410)
that reached a copy's controller. The second and third are 0 or the record names the caller.

## 6. Motion

6.1 **STATE** (unreliable, latest per (sender, enemy)), sent by the owner from phase 0: every frame for an enemy
that moved more than 1 mm or turned more than 0.1 degree since its last state, or has a player within 60 m; else
once a second. It carries one `EnSnap` stamped with the owner's world clock at that frame, the life points, the slot
clocks of 7.2 and the look-at point.

6.2 **Read at the owner**, in pre-calc of the next frame (the state of the frame that just ended, all phases
done): position and rotation of the actor, the main body's linear velocity, the controller's move state and the
landed / left-ground edges ([inf] MoveStates+0x58/+0x59; read once and cleared by the game, so read in the hook on
main+0x240440's exit side [open]; rule until read: edges are derived from the move state changing), the body the
enemy stands on (its entity or enemy id; then the pose is also given in that body's frame).

6.3 **Applied at a copy**, in pre-calc:
- the main body is static (main+0xae4300(body, 0), asked once and again whenever the body or its motion type
  changed; body = [[[comps+0x50]+0x20]+0x150]);
- the pose for owner time T = now_owner - delay is interpolated from the ring: a cubic on position through the two
  states around T using both velocities, a slerp on rotation; a state flagged teleport is not interpolated across;
  with no state after T the newest is held; with no state at all the copy stays where it was made;
- ForceSetMatrix (main+0x45ce70) with that pose ([m]: the actor and body are there the next frame, to the
  millimetre, for a static body; a keyframed one sinks);
- the controller's forward direction (main+0x298ce54) and speed (main+0x1ba5c8) from the state's velocity, and the
  move state, so local readers agree [open: the move state's setter; rule until read: not written];
- relative states (flag 8) are turned into the world through the ground's pose here, at the same T.

6.4 **Clock and delay.** now_owner is this game's estimate of the owner's world clock, from the heartbeat's
(world_us, wall_ns) pairs as the entity sync's drive has it. delay = clamp(jitter + one state interval, 50 ms,
150 ms), jitter being the 95th percentile of the last 2 s of arrival-time deviations for that owner; it changes by
at most 1 ms a frame.

6.5 **Animation-driven movement off** on a copy: [open: the component's switch; rule until read: the pose written
every frame overrides it, and the record counts frames where the actor was not at the pose asked].

6.6 **Ragdoll.** `rag` = the state int at [comps+0x3e8]+0x20, in every STATE. While the owner's is 2 or 3 the owner
sends BONES (unreliable, latest; every frame): the root pose and the local rotation of each bone as 3 x 16-bit, the
bone count as the copy's model has it. The copy's ragdoll is never started; its bones are overwritten in the hook
after main+0x283c750 while BONES under 0.3 s old exist. Enemies without a ragdoll component send none.

## 7. Animation

7.1 **Play events** (PLAY, reliable, in order per enemy). At the owner, the hooks on main+0x4dc628, main+0x2935064
and main+0x2935084 capture, for an actor that is an owned enemy: slot, name, rate, blend, start, the three flag
bytes, which of the three functions, and the world clock. main+0x2934934 (slot remove) and the three partial-change
calls are captured as their own kinds. A request the game refuses (answer 0) is not sent: the capture is completed
at the function's return [open: a return-side hook does not exist; rule: the capture is sent, and the copy's own
call is refused or not by the same test on its own slot].

7.2 **Slot state**, in every STATE: for each slot with a command, the current controller index, and per live
controller its node id, clock and weight, the slot's rate; read in pre-calc with 6.2.

7.3 **Applied at a copy**, in pre-calc, after the pose:
- PLAY events whose time is at or before T, in order: the same call with the owner's arguments, the name from this
  game's copy of the string, node 0 ([m]: the command changes at once and the clip starts in that frame's update);
- then, per slot whose command equals the owner's at T: the clock error e = owner clock at T - this controller's
  clock (clocks are in 30ths of a second [m]). |e| under 0.5: nothing. Under 3 (100 ms): the slot's rate is set to
  the owner's rate x (1 + clamp(e / 30, -0.1, 0.1)) until it closes. Over: the clock is written with the no-advance
  flag (ctx+0xa0 |= 0x400) [m] and the record counts a hard correction. Weights of live controllers are written
  when the current index differs;
- the blackboard: each value that differs from the owner's by more than 0.5 (floats) or at all (enums), by the
  game's setter (the table's virtual +0xa8 by name);
- the time rate: actor+0x280 [open: whether the game rewrites it every frame; rule: written every pre-calc].

7.4 **Other stated values** (EVENT, reliable, on change; applied in pre-calc at their time): look-at requests
(main+0x1bae4ac / +0x1bae5f0 / main+0xc061cc with the owner's arguments; the look point travels in STATE and is
written into target slot 8); model controller ice amount, death effect and hide mask (main+0x1b87a9c, +0x1b87b04,
main+0xbb0dd0); the attention reason bytes (written whole into [comps+0x130]+0x24.., 64 bytes); the dodgeable-attack
bit ([comps+0x268]+0x34 bit 7); condition slot flags and the chemical state ints (D 8.2).

7.5 **A copy made late** gets, in its declaration, every slot's command and state: the commands are played first.

## 8. Hits

8.1 **A hit on a copy.** The sensor pass main+0x303be0 builds the victim's hit list. For a copy, at the hook after
it (the entry of main+0x315dd4, the next call in phase A [inf from V5; rule: the entry of main+0x300a30 if that is
not so, with the count restored there]): every entry is copied out (0x1c0 bytes), the list count is set to 0, and
the entries go to phase 0 through the event ring. Phase 0 sends each as HIT (reliable) to the owner with its
game-local fields rewritten: the two actor links (+0x60, +0x78) as ids (a player's incarnation, an enemy id, an
entity id, a mirrored object's id, or none), the body pointer (+0x58) as the victim's body index, the attack id
(+0x38) and stealth pair (+0x1a4, +0x1a8) as (attacker id, serial).

8.2 **At the owner**, the HIT is queued for the enemy; in the hook at the entry of main+0x315dd4 for that actor
(right after its own sensor pass) the queued entries are appended to its list with the ids turned back into this
game's links (an attacker that has no actor here: the entry's link is the stand-in or null, and the damage is
applied all the same), and the number appended is remembered; at the entry of the next phase A for that actor the
count is checked to be 0 again (the game clears it in its own sensor pass; if the frame was skipped the appended
entries are removed so they are not counted twice).

8.3 **Life of a copy.** Never changed here by a hit: the copy's damage is taken away in 8.1 before the game adds it
up. Its life points are written from STATE in pre-calc ([comps+0x2f8], the life cell [open: its exact field; rule:
the health bar's source is found by a run before step 4 is built]).

8.4 **A copy's attack on this Link.** A copy arms its attack sensor from its own animation events
(main+0x42ec98 -> main+0x1bf20cc) for the kinds that arm that way. For kinds the census lists as arming from a
node, the owner captures main+0x1bf20cc for its enemy (the record's fields and the caller) and sends ARM; the copy
makes the same call at that time. A copy's hit on anything that is not this Link or this Link's own (its mount, its
contraption) is removed by the contacts filter the old code has for copies.

8.5 **The owner's enemy and a stand-in.** A hit of an owned enemy on another player's stand-in does nothing here
(the stand-in takes no damage). The other player's game decides (8.4) and, when its Link was touched by the copy's
attack, sends OUTCOME to the owner: enemy, attack serial, class (hit, guarded, just-guarded, dodged), the entry's
guard fields, direction, power. The owner turns guarded and just-guarded into a hit entry on its enemy with the
attribute bits of the guard cell and writes the guard record (L+0xfc, L+0x104 bit 1; L = [comps+0x2f8]) before the
enemy's next calc [open: whether the entry route fills the record; rule: both are done].

8.6 **Projectiles.** At the owner, the shooter's create and the lifter's throw are captured at the create request
(main+0x599670 with a creator that is an owned enemy) as PROJ (reliable): id, name, launch pose, velocity, clock
time, the create arguments' init values. Every game makes its own actor and flies it. The game of a Link it hits
sends PROJ_END (hit, with the place); a game where it hits the world sends nothing; the owner's own end (its actor
died) sends PROJ_END (gone) and the others delete theirs. Until then a projectile is in the shared object sync's
table as the owner's object, not synced in flight.

## 9. Life, death and awareness

9.1 Life points: 8.3.

9.2 **Death.** At the owner, the death request's hook (main+0x1b4b738) on an owned enemy, accepted, sends DEATH
(reliable, to all): cause, second cause, clock time; the entry is DYING. At a copy, at that time: D+0x2c bits 0 and
1 set, bit 7 cleared, the same request with both causes, bit 7 set again; the Die action's animation and bones
arrive as 6 and 7. When the owner's death tick ends (the entry's actor is gone) the owner sends DEATH_END; the copy
gives its death controller the end signal [open: the call the node ExecuteDeathSignalDeathProcedure makes; rule
until read: the copy's actor is deleted by request 1 s after DEATH_END, faded by its model controller's death
effect value as stated]. The entry is DEAD, kept 10 s against late messages, then freed.

9.3 **Loot and counters.** The owner's drops happen as the game makes them; each dropped actor enters the shared
object sync as the owner's object (the mirror's own event at the create, not its 6 m scan). A copy drops nothing
(5.4, D+0x2c bit 0). The defeat counters are added by the owner's game only (5.4).

9.4 **Awareness at a copy**: the attention bytes, the look-at (7.4); target slot 0 written in pre-calc as the
stated target's actor here (this Link, a stand-in, or cleared), with its kind byte, through main+0xa70a1c; the
notice component's flag word [comps+0x140]+0x40 from EVENT. The marks over the head and the battle music are checked
in the bench of step 5 against the owner's picture, and read then if they differ.

9.5 **Requests to the player's game** made by an owned enemy's graph (camera shake, rumble, boss gauge, weather):
captured at their engine calls [open: the calls; the node list is in the census], sent as EVENT and made by the
other game when its Link is within the request's own range. Not built before step 8.

## 10. Coming and going

10.1 **A copy here.** For every LIVE enemy another game owns whose newest state is within 120 m of this Link
[run: the range at which the game itself shows an enemy] and that has no actor here: `copy` = WANTED. Phase 0 then,
at most 2 a frame:
- adopts: an actor here at the stated hash with the stated name and no entry of its own;
- replaces: an actor here at the stated hash with another name is put away (the placement's own "dead" call
  without the save's flag [run 7 of D; rule until it is found: the enemy is not synced at this place, each game
  keeps its own, and the record says so]);
- creates: main+0x599670 with the stated name at the pose of time T; the handle claimed as the entity sync does;
  on the first pre-calc the switch is on and the declaration's state applied.
A copy is let go (deleted by request, `copy` = LETGO then NONE) when its enemy is beyond 150 m of this Link for 2 s,
or stated GONE. An adopted placed actor is not deleted: it is given back to the game (switch off, body dynamic)
only when its enemy is stated GONE; while its owner lives it stays a copy however far.

10.2 **Equipment** (step 6). At the owner, main+0xaf100c for an owned enemy is captured with all its arguments
(EQUIP create); main+0x4f59ec (equip, drop), the drop and throw records (main+0x1b738e4, main+0x1b73be4), removal
(main+0x1b70a04) and pick-up (main+0x1bb16a0) as EQUIP events with the slot. A copy makes the same calls on its own
equipment user at their time; its own re-create and replacement are gated (5.4). A dropped or thrown weapon leaves
the pack: it is the owner's object in the shared object sync from the event on.

10.3 **Children** (step 7). A create request whose creator link is an owned enemy (holder, shooter limb, summon) is
captured at main+0x599670's entry with the whole argument block, given an id, and declared with `pack` = the
creator's root. Other games make it by 10.1's create with the stated arguments. The same create on a copy does not
happen: its graphs are off and the holder's create is gated.

10.4 **The spawner** (step 7). An auto-placed enemy is seen at the create request with return address 0x538ca8,
minted and declared like any run-time enemy, owned by the game that spawned it. The push main+0xa80f28 returns
without queueing while an entry of the same actor name, owned by another game, has its newest state within 40 m of
this Link [open: the rule's own radius, record+0x14].

10.5 **Kill flags and the moon** (step 7). The owner's call of main+0x195321c for a placement is sent (KILL: the
hash); a receiver makes the same call on its placement object when it has one loaded, and writes the save's flag
through the shared game data otherwise. The reset-mask-80 enemy flags join the shared set in `coop_gamedata.c`.
The first game to enter main+0x21cd650 sends MOON (start); the others call it at phase 0 unless their own is in
progress (world +0x60 bit 20); main+0x21cd698 likewise (MOON end). After a moon every entry's life number rule
(3.1) gives the new actors new ids.

## 11. Messages

Through `runtime/coop_ctr.c`, types 60 to 89. Reliable messages are per peer and in order; what a full queue
refused is owed and said when a send succeeds. Every unreliable message starts with an 8-byte slot key.

```c
typedef struct { uint64_t key; Enid id; uint64_t owner; uint32_t epoch; EnSnap s; int32_t hp; uint8_t rag, nslot, look_on, pad;
                 float look[3]; struct { uint8_t slot, cur, n; uint32_t cmd; float rate; struct { uint16_t node; float clock, weight; } c[CTRL_N]; } sl[]; } EnState;    // 60
typedef struct { uint64_t key; Enid id; uint8_t n; float root[7]; int16_t q[][3]; } EnBones;                    // 61
typedef struct { Enid id, pack; uint64_t owner, hash; uint32_t epoch, decl_no, verb; uint8_t life, kind_i, nslot, neq; char name[48], form[24];
                 uint8_t attn[64]; /* then EnState's body, the equipment rows, the create arguments of a run-time enemy */ } EnDecl;   // 62, bulk
typedef struct { Enid id; int64_t us; uint8_t fn, slot, flags; float rate, blend, start; uint32_t cmd; char name[32]; } EnPlay;        // 63
typedef struct { Enid id; int64_t us; uint8_t kind; uint8_t len; uint8_t d[80]; } EnEvent;                      // 64
typedef struct { Enid id; uint32_t serial; uint8_t entry[0x1c0]; uint64_t link_id[2]; uint8_t link_kind[2]; uint32_t atk_serial; } EnHit;   // 65, bulk
typedef struct { Enid id; uint32_t atk_serial; uint8_t cls; float dir[3], power; uint8_t guard[0x20]; } EnOutcome;  // 66
typedef struct { Enid id; int64_t us; uint32_t cause, second; } EnDeath;                                        // 67; 68: its end
typedef struct { uint32_t req; Enid root; uint32_t epoch_seen; uint8_t reason; } EnTake;                        // 69
typedef struct { uint64_t asker; uint32_t req; uint64_t owner; uint32_t epoch; uint8_t n; Enid m[32]; } EnGrant;    // 70
typedef struct { uint32_t req; uint8_t reason; uint64_t owner; uint32_t epoch; } EnRefuse;                      // 71
typedef struct { Enid root; uint32_t epoch; uint8_t all; } EnOffer;                                             // 72
typedef struct { Enid id; uint32_t epoch; uint8_t life; } EnGone;                                               // 73: GONE or FREE
typedef struct { Enid id; } EnNeed;                                                                             // 74: (0,0) everything
typedef struct { uint64_t key; uint32_t count, hash; } EnIndex;                                                 // 75
typedef struct { Enid id, by; int64_t us; char name[48]; float pose[7], vel[3]; uint16_t len; uint8_t args[]; } EnProj;   // 76, bulk; 77: its end
typedef struct { Enid id; int64_t us; uint8_t kind, slot; uint16_t len; uint8_t args[]; } EnEquip;              // 78, bulk
typedef struct { uint64_t hash; } EnKill;                                                                       // 79
typedef struct { uint8_t start; } EnMoon;                                                                       // 80
```

11.1 **Declaration** (DECL): owed to every peer when an enemy is claimed, when its name, pack, life or owner
changes, on NEED, and on a peer's join. A receiver passes (owner, epoch) through 4.0 and drops a declaration whose
sender is then not the owner or whose `decl_no` is not above the last taken for that id.
11.2 **STATE** is dropped unless its (owner, epoch) is the entry's after 4.0; it is the keep-alive of an enemy: an
entry with no state for 3 s whose owner's heartbeat is running asks NEED; for 10 s, the enemy is treated as offered.
11.3 **INDEX** (unreliable, per peer, every 2 s): count and FNV over (id, epoch) of the sender's LIVE enemies. A
mismatch on two in a row: NEED(0, 0), at most once in 10 s; entries of that sender not declared again within 3 s of
the answer's end are GONE here.
11.4 An enemy id a receiver does not know in PLAY, EVENT, DEATH or EQUIP is kept 2 s for its declaration.

## 12. The target

At the owner, after the game's own target switch ran (the hook at the entry of main+0x1866bf4's caller side
[open: the exact point; rule: pre-calc of the next frame]), for each owned enemy with a battle target: the
candidates are this Link and each stand-in the enemy's receiver holds in its Sight list. The target is kept unless
it is gone, dead or unsensed for 2 s, or another candidate hit this enemy within the last 5 s and the target did
not, or another candidate is nearer by more than 5 m for 3 s. A change is written into target slot 0
(main+0xa70a1c) and stated (EVENT target). The game's own switch between two Player actors is then never seen: its
write is overwritten in the same frame.

## 13. Settings

`TOTK_COOP_ENEMY=1` turns the enemy sync on (default off until step 5 passes; the old code's setting is the other
way round from then). Tests stay in the code: `TOTK_COOP_TEST_ENPROBE` (the runs of D), `TOTK_COOP_EN_DELAY_MS`
(a fixed delay), `TOTK_COOP_EN_NOCLOCK=1` (no clock correction), `TOTK_COOP_EN_NOSWITCH=1` (copies think: only for
comparing).

## 14. The record

In the session record (`logs/totk.log`, `logs/motion.jsonl`, the audit), from step 1:
- `coop: en: change: <id> <name>: ...` every change of owner, epoch, life, copy state, verb, target, pack;
- `coop: en: own: ...` every claim, offer, take, grant, refusal, with its cause;
- `coop: en: leak: ...` a node tick or enter on a copy, with the caller; `coop: en: switch: ...` once a second the
  entries turned away, by hook, summed over copies;
- `coop: en: motion: <id>: ...` once a second per copy near this Link: delay, the count of states used, position
  error against the owner's state at T (after the fact, when the states around T are in), worst frame-to-frame change
  of velocity, snaps, frames not at the asked pose;
- `coop: en: anim: <id>: ...` once a second per copy: per slot the command here and at the owner, the clock error,
  soft and hard corrections; a line at every PLAY applied, with its lateness;
- `coop: en: hit: ...` every hit captured, sent, received, appended, with serial and the life before and after at
  the owner; `coop: en: death: ...`, `coop: en: equip: ...`, `coop: en: proj: ...` every such event at both ends;
- in `motion.jsonl`, ten times a second: every enemy near this Link (id, owner, pose, velocity, slot 0's command and
  clock, life), so the two games' files pair by id and owner clock.
`tools/coop/en_report.py` pairs the two games' records and prints, per enemy: time-aligned position error
(mean, 95th percentile, worst), delay, stutter, snaps, animation phase error, command lateness, hits sent against
hits applied, and every disagreement of life, verb, target or command lasting over 0.5 s.

## 15. The bench and the pass bars

`tools/coop/play_two.ps1 -Bench enemy,<case>`: both games load the bench save beside a placed camp
[open: the save and the camp; a Bokoblin camp with three members on flat ground], game 1's Link plays a script.
Cases: `idle` (60 s, nobody moves), `walk` (game 1 walks a circle in the camp's sight: the camp notices and chases),
`fight` (game 1 swings at the nearest member until it dies), `fight2` (game 2's Link swings), `leave` (game 1 walks
200 m off and back), `load` (game 1 reloads its save mid-fight). Each case ends with the report of section 14 and
pictures from both games every 2 s; a case passes only when the report's bars hold **and** the pictures of both
games show the same enemies in the same places and poses.

| Bar | Step | Value |
| --- | --- | --- |
| leaks (5.5) | 3 | 0 |
| position error, time-aligned: standing / walking / running | 3 | 2 / 4 / 8 cm at the 95th percentile |
| delay | 3 | within 50 to 150 ms, steady within 10 ms over a minute |
| snaps not flagged as teleports | 3 | 0 |
| frames not at the asked pose | 3 | 0 |
| animation phase error | 3 | 33 ms at the 95th percentile; hard corrections under 1 a minute per enemy |
| command lateness beyond the delay | 3 | under 17 ms |
| hits applied / hits sent | 4 | 1, each once |
| life, verb, target, command disagreements over 0.5 s | 4, 5 | 0 |
| one death, one set of drops, one kill flag per enemy | 4 | exact |
| hit reaction onset after the hit, in the hitter's game | 4 | measured; the bar is set from the first run |
| ownership changes in `idle`, `walk`, `fight` | 2 | 0 after the first claim |

The first run of each case replaces a number above only by decision, written here with the run's date.

## 16. Code moved out of coop_world.c first

Unchanged, into files of their own, each with one commit that moves and nothing else: the actor registry and the
key walkers (`coop_actors.c`); the stand-in's animation, pose and trail (`coop_standin_anim.c`); the ground sync
(`coop_ground.c`); the hit-update dispatcher on main+0x300a30 (`coop_hitupd.c`); the HIT channel's demultiplexer
(`coop_peer.c`). The old enemy logic stays in `coop_world.c` behind its setting until it is deleted.

## 17. What is not read, and its rule

Every [open] above has its rule beside it. Beyond those: an enemy kind outside the Bokoblin shape is not synced
(section 0); an enemy standing on something that is neither an entity nor an enemy is stated in the world frame;
two enemies of different owners do not hear each other's noise or messages; a boss gauge, a camera shake and a
weather change reach only the owner's player until step 8.
