# Horse and animal sync: the specification

Status, 2026-11-17: second draft; built as section 0 says, not run yet (no game may be started until the tester's
other play session ends). Horses and animals are synced by the enemy sync's method
([enemy-sync-spec.md](enemy-sync-spec.md), "the enemy spec" below, cited as E plus a section). This file says only
what differs from that method or is added to it. The older horse and animal code (`coop_mounts.c`, removed with the
older enemy sync in E B.30) chose who runs a horse and where a rider sits but never mounted a horse in a run; what
it got right is kept (the kinds, "a ridden horse is its rider's game's"), what it guessed is replaced (riding is read
from the game, not from a box above the horse).

Research (outside the repository, derived from game data): `totkCoop-research\horses\` H1 (census of the 205 horse,
animal and tack actors), H2 (the horse's side: driving, its AI, the save, tack), H3 (riders: the seat, Link, the
stand-in), H4 (animals; the movement and animation of four-legged actors). Marks as in E: [m] measured in a run,
[inf] an inference of a reading, [open] not read with the rule that holds until it is, [run] to be settled by a named
run of section 10. No mark: read in the game's code or data.

## 0. As built (2026-11-17)

`runtime/coop_mount.c`, on with `TOTK_COOP_MOUNTS=1` (off by default). Built and compiled, **not run**. Where the build
differs from the first draft, the sections below are written as built:
- What a horse or an animal states besides its STATE (the gait values, the tack, its rider and the rider's place, and
  for an enemy what it rides) travels in its own message, MOUNT (type 66, unreliable, the latest per entry), sent with
  the entry's STATE. An enemy's STATE does not grow; an enemy that rides nothing sends no MOUNT.
- The tack is given to a copy by the game's own tack call (main+0xc48ac0, which the horse's tack node calls), not by
  the weapon create; `EN_EQ_N` stays 8.
- A wild herd is held back at the herd node itself (main+0xd7ff68, a new entry hook), not at the create request.
- Not built: the copy of a registered horse made with its owner's coat (4.5); the wagon (9).

## 1. Reuse, not a second machine

Horses and animals become **kinds inside the enemy sync's tables** (E 3 to 11 unchanged: the entry, identity,
ownership, CLAIM/OFFER/GONE, the hold, the copy switch, STATE, the clock, hits, death, loot, tombstones). Reasons:
- What makes an enemy a copy is its BSA's graph runners, its life and death components and its character
  controller. Every horse and animal admitted here has all of them (H1: BSA, life, CCT; the horse's main body is a
  CCT with preset `Horse`, its HorseBody rigid bodies are bound to bones).
- The copy switch holds a horse too. A horse's scheduler is its own graph (`GameRomHorse.scheduler.root`, no enemy
  uses a custom scheduler), but it runs inside the scheduler entry the switch already turns away: main+0x44ff30
  calls main+0x2beb80 with that scheduler when the flag at +0xa8 of its state is set, and the switch's hook returns
  at main+0x44ff30's entry (and main+0x2beb80 is hooked too, E 5.1). The switch's proof counters (E 5.5) still run.
- A second copy of ownership, the clock and the death path would be a second set of faults.

What is new lives in one co-op file, `runtime/coop_mount.c`, with one-line calls from `coop_enemy*.c`:
1. the kinds (2); 2. riding (5, 6); 3. the gait values (7); 4. the run-time horses and animals (4.2 to 4.4).
The setting `TOTK_COOP_MOUNTS=1` admits the kinds; off (the default until section 10's runs pass) nothing here acts
and today's behaviour is unchanged.

## 2. What is synced

By actor name, from the census (H1 section 1, its TSVs):

| Kind | Names | Why |
| --- | --- | --- |
| horse | `GameRomHorse*` but `GameRomHorseSaddle_*`, `GameRomHorseReins_*` | 37 actors with brain, life, CCT and one Ridable seat (H1). Epona, the Lord of the Mountain (`Nushi`), the giant horses (`00L`, `01L`), the skeleton horses (`Bone`), the golden and patterned ones. |
| animal | `Animal_*` with a BSA and a life, but the left-out list below | the land animals: bear, boar, deer, doe, elk, wolf, fox, dog, goat, wild goat, sheep, cow, bull, cassowary, kokko, squirrel, rupee rabbit, sand seals, Pao. |

Not synced, each game keeps its own:
- fish (30 names), insects (28): caught as items into the pouch, many, no fight; the object mirror does not take them.
- birds (`LittleBird`, `SkyLittleBird`, `Crow`, `Gull`, `SkyGull`, `Pigeon`, `SkyPigeon`, `Hawk`, `Heron`, `WildDuck`,
  `Ptarmigan`) and every `*_Swarm`: a swarm is one actor with copied models and no BSA (H1).
- the scenery horses `Animal_Horse_*` (8 names, 43 placed): System actors with no BSA and no life, an event graph only.
- the event donkeys (`Animal_Donkey_*`, 27 placed): like the scenery horses, no BSA and no life; they move only when an
  event says so (H1). The ridable donkey is a horse (`GameRomHorse01S`, seat type Donkey) and is synced as one.
- tack: saddles, reins (and `Horse_Link_Mane*`): System actors bound bone to bone to their horse
  (`SameBoneModelBind`); each game's copy of a horse carries its own (4.5). `prop_skip` keeps them out of the object
  mirror already.

Animals and the stand-in: an animal notices only the Link of the game that runs it (H4 section A1: the stand-in is
not noticed by default). The hand-over of E A (35 m, 6 m nearer) therefore gives an animal to the game whose Link
is near it, and it flees from that Link. Two players at the same distance: it flees from one of them; accepted.

## 3. Identity

E 3 unchanged for placed actors (the placement hash and life number). Placed: 22 horses with a brain and about 840
land animals (H1 section 4: deer 206, wolf and fox 192, goat, sheep and cow 150, rabbits 104, squirrels 81, boars 49,
kokkos 43, bears 17). Everything else is made at run time and is named by the spawn sync's key (E A, "Run-time
enemies"), which today announces only `Enemy_` names (`coop_enemy_spawn_synced`): with the setting on it also
announces the kinds of section 2. Section 4 says which game may make each.

## 4. Who makes it, who owns it

4.1 **Placed** (E 4 unchanged): the game that runs it claims it; the hand-over moves it to the nearer player's game;
a far one rests.

4.2 **Area-made animals** (H1 section 6, H4 section A4: `AutoPlacementAnimal`, `...Bear`, `...Boar`, `...Deer`,
`...Fox`, `...Wolf` make bear, boar, deer, doe, elk, wolf, fox, goat, cassowary and squirrel near each game's own
Link). Each game makes its own; the one that made it owns it and the spawn sync makes it in the others as a copy. A
game's own area placement of a name is turned away while another game's live actor of that name stands within 60 m
of its Link (E B.23, the push main+0xa80f28, which compares the enemy table by name and so covers these kinds once
they are in it). [open: whether the animal sequences call main+0xa80f28 like the enemy ones; rule until read: the
same, and the record counts the animals of the kinds of section 2 made within 60 m of another game's live one, so a
double is seen.]

4.3 **Wild herds** (H2 section 3.2, H1 section 5): not placed. A `WildHorseCreateTag*` actor (9 kinds, 77 placed:
50 on the surface, 27 in the depths) makes 2 to 4 horses in one call (ExecuteWildHorseCreate, main+0xd7ff68), drawn
from a name list by the game's global random generator, so two games would draw different herds. Rule: the herd
node's update (main+0xd7ff68; x0 the node, its tag actor at [[[x0+8]+8]+0x1f8]; it returns true once the whole herd is
made) is not run in a game while another player's Link is nearer the tag than this game's (within 400 m), or another
game's live horse made at run time stands within 200 m of the tag (the tag's own radius, H1); it is answered true, so
the node does not ask again in that load. So the game of the player nearest the tag makes the herd, owns its horses,
and the spawn sync makes them in the others. Two games that load the tag in the same moment with their players at
the same distance can still make two herds; the record says so (`coop: mount: herd:`).

4.4 **The players' own horses** (H2 section 3). A registered horse is not placed. At a load each game's owned-horse
manager (SeqOneShotInitializeOwnedHorseMgr, main+0x981330 and main+0x981ac8) makes **its player's** last-ridden horse
by name, with overrides from that player's save (the saved entry's `UidHash`, colour, mane, saddle, reins); the
stable's "take out" makes one the same way. So two games make two different horses, one per save, and each is the
horse of the game that made it: it is owned by that game, announced by the spawn sync, and made in the other game as
a copy. The whistle moves the existing horse [inf, H2: HorseCall plays and the called horse walks to the player].
Nothing of a horse's save record crosses games: the owned-horse list, the dead-horse list, `Horse_IsRide` and
`Horse_Is_LastRidden_Owned` stay personal (`coop_gamedata.c` lists them already), and a copy's graphs, which write
those (ExecuteHorseWriteGameDataForUI, OneShotOwnedHorseRegistAsLastRiddenAnimal), do not run.
- "Two players want one horse": anyone may ride any horse (6). Riding another player's registered horse does not
  register it in the rider's save: the registration nodes run in the owner's game only, where the rider is a
  stand-in. Accepted and said in the notes for players.

4.5 **What a copy of a horse looks like.** A copy made by the spawn sync is made by name and has no tack and the
default coat (H2 section 4).
- **The tack** is in slots 0xf (mane), 0x10 (reins) and 0x11 (saddle) of the horse's equipment user (H2 section 4:
  the node OneShotHorseEquipHorseTack calls main+0x1b9cec8, 0x1b9ce60 and 0x1b9ce10, which call main+0xc48ac0(equipment
  user, slot, &name, -1, 0, 1); the name object is a pointer and a capacity, as the armour create's). MOUNT carries the
  name hash of what each holds; a copy's slot that is empty where its owner's holds something is filled by the same
  call, once a second, three tries a slot. Every tack name of the census is known to every game, so a name is never
  missing. [run: that the call makes and binds the tack on a copy.]
- **The coat** (the create's overrides `UidHash`, `OverwriteColorType`, `OverwriteFootType`,
  `Horse_OverwriteFamiliarity`): not built. A copy of a registered horse shows its kind's default coat [open: the
  overrides' layout in the create request, E 10.3's "init values as (hash, value) pairs"].
- The wagon: a child actor of `GameRomHorseSaddle_07_WithWagon` (ActorHolder); not synced in this draft.

4.6 **The ridden horse is its rider's game's** (H2 section 1, H3 section 2). The rider's input goes into the
rider's own Rider component and the horse's graph reads it through its seat in the same game (ExecutePlayerRide
writes Rider +0xf8 to +0x168; ExecuteHorseRiddenByPlayer reads them, H2 1.1). So a horse is run by the game whose
actor sits in its seat. Rules, added to E 4.4 and 4.5:
- **this Link mounts a copy**: the ride call main+0x502e60 for this Link's Rider component on a copy horse is let
  through when the owner's last MOUNT names no rider, and at the next phase 0 this game claims the horse (E 4.4's
  take, epoch + 1, no hold). Mounting takes about a second of animation in the game that mounts; the old owner's
  game gets the CLAIM within a round trip and its horse becomes a copy before the rider sits [inf].
- **a copy whose owner states a rider** refuses the ride call here (returns 0 before any write, as `coop_ent_id.c
  coop_ent_seat_hook` refuses a stick): two players cannot sit in one seat.
- **two mounts in the same round trip**: E 4.0 settles the pair; the game whose claim lost finds its Link seated on a
  copy whose owner states another rider and puts it off with the game's own get-off (main+0xc448c8(rider, 1, 1), as
  `coop_ent_own.c` ends a ride). Counted; the bench (section 10) makes it happen once.
- **no hand-over while ridden**: E A's offer to a nearer player is not made while the horse's seat holds this
  game's Link or an enemy this game runs. A ridden horse changes owner only when its owner's game leaves or loads
  (E 4.4); E 4.4's PAUSED take-over does not apply to it: a player in a menu is still sitting on the horse, which
  stands in both games until the menu closes (a horse has no fight to keep going, and a take-over would put the
  rider off).
- **a dismount** lets the horse stay with the game that ran it; the hand-over then applies as for any horse.
- **the rider's game leaves**: E 4.4; the horse rests or is taken, riderless.
- **an enemy rider** and its horse are one unit: the horse is owned by the game that runs the rider. A claim of the
  rider claims the horse in the same CLAIM message, and the horse is not offered on its own while ridden.

4.7 **Far from every player**: E 4.1's gate and the resting record, as for an enemy. A registered horse that rests
stays where it stood in each game until a player comes near; the owned-horse manager does not make it again while
its actor lives [inf].

## 5. The rider: a player

Link rides through the Rider component, as an enemy does (H3 section 2): the player's ride is main+0x502e60(rider,
horse handle, type 1, seat 0) (seen in main+0xd3c160, which re-seats a saved rider at a load), and `Horse_IsRide`
is worked out each frame from the Rider component by main+0x321e8c. So "this Link rides horse H" is: Rider
([comps+0x410]) +0x38 is 1 and its mount link +0x20 names H. No box test.

5.1 **Stated with the horse.** The horse's owner is the rider's game (4.6), so the horse's MOUNT carries its rider
at the same frame stamp as its pose: `rider` (0 none, 1 + net id a player's Link, 0xff an enemy) and the rider's
place in the horse's frame (`roff[3]`, from the two actors' poses at the read). NetState's ride fields stay for
contraptions; `COOP_RIDE_MOUNT` is not sent.

5.2 **Shown in another game.** The other player's stand-in (a PlayerPuppet: it has no Rider component, H3 section
4) is not seated by the game's ride call. Its place is the copy horse's shown pose times `roff`, from
`coop_players.c owner_matrix` through one call (`coop_mount_place(player, out)`), the horse's pose being the one
pre-calc gives the copy in the same frame; its body pose is the rider's own bones (POSE packet, written after
main+0x283c750), which already show the riding pose. The stand-in's own physics is left as for a contraption rider
(`ride_carry`'s rules: it is moved with the horse, not by its own step).
Bar: the stand-in's saddle point within 3 cm of the copy horse's `Saddle_Root` in every frame (they are one
computation: a larger error is a fault of order, not of the network).

## 6. The rider: an enemy

A Bokoblin on a horse (H3 section 1, R20): the ride writes the mount link and a seat into the rider's ModelBind
component ([comps+0x18]: +0x70 the parent link, +0x88 the seat's bone name, +0x40 a local matrix, +0x1a4 bit 0);
the engine places the rider from the horse's bone each frame [inf: the applying function is not found].
- The rider's MOUNT carries `ride` = (the horse's Enid, seat, type), from its Rider +0x20, +0x3c, +0x44 when +0x38
  is 1.
- A copy rider is seated by main+0x502e60(its Rider, its game's copy of that horse, type, seat) on the edge "rides
  nothing -> rides H", and put off by main+0xc448c8(Rider, 0, &0) on the other edge or when H changes (get off
  first: the ride refuses a busy rider). Never every frame (R20 1.5).
- **While a copy rider is seated, pre-calc does not write its pose** (no ForceSetMatrix, no static request): the
  bind places it from the copy horse, which is posed by this sync. [run: the rider model's place against the copy
  horse's `Saddle_Root`, with and without the write, H3 section 7; and the ride call's return on a copy.]
- The ride's preconditions hold no AI state (H3 section 2: the horse's flags +0x28 & 0xa0000 and +0x444 & 3, the
  rider's +0x1c8 > +0x1d0, a free seat), so a copy horse with a free seat takes a copy rider [inf].
- A rider whose horse has no copy here (not made, not yet bound) waits unseated and is posed as an enemy is; it is
  seated when the horse binds.

## 7. Movement and animation

A quadruped's gait is not only its command. The command (`Move_Gear1..4`, `Move_Normal_*`, `Move_Escape_*`) plays
through the animation player as an enemy's does (E A), but the clip within it is chosen by **blackboard values of
the animation sequence** (ASBB) that the AI writes each frame: main+0xab9d90 (the quadruped's gait update, called
from AI Execute nodes) ends with three writes through the player's setter (vtable +0x70) at the ids it keeps at
+0x54, +0x58 and +0x5c of its state. A copy's AI does not run, so its values stay at their defaults: the copy
would play straight, flat clips (H4 section B6).

7.1 **MOUNT carries the gait values.** Per kind a list of names, from the ASB blackboard tables (H4 B6.2):
- horse: `RequestGear`, `StickX`, `StickAngle`, `DirectionAngle`, `UserSpeed`, `GroundNormal`, `GroundNormalSide`,
  `ForwardBent`, `PullAnmRate`, `Variation`;
- deer, bear, wolf and the others: `RequestGear`, `StickX`, `DirectionAngle`, `GroundNormal`, and `Distance` where the
  table has it.
The owner reads them at phase 0 by the getter (`vt[+0x170](B, &name)`, G5 section 4); the copy writes each that
differs by the setter by id (`vt[+0x70](value, B, id)`, ids looked up once per copy by `vt[+0x290]`) in pre-calc
after the animation is asked. Up to 12 floats (48 bytes); the owner reads them when a state is due. Each name's id
is looked up once per actor; a name its table lacks (id -1) is not read or written. This is generic: an enemy whose ASB has such
keys (a Lynel, a Hinox's `StickX`) gets them by the same table. [open: which key selects the gear, the command or
`RequestGear` (H4 b1); both are carried, so either way the copy has it.]

7.2 **Root motion and slope fitting.** The pose written each frame overrides the root slot's motion (E 6.5). The
slope fitting (FittingModelToSlope) is a component that casts its own rays and is left to run on the copy: the same
ground gives the same normal.

7.3 **The bar** (H4 B7: the enemy bars scaled by speed; the enemy bench measured 8 cm at about 8 m/s, which is 10
ms of timing error). At the 95th percentile, two games, test network at 1 ms:

| Bar | Standing | Walk (1.3-2.4 m/s) | Trot (4-8) | Canter (8-15) | Gallop (13-21) |
| --- | --- | --- | --- | --- | --- |
| position error | 2 cm | 4 cm | 8 cm | 12 cm | 17 cm (21 for Epona and the fast horses) |
| in a turn | 2 cm | 6 cm | 12 cm | 18 cm | 26 cm |
| same command | 99 % | 97 % | 95 % | 95 % | 95 % |
| same gait (command and `RequestGear`) | | 97 % | 95 % | 95 % | 95 % |
| phase error of the gait loop (24 frames, 0.8 s) | 33 ms | 33 ms | 33 ms | 33 ms | 33 ms |
| hard clock corrections | 0 | under 1 a minute | under 1 | under 2 | under 2 |
| frames not at the pose asked; snaps not flagged | 0; 0 | 0; 0 | 0; 0 | 0; 0 | 0; 0 |

A ridden horse's stand-in rider: 3 cm from the copy horse's saddle point in every frame (5.2). An enemy rider: its
model within 3 cm of the copy horse's saddle point (6). A number here changes only by decision, written with the
run's date.

## 8. Life, death, drops

E 8 and 9 unchanged. A hunted animal dies once (the owner asks the death; a copy makes no drops, E 5.4); its drops
(meat; a rupee rabbit's rupees, H4 A2) are the owner's game's and reach the other game through the object mirror
(E B.9). The compendium and the defeated counters are each player's. A horse's death: its owner's game runs it; the
dead-horse list is the save's of the game that registered it (4.4). A fleeing animal that deletes itself far away
(OneShotGenericDeleteThisActor, H4 A1) is no death: the owner's GONE lets it rest (placed) or the spawn sync deletes
its copies (run-time).

## 9. Not done in this draft

- The wagon and its coupling (`GameRomHorseSaddle_07_WithWagon`, `_ExternalCoupler`, the street vendor's horse).
- Taming a wild horse and registering it at a stable as the second player: the taming runs in the game that runs
  the horse; registration is that player's save. Not looked at beyond that.
- The Lord of the Mountain's own event graph; the horse god's revival.
- Riding a bear, a deer, a sand seal or a Lynel: the same rules (their seat is a Ridable), not run.
- A horse's spur count and stamina on a copy (H2 b: the field is not found); a copy shows the gallop, not the spurs'
  flash.

## 10. Runs (each short, one thing, pictures from both games)

Bench pattern: `tools/coop/run_pair.ps1`, `en_bench.ps1` and `en_report.py`; `tools/coop/spawn.ps1 <Actor>` makes
an actor beside game 1's Link in a `play_two.ps1` session. Every case on the build with `TOTK_COOP_MOUNTS=1`.
1. `horse-stand` (2 min): spawn `GameRomHorse01` beside game 1's Link; bound, followed, tables equal, no leak, the
   switch's counters show the custom scheduler turned away.
2. `animal-stand` (2 min): spawn a deer, a boar, a wolf, a bear one after the other; the same, and the gait values
   stated and written.
3. `herd-flee` (2 min): game 1's Link walks into a placed deer herd; they flee in both games; bars of 7.3.
4. `ride` (3 min): the tester mounts a horse in game 1 and rides walk, trot, gallop, turns, stops; game 2 shows the
   stand-in seated on its copy; bars of 7.3 and 5.2.
5. `ride-claim` (2 min): game 2's Link mounts a horse game 1 owns; the claim moves it; then both mount at once.
6. `enemy-rider` (2 min): spawn a mounted Bokoblin; the copy rider seated by the ride call; its model on the saddle.
7. `hunt` (2 min): game 2's player kills a boar that game 1 runs: one death, one meat, in one place.
8. `owned` (3 min): both games load saves with a registered horse; each sees the other's, with its tack.
A case whose event did not happen is NOT JUDGED (E B.26). Then a build for the tester to ride in.
