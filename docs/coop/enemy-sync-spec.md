# Enemy sync: the specification

Status, 2026-11-15: third draft, nothing of it built. It turns [enemy-sync-design.md](enemy-sync-design.md) (the
design, D below) into data, messages and procedures for new files `runtime/coop_enemy*.c`. Where this file and D
disagree, D is corrected first (D section 16 lists what the review changed).

History: the first draft was reviewed by three readers that did not write it: against the co-op code it builds on,
against the game facts, and by walking twelve play scenarios by hand (kept outside the repository; cited as RA, RB,
RC). They found the ownership rule unsound, the identity rule contradictory, the hit and death procedures wrong in
several places, the transport and the old code misdescribed, and a third of what the draft called measured not
measured. Test runs 9 to 12 (D section 4, 6 and the notes) were made after the first draft: a placed camp, a fight,
and a play test with the switch held by the tester. The second draft was reviewed the same way (R2A, R2B, R2C). It
had closed most of the first round; the second round found ownership still unsound in four interleavings, the hold of
a new actor unbuildable as written, the clock on a time base that loses time, the flinch misread, the death at a copy
wrong in several details, and no rule for an enemy that exists in one game only. This draft makes the ownership
section smaller, puts the measurements that decide structure before the steps that depend on them, and specifies in
full only steps 0 and 1; sections 5 to 10 say what is known and what their runs must show before they are built.

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
6. The families of D section 12, one at a time, each with its own section added here when its reading is done.

Steps 1 to 5 are for enemy kinds of the **Bokoblin shape**: one actor with a character controller and the graph
units of D section 4. The list of kinds in the shape is data (`coop_enemy_kinds.inc`, from the census). An enemy of
another kind, and an enemy at a placement where the two games' saves give different actor names (3.4), is
**unsynced**: each game keeps its own, and the record says so once.

The enemy sync does not need the entity sync (on by default since 2026-11-15, off with `TOTK_COOP_CONTRAPTION=0`):
the groundwork of section 16 serves both; entity ids are used for grounds and hit links only while it is on.
**Before step 2**: runs 13, 14, 19, 20 and 22 (section 17), since they decide what a copy and a held actor are and
what a game does with a placed enemy it has no actor for. **Before step 3**: runs 16 and 18.

## 1. Threads and moments

- **Phase 0** (`coop_enemy_phase0`, called from `coop_spawn.c coop_hook` at main+0xa72b00 after session state and
  the entity sync's call, with the register save its neighbours use): once a frame, outside the parallel actor
  update, where a link to any actor resolves. It first pumps the transport (16.1). Everything of the table; all
  messages; the read of every owned enemy (6.2, 7.2); everything that names another actor is resolved here.
- **Pre-calc** (`coop_enemy_precalc`, at the entry of the actor's calc main+0x41bb20, on that actor's update
  thread; `coop_actor_seen` is called before it): for a copy or a held actor. Guest calls on its own actor only.
- **The damage step** (main+0x300a30, in the actor's phase A, slot 51, after its calc): section 8.
- **Hooks** at entries, any game thread: they read the map, turn a runner away, or post a capture. A hook that needs
  a function's result runs the function itself through `rt_dispatch` with a thread-local guard and returns nonzero;
  the inner entry falls through to the older handlers of that address.
- **The net tick** (`coop_tick`; its rate is measured in step 0, run 22): session state, the dispatcher's pause
  queue, and, under `en_lock` taken with `coop_lock_wait`: OFFER_ALL while loading, repeats, the list of enemy
  places the object mirror asks for.

`en_lock` guards the table. Phase 0 holds it for its pass; the net tick takes it for the three things above.

**The frame stamp.** Phase 0 reads the performance counter once and publishes it; every state read and every capture
made until the next phase 0 carries that value. All times in messages are on this clock (one per PC, monotonic, not
stopped by a menu, not shortened by a hitch as the game's world clock is).

**Captures** (hooks -> phase 0): one ring for all hooks, many producers, one consumer, a mutex held for a copy as
`coop_ent.c ent_post` does: 1024 entries of a tagged union whose largest member is a hit entry; a capture names the
actor, its generation and the frame stamp; phase 0 finds the entry. A full ring loses the capture and counts it by
kind; what each loss costs and how it heals is said where the capture is defined.

**The cell** (phase 0 -> the actor's threads), one per entry, a sequence-numbered whole value: the pose and clocks to
show, links as ready handles, and **this frame's due items**: at most 8 plays and events, 4 hits to append, 1 death
request, each with a "taken" mark the consumer sets (atomic). Phase 0 keeps the backlog in the entry (64 items; when
full the oldest PLAY of a slot gives way to the newest and the enemy is re-declared to this game by NEED) and
publishes again what was not taken. There is no queue toward the actor.

### 1.1 The map
One buffer of 2048 atomic 64-bit words, open-addressed; every writer uses compare-and-swap; a cleared cell is a
tombstone until phase 0 rebuilds. A word: (actor - heap base) >> 3 in 30 bits, the generation's low 12 bits, a table
index in 9 bits, a state in 3: **owned, copy, held, own** (an actor of a listed kind that this game runs for itself:
unsynced, far, or not yet examined). Phase 0 writes entries' cells; **pre-calc inserts a held cell** for an actor of
a listed kind that has none, so the hooks of 5.1 hold it from its first frame; the delete request's hooks clear an
actor's cell. `seen[]` (16.3) holds (actor, generation, kind index) per cell and recomputes the kind when the
generation differs. A held cell that phase 0 has not resolved within 0.5 s becomes **own** and the record says why
(table full, no session): nothing is held without end.

### 1.2 The order inside phase 0
1. session state (16.2); `coop_ctr_pump()`; messages from the dispatcher in arrival order; latest-only STATE and
   BONES under 2 s old, each snap pushed into its ring when its stamp is above the ring's newest;
2. peers that came, left, paused, loaded;
3. actors: liveness and generation of every bound actor; held and new actors from the map and `seen[]`;
4. ownership (4): settled before step 5;
5. the capture ring: of owned enemies into messages owed, of copies and held actors into requests to their owners;
6. the read of every owned enemy; declarations, events and STATE owed;
7. copies adopted and let go (10.1);
8. T per owner (6.4), fixed for the frame; the cells; the map.

### 1.3 Paused and loading
PAUSED: phase 0 has not run for 0.3 s. LOADING: the game's teardown flag or no resident Player. While LOADING here:
nothing is adopted or claimed; the switch is off for every copy and held actor (a load hung in the old code with
copies held); every owned enemy is offered once (OFFER_ALL, from the net tick); the table is kept. With no session
nothing is held and no table is kept.

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

Ownership is **per enemy**. What an enemy holds, and its children, limbs and projectiles, follow it. A camp can be
split between owners by which game runs each member; what the camp's node relays to its members is then sent between
the owners (4.11).

4.0 **One rule.** `en_apply_owner(e, owner, epoch)` is the only code that changes owner or epoch from a message; it
takes the pair only if greater than the entry's by (epoch, then incarnation). On a change it clears the ring and the
backlog toward the actor; captures already made are kept and routed by the new pair. Every message names its ids and
the pair its sender believed (`epoch8`); a PLAY, EVENT, DEATH, OFFER row or GONE row is dropped unless its sender is
the entry's owner at that epoch.

4.1 **"Runs it."** A game runs an enemy when it has the live actor, the actor's calc ran this frame, and the game's
own far gate for it has been open for 0.5 s. The gate is the calc's own test: with A = [[[BSA+0x25f0]+0x18]+0x150]
and X = [A+0x38]: open when BSA+0x26ad bit 5 is set, or [A+0x41] bit 0 or [X+0x2d2] bit 0 is clear, or
f32 [X+0x264] is negative, or [[comps+0xb0]+0x84] <= [X+0x264]^2; read at phase 0 [run 20: the values]. Only a game
that runs an enemy claims it, in any of the ways below.

4.2 **A new actor** has a held cell from its first pre-calc (1.1): the switch on and its controller's speed taken
away ([m] run 11: a switched-off enemy with a dynamic body walks on with its last speed; [run 13]: the write). At
the next phase 0, in this order:
- a tombstone for its hash and life number, or an entry for it that is DYING or DEAD: 9.4;
- an entry owned by another game, LIVE: it becomes that game's copy (10.1);
- an entry UNSYNCED, or the kind is not listed: **own**;
- this game does not run it (its gate is closed): **own**, with no entry; looked at again when its gate opens;
- else **claimed**: owner this game, epoch = 1 + the epoch of any entry or resting record for the id, declared at
  once to all. It stays held until no greater pair has come for max(0.15 s, twice the measured round trip to each
  peer whose heartbeat runs), **at most 0.4 s**; alone in the session, and for an id this game minted, there is no
  hold. While held it is a copy for 8.2: hits on it are captured and kept for whoever owns it after the hold.
[run 14: an actor held from its first frame and released: do its brain and its interrupt graph start; if not it is
let think for its first two frames before the hold.]

4.3 **Two claims** are settled by 4.0; the loser never thought. An owner whose enemy has left its hold answers a
first rival claim at its own epoch by claiming at epoch + 1 (tested before 4.0 applies the rival's pair); a game
still in its hold never answers; a game that joined later never answers a host's declaration.

4.4 **Hand-over**, per enemy, each a level looked at every phase 0:
- the owner no longer runs it: OFFER (4.5). When the cause is its gate, the offer waits for 4.6's states at most
  1 s; when its calc stopped or the unload was requested it does not wait;
- the owner's heartbeat says LOADING, or is silent for 3 s (it left): a game that runs the enemy claims at epoch + 1;
- the owner's heartbeat says PAUSED, or is silent for 1 s: a game that runs the enemy claims after 1 s **if it is in
  that fight** (its player hit the enemy within 5 s, or was hurt or made to guard by its copy within 5 s), else
  after 5 s. The enemy then stays with the claimer; there is no giving back.
A game that claimed from an OFFER does not offer the same enemy for 2 s.

4.5 **OFFER** (reliable; rows of (id, epoch, resting state, a dying mark)). A row is taken only when (sender, row
epoch) equals the entry's pair. The owner goes on simulating for up to 2 s; a game that runs the enemy claims at
epoch + 1; the offerer becomes a copy when it takes the claim. With no claim in 2 s: GONE with the resting record;
the offerer's actor is **own**. A GONE row is taken under the same test and never causes a claim by itself: a game
that runs the actor of an entry it has taken as GONE claims it as a new actor by 4.2. An index answer never ends an
entry: an entry not named asks NEED(id) once.

4.6 **Not in the middle.** An offer by the gate waits (4.4) while the enemy is in an interrupt action (main slot
status byte BSA+0x26a8 is 1 or 2) or in ragdoll. In a forced hand-over the new owner gives the body the last stated
velocity; with `rag` 2 or 3 it starts its own ragdoll from the bones it shows; an enemy lying in a reaction clip
gets its forced verb when that clip ends [run 16: all three; in no log yet].

4.7 **A death is its owner's.** A DYING enemy changes owner only when its owner left, loads, or unloaded it (the
OFFER row's dying mark; never when the owner is merely paused: the copy waits). Then a game with a copy claims at
epoch + 1; 4.0 settles two claimers; the claimer ends the death (9.3) after its hold passed with no greater pair.

4.8 **A copy becomes the owner**, in its next pre-calc: the body dynamic again, velocity as last stated, the switch
off with the saved queue count and bit word put back ([m] on enemies that had been thinking), the brain verb as last
stated with force, target slot 0 from the cell [run 14 for an actor that never thought; the game's own reset
main+0xa12a48 is the fallback]. Phase 0 reads it from the next frame and sends STATE and a declaration then.

4.9 **An owner becomes a copy**, when it takes the pair: the switch on, speed taken away, the body static, the ring
empty.

4.10 **A peer that left**: its entries with no actor here become resting records (placed) or are freed (run-time).

4.11 **A camp across two owners** [open: the camp node's relay, messages 0x3ddc0006 to 0x3ddc000a; rule until read:
its members of different owners do not hear each other, and the bench case `arrive` shows what that looks like].

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

6.4 **The clock** (`coop_sess.c`, one estimator per peer, for both syncs). Stamps are frame stamps (section 1). A
sample is (this game's frame stamp when a state was taken) - (the state's stamp); the estimate follows the smallest
sample of the last 10 s, **moving at most 2 ms a frame in either direction**; when it is off by more than 250 ms it
is set at once and every copy of that owner takes its next pose as a teleport. It is cleared, with the same
teleport, when this game was PAUSED, when the owner's heartbeat goes from PAUSED or LOADING to running, and on a
change of owner. T = this game's frame stamp - estimate - delay; delay = clamp(95th percentile of the last 2 s of
(sample - estimate) + two state intervals, 50 ms, 150 ms), changing at most 1 ms a frame; one interval when the
bench shows it is enough. A gap in a ring of over 0.5 s is a teleport. The record prints lat + delay as measured on
the PC's clock in the bench, and T's distance behind the newest state.

6.5 **Animation-driven movement** on a copy: [open: the component's switch; rule: the pose written every frame
overrides it; frames where the actor was not at the pose asked are counted and have a bar of 0].

6.6 **Ragdoll and the flinch.** Read in the game's code (R2B): when the damage step computes damage of 1 or more,
the life's damage handler sets a request bit (rag+0x149 bit 0; rag = [comps+0x3e8]); the ragdoll component's update
(main+0x432884) then starts a partial ragdoll with the hit's direction (main+0x1bdd810 -> main+0x1bddcb0, request
byte +0x33 = 1, state rag+0x20 = 2) and ends it itself. [m] runs 11, 12: a hit enemy flinches with every graph off.
So: at the owner a hook that runs main+0x1bddcb0 and sees its request (names by content, strength, direction, the
id pair, four bytes) sends EVENT ragdoll-start; a copy makes the same call from the mod at T and its own physics
plays the flinch. In the hitter's own game the mod makes the call on the copy at the blow, with the hit's own
direction, and ignores the stated start for that swing: the flinch is immediate. BONES (unreliable, latest; `us`,
bone count, local rotations as 3 x 16 bits, pieces of 176 bones, a pose needs all pieces of one stamp) are sent
only while the owner's state is 3 (full ragdoll) or the enemy is dying, to peers within 150 m; a copy keeps 4 and
uses the pair around T. [run 18: rag+0x20, rag+0x149 and every start request per frame at the owner; run 16: bones.]

## 7. Animation

7.1 **Play events** (PLAY, reliable, control stream). Every animation start ends in main+0x4dca24 (main+0x4dc628,
which graph nodes and modules call, is one of its 23 callers; the others are unread). The owner's hook there runs
the function and captures when the slot's command changed across the call: slot, name, rate, blend, transition, the
flags, and a **play number** per (enemy, slot) that also travels in STATE's slot rows and the declaration. A PLAY
whose number is not above the slot's current one is counted and not played. [inf until run 18 logs main+0x4dca24 by
caller on a hit, switched-off enemy.]

7.2 **Slot state** in STATE (6.1): per slot the command hash, rate, current controller index, and per live
controller its clock and weight; read at phase 0.

7.3 **Applied at a copy**, in pre-calc after the pose, from the queue and the cell:
- events and PLAYs of the enemy in one order by time stamp, a blackboard value before a PLAY of the same stamp, each
  when its time is at or before T: a PLAY by the same call with the owner's arguments, the name from the name
  table, node 0 ([m] run 8: the command changes at once and the clip starts in that frame's update); of several
  PLAYs due for one slot in one frame only the last is played;
- **repair**: a slot whose command here differs for 0.2 s from the owner's in a slot row whose play number is at or
  above the last PLAY applied to it, with the name known, is played with the owner's clock written after; a changed
  slot is sent in three consecutive states;
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

8.2 **A victim that is a copy or held.** Before the function runs, when the count is not 0, every entry is taken out:
released the game's way (main+0x1a550d4), the count set to 0 ([m] run 11: left alone, a switched-off enemy takes
the damage). The step also applies damage that is not in the list: impacts ([L+0x1900]+0x120) and condition damage
(bits of L+0x1780) [open: read both; rule: a copy's life is written from STATE every frame, so its own such damage
does not last; a local impact or fire on a copy does not reach the owner, and the record counts them]. Of the
entries taken, those whose attacker is **this game's own** are sent to the owner as HIT, once per swing (the hitter
keeps (victim, attack id +0x38) for 2 s [open: the game's own repeat ring; a spin or a flame may lose hits; run 18]).
"This game's own": either link's actor is this Link; or is in the entity sync's owned set; or is an owned enemy by
the map; or is in a set phase 0 publishes of what belongs to this game's player and enemies (held equipment, this
game's arrows and thrown things as the mirror's tick last reported them). An entry nobody claims is released and
named in the record. At the blow the mod also starts the copy's flinch (6.6).

8.3 **HIT** (reliable, control stream): the victim's id, the epoch the hitter believed, a serial per hitter, the
entry's bytes with its game-local fields replaced: the two links (+0x60, +0x78) as (kind, id) of a player, an enemy,
an entity or nothing; the body (+0x58) as the byte offset from the victim's main body; +0x38 and the stealth pair as
(hitter, serial); the guard class +0x0b as 0 and the contact copies +0x18, +0x28 zeroed; the vectors as they are.
The message names the hitter (incarnation) and a swing number. A game that does not own the enemy, or has offered
it, answers "not mine" at once (the net tick can); the hitter keeps every HIT until it is answered "applied" or
"refused, not LIVE", sends it to a newly declared owner, appends it itself if it becomes the owner, and gives up
after 1 s (a hit is never applied later than that). An owner drops a (hitter, swing) it has seen. Every HIT ends in
one record line at the hitter.

8.4 **At the owner**, in the wrap of main+0x300a30 for that actor: queued entries are appended with their links
built by the game's link assign (main+0x1b0614) from handles phase 0 resolved (an attacker with no actor here: the
stand-in, or none), the attack id mapped through a table (hitter, serial) -> an id drawn from the game's own id
source at first use; at most one entry of kind 1 a frame (the game counts one; the rest wait a frame); the function
runs (the older handler of this address first: the stand-in rule and the mounts' contacts); the appended entries
are released (main+0x1a550d4) and the count written back. The list is [[L+0x1910]+0x50]: capacity +8, entries
+0x10, count +0x18; the actor [L+0x198]. Damage and the reaction are computed from the list inside the step; the
attacker record L+0x90 was filled before it, so the enemy does not learn who hit it from an appended entry
[run 18: L+0x90 and the reaction chosen; if needed the mod writes the record].

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

9.2 **Death at the owner.** Phase 0 reads the death state word DC+0x20 of every owned enemy (bit 0 valid, bits 1 to
3 the phase, 4 to 8 the cause, 9 to 13 the second cause, 14 and 15 flags). When it becomes valid: DEATH (reliable,
all peers) with the committed causes and flags; the entry is DYING. The game records the kill at the death's start
for an ordinary placement ([m] run 11: DC+0x2c bit 1 set while the enemy still stood): the hook that runs
main+0x195321c captures (hash, flag) for an owned enemy and KILL goes out then, with the tombstone. A row that is
not a kill (it takes main+0x1953344 instead) makes no KILL and no tombstone. When the phase passes 3: DEATH_SIGNAL
(reliable). When the tick ended: DEATH_END; the entry is DEAD for 10 s. A row that ends in sleep and not in a
delete: DEATH_END when the phase reaches 6.

9.3 **Death at a copy** [run 16 before it is built]. A copy has DC+0x2c bits 0 (no drops) and 7 (no request but the
mod's) set from its first frame as a copy; bit 1 is left clear, so the copy's own death makes its own kill record,
complete, at its start, as the owner's did. At T of DEATH: the mod makes the request (main+0x1b4b738 with the
committed causes and flag, bit 7 cleared for the call) and checks its answer; refused, it is asked again each frame
for 1 s, then logged. The death waits at its third phase ([m] run 11: 0x8307). At DEATH_SIGNAL (or DEATH_END, if
that comes first) the mod gives the signal: (DC+0x20 & 0xe) == 6 -> 8. The copy's own tick ends the death with its
effect and deletes the actor. A cause that deletes at once: the copy's actor is not deleted by the mod; it is put
away by 9.4 [run 19]. A copy bound to an entry that is DYING gets the request at binding. A DYING enemy whose owner
is gone: 4.7; the claimer clears bit 0, so its death makes the drops.

9.4 **Kills.** Every game adds the tombstone (hash, life number, flag) at KILL; one below the session's life number
is dropped. A game with an actor for that hash that is not dying by 9.3 (an **own** actor; a fresh actor at a
tombstoned hash) makes the dead call on it, main+0x195321c(actor, flag): it sets the placement's suppress bit, tells
the placement's listeners, adds the hash to the game's dead set and, with the flag, writes the save
[run 19: that the placement then does not make the actor again, and how the actor already there is removed; until
it passes, a tombstoned enemy that stands here is **own** and the record counts it].

9.5 **Loot.** The owner's drops are made by the game; each dropped actor is handed to the object mirror as this
game's object at its create (16.5). A copy drops nothing.

9.6 **Awareness at a copy** (step 4): the attention bytes; the look-at; target slot 0 written **on change** through
main+0xa70a1c (it resets the slot and takes the kind byte from the target's emitter; a target with no emitter is
not written), from a link phase 0 resolved; the notice word. The marks, the battle music and the Sneakstrike prompt
are compared in the bench's pictures and read where they differ.

## 10. Coming and going

10.1 **A copy of a placed enemy** is the actor this game's own placement made, adopted when it appears; the mod does
not delete it. **An enemy this game has no actor for** (dead in this save, another name at the placement, the
placement not made here because of where this player is) is the commonest difference between two saves and decides
whether the sync is worth anything; the record prints once a minute how many enemies near the players are unsynced
or without an actor here. The rule is written after runs 19 and 20: either a placement can be made to agree with
the owner's (the clear-dead call, the name), or such an entry gets a created actor that is deleted in the phase 0
in which the placement's own appears. Until then: no copy, no projectile for a shooter with no actor here.

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

11.0 **Fields the layouts above gain in this draft** (the structs are rewritten when step 1 builds them, each with
its compile-time bound, sent as `offsetof` + rows after a `memset`): every reliable enemy message carries `epoch8`
(4.0); EnPlay a play number (7.1); EnState's slot rows the play number, and STATE the previous snap as well as the
newest, so one arrival repairs one loss; EnHit and EnHitAck the hitter's incarnation and a swing number (8.3);
EnBones pieces of 176 bones; EnOffer and GONE rows a dying mark; EnKill travels at the death's start; a
DEATH_SIGNAL type; EVENT kinds 18 ragdoll start {the request of main+0x1bddcb0} and 19 relay {a camp message};
blackboard keys are FNV-1a of the names this game's own table lists. Controller weights are sent and not applied
until a bench shows a blend that differs. The table row `En` gains: the hold's end, the gate and calc timers of
4.4, the last hit by this player, the offer's time, the backlog, the hit map of 8.4 and the swing memory of 8.2.

11.1 **Declaration**: owed to every peer when an enemy is claimed, when its name, life or owner changes, on NEED, on
a join, and to a peer that ran again after LOADING or after being PAUSED for over 5 s (a peer paused for less keeps
getting PLAY and EVENT and needs none). PLAYs and EVENTs owed to a peer in one phase 0 go as one message per 1,100
bytes, each event with its own length.
A receiver passes the pair through 4.0; `decl_no` is compared only against the last declaration taken **with the
same pair**.
11.2 **STATE** is dropped unless its sender is the entry's owner and `epoch8` its epoch's low byte; a STATE for an
unknown id for 0.5 s sends NEED(id). An entry with no state for 3 s from an owner whose heartbeat runs asks NEED;
4.4's levels cover an owner that does not.
11.3 **INDEX** (unreliable, per peer, every 2 s, not while a declaration is owed to that peer): count and the sum of
per-entry hashes over (id, epoch) of every LIVE enemy the sender has declared to that peer, and the number of
declarations sent to it. Compared only by a receiver that has taken that many; a mismatch on two in a row: NEED(0,
0) with a number, at most once in 10 s; the answer ends with INDEX_END (the number, count, hash) on the bulk stream
after the last declaration; an entry of that sender not named by then asks NEED(id) once (4.5).
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

`TOTK_COOP_ENEMY=1` (default off; when it becomes the default is the tester's decision after step 3's bench, and
`COOP_NET_VERSION` rises then). Each game states which enemy sync it runs in its HELLO; two games that differ say
so once in the log and on screen, and neither syncs enemies. Tests stay in the code: `TOTK_COOP_TEST_ENPROBE` (D's runs), `TOTK_COOP_EN_DELAY_MS`,
`TOTK_COOP_EN_NOCLOCK=1`, `TOTK_COOP_EN_NOSWITCH=1`, `TOTK_COOP_NET_TEST=<latency>,<jitter>,<loss %>,<reorder %>`.

## 14. The record

Every line and every motion sample carries the PC's performance-counter time (one clock for every game on a PC), and
the report pairs the two games by it: an error of the clock estimate then shows as a position error.
- `coop: en: change:` owner, epoch, life, copy state, verb, target; `coop: en: own:` every claim, offer, take,
  refusal, with its cause; `coop: en: unsynced:` with the reason;
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

16.1 **The dispatcher** (`coop_msg.c`): R2A's change list. It owns the pause queue (moved out of
`coop_ent_own.c`) and one ordered queue per sync; `coop_msg_next(sync, &m)` hands each reliable message once, by
type range, after the incarnation test; a sync that does not consume stalls both and nothing is dropped; the entity
sync's early handlers (GRANT, REFUSE, TAKE while paused) become a function the dispatcher calls; join and leave
events go to both. `coop_ctr.c` gets `coop_ctr_pump()` (phase 0 calls it), a round-trip accessor, and arrival times
taken in `coop_net.c`'s thread, where the test network sits too. A send the transport refused: the sync keeps the
message, in order, and sends it first at its next pass (counted); a KILL refused is covered by the tombstone list
owed to a peer that ran again. Values that change every frame travel in STATE, never as EVENT.
16.2 **Session state** (`coop_sess.c`): PAUSED, LOADING, the heartbeat flags (one `coop_ctr_hb_set` a tick for
both syncs), the frame stamp, the clock estimate per peer (6.4), the round trip per peer, the capability in HELLO.
The entity sync keeps its own reading of a silent peer (older than 2 s: no flags); the enemy sync reads 1 s of
silence as paused and 3 s as left.
16.3 **Out of `coop_world.c`**, bodies unchanged and linkage changed, in this order: (1) a header
`coop_world_int.h` with the shared types and the registry's symbols (about twenty statics become external); (2) the
actor registry and key walkers (`coop_actors.c`), whose `seen[]` gains the kind index; (3) the ground sync; (4) the
stand-in's pose, animation and trail with the animation helpers they call; (5) contacts, link hits, the hit-update
dispatcher and the HIT channel's demultiplexer as one file, since they share one state.
16.4 **The old enemy sync off** (`TOTK_COOP_ENEMY_OLD` unset), gate by gate: `peer_one` skips enemies but not
mounts; `coop_enemy_puppet_calc`, `_phase`, `_skel`, `_writer`, `coop_enemy_action` return at once; the old spawn
sync does not announce `Enemy_` actors; `link_hits_send` and its takers are off; `coop_audit_enemies` writes
nothing (`en_report.py` replaces `sync_audit.py`'s enemy part for play sessions too); the stand-in rule of the
contacts filter stays; the copy branch of that filter, `ce_take`, `spawn_take`, `epose_take_tick` and the bare hit
taker act by kind, so mounts keep them and an enemy announced by a peer on the old sync is not made here;
`coop_enemy_within` and `coop_near_my_enemy` are answered on the tick from the list of places phase 0 publishes.
Addresses both generations hook (main+0x41bb20, 0x50ad74, 0x599670, 0xaf100c, 0x1b4b738, 0x300a30, 0x283c750): the
enemy sync's handler is called first and says whether it ran the function.
16.5 **The object mirror** (step 3, R2A's list): an entry point on the co-op tick, queued from phase 0, that finds
or makes the row, marks it made here and exempt from both 20 m tests; arrows go to the arrow tracker, which asks a
published set of PROJ actors; a weapon of a name a player wears is not skipped for these rows; a copy's own
detached equipment is deleted at the EQUIP event; a row whose maker's actor goes while another Link is within 20 m
is handed to that game. Until this exists, loot of an enemy killed far from its owner's player is not shown to the
other.
16.6 **Hooks to add** (one rebuild of the generated code, with step 0): main+0x4dca24 (7.1), main+0x1b6f6a0 (5.4),
and any other address runs 13 to 22 call for, collected first.
16.8 **The bench**: the scene `enemy` (both Links placed, the camp found, an aim helper), the script passing the
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
21. two players before the camp: a stand-in in the Sight list, slot 0 written to it, whom the enemy fights;
22. the co-op tick's rate and the transport's receive rate, by a counter (step 0);
23. a crate dropped on a switched-off enemy and a fire lit under it: life and L+0x1780 per frame;
24. a release during a knock-down, and after three hits taken while off: slot commands, status bytes and ragdoll
    state every frame.
Runs 13, 14, 19, 20 and 22 come before step 2; 16, 18, 23 and 24 before step 3.

## 18. Not taken from the reviews, and why

- Review RC proposed re-aiming a projectile in the aimed player's game by the stand-in's offset: taken (8.7), but a
  shot's hit is still that player's game's alone.
- "Hits held for a paused owner are appended after it resumes": not taken as a rule of its own; a hitter that gets
  no HITACK in 2 s logs the hit as lost and the bar counts it, since a paused owner in a fight is claimed from
  after 1 s (4.4) and the hit is then sent on.
- Whole-game slow motion in the heartbeat: not in steps 0 to 5; the record notes when a game's world clock runs
  slow against its wall clock, and the bench decides.
- A pack as a unit of ownership: dropped (section 4).
- From the second review: a give-back after a pause and ownership by request (TAKE) are not in this specification;
  a way back is added only if the bench asks. Slow motion of a whole game in the heartbeat: as above.
- Sections 5 to 10 keep statements the second review marked as unmeasured or wrong in detail where a run named in
  section 17 comes before the step that builds them; each such step begins by correcting its section from its runs.
