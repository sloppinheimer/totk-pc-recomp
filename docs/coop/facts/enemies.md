# Enemies: game facts

How enemies work inside TotK 1.4.3, as far as it has been read or measured. Conventions are in
[README.md](README.md): addresses are main+offset (Ghidra 0x7100000000 + offset; lifted C `f_70` + offset). Marks:
**read** (seen in the code or data), **measured** (seen in a run, with its date), **inferred** (follows from what was
read), **open** (not read). Where a later reading corrected an earlier one, only the later fact is kept, and
"(corrects ...)" names what it replaces.

Sources are cited as an old document and section (enemy-system, enemy-spec, enemy-design: the files
docs/coop/enemy-system.md, enemy-sync-spec.md, enemy-sync-design.md) or as a research note ("research:
enemy/gaps/R22"; the research folder is outside this repository and is never copied into it). "was: spec B.NN" marks a
section whose facts code comments cite under that name.

Short names used throughout: `comps` = [actor+0x228] (the component table); `BSA` = [comps+0x158]; `L` (life) =
[comps+0x2f8]; `D` (death controller) = [comps+0x1c8]; `G` (animation player) = [[[comps+0x40]+0x18]+0x18].

## 1. Components

An enemy is an actor whose behaviour is data: AINB graphs whose nodes are C++ classes in the exe. Every actor class
shares one vtable layout (main+0x3633078). **read**

| What | Where | Notes |
| --- | --- | --- |
| Life | L = [comps+0x2f8] | life [[L+0x1830]+8], maximum [[L+0x1838]+8], ExtraLife +0x1840, BreakLife +0x1848 |
| Death controller | D = [comps+0x1c8] | state word D+0x20, switch word D+0x2c (section 9) |
| BSA (brain, actions, AI) | [comps+0x158] | section 3 |
| Condition | [comps+0x1a8] | burn, shock, wet, frozen per slot (section 7; elements.md) |
| Chemical component | [comps+0x180] | elements.md |
| Perception emitter | [comps+0x378] | its record: perception id, affiliation, link to the actor |
| Perception receiver | [comps+0x380] (main+0x4f8d90 returns it) | six sense lists at +0x40 + k*8, flags u16 +0x34 |
| Target holder | [comps+0x4b0] | 18 slots of 0xe8 bytes from +0x28 |
| Equipment user | [comps+0x230] | section 10 |
| Drop component | C = [comps+0x1e8] | DropInfo at C+0x19f0; switches C+0x33d4, C+0x33d6 |
| Character controller | [[comps+0x50]+0x20]+0x10 | section 2.6 |
| Animation player | G | section 5 |
| Ragdoll | [comps+0x3e8] | section 8 |
| SensorHit (attack and hit lists) | [comps+0x428] | section 7 |
| Shooter | [comps+0x448] | section 11 |
| Lifter, Liftable | [comps+0x308], [comps+0x300] | section 10.5 |
| Rider, Ridable | [comps+0x410], [comps+0x408] | horses-animals.md |
| Model controller (looks) | [comps+0x280] | section 12 |
| Attention sender | [comps+0x130] | section 12 |
| Battle-info | [comps+0x140] | section 12 |
| Game-balance client ("limiter holder") | [comps+0x268] | swift-attack bit, attack pause, defeat counters |
| Look arbiter (LookIK) | [comps+0x318] | section 5.5 |
| Weak points (bosses) | [comps+0x500] | separate actors with their own life |
| Creation record | [actor+0x370] | +0x88 placement hash (u64), +0x98 flags, +0xa4 banc slot |

Corrects re-notes "Enemy targets: not found", which called +0x378 "the perception component" and read the receiver
there: +0x378 is the **emitter**, +0x380 the **receiver** (contradiction C1, resolved). Sources: enemy-system "Big
picture"; research enemy/verify/00-synthesis 1, enemy/gaps/R3 (census of components).

### 1.1 The frame of one actor

The actor vtable's slots, in the order the frame runs them (**read**; research enemy/verify/00-synthesis 1):

| Slot | Function | Runs |
| --- | --- | --- |
| 48 | main+0x2e27b0 | pause masks into actor+0x3fc |
| 49 | main+0x41bb20 (the calc) | BSA tick, AI runners, animation update (AS_update), life update, death tick |
| 50 | main+0x245030 | the physics result back into the actor (main+0x257310); the model phase (ragdoll read-back, pose push) |
| 51 | main+0x194fd7c ("phase A") | SensorHit::calc, the damage accumulator reset, attacker pick, Life::calcDamage, an action runner |
| 52 | main+0x2fb9b0 | stored far-delete requests among others |

Damage worked out in slot 51 is applied by the life update in the **next** calc. Corrects enemy-system "Big picture",
which called main+0x245030 "slot +0x98": it is slot 50; "+0x98" came from another vtable base (C6, resolved).

## 2. The AI framework and its runners (was: spec 5.1, 5.2, B.37)

### 2.1 Node classes

- About 6970 node classes register in static initialisers (each calls main+0x21b40); main+0xcf95b4 builds a hash table
  of them and main+0x658c0 makes a node by class hash. **read**
- Node classes have no RTTI. Each has a small name getter in its vtable, so name -> getter -> vtable finds any node's
  code. **read**
- Shared vtable layout: update at slot 6 (main+0x2927448), postUpdate at slot 8. For Execute-kind module classes,
  slots 25, 26 and 27 are enter, per-frame update and leave. **read** (research enemy/gaps/R2)
- Every OneShot-kind class shares one enter, main+0x2b09278, which calls the class's own virtual +0x110; the vtable
  pointer at [node] tells the class. Execute-kind classes have their own enters. **read** (research enemy/gaps/R22 Q5)
- A node's actor: node+8 is its instance, instance+8 its controller, controller+0x1f8 the actor (main+0xb58158 reads
  the same chain). **read**, and **measured** 2026-11-15 (run 1: controller+0x1f8 was the actor for every graph
  controller).
- Parameters are tagged pointers to 4-byte cells. The actor blackboard is [[comps]+0x10]. **read**
- The game's AINB graphs: 1956 in `Pack/AI.Global.Product.140.pack.zs`; 126 are reachable from
  `Bokoblin.brain.root`. **read** (enemy-system "Tooling notes")

### 2.2 Graph units per enemy

The BSA loads its units into an array at BSA+0x1e0 (init main+0x88c420, loader main+0x5d640): 0 Main action, 1 Sub
action, 2 SP, 3 SP2, 4 SP3, 5 Brain, 6 Utility, 7 NoCalcSkipUtility, 8 PostSensorUtility. The scheduler (BSA+0x1a88,
init main+0x88df40) loads the action-sequence roots, the interrupt graph and the move-control graph. **read**
(research enemy/gaps/G1)

Census over the 221 enemy packs that have a BSA (research enemy/gaps/R3; **read**, data):

| Unit | Enemies that have it |
| --- | --- |
| Main action, Brain, Utility, InterruptSeq | all 221 |
| Sub action | 55 |
| NoCalcSkipUtility | 21 (Talus, Wizzrobe, Evermean, sand worm among them) |
| SP | 8; SP2 and SP3: Gleeok only |
| PostSensorUtility; a custom scheduler | none |
| MoveControlSeq | 178 (none on Like Like, the Goron, Rito and Zora bosses, swarms, cave bosses, bone heads) |
| Action sequences | 1 to 69 per enemy |

For a Bokoblin, the unit paths read at unit+0x28 were BSA 0 `Bokoblin.action.root`, 1 `Bokoblin.sub.action.root`,
5 `Bokoblin.brain.root`, 6 `Bokoblin.utility.root`; scheduler unit 0 `EnemyCommon.interruptseq.root`, unit 1
`EnemyBasePathFindingUser.movectrlseq.root`, then 248 action-sequence units (many share one controller).
**measured** 2026-11-15, run 1.

### 2.3 The four runner bodies and who ticks what

One runner body exists four times: main+0x50b404, main+0x42e0c8, main+0x2beb80, and a copy inlined in the calc
(main+0x41bb20 from 0x41c6b8). Every graph tick ends in the node tick main+0x1920 (about 105 call sites; a generic
subtree tick, not a per-actor gate). Node enter is main+0x1bb410; the root switch is main+0x565f74 / main+0x57f92c.
**read**

Who runs which unit, for a Bokoblin (**measured** 2026-11-15, runs 1, 9; by return address):

| Unit | Runner, called from | How often |
| --- | --- | --- |
| Interrupt graph ([BSA+0x2618] = scheduler unit 0) | the inlined body, from main+0x41dfa0 | every frame |
| Move control ([BSA+0x2620] = scheduler unit 1) | main+0x2beb80 from main+0x50aec0, inside the action request for slot 0 | every frame |
| Main action (unit 0) | main+0x50b404 from main+0x50afe4, the tail of the action request | every frame |
| Utility (unit 6) | main+0x50b404 from main+0x443b68 | every frame |
| Running action sequence | main+0x50b404 from main+0x46efe4, inside the scheduler | every frame |
| Scheduler | main+0x44ff30 from main+0x443b3c | every frame |
| Brain (unit 5) | main+0x50b404 from main+0x443ca8 (the BSA tick's "not yet ticked" branch, BSA+0x74 == 0) | in bursts, a few times a second; re-rooted by main+0xa12a48 |

main+0x42e0c8 was never entered for a Bokoblin (comps+0x48, the optional graph list, is null; comps+0x90 exists).
Nothing else ran a node. Corrects research enemy/gaps/G1, which took the inlined body for the brain: it runs the
interrupt graph (research enemy/gaps/R1 read it so; run 1 confirmed).

Other facts of the BSA (**read**; research enemy/gaps/G1, R1):
- Slot status bytes BSA+0x26a8..0x26ac (Main, Sub, SP, SP2, SP3): 0 normal, 1 interrupt requested, 2 running, 3
  finished (one frame).
- BSA+0x64 action-sequence status (0 none, 1 running, 2 and 3 done); [BSA+0x2608] the running action-sequence record
  (set only through main+0x44ff30 -> main+0x46edd4 -> main+0x4e10dc).
- BSA+0x74: 0 never ticked, 1 normal, 2 paused.
- BSA+0x26ad bit 5 only skips the calc's distance test; it is no AI switch.
- UseCustomScheduler is the byte [[[BSA+0x25d8]+0x150]+0xa8] (archetype data).
- The scheduler main+0x44ff30 loops 0x3a verb buckets at sched+0x58 + i*0x30 (records of 0x120 bytes) and ticks the
  active record through main+0x1a2f230, falling back through the record's virtual +0x28 (main+0x46f768) to
  main+0x50b404.

### 2.4 Graph nodes are made on first entry

A graph's nodes are made the first time each is entered, by the runner's own enter path (main+0x50b404 makes and
enters them). A tick of a node whose child was never entered reaches main+0x1920 with a null node and faults (read in
main+0x1920 at its child recursion, offset 0x1dd8). **read** (research enemy/gaps/R21) and **measured** 2026-11-17
(two crashes in a play session, main+0x1920 reading 0x1f, each right after a brain that had never entered was asked a
verb with force: requestBrainVerb sets the verb's fields and tests nothing). (was: spec B.37)

Open: the path [[[[BSA+0x1e0]+8]+0x28]+0x18] read as "the brain's controller" never showed an instance queue entry in
state 3 for any copy in a run, placed Bokoblins included; either the path is not the brain's, or such brains never
entered. Not settled.

### 2.5 The game's own ways to stop AI, and what each does not stop

| Way | What it does | What it does not do |
| --- | --- | --- |
| Stop timers: 12 float timers at [comps+0x2e0], counted down in the calc; timer 4 or 5 (+0x20, +0x24) above 0 | skips one runner: the inlined interrupt graph poll. Writer main+0x1b9ec40(seconds, comp, index), used by OneShotActorRequestInactive | does not stop the scheduler, the action sequence, the utility, the action slots or the move control; is bypassed while BSA+0x26a8 is 1 or 2 and while a death is in progress; and any active timer sets a byte (+0x48) that makes the life update throw away the frame's damage |
| Pause masks: main+0x2e90e0(actor, name hash, mask) adds a named mask to the map at actor+0x400; slot 48 ORs them into actor+0x3fc; main+0x672540 removes one | any of 0xc4 (IsAI 0x80, IsPreCalc 0x4, and others) skips the BSA tick, the AI, the target holder and the animation update; IsAS is 0x40000 | freezes animation too; on an actor that has not run yet the mask waits until byte actor+0x4b0 & 7 is nonzero |
| The far gate in the calc (main+0x41c670) | stops the interrupt-graph poll for a far actor | not animation, not physics |
| Whole-actor sleep and pause bits at actor+0x28 | | not read in detail |
| LOD "calc skip" nodes | | do not stop AI |

**read** (research enemy/verify/01, enemy/gaps/G1, R1). Corrects enemy-system "AI framework", which called the stop
timers "best for a copy; animation, physics and life go on" (C3, resolved: one runner only, and damage is
discarded).

### 2.6 Turning every runner away, and what a switched-off enemy still does

Hooks at the entries of main+0x1920 (actor [[[x0+8]+8]+0x1f8]), main+0x50b404 ([x0+0x1f8]), main+0x42e0c8
([x0+0x1f8]), main+0x2beb80 ([x0+0x1f8]), main+0x44ff30 ([x0+0xb70]) and main+0x50ad74 ([x0+0x18], returning 2 = "no
slot"), plus four writes before the calc that empty the inlined interrupt body (ctrl = [[BSA+0x2618]+0x18], inst =
[[ctrl+0x20]]: root index [ctrl+0x19c] = -1, pending root (i16)[inst+0xde] = -1, queue count [inst+0x90] = 0, run bit
(u16)[inst+0xd8] bit 0 clear), stop every runner. What was **measured** on placed and spawned Bokoblins, 2026-11-15
(runs 5, 9, 10, 11, 12):
- Only the scheduler, the utility and the brain's first-tick call ever reached the hooks; no node was ticked or
  entered, no action was requested. The hook on main+0x1920 was never needed.
- The enemy stood in its clip, updated every frame. Released, every runner but the interrupt graph ran again within
  a second; the interrupt graph came back only when its queue count and bit word were put back (a root index alone
  did not do it).
- Life falls natively with no graph: the life update applies damage.
- A small hit's flinch plays with no graph and no play request: it is a partial ragdoll asked by the damage step
  (section 8), not an animation command.
- A death waits at its third phase (D+0x20 = 0x8307) for the graph's signal; released, the enemy plays Die.
- A switched-off enemy with a dynamic body walks on with its last speed (116 m in 27 s in a play test).
- While dead-waiting, the brain's first-tick call comes every frame.

What runs on a switched-off enemy and must be expected to: the calc's pre-part (the BSA commits pending verbs), the
animation update (main+0x432fa0) with its events, the attack arming from animation events (main+0x42ec98), the life
update, the death tick, the perception receiver update, the physics result and the model. **read** (research
enemy/gaps/G1, R1)

(Earlier notes recorded a half-rate update after a switch in run 2: that run had the old enemy sync live between two
games, which took the enemy away; it is void past its 12th second.)

### 2.7 Graph content: who decides and who does

From the node censuses of research enemy/gaps/G1 and R22 (**read**, data):
- **Brain**: brain verbs (OneShotBSABrainVerbSetter, 141 nodes for a Bokoblin, plus native brain modules; 65 call sites
  of requestBrainVerb), target search and every write of target slot 0, notice and alert, memory. Decides only.
- **Utility**: look-at target, equipment search radius, the enemy-to-enemy messages (EnemyCoop reaction-damage,
  discover, investigate). Mostly decides; a Bokoblin's also makes effects, Shooter actors and basket game data.
- **Scheduler and action sequences**: pick the action sequence by brain verb; its nodes (ExecuteBSASetActionVerb,
  ...InvokeActionVerbWithArgs and their Force and Interrupt forms) make the action requests every frame.
- **InterruptSeq**: hit reactions, death, guard; also some doing (a lifted actor's death, a prepared shot dropped, the
  Shooter cancelled).
- **MoveControlSeq**: adjusts the slot-0 request (path steering; blackboard keys `OriginalActionVerbArg`,
  `AdjustedActionVerbArg`). Decides.
- **Main and Sub action graphs**: do. A Bokoblin's Main graph has 2720 nodes: 123 animation plays, 90 controller
  speed and direction writes, about 250 world effects (drops, shots, shockwaves, knock-back, noise, rider get-off, the
  death request). None of the three Main graphs read (Bokoblin, Chuchu, Yiga footsoldier) sets a brain verb or
  searches a target; the Chuchu's and the Yiga's write no target at all.
- Native modules (bodies unread for most): Walk, Run, Turn, WalkBackwards, Wait, Defeat, Discover, StopAndPlayAS,
  GetUp, Sleep. 220 distinct native modules over all enemies, 64 action modules beyond the Bokoblin's (research
  enemy/gaps/R3).

## 3. Brain verbs and actions (was: spec B.38, B.40)

### 3.1 The BSA's verb fields

- BSA+0x58 the current brain verb, +0x5c a queued verb, +0x64 the action-sequence status, +0x24 flags that follow the
  verb, +0x2628 the form key (a string compared by content: default, Sleep, HorseRide, RideSeat, ...). **read**
- The words once called "awareness words" (c182, c082 at +0x24) are these flags changing with the verb; +0x84 is not
  awareness (corrects re-notes "The stand-in and enemies' perception" and proxy-plan "The copy's awareness"; C2,
  resolved). BSA +0x60, +0x70, +0x84 are otherwise **open**.
- Brain verbs are indexes into a 58-name table (main+0x3635280; names to ids by main+0x19724dc). **read** The ones that
  matter most:

| id | verb | id | verb |
| --- | --- | --- | --- |
| 0x1 | Wait | 0xc | LandBattle |
| 0x2 | WaitAtCamp | 0xd, 0xe, 0xf | UnderWater, Flying, Hanging battle |
| 0x4 | Sleep | 0x10 | ReturnToHomePosition |
| 0x5 | Alerted | 0x12 | Wander |
| 0x6 | EnemyDiscovered | 0x14 | Escape |
| 0x7 | EnemySearch | 0x1d | GiveUp |
| 0xb | InvestigateSuspiciousActivity | 0x2e, 0x30 | EnemyLost, Defeat |

  0x35 is the terror verb, 0x36 SuspiciousIndividualNotice, 0x31 the notice balloon's (research enemy/gaps/G2, R4).
- "In a fight" is the verb's Battle attribute (table at BSA+0x16a0 + verb*0x10 + 8), not verb 0xc alone: about 12 verbs
  are battle verbs. **read** A verb's Sleep attribute is byte +0xb bit 0 of the same row (research enemy/gaps/R4).

### 3.2 requestBrainVerb

main+0xad9844(BSA, verb, force, caller), 65 call sites. The same verb only clears the queued one; a different verb is
queued at +0x5c when force is 0 and the BSA is busy; otherwise the verb changes at once and the leave and enter
procedures run (main+0xad9948). With force 1 no BSA tick is needed. **read** It tests nothing about whether the
brain's graph has ever run (section 2.4).

### 3.3 The scheduler and the action request

- The scheduler at BSA+0x1a88 keeps per brain verb a list of action sequences; main+0x46edd4 picks the first whose
  conditions pass. **read**
- An action is asked for by **main+0x50ad74(BSA, request, mode)**. x0 is the BSA ([BSA+0x18] the actor); mode bit 0
  means "restart even if the same verb is playing". Returns 0 accepted, 2 no slot object for that slot index, 3 the
  slot already took a request this frame. **read** (research enemy/gaps/R22 Q2)
- Callers: the action-sequence request nodes (enter main+0x2bab4 with mode 1, per-frame update main+0x50abd0 with mode
  0), the utility, the interrupt sequence, the move-control sequence, a first-frame default in slot 48 (main+0x2e4e18),
  the calc's empty fill (main+0x4284fc), the scheduler's fallback (main+0x46f0b0), and about a dozen others. In a camp
  fight: from main+0x50acdc every frame, from main+0x2bbd4 at an action-sequence node's enter, from main+0x2e4e1c
  twice. **read**; **measured** 2026-11-15 (run 9).

The request, 0x60 bytes (**read**; research enemy/gaps/R22 2.2, enemy/verify/04):

| Offset | Content |
| --- | --- |
| +0x00, +0x0c, +0x18 | three vec3: target or attack position, a second vector, desired velocity; a NaN first float means absent |
| +0x24 | u32 (4 in nodes that set no verb, 2 in the Wait fallback) |
| +0x28 | u32 forced verb; 0x11c means unset |
| +0x2c, +0x30 | floats (NaN by default); +0x30 an approach rate |
| +0x34 | u32 (0x12 by default) |
| +0x38 | pointer to the variation string; valid when byte +0x40 is 1 |
| +0x48 | u32 slot index 0..4 (Main, Sub, SP, SP2, SP3) |
| +0x4c | u32 action verb (the normal place) |
| +0x50 | u64 flags: bit 0 interrupt, bit 8 force restart |
| +0x58 | u64 pointer to the requesting node (an address of that game) |

The action verb enum has 284 values; among them 40 Wait, 108 Appear, 109 Disappear, 131 JumpAttack, 145 OrderAttack,
151..159 the interrupt verbs (159 InterruptDie). The effective verb is +0x28, or +0x4c when +0x28 is 0x11c. **read**

Of the 0x60 bytes, only +0x38 (text) and +0x58 (the asking node) are addresses; the rest are numbers. **read**
(was: spec B.40)

What main+0x50ad74 does, in order (**read**; research enemy/gaps/R22 2.3):
1. Copies the request to a local, guarding NaNs.
2. For slot 0, if the move-control sequence [BSA+0x2620] exists: registers its two blackboard keys on
   [[BSA+0x2620]+0x30] and runs the steering adjuster main+0x2beb80.
3. For an interrupt request on slot 0: leaves the running action-sequence record, nulls it and sets the status byte
   BSA+0x26a8 to 1.
4. Finds the slot object [BSA+0x1f0 + 8*slot] (null: returns 2; "request set this frame" bit: returns 3). The same
   effective verb without force restart refreshes the stored request (no restart).
5. Otherwise starts the action (main+0x50c36c): builds the root name `<formKey>_<ActionVerb>:<variation>` (then
   `<formKey>_<Verb>`, `<Verb>:<variation>`, `<Verb>`), looks it up in the slot graph's command table, writes the root
   index ([ctrl+0x19c] and [ctrl+0x1aa]), zeroes the action's age; no root found falls back to verb 40 Wait. The brain
   verb is not read there.
6. Stores the request in the slot, then ticks the slot's graph: **the call of main+0x50b404 at main+0x50afe0 is the only
   place a slot's action graph is ever ticked from** (the other eight callers of main+0x50b404 tick other units).

The slot object (0x80 bytes; **read**): +0x00..+0x5f the last request; +0x60 time since the action started (advanced by
dt times the rate at +0x68); +0x70 the slot's graph controller; +0x78 status (4 after a request); +0x7c bits (0 action
started, 1 request set this frame, 2 locked). The calc clears each slot's "set this frame" bit every frame. An action
starts at frame 0; an equal request does not restart it. The BSA tick ORs each slot's verb attribute word
(BSA+0x4e0 + verb*0x10 + 8) into the controller's physics flags at [comps+0xb0]+0x90.

### 3.4 Layering: decide versus act

The deciding units (brain, utility, scheduler with its action sequence, interrupt sequence, move control) only change
slot requests. The slot graph, ticked solely by the request call, does the action. So an action is named entirely by
(form key, effective verb, variation) and carries its parameters in the request; the brain verb is read only by a few
modules (Walk, Turn). **read** (research enemy/gaps/R22 Q1, Q2)

**Measured** 2026-11-17 (spec B.38, B.41; one game asking a copy for its owner's requests on Chuchus, a Yiga
footsoldier and Bokoblins): the slot graph ran every frame of the asked action (500 to 840 node ticks a second for a
Chuchu's JumpAttack), and the node classes ticked were the action's own (the clip, the jump, the facing, the arming
node; the Yiga's rune-ring effect node).

What an action reads besides its request (**read**, research enemy/gaps/R22 Q4; **measured** where said):
- target slot 0 (12 nodes in a Bokoblin's action graphs, 3 in a Chuchu's, 6 in a Yiga's); OneShotTargetClearSlotInfo
  can clear it from inside an action;
- the blackboard (ExecuteBlackboardSet* nodes, and many inputs bound to it); which blackboard the Set nodes write,
  the controller-local [ctrl+0x30] or the actor's, is **open**;
- the character controller's move state: QueryPhysicsCctIsPreLand is true when the move state is ground, in the air
  only when falling with ground close below. **measured** 2026-11-17: a Chuchu's tackle waits at that test, so a
  controller left in move state 1 never reaches its attack node;
- local random draws (counters, Defeat's repeats): cosmetic;
- QueryLastFrameActionVerb: the slot's own previous verb.

## 4. The engine functions an action's outward acts end in

What an action does to the shared world passes through a few engine functions. These are the places a call can be
seen or refused whichever node made it. Sources: research enemy/gaps/00-running-notes (the list), G8, R13, R16, R20,
R22 Q5, enemy/verify/06; spec A ("Shots"), B.9, B.13, B.14, B.17, B.35, B.36, 5.4.

| Act | Function | Arguments, what it does | Mark |
| --- | --- | --- | --- |
| Make an actor | main+0x599670 (create request) | x1 &name, x2 the create argument block, x3 the handle cell. 75 callers, most of them not enemy spawners (weapons, drops, projectiles, pouch, horses, props); tell them by return address. Layout of the block: engine.md | read |
| Make equipment | main+0xaf100c | (component, slot, &name, w3, initial life, initial extra life, &attachment name, fade, ex-effect type, ex-effect value, flag, recorded extra life, flag, attachment life); calls the create at the owner's pose | read |
| Equipment mode | main+0x4f59ec(component, slot, mode) | 0 drop where it is (the drop record main+0x1b738e4 with a NaN transform), 1 draw (main+0x4f5700), 2 put away (main+0x4f5194); 1 and 2 make, delete and write nothing outside the holder (research enemy/gaps/R24; older notes gave 0 as "create") | read |
| Drop a held thing | main+0x1b738e4 | a drop request with its velocity record (record +0xc0..+0xdc) | read |
| Throw a held thing | main+0x1b73be4 | the same shape; the 4th and 5th arguments are flags (bit 0 of each); the 0x30-byte request is copied into the slot (+0xc0..) | read; what reads it there is open |
| Remember a dropped or thrown weapon | main+0x1ce23c0 | so the enemy can fetch it again | read |
| Remove a held thing | main+0x1b70a04(component, slot, flags...) | resets the slot; kills the thing with death cause 12 (Unequiped) or deletes it; also kills the fused attachment | read |
| Pick a weapon up | main+0x1d71dfc(cell holding the weapon's ActorLink, picker, 1) | the weapon must resolve and be equippable; the slot comes from the weapon's own equipment component; forgets the slot's memory and equips with the draw mode and the Equip effect; the return says success | read |
| The AI's memory of a weapon | main+0x1bb16a0(memory object, link, kind 2 or 3, -1) | stores what the search remembered; no pick-up | read |
| Lift an actor | main+0x1ba6a20 (request), main+0x1ba75d0 (at once) | (lifter, reference to the object, pointer to the hold key's name, flag); held state 3 at once for the second | read |
| Throw a lifted actor | main+0x1ba796c(lifter, vec3 *v) | needs phase 3; takes the throw type from the hold key's parameter entry, then calls main+0x1ba7710 with mode 3 | read |
| Let a lifted actor fall | main+0x1ba756c | calls main+0x1ba7710 with mode 4 | read |
| The lifter's release setter | main+0x1ba7710(lifter, mode, param, vec3) | the common state write behind a pick-up (2), a throw (3) and a fall (4) | read |
| Prepare a shot | main+0x1bf9af8 | on a Shooter record (section 11) | read, measured |
| Shoot | main+0x1bfbc44 | the record goes to state 2 with the request's data | read, measured |
| Ask for a projectile | main+0x1bfd6a8 | fills the Shooter's pool for a name made on request | read, measured |
| Put shots to sleep | main+0x1bfa698(S, name hash, index), main+0x1bfa4e0(S, name hash, force), main+0x1bfa87c(S) | one, all of a name, every prepared | read |
| Arm an attack | main+0x1bf20cc(SensorHit component, request) | section 7.2 | read, measured |
| Ask for a death | main+0x1b4b738(D, cause, second cause, flag) | section 9 | read, measured |
| The wrapper for any actor | main+0x1953184(actor, ...) | finds D and calls main+0x1b4b738; an enemy's life reaching 0 does not go through it | read |
| Drops | main+0x1b4cfac -> main+0x1b5599c -> DropMgr_drop main+0x1c77af8 | items created through main+0x599670 inside main+0x1c78060; reserve path main+0x1b55ad8 | read, measured |
| Kill flag | main+0x195321c (placementSetDead), main+0x1953344 (the not-kill row), main+0x19533e4 (revive) | section 9.4 | read |
| Defeat counters | main+0xb28394 | section 9.5 | read |
| Delete an actor | main+0x63b6b0 -> main+0x63baa8 | the AI node OneShotGenericDeleteThisActor (calc main+0x2534c54) passes reason 0, OneShotGenericFarDeleteThisActor (main+0x2534d0c) passes 3 (engine.md) | read |
| Ragdoll start | main+0x1bddcb0(rag, request) | section 8 | read, measured |
| Ride, get off | main+0x502e60(rider, mount handle, type, seat), main+0xc448c8 | horses-animals.md | read |
| Noise | ExecutePerceptionEmitNoise (enter main+0x239a8f8), ExecuteEmitNoiseAction (main+0x22cd8b4); the manager's producers main+0x1f40d34 (noise) and main+0x4cac5c (terror, about 22 callers) | section 6.2 | read |
| Messages to other enemies | the Discover module's sends (main+0x1857c40); the camp relay (section 13.4) | | read |
| Death message to linked actors | main+0x1b6f6a0(component, cause) | message 0x6a0e0000 to up to 12 linked actors at death start | read; its receiver is open |
| Effects and sounds by key | ELink main+0xa98cf8, SLink main+0xa98d00 (user, key) | section 12.4 | read, measured |
| Magic weather | main+0x21cdf44(manager, type, reserve, actor) | section 12.5 | read, measured |

Nodes that end in these, as named in the graphs (**read**, research enemy/gaps/G8, R22 Q5): ChangeState
main+0x252605c, CreateDynamicEquipment main+0x2526258, RequestDrop main+0x2527818, DropAll main+0x2527c64, Throw
main+0x2528cc0, EquipAndAttachmentByLevelSensor main+0x25235a4, LifterThrowObjectByKey main+0x2541a60, the basket
drop main+0x25aee5c, OneShotShooterShoot main+0x258e1d4, the shockwave main+0x258ecd8, the weapon's ground shockwave
main+0x22d6f18, OneShotDeathRequestDeath main+0x251722c, ExecuteDeathSignalDeathProcedure (enter main+0x227d3d8; its
body moves the death from phase 3 to 4), ExecuteDamageKnockBack main+0x227bb58, TeleportMove main+0x24cf148,
OneShotRiderGetOff main+0x2570240, OneShotLifterRequestDeathLiftedActor main+0x2540ef4, the chemical switches
main+0x25224dc and main+0x250efb0, ExecuteLifeSetPreventGaugeRequest main+0x235835c, OneShotLifeSetOwnLifeIfNotDead
main+0x2540898. Node enter addresses in older notes were one row off in the node map; these are from each node's own
row.

Component paths that make or drop without any node (**read**, research enemy/gaps/G6): the equipment re-create
main+0x1b2bf1c (from the calc; actor [x0+0x18]), the unequip replacement main+0xb299bc, the drop manager, and the
Shooter.

Corrects PLAN.md step 2 and research enemy/gaps/G8, which give main+0x1bb16a0 as the pick-up and main+0x1ba7710 as
"lifter throw": the pick-up is main+0x1d71dfc (research enemy/gaps/R16), and main+0x1ba7710 is the release setter
shared by pick-up, throw and fall (research enemy/gaps/R20); a throw's entry is main+0x1ba796c.

### 4.1 Refusing them while a copy's action runs

What each caller an action's node reaches does when the call is skipped, and the answer that keeps it going (**read**,
research enemy/gaps/R24 and R25: each function's callers read after the call; two spot-checked by the lead,
main+0x4f59ec and main+0x25a0d2c). A node's result word is node+0x18: | 0x80000000 done, | 0xc0000000 failed, neither
still running.

| Function | Skip and answer | Why that answer |
| --- | --- | --- |
| Create main+0x599670 | x0 = 0; u32 at the caller's [sp+8] (error code out) = 1 if not null; the handle cell (x3) untouched | the game's own "manager closed" path; every node-reached caller ignores x0 or finds the handle empty |
| Equipment create main+0xaf100c, static equipment main+0xc4961c | x0 = 0, at the entry (inside them is too late: they write the slot's bookkeeping first and set +0x4600 bit 45 on a failed create) | every node caller ignores x0; the copy gets the owner's equipment by the held-things rule |
| Equipment mode main+0x4f59ec | mode 0 only (or leave it to the drop record) | modes 1 and 2 change the copy's own body |
| Drop record main+0x1b738e4, throw record main+0x1b73be4 | void | they only write the slot's request; with flag A they also call main+0x1c95440, which writes the placement's game-data flag |
| Remove main+0x1b70a04 | void | deletes the held weapon and kills its attachment |
| Pick up main+0x1d71dfc | x0 = 1 (OneShotTargetEquipTarget is then done, as on the owner) | 0 is also safe but fails the node |
| Lifter throw main+0x1ba796c | x0 = 1 (0 is the game's "nothing held") | its only enemy node is denied |
| Shooter prepare main+0x1bf8df4 | x0 = 0xffffffff (no record) | refuse at the entries, not main+0x1bf9af8 / main+0x1bfbc44, which the shot replay calls |
| Shooter shoot main+0x1bfbaa4, main+0x1bf8f74, main+0x1bfbb28 | x0 = 0 | "no prepared record" |
| Shooter sleep main+0x1bfa698, main+0x1bfa4e0, main+0x1bfa87c | x0 = 0 | the owner's sleeps come by the replay |
| Death request main+0x1b4b738 and its wrapper main+0x1953184 | x0 = 1 ("started") | the copy's death comes from the owner's; 0 is also safe |
| Drop now main+0x1b55b48, drop reserve main+0x1b55bb4 | void; x0 = 0 | drops a node makes without a death (OneShotDropDrop, not denied by class) |
| placementSetDead main+0x195321c, the Bool64 setter main+0x2ab6dcc | void | kill flags and game data a node writes |
| Noise main+0x1f40d34, terror main+0x4cac5c | void | not carried by the sync: heard only where made |

The mod's own replays on a copy (equipment create, draw and put away, the throw, the shots and sleeps) run in
`en_copy_precalc` before the action call, so a refusal keyed on the action's thread mark never touches them (**read**,
R24, R25). Node classes that reach these and are not in `coop_enemy_act_deny.inc` (R24): CreateDynamicEquipment-
IfUnequipped, CreateStaticEquipment and ...FromBB, EquipAndAttachmentByLevelSensor, EquipStandControl, OneShotDropDrop,
ExecuteGenericRequestCreateActor, OneShotTargetEquipTarget, ExecuteOctarockStoneKeepPredation (deny it whole: its
refused pick would still let its holder's entry go). Open (R24, R25): who reads the equipment user's +0x4600 bit 45 and
+0x4749; the ActorHolder entry's create; the lifter's actor offset; ExecuteShooterPrepare's leave; the answers of two
sleep entries.

## 5. Animation of an enemy (was: spec 7.3, enemy-design 6.2-6.4)

### 5.1 The update and its play request

- AS_update main+0x432fa0 (from the calc at main+0x41ca30; gated by the pause mask) turns commands into clip slots,
  overwrites the model's slots on every call (main+0x441970) and raises the animation events. **read**
- The play request **main+0x4dc628(request, mode)**: request = {actor, name pointer +8, slot u32 +0x10, rate +0x14,
  blend +0x18 (to S+0xd8), "start" +0x1c (overrides the new state's transition duration, S+0xec; no start frame),
  ignore_same +0x20, +0x21, +0x22}. With ignore_same and an equal name it does nothing. Otherwise rate goes to S+0xd0,
  blend to S+0xd8 when >= 0, and the command and event words change at once; the pose changes at the next AS_update.
  A null node pointer is accepted. **read**; **measured** 2026-11-15 (run 8: the command changed at once and the clip
  started in that frame's update).
- Every animation play of AI nodes and native modules goes through main+0x4dc628 or its two older forms
  (main+0x2935064, main+0x2935084). Every start ends in main+0x4dca24, of which main+0x4dc628 is one of 23 callers (the
  others unread). Both statements hold (C7: not a conflict). Native modules pass slot 0, rate 1.0, blend -1, start -1,
  ignore_same 1 (Defeat's repeats pass 0, a restart). **read**
- Slot removal main+0x2934934; partial changes main+0x29361b4, main+0x29362e4, main+0x2936214. The model slot setter
  main+0x1b569fc sets an absolute frame. The engine's own slot clone is main+0x2932418. **read**
- Commands are names: walk, run and turn are separate commands (a Bokoblin's ASB has no speed input). **read**

### 5.2 Slot state and clocks

- Slot state S = [G+0x20 + i*0x48]: command S+0x10, rate S+0xd0; X = [S+0x50] holds up to four cross-fading
  controllers ctx_k = [X+0x28 + 8k], the current index byte X+0x95; per controller the clock f32 ctx+0x68 (advances by
  dt times the rate, wraps at 1e6), weights ctx+0x6c, +0x78, +0x84 (advanced by dt, not by the clock), flags ctx+0xa0
  (bit 10, 0x400: do not advance in this update, seen in main+0x433c24). **read**
- The clock counts 30ths of a second (0.5 a frame at 60 frames a second). A clock written to 0 with the no-advance flag
  read 0 after that update and counted on from there. Rate 0 freezes the clock (hit-stop) but not the blends.
  **measured** 2026-11-15, run 8.
- S+0x1e4 is a random value in [0, 1) drawn at every slot update and every command start (main+0x433c24,
  main+0x4e1bdc; the global xorshift128); a clip started from outside ran from its first frame whatever its value.
  **measured** 2026-11-15, run 8. Corrects research enemy/gaps/R4, which read it as a start-delay countdown. What the
  animation graph uses it for is **open**.
- Time: the animation dt is the calc's dt scaled by the per-actor time rate actor+0x280 (hit-stop, bow slow motion,
  flurry rush) and by [[comps+0x40]+0x98]. **read**
- The owner plays most commands after its AS_update (shown from its next frame). **read**

### 5.3 Events

- AS_update ORs the slots' event words into G+0xb8.. (trigger) and G+0xc8.. (hold). Hold event ids (name list
  main+0x3713428): 0 AtCollision, 1 ShieldAtCollision, 2 Ragdoll, 3 JstAvdRec, 5 Guard, 10 Trans, 11 Rotate, 17
  WeaponBlur, 0x38 JustGuard, 0x3e Dodge, 0x40 OverrideReaction. **read**
- Events are raised by the animation update alone: hold words went on and off with the clip while no graph ran.
  **measured** 2026-11-15, run 8.
- Native consumers found: the attack arming main+0x42ec98 (section 7.2) and the bone control main+0x434fb0. Consumers
  of sound, effect, trail, JstAvdRec and Ragdoll events: **open**.
- Writing a slot's clock (ctx+0x68) does not move its events: a copy's AtCollision came when its clip had played as
  far as the owner's had at its arm, counted from the copy's own start, whatever its written clock said (77 of 77 armed
  episodes in three camp fights). So a copy started late and caught up by a clock jump arms its attack late by that
  much. **measured** 2026-11-17 (research enemy/gaps/R26, from the mismatch checker's records). Why the update keeps
  event time apart from ctx+0x68, and whether main+0x1b569fc ("sets an absolute frame", section 5.1) moves both:
  **open**.

### 5.4 The animation blackboard

B = [G+0x40], one per actor: name to id vt+0x290, set f32 by id vt+0x70, by name vt+0xa8, enum vt+0x28, bool vt+0xb0,
get vt+0x170; floats enumerated by vt+0x260 (count) and vt+0x1d0 (name). It has no native writers. Enemy writers found:
DirectionAngle (12 nodes ExecuteASSetDirectionAngleToTarget in turn and sidestep graphs; the ragdoll and recover
modules, which also write DirectionAngleY), Gear (riders), StickX (block construct, Yiga rider). ASPlayRate* is
parameter data. Writers of Distance, Random, SubsTgHitHeight, IsUnderwater, ShieldEquip and others: **open**. **read**
(research enemy/gaps/R2; corrects G5 and verify/09, which said AI nodes write StickX, UserSpeed and GroundNormal for
every enemy). Continuous inputs across 20 enemy ASBs: DirectionAngle, GroundNormal, StickX, UserSpeed, ASPlayRate*.

### 5.5 Looks that are not commands

- Look-at: arbiter A = [comps+0x318]; activate main+0x1bae4ac(A, request), pause main+0x1bae5f0(A, bool, id),
  interrupt main+0xc061cc(A, mode, id). The target is target slot 8 (TargetForLookIK). **read**
- During ragdoll and recovery the animation player's flag G+0x124 bit 5 is set. **read**
- Bones: a Bokoblin has 76, a Chuchu 25. **read**

## 6. Perception and targets (was: spec B.6, B.21, B.22)

### 6.1 The manager and the emitters

- The manager [main+0x3a6ea80] is a singleton with a pool of 0x180 emitter records of 0x268 bytes: +0x04 affiliation,
  +0x07 sublayer, +0x10 perception id, +0x1f8 an ActorLink to the actor. **read**
- **Perception ids are allocation serials and differ between games.** **read**
- Affiliation (emitter +4) is copied once, at the emitter's registration, from the actor's affiliation component
  ([[comps+0xf0]+0x28]). Values: 1 Enemy, 2 EnemyZonau, 4 EnemyLikeLike, 6 Player, 7 Sage, 8 Friendly, 10
  PlayerCreated, 11 EnemyAssassin. Hostility (main+0x442ea4): blocked when the enemy's u16 +0x48 mask has that bit,
  else true when the id is in TargetAffiliations (mostly EnemyZonau, Player, Sage, PlayerCreated). **read**
- Every emitter makes a unit sensor sphere on layer PerceptionEmitter in its actor's physics controller set
  (main+0x943620), so it moves with the actor; very likely what the sight query hits. **inferred**
- Resident emitters (data, every pack): the Player, the Sages and their puppets and Dm_ versions, two Gerudo
  dungeon-battle zombies. The manager update main+0xe9b070 keeps them in a list (+0xc88 count, +0xc90 array). **read**
  The old note "manager +0x58 = the resident player" is doubtful.

### 6.2 Events: terror and noise

Terror and noise go through a ring in the manager (+0xd98..+0xdac): producers main+0x4cac5c (terror) and
main+0x1f40d34 (noise); the manager update drains it. An enemy's noise is heard only by actors of the game that made
it. 154 enemy kinds emit noise from their graphs. **read** (enemy-system "Perception"; research enemy/gaps/R3)

### 6.3 The receiver and the sight test

The receiver update main+0x45b930 rebuilds each enemy's candidate pool from zero every full update. Sources in order:
residents (only while receiver flag bit 3 of u16 +0x34 is set; no range or sight test), this frame's events (hearing,
terror), the registry (bit 5), the box query (bit 4: main+0x49f5f8, a physics box of half size 0.5 times the largest
sense range on a sensor layer, plus the receiver's own sensor contacts), carry-over, the previous pool. A per-sense
filter (main+0x451f54) checks the sublayer mask, a sense level above 0, a channel mask and a range sphere. The sight
test main+0x1f41c90: affiliation, range, angle, vertical band, then a ray (visible when it hits nothing). **read**
(research enemy/verify/02)

The sense lists are unsorted, in the order the sensor world's box query writes its hits; that order is re-derived
every frame and its stability from frame to frame is unproven. **read** (research enemy/gaps/G2)

What an actor needs to be seen as a player: an emitter with affiliation type 6 at registration and sublayer
PerceptionPlayer, not resident, a sense level above 0, kept in the sensor world all the time, a live transform and a
physics controller set; the target slot's kind byte must keep matching its affiliation or "time since seen" runs.
**read** (research enemy/gaps/G2); **measured** 2026-11-07 (an actor built so was noticed).

### 6.4 Target slots

- Slot k = [comps+0x4b0] + 0x28 + k*0xe8: an ActorLink (+8), the target emitter's perception id (+0x84), actor id
  (+0x8c), type (+0xe2: 0 empty, 1 position, 2 actor), kind byte (+0x93), time since seen (+0x80), flags. Never an
  address. **read**
- Writers: main+0xa70734 (41 sites) and main+0xa70a1c (15 sites; Slot_setFromActorLink(slot, link, options): resets
  the slot and takes the kind byte from the target's emitter; a target with no emitter is not written). Clear one slot:
  main+0x83be9c. The game's reset of the slots: main+0x24f0a9c; the brain's reset main+0xa12a48. Adopter
  main+0x1866bf4. **read**
- Slot names: 0 Main, 1 TargetPosForMovement, 2 Sub, 3 Throw, 4 Escape, 5 Eat, 6 Investigate, 7 MetaAI, 8
  TargetForLookIK, 9 Companion, 10 Hunt, 11 Chemical, 12 AIScheduleTarget, 13 and 14 SubBattleTarget, 15 RideTarget,
  16 NextAttackTarget, 17 BattlingAlly. **read**

### 6.5 How a target is chosen

- Target modules pick from the receiver's lists, never from a global player: EnemyCommonTargetSearchAndSet (core
  main+0x1866e14), the ByAffiliationList module (update main+0x2748258), the shared search main+0x2741ac4. Each
  gathers up to 0x80 candidates (more are cut off in list order) and drops those the filter nodes reject (affiliation
  filter main+0x9755a4). No filter or scorer node is wired into any native search module in any of the 1956 graphs.
  **read**
- The notice search takes the **last** passing list element. **read** (research enemy/verify/03; corrects
  enemy-system "Targets" item 2, "the backward loop meets first" being the last in list order)
- ByAffiliationList scans ten states in priority order; the first that returns nonzero wins: 0 confusion (last
  attacker), 1 damage (attacker as target if seen, else its place and Investigate), 2 a friend's payload, 3 and 5 the
  first hearing entry (Investigate and an EnemyCoop message 0x3ddc000a), 6 a friend's alert (0x3ddc0007; its place
  into slot 6), 8 terror (verb 0x35), 9 the search (the last passing sight entry into the module's slot, slot 6 for a
  Bokoblin; verb 0x36). **read** (research enemy/gaps/G2)
- In the Bokoblin graphs EnemyDiscovered picks the target by perception id from the sight list, with the id from slot
  6, and copies it into slot 0; LandBattle and BattleCommon then run SwitchBattleTargetIfNeeded. **read**
- **SwitchBattleTargetIfNeeded** (base main+0x18658f8; entries main+0x18669fc, main+0x1866aa0): each frame it looks at
  the **first** sight entry (a native comparator: group preference, affiliation rank) and replaces the target when the
  target is dead or flagged, or when the candidate is inside the sight radius, at least as near, and either the target
  is beyond 15 m or the candidate is within 20 degrees of facing. The last attacker (Life+0x1170) is adopted only when
  not visible; earlier battle targets are recovered from registry key 4. It writes slot 0 only, reads no global player
  and weighs no damage. The old target's distance uses the slot's cached position. **read** (research
  enemy/verify/03, enemy/gaps/G2; enemy-system called it "not decoded")
- With two player emitters, a nearer second sight entry is never tested, and equal distance or the 20-degree cone can
  make slot 0 swap back and forth between frames. **read** (inferred from the rule; research enemy/gaps/G2 7)
- LoseSightOfTarget (core main+0x185c65c): no timer, distance or ray; it reads the slot's time since seen, which rises
  only when the target link is invalid or its affiliation changed; it writes slot 6 and reports lost sight, and does
  not clear slot 0. **read**
- Force battle is the one place that deliberately takes the resident player: the graph
  `EnemyCommon.brain.waitbeforeforcebattle` sets receiver bit 3 (main+0x2552530), searches sight into slot 0, then
  clears it; it runs only when the map-logic plug IsStartForceBattle is true (some Bokoblin, SocialEnemy and underwater
  Lizalfos wait brains; Lynel and Stal giants). **read**
- main+0x1fc9f6c is a generic link-holder resolver; six callers pass the local ResidentActorMgr (scene module 13),
  that is, "the local player". Enemy users: the cave-boss family (QueryGetResidentPlayerPos) for sure, a Chuchu die
  module probably. **read** (research enemy/gaps/G2; settles the disagreement of verify 07 and 08)

### 6.6 "In battle" and the enemy-to-enemy alert

- "In battle" is not a flag: it is the brain verb (12 land battle, 6 discovered, 18 wander, ...) together with target
  slot 0 naming a live actor (kind 2). Graphs clear battle memory whenever the brain is not in battle. **read**
  (research enemy/gaps/R10; was: spec B.6)
- A brain whose graph starts afresh (first tick, or after a take-over) picks an idle verb in its root's Enter (1, 2, 8,
  18 or 19), notices the player again and plays its Find clip. **measured** 2026-11-17 (a Wizzrobe taken over: verb 0 to
  18, then 6, with a Find clip).
- An enemy with a stand-in in slot 0 fights it: it is seen every frame and attacked. **measured** 2026-11-17 (a Fire
  Wizzrobe; after 12 s its own brain changed to the nearer Link).
- Asking a verb with force runs the game's own procedures for it (in battle: the head's look-at at slot 0), and with
  that the battle-info flags (section 12.3) come out right. **measured** 2026-11-17 (was: spec B.21).
- The alert between enemies is the EnemyCoop message (id 0x3ddc0007: target perception id, last position, loud-noise
  flag), carried in the AI blackboard; enemy to enemy only. **read**

### 6.7 Global reads in enemy graphs

Over all enemy packs: the resident-emitter switch node in 71 actors (force battle and sleep graphs);
QueryPerceptionIsPlayerDiscoveredBySenseType in 61 (reads the enemy's own receiver); camera position and direction
reads in 36 to 46 (every dungeon boss, Yiga, dragon; EventWarpBehindCamera); FixPlayerLookPos in 24;
OverwriteGlobalPlayerNoAttackTime in 68 (a game-balance value). Other global writers: the game-balance manager 180
actors, enemy-to-enemy messages 177, camera shake and rumble 50, boss gauge, notice UI and boss save position 213,
weather 6 (Wizzrobes), game-data writes 7 (Gibdo, Ganondorf's miasma form). No graph node asks for battle music: a
native manager does (parameter list CommonEnemyBattleBGMBrainVerbs, loader main+0x1d38168; consumer **open**). **read**
(research enemy/gaps/G2 5, R3, R4)

## 7. Hits and damage (was: spec 8.1, 8.4, B.15, B.27, B.35)

### 7.1 The hit list and the damage step

- On contact, the victim's SensorHit::calc (main+0x303be0) filters the hit (main+0x1a52548) and copies a 0x1c0-byte
  hit entry into its list. The list is cleared only at the start of SensorHit::calc. Build-time rejection rules run
  only there. **read**
- The list: [[L+0x1910]+0x50], capacity +8, entries +0x10, count +0x18; the writer wraps (index at or past the capacity
  goes to 0 while the count keeps rising), so "every entry" is min(count, capacity). The actor is [L+0x198]. Release an
  entry the game's way: main+0x1a550d4. The link assign: main+0x1b0614. **read**
- The damage step **Life::calcDamage main+0x300a30** (x0 = L) runs on every path; main+0x315dd4 is conditional. The
  earliest safe point to add an entry is right after SensorHit::calc returns; an added entry must be removed after
  calcDamage, or a skipped frame counts it twice. **read**
- The external damage calculator: EDC_accumulateHitEntry main+0x1cd263c, EDC_calc main+0x314b68 (the first kind-1
  entry becomes L+0x1a8), main+0x1cd0c80 (entries of kind 0 or 3 and up). Internal sources (burn ticks and the like) go
  to L+0x1230. The life update main+0x4400b0 adds both into L+0x18e8. ExtraLife takes damage first; BreakLife caps life
  at maximum minus BreakLife. **read**
- The step also applies damage that is not in the list: impacts ([L+0x1900]+0x120) and condition damage (bits of
  L+0x1780). **read**; their detail is **open**.
- Frame timing: a hit entered in frame N gives the calculator fields in phase A of N; the reaction selector picks the
  verb in the calc of N+1 (the AI run), and the life update applies the damage later in the same calc. **read**
  (research enemy/gaps/G7). **Measured** 2026-11-15: the damage is taken at the next calc, not in the step.
- A copy's own damage step left alone takes damage; the life falls. **measured** 2026-11-15 (run 11).

### 7.2 Arming an attack

- **main+0x1bf20cc(component, request)** arms an attack slot on the attacker's SensorHit component; it calls the slot
  writer main+0x1bf327c (power with the attacker's modifier S+0x130, rate +0x134, type +0x138, level +0x13c, attributes
  +0x140, ids from +0x178; the armed bit is SC+0x28 |= 1 << kind). Parameters come from the actor's AttackParam data,
  so they are equal in every game; ids are minted at arming (one swing, one id). 23 callers. **read**
- An attack is no held flag: the call is made again every frame while the attack is on, and the component's calc drops
  the armed bits each frame. The request is 0x24 bytes with no pointer; its padding is stack leftovers. **read**
  (research enemy/gaps/R20-sensorhit; was: spec B.35)
- Two routes call it: the animation's own poll **main+0x42ec98** (only caller: the calc at main+0x421a7c), which reads
  hold event 0 (AtCollision) of each slot and two int parameters per event entry, gated by [attack component+0x22]
  bit 1; and AI nodes: ExecuteSensorHitSetupAttack (enter main+0x2444d74, update main+0x2444e84, leave main+0x2444e9c;
  enter and update both call main+0x2444d80, which calls main+0x1bf20cc, the update every frame while the node runs)
  and about twenty other callers. **read** (spec B.3's "main+0x2444d80" is that helper, not the node's enter)
- By enemy (research enemy/gaps/R3, R19; **read**, data): animation event only 148; both routes 34 (Lynel, Like Like,
  Wizzrobe, Hinox, Frox among them); node only 17 (the plain, fire and ice Chuchus, the Rito dungeon boss, the Keese
  swarms); neither found 22 (the electric Chuchu, Octorok, the Goron mid-boss, bone heads). A Bokoblin's basic-attack
  graph has no AtCollision query.
- **Measured** 2026-11-17: a Chuchu's attack node, run from its action, made the arm call (from main+0x2444e70) every
  frame of the attack with the same request bytes and the same component flags and layer as in the game that ran the
  enemy. The armed bits of the SensorHit component (+0x20, +0x21) read 0 at the node's arm call even while blows land:
  they do not tell armed from not.
- Held values on the component: the attack layer override (byte +0x23, 0x16 none) and the "hit-receive sensors off"
  reasons word (+0x26), switched by main+0x4e0edc (on, when the word was 0) and main+0x4f2384 (off, when it becomes 0).
  **read** Not read: the eat layer, the confused enemy's sensors, the all-sensor mask, the force-no-hit map
  [comps+0x258].

### 7.3 The hit entry

Fields that matter (**read**, research enemy/verify/05 6, enemy/gaps/G7):
- power +0xa0, rate +0xa4, type +0x90, intensity +0x91, attribute bits +0x1b0, attack id hash +0x1b8, kind +0x08,
  vectors +0x0c, +0x40, +0x4c, +0xa8, +0xb0, and +0x93, +0x96, +0xac;
- game-local: the two ActorLinks +0x60 and +0x78, the body pointer +0x58, the attack id +0x38 (with +0x3c), the stealth
  id +0x1a4, the contact copies +0x18 and +0x28;
- the guard class byte +0x0b, set by the classifier main+0x1a54430 from the **defender's** own state: 0 a hit, 1
  guarded, 3 guarded at the right moment (bit 1 is the just-guard bit), 4 avoided (the defender has a Dodge event or
  flag). Just-guard comes from bits 10 and 12 of the defender's equipment-user word +0x4600. Only the defender's game can
  decide it.

The hit delivery main+0x31dbc0 builds these bytes (its full byte map: research enemy/gaps/G7 2.1).

### 7.4 The damage formula

dmg = floor(M * (bonus + power)); then a minimum for some victim classes (a fraction of maximum life); then
max(1, dmg - defence). M multiplies the entry's rate, a per-type rate, per-attribute rates and special rates (critical,
stealth, shatter, impact, ...). Several attribute bits are re-derived from the victim, so the same entry gives the same
damage in another game only with the same victim state. No separate guard, headshot or weak-point multiplier is in
this path. **read** (enemy-system "Hits and damage")

### 7.5 Reactions

- Bsa_calcInterruptVerb main+0x619710 (the selector SelectorBSAInterruptAction: calc main+0x616ca0 ->
  main+0x616e7c -> main+0x619710) looks up the ExternalDamageReactionTable or GuardReactionTable by the damage type
  (entry +0x90, kept at L+0x253) and intensity (+0x91, L+0x254) as `<Type>___<Intensity>`. Overrides: stealth (attribute
  bit 13), critical (bit 12), weak point (bit 11), broken armour (bit 27), fall. The damage amount is not in the key.
  **read**
- Reaction verb ids: 0x97 small, 0x98 medium, 0x99 huge, 0x9a smash, 0x9e helmet, 0x9f die, 0xa0 guard, 0xa2 guard
  break, 0xa4 just repelled, 0xa5 weapon drop, 0xa6 repelled, 0xb1 drown, 0xbc down. Conditions from the same selector:
  burn 0xa7, frozen 0xa8, frozen break 0xbd, paralyse 0xa9, wet 0xab. **read**
- Knock-back, ragdoll, weapon drop and the ice effect of a reaction are nodes of that verb's module. **read**
- An entry with a launch: a whole hit entry of another game's player, added to an enemy's list, launched it into the
  sky at every blow it survived. **measured** 2026-11-17 (spec B.10). Which field does it is **open**.

### 7.6 What the attacker learns: the second list, the guard type

- An attacker learns that its blow was guarded from a second list on its own SensorHit component ([comps+0x428]+0x58,
  entries of 0xa8 bytes, filled by the component's calc from the frame's contacts): class byte +0x0b, bit 0 guarded,
  bit 1 guarded at the right moment. Its graphs ask whether any entry of the frame has it (QueryIsOnHitFlagGuardJust
  main+0x2686148; main+0x2632958) to choose the repelled reaction. One reader of the list follows a handle in the
  entry. **read** (research enemy/gaps/R17; was: spec B.27)
- The attacker also reads its own life record: guard type int L+0xfc and bit 1 of L+0x104 (QueryLifeGetAttackerGuardType
  main+0x2695814). Who writes that record is **open** (copies through main+0x1ccecb4, main+0x1cd4218). **read**
- After a guarded blow a node sets the attack pause: the float [[comps+0x268]+0x3c] = 10 + 5 times a random number
  below 1. **read**

### 7.7 The flurry rush

The dodging player's game decides it from an entry for its game-balance client in the table at
[[main+0x395e090]+0x31a8] with kind 4, 5, 6 or 10 (trigger main+0x23ba3b8, test main+0x1d0a990). That list is the
limiter manager's "kind 1" list (request main+0x4f1a84), asked for by Sage and Ganondorf nodes only. Ordinary enemies
set a swift-attack bit, bit 7 of the word +0x34 of their game-balance client [comps+0x268], by
OneShotGameBalanceSetSwiftAttack (68 nodes). What reads that bit on the player's side, and what slows the enemy during
the rush: **open**. **read** (research enemy/gaps/G7, R4 5, R18; was: spec B.31)

## 8. Ragdoll (was: spec 6.6, B.7, B.18)

- The component rag = [comps+0x3e8]; state int rag+0x20: 0 none, 1 recovering, 2 partial (a flinch), 3 full. 143 of
  221 enemies have one; 78 have none (Chuchus, bosses, Keese and others). **read**
- Start main+0x1bddcb0(rag, request {subtype, list name, key, strength, impulse, ids, +0x32, +0x33 partial}): hands
  the controller's follow target to the ragdoll body. Recover main+0x1bdf2b4, end main+0x1bdc9a0(rag, 0). The
  controller-follow byte is [C+0x38]+0x9d. **read**
- **The flinch is native**: when the damage step works out damage of 1 or more, the life's damage handler sets a
  request bit (rag+0x149 bit 0); the ragdoll update (main+0x432884) then starts a partial ragdoll in the hit's
  direction (main+0x1bdd810 -> main+0x1bddcb0, request byte +0x33 = 1, state 2) and ends it by itself. **read**
  (a review's reading, spec 6.6); **measured** 2026-11-15 (runs 11, 12: a hit enemy flinched with every graph off and no play
  request made). Corrects the guess that a "reaction component" plays it.
- A partial ragdoll on an actor whose main body is static and whose root is written throws the drawn model until it
  ends. **measured** 2026-11-17 (a tester saw a Bokoblin's model soar and snap back; the recorded places stayed on the
  ground).
- The model phase (inside main+0x245030) reads a ragdoll's bodies back into the pose (main+0x2752c8), turns the pose
  into the skeleton's parent-local records (main+0x289620) and builds the final matrices from them (main+0x24cc30,
  only for a model marked changed: bit 0 of model+0x240). The modifier chain main+0x34526c runs main+0x289620,
  main+0x2925e0 and main+0x43e450 (LookIK, FootIK, ragdoll blend; identities not decompiled); then main+0x283c750
  recomputes world matrices. **read**
- A copy's model matrix (model+0x1f8) was found stale, 5.5 m from its actor, when nothing pushed it. **measured**
  2026-11-17.
- The get-up is an ordinary animation command plus the DirectionAngle value the recover module writes. **read**

## 9. Death, drops, kill flags, the blood moon (was: spec 9.2, 9.3, 9.4, B.9, B.12)

### 9.1 The request

**RequestDeath main+0x1b4b738(D, cause, second cause, flag)** only starts a death. In order: sets bit 15 of the state
word (unless flag bit 0); refuses when blocked (main+0x1b4c308: D+0x2c bit 7, and extra rules for causes 16 and 13);
returns 0 for a cause already committed; looks up the cause's row in [D+0x78]+0x150; a row that ignores this cause
returns 0; for a non-player actor whose row has DeathProcedure 2 or 3 and no wait for a signal, asks the actor's
delete (main+0x63baa8, actor flag +0x28 bit 17) and returns 1 **without committing** (no drops, no kill flag, no
effect); otherwise rewrites the cause (main+0x1b4c45c), derives the second cause when it is 0 (main+0x1b4c79c, from
local state such as burning, frozen or electric condition slots), applies hit-stop for a kill by the player, sets the
phase to 1 and commits the cause (main+0x1b4fb9c), sets bit 17 of the actor-system flag word and returns 1. A later
request with a lower cause id overrides. **read** (research enemy/verify/06)

- Callers: 46 in Ghidra's list and at least 13 more in the lifted code (the life update main+0x440cfc and
  main+0x440d4c among them). When life reaches 0 the life update asks cause 16 (LifeDepleted) with second cause 0 and
  flag 1 (after an optional AncientWeapon request, cause 2). **read**
- The 21 causes (table main+0x366f8b0): 0 AI, 1 AttachmentUseEffect, 2 AncientWeapon, 3 ChemicalCriticalHitFire, 4
  ChemicalCriticalHitIce, 5 Drowned, 6 Eaten, 7 PickedUp, 8 Exploded, 9 Burned, 10 Frozen, 11 Electrified, 12
  Unequiped, 13 FallIntoAbyss, 14 Boiled, 15 PhysicsKinematicCollision, 16 LifeDepleted, 17 LifeCondition, 18 Melt, 19
  Crushed, 20 ImaginaryAutoBuildedActorDeath. **read**

### 9.2 The state word and the death tick

D+0x20: bit 0 valid; bits 1-3 the phase; bits 4-8 the cause (0x15 none); bits 9-13 the second cause; bits 14 and 15
flags. The AI's "dying" test: phase nonzero and bits 14-15 not exactly 0x4000. **read**

The death tick main+0x1b4badc runs from the calc every frame until the phase is 7 (a second caller at main+0x430fa4).
**read**

| Phase | When | What runs |
| --- | --- | --- |
| 0 -> 1 | the request | the state word only; no drops, flag or counter yet |
| 1 -> 2 (-> 3 with a wait for a signal) | the next tick | stage A (main+0x1b4cad0), once: the message to linked actors (main+0x1b6f6a0, 0x6a0e0000); the kill flag now unless D+0x2c bit 3; the drop reservation; the delay timer D+0x24 unless D+0x2c bit 8 |
| 3 | the Die action runs | the death animation, ragdoll (`EnemyCommon.common.ragdolldie.module`), noise, the weapon drop node, the basket drop node |
| 3 -> 4 | ExecuteDeathSignalDeathProcedure (body main+0x227d3dc) after the Die clip ends | the signal |
| 4 -> 5 | the tick | pre-death effect settings and a timer D+0x28 (none for normal enemies) |
| 5 -> 6 | the tick | waits for the drop reservation (only with C+0x33d6 bit 1) and counts down D+0x24 (0 for LifeDepleted; random 60 to 300 for cause 17) |
| 6 -> 7 | the tick | the death procedure (a delete request, main+0x63baa8), then onCommit main+0x1b4cfac: drops released, death effect, kill flag if bit 3, actor-system bit 17 cleared |

A cause with no wait for a signal goes from phase 2 to 4 at once. How long a body stays is not a constant in code.
**read** (research enemy/verify/06). A switched-off enemy waits at phase 3 (state word 0x8307), and a death's start
sets D+0x2c bit 1 while the enemy still stands. **measured** 2026-11-15 (run 11).

### 9.3 The switch word D+0x2c

| Bit | Effect |
| --- | --- |
| 0 | no drops, including the reservation and the roll's gate |
| 1 | the kill record has been made (a once-flag); set, no kill flag is written |
| 3 | record the kill at the commit (end) instead of the death start; set at main+0xe22848 from a per-placement parameter (key hash 0xb12136af in [job+0xd8]); run-time spawns never have it |
| 7 | blocks every death request |
| 8 | no delay timer |

**read** (research enemy/verify/06, enemy/gaps/G3). Enemy-system listed the setter of bit 3 as not found; G3 found it.

### 9.4 Drops

- DropInfo is at C+0x19f0 (C = [comps+0x1e8]). At the commit: when the row drops items, D+0x2c bit 0 is clear and a
  drop component exists, main+0x1b5599c rolls (main+0x1b55a18, only if the parameter IsInitializeDropInfoWhenDrop),
  checks its gate (bit 0, the manager, [[comps+0x480]+0x150] bit 0 clear), sets the drop's kill flags and calls
  DropMgr_drop main+0x1c77af8 (kind 0), which releases the reservation or makes the items fresh through
  main+0x1c78060 (which calls the create main+0x599670); then C+0x33d6 |= 8. **read**
- For waiting deaths the items are rolled and created inactive at death start (main+0x1b55ad8 -> main+0x1c77d0c,
  parked in a manager slot) and released at the end. A roll also happens at spawn. **read**
- The rolls use global xorshift128 generators with no per-actor seed; among tables of equal name, a time-seeded one
  chooses. **read**
- **Every game that runs the death tick on an actor rolls its own drops**; no game "commits for all". C+0x33d4 nonzero
  makes the drop manager and the reservation bail; C+0x33d6 bit 0 clear also stops the drop, but the roll before it
  still runs. D+0x2c bit 0 is the stronger switch. **read** Corrects enemy-system "Hits and damage" (Drops): "rolled
  only by the game that commits the death" and "+0x33d6 bit 0 is the no-drop idiom" (C4, resolved).
- A "no redrop" save flag: key = placement hash XOR 2; set (by main+0x1b55e80, just before items are made) only when a
  DropParam bool is true and the flag already exists in the table. **read**
- HornMaterial drops follow the enemy name (equal in every game); a Like Like's weapon drops follow the level score.
  **read** (research enemy/gaps/G8)
- **Measured** 2026-11-17: a dying enemy's item (Item_Enemy_77) is made by main+0x599670 called inside main+0x1c78060.

### 9.5 Kill flags, revive, counters

- A dead placed enemy is a GameData Bool64bitKey flag keyed by its placement hash, value 1, saved in progress.sav;
  8853 of 8926 placed enemies have one. **read**
- The kill record (main+0x1b4f348 -> main+0x1b4f390) sets D+0x2c bit 1; if the row says not to clear the respawn
  flag it calls actorClearDead (revive), else placementSetDead **main+0x195321c(actor, flag)**, which sets the
  placement's suppress bit (job+0x98 bit 0), tells the placement's listeners, adds the hash to the dead set, writes the
  Bool64 when asked (placement +0x14 bit 3, a hash, a run-time gate clear) and an older u32-keyed store (Struct
  0xce0c4dfe); then the link sync main+0x1b4b1ac. A row that is not a kill takes main+0x1953344 instead. Revive clears
  through main+0x19533e4. The Bool64 setter is main+0x2ab6dcc (8 callers: sky chests, bee hives, garden crops, the
  cave-boss medal node, OneShotActorChangeIsRespawn too). placementSetDead has 7 callers (the death record at
  main+0x1b4f390, the link message receiver main+0x1953ccc, a signboard node, the depths ghost helper, three
  unidentified). **read** (research enemy/gaps/G3, verify/06, 07)
- Map loading checks the flag in main+0x1fcc070; preActorUpdateDeadFlag main+0x1fe1fd4 (a sequence node at load and
  blood moon) syncs suppress bits from the Bool64. Nothing re-reads the save when a player walks back into range, so a
  dead placement stays away only through its suppress bit. **read**
- The placement update bancSpawnUpdate main+0x65b710 creates when job+0x20 is 0, bit 20 is clear, main+0x66aa70 is true
  and (job+0x98 & 0x840401) is 0; it clears bit 0 on every pass for pending jobs, so a lone set of bit 0 may not hold.
  **read**; which state durably holds a placed actor away without the save's flag is **open**.
- Defeat counters: main+0xb28394 adds, as ADD requests on the Int queue (main+0x2aa0278), DefeatedNum +1 always,
  NoDamage +1 if never damaged, GuardJust, HeadShot and JustAvoid from capped bytes (rec+0xa1..0xa3), when the enemy's
  game-balance client ([comps+0x268], vtable main+0x3673798) ends (slot 20, main+0xe27f24). No "the player did it"
  test. The same function writes a "boss instance defeated" Struct member (0xcd3ec27e) named by the decimal placement
  hash. **read** A copy's death record arms the same writer. **measured** 2026-11-17 (the counter's value not read).

### 9.6 The blood moon

- World object W = [[main+0x395dc20]]. Start main+0x21cd650(W) sets W+0x60 |= 0x100000 (bit 20; the implementation
  is virtual +0x360); done main+0x21cd698(W). The moon forced by a memory shortage is W+0x60 |= 0x80000 (bit 19; node
  enter main+0x1fdc8cc). What decides "tonight" (the object at W+0xd0, virtuals +0xe0, +0xe8, +0xf0, evaluated from the
  world clock at hours 21 and 0): **open**. **read** (research enemy/gaps/R5, G3)
- The reset is applied inside GameDataMgr::calc main+0xa72b00: for each value type, entries whose ResetTypeValue has the
  reset bit are written back to their default directly in the table, not through the request ring. Then
  SeqExecuteUpdatePreActorFlag clears suppress bits and re-creates. **read** (research enemy/gaps/G3; enemy-system
  said "not found")
- Reset masks: type 3 (the moon) clears flags whose mask has bit 3 (mask 88: about 8100 enemies); mask 208 is the
  Zonai day (type 7); mask 80 never resets (mini-bosses such as the cave bosses, some armies: 165 enemy flags).
  **read**

## 10. Equipment and weapons (was: spec B.11, B.13, B.14, B.17, B.34)

### 10.1 The equipment user

[comps+0x230] (**read**; research enemy/gaps/G8):
- slot links +0x20 + s*0x18 (8 slots); request records +0x570 + s*0x100; in-hand state bytes +0x4620 + s; flag word
  +0x4600 (jump-fall attack bit 0x80000, arrow attachment bit 33, guard and just-guard bits 10 and 12, lightning wait
  bit 16).
- Slots (inferred from use): 0 Weapon, 1 Sub, 2 Third, 3 Bow, 4 Shield, 5 Tool, 6 SubTool, 7 Helmet. Index 3 is special
  in the remove and equip paths.
- The visible byte of a slot: +0x570 + slot*0x100 + 0xf4; its setter main+0xced8c0 writes it and sets bit 0x200 of the
  slot's command word (+0xf0), which the component's calc acts on. Other commands of that word: damage 0x100,
  extinguish 0x20000, re-fit 0x10000, change state, the attachment's activate and detach; each is a request the calc
  consumes. **read** (research enemy/gaps/R19; nodes EquipmentUserRequestSetVisible, RequestAllEquipmentVisible)

### 10.2 Who holds what

- A weapon is its own actor, tied to its enemy by an ActorLink in a slot. The holder walk main+0x1d19380(thing)
  returns the topmost holder (pure; it follows +0x208, +0x440, +0x508, +0x120 of the component table). The held
  weapon's link to its holder is most likely [[w+0x228]+0x208]+0x20, and the same component's +0xb0 is the fused
  attachment. **read** (the link's offset inferred)
- Placed enemies carry Zonai devices as fused attachments (flame emitter, light mirror, spring). **read**
- The level-sensor weapon swap (EquipAndAttachmentByLevelSensor) uses the weapon score ([level manager+8]), which can
  differ between saves. **read**
- Whether enemy weapons lose durability: **open**.

### 10.3 Equip, drop, throw, remove, pick up

The entries are in section 4. Facts about them (**read**; **measured** where said):
- A dropped or thrown weapon is remembered (main+0x1ce23c0) so the search module can fetch it again.
- A dying enemy's weapon is released by its Die action's node, not by the death tick.
- A drop or throw by an enemy happens at main+0x1b738e4 or main+0x1b73be4. **measured** 2026-11-17 (a Bokoblin's sword
  noted at main+0x1b738e4).
- The pick-up search is the perception module EnemyCommonSearchmemoryequipmentModule; the pick itself is
  main+0x1d71dfc (callers include a node-class function at main+0x238fe98); main+0x4f3834 is also reached by item
  creation, so it is no clean "pick-up" point. **read** (research enemy/gaps/R16)

### 10.4 What an enemy holds belongs to it

An actor whose holder chain ends at an enemy belongs to that enemy: weapon, shield, bow, the Zonai device fused to
them, a carried rock. A Wizzrobe's rod does not pass the slot link's id word; its holder is told by the thing's live
state. **read**; **measured** 2026-11-15/17.

### 10.5 The Lifter and the Liftable

- Lifter [comps+0x308] (accessor main+0x194fcd4): +0x20 the hold key's name (the entry in the lift-parameter table),
  +0x70 lift capacity, +0x98 phase (0 idle, 1 going to lift, 2 lifting, 3 holding), +0xc8 the reference to the held
  object, +0x128 the hold anchor matrix, +0x174 release mode (1 and 2 put, 3 throw, 4 fall), +0x178 the throw type,
  +0x1a0..+0x1a8 the throw vector. Liftable [comps+0x300]: state +0x34 (2, 3, 4 bound; 6 thrown), the lifter's reference
  +0xc0 (writer not found), the last thrown velocity +0x16c, a physics constraint +0x188. **read** (research
  enemy/gaps/R20-lifter)
- 68 enemies have a Lifter, 56 a Liftable. Ultrahand cannot grab enemies (no accepting receiver; Talus arms and rock
  bullets accept Recall only). **read** (research enemy/gaps/R3, G4)

## 11. Shooters and projectiles (was: spec A "Shots", B.36, B.39, B.40)

- A projectile is **not** made at the shot. The Shooter component ([comps+0x448]; 182 enemies) makes its projectile
  actors in advance from ShooterParam (`ShootableActorSettings`: actor name, CreateMethod OnInitialization or
  OnRequest, MaxActors, ParamNum 12; read at main+0x1aeb9c4). Examples: a Fire Wizzrobe's `Wizzrobe_WeatherFireBall`
  (one at start, at most 5), an Octorok's bullet (one at start, 2), a Bokoblin's arrows (on request, 12) and
  `Rock_Weapon` (2), the elemental fruits, `EnemyShockWave`. **read** (research enemy/gaps/R6)
- A shot is a state change of a 0xf8-byte record in the component: PREPARE main+0x1bf9af8, then SHOOT main+0x1bfbc44
  (state 2 with the request's data). The projectile's own component polls the record and launches, flies and ends by
  itself. A prepare is refused for a name made on request until the request main+0x1bfd6a8 has filled the pool.
  **read**; **measured** 2026-11-15/17 (a Fire Wizzrobe's fireballs and archers' arrows).
- The request block has two layouts, both holding up to 32 named parameters (header: capacity u32, pointer to the
  cells, count i32; cells of 0xc8 bytes: type u32 +0, pointer to the name +8, the name's capacity 0x80 +0x10, the name
  +0x14, the value +0x98). A shoot's block: vtable +0, header +8, cells from +0x20, numbers from +0x1920 (attack type
  +0x1924, key +0x1928, flag bytes from +0x1938); its parameters are `Range`, `InitPos`, `Velocity`, `MotionProperty`,
  `Level`. A prepare's block: key +0, a pointer +8, header +0x18, cells from +0x30. Cells past the count and a name's
  bytes after its end are stack leftovers. **read**
- The pointer at +8 of a prepare's block is the name of the shooter's bone the prepared projectile is held at (the
  node's TrackBoneName; ExecuteShooterPrepare's virtual +0x118, main+0x24521f8, stores it); the prepare tests its first
  character and the record keeps it at +0x50 while the projectile is held. **read**; **measured** 2026-11-17 (a record
  with no bone name left a Bokoblin's stone unheld: it fell 7.87 m in the 1.3 s between prepare and throw).
- A cell's value is eight bytes for six value types (1, 6, 9, 0xd, 0xe, 0x14); whether any is an address is **open**.
- The flight is the projectile's own physics from the launch. A rod's flame takes its launch speed and direction from
  the shoot's `Velocity`; the projectile's aim point ([proj comps+0x440]+0x100) is set to its own start and gives no
  direction. **measured** 2026-11-15/17 (two games: five flames within 3 cm at 6 and 30 frames).
- A held thing shoots with its own Shooter: a fire rod's flame is shot by the ruby fused to the rod, not by the
  Wizzrobe. **measured** 2026-11-15/17.
- A shoot takes two id pairs from its shooter (record +0xe4..0xf3, by main+0x482760: a fresh pair per swing, from the
  weapon's [comps+0x428]). What reads them (whom a flame may hurt, once per swing) is **open**.
- Shots put to sleep: OneShotEquipmentUserCancelShooter, OneShotShooterRequestSleep and the "drop prepared" nodes end
  in main+0x1bfa698, main+0x1bfa4e0 or main+0x1bfa87c (section 4); a "drop prepared" of a shot still in the hand ends
  in the shoot call. **read** (research enemy/gaps/R19)

## 12. What a non-thinking enemy lacks unless it is given it (was: spec B.2, B.16, B.17, B.20, B.23, B.33)

These are game facts about state that graph nodes write. An enemy whose graphs do not run keeps the values it was
loaded with.

### 12.1 The model controller's looks

[comps+0x280] keeps each look in a field with a dirty bit in the word at +0xac (the apply main+0x306e54 acts on them):
the ice look's amount +0x4c (bit 0x1000; setter main+0x1b87a9c), +0x54 (0x10000000), the death effect's amount +0x58
(0x100; setter main+0x1b87b04), the fade +0x5c (0x20000), the hidden mask +0x9b (bit 1 when it goes between none and
some; setter main+0xbb0dd0), the request byte +0x9d ("depth and shadow only", a Wizzrobe's unseen flight; bit 0x200;
setter main+0x1b87b74), the model enabled +0x9c (0x100000), +0x9e (0x10000). **read** (research enemy/gaps/R7, R7b,
R19). The ice look is not an animation.

### 12.2 The attention sender (prompts)

[comps+0x130] keeps per client type a "not offered for reason" mask at +0x24 + type (types up to 0x3f; +0x24..0x64).
A client with any reason set is not offered to the player's game. A Bokoblin's clients: LockOn (fan 180 degrees, 20 m,
line of sight), AutoAim, Grab, AwakeSilentKill (90 degrees, 3 m) and SleepSilentKill (180 degrees, 4 m, sleep check);
Sneakstrike awake is type 0x2c and asleep 0x2d (which is which inferred). The setter main+0x4e603c(sender, reason bit,
client type, 1 offered | 0 not) also marks the clients' records. Graph nodes (a utility-graph module keyed on the verb's
Sleep attribute and the form key "Sleep"; 30 OneShotSetDoActionAttentionValidFLag nodes, which also end lock-on on a
dying enemy) write them. **read** (research enemy/gaps/R4 1.3, 1.4). **Measured** 2026-11-16: an enemy whose graphs did
not run offered a Sneakstrike while in battle.

### 12.3 The battle-info component

[comps+0x140] holds the flags the life gauge, the marks over the head and the battle music's focus hang on (+0x38;
+0x40 bit 8 "force disappear"). An enemy in battle has 0x720 there (0x1720, 0x1f20, 0x10720 at moments); a
non-thinking one 0 or 0x200. The component puts its own value back every frame, so a written word does not hold; with
the brain verb asked the game's way, the game's own procedures work it out. The battle manager's list
([*main+0x395e090]+0x4548) links a component at an offset of its own. **measured** 2026-11-16/17 (research
enemy/gaps/R15; was: spec B.20, B.21). What draws the marks, the gauge and the name: **open**.

### 12.4 Effects and sounds by key

OneShotXLinkSearchAndEmit (entry main+0x25aafec; the node map also names main+0x25aafbc) emits on the actor's users
([[comps+0x68]+0x70] and +0x78) through ELink main+0xa98cf8 and SLink main+0xa98d00. A held effect
(ExecuteXLinkSearchAndEmitAndFade: enter main+0x24901b0, leave main+0x24902e0; a Yiga footsoldier's `Sign`,
`Vehicle_Magic`) keeps two handles and is stopped by main+0x6e34e8(the pair, -1). **read**; **measured** 2026-11-17 (a
Wizzrobe's `WeatherFlash`).

### 12.5 Blackboard memory and the magic weather

- What a graph keeps in the actor blackboard and acts on later is the enemy's: a Wizzrobe's `IsMagicWeather` is set by
  the weather action at an animation event and cleared only when the brain goes back out of battle; while it is set
  the always-running graph asks for the weather every frame and shoots a fireball at a random interval, with no timer.
  **read** (research enemy/gaps/R8c)
- The game's bool setter for an actor blackboard: main+0x740b68 (the value at [bb+0x1a0], not written while the lock
  byte [bb+0x1e0] is set; the change counter [bb+0x1f0] goes up by one). **read**
- `IsShootMagicObj` read false in the actor blackboard while fireballs were shot: it is kept elsewhere. **measured**
  2026-11-16; where is **open**.
- The magic weather request main+0x21cdf44(manager, type, reserve, actor): the manager keeps a claim for four updates
  and reads nothing of the actor. **read**; **measured** 2026-11-16 (type 1 while a Wizzrobe's weather was on).

### 12.6 Other state

- The swift-attack bit (section 7.7). The attack layer override and the sensors-off word (section 7.2).
- The sweep-out table [comps+0x4a0]: its states only move the enemy's own bodies to another collision layer. **read**
  (research enemy/gaps/R20-rider-sweepout)
- The rider keeps its mount as an actor link (horses-animals.md).

### 12.7 The graph census of outward acts

Every outward-acting node class of every enemy family's graphs (578 classes over 95 families, 2,734 graph files) was
read to the game function it ends in. By node instances in the graphs, the acts beyond what an animation does: physics
on the enemy's own body 3,192; grab, hold, swallow, lift and throw of another actor 929; commands on a held weapon
829; messages to other actors 684; game-balance bits and timers 564; Shooter state other than prepare and shoot 532;
mount and dismount 511; the model's looks 498; an attack or sensor armed by a node 377; blackboard writes 339; the
sweep-out table 225; game-wide state 199; chemical generators 175; an actor deleted 162; weak-point parts 151; smaller
ones (noise, the notice mark, form change, territory, the block construct's own state, swarm state). About a fifth of
the classes were placed by name only (no function at the node's address), most of them boss moves; native modules,
sequence data and animation events are not counted. **read** (research enemy/gaps/R19; was: spec B.33). The eleven
families of an earlier count use 1,367 node classes, at least 315 acting on the world, 40 holding something between
enter and leave (research enemy/gaps/R9).

## 13. Identity, spawning, camps

### 13.1 Placed enemies

- Placed enemies come from Banc entity files into banc slots; main+0x65b710 makes the create requests; new actors show
  at main+0x50d6bc. **read**
- The creation job (built by main+0x59c870, 0x100 bytes): +0x40 position, +0x4c rotation, +0x88 the u64 placement hash
  from the entity's `Hash`, +0x98 flags, +0xa4 the banc slot. Run-time spawns get hash -1 and slot -1. [actor+0x370]
  is first stored by the create callbacks main+0x195d2e8, main+0x2b026a8, main+0x2b02b94; the re-create path
  (main+0x2b04114) keeps the hash and sets the slot to -1. **read** (research enemy/gaps/G3)
- Scale of placed enemies with a BSA: 8,511. At most 15 within 50 m and 51 within 200 m on the surface; one cluster of
  61 in the depths; 44 in the castle. **read** (research enemy/gaps/R3)

### 13.2 The level sensor: the name at a placement

The actor name at a placement can differ between two saves: the level sensor swaps it at creation (main+0x5f2f40 ->
main+0x21c1a44) from one global score. The score (main+0x22bde4 per PointDefine element, main+0x22b920 for the sum):
per element Point times min(DefeatedNum, cap), plus skill terms from NoDamage, GuardJust, JustAvoid and HeadShot
counts (capped by the defeat term; times 0.1 for bosses). The enemy score (+0xc) is the total times 0.01, the weapon
score (+8) times 0.0075. Inputs are GameData Ints and the parameter file only. It is recomputed in full only through
the virtual main+0x21c7974 -> main+0x21c2660 (a parameter reload, inferred), not per kill or frame; who calls it is
**open**. So two games with equal Ints can differ until it runs. **read** (research enemy/gaps/G3, R5)

### 13.3 Run-time spawns

- Enemy sources of run-time actors (research enemy/gaps/G6, R5; **read**):
  - The sequence auto-placement (node SeqExecuteRequestCreateActorAutoPlacement, enter main+0xa80c60): field Keese,
    Lizalfos, Octoroks, Evermean dummies, Gibdo, and the "near" families (night Stal, Chuchus, pop-up Lizalfos, Yiga,
    snow Octorok). It is driven by the local player (position, temperature, riding, gear, time of day) and the global
    random generator. The push main+0xa80f28 picks a spawn point near the manager's tracked position (the local player,
    inferred) into a ring (records of 0x98 bytes at M+0x88); the executor main+0x538000 (called only from main+0x5355b0,
    the manager's per-phase update) makes the create request at main+0x538ca4 (return address main+0x538ca8; x1 &name,
    x2 the create block, x3 the handle cell). The live-slot table is at mgr+0x66b38 (0x40 slots of 0x68 bytes: name, id,
    radius, position); the executor refuses a second actor of the same name and id within the slot radius, caps per
    name and at 9 per rule. Its despawn rule is **open**.
  - "Near" enemies are made somewhere, then warp next to their target by their own AI (QueryCalcAppearPos
    main+0x25f0c1c: the target's position plus the global random generator) and may delete themselves when the target
    is gone.
  - The ActorHolder component (201 rows in 128 packs) through OneShotActorHolderTryRequestCreateActor ->
    main+0x1b11d38: Stal head and arm weapon, Wizzrobe summons, Evermean dummy to real Evermean, Stalnox eye, camp
    meat, dragon parts, Colgera thorns, the Seized Construct's devices.
  - The creator node main+0x231d288: Gibdo nests only (ZombieSpawnControl and GenericCreateActorsAsync are in no
    graph).
  - The army manager main+0x1b1edcc; event and quest creates. Camps create nothing.
  - No node exists for a Chuchu's split or merge, a Stal's revive or a Gibdo's armour burn. "Transform" is not a death
    cause.
- Identities the game gives: (parent, holder key hash, index below MaxActors) for holder children; (nest hash, slot
  0..0x3f) and (army owner hash, slot) while the occupant lives; auto-placed enemies have none.
- An enemy made by a game's own spawner (a skeleton) stood at the other player's stand-in and played no clip in the game
  that made it. **measured** 2026-11-17. How the game keeps such an enemy undrawn before it rises: **open** (research
  enemy/gaps/R20-buried-skeleton).
- An enemy's home, where it returns to: the nest's place when it has a nest, else [[[comps+0x50]+0x20]+0x140]+0x10
  when actor+0x4b1 bit 1 is set, else actor+0x284 (position; rotation from main+0x2af6194) (QueryEnemyGetHome-
  OrNestPositionAndRotation, calc main+0x262f020). **read** (research enemy/gaps/R23)
- A run-time enemy that finds no path home removes itself: a Bokoblin_Junior made by the create request beside the
  Links on the Great Sky Island start platform (home read at actor+0x284: its spawn place) took brain verb 47
  (ForceReturnHome) 5 s after it was made, and 3.5 s later the ForceReturnHome module's node
  OneShotGenericFarDeleteThisActor asked a delete with reason 3 (deferred delete main+0x63b6b0 from return address
  main+0x2534d34, run at main+0x63baa8 from main+0x2fd080): no death, no drop. **measured** 2026-11-17. The verb is set
  when the path to home has length 0 or less in Bokoblin.brain.returntohomeposition (a spot with no navigation path);
  without a nest the ForceReturn timeout does not apply. **read** (research enemy/gaps/R23). Which spots lack a path,
  and whether the spawn sync's real run-time enemies meet this: **open**.

### 13.4 Camps, armies, the attack limiter

- A camp is a separate meta-AI actor with a link list of members (main+0x2af3d48: count +0x10, array +0x18, stride
  0x18, actor +8; a member has actor+0x98 bit 15). Its node ExecuteMetaAIEnemyCampMessageRecieveAndSender (enter
  main+0x236dc10, receive main+0x236e11c) relays messages 0x3ddc0006 to 0x3ddc000a (not 8) to every member. The camp's
  cleared record and chest unlock: **open**. **read** (research enemy/gaps/R5)
- Camps (ExecuteMetaAIEnemyCampNormal) and armies use the global random generator. **read**
- The attack-permission limiter [main+0x3a64670] caps only pre-registered target ids. The game-balance manager
  (attack permission and intervals) is one per game. **read**

### 13.5 Unloading

The despawn check main+0x9550f0 gives the reasons 6 (banc slot unloaded), 3 (radius: LoadRadius and
DeleteRadiusOffset), 5 (a flag) and 10. The calc's far gate ([LOD+0x84] > [class+0x264]^2, [LOD] = [comps+0xb0]) stops
only the interrupt-graph poll. Both are relative to the local player and camera. The radii in metres: **open**.
**read**. (Delete reasons in full: engine.md.)

The far gate in full (**read**, spec 4.1; values not measured): with A = [[[BSA+0x25f0]+0x18]+0x150] and X = [A+0x38],
the gate is open when BSA+0x26ad bit 5 is set, or [A+0x41] bit 0 or [X+0x2d2] bit 0 is clear, or f32 [X+0x264] is
negative, or [[comps+0xb0]+0x84] <= [X+0x264]^2. **Measured** 2026-11-15: the gate read open for an enemy 70 m off for
which no graph runner was entered.

## 14. Movement

- AI movement never writes the position. Nodes push a direction and speed into the character controller; its vtable:
  +0x48 direction, +0x68 speed, +0x70 velocity (main+0x298cf64 splits a velocity into direction and speed), +0xc0
  position and rotation. The controller keeps forward direction at CctData+0x1a8 (written via main+0x298ce54) and speed
  at +0x1c0 (via main+0x1ba5c8). **read**
- Native walk modules write those two and a speed pair at [[comps+0x370]+0x20] +0xa8/+0xac; never a jump. **read**
- The controller's result pass main+0x240440 (after the physics step) reads the body's post-step velocity, splits what
  exceeds its own last output (CctData+0x1e0) off as external velocity (+0x210), runs the contact search, writes the
  next velocity and sets the landed and left-ground edge flags (MoveStates+0x58, +0x59). Where direction times speed
  becomes velocity is **open**. **read** (research enemy/gaps/G4; corrects verify/08)
- Move states: 0 ground, 1 air, 2 free (flyers). ChangeMoveState main+0x298f7a8. The move-state index is at [K+0x40]+0x50
  and the controller body's velocity at [K+0x18]+0xf8, with K = [controller+8]. **read**
- Knock-back and launches are an additive per-frame accumulator (main+0x2992618 on a controller sub-item). Jumps:
  OneShotPhysicsCctJump main+0x2554204 -> the jump module [body+0x208]. **read**
- After the AI, slot 50 calls main+0x257310, which copies the physics body into actor+0x2b4 (position), +0x2c0
  (rotation), +0x320 (linear velocity, also from a pose jump) and +0x32c (angular). The actor also keeps a transform at
  +0x2f0. **read**
- ForceSetMatrix main+0x45ce70(actor, m[12], 0) queues a pose for the controller or rigid body (a NaN drops it;
  velocity untouched); the next step mirrors it into the actor. **read**; **measured** 2026-11-15 (run 7).
- Motion type main+0xae4300(body, 0 static / 1 keyframed / 2 dynamic); body = [[[comps+0x50]+0x20]+0x150]. A static
  main body under ForceSetMatrix each frame: actor and body at the pose asked the next frame, to the millimetre,
  upright. A keyframed body sank 3.3 m through the floor. **measured** 2026-11-15 (runs 6, 7).
- A switched-off actor whose body was posed this way still had its actor where it had stood unless the actor's
  transforms (+0x2b4, +0x2f0) were written too. **measured** 2026-11-15.
- Census (RomFS): 225 of 279 enemy-affiliated actors have a character controller; 54 are rigid-body-only parts (Talus
  arms and weak points, Stal heads, a Stalnox eye, rocks, bullets, barriers). Of the 221 with a BSA, 214 have a
  controller. Flyers are controller bodies with default move state free. AnimeDrivenMove is on 36 enemies. **read**
  (research enemy/gaps/G4, R3)
- SetLinearVelocity main+0x567750 on a controller body is overwritten by the result pass. **read**

## 15. Families

- 306 `Enemy_*` packs: 221 have a BSA; 85 do not (23 army managers, 6 dragons driven by a Dragon component, 14 weak
  points, 30 shootables (projectiles and limbs), dummies). The Bokoblin graph shape covers 90+ actors. **read**
  (research enemy/gaps/R3; the "279" of earlier notes counts enemy-affiliated actors)
- Components over the 221: Rider 40, Ridable 23, Lifter 68, Liftable 56, weak-point user 20, Shooter 182, Drop 192,
  EquipmentUser 138, ActorHolder 111, Nest (territory) 216, Swarm 4, GelEnemy 15, Quadruped 14. **read**
- Parts: Talus arms are Shooter-made children; Hinox attention helpers, the Stalnox eye, Stal head and arm, Colgera
  thorns, the Seized Construct's devices, a Like Like's kept actors and Keese cadavers are ActorHolder children; a
  Gleeok's heads and a Hinox's eye and armour are inside the one actor (inferred). Frox, Flux Construct, Gohma,
  Mucktorok, Queen Gibdo and Gloom Hands: no child rows; how a part's damage reaches its body: **open**. **read**
- Names that are not whole enemies: weak points and secret stones, dummies, bullets, tornadoes, thorns, event starters;
  the census's System actors (army managers, dragons). **read**
- Standing follow shares (an enemy spawned beside one game's Link, the other game showing a copy that plays its
  owner's commands; share of samples with the same command), **measured** 2026-11-16/17: Bokoblin camp 96.5 to 100 %;
  Like Like 99.3 %, Lynel 99.7 % (19 cm at worst moving), Evermean 98.7 %, Gibdo 100 % (98.9 % in a camp fight),
  soldier construct 99.2 %; Hinox 98.2 and 98.6 %, Talus 96.5 and 98.4 % (nine actors with its arms), Frox 98.3 %, cave
  boss 98.2 % (6.6 cm moving); Gleeok 99.8 %, block construct 99.7 %, boss Bokoblin 99.7 %, armoured Lynel 99.7 %, Gibdo
  boss with its parts 98.8 %; sand worm 99.2 %, Keese swarm 100 % (20 cm moving), bee swarm 100 %, Goron mid-boss 100 %
  (33 cm moving), practice construct 99.7 %, Kohga's construct 100 %; a baby Frox 91 to 94 % (25 cm off its pose). A
  Yiga footsoldier's blinks were not followed well. Not fought: these families' attacks, parts and special moves.
  (was: spec B.19, B.24, B.29, B.32)
- A far cliff guard plays the game's far-wait clip (`WaitBattleLODMgr_Wait`) where the game running it in range plays
  another. **measured** 2026-11-17.

## 16. The player as the enemy's victim

Link's life is the same Life component, with ExtraLife and BreakLife on and a Player damage calculator. While Player
flag P+0xae8 bit 19 is set that calculator drops most hits (likely the invincibility gate; its setter is **open**).
"PlayerDamageMessages" only makes a Sage talk. The perfect guard is an animation window on the guarding Link's own game
(hold event 0x38, QueryASGetHoldEventJustGuard). **read** Where a Player's hearts really live is **open** (C23; see
player.md).

A copy's melee blow does hurt the player of the game that shows it, when that player is inside the swing. **measured**
2026-11-17 (a play session: player 1's life fell nine times while the attacking Bokoblin was the other game's); the
tester found the hits "inconsistent", which is not explained. (was: spec B.39)

## 17. Not read

- The view cone and line-of-sight details beyond main+0x1f41c90; the stability of the sight list's order.
- The brain's per-frame path (section 2.4); comps+0x48's optional graphs; the unread callers of the runner
  bodies (main+0x198360, main+0x50c950, main+0x1949b40, main+0x1ceee24 among them).
- Which blackboard action nodes write; whether a slot graph that has returned success keeps ticking under repeated
  equal requests; whether the move-control adjustment is in the bytes seen at main+0x50ad74's entry.
- What the animation graph uses the slot's random value for; consumers of sound, effect, trail and JstAvdRec events; a
  clock jump over an event.
- The hit entry's field that launches an enemy; who writes the attacker's guard record (L+0xfc, L+0x104); P+0xae8 bit
  19's setter; the enemy side of the flurry rush.
- The death message 0x6a0e0000's receiver; bit 14 of the death state word in play; the camp-cleared record and its
  chest; what decides the blood moon; who refreshes the level score.
- Which state durably keeps a placed actor away without the save's flag.
- How a part's damage reaches its body; the dragons' component; the 22 enemies whose attack arming was not found.
- The battle music's manager (battle_bgm_and_actor_ai_group_mgr); the code that draws the marks, health bar and name.
- A Like Like's swallow, a Lynel's charge and bow, an Evermean's disguise, a Horriblin's ceiling hold, a Lizalfos's
  wall hold.
- The form key's setter, and which enemies have more than one form key.
- A buried skeleton's undrawn state before it rises.
