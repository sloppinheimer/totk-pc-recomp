# Enemy sync: the design

Status, 2026-11-15: second draft. Nothing in it is built or run. It rests on three rounds of game-code readings
(nine verification reports, nine gap reports and five follow-ups, kept outside the repository; cited here as V1..V9,
G1..G8 and R1..R5) that re-checked [enemy-system.md](enemy-system.md) against the game. Where this file and
enemy-system.md disagree, this file is the later reading. A statement marked [inf] rests on an inference of a
reading, [open] is not read (section 14 says what settles each), [run] can only be settled by a game run. Readers
have contradicted each other once already (which graph the actor's update runs inline, section 4), so every [inf]
that the structure rests on is on the list of runs.

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
and its mount, the weapons and devices it holds, section 10, the children it made), and every enemy that the game ties to it in a
group that exchanges messages: the members of a camp are the actors in the camp actor's link list (main+0x2af3d48),
to each of which the camp's own node relays its members' messages (R5); an army's members; a swarm's. Enemies also
hear each other's noise and send each other reaction, discover and investigate messages outside a camp (177 of the
221 enemy kinds, R3); two enemies of different owners do not hear each other, which is accepted for enemies that no
group ties together [run: whether it shows]. A pack has one owner; a boss's parts are never handed over apart from
its body (seen in a run of the old code).

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

Facts (G1, R1 on the Bokoblin; R3 over all 221 enemy kinds with graphs): an enemy has up to nine graph units (main
action on all; sub action on 55; three special action slots on 8; brain and utility on all; a second utility graph on
21; none uses the third) plus, in its scheduler, the action-sequence graphs (1 to 69), an interrupt graph (all) and a
move-control graph (178). One runner body exists four times: main+0x50b404, main+0x42e0c8, main+0x2beb80 and a copy
inlined in the actor's calc (main+0x41bb20 at 0x41c6b8). Measured on a Bokoblin (run 1, 2026-11-15): the inlined
copy runs the interrupt graph every frame; the brain has no per-frame runner, it is run in bursts, a few times a
second, through main+0x50b404 from the BSA tick's "not yet ticked" branch (0x443ca8); main action, utility and the
running action sequence go through main+0x50b404 every frame (the last from inside the scheduler, main+0x44ff30);
the move-control graph through main+0x2beb80 from inside every action request for slot 0; a controller's +0x1f8 is
its actor for all of them; nothing else ran a node.

The game's own switches are **not** the switch: the stop timers stop the inlined runner only, not when an interrupt
is pending or the enemy is dying, and any active timer makes the life update throw the frame's damage away (V1, G1);
the pause mask for AI also stops the animation; the flag byte BSA+0x26ad bit 5 only skips a distance test (R1).

The mod's hooks are at function entries only. The switch (R1 section 9), for an actor that is a copy:

| # | Entry hook | Does | The actor is |
| --- | --- | --- | --- |
| H1 | main+0x1920, the node tick every runner ends in | returns | [[[x0+8]+8]+0x1f8] (node, instance, controller) |
| H2 | main+0x50b404 | returns | [x0+0x1f8] |
| H3 | main+0x42e0c8 | returns | [x0+0x1f8] |
| H4 | main+0x2beb80 | returns | [x0+0x1f8] |
| H5 | main+0x44ff30, the scheduler | returns | [x0+0xb70] |
| H6 | main+0x50ad74, the action request | returns 2 ("no slot") | [x0+0x18] |

H1 alone covers every runner, known or not, the inlined one included; H2 to H6 stop each known runner before it does
any bookkeeping, and say in the record which one was reached. The inlined body is also emptied by four writes before
the calc (root index [ctrl+0x19c] = -1, the instance's pending root [inst+0xde] = -1, its queue count [inst+0x90] =
0, its run bit [inst+0xd8] bit 0 clear; ctrl = [[BSA+0x2618]+0x18], inst = [[ctrl+0x20]]). Measured (run 5, one
game, the switch on for 30 s on an idle Bokoblin): H2 and H5 turned away the utility, the scheduler and the brain's
first-tick call, each once a frame, and with them gone no node was ticked or entered, no action was requested and the
inlined body ran nothing; H1 was never reached; the enemy stood in its idle animation, updated every frame. When the
switch went off every runner but the interrupt graph ran again within a second and the enemy went on. The interrupt
graph comes back when the queue count and the bit word the writes overwrote are put back (run 8: it ran every frame
again); setting its root index did not do it. So the writes save what they overwrite, for a hand-over (3.4). Still
to see: the same in a fight. (Two earlier runs with two games are void past their first
seconds: the old enemy sync, live between the games, took the test enemy away.)

A copy with no runner still has readers of what the runners kept fresh (R1 section 8). The mod holds, each frame: the
brain verb (requestBrainVerb main+0xad9844 with force, on the owner's change; the queued verb BSA+0x5c at 0), the
action sequence status BSA+0x64 at 0, the verb ids of action slots 0 and 1 as the owner's (the game's own
bookkeeping then gives the copy's controller the owner's physics flags), the slot status bytes at 0, the form key
string, target slots 0 and 8 (section 9.4), and no stop timer.

What stays on in a copy: the pre-part of the calc, the animation update (main+0x432fa0), the attack arming from
animation events (main+0x42ec98), the life update (with the frame's damage taken away, section 8), the death tick
(driven, section 9), the perception receiver update (so a copy has an emitter and can be seen by local actors), the
physics result and the model.

The switch is **proved by a counter, always on**: per copy and frame, the number of hook entries that turned a
runner away, by hook. H1 at 0 with H2 to H6 doing the work is the expected picture; anything else is in the session
record with the caller's address (section 13).

Global side effects that then happen only in the owner's game, as they should: reads of "the player" and of the camera
in 36 to 71 enemy graphs, the global no-attack time, messages between enemies, the battle-music request (G2).

## 5. Where a copy is: motion

5.1 **Pose-driven.** A copy's main body does not simulate. Its body is made static (main+0xae4300 with type 0) and
its pose is set before every calc (ForceSetMatrix main+0x45ce70) from the owner's states. Measured on a Bokoblin
moved along a circle for 30 s (run 7): actor and body were at the asked pose the next frame, every frame, to the
millimetre, upright on the floor in the pictures, and the enemy went on by itself when its body was made dynamic
again. A keyframed body (type 1) is wrong: the actor sank 3.3 m through the floor (run 6). The controller's forward direction, speed and move state are written too, so every local
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
while it is partial or full, the bones. The copy's own ragdoll is never started (its physics would fall differently):
the copy's bones are overwritten after the game recomputes the world matrices (main+0x283c750), the point with run
evidence from the stand-ins. The get-up is an ordinary animation command plus the blackboard angle the game's
recover module wrote (section 6.2). 78 enemy kinds have no ragdoll at all (R3).

5.7 **Animation-driven movement** is off on copies (41 enemies have it), since the pose already holds it.

## 6. What a copy does: animation

6.1 **The copy runs the game's animation update.** It turns commands into clips, raises the animation's events
(attack windows, sounds, weapon trails, footsteps) and arms the attack sensor from them (G5). The copy is therefore
told what to play, not which bones to hold.

6.2 **The owner states:**
- every play request, as made: every animation an enemy's behaviour starts, from a graph node or a native module,
  goes through main+0x4dc628 (or its two older forms), so a hook there on the owner's enemy gives the command name,
  slot, rate, blend, transition and the "leave an equal command alone" flag exactly (R2). Reliable, with the clock
  time. Slot removals and partial changes (main+0x2934934, main+0x29361b4 and its two siblings) likewise;
- per slot with every motion state: the current blend controller and, for each live one, its clock and weight (slot
  state S at G+0x20+i*0x48: rate S+0xd0, controllers *(S+0x50)+0x28.., current index +0x95, clock ctx+0x68, weight
  ctx+0x6c), read at the start of the owner's next frame, when all of the frame's behaviour has run;
- the animation blackboard, whole, on change: the table is small and its enemy writers are few (a turn angle from 12
  graph nodes and the ragdoll modules, a rider's gear, a stick value for two kinds; R2), but the writers of several
  keys are not found, so the table is not picked from (B = *(G+0x40); floats listed by vt+0x260 / vt+0x1d0);
- the actor's time rate (actor+0x280: hit-stop, slow motion); the look-at state (9.4); the model controller's three
  values (ice amount +0x4c, death effect +0x58, hide mask +0x9b of [[a+0x228]+0x280], each with the game's setter so
  its dirty bit is set).

6.3 **The copy applies**, before its calc, so the frame's animation update shows it: a play request by the same call
with the owner's arguments (the name by content), then the clocks and
weights. A copy runs the same delay behind as its position (5.3), so its picture is one moment of the owner's enemy,
not a pose from one time at a place from another. Phase is corrected softly: an error under 100 ms is closed by
running the copy's rate up to 10% off for a while; a larger one is written into the controller clock with the "do
not advance this update" flag (ctx+0xa0 bit 10, seen in main+0x433c24).

Measured on a Bokoblin whose graphs were off (run 8): a play request from the mod changed the command at once and
the clip ran from its start in that frame's update; the clock counts 30ths of a second (0.5 a frame at 60 frames a
second); a clock written to 0 with the flag read 0 after that frame's update and counted on from there; the
animation's hold events went on and off with the clip while no graph ran. What a clock jump does to an event it
jumps over is still to see.

6.4 **Randomness.** A slot holds one random value (S+0x1e4). A reading took it for a start delay (R4); measured, it
is a new number in [0, 1) every frame, and a clip started by the mod ran from its first frame with it at any value
(run 8). What the animation graph uses it for is not read; an enemy whose graph picks a clip by it would show in the
bench as a copy playing another variation than its owner, and gets the owner's pick stated then.

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
burn, shock: the owner states the condition slots' flags and the chemical state; ice is one model-controller value,
6.2; what draws fire and sparks, and the timers, are not read [run]). The hitter
sees it one round trip plus the delay later. If the measured time is too long for small hits, the hitter's game
plays the flinch at once from the same table the owner uses (the reaction key is a function of the entry, G7) and the
owner's stream corrects it; this is added only on that measurement.

8.3 **An enemy hits a player.** Each game decides for its own player, so the player's game sees the blow when its
picture shows it, and guard, parry and dodge are that game's own. A copy's attack sensor is armed in two ways,
because the game has two (R3): 148 enemy kinds arm from an animation event only, which the copy's own animation
update does; 34 arm some attacks and 17 all of them from a behaviour node (Lynel, Like Like, Wizzrobe, Hinox,
Mogurudo, the Chuchus, a boss, the Keese swarm), and for those the owner states each arming call (main+0x1bf20cc:
its parameters are data, equal in both games) and the copy makes it; 22 kinds attack by projectile or contact and
were not resolved [open]. In the owner's game the enemy's blow passes through the other player's stand-in without
damage, as today.

What the enemy learns of the outcome: its behaviour reads the guard type from its own life record (L+0xfc, L+0x104
bit 1) and its reaction (repelled, guard break) is chosen from a hit on its own life (R4). So the hit player's game
sends the owner the outcome (the enemy, the attack, guarded / parried / dodged / landed, the guard values, direction
and power), and the owner applies it by the route of 8.1. For the flurry rush an ordinary enemy marks its attack as
dodgeable with a bit a behaviour node sets (limiter client +0x34 bit 7); a copy never sets it, so the owner states
it. What reads it in the player's game, and what slows the enemy during the rush, are not found [run].

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

9.4 **Awareness.** A copy must look the way the owner's enemy is, to the local player and the local game:
- the look-at: the look arbiter's requests (activate main+0x1bae4ac, pause main+0x1bae5f0, interrupt main+0xc061cc)
  and its target, target slot 8, turned into the id of a player or a place;
- the battle target as a player id, written into the copy's target slot 0 so local readers of "its target" have one;
- Sneakstrike and lock-on: both are entries of the enemy's attention sender ([[a+0x228]+0x130]) that the player's
  game picks from, and each entry has a byte of reasons it is off (S+0x24+type). Behaviour graphs switch them (the
  silent-kill entries from the brain verb and the form key; lock-on off while dying), so on a copy they would all
  stay on: the stray Sneakstrike prompt of the old runs (R4). The owner states the 64 bytes on change;
- the marks over its head: their state is in a component at [[a+0x228]+0x140] and, probably, the receiver's notice
  level; the drawing code is not read [run: log both on an owner that notices a player];
- battle music: no behaviour node asks for it; a native manager does, probably from the brain verbs of enemies near
  the player, which a copy holds [run].

9.5 **What a behaviour graph asks of the player's game** reaches only the owner's player, since only the owner's
graphs run: camera shake and controller rumble (50 enemy kinds), the boss life gauge, its "prevent" requests and the
boss save point (213), a Wizzrobe's weather. The owner states these requests and the other game makes them. The
attack-permission manager and the global no-attack time are per game and stay the owner's; a copy asks for nothing.

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
another name puts that actor away and makes the stated one. Putting a placed actor away without the save's flag is
not solved: the placement update (main+0x65b710) creates whenever the placement has no actor and none of four bits
of job+0x98 is set, and it clears the suppress bit (bit 0) itself on each pass (R5) [run: which state holds].

11.3 **A game makes a copy** for every stated enemy within the range it shows enemies at [run: the range] when it
has no actor for the id: by adopting the actor its own placement made (same hash, same name), else by a create with
the stated name and pose. It lets a copy go when the enemy is beyond that range or its owner states it gone.

11.4 **Run-time spawns** proceed only where their creator is owned or there is none (G6): summons, split parts, nest
and army members are made by the owner's actor and stated; the same create on a copy does not happen (its graphs are
off) or is gated. The enemies the game places around a player by itself (night Stal, field Keese, pop-up Lizalfos)
are each game's own for its own player (the spawner places them around the position it tracks, the local player's
[inf]) and are stated like any other: the create is seen at the create request with the spawner's return address
(main+0x599670 called from 0x538ca4). The spawner itself refuses a second enemy of the same name and rule near that
position; two players side by side would still get one set each, so a game does not spawn (the request push
main+0xa80f28 returns) while a copy of the same name stands within that rule's radius of its player.

11.5 **Kill flags and the map.** The owner's kill writes the save's flag through the placement (main+0x195321c); that
call is stated, and a receiver makes the same call on its placement, which sets the flag and the suppress bit (the
flag alone would bring the enemy back at the next load, G3). The 165 enemy flags the shared game data leaves personal
today (reset mask 80) join the shared set.

11.6 **Blood moon.** The reset writes the flags straight into the table inside the game-data update, where the
mod's capture does not see it. What decides that tonight is the night is not read, and need not be: the moon
starts through one call (main+0x21cd650 on the world object) and ends through another (main+0x21cd698). The first
game to enter the start states it; the others make the same call, and each game's own sequence then resets its
table. The moon the game forces when memory runs short is one bit (world +0x60 bit 19) and is each game's own.

## 12. Enemies that are not a Bokoblin

All detailed readings were made on one Bokoblin. The census (R3) covers the 306 enemy actor kinds: 221 have behaviour
graphs, 85 do not (army managers, the six dragons, weak points, limbs and projectiles, dummies). Since a copy runs no
graph, the differences between families are in what the owner must state, in this order of distance from the
Bokoblin:

| Family | Beyond this file |
| --- | --- |
| Bokoblin relatives: all tiers, Moblin, Horriblin, Lizalfos, Yiga, Constructs, Gibdo (most actors; the graph shape is shared) | swimming and ceiling states; riders |
| Lynel | arming events (8.3); root-motion movement off (5.7); its rider |
| riders and mounts (40 riders, 23 ridable) | the pair is one pack; mount and dismount events; the rider stated relative to the mount (5.5) |
| flyers and swimmers | nothing new for motion (controller bodies in the free state); water entry events |
| odd bodies: Stal heads and the Stalnox eye (rigid bodies, no controller), Chuchu, Like Like, Mogurudo, Octorok | pose-drive of a rigid body; arming events; the swallow as a stated attachment to a player |
| swarms and summoners: Keese swarm, Wizzrobe, Treant, Gibdo nests | children with minted ids (11.4); the swarm's members |
| multi-part giants and constructs: Talus, Hinox, Stalnox, Frox, Flux Construct, Battle Talus | parts as children (Talus arms are shot from the body, attention helpers and the eye are held children); how a part's damage reaches the body is not read [open]; the second utility graph is on a copy's switch already |
| dungeon bosses and the dragons | scripted events, the camera their graphs read, children; the dragons have no graph and are driven by a component not read [open] |

Each family gets a section in the specification and a bench case. Scale: at most 15 placed enemies within 50 m and
51 within 200 m on the surface, one cluster of 61 in the depths, 44 in the castle; the tables are sized for 64 synced
enemies in range, and run-time spawns are counted in a run.

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

By reading, still open: the brain's per-frame runner (run 1 will name it); how the 22 unresolved enemy kinds attack;
how a part's damage reaches its body, and the parts of Frox, Flux Construct, Gohma, Mucktorok, Queen Gibdo and Gloom
Hands; the dragons' component; the camp-cleared record and its chest; the code that draws the marks, the health bar
and starts battle music; the enemy side of the flurry rush; fire and shock visuals and condition timers; the update
and load ranges in metres; whether a boss that reads the camera needs the other player's camera stated to its owner.
None of these changes the structure; each is read when its section of the specification is written.

By a run (each needs the tester's yes; runs 1 to 5 only log, on one placed camp; nothing is synced by them):
1. an untouched Bokoblin: every entry of the four runner bodies and of the node tick with its controller and caller,
   against the unit pointers of its graphs: names every runner, the brain's included, and the inlined graph;
2. the same with the switch on for that Bokoblin in one game: the counts are 0, the animation update still raises
   its events, nothing reads a stale field into a fault; node enters counted;
3. a static main body under ForceSetMatrix on an enemy, and the controller's readers afterwards;
4. a command played by the mod with the clock written: does the phase hold, do events fire once;
5. two players before one camp for a minute: the Sight list order, target slot 0, the attention bytes, the notice
   component, the animation blackboard, the limiter bit, per frame;
6. the first motion bench: the measures of section 7 on walking, running, a jump attack and a knock-back;
7. placements: which state keeps a placed actor away without the save's flag (11.2).

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

## 16. What the specification's review and runs 9 to 12 changed (2026-11-15)

Where a section above says otherwise, this section holds.

- **Ownership is per enemy, not per pack** (3.1). A camp member can exist in one game only, and a rule that hands a
  whole camp to one owner then moves it back and forth with some member simulated nowhere. A game owns only what it
  runs; a camp may be split between owners; what an enemy holds and makes still follows it.
- **Claiming** (3.2): a new actor is held unthinking from its first update, claimed and declared at once, and
  released when no greater claim came for a short time; an owner that already fought an enemy keeps it against a
  newcomer's first claim.
- **A paused owner** (3.3) is claimed from after 1 s only by a game whose player is in that fight, else after 5 s,
  and the enemy is given back when the owner runs again.
- **Identity over time** (11.1): the life number is 1 + the blood moons of the session, not a count of actors seen.
  Every game keeps a tombstone for each placed enemy that died and a resting record for each that was let go.
- **Placed actors** (11.2, 11.3): the mod never creates or deletes the actor of a placed enemy; a copy is the actor
  the game's own placement made. A placement whose two saves give different actor names is unsynced until a run
  shows it can be replaced.
- **Kills** (11.5) come with death, not later: at a copy's death end the placement's own dead call is made on the
  copy's actor before it goes.
- **Hits** (8.1): taken out of a copy and appended at the owner inside the damage step (main+0x300a30), released the
  game's way, once per swing, only by the game whose own attack it was; a hit that reaches a game that no longer
  owns the enemy is sent on.
- **The flinch** (8.2, 5.6): measured in a play test, a hit makes an enemy flinch with every graph off and no
  animation started. It is physics on the bones. The owner sends bones for a short time after a hit; in the hitter's
  own game the copy may flinch by itself.
- **Death at a copy** (9.2): measured, a switched-off enemy whose life reaches 0 waits at the death's third phase.
  The owner states the committed causes read from the death state; the mod gives the copy the signal the graph
  would have given.
- **The switch** (4): measured in melee, while hit and while dying, on placed enemies: only the scheduler, the
  utility and the brain's first-tick call are ever reached, and turned away. No field of the BSA is held beyond the
  four writes. An enemy released from the switch goes on with its fight.
- **Motion** (5.2, 5.3): states at 30 a second with counters for teleports and landings; the time stamp is the frame
  the values describe; the clock offset is a filtered minimum that never jumps forward.
- **A copy's attack** (8.3) may hurt only this game's player and what is this game's; the test is made at the
  victim.
- **The target** (9.4, 2): no run has shown an enemy fighting a stand-in; the rule is written after one has.
- **The bench** (7, 13) runs every case with a simulated network as well, and pairs the two games by the PC's
  clock, not by the copy's own estimate of the owner's.
- **Groundwork first**: the message transport had one reader, the old enemy sync has no off switch and shares code
  with the mount sync and the object mirror, and the object mirror cannot be told of an object. These come before
  anything of the sync.

Added after the specification's second review (same day):

- **"Runs it"** is the test for every claim: the actor is here, its update ran, and this game's own far gate for it
  is open. Without it two players passing a camp at a distance hand each enemy back and forth many times a second.
- **No giving back** after a pause and **no ownership by request**: a fight claimed from a paused owner stays where
  it went.
- **A dying enemy** changes owner only when its owner is gone, and the new owner ends the death; the kill is
  recorded at the death's start, as the game does it.
- **The flinch** is read: the damage step asks the ragdoll component for a brief partial ragdoll. The owner states
  that request and each copy's own physics plays it; the hitter's game makes it at the blow. Bones travel only for a
  full ragdoll and a death.
- **A switched-off enemy with a moving body walks on** with its last speed (measured in the play test: 116 m in 27
  s). A held or copied enemy has its speed taken away at once.
- **Time** is the PC's performance counter, read once a frame, not the game's world clock, which loses time at a
  hitch.
- **An enemy that one game has no actor for** (dead in that save, another tier, its placement not loaded there) is
  the ordinary difference between two saves. Two runs decide what is done about it before any copy is built, and the
  record counts such enemies in every session.
- **Captures and the per-frame hand-off** to an enemy's own update, the map of actors, the message dispatcher and
  the hooks still to register are specified as the code needs them (specification sections 1 and 16).
