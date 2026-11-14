# The enemy system (TotK 1.4.3): what a co-op sync needs

How enemies work inside the game, written for the enemy sync that follows the contraption sync. It summarises a
static decode done on 2026-11-14 in two rounds:

- 14 areas in a first round;
- 9 open gaps in a second.

A second pass re-checked every area and every gap. Addresses are `main+0x...`: the Ghidra address is 0x7100000000
plus the offset, and the lifted C function is `f_70` plus the offset in `generated/v143/main/`.

Confidence:

- **sure**: read in the code or the game data;
- **likely**: inferred from the code;
- **lead**: an untested idea.

Nothing here was confirmed by a game run unless it says so. The older run results are in
[re-notes.md](re-notes.md) ("Enemies", "Enemy targets") and [proxy-plan.md](proxy-plan.md) ("Enemies and the
stand-in"). The questions only a run can settle are collected at the end.

The readable decompiled code is in `build/re/enemy/<area>/`: a README.md plus annotated `.c` files for each area.
`build/re/enemy/INDEX.md` lists every decoded function. It stays out of the repo because it is derived from the game.
Functions the decode understood are renamed in the Ghidra project `enemy_<area>_...`.

## Tooling notes

- Ghidra has no function at many important addresses. Read these in the lifted C:
  - the per-actor calc main+0x41bb20;
  - the life update main+0x4400b0;
  - the receiver update main+0x45b930;
  - the perception manager update main+0xe9b070;
  - most AI node bodies, which are reached only through vtables.

  Several areas left small capstone helpers next to their notes: `aiframe/dis.py`, `awareness/lf.py`,
  `target/vt.py`, `gaps/mydis.py`.
- AI node classes have no RTTI. Each class has a tiny name getter in its vtable, so string -> getter -> vtable finds
  any node's code. `aiframe/node_map.tsv` maps all ~7000 node names to their vtables and functions.
- `battlebrain/ainb.py` reads the game's AINB graphs; it parses all 1956 in `Pack/AI.Global.Product.140.pack.zs`.
  `battlebrain/graphs/` has readable dumps, including the 126 graphs reachable from `Bokoblin.brain.root`. The vec3
  and pointer inputs it prints are approximate.

## Big picture

An enemy is an actor with a component table at `actor+0x228`. Its behaviour is data: AINB graphs whose nodes are
C++ classes in the exe.

The actor vtable has one class for every actor (main+0x3633078):

- slot 49 (+0x188) is the per-actor calc main+0x41bb20, which evaluates the AI;
- slot 48 (main+0x2e27b0) runs before it;
- slot +0x98 (main+0x245030) copies the physics result back into the actor.

| What | Where it lives | Notes |
| --- | --- | --- |
| Life | `[[actor+0x228]+0x2f8]` | life `[L+0x1830]+8`, max +0x1838, ExtraLife +0x1840, BreakLife +0x1848 |
| Death controller | `[[actor+0x228]+0x1c8]` | state word +0x20; RequestDeath main+0x1b4b738 |
| BSA (brain, actions, AI) | `[[actor+0x228]+0x158]` | +0x58 brain verb; the AI controller hangs off +0x2618 |
| Condition (status) | `[[actor+0x228]+0x1a8]` | burn, shock, wet, frozen flags |
| Perception emitter | `[[actor+0x228]+0x378]` | its record: perception id, affiliation, link to the actor |
| Perception receiver | `[[actor+0x228]+0x380]` (main+0x4f8d90) | 6 sense lists at +0x40 + k*8, flags u16 +0x34 |
| Target holder | `[[actor+0x228]+0x4b0]` | 18 target slots of 0xe8 bytes at +0x28 |
| Equipment | `[[actor+0x228]+0x230]` | 8 slots of weapon ActorLinks |
| Drops | drop component | DropInfo at +0x19f0; +0x33d4 / +0x33d6 switches |
| Physics / controller | `[[actor+0x228]+0x50]` | movement goes through the character controller |
| Animation (AsPlayer) | `actor+0x228 -> +0x40 -> +0x18 -> +0x18` (main+0xba8ed0) | slots with the current command |
| Weak points (bosses) | `[[actor+0x228]+0x500]` | separate actors with their own life |
| Creation record | `[actor+0x370]` | +0x88 placement hash (u64), +0x98 flags, +0xa4 banc slot |

Older notes put the perception component at +0x378 and read the receiver there. +0x378 is the **emitter** and the
**receiver** is +0x380. This is sure: main+0x4f8d90 returns +0x380.

## AI framework and switching a copy's AI off (areas aiframe, aioff)

- Node classes register in static initialisers (about 6970 call main+0x21b40). main+0xcf95b4 builds a hash table
  of them, and main+0x658c0 creates a node by class hash.
- Every node class has the same vtable layout: update at slot 6 (main+0x2927448) and postUpdate at slot 8. A node's
  actor is `((node+8)->+8)->+0x1f8` (main+0xb58158). Parameters are tagged pointers to 4-byte cells. The blackboard
  is `**(actor+0x228)` -> +0x10. Sure.
- The AI lives in the BSA component. BSA+0x2618 -> +0x18 is the AI controller, and its +0x20[0] is the main
  instance.
  - Each frame, main+0x41bb20 scans the instance's event queue (+0x90/+0x98, 0x10-byte entries; state 2 pending, 3
    running).
  - It ticks running nodes through main+0x1920(node, ctx). Note main+0x1920 is a generic subtree tick with about 105
    call sites, not one per-actor gate.

  Sure.
- **The game's own ways to stop an actor's AI** (sure, code; none tried on a live enemy yet):
  - **Stop timers**, best for a copy: the object at `[[actor+0x228]+0x2e0]` holds 12 float timers that count down
    in calc. While timer 4 or 5 (+0x20/+0x24) is above 0, calc skips the AI instance run. Animation, physics and
    life go on, and the BSA pre-part still commits pending verbs.
    - The game's writer is main+0x1b9ec40(seconds, comp, index), used by the node OneShotActorRequestInactive.
    - A copy would refresh timer 4 every frame.
    - Exception: the timer is ignored while a death is in progress (the death controller's state word bits).
  - **Pause masks**: main+0x2e90e0(actor, name hash, mask) adds a named mask to a map at actor+0x400. Slot 48 ORs the
    masks into actor+0x3fc, and main+0x672540 removes one.
    - Mask bits: IsAI 0x80, IsPreCalc 0x4, IsAS 0x40000, ...
    - Any of 0xc4 skips the BSA tick, the AI, the target holder and the animation update, so it freezes animation
      too.
    - On an actor that has not run yet, the mask is held back until byte `actor+0x4b0 & 7` is nonzero.
  - A far-actor distance gate in calc (main+0x41c670) and whole-actor sleep/pause request bits at actor+0x28.
- The LOD "calc skip" nodes do not stop AI.

## Perception and sight (areas perception, sight, battlebrain, data)

- The manager `[main+0x3a6ea80]` is a singleton with a pool of 0x180 emitter records of 0x268 bytes. A record holds:
  - +0x04 affiliation;
  - +0x07 sublayer;
  - +0x10 perception id;
  - +0x1f8 an ActorLink to the actor.

  **Perception ids are allocation serials and differ between games.** Never send them. Sure.
- Terror and noise events go through a ring (+0xd98..+0xdac). The producers are main+0x4cac5c (terror, about 22
  callers) and main+0x1f40d34 (noise). The manager update main+0xe9b070 drains the ring and keeps the resident
  emitters in a list (+0xc88 count, +0xc90 array). The old note "manager +0x58 = the resident player" is doubtful.
- **Who is resident** (sure, data, every pack): the Player, the Sages and their puppets and Dm_ versions, and two
  Gerudo dungeon-battle zombies.
- The receiver update main+0x45b930 builds each enemy's candidate pool from three sources:
  1. The resident list, but only while the receiver flag bit 3 (u16 +0x34) is set. Resident records are taken with
     no range or sight test.
  2. This frame's events (hearing, terror).
  3. Sight. main+0x49f5f8 makes a physics box query around the enemy (half size = its largest sense range) on a
     sensor layer, and also uses the receiver's own sensor-body contacts. The hits become emitter records.

  A per-sense filter (main+0x451f54) then checks the sublayer mask, a sense level above 0, a channel mask and a
  range sphere. The view cone and the line-of-sight ray are further on and not decoded. Likely.
- Every emitter makes a unit sensor sphere on layer PerceptionEmitter in its actor's physics controller set
  (main+0x943620), so it moves with the actor. That sphere is very likely what the sight query hits (likely).
- What a stand-in needs to be seen like Link:
  - an emitter with sublayer PerceptionPlayer and the Player affiliation;
  - non-resident;
  - a live transform and a sense level above 0;
  - a physics controller set.

  The `--target-fix` pack (proxy-plan.md) gives it that, and the 2026-11-07 run saw it noticed. Data: ordinary enemies
  do not receive resident emitters by default, and they hunt by `TargetAffiliations` (Player, Sage, PlayerCreated,
  ...).

## Targets: how an enemy picks whom to fight (areas target, battlebrain, awareness)

1. Target modules pick from the receiver's lists, never from a global player:
   - EnemyCommonTargetSearchAndSet, core main+0x1866e14;
   - the ByAffiliationList module, update main+0x2748258;
   - the shared search main+0x2741ac4.

   Each gathers up to 0x80 candidates (more are cut off in list order) and drops those the filter nodes reject. The
   affiliation filter is main+0x9755a4. Sure.
2. **No filter or scorer node is wired into any native search module in any of the 1956 graphs** (sure, data). With
   no scorer, the pick is the candidate the backward loop meets first, so **list order decides** (sure, code). How
   the sense lists are ordered is not found.
3. The winner goes into a target slot `[[actor+0x228]+0x4b0]` + 0x28 + k*0xe8. The slot holds an ActorLink, the
   target emitter's perception id (+0x84), the actor id (+0x8c), the type (+0xe2: 0 empty, 1 position, 2 actor) and
   flags. It never holds an actor address. Writers: main+0xa70734 (41 sites) and main+0xa70a1c (15 sites). Sure.
4. In the Bokoblin graphs:
   - The notice target comes from the ByAffiliationList module.
   - EnemyDiscovered picks it by perception id from the Sight list, with the id taken from slot 6, and copies it
     into slot 0.
   - LandBattle and BattleCommon then run the native **SwitchBattleTargetIfNeeded** (main+0x18669fc, main+0x1866aa0),
     which is not decoded.

   Sure for the graphs.
5. **Force battle** is the one place that deliberately takes the resident player (sure, data).
   - The graph `EnemyCommon.brain.waitbeforeforcebattle` sets receiver bit 3 (main+0x2552530), searches Sight into
     slot 0, then clears it.
   - It runs only when the map-logic plug IsStartForceBattle is true (Bokoblin, SocialEnemy and Lizalfos_UnderWater
     wait brains; Lynel and Stal giants too).
   - That is scripted map logic, not ordinary combat, so it **does not explain** the 2026-11-07 stand-in run by
     itself.
6. Why the Bokoblin went for this game's Link after noticing the stand-in (c882 for 3 s) is still open. The
   candidates are:
   - SwitchBattleTargetIfNeeded;
   - the list order;
   - the ByAffiliationList damage and friend-noise branches;
   - the perception "memory" path.

   A run settles it (end of this doc).
7. Alert between enemies is the game's EnemyCoop message (id 0x3ddc0007: target perception id, last position,
   loud-noise flag), carried in the AI blackboard. It is enemy-to-enemy only.

## Brain verbs and actions (areas awareness, verbs)

- `[[actor+0x228]+0x158]` is the BSA:
  - +0x58 the current brain verb;
  - +0x5c a queued verb;
  - +0x64 a state word;
  - +0x24 flags;
  - +0x2628 the form key (default, Sleep, HorseRide, RideSeat, ...).

  The old "awareness words" c182/c082 are the flags at +0x24 changing with the verb. Sure.
- The verb ids are indexes into a 58-name table (main+0x3635280; names turn into ids through main+0x19724dc). Sure.
  The ones that matter most:

  | id | verb | id | verb |
  | --- | --- | --- | --- |
  | 0x1 | Wait | 0xc | LandBattle |
  | 0x2 | WaitAtCamp | 0xd / 0xe / 0xf | UnderWater / Flying / Hanging battle |
  | 0x4 | Sleep | 0x10 | ReturnToHomePosition |
  | 0x5 | Alerted | 0x12 | Wander |
  | 0x6 | EnemyDiscovered | 0x14 | Escape |
  | 0x7 | EnemySearch | 0x1d | GiveUp |
  | 0xb | InvestigateSuspiciousActivity | 0x2e / 0x30 | EnemyLost / Defeat |

  The full list is in `build/re/enemy/verbs/README.md`. To test "in a fight", use the verb's Battle attribute
  (table at bsa+0x16a0 + verb*0x10 + 8), not id 0xc alone: about 12 verbs are battle verbs.
- requestBrainVerb main+0xad9844(bsa, verb, force, caller), 65 call sites:
  - The same verb only clears the queued one.
  - A different verb is queued at +0x5c when force=0 and the BSA is busy.
  - Otherwise it changes the verb at once and runs the leave and enter procedures (main+0xad9948).

  So a copy gets the owner's verb with force=1. Sure.
- Actions: the scheduler at bsa+0x1a88 keeps per verb a list of action sequences. main+0x46edd4 picks the first
  whose conditions pass. Action requests go through main+0x50ad74 into one of five slots at bsa+0x1f0. main+0x50c36c
  then switches the slot's action graph to `<formKey>_<ActionVerb>:<variation>`, which plays the animation commands.
  Sure up to the graph switch.
- To mirror the owner on a copy, send:
  - the verb;
  - the form key;
  - for each slot, the action verb and its variation.

  The mod's existing hook on main+0x50ad74 keeps one request per actor. It must keep one per slot (request +0x48).

## Hits and damage (areas attack, damage, dmgcalc, drops)

- Attacker side: the AI node ExecuteSensorHitSetupAttack (and 22 other callers of main+0x1bf20cc) arms an attack slot
  on the attacker's SensorHit component. On contact, the victim's SensorHit::calc (main+0x303be0) filters the hit
  (main+0x1a52548) and copies a 0x1c0-byte hit entry into its list at `[SH+0x50]`. Sure.
- Victim side: Life::calcDamage (main+0x300a30) runs the external damage calculator.
  - EDC_accumulateHitEntry main+0x1cd263c, then EDC_calc main+0x314b68: the first kind-1 entry becomes `L+0x1a8`.
  - main+0x1cd0c80: all entries of kind 0 or 3 and up.
  - Internal sources (burn ticks, ...) go to `L+0x1230`.

  The life update main+0x4400b0 adds both into L+0x18e8. ExtraLife (yellow hearts) takes damage first, then life;
  BreakLife caps life at max minus BreakLife.
- Formula: `dmg = floor(M * (bonus + power))`, then a minimum floor for some victim classes (a fraction of max life),
  then `max(1, dmg - defence)`.
  - M multiplies the entry rate, a per-type rate, per-attribute rates and special rates (critical, stealth, shatter,
    impact, ...). The rate names are in `dmgcalc/README.md`.
  - Several attribute bits are re-derived from the victim, so the same entry gives the same damage in another game
    only with the same victim state.
  - No separate guard, headshot or weak-point multiplier is in this path.
  - Sure (code).
- **The best way to apply a remote hit** (likely, untested):
  - Write one kind-1 hit entry into the victim's `[SH+0x50]` list after SensorHit::calc and before Life::calcDamage
    in the same frame. The game then works out damage, defence, immunity and the reaction itself.
  - The entry needs: power (+0xa0), rate (+0xa4), type (+0x90), attribute bits (+0x1b0) and the attack id hash
    (+0x1b8). For the reaction it also needs position, velocity, direction, impulse and attacker links that resolve
    in that game.
  - For a number only: add to `L+0x1a8` after EDC_calc. That gives no reaction.
- Reactions: Bsa_calcInterruptVerb main+0x619710 looks up the ExternalDamageReactionTable (or GuardReactionTable) by
  the hit's damage type L+0x253 and intensity L+0x254. It gives the action verb: InterruptSmallHit, MediumHit,
  HugeHit, Smash, Gust, ... Copies should get the owner's reaction verb, not work out their own. Sure.
- Death: when life reaches 0, RequestDeath main+0x1b4b738 runs with cause 16 (LifeDepleted). It has 46 callers and
  21 causes. For some causes it deletes the actor instead. Replay a death on a copy with the owner's cause. Sure.
- Status (burn, shock, wet, frozen): flag word at slot+0x2c of the condition component `[[actor+0x228]+0x1a8]`
  (slots of 0x100). Durations come from ConditionParam. A copy mirrors the flags for the look; the owner applies the
  ticks.
- **Drops** are rolled only by the game that commits the death: main+0x1b4cfac -> main+0x1b5599c -> DropMgr_drop
  main+0x1c77af8. The rolls use unseeded global random generators, so two games always differ.
  - The owner must drop, and the copy must not. The game's own no-drop idiom clears drop component +0x33d6 bit0;
    +0x33d4 nonzero also stops it.
  - A copy still uses up random draws at its first roll.
  - There is also a "no redrop" save flag keyed from the placement hash (main+0x1b55e18).

## Link as the victim (area linkvictim)

- Link's life is the same Life component, with ExtraLife and BreakLife on and a Player damage calculator. While
  Player flag P+0xae8 bit 19 is set, that calculator drops most hits (likely the invincibility gate).
  "PlayerDamageMessages" only makes a Sage talk.
- To apply "enemy X hit you" to the local Link with the game's own reaction:
  - Best (likely): the same hit-entry injection as above, on Link.
  - Otherwise: add the damage into `L+0x1230` before the life update, fill the last-hit fields the reaction nodes
    read (L+0x1a8, +0x1b8 params, +0x240, +0x1d0, +0x1f8, +0x214 velocity), and force the Interrupt* verb.
- The perfect guard is an animation window on the guarding Link's own game (hold event 0x38, QueryASGetHoldEventJustGuard).
  The hit entry carries a just-guard bit (+0xb bit 1). Only the guarding player's game can decide it; it then tells
  the enemy's owner game, which makes the enemy react. What starts the flurry rush is not found. The enemy side only
  asks the attack-permission limiter for a slot.

## Spawn, respawn, identity (areas spawn, respawn, identity)

- Placed enemies come from Banc entity files into banc slots. main+0x65b710 makes create requests. New actors show
  at main+0x50d6bc, which the mod already hooks.
- The creation job is built by main+0x59c870 (0x100 bytes): +0x40 position, +0x4c rotation, +0x88 the u64 placement
  hash read from the entity's `Hash`, +0x98 flags, +0xa4 the banc slot. Runtime spawns get hash -1 and slot -1.
  Sure, except that `[actor+0x370]` is this job is likely.
- Delete: main+0x63b6b0 -> main+0x63baa8. The despawn check main+0x9550f0 gives the reasons: 6 banc slot unloaded,
  3 radius, 5 a flag, 10. Do not send unload deletes to other games.
- **Killed flags** (sure, code and data):
  - A dead placed enemy is a GameData `Bool64bitKey` flag keyed by its placement hash, value 1, saved in
    progress.sav; 8853 of 8926 placed enemies have one.
  - Death sets it through placementSetDead main+0x195321c, which also writes an older store, and revive clears it
    through main+0x19533e4. The setter is main+0x2ab6dcc.
  - Map loading checks it in main+0x1fcc070.
- **Blood moon**: the `Sequence/BloodyMoon` module requests reset type 3, which clears the flags whose reset mask has
  bit 3 (mask 88; about 8100 enemies) and re-creates the static placements. Mask 208 is the Zonai day (type 7); mask
  80 never resets (mini-bosses such as the Depths' CaveMaster). Sure for the data; the code that applies the mask is
  not found.
- For sync, send the kill flag `(placement hash, 1)` and revives `(hash, 0)`. Keep the world clock equal, or send
  resets. Runtime spawns have no hash. The proposed id is `(owner player, sequence)` for free spawns and
  `(parent placement hash, name hash, ordinal)` for children of placed spawners, carried in the spawn message.

## Movement, position, animation (areas move, identity, anim)

- AI movement never writes the position. Nodes push a direction and speed into the character controller at
  `[[actor+0x228]+0x50]` -> +0x20 -> +0x10 -> +8. Its vtable: +0x48 direction, +0x68 speed, +0x70 velocity, +0xc0
  position and rotation. Sure.
- After the AI, the actor's slot +0x98 calls main+0x257310, which copies the physics body into actor+0x2b4 (position),
  +0x2c0 (rotation), +0x320 (linear velocity) and +0x32c (angular). Sure.
- **Place a copy** with ForceSetMatrix main+0x45ce70(actor, m[12], 0), the mod's coop_ForceSetMatrix. It sets the
  controller or rigid body, and the next step mirrors that into the actor. Never write actor+0x2b4 alone. Sure.
- A copy also needs the move state (ground, fly, water) and the forward speed so its animation blends. Jumps and
  move-state changes are events.
- Animation: AsPlayer slots hold the current command (+0x10) and rate (+0xd0). Commands start through main+0x4dc628.
  Attack windows are animation events. With the action-slot mirror above, the copy's action graph plays the same
  commands. Sending bones, as for Link, is the fallback.

## Weapons, bosses, groups (areas weapon, special, groups)

- Weapons: a weapon is its own actor, tied to its enemy by an ActorLink in an EquipmentUser slot. To copy one, send
  owner, slot, weapon name and durability, and request the equip (main+0x4f59ec) once the weapon actor exists.
- Bosses: weak points are separate actors with their own life. Talus arms, the Hinox eye, Flux Construct blocks and
  the Kohga golem are many-actor bodies, so sync every part. Creator children (main+0x231d288) are made only by the
  owner. The Like Like can swallow any actor. Forms change through the form key.
- Camps (ExecuteMetaAIEnemyCampNormal) and armies use the global random generator, so one game owns each.
- The attack-permission limiter `[main+0x3a64670]` caps only pre-registered target ids, so a stand-in gets unlimited
  attackers. The enemy level comes from save-game defeat counters (GameData Ints), which must be equal.

## Single-player assumptions

1. The resident player emitter (this game's Link), taken only by force battles and Sage modules.
2. The resident actor manager (main+0x1fc9f6c) returns one Link. Its callers are despawn-by-distance and idle gates.
3. Name lookup main+0x1a30670 knows only "Player".
4. The player-status hub `**(main+0x395ea40)` is rebuilt every frame from this game's Link by PlayingInfoMgr::calc
   (main+0x7216f0). The enemy calc does not read its position. The SensorMgr re-centres on it after a jump of 300 m
   or more (a fast travel).
5. Global random generators, one attack limiter, and camps, armies and drops all decided locally.

## The sync design this points to

- **One owner per enemy**, the game nearest to it (today's hand-over). Everything that decides is local: AI, camps,
  armies, random rolls, drops.
- **Copies do not think.** A copy has:
  - its AI stopped through stop timer 4, refreshed each frame;
  - drops off (+0x33d6 bit0 cleared);
  - the owner's transform set through ForceSetMatrix;
  - the owner's verb through requestBrainVerb with force;
  - per action slot, the owner's action request and form key;
  - life, condition flags, the reaction verb and the death cause from the owner;
  - weapons as linked actors.
- **Hits**: a player's hit on a copy goes to the owner as a hit entry, which the owner injects. An enemy's hit on a
  remote player goes to that player's game as a hit entry, injected on their Link. A perfect guard is decided by the
  guarding player's game and sent to the owner.
- **Targets** stay in the owner game: the remote player's stand-in must be a sensed Player-affiliated emitter there.
  The open item is why an enemy that saw it turns to this game's Link (Targets, item 6).
- **Saves**: kill flags keyed by placement hash, defeat counters and the world clock kept equal.
- **Identity**: placement hash for placed enemies, an owner-assigned id for spawns. Never perception ids or
  ActorLinks.

## Still open by reading code

- The view cone and line-of-sight test after the sense filter, and how the sense lists are ordered.
- SwitchBattleTargetIfNeeded (main+0x18669fc, 0x1866aa0).
- What starts the flurry rush; Link's reaction choice site.
- The applier of the blood-moon reset mask (GameDataMgr+0xcb8) and the setter of the "record the kill" bit (D+0x2c
  bit 3).
- Where the job is stored into actor+0x370.
- BSA +0x60/+0x70/+0x84.

## Needs a game run (when the game is free)

Each area's README lists its exact dumps; these are the ones that matter most:

1. **The target switch.** A Bokoblin that noticed the stand-in. Dump in order:
   - its Sight list entries, with each emitter's perception id;
   - receiver bit 3;
   - target slots 0 and 6;
   - which function writes slot 0 (main+0xa70734 / 0xa70a1c callers).

   Setup: `build/re/enemy/target/README.md` and `battlebrain/README.md`.
2. **The copy's AI switch.** Set stop timer 4 on a live enemy. Check that animation, movement and the BSA keep going
   while the AI stops. Dump the 12 timers of a normal enemy first.
3. **A real hit entry.** Dump at main+0x1cd263c (x3) and read `L+0x1a8`:
   - Link's sword on a Bokoblin;
   - an arrow;
   - a sneak strike;
   - an enemy hitting Link.

   This checks the formula and the injection plan.
4. **Kill flags.** Log main+0x2ab6dcc while killing a placed enemy (one write expected) and while one despawns by
   distance (none expected).
5. **Reactions and ids.** The Interrupt* action-verb numbers. Link's BSA after small, medium and huge hits and after
   a perfect guard. The flag P+0xae8 bit 19 while rolling.
6. **Drops on a copy.** Does RequestDeath on a copy still reach DropMgr_drop when +0x33d6 bit0 is cleared?
