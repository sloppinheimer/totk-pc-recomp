# Enemy sync: the design

Status, 2026-11-15: first draft. Nothing in it is built or run. It rests on two rounds of game-code readings
(nine verification reports and nine gap reports, kept outside the repository; cited here as V1..V9 and G1..G8) that
re-checked [enemy-system.md](enemy-system.md) against the game. Where this file and enemy-system.md disagree, this
file is the later reading. A statement marked [inf] rests on an inference of a reading, [open] is not read yet (section
14 says what settles each), [run] can only be settled by a game run. A third round of readings (R1 the copy switch,
R2 the native action modules and the animation blackboard, R3 a census over every enemy) is under way; sections 4, 6
and 12 are provisional until it is in.

The specification (data, messages, procedures, tests) is written from this file once the [open] points that change the
structure are closed, and is reviewed by readers that did not write it before any code, as the contraption sync was
([contraption-sync-v4-spec.md](contraption-sync-v4-spec.md)).

Conventions. Addresses are TotK 1.4.3, main+offset. "Here" means in this game. An **owner** is the one game that
simulates an enemy; every other game shows a **copy**. A **stand-in** is the actor that shows another player here.
An incarnation is a game's 64-bit id for this session.

## 1. What must be true for the player

1. Both players see the same enemy in the same place doing the same thing, close enough in time that they can fight it
   together: a swing one player sees is the swing the other sees.
2. An enemy moves and animates as smoothly in a copy as in its owner's game. Position, lag, stutter and animation are
   each measured and each has a bar (section 7), from the first build on.
3. A hit lands when the player who made it saw it land. A player is hit when that player saw the blow reach them, and
   their guard, parry and dodge are judged against what they saw.
4. An enemy fights the two players as one fight: it has one target at a time, chosen by a stated rule, and does not
   flicker between them.
5. An enemy dies once, drops its loot once, counts once, and stays dead or comes back (blood moon) in both saves alike.
6. None of this depends on which player is the host, who walked up first, or who is looking.

## 2. The model in one page

- **One owner per enemy.** The owner's game runs the enemy exactly as the single-player game does: every behaviour
  graph, perception, the character controller, damage, death, drops. Nothing is taken out of the owner's enemy.
- **A copy plays and never thinks.** In every other game the enemy's actor exists, but none of its behaviour graphs
  run (section 4). The mod gives it what the owner's enemy shows: where it is (section 5), what its animation slots
  play and at which time (section 6), what it holds (section 10), its life, and the events that are not visible in
  those (section 8, 9). The copy still runs the engine parts that turn that into a picture and into contact: the
  animation update, the attack sensor that arms from animation events, the hit sensors, the model.
- **Decisions are made in exactly one game each.** What the enemy does: its owner. Whether a player's attack hit the
  enemy: the attacker's game, against the copy or the original it sees; the hit is sent to the owner, who applies it
  as if it had happened there. Whether the enemy's attack hit a player: that player's game, against what it sees.
- **Ownership is explicit and sticky.** It changes by a message with an epoch, as in the contraption sync, and only
  when the owner can no longer simulate the enemy (section 3). It does not follow who is nearer.
- **Identity does not depend on the save.** A placed enemy is known by its placement hash, a run-time one by an id
  its owner mints. The owner states the actor name; a copy is whatever the owner says it is (section 11).

Why not let the copy run the owner's action requests (the "native" model of the code today): the action graphs hold
about 250 nodes with world effects (drops, shots, shock waves, noise, the death request) among the 120 that play
animations, and the requests themselves are issued by a graph (the action sequence) that also decides (G1). A copy
that acts would have to be gated node by node, and one missed gate is a duplicated arrow or a second death. A copy
that only plays cannot cause a world effect at all; what it then lacks is a finite list of visible things, each sent
as a stated event (section 8).

## 3. Ownership

3.1 **The unit.** Ownership is per **pack**: an enemy, the actors that are parts of it (weak points, arms, a rider
and its mount, the weapons and devices it holds, section 10), and every enemy that the game ties to it in a group
that exchanges messages (a camp, an army, a swarm) [open: the census R3 says which components make a group]. A pack
has one owner; a boss's parts are never handed over apart from its body (seen in a run of the old code).

3.2 **Who.** An enemy nobody owns is FREE. A game claims a FREE pack when it has the pack's actors and updates them
(the actor is in the part of the world this game simulates). Two claimers are settled by (epoch, incarnation), as the
contraption sync's `apply_owner`.

3.3 **Hand-over, only when the owner cannot go on:**
- the owner's game stops updating the enemy (it left the simulated range, or is being unloaded): the owner offers the
  pack; a game that updates it claims it. With no claimer the enemy rests: its last state is kept by the owner for
  its return and nothing is sent;
- the owner loads, pauses for longer than 1 s, or leaves: a game that updates the pack claims it.
There is no hand-over because another player is nearer or is the target. The old code's rule (nearest wins, margins of
3 and 5 m, no message) flapped when a player stood 30 m off, and once gave a fighting enemy to a game in which it
stood still. Whether a fight against a far owner's enemy feels late is measured (section 7); a hand-over to the
targeted player is added only if that measurement asks for it, and then as a request the owner grants between two
actions.

3.4 **What a hand-over does.** The new owner turns the graphs of its copy on again (section 4), sets the brain verb
the old owner last stated (requestBrainVerb main+0xad9844, force), the form key, the target, and lets the game's own
reset (main+0xa12a48 re-roots the brain, resets the scheduler and the utility graphs) start the behaviour from there.
The enemy may begin its current action again; a hand-over is rare and never happens in the middle of a hit reaction
or a death (the offer waits for them). The old owner turns its actor into a copy in the same frame it takes the GRANT.

3.5 **During a load here**, copies are neither frozen nor taken over (a load hung in a run of the old code): the table
is kept, the actors are let go, and everything owned is offered once.

## 4. Stopping a copy's thinking

Facts (G1, on the Bokoblin; R3 extends them): an enemy has up to nine graph units (main, sub and three special action
slots, brain, utility, and two more utility graphs) plus, in its scheduler, the action-sequence graphs, an interrupt
graph and a move-control graph. They are run by one runner body that exists three times: main+0x50b404, main+0x42e0c8
and a copy inlined in the actor's calc (main+0x41bb20 at 0x41c6b8) that runs the brain [inf]. The running action
sequence has its own inlined loop (calc 0x41f7f4).

The game's stop timers are **not** the switch: they stop the brain only, not when an interrupt is pending or the enemy
is dying, and any active timer makes the life update throw the frame's damage away (V1, G1).

The switch has one item per runner (G1 6.1). The mod's hooks are at function entries only, so each item is either an
entry hook that returns at once for a copy, or a field the mod writes each frame that makes the game's own test skip
the runner [open, R1: the exact set]:

| Runner | Candidate switch |
| --- | --- |
| scheduler (picks the action sequence) | entry of main+0x44ff30 |
| utility graphs, brain bootstrap, post-sensor utility | entry of main+0x50b404, by the controller's actor |
| action slots | entry of main+0x50ad74 (hooked today) |
| the other controllers | entry of main+0x42e0c8, main+0x2beb80 |
| running action sequence (inline) | never started when the scheduler never runs [inf] |
| brain (inline) | a flag the calc tests before it (BSA+0x26ad bit 5 skips the brain) [open], or a hook on a callee of the inlined body |
| interrupt and move-control graphs | not located [open] |

What stays on in a copy: the pre-part of the calc, the animation update (main+0x432fa0), the attack arming from
animation events (main+0x42ec98), the life update (with the frame's damage taken away, section 8), the death tick
(driven, section 9), the perception receiver update (so a copy has an emitter and can be seen by local actors), the
physics result and the model.

The switch is **proved by a counter, always on**: per copy and frame, the number of runner entries that reached a
graph. It is 0 or the session record says which runner ran (section 13).

Global side effects that then happen only in the owner's game, as they should: reads of "the player" and of the camera
in 36 to 71 enemy graphs, the global no-attack time, messages between enemies, the battle-music request (G2).

## 5. Where a copy is: motion

5.1 **Pose-driven.** A copy's main body does not simulate. Its body is made static or keyframed (main+0xae4300) and
its pose is set every frame (ForceSetMatrix main+0x45ce70) from the owner's states [run: that an enemy's controller
accepts a static main body]. The controller's forward direction, speed and move state are written too, so every local
reader (foot placement, animation inputs, other actors' queries) agrees with what is shown. A copy is never eased
toward a target: the old ease lags 0.1 s times the speed (2.5 m for a galloping Lynel) and snaps.

5.2 **States.** The owner sends, per enemy, a state stamped with its world clock: position, rotation, linear
velocity, move state with the landed and left-ground edges, the ground it stands on (5.5), forward direction and
speed. Every owner frame while the enemy or a player near it moves; once a second at rest. Unreliable, latest only.

5.3 **Interpolation, never extrapolation.** A copy shows the owner's enemy a fixed delay in the past: the measured
jitter of the state stream plus one state interval, held between 50 and 150 ms. Between two states: a cubic on
position that uses both velocities, a slerp on rotation. A state marked as a teleport (a warp of a Wizzrobe, a Yiga,
a Like Like's swallow) is taken at once, with no path between. When states stop arriving the copy stands at the last
one; it does not run on.

5.4 **Things that are motion but not walking**, each stated by the owner as an event at a clock time, so the copy's
animation and its path agree: a jump (start, launch velocity), a knock-back (the hit that caused it travels with the
reaction, section 8), a fall, a climb or a ceiling hold [open: how a Horriblin holds a ceiling]. The path itself
always comes from the states; the event only says why.

5.5 **Moving ground.** An enemy that stands on a moving thing (a platform, a contraption, a mount) is stated relative
to it, as riders of contraptions are, so the two streams' delays do not show as sliding.

5.6 **Ragdoll.** The owner states the ragdoll state (none, partial, full; component [[a+0x228]+0x3e8] +0x20) and,
while it is not none, the bones. The copy's physics would fall differently, so it is not asked. The copy enters and
leaves the state at the owner's word [open, R2: the smallest enter and leave].

5.7 **Animation-driven movement** is off on copies (41 enemies have it), since the pose already holds it.

## 6. What a copy does: animation

6.1 **The copy runs the game's animation update.** It turns commands into clips, raises the animation's events
(attack windows, sounds, weapon trails, footsteps) and arms the attack sensor from them (G5). The copy is therefore
told what to play, not which bones to hold.

6.2 **The owner states, per animation slot:** the command (a name hash), its rate, and for each live blend
controller its node and its clock (slot state S at G+0x20+i*0x48: command S+0x10, rate S+0xd0, controllers
*(S+0x50)+0x28.., clock ctx+0x68). On a change at once and reliably; the clocks with every motion state. Also: the
actor's time rate (actor+0x280: hit-stop, slow motion), the animation blackboard values the behaviour writes
(DirectionAngle, GroundNormal, StickX, UserSpeed, the play rates [open, R2: the full list and where to read them]),
and the look-at target (section 9.4).

6.3 **The copy applies:** a command change by the game's play request (main+0x4dc628; an equal command is left
alone), then the clocks. A copy runs the same delay behind as its position (5.3), so its picture is one moment of the
owner's enemy, not a pose from one time at a place from another. Phase is corrected softly: an error under 100 ms is
closed by running the copy's rate up to 10% off for a while; a larger one is written into the controller clock
[run: that writing ctx+0x68 with flag bit 10 holds].

6.4 **Randomness.** The animation system draws a random number at each command start and slot update. The copy's
draws differ, so wherever a draw selects a clip the owner's result (controller node and clock, 6.2) is what the copy
is given, not the draw [open: the reader of S+0x1e4].

6.5 **What an action did besides its animation** is the list of section 8. Nothing else of an action reaches a copy.

## 7. The quality bars for motion and animation

Each is logged all the time for every copy near a player, compared in a bench that runs both games on one machine
with a shared clock, and has a number that the first bench run replaces with a measured one. The bench places a fixed
camp; spawned test enemies wander and are not used.

| Measure | How | First bar |
| --- | --- | --- |
| position error | copy's place minus the owner's place at the same owner clock time | standing 2 cm, walking 4, running 8, Lynel gallop 20, jump peak 15, knock-back 15, fall 20 |
| delay | owner clock time shown by the copy, behind the owner's | 50 to 150 ms, steady within 10 ms over a minute |
| stutter | frame-to-frame change of velocity beyond what the owner's own path has | no frame above 3 times the owner's own, none visible in a slowed capture |
| snaps | pose changes larger than a frame of the stated velocity, not marked as teleports | 0 |
| animation phase | copy's controller clock minus the owner's at the same clock time | 33 ms (one game frame) |
| command start | time from the owner's command change to the copy's, minus the delay | under one frame |
| foot slide | ground speed of a planted foot | as the owner's own, within 2 cm a frame |
| hit reaction | time from a hit in the hitter's game to the first frame of the reaction there | measured first; bar set after |

A scripted test counts only when pictures from both games show the same thing.

## 8. Hits and what follows them

8.1 **A player hits an enemy.** In the hitter's game the hit is built by the game against the actor it has (original
or copy): the hit entry (0x1c0 bytes) holds everything the reaction is chosen from (V5, G7). If the actor is a copy,
the mod takes the entry right after the sensor pass, removes it from the copy's own damage (a copy never changes its
own life), and sends it to the owner, reliably, with its game-local fields (the two actor links, the body pointer,
the attack id, the stealth id) turned into ids both games know. The owner appends it to its enemy's list right after
its own sensor pass and restores the count after the damage step (so a skipped frame does not count it twice). From
there the owner's game does what it does for any hit: reaction, life, death.

8.2 **The reaction comes back as what the enemy does:** the interrupt action's animation command (section 6), its
knock-back as motion with a stated cause (5.4), a ragdoll (5.6), a dropped weapon (section 10), a condition look (ice,
burn, shock: stated as the condition and its time left, shown by the copy's own model controller [open]). The hitter
sees it one round trip plus the delay later. If the measured time is too long for small hits, the hitter's game
plays the flinch at once from the same table the owner uses (the reaction key is a function of the entry, G7) and the
owner's stream corrects it; this is added only on that measurement.

8.3 **An enemy hits a player.** Each game decides for its own player. A copy arms its attack sensor from its own
animation [open, R3: which enemies arm from a behaviour node instead; those are armed by an owner event], so the
player's game sees the blow when its picture shows it, and guard, parry, dodge and the flurry rush are that game's
own. In the owner's game the enemy's blow passes through the other player's stand-in without damage, as today. What
the enemy learns of the outcome (guarded, parried, hit) is sent to the owner by the hit player's game [open: what the
enemy's behaviour reads of it].

8.4 **Projectiles and thrown things.** Only the owner's enemy shoots or throws (a copy has no node that could). Each
projectile is stated as its own event (name, launch pose, velocity, clock time, an id), reliably, not as a field of
the enemy (a burst of arrows was lost that way). Every game flies its own; the game of the player it reaches decides
the hit and says so, and the projectile ends everywhere.

8.5 **Things other than players that hit an enemy** (a falling rock, fire, another enemy, a contraption): the game
that owns the thing that hit decides, by the same route as 8.1.

## 9. Life, death and what a copy must show without thinking

9.1 **Life** is stated with every motion state; a copy's life component is written, so the health bar and anything
local that reads it agree.

9.2 **Death is the owner's.** When the owner's enemy starts to die, the owner states it: cause, second cause, the
clock time. A copy starts the same death (RequestDeath main+0x1b4b738 with both causes) with its switches set so that
it rolls no drops and writes no kill flag (D+0x2c bits 0 and 1) and can be killed by nothing else (bit 7 until then).
The Die action's parts arrive as section 6 and 8 give them. The death's end, which the game waits for from a
behaviour node, is given to the copy by the mod when the owner says the death finished.

9.3 **Loot** is rolled by the owner only. The owner states the resolved drops; they enter the shared object sync as
things the owner made. The defeat counters (five game-data values, G3) are added by the owner's game only; the copy's
end does not add (gate main+0xb28394 for copies), since the values are shared.

9.4 **Awareness.** A copy must look the way the owner's enemy is, to the local player and the local game: the look-at
target (target slot 8, turned into the id of a player or a place), the state that the Sneakstrike prompt reads
[open: which field], the alert marks over its head [open: who draws them]. The owner states the battle target as a
player id; the copy's target slot 0 is written so that local readers of "its target" have one.

## 10. What an enemy holds

An actor whose holder chain ends at an enemy (main+0x1d19380) belongs to that enemy's pack: weapon, shield, bow,
the Zonai device fused to them, a carried rock. Equipment is created only for the owner's enemy; the hook on the
equipment create (main+0xaf100c) states the full argument list and a copy's equipment is created from it (the weapon
swap by level uses a save-dependent score, so a copy never picks its own). Equip, drop, throw, pick up and removal
are stated by the owner as events on the equipment user's request records; on a copy the four paths that create or
drop without a behaviour node (equipment re-create main+0x1b2bf1c, unequip replacement main+0xb299bc, the drop
manager, the shooter) are gated. A dropped weapon leaves the pack and becomes a shared object.

## 11. Identity, spawning and going away

11.1 **Ids.** A placed enemy: its placement hash ([actor+0x370]+0x88, equal in every game) and a life number its
owner raises each time the place is filled again, so a respawn is not taken for the enemy that died there. A
run-time enemy: (owner's incarnation, serial). A child made by an actor-holder, a nest or an army keeps the id its
owner minted; copies never create children.

11.2 **The owner states the actor.** The actor name at a placement can differ between two saves (the level sensor
swaps it by a score that is refreshed at unknown times, G3). The owner's name wins: a game whose own placement made
another name puts that actor away (the placement's suppress bit, job+0x98 bit 0, not the save's flag) and makes the
stated one.

11.3 **A game makes a copy** for every stated enemy within the range it shows enemies at [run: the range] when it
has no actor for the id: by adopting the actor its own placement made (same hash, same name), else by a create with
the stated name and pose. It lets a copy go when the enemy is beyond that range or its owner states it gone.

11.4 **Run-time spawns** proceed only where their creator is owned or there is none (G6): summons, split parts, nest
and army members are made by the owner's actor and stated; the same create on a copy does not happen (its graphs are
off) or is gated. The enemies the game places around a player by itself (night Stal, field Keese, pop-up Lizalfos)
are each game's own for its own player and are stated like any other; that two players side by side get twice as
many is prevented where the spawner can be gated [open: the auto-placement's create call].

11.5 **Kill flags and the map.** The owner's kill writes the save's flag through the placement (main+0x195321c); that
call is stated, and a receiver makes the same call on its placement, which sets the flag and the suppress bit (the
flag alone would bring the enemy back at the next load, G3). The 165 enemy flags the shared game data leaves personal
today (reset mask 80) join the shared set.

11.6 **Blood moon.** The reset writes the flags straight into the table inside the game-data update, where the
mod's capture does not see it. Both games' clocks are shared, but what triggers the moon is not read [open]. The
rule: one game (the session's first) decides a blood moon and states it; the others run theirs then.

## 12. Enemies that are not a Bokoblin

All detailed readings were made on one Bokoblin. The census (R3) lists, for each of the 279 enemy actors, its graph
units, native modules, components and movement kind. Known already: 225 use a character controller; 54 are
rigid-body parts (Talus arms, Stal heads, rocks, barriers), which follow the pack's body or the rules for shared
objects; flyers are controller bodies in the free move state; bosses read the camera and "the player" in their graphs
(harmless on a copy that does not think, but the owner's boss then watches the owner's camera: section 14). Each
family gets a row in the specification saying what it needs beyond this file, and a bench case.

## 13. The record: what is logged from the first build

As for the contraption sync, every claim a player makes about an enemy must be answerable from the session record of
that play, without a second run:

- per enemy and second, in both games: id, owner, epoch, life, brain verb, target, animation commands, position; a
  line on every change of each;
- per copy: the thinking counter (section 4), delay, position error and animation phase as section 7 defines them,
  every snap, every hard phase correction;
- every hit sent, received, appended and its result; every event of sections 8 to 11 with its clock time in both
  games;
- ownership: every claim, offer, grant and refusal with its cause;
- the mark key writes a numbered line and a picture in both games, as today.

A tool pairs the two games' records by id and clock time and lists every disagreement.

## 14. Open points, and what settles each

By reading (third round, under way): the copy switch as entry hooks and fields, the interrupt and move-control
runners (R1); the native action modules, the animation blackboard, entering and leaving ragdoll (R2); every other
enemy family, which enemies arm attacks from a node, what makes a group (R3).

By reading, not yet asked: the field the Sneakstrike prompt reads and who draws the alert marks; what an enemy's
behaviour reads of its attack's outcome on a player; the reader of the animation's random draw; the auto-placement
create call; the blood-moon trigger; condition looks on a copy; how boss parts are created and linked; whether a boss
that reads the camera needs the target player's camera stated to the owner.

By a run (each needs the tester's yes; all are logging runs on one placed camp, nothing is changed in the game):
1. the identity of the brain controller and the per-frame count of runner entries on an untouched Bokoblin (G1 8b);
2. the same counts with the switch on, and that the animation update still raises its events;
3. a static main body under ForceSetMatrix on an enemy; the controller's readers afterwards;
4. a command played by the mod with the clock written: does the phase hold;
5. the Sight list order with two Player emitters over a minute; target slot 0 per frame;
6. the first motion bench: the measures of section 7 on walking, running, a jump attack and a knock-back.

## 15. The code that exists

runtime/coop_world.c (about 6,000 lines) is today's enemy sync in the "native" model. Its inventory found every
suspected defect present (one action request kept per actor, no verb or form key sent, a hit appended at the wrong
point and counted twice on a skipped frame, ownership by an unspoken rule on second-old packets, drops rolled twice,
kill flags untouched). It is not repaired. Order of work:

1. Move out what other code needs, unchanged, into files of their own: the actor registry and key walkers, the
   stand-in's animation, pose and trail, the ground sync, the hit-update dispatcher, the demultiplexer of the HIT
   channel.
2. Build the enemy sync new, in co-op files (`coop_enemy*.c`), on the transport and the ownership rule the contraption
   sync uses, with the record of section 13 and the bench first.
3. Delete the old enemy logic when the new one passes its bench; keep its probes as test tools.
