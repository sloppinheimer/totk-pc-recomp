# Horses and animals: game facts

Horses, land animals and riding in TotK 1.4.3. Conventions and marks as in [README.md](README.md): **read**,
**measured** (with date), **inferred**, **open**. Sources: horse-spec (docs/coop/horse-animal-sync-spec.md) and
research horses/H1 to H4 (outside this repository). Nothing here was measured in a run: every horse fact is a reading.
A player as a rider: [player.md](player.md). The AI framework these actors share with enemies: [enemies.md](enemies.md).

## 1. Census (was: horse-spec 2)

205 names in the RomFS (**read**, research horses/H1): 130 `Animal_*` (9 scenery horses `Animal_Horse_*`, 30 fish, 28
insects, birds and 4 swarms, and the land animals), 46 `GameRomHorse*` (30 horses, 8 reins, 15 saddles including
`_WithWagon` and `_ExternalCoupler`), 19 `Horse_Link_Mane*`.

| Group | Names | Brain, life, controller, seat |
| --- | --- | --- |
| Horses | `GameRomHorse*` but saddles and reins: Epona, the Lord of the Mountain (`Nushi`), the giant horses (`00L`, `01L`), skeleton horses (`Bone`), golden, Zelda's, patterned, the street vendor's | BSA, life, character controller (preset `Horse`), one Ridable seat; 37 actors |
| The ridable donkey | `GameRomHorse01S` | as a horse; seat type Donkey |
| Land animals | bear, boar, deer, doe, elk, wolf, fox, dog, goat, wild goat, sheep, cow, bull, cassowary, kokko, squirrel, rupee rabbit, sand seals, Pao | BSA, life, controller |
| Scenery horses | `Animal_Horse_*` (8 names, 43 placed) | System actors, no BSA, no life; an event graph only |
| Event donkeys | `Animal_Donkey_*` (27 placed) | no BSA, no life; move only when an event says so |
| Birds and swarms | `LittleBird`, `Crow`, `Gull`, `Pigeon`, `Hawk`, `Heron`, `WildDuck`, `Ptarmigan` and Sky forms; every `*_Swarm` | a swarm is one actor with copied models and no BSA |
| Fish, insects | 30 and 28 names | caught as items |
| Tack | saddles, reins, `Horse_Link_Mane*` | System actors bound bone to bone to the horse (`SameBoneModelBind`) |

Placed: 22 horses with a brain and about 840 land animals (deer 206, wolf and fox 192, goat, sheep and cow 150, rabbits
104, squirrels 81, boars 49, kokkos 43, bears 17). **read**

## 2. A horse's AI and the copy switch

- Every `GameRomHorse*` shares one pack layout and AI. Its scheduler is its own graph, `GameRomHorse.scheduler.root`
  (SchedulerParam `GameRomHorseTmp`, UseCustomScheduler); no enemy uses a custom scheduler. It runs inside the scheduler
  entry main+0x44ff30, which calls main+0x2beb80 with that scheduler when the flag at +0xa8 of its state is set. So the
  hooks that turn an enemy's runners away (enemies.md 2.6) cover a horse too. **read** (inferred that the switch holds;
  not run)
- The scheduler root's children: not ridden, ridden, return to a safe place (verb 280), swim (97), freeze (168),
  paralyse (169), light chemical (172), fall, die (159). Not ridden: wait (verb 40), wander, eat, follow food, move to
  the player, escape, kick back, damage reaction. **read** (research horses/H2 2.1)
- The brain root is `ExecuteHorseUpdateTarget` only. The utility emits noise and terror (other horses receive terror of
  type Horse). **read**
- Wander: leader, wander, follow the leader, follow a rail (`RailPtr`). Escape chooses by nature (Gentle, Impatient by
  default). Come: follows target slot 0; on failure the "cannot come" UI. Follow food: slot 9, a player carrying a
  favourite food (find radius 20 m). **read**
- What a horse's AI does to the world: only its own body and controller, perception noise and terror, a kick (sensor
  attack), chemical and life reactions, and game-data writes for the UI (familiarity:
  ExecuteHorseWriteGameDataForUI; OneShotOwnedHorseRegistAsLastRiddenAnimal). No node writes another actor's fields.
  **read**

## 3. Riding (was: horse-spec 4.6, 5, 6)

- The Rider component [comps+0x410]: mount link +0x20, riding 0 or 1 +0x38 (setter main+0x503c74, fires event
  0xc33c17bd), seat +0x3c, type +0x44 (1, 2, 3), seat matrix +0x48. The Ridable side [comps+0x408]: seats at +0x28,
  count +0x20. **read** (research enemy/gaps/R20)
- **Ride**: main+0x502e60(rider, mount handle, type, seat), synchronous; it refuses a busy rider. Its preconditions
  hold no AI state: the horse's flags +0x28 & 0xa0000 and +0x444 & 3, the rider's +0x1c8 > +0x1d0, a free seat.
  **Get off**: main+0xc448c8(rider, ...). The player's ride is type 1, seat 0 (seen in main+0xd3c160, which re-seats a
  saved rider at a load). **read**
- "This Link rides horse H" is: Rider +0x38 is 1 and +0x20 names H. `Horse_IsRide` is worked out each frame from the
  Rider component by main+0x321e8c. **read** Corrects proxy-plan "horses and animals", which told a riding Link by a box
  above the horse and said no riding flag was known (C27, resolved).
- **A ridden horse is driven from its rider's game.** The player's ExecutePlayerRide node (`Player.action.ride.module`)
  turns the pad into stick axes and button flags in the rider's Rider component (+0xf8..+0x168); the horse's
  ExecuteHorseRiddenByPlayer node reads them through its seat (main+0x1beb4b8 -> main+0x4dacf4 -> main+0x45a0ac). The
  horse holds no pad state. **read** (research horses/H2 1.1)
- The ride also writes the mount link and a seat into the rider's ModelBind component ([comps+0x18]: +0x70 the parent
  link, +0x88 the seat's bone name, +0x40 a local matrix, +0x1a4 bit 0). The engine places the rider from the horse's
  bone each frame; the function that applies it was not found. **read**; the placement **inferred**.
- An actor made from the Player's pack without a Rider component (the stand-in) cannot be seated by the ride call.
  **read** (research horses/H3 4)
- Riders and ridables among enemies: 40 to 45 riders (Bokoblin tiers, Zonai constructs, Yiga, Kohga) with RidableType
  Horse, Bear, Deer, LargeHorse, Donkey or WagonSeat; 23 ridable enemies (Lynel, Like Like, Frox, Kohga's construct, the
  Gloom tentacle). Enemy riders use action sequences `HorseRide_*` and `RideSeat_*`, form keys, and
  `ExecuteRideHorseASBlackBoardGear` (the rider's Gear from its mount). **read**
- Seats (`RidableRef`): bear, deer and horses at bone `Saddle_Root` (horses: Main or Sub); the giant horse Sub; the
  donkey at `Root`; sand seals at `Pod` with towing hooks (the seal's graph computes the towing velocity). Not
  ridable: elk, boar, wolf, fox, goat, sheep, cow, bull, cassowary, Pao, dog, kokko, rupee rabbit, squirrel. **read**
- A horse's gear lives in the Quadruped component [comps+0x3e0] (accessor main+0x1952c68): q+0x24 the current gear,
  q+0x20 a second gear word, q+0x2c a forced gear (5 none). The live spur count is **open**; spurs per horse:
  GearTopChargeNum 2 for ordinary horses, 5 for the Lord of the Mountain. Rodeo (an unbroken horse ridden) calms after
  CalmDownNum 10. Riding by an enemy raises the horse's familiarity by one. **read**

## 4. Tack (was: horse-spec 4.5)

- Saddle, reins and mane are separate actors put on the horse through its EquipmentUser: slots 0xf mane, 0x10 reins,
  0x11 saddle. The node OneShotHorseEquipHorseTack calls main+0x1b9cec8, main+0x1b9ce60 and main+0x1b9ce10, which call
  **main+0xc48ac0(equipment user, slot, &name, -1, 0, 1)**; the name object is a pointer and a capacity, as for an
  armour create. **read**
- A horse made by name alone carries no tack and has its kind's default coat. The coat comes from the create's
  overrides `UidHash`, `OverwriteColorType`, `OverwriteFootType`, `Horse_OverwriteFamiliarity` (their layout in the
  create block is **open**). **read**
- The wagon is a child actor of `GameRomHorseSaddle_07_WithWagon` (ActorHolder). **read**

## 5. Movement and the gait values (was: horse-spec 7)

- A quadruped moves with a character controller, not with root motion: the main body is a controller with preset
  `Horse`; the dynamic `HorseBody` bodies are sub-bodies bound to the skeleton. AnimeDrivenMove exists on every horse,
  deer, bear, wolf and boar, but no node of the horse graphs uses it. **read**
- Gear speeds (`QuadrupedParam.AnimeSpeeds[..].LinearSpeeds`): GameRomHorse 1.59, 5.26, 10.52, 15.78 m/s (angular
  1.31, 1.87, 1.87, 1.87 rad/s); the slow variation 1.59, 5.26, 9.86, 13.41; a Lynel 2.51, 8.30, 16.61, 24.92. Gear 4
  ("Top") is inferred to be the spurred gallop. **read**
- Every gait clip of a horse is 24 frames (0.8 s at rate 1); the speed is the play rate (S+0xd0). **read**
- **The gait is not chosen by the animation command alone.** The command is coarse (`Move_Gear1..4`, `Move_Normal_*`,
  `Move_Escape_*`, `Move_Attack`, `Wait`); inside it the ASB reads blackboard values (32 for a horse, 6 to 8 for deer,
  bear, wolf) that the AI writes each frame: main+0xab9d90 (the quadruped's gait update, called from AI Execute nodes)
  ends with three writes through the animation player's setter (vtable +0x70) at ids kept at +0x54, +0x58 and +0x5c of
  its state. Which ids and values those are is **open**. **read** (research horses/H4 B6)
- The keys: horse `RequestGear`, `StickX`, `StickAngle`, `DirectionAngle`, `UserSpeed`, `GroundNormal`,
  `GroundNormalSide`, `ForwardBent`, `PullAnmRate`, `Variation`; deer, bear, wolf and others `RequestGear`, `StickX`,
  `DirectionAngle`, `GroundNormal`, and `Distance` where the table has it. Whether the gear is selected by the command or
  by `RequestGear` is **open**. **read**
- FittingModelToSlope is a component that casts its own rays and tilts the model each frame; on the same ground it
  gives the same normal. Whether it writes the actor transform or only the model matrix is **open**. **read**
- Death: a horse's Die (verb 159) runs ExecuteHorseDieAction (DyingFrames 500); an animal's dead clip is 70 frames.
  **read**

## 6. Where horses and animals come from (was: horse-spec 3, 4.2-4.4)

- **Placed**: placement hash, as enemies. **read**
- **Area-made animals**: `Sequence/AutoPlacementAnimal.module` and its Bear, Boar, Deer, Fox and Wolf forms make
  animals around each game's own player (deer, doe and elk where there is forest, away from water: group radius 10,
  water distance 15, forest density 0.6). They have no placement hash. Whether the animal sequences go through the
  enemy push main+0xa80f28 is **open**. **read** (research horses/H4 A4)
- **Wild herds**: not placed. A `WildHorseCreateTag*` actor (9 kinds, 77 placed: 50 on the surface, 27 in the depths)
  makes 2 to 4 horses in one call (ExecuteWildHorseCreate, main+0xd7ff68; its tag actor at [[[x0+8]+8]+0x1f8]; it
  returns true once the herd is made), drawn from a name list by the global random generator. The tag has a radius
  of 200 m. **read**
- **A player's own horses**: `OwnedHorseList.*` arrays (10 entries) in the progress save; each entry stores the actor
  name, a 64-bit `UidHash`, stats (familiarity, spur count) and the three tack names. At a load the owned-horse
  manager (SeqOneShotInitializeOwnedHorseMgr, main+0x981330 and main+0x981ac8) makes the last-ridden horse as a **new
  actor by name** with overrides from the save; the stable's "take out" does the same. The whistle moves the existing
  horse (inferred). `Horse_IsRide` and `Horse_Is_LastRidden_Owned` are game-data flags of the player. **read**
- No "wild or tamed" flag exists in a horse's parameters; taming is calming the rodeo state, and registration at a
  stable gives the horse a `UidHash` and a list entry (inferred). **read**

## 7. Animals

- Brains: `QuadrupedCommon.brain.root` for every wild quadruped; wolves and bears `BattleAnimalCommon`; prey and
  domestic animals `DomesticAnimal`; kokko, sand seals, rabbit, squirrel, dog their own. **read** (research horses/H4
  A1)
- **Flee**: `AnimalCommon.brain.escape.module` reads target slot 4 (set by the prey perception filter); while fleeing it
  emits noise (level 0.75 at the target's last place) and sends a force-escape message; at the end the animal deletes
  itself (OneShotGenericDeleteThisActor or the far form): a far fled animal is gone, not kept. **read**
- **Herds are not groups**: no group, nest or leader component on any land animal. Flee spreads by noise and by the
  node OneShotSendQuadrupedForceEscapeMessage (calc main+0x257a8c4), which sends message 0x723f0000 (payload the
  target's place and the sender) through main+0x2573a74 to every perceived animal within **30 m**; the receiver is
  QueryReceiveQuadrupedForceEscapeMessage (main+0x27042c0). Wolves and bears also send one when they die. **read**
- **Senses** (a deer): sight 50 m, discover angle 20 degrees, vertical plus or minus 10 m, grass ignored within 3.5 m;
  hearing 30 m (discover noise level 0.75, alert 0.1, noise reduction on); intuition 4 m; terror type Animal. An animal
  notices a sneaking player by noise within 30 m and by sight within 50 m. **read**
- An animal notices only actors with a player emitter in its own game; an actor built from the Player's pack is not
  noticed by default. **inferred** (from the puppet pack's flags; not run)
- Feeding and following: favourite-food follow modules (dogs and Pao follow the player); a predator eating kills the
  target it holds (OneShotDeathRequestDeathToTargetActor). **read**
- **Hunting**: animals use the enemy damage and death machinery. A deer has life 10 (headshot ratio 10000 on bone
  `Head`, bombs times 2). Drops by the Drop component at death, by how it died: `Animal_Big_Normal`, `BurnedOut`, `Iced`
  (a deer: `Item_Meat_02`, 100 %, up to 2); small animals `Item_Meat_01`, `Item_Roast_01`, `Item_Chilled_01`; a rupee
  rabbit rupees (green 65 %, blue 25 %, red 9 % and more). A kokko lays eggs by a drop with no death. No animal-specific
  save entry was found beyond the compendium photo (names are hashed: not proven). **read** (research horses/H4 A2)
- **Horses**: life 80 to 300 by kind (GameRomHorse 220, the Lord of the Mountain 100); horse damage rates 1.0
  (shockwave 0), impulse damage 4, 8, 12, 16; `ActorGameDataSetting AlwaysRespawn`; no drop table. Any damage plays the
  damage module and a reaction (kick back, find a safe place). **read** (research horses/H2 5)
- The Lord of the Mountain: `HorseParam.Type` Satori, life 100, 5 spurs, its own event graph (not read), seat alias
  `Sub`. **read**

## 8. Not read

- How a seated rider's pose follows its mount each frame (the ModelBind consumer).
- The three gait ids and values of main+0xab9d90; which key selects the gear.
- The horse's live spur count; the coat overrides' layout in the create block.
- Whether the animal area placements use main+0xa80f28; their despawn rule.
- The wolf pack's "give orders" brain verb (target slot 10).
- Taming and registration in detail; the Lord of the Mountain's event graph; the horse god's revival.
- Riding a bear, a deer, a sand seal or a Lynel in a run; the wagon's coupling.
