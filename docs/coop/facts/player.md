# The player: Link, gear, riding, hits on Link

What the game does with the Player actor, its gear, what it rides and what hits it (TotK 1.4.3). Conventions are in
[README.md](README.md): addresses are main+offset, `comps` is the component table `[actor+0x228]`. Each fact carries a
mark: **read** (code or data), **measured** (seen in a run, with its date), **inferred**, or **open**. The actor
lookup, the actor's position and rotation, skeletons, animation slots, input and the camera are in
[engine.md](engine.md); Ultrahand, glue and devices are in [zonai.md](zonai.md).

## 1. The Player and its life

**Life** [read]
- Link's life is the ordinary Life component (`comps+0x2f8`), with ExtraLife (yellow hearts) and BreakLife on and a
  Player damage calculator. Current life is a u32 at `[L+0x1830]+8`, max at `[L+0x1838]+8`, in quarter hearts.
  Measured 2026-11-04: 14 of 152 for 3.5 of 38 hearts. Source: enemy-system "Link as the victim"; re-notes "Enemies".
- The life update main+0x4400b0 applies the frame's damage. It also asks death kind 16 itself when an actor's life
  reaches 0 (for parts: zonai.md 9).
- **Open (C23): where Link's hearts really live.** In a 2026-11-06 session Link lost hearts while the cell above did
  not go down and main+0x4400b0 took nothing off it; the note concluded "a Player's hearts are kept elsewhere". Other
  runs (2026-11-06, hits delivered through main+0x31dbc0, section 4) saw that cell fall 14 -> 11 -> 8. Not settled.
  Source: proxy-plan "A bomb arrow's blast hurts the far Link" against re-notes "Enemies".
- **Invincibility gate** [read]: while Player flag P+0xae8 bit 19 is set (the byte `[[comps+0x3a8]+0xaea]` bit 3),
  the Player damage calculator drops most hits (one hit hash, 0x907efc91, still passes). Its setter is not found.
  "PlayerDamageMessages" only makes a Sage talk. Source: enemy-system "Link as the victim"; research: enemy/gaps/G7 4.3.

**What a Player's update writes outside itself** [read, measured 2026-11-04]. A second Player actor running its own
update showed which shared systems a Player touches (the tour runs of re-notes "A co-op Link must not change this
game"):
- Game data: setters main+0x170210, 0x16fdcc, 0x170490, 0x282280, 0x2aabf84, 0x923d20, 0x16ee70, 0x31d824 are
  called from inside a Player's update, some every frame from the player state code (main+0x281d28, 0x281d40,
  0x282a0c, 0x2b04c8, 0x2b0568). main+0x923d20 (game data [main+0x395dff0], 1, &key, value) sets an enum; the Player
  calls it from main+0x207eba0 with key 0x57f52eb8 (the record of the player's last action, values from the table
  "AttackByWeapon" and others), and the instant tips ("To shield surf...") show from it.
- Tips: main+0x207ea50 (tips manager, tip id) queues a tip.
- HUD control guide: every Player's calc shows and hides prompts on the one CtrlGuide screen (engine.md, input and
  UI).
- Ability screen effects ([main+0x3a5e878]: start main+0x2056f3c, end main+0x205719c, focus main+0x2057384) and the
  Ultrahand object highlight main+0x585a30 (from the model update main+0x580ff0, outside the calc) follow whichever
  Player uses an ability. The red Ultrahand tint showed in the other view for 4-5 frames.
- The A button of a Player starts talks, pick-ups, opening, cooking and mounting on whatever this game holds near it.
- A Player's fatal fall brings the game's Game Over.
Source: re-notes "Instant tips from a co-op Link", "A co-op Link must not change this game".

## 2. The EquipmentUser

The EquipmentUser component is `comps+0x230` on the Player and on enemies. [read]

| Offset | What | Mark | Source |
|---|---|---|---|
| +0x28 | gear table, 0x18 a row; the row's first word is a reference whose +0x40 is the item actor | measured 2026-11-04 | re-notes "Gear" |
| rows | 0 weapon, 8 head, 9 body, 10 legs, 11 sheath, 14 quiver, 15/16 extras; bow and shield rows not seen (found by name) | measured | re-notes "Gear" |
| +0x570 + slot * 0x100 | per-slot request records; +0xf0 the slot's command word, +0xf4 the visible byte | read | enemy-sync-spec B.34; research: enemy/gaps/R19 |
| +0x660 | weapon attack byte: 1 for about 0.1 s in each swing (same offset in 1.2.1) | measured 2026-11-07 | proxy-plan "the stand-in as TotK Online's" |
| +0x1430 + 8 * slot | the slot's create handle | read | re-notes "Gear" |
| +0x1620 | an object every EquipmentUser shares (the Player's and any other's point to the same one) | measured 2026-11-07 | proxy-plan, held-item placement |
| +0x3f14 | the lightning wait's time | read | element-sync-spec 5.6 |
| +0x4600 | flag word (bits below), written only by atomic read-modify-write | read | research: enemy/gaps/G7 4.2 |
| +0x4608 | a position stored when bits 4/5 of +0x4600 are set | read | G7 4.2 |
| +0x4620 + slot | in-hand byte per slot (slot 0 weapon, 3 bow, 4 shield), bit 1 = in hand | measured 2026-11-05 | re-notes "Items in hand" |

**Flag word +0x4600** [read unless marked]
- Bits 10 to 13: read by the guard classifier (section 4) as guard (11, 13) and just guard (10, 12). Their writer
  was not found; most likely the shield animation's JustGuard window [inferred].
- Bit 16: the lightning wait. Set only by the Sage's thunder field node (main+0x2438e60, through the request
  0x75e90009 to main+0xe26964) and by the AI node OneShotEquipmentUserSetLightningWait (main+0x2529c3c), which no
  graph of 1.4.3 uses. A thunderstorm's static warning on metal gear is not this bit but the gear's own chemistry
  (elements.md). An arrow shot during the wait carries lightning (ShootableArrow_IsLightningWait, main+0x1d1fb70).
- Bit 17: set when an attack meets the raised shield; the Player's hit filter cancels a hit while it is set
  (section 4). Measured 2026-11-06.
- Bit 19 (0x80000): the jump or fall attack. Bit 33: an arrow attachment. Source: enemy-sync-spec B.34.
- Bits 4/5 (0x10, 0x20): set by main+0x1b738e4. G7 read that function as "weapon use begins"; the later enemy reading
  uses it as an enemy's drop call (enemy-sync-spec B.13). Which is right for the Player: open.
- Bit 45: set by main+0x1b7c3f4 when an equip fails or the slot is empty.

**Visible byte** [read]: the game's setter main+0xced8c0 writes the byte at +0x570 + slot * 0x100 + 0xf4 and sets bit
0x200 of the slot's command word, which the component's own calc then acts on. Other command bits: 0x100 damage,
0x20000 extinguish, 0x10000 re-fit. Source: enemy-sync-spec B.34.

**In hand** [measured 2026-11-05]
- The hold query main+0x3e322c (eq, slot 0 / 3 / 4) answers 1 when the item is out; it reads the in-hand byte's bit 1.
  The item's own hold state (`[[item+0x228]+0x208]+0x3d0` bit 0, `+0x3d4 == 1`) is a second, separate record.
- The bow's in-hand bit is set only for a moment when the bow comes out, not while aiming.
- The game's in-hand calls (1.4.3 addresses matched from TotK Online's 1.2.1 ones): setEquipmentInHand main+0x4f5700,
  removeEquipmentInHand main+0x4f5194 [read].
- Held-item placement [read, measured 2026-11-07]: main+0x45a460, a method of the item's component (x1 the holder),
  called from main+0x455108 for every holder, follows the holder's +0x1620 object. With that object's +0x2cc set, it
  asks game data (key 0x90b81999, by slot) for this game's player's item in that slot; when the player has none it
  never reads the holder's in-hand byte, so the item is not put in the hand. With +0x2f4 bit 0 set and +0x2cc clear
  on that object, it reads the holder's own byte. A null object there crashed another thread (main+0xc9ad84).
  Item placement also reads the model matrix: main+0x25cf88 and main+0x269f70.

**"The player's EquipmentUser"** [read, measured 2026-11-05]
- The global `[[[main+0x39601e8]]+0x480]` holds the EquipmentUser the game takes as the player's. A newly made
  Player actor takes this field over (it went 0, then the new Player's).
- Loading another save keeps this game's Link actor and its EquipmentUser. The load's re-dress, main+0xd97d70
  (main+0xd97dac: slots 0, 8, 7, 9, 10, 11 through main+0xc4a5f4), goes to the EquipmentUser in that global, not to
  the calling Link's. Frame chain of the re-dress: main+0xc4aba4 <- 0xd97e08 / 0xd97dfc <- 0xd97d90 <- 0x1fe4ed4
  <- ... <- 0x244d48 (a Player's update).
Source: re-notes "Second-save gear, traced", "Proxy mode, gear (stage 2)".

## 3. Creating gear

[read; calls confirmed working 2026-11-04]
- **Slot build** main+0xc4a5f4 (eq, slot) reads the slot's item from the save and creates it by name.
- **Weapons** main+0xaf100c (eq, slot, &name, 1 in hand / 2 put away, life -1, extra life -1, &attachment, 1, 0, 0,
  0, -1, 0, -1). The 7th argument (a string object) is the fused material's actor name (for a rock-fused stick:
  AsbObj_RockParts_C_S_01). It clears the slot's handle and destroys the old item first; an empty name leaves the
  slot empty. `&name` is +0 char*, +8 capacity 0x40.
- **Armour** main+0xc48ac0 / main+0xc4961c (eq, slot, &name, dye, 0, 1). The dye is the 4th argument: the save holds
  an enum mapped to 0..14, else -1 (undyed). Measured 2026-11-05: dye 10 travelled as 10; dye 3 gave a darker tunic.
  A horse's tack node calls main+0xc48ac0 too.
- **Create slots**: 0 weapon, 7 bow, 8 shield, 9 head, 10 body, 11 legs; 3 sheath, 6 quiver and 15/16 extras are made
  along with them. The table rows (section 2) are not in create-slot order.
- A Player builds its gear from the save's inventory: two Players on one save look alike. Each Player makes its own
  weapons, armour, quiver, Zonai devices, PlayerRaulHand, Obj_OneTouch_Connection and WakeBoardRope; ThrowBox,
  CarryBox, Parasail and Dm_Npc_RevivalFairy are one per game (resident helpers). Measured 2026-11-04.
- A dropped weapon is the equipped weapon's own actor (it kept its address). Measured 2026-11-05.
- The Player's sword, shield and bow are actors of their own, each with one solid body. Source: contraption-sync-v3 29.2; research
  zonai/gear-physics.
Source: re-notes "Gear", "Armor dye", "Proxy mode in a play session", "Equipment, HUD, camera".

## 4. Hits on Link

**The delivery** [read, measured 2026-11-06]
- An attacker arms an attack slot on its SensorHit component (`comps+0x428`). On contact, the victim's SensorHit
  calc (main+0x303be0) filters the hit and copies a 0x1c0-byte hit entry into its list at `[SH+0x50]`. The same
  contact also adds a 0xa8-byte entry to the attacker's second list `[SH+0x58]` with the same class byte (R17).
- Life::calcDamage, the hit update (main+0x300a30), runs on the victim's own thread and turns the entries into the
  damage component's fields. main+0x31dbc0 (x0 the damage component's receiver at +0x190, x1 the attack, x2 the hit,
  0xe8 bytes: damage, kind, attacker handle, directions, flags) merges a hit into the receiver; the hit functions
  call it up to three times per hit, once with damage and a kind.
- The hit's record on the damage component (`comps+0x2f8`): +0x1a4 kind (4 for a sword, back to 0 the next frame),
  +0x1a8 damage in quarter hearts, +0x1c0 the attacker's reference, +0x1c8 its id, +0x1d0 attack flags, +0x1d8 type,
  +0x1dc power, +0x1fc the hit place, then its direction; list links at +0x260..+0x270. The contact records wait at
  `[dc+0x1910]+0x50`, 0x1c0 bytes each.
- A sword's attack flags carry the melee bit 0x100000; an arrow's and a bomb's do not. Measured 2026-11-06.
- A hit entry given to a Link's own list at the start of its hit update is taken as a hit of its own (damage and
  knock-back by the update itself). Measured 2026-11-06: two hits of 3, life 14 -> 11 -> 8. A delivery from another
  thread raced the update and broke an object at +0x1238 (two crashes). Copying the record's words into the Link is
  not taken.
Source: proxy-plan "Enemies shared" (its hits, contact replay); enemy-system "Hits and damage"; research: enemy/gaps/R17.

**The guard classifier** [read] main+0x1a54430, called from the entry builder main+0x1a55134, stores a class in the
entry's byte +0x0b. The builder also stores the attack id pair at +0x38/+0x3c.

| Class | Meaning | Condition |
|---|---|---|
| 0 | a plain hit | none of the below |
| 1 | guarded | equipment word +0x4600 bit 11 or 13 with the geometric test passing; or a forced guard set by AI flags |
| 3 | guarded at the right moment (parry) | bit 10 or 12 (`1 | 2`) |
| 4 | avoided | attribute bit 35 clear and the defender's sensor flag or Dodge hold event (0x3e) active, and its ignore list allows it |

A guard break is no class value. Guard type names (strings at main+0x36512d8): None, GuardSuccessNoReaction,
GuardSuccessDodge, GuardSuccess, GuardSuccessRepelAttacker, GuardBreak, GuardJustSuccess. Source: research
enemy/gaps/G7 4.2, R11 1.1.

**What cancels or changes a hit on Link** [read]
- The Player damage calculator is vtable main+0x368d990. Slot 7 (main+0x1cca41c) returns at hit state 6, clears the
  hit when the invincibility gate (section 1) holds, or when the guard ints read 7. The listener (slot 9,
  main+0x1cca59c) is the Player's damage filter.
- The Player's hit filter main+0x319120 (from main+0x1cca59c) cancels a hit while EquipmentUser +0x4600 bit 17 is
  set. Measured 2026-11-06: with the bit set, hits were cancelled with no reaction at all (the Link stood as before).
- A guarded hit reaches main+0x31dbc0 with damage 0 and nothing else different: the block is decided where the hit
  is built, from the attack's contact with the shield. It is not the guard check the delivery runs (main+0x1cd380c
  from main+0x1cceaac), and flag +0xe0 bit 3 is the guard's result, not a skip. Measured 2026-11-06.
- Shield raised, as seen from outside: component `comps+0x3b8` word +0x160 bit 28; the BSA action 0x41 at
  `[comps+0x158]+0x84`; actor +0x328 bit 1. With the sword on the back and ZL held, hits still landed. Measured
  2026-11-06.
- The classifier and filter use the attacker's ActorLink, its sensor flag and a geometric test; with an attacker
  that does not resolve, the attacker is treated as absent (G7 4.3).
- Link's small, medium and huge reactions are chosen in the Player scheduler's reaction selectors (not decoded).

**Perfect guard** [read, the last link inferred]: the window is the guarding Link's own animation hold event JustGuard
(id 0x38; QueryASGetHoldEventJustGuard), on its own game's clock, and works through bits 10/12 of +0x4600 at
SensorHit calc time. Only the defender's game can decide it. The attacker learns of it from its own `[SH+0x58]`
entries (+0x0b bit 0 guarded, bit 1 just guarded), which its graphs ask through QueryIsOnHitFlagGuardJust
(main+0x2686148) and main+0x2632958; that list is rebuilt at each SensorHit calc. After a guard, an enemy's own node
sets its attack pause `[[comps+0x268]+0x3c]` to 10 + 5 x a random number below 1. Source: G7 4.4; R11; R17.

**Perfect dodge and flurry rush** [read; corrects G7 4.5]: the flurry-rush entry for Link's client is created by Link's
own Player logic (main+0x4c2bb0, call site main+0x4c3578) as an empty request, in the LimiterMgr list at
`[[main+0x395e090]+0x31a8]`. The LimiterMgr's per-frame update (main+0x684e10, store at main+0x685904) then fills the
kind word (+0x20), the attack id (+0x24), the attacker's ActorLink and the active bit, from the attacker's playing
animation and AttackParam and the geometry against Link. It runs no AI node of the attacker. The rush starts when an
entry exists with kind 4, 5, 6 or 10 (main+0x1d0a990; trigger main+0x23ba3b8). An enemy is "receiving a just avoid"
when a marked entry of anybody names it. G7 had guessed the enemy wrote the entry. Source: research enemy/gaps/R18.

**Hold event ids** [read, order inferred from the list]: the names at main+0x3713428 are the hold-event enum: 0
AtCollision, 1 ShieldAtCollision, 2 Ragdoll, 3 JstAvdRec, 5 Guard, 17 WeaponBlur, 56 JustGuard (0x38), 62 Dodge
(0x3e), 64 OverrideReaction, among others. Source: G7 4.1.

## 5. Riding

**The Rider component** `comps+0x410` [read]
- +0x20 a handle to the ridden actor (slot +0x28, id +0x30); +0x38 == 1 while riding; +0x3c seat; +0x44 type. The
  controller's state word 3 means steering a stick.
- "This Link rides horse H" is Rider +0x38 == 1 with +0x20 naming H; `Horse_IsRide` is worked out each frame from the
  Rider component by main+0x321e8c. This corrects the older "no riding flag of 1.4.3 is known" and its box test
  (C27).
- ExecutePlayerRide writes Rider +0xf8 to +0x168 (the rider's input); the horse's ExecuteHorseRiddenByPlayer reads
  them through its seat, in the same game.
Source: contraption-game-facts 34 (stick-head); horse-animal-sync-spec 4.6, 5; research: horses/H2, H3.

**Seat claim and ride end** [read]
- main+0x502e60(rider component, ridden handle, mode or type, seat) claims a seat and returns bit 0 (0 refused); the
  rider's actor is `[x0+0x18]`. Every refusal returns before any write. The player's ride on a horse passes type 1,
  seat 0 (main+0xd3c160 re-seats a saved rider at a load). A stick ride starts from an event node (main+0x22ec6fc) or a
  Player selector candidate (main+0x590220); both pass mode 0.
- main+0x592264 means "already riding" and cannot name a stick (corrects contraption-game-facts 49.6, which had it refuse a
  mount: C11).
- A ride ends with main+0xc448c8(rider, 1, 1), as the Player's own controls do (an enemy rider is put off with
  (rider, 0, &0)).
- A seat is emptied by the rider's end of ride, by the rider's teardown, or by the ridden actor's own per-frame check
  when the occupant is dead or gone.
- The code that puts a rider on the saddle each frame was not found [open].
Source: contraption-game-facts 35 (seat-rider), 49.6, 50.6; horse-animal-sync-spec 5, 6.

**A steering stick's rider** [read]: a stick's rider is the occupant of the one seat of its Ridable component; the
seat is filled only by the rider's own ride start. Its rider sources are component 129 (`comps+0x408`: count +0x20,
pointer array +0x28); main+0x42cbdc reads a source with state +0x18 == 2, +0x74 <= 0 and a live handle as the rider
(`[[src+8]+0x40]`). The stick's "engaged" input is its own on/off latch, whose condition is the seat test; it is true
only in the game where the seat is really occupied. The Link takes the stick with its A button inside its own state
machine, not by a call. Source: contraption-game-facts 34 (stick-head), 35 (seat-rider); contraption-sync.md "Phase 4, second
step"; re-notes "Autobuild copies" (earlier research).

**What carries a Link on a moving body** [read, measured]
- The Link is a character controller. CharacterUpdateAttachGround (main+0x1f554f8) casts a ray 4 m down from 1 m above
  the Link; on a hit it adds the hit body's point velocity (main+0x29a9398, v + w x (p - com)) to the character's
  move, replacing last frame's surface velocity, and stays armed 5 s. The ray's call returns to main+0x1f556c4.
  Whether it reads the pending or the stepped velocity: open. Source: research zonai/physics s3.
- So a body moved only by teleport has no velocity and leaves the Link behind. Measured 2026-11-08/09: a plank driven
  by linear velocity each tick carried the Link within 0.01-0.1 m at 3 m/s; teleported each tick it left the Link 12 m
  behind in 4 s. The plank was dynamic in both rows (the old table called some rows "keyframed": C13). An earlier run
  (2026-11-05) that saw no carry on a moving copy is corrected by this one.
- What the Link stands on is in no field of its actor (none in its first 0x4000 bytes). It can be read each frame
  from the controller's contact list, or at the ground ray's call.
- Standing on a contraption is decided by the group floor-contact test main+0x1c04dd8 (dispatcher main+0x1beaa9c),
  which skips contacts inside the group.
- Seats are a separate mechanism (`game::ride::RidingRigidBodyParam`, `RidableSeatParam`): a rider is attached by a
  constraint with its own breakable flag. `MovingPlatformConstraint` exists as a name; its factory returns 0.
Source: contraption-sync.md "Phase 0 results" 1; re-notes "Riding a co-op player's object"; contraption-sync-v3 45.1; research
zonai/sync-v3/p6/controller.

**Weight and push** [read]
- The game sends no force for a Link standing on or walking into something. The Link's main matter body is dynamic,
  10 kg, ContactMassScale 0.1, moved by velocity (PlayerApplyRigidBody main+0x29901c8 -> main+0x298ffd0 ->
  main+0x567750), plus a 90 kg WeightBody and sweepers.
- The contact-mass scale (main+0x2282b8) applies per contact only between two dynamic bodies and, for a Link, only
  when it pushes (a side contact with commanded momentum along the normal: 1 kg), never when it stands: standing,
  the load on a part of mass m_p is 10 m_p / (10 + m_p). The "is pushing" test reads the controller's commanded
  speed. A static or keyframed body gets no scaling and is an immovable obstacle.
- The one impulse the controller applies by code, CharacterUpdatePushObject (main+0x1f591b8: ApplyLinearImpulse
  main+0x29ae114, or an angular velocity), does not go through the collision pair filter. It was not shown to run
  for the player.
- An actor without a controller record that is made dynamic risks a null read in the foot-band test main+0x2a86318.
Source: contraption-game-facts 41 (weight-push); contraption-sync-v3 45.1; research: zonai/sync-v3/p6/mass.

## 6. The player as enemies see it

- **Emitters** [read]: every actor with a PerceptionEmitterParam makes a unit sensor sphere on layer
  PerceptionEmitter in its physics controller set (main+0x943620). The parameter reader main+0x1abe6e0 names
  SubLayerSensor, IsResidentEmitter, NoiseAndTerrorOnlyEmitter and NoEmitUntilConditionsMet (param +0x78..+0x81).
  The Player: resident, PerceptionPlayer. SageCommon: resident, PerceptionSage. EnemyCommon (enemies and horses): not
  resident, PerceptionEnemy. Resident are the Player, the Sages and their puppets and Dm_ versions, and two Gerudo
  dungeon zombies.
- **Resident emitters are not sensed**: an enemy takes them only from the manager's resident list, and only while its
  receiver flag bit 3 (u16 +0x34) is set, which the force-battle graph does. A Bokoblin's receiver has DefaultFilter
  PerceptionFilterEnemy, IsReceiveFromSensorWorld true, IsReceiveResidentEmitter false. So a second actor with the
  Player's resident emitter is never noticed (measured 2026-11-05 to 11-07).
- **Affiliation**: GameActorInfo (RSDB) holds DefaultAffiliation for 1356 actors (the Player's "Player"). Enemies
  keep targets by affiliation (TargetAffiliations: Player, Sage, PlayerCreated, ...).
- Measured 2026-11-07: an actor from the Player's pack with a non-resident emitter and DefaultAffiliation Player was
  noticed by a Bokoblin (awareness c882 for about 3 s), which then fought the real Link 30 m away. Why it went for
  the Link is open (candidates in enemies.md, targets).
- **The manager's player field** [read; doubtful]: the perception manager `[main+0x3a6ea80]` +0x58 (the same field as
  `[main+0x3ad9830]+0x250`) holds this game's Link's data, and main+0x45b930 (the receiver update) reads it. The
  enemy reading calls "manager +0x58 = the resident player" doubtful; in runs that field was read about once per
  run. Three more globals hold this Link's perception data and no second Player's: `[main+0x3a4e6a8]+0x98`,
  `[main+0x3a4e6e0]+0x370`, `[main+0x3a83a30]+0x3b0`. Perception ids are allocation serials: this Link had id 4,
  second Players 0x2b and 0x29 (measured 2026-11-04).
- main+0x72b4e8 takes a scale factor from the registry `[main+0x3a75a40]` (key 0x5d5d398d); it is not "the player".
- **Not targeting**: 1.4.3 main+0x1d1fc70 is TotK Online's 1.2.1 "PlayerPuppet is a player" hook (main+0x1736dec,
  the same code). Its one caller main+0x1d1f9e8 is in the bow function that loads "IsAdditionalArrowBow": a bow check.
  main+0x1a30670 (actor system, out link, &name) finds the one actor of a name ("Player") and is not called in a fight.
Source: re-notes "Enemy targets: not found", "The stand-in and enemies' perception"; proxy-plan "Enemies and the
stand-in"; enemy-system "Perception and sight", "Targets", "Single-player assumptions". C1 and C2 (the perception
component and the "awareness words") are settled in enemies.md.

## 7. An actor made from the Player's pack (the stand-in)

What the game does with an actor whose pack is the Player's with the Player, brain, camera, riding and life
components left out (the co-op stand-in). These are game behaviours, measured 2026-11-07 unless marked.
- **Actor tables**: a new row added to the RSDB actor tables crashed the load; ActorInfo and GameActorInfo are paired
  by row position, so a row must be renamed in place [measured].
- **No Rider, no Player component**: it cannot ride or take a stick; the ride start reads the Rider component without
  a check [read]. It has most likely no character-controller record (a hook on the controller update saw one
  controller in a whole run) [inferred].
- **Animation**: clips played straight on its animation player (play call main+0x1b569fc, engine.md) never pass
  through graph nodes, so graph events (WeaponBlur) are missing. Each slot's body mask is a layer object of the
  animation graph G (`[G+0x1a8]` whole body, `[G+0x1f0]`, `[G+0x2c8]` upper body); the flags the stand-in's own graph
  sets in those masks differ from a Link's, and with its own flags it stood in a T pose in a sword fight.
- **Sword trail**: the Player's SwordBlur component (`comps+0x4a8`, vtable main+0x367e028; update main+0x275954 every
  frame) looks through the animation graph (`[[[comps+0x40]+0x18]+0x18]`, the layers' nodes) for an active ASB event
  of kind 17 (WeaponBlur, the hand as a game::WeaponBlurType) and while one is on emits the weapon's "Blur_Default"
  effect on the weapon in that hand. Handle at +0xc8 (an effect pool index and serial, meaningless in another game),
  event at +0xe0, +0x2a is 1 while the effect lives (about 0.17 s a swing). [read, measured]
- **Held items** follow the shared +0x1620 object (section 2); a stand-in's held sword stays on its back unless that
  object reads the holder's own byte.
- **Motion type**: TotK Online asks requestChangeMotionType (1.4.3 main+0xae4300) for type 0 on its main body every
  tick; what that changes was not checked.
- **Bodies**: a Player's bodies sit in several places of its physics object (engine.md, physics bodies); the stand-in
  has the Player's bodies by data, including a second body the main-body path misses.
- **Lightning**: with the chemical parameter IsEquipmentAvoidLightningTarget true (the Lynels', Ganondorf's and the
  Sage golem's have it), a storm builds no static on what the actor holds and no strike is drawn to it [read]. Its
  chemical component has no elements and its update does not run, so it cannot burn by its own chemistry [measured,
  element-sync-spec 5.7].
Source: proxy-plan "the stand-in as TotK Online's", "The stand-in's sword trail"; element-sync-spec 5.6, 5.7;
contraption-game-facts 35 (seat-rider); contraption-sync-v3 45.1; research: zonai/sync-v3/p6/pose, controller, zonai/player-physics.

## 8. Link's things

**Arrows** [measured 2026-11-05]
- A bow's arrow is an actor (NormalArrow; other types have "Arrow" in the name). The first is made at the bow on the
  ZR press, 1.1-1.4 m from Link; it flies on release (21 m in 0.12 s) and sticks; the next is put on the bow 46 ticks
  after the release.
- Each game also makes two arrows 0.45 m from every Link at load (the quiver, by the look).
- A fused arrow's material is an actor of its own: a Bomb Flower made a BombFruit 1.0 m from the arrow, which moved
  with it and blew up when the arrow hit.
- An arrow made on its own, not by a bow, sleeps: actor state (+0x24) 6 where a nocked arrow is at 4, its model's
  +0x244 at 6 (5), and +0x28 bit 0 set. Running its calc, asking state 4 through vtable +0xf8, or setting +0x28 did not
  wake it. The game stopped calling a frozen arrow's calc after two frames. What a bow does to keep its arrow awake is
  open. (The state machine itself: engine.md, C26.)
- **Trail** [read]: a shot arrow emits its trail once as its flight starts. The flight update main+0x1d23a4c (a method
  of the object at `[[arrow+0x228]+0x3d8]+0x340`, vtable main+0x3693970) calls main+0x1d24498, which emits
  "ArrowTail" (or "LongThrow_ArrowTail") through main+0x4f4e40 (x0 the link user `[comps+0x68]`, x1 &name, x2 2,
  two handles out by x8), keeping the handles at +0x1b0 and a flag at +0x1ed. An emit made for an arrow that was never
  shot lives in the effect pool but draws nothing; its pool entry differs from a shot arrow's only in two flag words
  (+0x8 0x2 against 0x402, +0x34 0x3 against 0xf). What sets those bits is open. Effect pool handles are checked
  against group bases `[main+0x395f388]`, sizes `[main+0x395f390]` and a serial at +0x20.
- The bow function family near main+0x1d1f960, 0x1d1f9e4, 0x1d1fa54 and 0x1d1ffd0 is where TotK Online puts its
  crash guards [read].
Source: re-notes "Arrows", "Arrow copies are not drawn"; proxy-plan "arrow trails".

**The Master Sword's beam** [measured 2026-11-06]: PlayerBeam, a pooled actor, awake while it flies; scale 0.3 (actor
+0x2e4); 50 m/s along its first direction. What makes its burst on a hit was not found. Source: proxy-plan "The
Master Sword's beam".

**The paraglider** [measured 2026-11-14]: the Parasail actor is made when the paraglider is first opened, then kept
and used again; it is never new a second time. Source: contraption-sync-v4-spec 24, note 5.

**Shield surf** [measured 2026-11-05]: the surfing Link's shield is out of hand, under its feet (about -0.15, 0.15, 0
in the Link's frame), and back on its back after.

**Ultrahand's beam** [read, inferred]: main+0x916860 (a Player's Ultrahand part, x0) draws the beam into one global
pool of segments at `[main+0x395f6f8]` (+0x1f0 count, +0x1f4 capacity, +0x1f8 used, +0x200 free list), first giving
every used segment back; main+0x19164e0 pushes onto it. Two Players running it at once on two worker threads broke
the free list (a read of -1 in main+0x19164e0, from the actor job; seen 2026-11-05) [inferred cause]. The same global
holds the contraption group table (zonai.md 2). Source: re-notes "Zonai devices: what turns one on".

**Abilities** [read, likely]: the ability end call main+0x205719c takes ids 1 UltraHand, 2 OneTouchBond (Fuse), 3
CeilingClipper (Ascend), 4 ReverseRecorder (Recall), 5 AutoBuilderMaker. What Recall, Fuse and Ascend do to
contraption parts is not read. Source: research zonai/sync-v3/review3/facts.md E4.

**Wind on cloth** [measured 2026-11-05]: after a fan's wind, a Link's hair and clothes can keep flapping. The flapping
is not in the 127 body bones, and no object of the Havok cloth wind class TthclSimpleWindAction (vtable
main+0x35c4888; base wind +0x20, gust +0x40 made by main+0x150dcb4) exists in the scene. What sets the cloth's wind
is open. Source: re-notes "Proxy hair and clothes stay blown after a fan".

## 9. Not read

- Where Link's hearts are kept, if not in the Life cell (C23); the setter of the invincibility bit P+0xae8 bit 19.
- The writer of EquipmentUser +0x4600 bits 10-13 (guard, just guard).
- Link's reaction choice (small, medium, huge) in the Player scheduler selectors; fall damage (verbs 0xad..0xaf,
  L+0x1240/0x1242); armour set bonuses; fairy revival.
- The code that puts a rider on the saddle each frame; whether AttachGround reads pending or stepped velocity.
- Why an enemy that noticed a Player-like actor fights this game's Link instead.
- What keeps a bow's arrow awake; what turns an arrow trail emit into a drawn effect; the sword beam's burst.
- What moves a Link's cloth in wind.
- What Recall, Fuse and Ascend do to Zonai parts.
