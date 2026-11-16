# Enemy sync: the specification

Status, 2026-11-15: second draft, nothing of it built. It turns [enemy-sync-design.md](enemy-sync-design.md) (the
design, D below) into data, messages and procedures for new files `runtime/coop_enemy*.c`. Where this file and D
disagree, D is corrected first (D section 16 lists what the review changed).

History: the first draft was reviewed by three readers that did not write it: against the co-op code it builds on,
against the game facts, and by walking twelve play scenarios by hand (kept outside the repository; cited as RA, RB,
RC). They found the ownership rule unsound, the identity rule contradictory, the hit and death procedures wrong in
several places, the transport and the old code misdescribed, and a third of what the draft called measured not
measured. Every finding is in this draft or in section 18 with its reason. Test runs 9 to 12 (D section 4, 6 and the
notes) were made after the first draft: a placed camp, a fight, and a play test with the switch held by the tester.

Conventions. Addresses are TotK 1.4.3, main+offset. "The world" is the game's memory; "the table" is ours. An
incarnation is a game's 64-bit id (`coop_ctr_me`). Every timer is wall-clock time; state times are on the owner's
world clock. Marks: [m] measured in a test run; [inf] an inference of a reading; [open] not read, with the rule that
holds until it is; [run] to be settled by a named run of section 17. No mark: read in the game's code by a reader.
"Here" means in this game. "This Link" is `coop_player_actor()`; another player's stand-in here is a `PlayerPuppet`
actor. `comps(a)` = [a+0x228]; `BSA(a)` = [comps+0x158]; `L(a)` (life) = [comps+0x2f8]; `DC(a)` (the death
component) = [comps+0x1c8]; `G(a)` (the animation player) = [[[comps+0x40]+0x18]+0x18]. An actor's generation is the
u32 at actor+0x10, its state the byte at +0x24 (`ctn_gen_of`, `ctn_alive_as`).

## 0. Scope and order of building

Everything in D is in scope. The order, each step ending in its bench pass (section 15):

0. **Groundwork** (section 16): the shared message dispatcher, session state (paused, loading, peers) and clock
   estimate; the transport's test network; the old enemy sync behind one setting, gate by gate; the code other
   files need moved out of `coop_world.c`; the bench scene and the record's writers.
1. The table, identity, declarations, ownership and the record, with copies **not** made: each game still runs its
   own enemies, but claims, epochs, tombstones and the report are live and checked (bench: no ownership change after
   the first claim; both tables equal).
2. Copies of placed enemies: the switch, the static body, motion, animation, equipment as declared (sections 5 to 7,
   10.1, 10.2's create). A copy walks, runs, idles and holds what its owner's enemy does.
3. Hits on an enemy, its flinch, big reactions, ragdoll, life, death, loot, kill flags, tombstones (8, 9).
4. An enemy's attacks on players, outcomes, projectiles (8.5 to 8.7); awareness (9.5); the target (12).
5. Equipment events (10.2); run-time enemies: copies by create, children, the spawner (10.3, 10.4); the moon (10.5).
6. Hand-over by request (4.6), if the bench of step 4 asks for it.
7. The families of D section 12, one at a time, each with its own section added here when its reading is done.

Steps 1 to 5 are for enemy kinds of the **Bokoblin shape**: one actor with a character controller and the graph
units of D section 4. The list of kinds in the shape is data (`coop_enemy_kinds.inc`, from the census). An enemy of
another kind, and an enemy at a placement where the two games' saves give different actor names (3.4), is
**unsynced**: each game keeps its own, and the record says so once.

`TOTK_COOP_ENEMY=1` needs `TOTK_COOP_CONTRAPTION=1` (entity ids are used for grounds and hit links).

## 1. Threads and moments

- **Phase 0** (`coop_enemy_phase0`, called from `coop_spawn.c coop_hook` at main+0xa72b00 after the entity sync's
  call, with the register save its neighbours use): once a frame, outside the parallel actor update, where a link to
  any actor resolves. Everything of the table; all messages; **the read of every owned enemy** (last frame is
  finished and every other actor and table is readable: 6.2, 7.2); everything that names another actor (target and
  look links, hit links, a ground's pose) is resolved here into finished values.
- **Pre-calc** (`coop_enemy_precalc`, at the entry of the actor's calc main+0x41bb20, on that actor's update thread,
  called from `coop_hook` before the old handlers of that address): for a copy only. It copies finished values into
  its own actor and makes guest calls on its own actor only (pose, play requests, clocks, the switch's writes).
- **Hooks** at entries (`COOP_HOOKED_EN` and the shared ones of 16.4), any game thread: they read the published map
  and per-actor flags, turn a runner away, or capture an event. A hook that needs the result of the function runs it
  itself and acts after it (the hook calls the function through `rt_dispatch` with a thread-local guard and returns
  nonzero, as `coop_spawn.c` does at main+0x50d6bc); used only when the map says the actor is ours.
- **The net tick** (`coop_tick`, 30 a second, also in menus and loads): the dispatcher's pause queue, heartbeat
  flags, repeats that only send. The transport receives here only: an unreliable message is seen at most 30 times a
  second (16.1).

`en_lock` guards the table; phase 0 holds it for its pass. Nothing else takes it. Between phase 0 and the other
threads there are, per table entry: one **cell** (a sequence-numbered latest value: the pose and clocks to show, the
held fields, links as ready handles) and two **queues** (single producer, single consumer, 32 entries each, the
consumer owns the read index): phase 0 -> the actor's thread (play requests, events, hits to append, the death
request), and the actor's thread -> phase 0 (captures). A full queue: the producer keeps the item and the record
counts it; nothing is dropped silently. The two hooks that consume for one actor (its calc and its damage step) are
never concurrent [inf: both are that actor's own update].

### 1.1 The published map
1024 cells of one 64-bit word each: (actor address >> 4, generation's low 12 bits, table index, 2 bits: owned / copy
/ held). Phase 0 writes it; any thread reads it. Pre-calc compares the full generation once per actor and frame and
sets a per-actor "this frame" byte in the cell the other hooks trust. The delete request's hooks (main+0x63baa8,
main+0x63b6b0) clear an actor's cell at once.

A new actor is seen through the registry's `seen[]` table (16.3), which gets a kind index per cell, set once when
the cell is taken. **An actor of a listed kind that is in no map cell is held from its first pre-calc**: the switch
on, the body left dynamic (4.1).

### 1.2 The order inside phase 0
1. messages from the dispatcher, in arrival order; then latest-only STATE and BONES under 2 s old, pushed into an
   entry's ring only when their time stamp is above the ring's newest;
2. peers that came, left, paused, loaded (16.2);
3. actors: liveness and generation of every bound actor; new actors from `seen[]`;
4. **ownership**: timers, claims, offers, grants (4): settled before step 5 so a capture knows the owner;
5. captures from the queues: of owned enemies into messages owed, of copies into requests to their owners;
6. the read of every owned enemy; declarations, events and STATE owed (6.1);
7. copies: adopt, make, let go (10.1);
8. T for each owner (6.4), fixed for the frame; the cells and queues for pre-calc; the map.

### 1.3 Paused and loading (session state, 16.2)
PAUSED: phase 0 has not run for 0.3 s. LOADING: the game's teardown flag or no resident Player. While LOADING here:
nothing is made, adopted or claimed; **the switch is off for every copy** (hooks pass, pre-calc returns: a load hung
in the old code with copies held); every owned enemy is offered once (OFFER_ALL); the table is kept. After the load
ends: 10.6.

## 2. Data

EN_N 256 live entries; REST_N 2048 resting records and TOMB_N 4096 tombstones outside it (3.3); SNAP_N 16; SLOT_N 6;
CTRL_N 4; BB_N 32; NAME_N 512 command names; PROJ_N 128. A full table: the action is not done, retried once a
second, counted, one log line per 10 s names the table and the enemy.

```c
typedef struct { uint64_t a; uint32_t b; uint32_t zero; } Enid;   // 16 bytes on the wire, `zero` always 0.
    // placed: a = placement hash, b = life number (3.1). run-time: a = maker's incarnation, b = serial | 0x80000000
enum { EN_LIVE = 1, EN_DYING, EN_DEAD, EN_GONE, EN_UNSYNCED };
enum { CP_NONE, CP_WANTED, CP_MAKING, CP_HERE, CP_HELD };          // this game's actor for the entry
typedef struct { int64_t us; float pos[3], quat[4], vel[3]; uint8_t move, flags, teleports, landings, liftoffs, ground_kind;
                 Enid ground; float gpos[3], gquat[4]; } EnSnap;    // counters, not one-time flags (6.1)
typedef struct { uint32_t cmd; float rate; uint8_t cur, n; struct { float clock, weight; } c[CTRL_N]; } EnSlot;
typedef struct {
    Enid id; uint64_t owner; uint32_t epoch; uint8_t life, copy, kind_i;
    char name[48]; uint64_t hash;
    uint64_t actor; uint32_t gen;
    int32_t hp; uint32_t verb, slot_verb[5]; char form[24];
    EnSnap snap[SNAP_N]; uint8_t snap_w; int64_t clock_off; uint8_t clock_ok;   // per entry, reset with the owner
    EnSlot slot[SLOT_N]; float bb[BB_N]; uint32_t bb_key[BB_N]; float time_rate;
    uint8_t attn[64], rag; uint32_t dstate;               // attention bytes, ragdoll state, death state word (9.2)
    uint64_t target_inc; float look[3]; uint8_t look_on;
    uint32_t decl_no; uint64_t decl_owner; uint32_t decl_epoch;    // last declaration taken, with the pair it had
    uint8_t handle[0x40], link[0x40]; int64_t make_ms; uint8_t make_tries;   // a create under way (10.1)
    uint32_t saved_qcount; uint16_t saved_bits; uint8_t saved;     // the switch's writes (5.2)
} En;
```

`Enid` is compared field by field. The slot key of an unreliable message is FNV-1a over the 16 bytes; a receiver
checks the id inside and drops a mismatch (two ids on one key lose one stream; counted). A command travels as the
FNV-1a of its name; names are sent in the declaration of an enemy (all of its slots') and in each PLAY (`name[32]`;
no command name is longer [open: asserted at capture, a longer one is logged and not sent]); a game keeps every name
it has seen in a table whose strings live for the session (the game keeps the pointer a play request gives it).

## 3. Identity

3.1 **Placed.** `hash` = [[actor+0x370]+0x88]; [actor+0x370] null or a hash of 0 or -1: not placed. The life number
is **1 + the number of blood moons this session has stated** (10.5), carried in MOON and in the answer to a join. A
fresh actor at a hash takes the number of the LIVE entry for that hash when there is one, else the session's. There
is at most one entry per hash; a declaration with a greater life number for a hash replaces the entry and rebinds
the actor that is here. Nothing is deleted or re-created for a life number.

3.2 **Run-time.** Minted by the game that creates it, at the create's capture (10.3): (incarnation, serial with the
top bit set). The transport draws a new incarnation at a join [open: its comment plans one at each save load; rule:
a load does not change it, checked in step 0].

3.3 **The dead and the resting.** A **tombstone** (hash, life number) is kept by every game for the session for
every placed enemy that died; cleared by MOON; sent whole to a peer at its join and after its load ends. A new actor
at a tombstoned hash is not claimed: it is put away by the placement's dead call (9.4). A **resting record** (hash,
life number, epoch, pose, life points, verb) is kept by every game for every placed enemy stated GONE (GONE carries
it); a claim of that hash uses the record's epoch + 1 and starts from its state.

3.4 **Kind and name.** The owner states the actor name. A game whose actor at that hash has another name (the level
sensor) declares the hash UNSYNCED to all; no copy is made for it, no kill is applied to it, each game's actor
thinks. [run 19: whether a placed actor can be put away and replaced; until then this rule.]

## 4. Ownership

Ownership is **per enemy**. What an enemy holds, and its children, limbs and projectiles, have no owner of their
own: they follow it (10.2, 10.3). A camp can be split between owners by which game runs each member (D 3.1: members
of different owners do not hear each other).

4.0 **One rule.** `en_apply_owner(e, owner, epoch)` is the only code that changes owner or epoch from a message; it
takes the pair only if greater than the entry's by (epoch, then incarnation). On a change it clears the ring, the
clock offset (6.4), the queues and the last declaration number. Every message names the ids it is about; a pair is
applied to those ids only.

4.1 **A new actor.** Held from its first pre-calc (1.1). At the next phase 0:
- a tombstone for its hash: 9.4;
- an entry for its id owned by another game, LIVE: it becomes that game's copy (adopted, 10.1);
- else it is **claimed**: owner this game, epoch = 1 + the epoch of any entry or resting record for the id, declared
  at once to all. It stays held until no greater pair has come for max(0.15 s, twice the measured round trip to
  each peer); alone in the session there is no hold. Then the switch goes off and it thinks.
[run 14: an enemy held from its first frame and released: do its brain and its interrupt graph start. If not, the
hold lets it think for its first 2 frames first.]

4.2 **Two claims** are settled by 4.0; the loser never thought. An owner that has stated an enemy and takes a rival
declaration at its own epoch from a greater incarnation answers by claiming at epoch + 1 (a fight is not replaced by
a fresh enemy because the newcomer drew the greater number).

4.3 **A game owns only what it runs.** A claim, a grant and an offer-claim are made only by a game whose calc runs
the enemy's actor. An entry whose owner has no running actor for it is a contradiction that 4.4 ends.

4.4 **Hand-over when the owner cannot go on**, per enemy:
- the game's own far gate has been closed for the enemy for 0.5 s (the comparison the calc makes:
  [[comps+0xb0]+0x84] > [class+0x264]^2, read at phase 0) [run 20: the two radii], or its calc has not run for 0.5 s
  while the game runs, or the unload delete was requested: OFFER (4.5);
- the owner's heartbeat is LOADING, or silent for 3 s (it left): a game that runs the actor claims at epoch + 1;
- the owner's heartbeat is PAUSED (or silent 1 s): a game that runs the actor claims after 1 s **only if** the enemy
  is in a fight with this game's player (its target, or hit by it within 5 s); otherwise after 5 s. A pack taken
  from a paused owner is **given back** (GRANT) when that owner has run again for 2 s and the enemy is not in 4.7's
  states. A fight does not move to the other player at every meal.
These causes are levels, looked at every phase 0, not edges.

4.5 **OFFER** (reliable; lists ids, each with the offered epoch and its resting state). The owner keeps simulating
for up to 2 s; a game that runs the actor claims at epoch + 1; the offerer becomes a copy when it takes the claim.
With no claim in 2 s: GONE with the resting record (3.3) and the actor is the game's own again (unsynced, far).
GONE taken by a game that runs the actor is a claim. An own actor whose gate opens again on a GONE entry is a claim.

4.6 **TAKE** (step 6): as the contraption specification's 5.2 to 5.5: a request open at most 1.5 s, repeated every
0.3 s; answers GRANT, or REFUSE with NOT_OWNER (and the owner known), STALE (`epoch_seen` below) or BUSY (4.7). A
GRANT uses 1 + the greatest epoch involved, is taken as ownership whatever the request's state, is repeated every
0.5 s until the new owner's STATE at that epoch is seen, and after 1 s without it, or when the grantee leaves, the
granter claims back at epoch + 2. No GRANT to an asker whose heartbeat says LOADING or PAUSED.

4.7 **Not in the middle.** An offer by choice and a grant wait while the enemy is in an interrupt action (main slot
status byte BSA+0x26a8 is 1 or 2), in ragdoll, or dying. A forced hand-over does not wait, and then: the new owner
gives the body the last stated velocity; with `rag` 2 or 3 it starts its own ragdoll from the bones it shows
(main+0x1bddcb0) [run 16]; a DYING enemy is **not claimed**: 9.3.

4.8 **A copy becomes the owner**, in its next pre-calc: the body dynamic again (the request is queued by the game:
not true in the frame asked), velocity as last stated, the switch off with the saved queue count and bit word put
back (5.2; [m] on enemies that had been thinking: the interrupt graph runs again), the brain verb as last stated
with force, target slot 0 from the cell. [run 14 also settles the enemy that never thought; the game's own reset
main+0xa12a48 is the fallback.] Phase 0 reads it from the next frame and sends STATE and a declaration then.

4.9 **An owner becomes a copy**, when it takes the pair: the switch on, the body static, ring and queues empty.

## 5. The copy switch

5.1 **Hooks.** For an actor whose map cell says copy or held, at the entry of:

| Function | Actor | Does | Status |
| --- | --- | --- | --- |
| main+0x50b404 | [x0+0x1f8] | returns | [m] turned away utility and brain every time, idle, in a fight, hit, dying |
| main+0x44ff30 (scheduler) | [x0+0xb70] | returns | [m] likewise |
| main+0x50ad74 (action request) | [x0+0x18] | returns 2 | never reached with the switch on in any run [inf] |
| main+0x2beb80, main+0x42e0c8 | [x0+0x1f8] | returns | never reached with the switch on (the first is only called from a request; the second never for a Bokoblin) [inf] |
| main+0x1920 (node tick) | [[[x0+8]+8]+0x1f8] | counted as a leak, then returns | never reached in any run; what a caller reads back is not known, so a leak is logged with its caller and that enemy is declared UNSYNCED for the session |

[m], runs 5, 10, 11, 12: with these and 5.2, on a spawned and on placed Bokoblins, idle, noticing, in melee, while
hit and while killed, no node was ticked or entered and no action was requested; the enemy stood in its clip and was
updated every frame; released, it went on with its fight.

5.2 **Writes in pre-calc**, every frame for a copy or held actor, with rec = [BSA+0x2618], ctrl = [rec+0x18],
inst = [[ctrl+0x20]]; any of them null: nothing is written that frame. On the first frame with all present the
queue count [inst+0x90] and the bit word [inst+0xd8] are saved in the entry. Then [ctrl+0x19c] = -1,
(i16)[inst+0xde] = -1, [inst+0x90] = 0, (u16)[inst+0xd8] bit 0 clear ([m]: the inline interrupt runner then runs
nothing). **Nothing else of the BSA is held**: BSA+0x74 in particular is left alone ([m] run 5: the brain's
first-tick call comes with the switch on and is turned away; holding the field at 1 would close the only door the
brain has). The action slots' verb ids, which the game's own bookkeeping turns into the controller's physics flags,
are written from the owner's (`slot_verb[]`, in the declaration and in EVENT verb) [inf: unmeasured; run 15 compares
the flag word [comps+0xb0]+0x90 with and without].

5.3 **Stated and applied on change**: the brain verb (main+0xad9844(BSA, verb, 1): it runs the game's leave and
enter lists for the verb, unread [inf]); the form key [open: its setter; rule: not applied, the record names copies
whose form differs].

5.4 **Gates that are not graphs.** Each has a thread-local "ours" flag so the mod's own calls pass.
- A create request (main+0x599670) whose creator is a copy or whose caller is the equipment re-create
  (main+0x1b2bf1c, actor [x0+0x18]) or the tail of the detach main+0xb299bc: refused (the detach itself runs: it is
  how a copy's weapon leaves its hand). The shooter's create likewise [open: its call site; rule: by the creator
  link in the arguments].
- DC+0x2c bit 0 (no drops, no reservation) and bit 7 (no death request but the mod's) are set on a copy from its
  first frame as a copy and cleared at 4.8.
- The defeat counters main+0xb28394 (the limiter holder; actor at its usual +0x18 [inf]): returns for a copy. The
  boss-instance record it also writes is stated by the owner (EVENT) [open until step 7].
- The message a death's first stage sends to linked equipment (main+0x1b6f6a0): refused for a copy.
- A copy's ragdoll is never started: main+0x1bddcb0 for a copy returns unless the mod calls it (4.7).

5.5 **The proof**, always on: per copy and second, entries turned away by hook; leaks; node enters
(main+0x1bb410) that reached a copy's controller.

## 6. Motion

6.1 **STATE** (unreliable, latest per (sender, enemy); section 11). Sent at phase 0 for each owned enemy, to each
peer **whose Link is within 150 m of the enemy**, 30 times a second while it moved (over 1 mm or 0.1 degree) or a
player is within 30 m of it, else once a second; to farther peers once a second. One `EnSnap` whose `us` is **the
world clock of the frame whose end the values describe** (the frame before this phase 0), the same scale a capture
takes inside that frame; life points; `rag`; the death state; the look point; slot 0's clocks always and the other
slots' when a command, index or rate changed or once a second. `teleports`, `landings` and `liftoffs` are counters:
a change against the previous ring state means "happened in between" whatever was lost.

6.2 **Read at the owner**, at phase 0: position and rotation; the main body's linear velocity; the move state; the
edge counters (the hook on main+0x240440 runs it and reads the landed and left-ground bytes after it, counting them
per actor); the ground: the body the controller stands on [open: where a controller keeps it; rule: none is stated
and the pose is in the world frame], as (kind, id) of an entity or an enemy, with the pose in its frame.

6.3 **Shown at a copy.** Phase 0 computes, for T of the enemy's owner (6.4): the pose (a cubic on position through
the two ring states around T using both velocities; a slerp; no interpolation across a change of `teleports`; with
no state after T the newest is held **and the slot clocks are held with it** (7.3), and when states come again T
catches up by running at most 10 % fast, never by a jump); a relative state is turned into the world through the
ground actor's pose **as this game shows it now**. The first pose of a newly bound copy is a teleport. Pre-calc then:
- asks the body static (main+0xae4300(body, 0); body = [[[comps+0x50]+0x20]+0x150]) when its motion type is not
  static and no request is pending (the test of `coop_spawn.c kfride_test`: a pending request [body+0x60]+0xd4 bit
  2, else body+0x68 bit 6);
- ForceSetMatrix (main+0x45ce70) with the pose ([m] run 7: actor and body there the next frame, to the millimetre;
  a keyframed body sinks);
- the controller's forward direction and speed from the state's velocity (main+0x298ce54, main+0x1ba5c8, on the
  controller object) [run 13: whether the actor stays at the asked pose with them written; until it passes they are
  not written].

6.4 **The clock.** Per entry: off = this game's world clock at the phase 0 that took a state - the state's `us`,
kept as the minimum over the last 10 s (so now_owner lags the owner's present by the smallest latency seen). A new
minimum is taken at once; a rise is followed at 1 ms a frame, never jumped. T = (this game's world clock - off) -
delay; delay = clamp(95th percentile of the last 2 s of (arrival - expected by stamp) + one state interval, 50 ms,
150 ms), changing at most 1 ms a frame. The offset is cleared on a change of owner (4.0) and when this game's world
clock stood (a pause): the first state after gives it anew and that pose is a teleport. The record's "delay" is
lat + delay, measured on the PC's shared clock in the bench (14).

6.5 **Animation-driven movement** on a copy: [open: the component's switch; rule: the pose written every frame
overrides it; frames where the actor was not at the pose asked are counted and have a bar of 0].

6.6 **Ragdoll and the flinch.** [m] runs 11, 12: a hit makes an enemy flinch with no graph running and no animation
started: the flinch is physics on the bones (the ragdoll reaction component, [inf]). `rag` = [comps+0x3e8]+0x20
(0 none, 1 recovering, 2 partial, 3 full). While the owner's is not 0, or for 0.5 s after a hit was applied at the
owner, the owner sends BONES (unreliable, latest; 30 a second): `us`, the bone count, each bone's local rotation as
3 x 16 bits, cut into pieces of 180 bones, each piece its own slot. A copy keeps 4 and uses the pair around T; the
root is 6.3's pose. Bones are read at the owner and written at a copy in the hook after main+0x283c750 for that
actor's skeleton [inf: run evidence is for stand-ins; run 16]. In the hitter's own game the copy flinches by itself
if the reaction is fed before the hit is taken out (8.1) [open: what feeds it; run 18 shows it].

## 7. Animation

7.1 **Play events** (PLAY, reliable, control stream, in order). At the owner the hook on main+0x4dc628 runs the
function and, when it answered nonzero and the request's actor is an owned enemy, captures slot, name, rate, blend,
start, the three flag bytes, the mode (x1) and the frame's world clock. main+0x4dc628 calls the two older entries
itself; a play that enters one of them directly is not captured [inf: every play of a Bokoblin's graphs and modules
goes through main+0x4dc628; the bench's command comparison shows any other]. Slot removals (main+0x2934934) and
partial changes are captured as EVENT kinds at step 2 only if the bench shows a difference.

7.2 **Slot state** in STATE (6.1): per slot the command hash, rate, current controller index, and per live
controller its clock and weight; read at phase 0.

7.3 **Applied at a copy**, in pre-calc after the pose, from the queue and the cell:
- events and PLAYs of the enemy in one order by time stamp, a blackboard value before a PLAY of the same stamp, each
  when its time is at or before T: a PLAY by the same call with the owner's arguments, the name from the name
  table, node 0 ([m] run 8: the command changes at once and the clip starts in that frame's update); of several
  PLAYs due for one slot in one frame only the last is played;
- **repair**: a slot whose command here differs from the owner's at T for 0.2 s, with the name known, is played with
  the owner's clock written after (covers a lost capture, a refused request, a late copy, a lost claim);
- phase: the owner's clock at T is the earlier ring state's clock + rate x (T - its time), compared modulo the
  clip's length for a loop [open: where a controller keeps the length; rule: an error over half a second is taken
  modulo the last wrap seen]. e under 0.5 (a 30th of a second is 1 [m]): nothing. Under 3: the slot's rate is
  the owner's x (1 + clamp(e / 30, -0.1, 0.1)) [inf: whether anything rewrites the rate between plays; the bench's
  phase bar shows it]. Over: the clock is written with the no-advance flag (ctx+0xa0 |= 0x400) [m] and counted;
  never while the slot's attack hold bit is set (the arming window; [open: what a jump over an event does]);
- while T has no state after it: the no-advance flag every frame, so place and clip freeze together;
- the blackboard values by the table's setter by name [run 17: the names, types and whether the game rewrites bound
  values every frame]; the time rate actor+0x280 every pre-calc.

7.4 **Other stated values** (EVENT kinds, section 11), applied at their time: look-at requests and the look point
(resolved at phase 0); model controller ice amount, death effect, hide mask; the attention reason bytes; the
dodgeable-attack bit; condition flags; the notice word.

## 8. Hits

8.1 **The point.** Everything is done in a hook that wraps the damage step main+0x300a30 (x0 = L; the hook the old
code has there; it is called on every path, unlike main+0x315dd4 which is conditional and has a caller nobody read).
The hit list is at SH = [comps+0x428]: entries [SH+0x50] (0x1c0 bytes each), the count at [[SH+0x50]+0x18] with the
writer's wrap rule (index >= capacity -> 0 while the count keeps rising): "every entry" is min(count, capacity).

8.2 **A victim that is a copy.** Before the function runs, every entry is taken out: released the game's way
(main+0x1a550d4: its two links, its virtual), the count set to 0; so a copy's life never changes by a hit ([m] run
11: left alone, a switched-off enemy takes the damage). Of the entries taken, those whose attacker is **this game's
own** (this Link, its weapon or arrow, an entity this game owns, an enemy this game owns) are sent to the owner as
HIT, once per swing: the hitter keeps (victim, the entry's attack id +0x38) for 2 s and sends a swing once (one
swing sits in the list for several frames). Entries of any other attacker are another game's to send.
[run 18: one swing on a copy, the list per frame at both ends, and whether the copy still flinches.]

8.3 **HIT** (reliable, control stream): the victim's id, the epoch the hitter believed, a serial per hitter, the
entry's bytes with its game-local fields replaced: the two links (+0x60, +0x78) as (kind, id) of a player, an enemy,
an entity or nothing; the body (+0x58) as the byte offset from the victim's main body; +0x38 and the stealth pair as
(hitter, serial); the guard class +0x0b as 0 and the contact copies +0x18, +0x28 zeroed; the vectors as they are.
A game that takes a HIT for an enemy it does not own sends it on once to the owner it knows. An owner drops a
(hitter, serial) it has seen. Every HIT ends in one record line at the hitter (answered by a HITACK): applied;
refused, not LIVE; sent on; lost.

8.4 **At the owner**, in the wrap of main+0x300a30 for that actor: queued entries are appended with their links
built by the game's link assign (main+0x1b0614) from handles phase 0 resolved (an attacker with no actor here: the
stand-in, or none), the attack id mapped through a table (hitter, serial) -> an id drawn from the game's own id
source at first use; at most one entry of kind 1 a frame (the game counts one; the rest wait a frame); the function
runs; the count is written back to what it was. [inf: an entry appended here is not seen by main+0x315dd4's
strongest-attacker pick; run 18 shows whether a reaction is chosen.]

8.5 **A copy's attack.** A copy arms its attack sensor from its animation events (main+0x42ec98, a copy's own calc)
[inf: not yet seen on a copy; run 18]. For kinds the census lists as arming from a node the owner captures
main+0x1bf20cc for its enemy (the record is plain data) as EVENT arm, and disarm when the armed bit [sensor+0x20]
clears; the copy makes the call and clears the bit. What a copy's attack may hurt is decided in the same wrap, at
the **victim**: an entry whose attacker is a copy is released unless the victim is this Link, its mount, or an
entity this game owns. (The old filter works on the victim's list too and only for stand-ins and copies.)

8.6 **The outcome for a player.** A stand-in takes no damage (the old filter's stand-in rule, kept). When a copy's
attack touched this Link the game sends OUTCOME to the owner: the enemy, the attack's serial (the number of the PLAY
that started the clip and the index of the arming in it, counted alike in both games), class (hit, guarded,
just-guarded, dodged), the guard fields, direction, power. On just-guard this game plays the repelled reaction on
the copy at once and the owner's stream confirms; the owner ignores an outcome for a swing that is not its enemy's
current or last, takes one outcome per swing and player, and applies guarded and just-guarded as a hit entry with
the guard cell's attribute bits plus the guard record (L+0xfc, L+0x104 bit 1) [open: who writes that record
natively]. Dodged: RUSH (start, end) [open: the enemy side of the flurry rush; rule: the copy in the rushing game
holds T for that enemy for the rush].

8.7 **Projectiles** (step 4). PROJ (reliable): id, shooter, name, launch pose, velocity [open: the create's
arguments do not hold it; read the shooter first], time, the player aimed at. Every game makes its own at T; one
whose flight time has passed is not made. The game of the player aimed at turns the launch by the difference
between its Link's place and where the owner's stand-in stood at that time. PROJ_END carries time and place and is
applied at T: hit (by the hit player's game) or gone (the owner's). One that comes to rest is stated with its place
and becomes one shared object (16.5).

## 9. Life, death and awareness

9.1 **Life points** are [[L+0x1830]+8] (maximum [[L+0x1838]+8]). A copy's are written from STATE at phase 0. What
draws the health bar is not read: the bench of step 3 compares the bars in the pictures.

9.2 **Death at the owner.** Phase 0 reads the death state word DC+0x20 of every owned enemy: when it becomes valid
the committed cause, second cause and flag bits are sent as DEATH (reliable, all peers) with the time; the entry is
DYING. When the tick ended and the game deleted the actor (not an unload): DEATH_END, the tombstone, the kill
(9.4); the entry is DEAD for 10 s, then freed.

9.3 **Death at a copy.** At T: the mod makes the request (main+0x1b4b738 with the committed causes and flag, bit 7
cleared for the call) and checks its answer; refused, it is asked again each frame for 1 s, then logged. The death
then waits at its third phase for the graph's signal ([m] run 11: DC+0x20 = 0x8307, the enemy stands until
released). When the owner's state word has passed that phase (it travels in STATE), the mod gives the signal: the
compare-and-swap main+0x227d3dc makes, (DC+0x20 & 0xe) == 6 -> 8. The copy's own tick then ends the death with its
effect and deletes the actor; drops and the kill flag are off (bits 0 and 1). [run 16.] Causes that delete at once
(crushed, fell into the abyss, eaten): DEATH with a flag; the copy is deleted by request at T (a placed one after
9.4's call). A copy bound to an entry that is DYING gets the request at binding. **A DYING enemy is never claimed**:
if its owner leaves or unloads it, each game with a copy clears bits 0 and 1 on it only if it is the game with the
smallest incarnation that has one, and that game's death makes the drops and the flag; a DYING copy with no word for
15 s is ended the same way.

9.4 **Kills.** The owner's death calls main+0x195321c(actor, flag) for a placed enemy; the hook captures it when the
actor is an owned enemy and sends KILL (hash, life number, flag) with DEATH_END. Every game adds the tombstone. A
game with an actor for that hash (a copy, or an own actor in an unsynced state) makes the same call **on that
actor before it goes**: it sets the placement's suppress bit and, with the flag, the save's. A placed copy is never
deleted by the mod's request. [run 19: whether the bit holds through the placement update's passes; if not, the
call is repeated while the actor's placement is loaded.] A game with no actor there holds the tombstone; 4.1 applies
it when the actor appears.

9.5 **Loot.** The owner's drops are made by the game; each dropped actor is handed to the object mirror as this
game's object at its create (16.5). A copy drops nothing.

9.6 **Awareness at a copy** (step 4): the attention bytes; the look-at; target slot 0 written **on change** through
main+0xa70a1c (it resets the slot and takes the kind byte from the target's emitter; a target with no emitter is
not written), from a link phase 0 resolved; the notice word. The marks, the battle music and the Sneakstrike prompt
are compared in the bench's pictures and read where they differ.

## 10. Coming and going

10.1 **A copy of a placed enemy** is always the actor this game's own placement made: adopted when it appears (the
mod's range for placed enemies is the game's own). The mod never creates an actor for a placed id and never deletes
one. An adopted copy whose enemy is stated GONE is given back to the game (4.5). If the placement unloads a copy's
actor, the entry stays, `copy` = NONE.

10.2 **Equipment.** The declaration lists what the enemy holds: per slot the arguments of the equipment create
main+0xaf100c as the owner captured them, strings by content (step 2). A copy is given exactly that: its own
equipment create runs with the stated arguments from the mod; the game's own for a copy is refused (5.4). Events
(step 5): equip and drop (main+0x4f59ec), drop and throw records, removal, pick-up, each EQUIP with the slot, made
on the copy at their time; a dropped or thrown weapon is handed to the object mirror (16.5).

10.3 **Run-time enemies** (step 5). A create whose creator is an owned enemy, or that the spawner makes, is
captured at main+0x599670 (the fields the create needs are listed in the message: name, pose, scale, the init
values as (hash, value) pairs, the creator's id; pointers are not sent), minted and declared. Another game within
120 m of it [run 20] makes it with the mod's create (handle and link in the entry, a failed or unanswered request
released after 5 s and retried 3 times) and holds it as a copy; lets it go (delete by request) when 150 m off or
GONE.

10.4 **The spawner** (step 5): the push main+0xa80f28 returns without queueing while a LIVE entry of the same actor
name owned by another game has its newest state within 40 m of this Link [open: the rule's own radius]. Two players
side by side in the same second can still get a set each; the bench counts it.

10.5 **The moon** (step 5). The first game to enter main+0x21cd650 sends MOON with the new life number; the others
call it at their next phase 0 that runs (a moon owed is kept through a pause or a load) unless their own is in
progress (world +0x60 bit 20). Each game ends its own. On MOON: tombstones and resting records are cleared, DEAD
and DYING entries freed, the session's life number raised.

10.6 **After this game's load ends**: its tombstones are applied to the actors of the loaded world as they appear
(the session wins over the save); its resting records are dropped; it asks every peer for all (NEED) and for the
tombstones. **A join of two running worlds**: the host's world stands (the entity sync's rule): the joiner claims
nothing for 2 s, takes the declarations and tombstones, and holds every actor of a listed kind until then.

## 11. Messages

Through the dispatcher (16.1), types 60 to 89. Reliable messages are per peer and in order on a stream; control and
bulk are two streams with no order between them: **everything of an enemy but its declaration is on control**; a
declaration carries the owner-clock time it was read at, and an event older than the last declaration applied for
that id is dropped. A message larger than 1100 bytes is cut into pieces with (number, piece, pieces), as the entity
sync's statements are. Every struct below is checked against the payload at compile time.

```c
typedef struct { uint64_t key; Enid id; uint8_t epoch8, life, rag, nslot; EnSnap s; int32_t hp; uint32_t dstate; float look[3];
                 struct { uint8_t slot, cur, n, pad; uint32_t cmd; float rate; struct { float clock, weight; } c[CTRL_N]; } sl[]; } EnState;   // 60
typedef struct { uint64_t key; Enid id; int64_t us; uint16_t n, first; int16_t q[][3]; } EnBones;              // 61; key over (id, piece)
typedef struct { Enid id; uint64_t owner, hash; uint32_t epoch, decl_no, verb, slot_verb[5]; int64_t us; uint8_t life, kind_i, nslot, neq;
                 char name[48], form[24]; uint8_t attn[64]; /* slots with their names, blackboard pairs, equipment rows, a run-time enemy's create fields */ } EnDecl;   // 62, bulk, pieces
typedef struct { Enid id; int64_t us; uint8_t slot, flags[3], mode; float rate, blend, start; uint32_t cmd; char name[32]; } EnPlay;          // 63
typedef struct { Enid id; int64_t us; uint8_t kind, len; uint8_t d[96]; } EnEvent;                             // 64
typedef struct { Enid id; uint32_t epoch, serial; uint8_t link_kind[2]; Enid link_id[2]; uint32_t body_off; uint8_t entry[0x1c0]; } EnHit;   // 65
typedef struct { uint32_t serial; uint8_t result; } EnHitAck;                                                  // 66
typedef struct { Enid id; uint32_t play_no; uint8_t arm_i, cls; float dir[3], power; uint8_t guard[0x20]; } EnOutcome;   // 67
typedef struct { Enid id; int64_t us; uint32_t cause, second; uint8_t flag, at_once; } EnDeath;                // 68; 69: its end
typedef struct { uint64_t hash; uint32_t life_no; uint8_t flag; } EnKill;                                      // 70; 71: tombstones, in pieces
typedef struct { uint32_t req; Enid id; uint32_t epoch_seen; uint8_t reason; } EnTake;                         // 72
typedef struct { uint64_t asker; uint32_t req; uint64_t owner; uint8_t n; struct { Enid id; uint32_t epoch; } m[]; } EnGrant;   // 73
typedef struct { uint32_t req; uint8_t reason; uint64_t owner; uint32_t epoch; } EnRefuse;                     // 74
typedef struct { uint8_t all, n; struct { Enid id; uint32_t epoch; int32_t hp; uint32_t verb; float pose[7]; } m[]; } EnOffer;   // 75; 76 GONE: the same rows
typedef struct { Enid id; uint32_t number; } EnNeed;                                                           // 77; id (0,0): everything
typedef struct { uint64_t key; uint32_t count, hash, number; } EnIndex;                                        // 78; 79: its end (bulk), with count and hash
typedef struct { Enid id, by; int64_t us; char name[48]; float pose[7], vel[3]; uint64_t aimed_inc; uint16_t len; uint8_t args[]; } EnProj;   // 80; 81: its end
typedef struct { Enid id; int64_t us; uint8_t kind, slot; uint16_t len; uint8_t args[]; } EnEquip;             // 82
typedef struct { uint32_t life_no; } EnMoon;                                                                   // 83
typedef struct { uint64_t hash; uint8_t why; } EnUnsynced;                                                     // 84
```

EVENT kinds (`d` layouts): 1 verb {u32 verb, u32 slot_verb[5]}; 2 form {char[24]}; 3 target {u64 incarnation, or
Enid}; 4 blackboard {u8 n; {u32 key hash; u8 type; f32 or u32 value}[]}; 5 time rate {f32}; 6 attention {u8[64]};
7 look request {u8 which; u8 args[40]}; 8 model {f32 ice, f32 death, u8 hide}; 9 arm {the record of
main+0x1bf20cc}; 10 disarm {u8 kind}; 11 dodgeable {u8}; 12 conditions {u32 flags[4], i32 chem[2]}; 13 notice
{u16}; 14 jump {f32 vel[3]}; 15 knock-back cause {u32 hit serial}; 16 rush {u8 start}; 17 boss record {u64 hash}.
Applied at their time (T) except 6, 11 and 13, applied at once.

11.1 **Declaration**: owed to every peer when an enemy is claimed, when its name, life or owner changes, on NEED, on
a join, and to a peer that ran again after PAUSED or LOADING (it gets no PLAY or EVENT while its heartbeat says so).
A receiver passes the pair through 4.0; `decl_no` is compared only against the last declaration taken **with the
same pair**.
11.2 **STATE** is dropped unless its sender is the entry's owner and `epoch8` its epoch's low byte; a STATE for an
unknown id for 0.5 s sends NEED(id). An entry with no state for 3 s from an owner whose heartbeat runs asks NEED;
4.4's levels cover an owner that does not.
11.3 **INDEX** (unreliable, per peer, every 2 s, not while a declaration is owed to that peer): count and the sum of
per-entry hashes over (id, epoch) of the sender's LIVE enemies within 150 m of that peer's Link, and the number of
declarations sent to it. Compared only by a receiver that has taken that many; a mismatch on two in a row: NEED(0,
0) with a number, at most once in 10 s; the answer ends with INDEX_END (the number, count, hash) on the bulk stream
after the last declaration; entries of that sender within range not named by then are GONE here.
11.4 An id a receiver does not know in PLAY, EVENT, DEATH, EQUIP, PROJ, OUTCOME or a GRANT's list is kept 2 s (64
messages) for its declaration.

## 12. The target (step 4)

Not specified beyond D until two things are measured: that an enemy's Sight list holds a stand-in built to the five
requirements of the reading (affiliation type Player at registration, non-resident, a sense level above 0, the
player perception sublayer, kept in the sensor world), and that an enemy whose target slot 0 names a stand-in fights
it (in the only runs on record a noticed stand-in was not fought) [run 21]. The owner's rule (D 9.4, section 12 of
the first draft) is written into this section after that run, with the hook point read from the caller of
main+0x1866bf4. Until then an enemy fights its owner's player only, and step 4's bench has no case for it.

## 13. Settings

`TOTK_COOP_ENEMY=1` (default off until step 3 passes; then the old code's setting `TOTK_COOP_ENEMY_OLD=1` is the
opt-in). Tests stay in the code: `TOTK_COOP_TEST_ENPROBE` (D's runs), `TOTK_COOP_EN_DELAY_MS`,
`TOTK_COOP_EN_NOCLOCK=1`, `TOTK_COOP_EN_NOSWITCH=1`, `TOTK_COOP_NET_TEST=<latency>,<jitter>,<loss %>,<reorder %>`.

## 14. The record

Every line and every motion sample carries the PC's performance-counter time (one clock for every game on a PC), and
the report pairs the two games by it: an error of the clock estimate then shows as a position error.
- `coop: en: change:` owner, epoch, life, copy state, verb, target; `coop: en: own:` every claim, offer, take,
  grant, refusal, give-back with its cause; `coop: en: unsynced:` with the reason;
- `coop: en: leak:` and, once a second, `coop: en: switch:` (5.5);
- `coop: en: motion: <id>:` once a second per copy near this Link: delay, lat + delay, states used and lost, frames
  held (no state after T), position error, worst change of velocity, snaps, frames not at the asked pose;
- `coop: en: anim: <id>:` per slot the command here and at the owner, clock error, soft and hard corrections,
  repairs; a line at every PLAY applied with its lateness;
- `coop: en: hit:` every capture, HIT and its result (8.3), the life before and after at the owner; `death`,
  `kill`, `equip`, `proj` at both ends;
- in `motion.jsonl` (through `coop_ent_log.c`'s writer), ten times a second: every enemy near this Link (id, owner,
  pose, velocity, slot 0's command and clock, life, and the world places of six bones: feet, hands, head, hips).
`tools/coop/en_report.py` prints per enemy and for the session: time-aligned position error (mean, 95th percentile,
worst), delay, stutter, snaps, foot slide, phase error, command lateness, hits without a result, and every
disagreement of life, verb, target, command or held item lasting over 0.5 s.

## 15. The bench and the pass bars

`tools/coop/play_two.ps1 -Bench enemy,<case>` (a scene `enemy` in the bench code, 16.6): both games load the camp
save (a Bokoblin camp with a lookout in Hyrule Field, made 2026-11-15; kept with the bench's files outside the
repository); the bench code aims and moves game 1's Link (an input script cannot swing at a moving enemy). Every
case runs twice: with the test network at 1 ms, and at 80 ms with 20 ms jitter and 2 % loss. Cases: `idle`,
`arrive` (both walk in together), `late` (game 2 arrives mid-fight), `walk`, `sprint` (run, stop dead, turn),
`fight`, `fight2`, `knock` (a knock-down and get-up), `jump`, `ledge`, `leave` (also mid-attack), `menu` (the owner
pauses 0.5 s and 5 s), `quit` (the owner's process is killed), `load`. A case passes when the report's bars hold
**and** pictures from both games every 2 s show the same enemies in the same places and poses.

| Bar | Step | Value |
| --- | --- | --- |
| ownership changes after the first claim in `idle`, `walk`, `fight`; both tables equal | 1 | 0; equal |
| leaks | 2 | 0 |
| position error: standing / walking / running / jump peak / knock-back / fall | 2, 3 | 2 / 4 / 8 / 15 / 15 / 20 cm at the 95th percentile |
| delay | 2 | delay within 50 to 150 ms, steady within 10 ms over a minute |
| stutter (frame-to-frame change of velocity) | 2 | no frame above 3 times the owner's own |
| snaps not flagged; frames not at the asked pose | 2 | 0; 0 |
| frames held | 2 | 0 at 1 ms; under 1 % at the lossy setting |
| foot slide of a planted foot | 2 | within 2 cm a frame of the owner's |
| phase error; hard corrections; repairs | 2 | 33 ms at the 95th percentile; under 1 a minute; 0 at 1 ms |
| command lateness beyond the delay | 2 | under 17 ms at the 95th percentile |
| hits with no result; hits lost | 3 | 0; 0 |
| disagreements over 0.5 s | 3, 4 | 0 |
| one death, one set of drops, one kill flag, one tombstone per enemy | 3 | exact |
| flinch onset after the blow in the hitter's game | 3 | measured first; the bar is written here before the second run |

A number above changes only by decision, written here with the run's date.

## 16. Groundwork (step 0)

16.1 **The dispatcher** (`coop_msg.c`). Phase 0 and the pause queue take each reliable message once and hand it by
type (40 to 55: the entity sync, 60 to 89: the enemy sync); today `coop_ent_phase0` and `own_net` take everything
and drop what they do not know. Join and leave events are given to both. STATE's rate is 30 a second because the
transport receives on the 30 Hz tick; slots are read only under 2 s old.
16.2 **Session state** (`coop_sess.c`): PAUSED, LOADING, the heartbeat flags (one `coop_ctr_hb_set` a tick for
both syncs), a peer's silence (1 s paused, 3 s left), the round trip per peer. Today these live in the entity sync
behind its setting.
16.3 **Out of `coop_world.c`**, bodies unchanged and linkage changed, in this order: (1) a header
`coop_world_int.h` with the shared types and the registry's symbols (about twenty statics become external); (2) the
actor registry and key walkers (`coop_actors.c`), whose `seen[]` gains the kind index; (3) the ground sync; (4) the
stand-in's pose, animation and trail with the animation helpers they call; (5) contacts, link hits, the hit-update
dispatcher and the HIT channel's demultiplexer as one file, since they share one state.
16.4 **The old enemy sync off** (`TOTK_COOP_ENEMY_OLD` unset), gate by gate: `peer_one` skips enemies but not
mounts; `coop_enemy_puppet_calc`, `_phase`, `_skel`, `_writer`, `coop_enemy_action` return at once; the old spawn
sync does not announce `Enemy_` actors; `link_hits_send` and its takers are off; `coop_audit_enemies` writes
nothing; the stand-in rule of the contacts filter stays; `coop_enemy_within` and `coop_near_my_enemy`, which keep
the object mirror from sending an enemy's weapon or arrow as a loose object, are answered from the new table.
Addresses both generations hook (main+0x41bb20, 0x50ad74, 0x599670, 0xaf100c, 0x1b4b738, 0x300a30, 0x283c750): the
enemy sync's handler is called first and says whether it ran the function.
16.5 **The object mirror** gets an entry point on the co-op tick: `coop_obj_made_by_enemy(actor)`, queued from
phase 0, free of its 6 m and 20 m rules, for drops, dropped and thrown weapons and resting projectiles; an actor
with a PROJ id or in an enemy's hand is not the mirror's.
16.6 **The bench**: the scene `enemy` (both Links placed, the camp found, an aim helper), the script passing the
settings, `en_report.py` called at the end, the transport's test network (delay, jitter, loss and reorder applied
to received packets).
16.7 **Shared game data** (only with `TOTK_COOP_SHARE_WORLD=1`, which is off for play today): the kill's save flag
is written by the placement's own call (9.4), not through it. The 165 enemy flags of reset type 0x50 are added to
its shared set by a list of keys from the census when that setting is used.

## 17. Runs owed

All on the camp save unless said; each needs the tester's yes.
13. direction and speed written on a static body (run 7 again, a spawned enemy is enough);
14. an enemy held from its first frame, released: brain and interrupt graph; and a copy that never thought made
    owner;
15. the fight of run 10 with 5.2's slot verb ids written: the physics flag word against the control;
16. death on a copy by the mod's request and signal; ragdoll and get-up from bones; a forced hand-over in ragdoll;
17. the blackboard table logged on a Bokoblin, a Lizalfos, a Lynel and a Hinox;
18. one swing on a copy: the hit list per frame at both ends, the flinch, a reaction chosen at the owner, the
    copy's own attack arming;
19. a placed enemy put away by the dead call without the save flag: the placement's bits over two passes;
20. the game's far gate and unload radii for a Bokoblin; the range at which a placement makes its actor;
21. two players before the camp: a stand-in in the Sight list, slot 0 written to it, whom the enemy fights.

## 18. Not taken from the reviews, and why

- Review RC proposed re-aiming a projectile in the aimed player's game by the stand-in's offset: taken (8.7), but a
  shot's hit is still that player's game's alone.
- "Hits held for a paused owner are appended after it resumes": not taken as a rule of its own; a hitter that gets
  no HITACK in 2 s logs the hit as lost and the bar counts it, since a paused owner in a fight is claimed from
  after 1 s (4.4) and the hit is then sent on.
- Whole-game slow motion in the heartbeat: not in steps 0 to 5; the record notes when a game's world clock runs
  slow against its wall clock, and the bench decides.
- A pack as a unit of ownership: dropped (section 4).
