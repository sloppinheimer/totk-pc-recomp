# Enemy sync: the specification

Status, 2026-11-15: fourth draft; a first form is built and runs (section A below says what, and where it departs
from the sections after it, which remain the plan for what is not built). It turns [enemy-sync-design.md](enemy-sync-design.md) (the
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
Section 4 of the third draft was walked once more by scenarios (R3C): the earlier counter-examples were closed, and
three new walks ended badly (a game that loaded or joined did not know the others' epochs; a message could arrive
before the claim it depended on; a change of owner could undo or double a death). The fourth draft's section 4 has
the pair travel on the same stream as everything that depends on it, keeps resting records through a load and sends
them to a joiner, makes a death a fact any game states and nobody takes back, puts the gate's waiting time on the
closing side, and removes the answer rule.

## A. As built (2026-11-15)

Files: `runtime/coop_enemy.c` (table, identity, ownership, messages, record), `coop_enemy_copy.c` (the switch, motion,
animation), `coop_enemy_hit.c` (life, hits, death), `coop_enemy.h`; `tools/coop/en_report.py`. On with
`TOTK_COOP_ENEMY=1`, for the kinds `coop_enemy.c kind_of` lists (Bokoblin, Moblin, Horriblin, Lizalfos by actor name);
`coop_world.c`'s older sync keeps every other kind, and every enemy made at run time (no placement hash), and can be
turned off whole with `TOTK_COOP_ENEMY_OLD=0`. A game
says which sync it runs in a heartbeat flag; a peer without it stops the enemy sync for the session (said in the log,
not yet on screen).

What is as the sections below say: identity by placement hash and life number (3.1); ownership per enemy by the one
rule of 4.0 with CLAIM, OFFER, GONE, resting records, tombstones and the full statement at a join and after a load
(4.1 to 4.5, 4.10, 10.6); the far gate as "runs it"; the hold of a new actor and of a claim; the copy switch's hooks
and four writes (5.1, 5.2); a static main body put at the stated pose before every calc (6.3); the clock of
`coop_sess.c` (6.4); the death request and the signal on a copy (9.3); tombstones.

Where it departs, each by a measurement or to stay simple:
- **Nothing of an enemy travels as an event.** The owner states each enemy whole (STATE, the latest per enemy:
  place, rotation, velocity, life, the death word, and per animation slot the command's name hash, rate and the
  clocks of its blend controllers), read at phase 0. A copy plays a command by the game's play request when a slot's
  stated command differs from its own, and keeps the clocks by bending the rate (an error under 3 thirtieths of a
  second) or writing the clock. A command's name travels once per name and peer (reliable). So there is no PLAY or
  EVENT stream, no play number, no capture ring and no hook on main+0x4dca24; a lost state costs a frame. A state is
  sent every frame while a player is within 40 m or the enemy moved, at once for three frames when a command or the
  death word changed, else once a second (a standing enemy's clips run on between states by their rate).
- **All reliable messages are on the control stream**, the declaration too: one order per sender.
- **The net tick reads the reliable messages** and changes the table (it runs in a menu and a load); phase 0 looks
  at actors. There is no transport pump.
- **The actor is written as well as its body** [m]: with the move control turned away the camp's copies' bodies were
  at the pose asked and their actors where they had stood; the copy's actor transforms (actor+0x2b4 and +0x2f0, each
  a place and a 3x3 rotation) are written with the same pose. Controller speed and direction are not written (run 13
  is not needed: a held actor's body is static and it is put where it stood).
- **Hits are life, not entries.** A copy's own damage step runs untouched, so the blow and the flinch show at once
  in the hitter's game; the life the copy lost since it was last written (the game takes a hit's damage at the next
  calc, not in the step [m]) is sent to the owner as HIT (points), and the copy shows the lower of the stated life
  and its life after that hit for 1.5 s. The owner takes the points from its enemy, asks the ragdoll component for a
  flinch (rag+0x149 bit 0) and, at 0, asks the game for the death (cause 16). A stand-in's hits on any enemy this
  sync knows are taken out of the hit list (`coop_world.c contacts_filter`) [m: left in, a watching game sent them as
  its own and every blow counted twice]. Not built: the hit entry sent whole and appended (8.3, 8.4), so the owner's
  enemy does not learn who hit it and plays no big reaction to another player's blow; HITACK and resending.
- **The hand-over** (not in the drafts; `TOTK_COOP_EN_HANDOVER=0` turns it off): an enemy fights the player of the
  game that runs it, so the owner offers an enemy to the game of a player whose Link is within 35 m of it and 6 m
  nearer than its own, not in its first 6 s with this game, not during an interrupt action, not again for 10 s when
  the offer was not taken. A player who strikes it gets it sooner: for 5 s after another player's HIT the enemy is
  offered to that player's game when its Link is nearer at all or the owner's is over 15 m off (after 3 s with the
  owner, within 80 m) [m]. The target of section 12 would replace both.
- **"Runs it" needs the runners to come** [m]: the far gate as read was open for an enemy 70 m off for which no graph
  runner was entered, and the game that "ran" it took it and then looked paused. A game runs an enemy when its calc
  ran, its gate is open and a graph runner of it was entered (let through or turned away) in the last 1.5 s. A far,
  idle camp member (a few runner entries a second or none) then rests, each game keeping its own, until a player
  comes near.
- **A death** is stated by the owner when its death word becomes valid (DEATH, with a tombstone in every game) and
  again when its actor is gone (DEATH_END, the entry is freed). A copy still dying then thinks again and ends by its
  own Die action. An actor at a tombstoned placement gets the dead call and a delete request (not yet seen in a run).
- A copy's written life is forgotten at every change of owner [m: kept, a game that became a copy again sent the
  difference as its player's hit, and an enemy handed back and forth lost that life each time until it died].
  Still open: a hit in flight at a hand-over can be lost (the taker starts from the life it last showed).
- Two games that come together: the smaller incarnation waits half a second longer before its first claims.
- **Equipment is state too.** STATE carries, per equipment slot (8), the name hash of the held actor. A copy that
  holds something where its owner's enemy holds nothing for three looks (0.8 s) has that actor deleted by request; one
  that holds nothing where the owner's holds something is given it by the game's equipment create (main+0xaf100c,
  called as the stand-in's gear is: in the hand, default life, no fused attachment), at most once in 2 s [m: a sword
  knocked away, fetched again and lost again followed on the copy]. The dropped thing itself is the object mirror's.
  Not carried: the fused attachment, the weapon's remaining life.
- **The owner's flinch on its copies** [m]: the ragdoll component's state travels in STATE; a copy whose owner's
  enemy went into a partial ragdoll asks its own component for one (rag+0x149 bit 0), unless this game's player
  struck it within the last second (it flinched here already). Eight of eight in a run. Full ragdoll was only seen at
  death, where each game's actor plays its own.
- **The test network** is in the relay (`server/coop_relay.py --bad "<delay ms>,<jitter ms>,<loss %>"`, or the
  environment variable `TOTK_COOP_RELAY_BAD`), on every forwarded packet. At 40 to 60 ms with 2 % loss and at 120 to
  180 ms with 8 % loss [m]: tables equal, copies within 6 cm of their owner while running, every hit taken, kills
  ended in both games; the delay rose to 63 and 92 ms. The harsher run first found a fault in the transport
  (`coop_ctr.c fill_window` overwrote a lost message's slot and the stream died), fixed with it.
- **Shots** (`coop_enemy_shot.c`; research R6, R6b). An enemy makes no projectile at the shot: its Shooter
  component ([comps+0x448]) makes them in advance and a shot is PREPARE main+0x1bf9af8 then SHOOT main+0x1bfbc44 on
  a record; the projectile's own component polls the record and launches, flies and ends by itself. A copy's graphs
  never call either [m]. The owner's hooks run the call and state it with the request block it was made with (its
  runs of zeros squeezed out, 240 to 960 bytes); a copy makes the same call on its own record of the same key and
  index at T. A prepare the copy's game refuses (arrows are made on request, which a copy never asks) is preceded by
  the request main+0x1bfd6a8 and tried again for up to 150 frames. Seen [m]: a Fire Wizzrobe's fireballs and the
  camp archers' arrows shot on copies in both directions, every call answered 1 after the first arrow's wait; the
  other game shows the fireball land and light the grass. The older object mirror no longer sends these enemies'
  arrows. Open: what a mirrored projectile hurts and lights (the element sync asks `coop_enemy_shot_mirror`);
  an Octorok's rock and a Lizalfos's spit not run.
  - **The request block** has two layouts. Both hold a list of named parameters: a header (capacity u32, pointer
    to the cells, count i32) and 32 cells of 0xc8 bytes (type u32 +0, pointer to the name +8, the name's capacity
    0x80 +0x10, the name +0x14, the value +0x98). A shoot's block has its vtable at +0, the header at +8, the cells
    from +0x20 and its numbers from +0x1920 (attack type +0x1924, key +0x1928, flag bytes +0x1938..); its
    parameters are `Range`, `InitPos`, `Velocity`, `MotionProperty`, `Level`. A prepare's block has the key at +0, a
    pointer at +8 (kept by the record at +0x50; into the game's image, so the same in every game), the header at
    +0x18 and the cells from +0x30. Cells past the count and a name's bytes after its end are stack leftovers and
    are sent as zeros; a cell's pointer to its own name is written again where the block is rebuilt.
  - **A held thing's shots** [m]. A fire rod's flame is shot by the ruby fused to the rod, not by the Wizzrobe: the
    owner states with each prepare and shoot whose Shooter it is for (`via`: 0 the enemy, 1 + k what its equipment
    slot k holds, 0x80 | (1 + k) what is fused to that) and the copy makes the call on its own actor of that place.
    Every ready projectile of an enemy and of what it holds counts as that enemy's for the mirror query.
  - **The flight** [m] is the copy's own physics from the same launch. The launch speed and direction of a rod's
    flame are the shoot's `Velocity` parameter; the projectile's aim point ([proj comps+0x440]+0x100) is set to its
    own start by the game in both games and gives no direction. Five flames of a rod: the copy's within 3 cm of
    the owner's at 6 and at 30 frames. Arrows: the same path, one frame apart in the samples.
  - Open [m]: the two id pairs a shoot takes from its shooter (record +0xe4..0xf3, by main+0x482760: a fresh pair
    per swing of the owner's rod, from the weapon's [comps+0x428]) are 0 on a copy, whose rod starts no attack.
    The flight does not need them; what reads them (who a flame may hurt, once per swing) is not read yet.
- **Run-time enemies** [m]: an enemy with no placement is known by the key the older spawn sync gives it when it
  announces it and makes the other games' actors (`Enid.b` = `EN_RT`); owner, motion, life, shots and death are
  this sync's. No tombstone is kept for one.
- **A young actor** [m]: the four writes keep and put back all four values (the root index is two 16-bit halves;
  a new actor's second half is 0), and a new actor thinks until its interrupt graph has started before it may be
  held. Switched off from its first frame, a spawned Lizalfos went on fighting at 0 life with no death.
- **What an enemy holds is not a loose object** [m]: held equipment and what is fused to it are kept out of the
  object mirror (a Wizzrobe's rod carries a ruby; the mirror showed a second gem floating beside the copy). A held
  actor is told by its live state, not by the slot link's id word, which a Wizzrobe's rod does not pass.
- **Not drawn** [m] (research R7, R7b). A Wizzrobe's unseen flight is the model controller's request byte
  ([comps+0x280]+0x9d, "depth and shadow only") with its dirty bit 0x200 at +0xac, as the game's setter
  main+0x1b87b74 writes them; the controller's apply main+0x306e54 does the rest. The byte travels in STATE and a
  copy, what it holds and what is fused to that get it with the dirty bit. It is neither an animation nor the
  hidden mask at +0x9b. No hand-over is offered while an enemy is not drawn, and a copy that thinks again is drawn
  again: a Wizzrobe handed over mid-flight stayed unseen and still for good in a play session.
- **Families tried by spawning** [m] (`TOTK_COOP_EN_KINDS`, `TOTK_COOP_TEST_SPAWN`, `TOTK_COOP_TEST_SPAWN_KEY`):
  Lizalfos, Moblin, Horriblin, Chuchu, Keese, Octorok, Fire Wizzrobe, the two Zonai constructs: made in the other
  game, bound, followed with no leak, also in a tester's half-hour session. A Yiga footsoldier's blinks are not
  followed well. The default list is these families (`kind_of`): Bokoblin, Moblin, Horriblin, Lizalfos, Chuchu,
  Keese, Octorok, Wizzrobe, the two Zonai constructs and the Yiga footsoldier; swarms are left out.
- Traffic [m]: a state is 232 bytes with one animation slot in use; four enemies in a fight are 240 states and 56 to
  70 kB a second to each peer.
- **PAUSED in a menu** [m]: phase 0 and the actors' calcs go on in the inventory menu, so 1.3's test does not see it.
  A game says PAUSED when no graph runner has been entered for a second for any of the live enemies it runs.
  In a run the other game took the paused owner's six enemies over after 5 s and the fight went on.

Measured, two games on the camp save (2026-11-15, `build\en6` to `en15`): both tables equal at the end of every run;
no change of owner in a fight but the hand-overs; no leak; a copy within 5 to 12 cm (95th percentile) of its owner
while running and under 1 cm standing; the same command in 97 to 100 % of samples; no frame not at the pose asked; a
non-owner's blows each taken once by the owner; a kill ended in both games; the owner's game closed mid-fight and
the other took its enemies over. Pictures from both games show the same enemies at the same places.

Seen with the test settings (`TOTK_COOP_EN_TEST_HIT`, `_KILL`, `_BLIND`): a kill by a non-owner's hit alone ended in
both games; a game that took its actors into the sync 40 s late bound them as copies mid-fight, and its actor at a
placement whose enemy had died in the other game was put away and did not come back.

Not built or not seen: loot beyond the older object mirror, the target, the moon
message, big reactions and ragdoll bones on a copy, the orphaned death of 4.7, a load of the owner in a run, a third
player, a shot of the notice on screen when two games run different syncs (the notice is made and logged). One run in about thirty stopped its co-op tick in
both games just after they met; `coop: en: the net tick has not come round` now says where when it happens again.

Conventions. Addresses are TotK 1.4.3, main+offset. "The world" is the game's memory; "the table" is ours. An
incarnation is a game's 64-bit id (`coop_ctr_me`). Every timer is wall-clock time; state times are on the owner's
world clock. Marks: [m] measured in a test run; [inf] an inference of a reading; [open] not read, with the rule that
holds until it is; [run] to be settled by a named run of section 17. No mark: read in the game's code by a reader.
"Here" means in this game. "This Link" is `coop_player_actor()`; another player's stand-in here is a `PlayerPuppet`
actor. `comps(a)` = [a+0x228]; `BSA(a)` = [comps+0x158]; `L(a)` (life) = [comps+0x2f8]; `DC(a)` (the death
component) = [comps+0x1c8]; `G(a)` (the animation player) = [[[comps+0x40]+0x18]+0x18]. An actor's generation is the
u32 at actor+0x10, its state the byte at +0x24 (`ctn_gen_of`, `ctn_alive_as`).

## B. What a copy does besides moving (2026-11-16)

A copy's graphs do not run, so nothing its AI would do happens there unless this sync makes it happen. Found one case
at a time so far (a shot, an unseen flight), then three more in one play session: a rod's flame not seen in the game
that copies the Wizzrobe, no hurt from a copy's fireballs to that game's Link, a rain of fireballs that stopped and
started with each change of owner. This section is the rule for all of them. Research: R8a, R8b, R8c, R9.

**Not by replaying AI nodes** (R9). The eleven families' graphs use 1,367 node classes, at least 315 of them acting on
the world, 138 of those in one family only; most take inputs worked out at the call from the target or the
blackboard, 40 hold something between their enter and leave, and many must never be made twice (spawns, drops, death,
movement, messages to other actors, a game's player-wide settings). A node cannot be run alone from stated values.

**By the entries the nodes end in.** What acts on the world passes through a few component entries. The owner's game
runs the call and states it with its data; the copy's game makes the same call on its own actor of the same place
(the enemy, what its slot k holds, what is fused to that: `via` of section A). What is held (armed, a weather asked
for) travels in STATE as a value, not as a start and an end.

| Entry | Carries | State |
| --- | --- | --- |
| Shooter PREPARE main+0x1bf9af8, SHOOT main+0x1bfbc44 | every projectile | built (A) |
| Play request main+0x4dc628 | every animation, and with it the animation's own events | built (A) |
| Attack arm main+0x1bf20cc (attack component [comps+0x428], attack block) | a swing, an armed body part | B.3 |
| ELink main+0xa98cf8, SLink main+0xa98d00 search-and-emit (user, key) | effects and sounds a node starts | B.5, by a list of keys |
| Magic weather request main+0x21cdf44 (manager, type, reserve, actor) | a Wizzrobe's weather | B.5 |

**B.1 Who decides what.** The game that runs an enemy decides what its attack does to the world: the ground, objects,
other enemies. It states that (the element sync's cells and things; a hit on an enemy is the hit sync's). Every game
decides what an attack does to its own Link, from its own actor of the attacker: a stand-in takes no hit anywhere
(`coop_world.c contacts_filter`). So a copy's attack must be whole towards this game's Link and nothing towards the
rest. The element sync's reaction hook already lets a shown-only actor's reaction reach this Link alone and its
ground contact nothing; the "does not interact" bit it also set on a mirror's component (bit 32 of [comp+0x20])
stops the element's whole contact step (the update main+0x30efa0 turns it into bit 33, which the element step
main+0x18d32c4 reads), Link's included, and makes the element answer "not active". The bit is not set on a mirror.

**B.2 An enemy's AI memory is state.** What a graph keeps in the actor's blackboard and acts on later is the enemy's,
not the game's that happened to run it: a Wizzrobe's `IsMagicWeather` (set by the weather action at an animation
event, cleared only when the brain goes back to wandering; while it is set the always-running graph asks for the
weather every frame and shoots a fireball at a random interval, with no timer: R8c) stayed in the game that had run
the summon. The keys a family's graphs set and read across actions are listed per family and travel in STATE; a game
that takes an enemy over writes them to its own actor through the game's blackboard setter before its graphs run.
First list: Wizzrobe `IsMagicWeather`, `IsShootMagicObj`. Built [m]: the bools travel as bits of one STATE byte and a
copy's blackboard is given them at every pre-calc as the game's setter writes a bool (main+0x740b68, the actor case:
the value at [bb+0x1a0], not when its lock byte at [bb+0x1e0] is set, its change counter at [bb+0x1f0] up by one). A
run: the owner's Wizzrobe set `IsMagicWeather`, the copy was given it, the owner's game closed and the other took the
Wizzrobe over with the bool true. Its graphs then cleared it at once: a taken-over enemy's brain starts unaware (the
clips after the take-over were Fly, Find, Disappear), and the bool is cleared whenever the brain is not in battle. So
the rain still ends at a change of owner; what it needs is the brain's state at hand-over (section 12, the target),
not more memory. `IsShootMagicObj` read false in the owner's actor blackboard while fireballs were shot: it is kept
somewhere else [open].

**B.3 Melee.** A swing is armed either by the node ExecuteSensorHitSetupAttack (main+0x2444d80) or by the attack
component's own poll of the playing clips' AtCollision events (main+0x42ec98), which reads no AI state and arms 148
of the 221 enemy actors (R8a, R9). [run] whether that poll runs on a copy and its swing takes this Link's life once.
If it does, nothing is added for those; for the node-armed ones the owner states the arm (the attack block) and the
copy makes it.

**B.4 Never made on a copy**: spawns and deletes, drops, death (their own syncs), messages to other actors, a game's
player-wide settings (GameBalance), UI, boss and event nodes, physics set-up, chemical switches, movement, lifting
and throwing, camera shake.

**B.6 The brain at a change of owner** (research R10). A copy's brain graph never started: at its first tick as this
game's own its root Enter picks an idle verb, the idle module notices the player afresh and plays the Find clip, and
what is cleared outside battle is cleared. "In battle" is no flag: it is the brain verb ([BSA+0x58]; 12 land battle,
6 discovered, 18 wander) with target slot 0 naming a live actor (holder [comps+0x4b0], slot k at +0x28 + k*0xe8, its
actor link at +8, kind 2 at +0xe2). STATE carries the verb and whom slot 0 names as a player (0 no one, 1 + net id,
0xff another actor). The game that takes an enemy over, before its graphs first run: writes the memory of B.2, fills
slot 0 with its own actor of that player (its Link or its stand-in) by main+0xa70a1c(slot, link, options) and clears
slot 6 (main+0x83be9c), asks the verb with force (main+0xad9844(BSA, verb, 1)), and for four frames turns away a
different verb asked by the graphs and the target slots' reset (main+0x24f0a9c), putting both back at each of those
frames' pre-calc. Measured [m]: a Fire Wizzrobe whose owner left: verb 0 -> 12, slot 0 filled, the start's verb 18
and the reset turned away, and the next clips were MagicAttack, Disappear, Fly with no Find. The brain's start does not always come in those four frames: in three of four runs it came seconds later, after the
hold, and the Wizzrobe went to verb 18, then 6, with a Find clip. So the hold goes on in a narrow form until the start
has been seen and turned away once, 20 s at most: only the idle verbs the root's Enter chooses among (1, 2, 8, 18,
19), asked without force, and the slots' reset are turned away. Handed over with both players staying, three runs of
three [m]: verb 12 for the 20 s logged, no Find clip, attack after attack, `IsMagicWeather` kept where it was set and
fireballs shot by the new owner (10 after 8 by the old one). The target it is given is the other player's stand-in:
it is seen every frame (the slot's time since seen stays 0) and fought, and in one run the brain itself changed to
this game's nearer Link after 12 s. Section 12's question, whether an enemy fights a stand-in in slot 0, is answered
yes for a Wizzrobe. Not done on a copy: asking the verb while it is a copy (R10 4 ii).

**B.7 A ragdoll on a copy** [m] (research R12, R12b; `coop_enemy_pose.c`; `TOTK_COOP_EN_POSE=0` turns it off). The
ragdoll component's state ([[comps+0x3e8]+0x20]) is 0 none, 1 recovering, 2 partial (a flinch), 3 full. A copy does
not start its own full ragdoll. The game reads a ragdoll's bodies back into the pose in the actor's model phase
(main+0x2752c8 inside main+0x245030), turns the pose into the skeleton's parent-local records (main+0x289620) and
builds the final matrices from them (main+0x24cc30, only for a model marked changed: bit 0 of model+0x240). While its
enemy is in state 3 or 1 the owner's game sends, after that phase and every other frame, the records (12 bytes a
bone) with the actor's place and turn of the same instant; the copy's game, after its own model phase, writes the
actor, the model's matrix and the records, marks the model changed and pushes. Measured: 6 ragdolls, 21 poses laid
side by side by three bones' places from bone 0: 0.1 cm median, 0.2 cm at worst with the actor's place and turn
written from the packet. That write is not made: the model's matrix (model+0x1f8) on a copy was found stale, 5.5 m
from the actor, and is set from the copy's own actor before the push instead; bone 0 then moves 7 cm by the put (the
whole model stays where the copy stands) and the three bones agree to 2.3 cm median, 50 cm at worst, the copy's
stated turn being a little behind the pose's. Not seen in pictures up close.

**B.9 Loot, a weapon let go, the kill counters** [m] (research R13; `coop_enemy_loot.c`). A copy makes no drops (its
death's reserve and release sit under the death component's "no drops" bit). The owner's game notes each item its
dying enemy's drop asks for (main+0x599670 called inside main+0x1c78060) and each weapon an enemy it runs lets go of
(main+0x1b738e4), and the object mirror takes the noted ones as this game's made objects wherever they are: before,
loot far from the owner's Link and every weapon near an enemy stayed in the owner's game alone. Seen: a Bokoblin's
sword and its Item_Enemy_77 made in the owner's game and copied in the other. A copy's death record arms the kill
counters' writer (main+0xb28394) as the owner's does; on a copy the record's done bit is set at its entry, so the
shared counters go up once [built; the counter's value not read in a run]. A game with no actor at a dead enemy's
placement is NOT given the save's defeated flag by this sync, on purpose: the co-op mod does not write one game's
world progress into another's save unless `TOTK_COOP_SHARE_WORLD=1`, and with that on the flag already travels by
value (research R13 section 1). Within a session the tombstone keeps the enemy away. Open: a camp's cleared record.

**B.10 A player's blow on a copy, sent to its owner whole** (`coop_world.c`, the older sync's contact packets; OFF
unless `TOTK_COOP_EN_FWD=1`). The points-only HIT gives the owner's enemy a flinch and no more: the reaction is
chosen by the struck enemy's own selector from the contact entry, which only its owner's game runs. Built: the
contact entry of this game's Link on a copy is kept there and sent whole to the owner's game, which adds it to its
enemy's contact list at its next hit update with the stand-in as attacker; life lost on the copy within three frames
is then not sent as points too. Measured [m]: the blows arrive and the owner's enemy reacts by itself (flinches, full
ragdolls). But in the game that runs it the struck enemy flew into the sky at every blow it survived (a play test
with pictures): something in the sent entry launches it. Which field is not found, so it is off and a blow goes as
lost life, as before. [open: the entry's fields that must be made again in the owner's game; then a shield's block
and a parry reaching the owner the same way.]

**B.11 Equipment** (research R13 section 3). STATE also carries, per slot, what is fused to the held thing and the
slot's drawn or put-away state. A copy lets go of a held thing of another name than its owner's (a swap), makes a
thing again with what is fused to its owner's when its own has none, and draws or puts away as its owner's
(main+0x4f59ec). [built; a swap, a fused thing and a sheathe not seen in a run.] Not built: a throw replayed on the
copy, a picked-up thing taken from the copy's ground.

**B.12 The blood moon** (research R13 section 5, G3 Q6; built, not seen in a run: no moon came up). At the moon's
start in a game (main+0x21cd650) the life number goes up by one; every tombstone, resting record and entry of a
placement's earlier life is dropped; the other games are told (MOON) and do the same. A game told of the moon within
five minutes does not raise the number again when its own moon begins. Entries of enemies made at run time stay.
[open: a tombstone whose save flag the moon does not reset (reset masks 208 and 80: cave and sky enemies) is dropped
too, since the mask is not read; a game is not made to start its own moon when told of another's.]

**B.13 A drop or a throw, at once** [m]. A drop (main+0x1b738e4) or a throw (main+0x1b73be4) by an enemy this game
runs is told to the other games at the call (EQUIP), and their copies let go of that slot's thing at their next
pre-calc instead of after the three looks of the held-things rule. The let-go thing is the owner's loose object
(B.9).

**B.14 A throw made by the copy** (built, not seen in a run: no enemy threw). The owner's game sends the throw's
request block (0x30 bytes) and its two last arguments with the EQUIP message; the copy's game makes the same call
(main+0x1b73be4) on its own equipment user, so what flies there is its own and can hurt its own Link (B.1). The
copy's thrown thing is no loose object while it flies and is deleted once it rests (20 frames still, or 10 s); the
owner's thrown thing is taken by the object mirror once it rests, and is the one object that stays in every game.

**B.15 How a copy's blow on this game's Link ended** (research R11; built, not seen in a run: in the run after it
the copies struck the other player only). The game classes each blow when it builds the hit entry (byte +0x0b: 0 a
hit, 1 guarded, 3 guarded at the right moment, 4 avoided). In this Link's hit update each entry whose attacker is an
enemy's copy, or a thing a copy holds or shot, is told to the copy's owner once per blow (OUTCOME). The owner's game,
for a guarded blow, sets its enemy's attack pause as the game's own node does after a guard (the float at
[[comps+0x268]+0x3c] = 10 + 5 x a random number below 1). Not built: the enemy's repelled or guard-break reaction
(with B.10 on, the shield's contact on the copy may carry it), and anything for an avoided blow or a flurry rush.

**B.16 The prompts a copy offers** [m] (research R4 1.3, 1.4). The attention sender ([comps+0x130]) keeps a reason
mask per client type at +0x24..0x64; a client with a reason is not offered to the player's game (Sneakstrike awake is
type 0x2c, asleep 0x2d). Graph nodes write them, so a copy kept the loaded zeros and offered a Sneakstrike on an
enemy in battle. The owner states the 0x40 bytes when they change (ATTN); a copy's are changed by the game's own
setter, main+0x4e603c(sender, reason bit, client type, 1 offered | 0 not), which also marks the clients' records, at
most six calls a frame. Measured: the owner's Bokoblins set the awake reason in battle and clear it out of battle,
and their copies were given each change. Not seen: the prompt itself on screen.

**B.18 A copy starts no ragdoll of its own** [m, seen by a tester]. A hit Bokoblin's copy soared into the sky and
snapped back down. The cause was the flinch this sync asked of the copy's own ragdoll component when its owner's
enemy flinched (section A): a partial ragdoll on an actor whose main body is static and whose root is written throws
the model until it ends. The recorded places of actor and copy never left the ground; only the drawn model did. With
the flinch request off the tester saw no more of it. Now every ragdoll start on a copy is turned away at the
function's entry (main+0x1bddcb0), the one its own damage step asks when this game's player strikes it included, and
the owner's flinch (state 2) is sent as bones like a full ragdoll (B.7). Measured: 7 flinches and 2 full ragdolls,
243 poses sent and put, bones agreeing to 0.1 cm (median).

**B.17 What a copy's graphs would have asked of other systems** (research R16; `coop_enemy_fx.c`; off with
`TOTK_COOP_EN_FX=0`).
- **An effect or a sound a node starts by key** [m]: the owner states actor, key and the node's effect and sound flags
  at the entry of OneShotXLinkSearchAndEmit (main+0x25aafec); the copy's game makes the emits on its own actor's
  users ([[comps+0x68]+0x70] and +0x78) by main+0xa98cf8 and main+0xa98d00. Only what a node starts is stated, so
  nothing an animation event already starts on a copy sounds twice. Seen: a Wizzrobe's `WeatherFlash` started on its
  copy. Not built: the held ones (a Yiga's `Sign`, `Vehicle_Magic`), the emit with a matrix, the stop of a sound.
- **A Wizzrobe's magic weather** [m]: the owner notes the type its graph asks the weather manager for
  (main+0x21cdf44), STATE carries it, and the copy's game makes the same call for its own actor at each pre-calc
  while it is stated (the manager keeps a claim four updates and reads nothing of the actor). Seen: the copy asking
  type 1 while its owner's weather was on. The sky was not looked at.
- **A weapon picked up from the ground** (built, not seen: no enemy picked one up): the pick-up is main+0x1d71dfc
  (weapon link, picker, 1), not main+0x1bb16a0, which only stores the AI's memory of a weapon. The owner states which
  object the weapon was by the object mirror's name for it; the copy's game deletes its own actor of that object, and
  the held-things rule gives the copy the weapon. [open: a placed weapon nobody has moved is not known to the mirror
  and stays lying in the copy's game.]

**B.19 Five more families** [m] (2026-11-17). Each was spawned beside one game's Link with the other game watching,
standing: Like Like (same command in 99.3 % of samples), Lynel (99.7 %, 19 cm at worst while moving), Evermean
(Treant, 98.7 %), Gibdo (Zombie, 100 %; 98.9 % and 0.5 cm in a camp fight), soldier construct (Toby, 99.2 %): made in
the other game, bound, followed, no leak, tables equal. They are in the default list (`kind_of`). A baby Frox was
94 % and 25 cm off its pose and is left out; an Aerocuda's actor name was not found. Names with WeakPoint, Dummy,
Bullet or Boss are left out. None of the five was fought beyond the Gibdo's run: their attacks, parts and special
moves (a Like Like swallowing, a Lynel's charge and bow, an Evermean's disguise) are not looked at.

**B.20 The battle-info component: measured, not carried** (research R15). [comps+0x140] holds the flags the life
gauge, the marks over the head and the battle music's focus hang on (+0x38). Measured in a camp fight [m]: an enemy
in battle has 0x720 there in the game that runs it (0x1720, 0x1f20 and 0x10720 at moments), and its copy has 0 or
0x200. STATE carries the owner's word and the record prints both at each change. Writing the owner's word on the
copy each pre-calc did not hold: the copy's own component puts its own value back every frame. What works the flags
out on an enemy that runs (so what a copy lacks) is not read, and the list the battle manager keeps
([*main+0x395e090]+0x4548) links a component at an offset of its own, not at +0x58. **Answered by B.21** [m]: with
the copy's brain verb asked the game's way, the game's own procedures work the flags out on the copy too. In a camp
fight the copy's word was the owner's 0x720 while its enemy fought (0x520 for a moment at the change), where it had
been 0 or 0x200. [open: the marks, the gauge and the battle music themselves were not looked at or listened to.]

**B.21 A copy holds its owner's brain verb and battle target all the time** [m] (research R10 4 ii, R15 3 and 4). Not
only at a take-over (B.6): at each of a copy's pre-calcs, when its verb differs from the stated one it is asked with
force (main+0xad9844), which runs the game's own procedures for the verb (in battle: the head's look-at at target
slot 0), and when slot 0 does not name this game's actor of the stated player it is filled (main+0xa70a1c). Measured
in a camp fight: copies went 2 -> 6 -> 12 with their owner's enemies, slot 0 on the stand-in; tables equal, no leak,
position and command figures as before. Not seen: the head turning. Slot 8 is left alone (these verbs do not use it).

**B.22 One target at a time, no flicker** (design 9.4; built, not seen: it needs two players about as far from an
enemy). The game's own rule can swap an enemy's target between two players from one frame to the next (research G2
section 7). In the game that runs the enemy, a change of target slot 0 from one player to another less than three
seconds after its last change is put back: the slot is filled again with the player it had (main+0xa70a1c) at the
enemy's next pre-calc, and STATE goes on naming that player. A change to no one, or to something that is no player,
is left to the game.

**B.23 Held effects, and the spawner** (research R16, R5 Q1; built, not seen).
- A held effect (a Yiga footsoldier's `Sign`, `Vehicle_Magic`: ExecuteXLinkSearchAndEmitAndFade, enter main+0x24901b0,
  leave main+0x24902e0) is told at its node's enter and leave; the copy's game makes the emits, keeps the two handles
  and stops them by the node's own stop (main+0x6e34e8(the pair, -1)).
- Each game's auto placement puts ambient enemies around its own player, so two players together got them twice,
  each game's as the other's copies too. A game's push (main+0xa80f28, the name behind x3) is turned away while
  another game's live enemy of the same name stands within 60 m of its Link.

**B.24 Four more families** [m] (2026-11-17, the standing test of B.19): a Hinox (Giant: same command in 98.2 and
98.6 % of samples), a Talus with its arms as enemies of their own (Golem: 96.5 and 98.4 %; nine actors bound), a
Frox (Mogurudo: 98.3 %), a cave boss (CaveMaster: 98.2 %, 6.6 cm while moving): made in the other game, bound,
followed, no leak, tables equal. They are in the default list; a baby Frox stays out. Not fought: a Hinox's parts
and grab, a Talus's ore and its arms thrown, a Frox's swallow.

**B.25 Camps across owners**: nothing more to build. A camp's node runs in the game that runs the camp actor and
tells its members by messages (0x3ddc0006 to 0x3ddc000a; research R5 Q4); those messages are not replayed. A member
another game runs is shown by its own state (pose, commands, brain verb, target), which is what this sync states.
[open: a camp's cleared record, which no reading has found.]

**B.26 The bench** (`tools/coop/en_bench.ps1`; section 15's `-Bench` cases were never built). Cases: `fight` (a camp
fight: tables equal, no leak, copies within 1 cm standing, 12 cm moving, the same command in 95 % of samples),
`wizzrobe` (shots made on the copy, the copy game's Link burned, the weather asked for), `handover` (a pause: the
taken-over Wizzrobe in battle for every second logged), `quit` (the owner's game closed: taken over), `families`
(each default family spawned and followed). A case whose event did not happen in the run is NOT JUDGED, not a pass.
A copy seen for under six seconds is not judged. Run on the build of 2026-11-17 [m]: `fight` PASS (10 copies: 0.2 cm
standing, 3 cm moving, the same command in 96.5 % at worst), `wizzrobe` PASS (6 shots stated and made on the copy,
the copy game's Link burned; no weather attack came up), `handover` PASS (in battle for 20 of 20 seconds logged),
`quit` PASS. Seen in it: a far cliff guard's copy played the game's far-wait clip (`WaitBattleLODMgr_Wait`) while its
owner's enemy played `LookUp`, for the 2.5 s it was owned: the game's own far clip on a copy is not overridden.

**B.27 A guarded blow shown to the owner's enemy** (research R17; OFF unless `TOTK_COOP_EN_PARRY=1`). In one game an
attacker learns that its blow was guarded from a second list of its own SensorHit component ([comps+0x428]+0x58,
entries of 0xa8 bytes, cleared and filled by the component's calc from the frame's contacts): the entry's class byte
+0x0b has bit 0 for guarded and bit 1 for guarded at the right moment, and the enemy's graphs ask whether any entry
of the frame has it (QueryIsOnHitFlagGuardJust, main+0x2686148; main+0x2632958) to choose their repelled reaction.
The class is worked out from the defender's own guard state, which only the defender's game has. Built: on an
OUTCOME of class 1 or 3 the owner's game adds one entry with that class byte to its enemy's list at the enemy's calc,
for three frames. Measured with the test setting `TOTK_COOP_EN_TEST_OUTCOME` [m]: the entries were added (30 times)
and the attack pause set, but no repelled clip was played: the test tells the outcome at moments that are not the
enemy's blows. The added entry's handles are not made (the slot's old bytes), and one reader of the list follows a
handle, so it is off until the reaction is seen with a real guard and the handle is made properly.

**B.28 The world file is taken apart** (2026-11-17; the older enemy sync lives in `runtime/coop_world.c` with five
other things, and it can only go once they are out). Moved as they were, no behaviour changed:
- the bone helpers of the ragdoll pose, into `coop_enemy_pose.c` (the new sync no longer reaches into the world file
  for a pose);
- the stand-in's animation, masks and sword trail, into `coop_standin_anim.c`;
- the players' body poses and the stand-in that shows them, into `coop_standin_pose.c`;
- the rotating ground's sync, into `coop_ground.c` (its packets still come in through the world file's reader of the
  HIT channel, which hands them to `ground_take`).
- the contact packets a player's blow on a copy travels by, the contact filter and the hit-update dispatcher, into
  `coop_contact.c`;
- the sync of enemies made at run time and its key table, into `coop_spawn_sync.c`.
`coop_world_int.h` declares what the split files and the world file share. The world file (about 1,450 lines) keeps
the actor registry and keys, the HIT channel's reader, and research probes.

**B.29 More families, and a weapon picked up from the ground.**
- Taken by default now [m] (the standing test of B.19, command the same in that share of samples): a Gleeok
  (`Enemy_Drake`, 99.8 %), a block construct (`Enemy_Zonau_BlockMaster`, 99.7 %), and the boss variants of families
  already taken, which a name rule used to leave out: a boss Bokoblin 99.7 %, an armoured Lynel 99.7 %, a Gibdo boss
  with its parts 98.8 %. Not fought. (A baby Frox, 91.4 %, was left out here and is taken since B.31.)
- `TOTK_COOP_EN_KINDS` is asked before the name exclusions, so a left-out name can be tried.
- A pick-up (B.17's ENM_PICK) of a weapon that lies placed in the world, not one a game dropped: the message carries
  the weapon's place, and the copy's game finds its own actor of that name within 2.5 m of it among all its actors
  (`coop_actors_all`) and takes it off its ground, as it does a dropped one. Built, not seen.

**B.30 The older enemy sync is gone** (2026-11-17).
- The enemy sync is on unless `TOTK_COOP_ENEMY=0`. `TOTK_COOP_ENEMY_OLD` and `TOTK_COOP_ENEMY_MODEL` mean nothing now.
- Removed from `coop_world.c` (4,459 lines to about 2,150): the owner choice and hand-over by nearness, the WORLD
  packets and the dead list, the place and action streams (EPOSE), the frozen-copy model, the lost-life and
  enemy-to-Link hit replays, the target redirect, and their tests. Removed from `coop_net.c`: the WORLD and EPOSE
  channels (their packet kinds 6 and 10 stay reserved). Removed from `coop_spawn.c`: the calls into all of these; the
  addresses stay hooked and do nothing (taking them out of the hook list is a full rebuild, left for later).
- Kept in `coop_world.c`: the actor registry and keys, the sync of actors made at run time (now for every enemy:
  its key is the enemy's identity here), the contact packets a player's blow on a copy travels by and the hit-update
  dispatcher, the HIT channel's reader, and research probes.
- Horses and animals (`TOTK_COOP_MOUNTS=1`, written and never run) rode on the removed code and are removed with it
  (`coop_mounts.c`); a remote rider's stand-in is placed as any other. To be built again on this sync's design.
- An enemy's arrow is no longer sent by the object mirror's "made near an enemy this game runs" rule: a copy's shot
  is made from its owner's shoot request (section B.3).
- The far-copy rule's "a player is here but has sent no state" sign is now the session heartbeat, not a WORLD packet.
- The audit file's `enemies` array is empty; `tools/coop/en_report.py` is the enemy sync's record.
- What this loses until built: an enemy of a family this sync does not take (the dungeon bosses, Ganondorf and his
  phantoms, Kohga's constructs, the Goron mid-boss, the sand worm, swarms, weak-point part actors, a baby Frox) is
  each game's own. (On a copy, a blow by a device another game runs is taken out of its contacts as before; a
  followed device striking an enemy this game runs is counted in both games, as it was: [open].)
- [m] before the code was cut, with only the defaults flipped: the bench's fight, wizzrobe and handover cases, tables
  equal, no leak. The state with the code cut builds and has not been run.

**B.31 After the first play session without the older sync** (2026-11-17).
- A baby Frox is taken (it had been left out for its 91 % command share). Left out, it was each game's own, and the
  spawn sync still made each game's Frox in the other: two Froxes fought apart where the players saw one.
- The spawn sync announces and makes only what this sync takes, and part actors (a weak point, a dummy, a bullet); a
  whole enemy of a family this sync does not take is each game's own and is no longer made in the other game.
  Not taken today, by the name rule against the census (30 of 221 names): the five dungeon bosses and their
  Uncleared and Underground forms, Ganondorf and his Miasma form, Phantom Ganon, Kohga's two constructs, the Goron
  mid-boss and its weak points, the sand worm, the Keese and bee swarms, the dungeon practice construct.
- The shield's fuse is found by the nearest actor at the shield; an enemy standing in a Link's place was taken for
  one and sent as the shield's material. An `Enemy_` actor is never a fuse.
- The swift-attack bit (research R4 section 5): bit 7 of the word +0x34 of an enemy's game-balance client
  ([comps+0x268]), set and cleared by the node OneShotGameBalanceSetSwiftAttack of its attack's graph. A copy's graph
  does not run, so STATE carries it (`gb`: 0x80 stated, bit 0 the bit) and the copy's is written before its calc.
  Built, not seen; whether a perfect dodge against a copy then starts the rush is not known (research R18, being
  read).
- The HIT channel's reader is `coop_hitchan.c`. Seven message types the design named and nothing sent are gone from
  the list (their numbers stay free).
- [open] A skeleton made by a game's own spawner stood 0.0 and 0.2 m from the other player's stand-in (twice, once
  in each game) and played no clip in the game that ran it; its copy stood in its idle clip inside the other player's
  Link. Not read: how the game keeps such an enemy undrawn before it rises, and why it is made at the stand-in.

**B.32 Every whole enemy is taken** (2026-11-17, the tester's call). The name rule now takes every actor of the
census (research R3, `q4_component_matrix.tsv`) that has a brain and a life: added are the five dungeon bosses with
their Uncleared and Underground forms, Ganondorf and his Miasma form, Phantom Ganon, Kohga's constructs, the Goron
mid-boss, the sand worm, the dungeon practice construct, and the swarms (the "Swarm" exclusion is gone). Left out by
name, as not whole enemies: weak points and secret stones, dummies, bullets, tornadoes, thorns, event starters. Not in
the rule at all: the census's System actors (the army managers, the dragons).
- [m] the standing test of B.19, one of each spawned beside a Link, 75 s: made in the other game, bound, tables
  equal, no leak, no crash; same command in that share of samples: the sand worm 99.2 %, a Keese swarm 100 % (20 cm
  off while moving), a bee swarm 100 %, the Goron mid-boss 100 % (33 cm off while moving), the practice construct
  99.7 %, Kohga's construct 100 %.
- Not run: the five dungeon bosses, Ganondorf, Phantom Ganon. None was fought; a boss stood in a field, not in its
  arena or its scripted fight. What each family's graphs do that a copy does not show is being read from the graphs
  themselves (research R19), not found by fighting each.

**B.33 What a copy does not show, read from the graphs; and the model's looks** (2026-11-17; research R19).
- Method: every outward-acting node class of every enemy family's AI graphs (578 classes over 95 families, 2,734 graph
  files; the 11-family census of R9 is reproduced exactly) was read to the game function it ends in and put in a
  bucket: carried by this sync (22 entry points), local to the game that runs the enemy, or a gap. By node instances
  in the graphs: 3,868 carried, 2,681 local, 9,748 gaps. About a fifth of the classes were placed by name only (no
  function at the node's address), most of them boss moves; native modules, sequence data and animation events are not
  in the count. The tables are outside this repository (they are derived from game data).
- The gaps by instances: physics on the enemy's own body 3,192 (a copy's body is static and posed from its owner's;
  each needs a judgement, not all need building); grab, hold, swallow, lift and throw of another actor 929; commands
  on a held weapon (hide, extinguish, state) 829; messages to other actors 684; game-balance client bits and timers
  564; shooter state other than prepare and shoot 532; mount and dismount 511; the model's looks 498; an attack or
  sensor armed by a node 377; blackboard writes 339; the sweep-out table 225; game-wide state 199; chemical
  generators switched 175; an actor deleted by a node 162; weak-point parts 151; smaller ones (noise, the notice
  mark, form change, territory, the block construct's own state, swarm state).
- Built from it: the model controller's looks. [comps+0x280] keeps each look in a field with a dirty bit in the word
  at +0xac, set by nodes a copy does not run (research R7b): the ice look's amount +0x4c (bit 0x1000), +0x54
  (0x10000000), the death effect's amount +0x58 (0x100), the fade +0x5c (0x20000), the hidden mask +0x9b (bit 1 when
  it goes between none and some), the model enabled +0x9c (0x100000), +0x9e (0x10000). STATE carries them (`look`,
  `lk`) and the copy's are written with their dirty bits before its calc. [m] a skeleton spawned by the test, copy
  bound, same command 97.1 %, no crash, the values stated; none of the looks was seen to change in a run.
- [open] the buried skeleton of B.31 is not explained by these: one spawned by the test plays a clip at once and
  has every look at its default. The buried state belongs to the game's own spawner's skeleton and is not read.

**B.34 What a slot holds is shown or not** (research R19: the nodes EquipmentUserRequestSetVisible and
RequestAllEquipmentVisible). The equipment user ([comps+0x230]) keeps a visible byte for each slot at +0x570 + slot *
0x100 + 0xf4; the game's setter main+0xced8c0 writes it and sets bit 0x200 of the slot's command word (+0xf0), which the
component's own calc acts on. STATE carries the byte in the top bits of the slot's state byte (0x40 stated, 0x80
shown); where the copy's differs for a slot that holds something, the byte and the command bit are written as the
setter does. [m] a camp fight: the case passes (tables equal, no leak, same command 98.2 %) and nothing was written,
the bytes being equal throughout; built, not seen to act. Not carried: the other commands of that word (damage
0x100, extinguish 0x20000, re-fit 0x10000, change state, the attachment's activate and detach), which are requests
the calc consumes, and the flag word +0x4600 (jump-fall attack bit 0x80000, arrow attachment bit 33).

**B.35 An attack armed by a node** (research R20, the SensorHit component [comps+0x428]; `coop_enemy_arm.c`;
built, not run: off with `TOTK_COOP_EN_ARM=0`). An attack is not a held flag: the arm call main+0x1bf20cc(component,
request) is made again every frame while the attack is on, and the component's calc drops the armed bits each frame.
The animation makes the call (main+0x42ec98, in the actor's calc, from the clip's AtCollision events), which happens
on a copy too; and nodes of the AI graph make it (ExecuteSensorHitSetupAttack and about twenty other callers), which
does not. By the graphs, seventeen actors are armed by nodes only (the plain, fire and ice Chuchus, a dungeon boss,
the Keese swarms): their copies never hit. Thirty-four more are armed by both.
- The request is 0x24 bytes without a pointer. The game that runs the enemy tells each call that does not return
  into the animation's function (ARM, type 61); the copy's game makes the same call once a frame before the copy's
  calc while the calls keep coming, and stops four frames after the last.
- Two held values are stated with the enemy: the attack layer override (byte +0x23, 0x16 none), written when it
  differs; and bit 0 of the "hit-receive sensors off" reasons word (+0x26), put on and off at its edges with the
  game's own calls (main+0x4e0edc when the word was 0, main+0x4f2384 when it becomes 0).
- Not known: whether the copy's pre-calc is early enough for the struck actor's own sensor calc in the same frame;
  what the other node-armed pieces need (the eat layer, the confused enemy's sensors, the all-sensor mask, the
  force-no-hit map [comps+0x258]).
- In the hook list for the next pieces, with no handler yet: the lifter's calls (lift at once main+0x1ba75d0, throw
  main+0x1ba796c, fall, the lifted actor's death, the lift requests), the shooter's sleep and drop-prepared calls,
  the rider's get-off, the sweep-out state setter. Research R20 has a recipe for each. Whether a lifted object
  already follows through the object mirror is to be seen in play before the lifter is built.

**B.8 One enemy's blow on another** [m]: on a copy, the hits of other enemies, of what they hold and of the
projectiles they shot are taken out of its contact list (`coop_world.c contacts_filter`); the game that runs the
struck enemy counts the blow from its own actor of the attacker. A hit that arrives after its enemy changed owner is
passed on to the game that runs it now (built, not seen in a run).

**B.5 Order.** B.1 (the bit) with a run that also says whether the flame is seen again; B.3's run; B.2; then effects
and sounds by key and the weather request, each when a play session or a run shows it missing.

**Not known**: whether the flame's missing picture is the bit (R8b: low to medium); whether a minutes-long rain is
the game's own (R8c: probably; no timer in the data); what ends an arm (R9); which effect keys the animations
already fire, so which a replay would double.

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
- **The net tick** (`coop_tick`; [m] run 22: twice a game frame in the world, 120 a second at 60 frames, once a
  frame at the title screen and in a load; the transport receives there): session state, the dispatcher's pause
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
split between owners by which game runs each member (4.11).

4.0 **One rule.** `en_apply_owner(id, owner, epoch)` is the only code that changes an owner or an epoch from a
message. It takes the pair only if greater, by (epoch, then incarnation), than the greater of the entry's pair and
the resting record's pair for that id; taking a pair deletes the resting record. On a change it clears the ring and
the backlog toward the actor; captures already made are kept and routed by the new pair. **The pair travels as
CLAIM on the control stream** (ids with their epochs), so one sender's CLAIM, PLAY, EVENT, DEATH, OFFER and GONE
arrive in the order sent; the declaration (bulk) only describes the enemy and changes no pair. A PLAY, EVENT, OFFER
row or GONE row is taken only when its sender is the entry's owner at the epoch it names; otherwise dropped and
counted. Messages are read in arrival order and each is judged against the table as it then is.
**Life only moves forward**: no pair, declaration or state turns DYING or DEAD into LIVE (9.2).

4.1 **"Runs it."** A game runs an enemy, whether it thinks it or shows it as a copy, when it has the live actor,
the actor's calc ran this frame, and its own far gate for it is open. The gate is the calc's own test: with
A = [[[BSA+0x25f0]+0x18]+0x150] and X = [A+0x38]: open when BSA+0x26ad bit 5 is set, or [A+0x41] bit 0 or
[X+0x2d2] bit 0 is clear, or f32 [X+0x264] is negative, or [[comps+0xb0]+0x84] <= [X+0x264]^2; read at phase 0
[run 20: the values]. "No longer runs it" means: the gate has been closed for 0.5 s without a break, or the calc
has not run for 0.5 s while the game runs, or the unload was requested. Only a game that runs an enemy claims it;
4.7 is the one exception.

4.2 **A new actor** has a held cell from its first pre-calc (1.1): the switch on and its controller's speed taken
away ([m] run 11: a switched-off enemy with a dynamic body walks on; [run 13]: the write). At the next phase 0, the
first case that applies:
1. a tombstone for its hash and life number, or an entry for it that is DYING or DEAD: 9.4;
2. an entry this game owns: the actor is bound to it; an offer under way is withdrawn if this game runs it;
3. an entry another game owns, LIVE: the actor becomes that game's copy (10.1);
4. an entry UNSYNCED, or the kind is not listed, or there is no session: **own**;
5. this game's gate for it is closed: **own**, with no entry; it comes back to this list in the phase 0 its gate
   opens (it then has thought, and is held in the middle of what it does: accepted);
6. else it is **claimed**: CLAIM with epoch = 1 + the greater of the entry's and the resting record's epoch for the
   id (0 when neither), then its declaration; it starts from the resting record's life and verb, and from its pose
   only within 20 m of where the placement made the actor. It stays **held** for H = the greatest, over peers whose
   heartbeat arrives without PAUSED or LOADING, of (smoothed round trip + 4 x its variance) + 0.1 s, a peer with no
   sample counting 0.2 s, at most 0.4 s; counted from the claim. Alone in the session, and for an id this game
   minted, there is no hold. While held it is a copy for 8.2.
[run 14: an actor held from its first frame and released: do its brain and interrupt graph start; if not, it is let
think for two frames before the hold.]

4.3 **Two claims** are settled by 4.0. A claimer still in its hold never thought; one whose hold had ended, or that
claimed without one (4.4, 4.5), is converted by 4.9: both thought for one crossing of the messages, which is
accepted and counted (a change taken back within 0.5 s is one change for the bars). A game that joined claims
nothing until every peer's answer to its join has ended (10.6), and for at least 2 s.

4.4 **Hand-over**, per enemy, each a level looked at every phase 0; a claim under this paragraph has no hold:
- the owner no longer runs it (4.1): OFFER (4.5). For the gate's cause the offer also waits, at most 1 s, while
  4.6's states last;
- the owner's heartbeat says LOADING, or none has arrived for 3 s (it left): a game that runs the enemy claims at
  epoch + 1;
- the owner's heartbeat has said PAUSED for 1 s, or none has arrived for 1 s: a game that runs the enemy claims if
  it is in that fight (its player hit the enemy within 5 s, or was hurt or made to guard by its copy within 5 s);
  after 5 s any game that runs it claims. The enemy stays with the claimer.

4.5 **OFFER** (control; rows of (id, epoch, resting state, a dying mark)). The owner goes on simulating. Any game
that runs the enemy may claim at epoch + 1 from the moment it takes the row until it takes the GONE; the offerer
becomes a copy when it takes the claim. The offer is **withdrawn** (nothing is sent, its clock stops) when the
owner runs the enemy again before a claim came; a claim that crosses is still taken. With no claim 2 s after the
offer: GONE (control) with the resting record; every game's actor for the id is then **own**, and every game keeps
the record. A game that claimed from an OFFER does not offer the same enemy by the gate for 10 s, unless a HIT from
another game's player arrives for it. A game that later runs the actor of a resting id claims it by 4.2.

4.6 **Not in the middle.** An offer by the gate waits (4.4) while the enemy is in an interrupt action (main slot
status byte BSA+0x26a8 is 1 or 2) or in ragdoll. In a forced hand-over the new owner gives the body the last stated
velocity; with `rag` 3 it starts its own ragdoll from the bones it shows; an enemy lying in a reaction clip gets
its forced verb when that clip ends [run 16, run 24: all three; in no log yet].

4.7 **An orphaned death.** A DYING enemy whose owner left, loads, or unloaded it (the OFFER row's dying mark; not
when the owner is merely paused: the copy waits) is claimed at epoch + 1 by any game that has a copy of it, whatever
its gate. Each game that takes such a CLAIM answers it on the control stream (ORPHAN_ACK; a game that made a claim
of its own for the id has sent that first, so the order shows it). The claimer ends the death (9.3) when every peer
whose heartbeat arrives has answered and none sent a greater pair; nothing that cannot be undone rests on a timer.
A GONE row with the dying mark makes no resting record: a tombstone if the death was a kill, else the entry is
freed.

4.8 **A copy becomes the owner**, in its next pre-calc. LIVE: the body dynamic again, velocity as last stated, the
switch off with the saved queue count and bit word put back ([m] on enemies that had been thinking), the brain verb
as last stated with force, target slot 0 from the cell [run 14 for an actor that never thought; the game's own
reset main+0xa12a48 is the fallback]. DYING: only DC+0x2c bits 0 and 7 are cleared. Phase 0 reads it from the next
frame and sends STATE and a declaration then.

4.9 **An owner becomes a copy**, when it takes the pair: if its actor's death word is valid it states DEATH (and
KILL) first (9.2); then the switch on, speed taken away, the body static, the ring empty. For every HIT it applied
within the last (round trip to the new owner + 0.1 s) it answers "not mine" again, and it sends its own player's
blows of that time as HITs; the new owner drops a (hitter, swing) it has seen.

4.10 **A peer that left**: its entries with no actor here become resting records (placed) or are freed (run-time).
A link that stalls without a leave gives two simulations of what both games then claim, and two sets of drops if a
death falls in it; nothing prevents that, and the record counts it.

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
peer **whose Link is within 150 m of the enemy**, every frame while a player is within 30 m of it, every second
frame while it otherwise moved (over 1 mm or 0.1 degree), else once a second; to farther peers once a second (the
receiver takes messages twice a frame, [m] run 22, so a state a frame is not overwritten unread in the ordinary
case; each STATE still carries the previous snap, 11.0). One `EnSnap` whose `us` is **the
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
sample of the last 3 s, **moving at most 2 ms a frame in either direction**; when it has been off by more than
250 ms for a second it is set at once and every copy of that owner takes its next pose as a teleport (built in
`coop_sess.c` with a self-test, `TOTK_COOP_TEST_SESSCLOCK=1`: steady, a latency step of 100 ms up and down, a jump
of 400 ms, a pause: T never ran backwards but at the one flagged jump, and was ahead of the newest state for under
a second after a step up). It is cleared, with the same
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

9.2 **A death is a fact.** DEATH, DEATH_SIGNAL, DEATH_END and KILL are taken from any game for an entry of that id
and life number that is not already further along, whoever owns it; an owner that takes DEATH for its own LIVE
enemy makes the request on its actor with bit 0 set (the sender made the drops). A game that holds DYING or DEAD for
an id and takes a CLAIM or declaration that says LIVE sends DEATH (and KILL if it has the tombstone) to that owner.
The kill hook captures for copies too, into the entry, so a 4.7 claimer can state KILL.
**At the owner**, phase 0 reads the death state word DC+0x20 of every owned enemy (bit 0 valid, bits 1 to
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
(the session wins over the save); **its resting records are kept**; it asks every peer for all (NEED), the
tombstones and the resting records. **A join**: the joiner asks the same of every peer, claims nothing until each
answer has ended (INDEX_END) and for at least 2 s, and holds every actor of a listed kind until then (the one hold
that outlasts 1.1's 0.5 s).

## 11. Messages

Through the dispatcher (16.1), types 60 to 89. Reliable messages are per peer and in order on a stream; control and
bulk are two streams with no order between them: **everything of an enemy but its declaration is on control**; a
declaration carries the frame stamp it was read at, and an event older than the last declaration applied for that
id is dropped. A message larger than 1100 bytes is cut into pieces with (number, piece, pieces), as the entity
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
typedef struct { uint8_t n; struct { Enid id; uint32_t epoch; uint8_t life, dying; } m[]; } EnClaim;           // 72, control
typedef struct { Enid id; uint32_t epoch; } EnOrphanAck;                                                       // 73, control
                                                                                                               // 74: resting records, in pieces
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
11.4 A CLAIM makes the entry when the id is not known (owner and epoch only; name and state come with the
declaration, asked by NEED after 0.5 s). PLAY, EVENT, EQUIP, PROJ and OUTCOME for an entry that has no declaration
yet are kept 2 s (64 messages) for it. NEED(id) is always answered: the declaration by the owner of a LIVE or
DYING entry; a GONE row with its record by a game that holds it resting; else "not mine" with the pair known. It
is asked again every 3 s while its cause stands.

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
22. (done 2026-11-15: 120 ticks and 60 frames a second in the world in both games of a pair) the co-op tick's
    rate;
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
