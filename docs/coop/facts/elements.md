# Elements: game facts

Fire, ice, electricity, wetness and lightning in TotK 1.4.3. Conventions and marks as in [README.md](README.md):
**read**, **measured** (with date), **inferred**, **open**. Sources: element-design (docs/coop/element-sync-design.md,
section 1), element-spec (element-sync-spec.md), and research elements/E1, E2, E3 (outside this repository).
Enemy conditions and death causes are also in [enemies.md](enemies.md); a burning contraption part in
[zonai.md](zonai.md).

## 1. The chemical system

- The game calls it "chemical" (`game__chm__WorldParam`, `ObjectParam`, `ElementParam`, `MaterialParam`). It has three
  kinds of thing: elements on actors, the ground cells (a grid of burnable ground, not actors), and free elements the
  system or an emitter makes (a burning cell's fire, the pool of a flame or frost emitter, wind capsules, water
  objects). **read** for the first two and the cell fire; the others **open**.
- An actor's chemical component is [[actor+0x228]+0x180]; it points back at its actor at +0x18. Its elements are an
  intrusive list (head +0x40, first +0x48, count +0x50, link offset +0x54; an entry's element at +0x10). The element
  class's vtable is main+0x3625f60. **read** (was: element-design 1.1)
- Elements register with the system's list at S+0x100 through main+0x18d53c8. The element update main+0x547a2c is
  reached only through the element vtable's slot +0x158; who calls that slot (probably the system's list) is **open**.
  **read** (research elements/E1)

### 1.1 When a component updates

- The component update main+0x30efa0(component, step) has no gate of its own: it clears bits 49..53 of comp+0x20 and
  ticks every element (main+0x2df64). comp+0xa8 is a validity counter for a cache of flag bits (rebuilt by
  main+0x1de340), not a "run or return" gate. **read** (research elements/E1; corrects an earlier reading)
- Its callers are the per-actor calc tasks (main+0x2f9910, main+0x194fd7c, main+0x1951200). Inside main+0x2f9910 the
  component is skipped when actor+0x5ac bit 6 is set, when actor+0x3fc & 3, when actor+0x3fc & 0xc4 (a pause reason),
  or on a hash-set test of the actor's ids. So a thing at rest is updated only while something happens to it.
  **read**; **measured** 2026-11-16/17 (a resting crate's component was visited once, when it was made).
- The reaction walkers run inside the component update, so a resting thing neither gives nor takes reactions until
  its component updates. **read**

### 1.2 Switch bits on a component (comp+0x20)

| Bit | Meaning | Mark |
| --- | --- | --- |
| 29 | unaffected by elements (switch main+0x1b35118) | read |
| 32 | does not interact with the world (node run main+0x250cc48); stops the element's whole contact step, a Link's included, and makes the element answer "not active": the update main+0x30efa0 turns it into bit 33, which main+0x18d32c4 reads | read; measured 2026-11-16 |
| 22; 12, 13, 14 | block an ignite (section 4.2); 14 is set from a parameter byte (main+0x1b30968, inferred "cannot burn"); 12 from a runtime boolean in the pre-calc near main+0x2ff5d0 | read; writers of 13 and 22 open |
| 30, 31, 32, 29 | tested by the element's virtual +0x1a0 (main+0x18d1d54), the "may take a reaction" gate | read |
| 19, 20 | inferred NoFrozen switches | inferred |
| 28, 15 | named "no burning" and "no burnt" in older notes; not tested in the ignite path | not confirmed |

(was: element-design 1.3, element-spec 5.5; research elements/E1 Q1, Q3)

## 2. Ground cells (was: element-design 1.2, element-spec 4.1, 4.3, 4.5)

- The system object S holds a pool of cell records (S+0x18) and a list of live cells (head S+0xc6d0; first S+0xc6d8,
  link offset S+0xc6e4). **read**
- A cell is named by a 32-bit id of two 16-bit grid coordinates. Its place follows from the id alone (scale S+8,
  origins S+0xc and S+0x10, bias S+0x14, shift S+0x3c, cell size S+0xc700), so the same id is the same patch of ground
  in every game. The id of a place: ((x - origin) / scale + bias) as a signed 16-bit number, shifted right by S+0x3c,
  for x and z (main+0x1f0f71c does it). Find or make a record: main+0x1f0ff60(S+0x18, id); set one up:
  main+0x1f100e8(S, record, cell context), which sets bit 0 when the ground there can burn. **read**
- Record fields: state byte R+0xa (0 new, 1 heating, 2 burning, 3 burnt); flags R+9 (bit 1 from the terrain: a cell
  that burns longer); time in the state R+0x10; wetness R+0x28 (0 to 1); centre R+0x2c (x) and R+0x34 (z); this
  frame's inputs: fire contacts R+0x80 and R+0x81, water contacts R+0x82, a wind vector R+0x68; heat R+0x84 (ignites at
  30); burn time R+0x88 (ends at 300, later for a long-burning cell); burning frames R+0x8c; the holder of its elements
  R+0xb0. **read**
- At most 254 live cells were walked in a pass. **read** (element-spec 4.1)

### 2.1 The driver and the state step

- Once a frame the driver main+0x2dd070(S, cell context, step record) runs the state step main+0x1f105e0 for every
  live cell, frees one cell idle for more than 45 units, then runs the spread main+0x1f10adc for every cell. Times are
  in frames at 30 a second: a contact heats a cell for one second before it burns, and it burns ten seconds. **read**;
  **measured** 2026-11-16/17 (a cell burned 300 frames, 10.0 s).
- The state step: wetness rises with water contacts and rain (a factor in the step record) and falls otherwise; heat
  rises by the frame's contacts and falls without them; at 30 the cell burns unless it is wet or covered. When it
  starts to burn the step makes the cell's two elements, `GrassFire` (main+0x1f1202c, vtable main+0x36bdc60) and
  `GrassWarm` (main+0x1f121d8, vtable main+0x36bdf80), and puts them in the holder; when the state leaves 2 it removes
  them. So a cell whose state and timers are written from outside makes and removes its own fire, effect and sound.
  **read**
- `GrassFire` shows `Chemical_Fire_Grass` (or `..._OnBoost`), sized by the cell, and registers with a second module
  (main+0x202d6b4; by its strings the burning sound). **read**
- `GrassWarm` is a physics sensor volume above the fire (contact type 9, which the contact entry maps to "nothing", so
  it does nothing to cells). Inferred: the warm air (updraft) gliders and balloons read; its consumer is **open**.
  **read** (research elements/E1 Q6; element-design called it "not read")

### 2.2 Spread and contact

- The spread (main+0x1f10adc), for a burning cell and each of its eight neighbours: the neighbour is heated when
  distance times a random number is below (wind term + 1) times 0.75, with the game's global random generator
  (main+0xc8b920) and the world's wind (direction and speed, plus wind an element blew on the cell). Every tenth
  burning frame it sends a record (centre, radius, strength) to listeners through main+0x1f14f9c; who listens is
  **open** (by the strings, burnt-away grass and the navigation mesh's `NavMeshGrassFire`). **read**
- Everything that touches the ground reaches the cells through one function, main+0x1f0eb94(S, cell context, shape,
  object). The object is a chemical **emission object** made by an element's update (class main+0x3625660), not an
  element; a burning cell's own fire and the warm air over it make objects of class main+0x36295d0. By the object's
  material it heats cells (fire main+0x1f0effc, which also draws a random number), blows on them (main+0x1f0f71c) or
  wets them (main+0x1f0fb78, main+0x1f0fd6c). This, more than the spread, is how a fire grows. **read**; **measured**
  2026-11-16/17 (a copied fire touching its neighbours: about 600 contacts in one game and 5,500 in the other in 70 s).
- From an emission object to its actor: object+0x60 is its source (class main+0x3626048); the source's +0x60 is an
  owner object with the chemical component at +0x10, whose +0x18 is the actor. **read**; **measured** 2026-11-16/17 (a
  fireball and Link were named this way). An object with no actor in its chain is a cell's own.
- Random or different per game: the spread's draw, a fire contact's draw, the wind, the rain. Two games that both
  simulate a fire spread it differently. **read**

### 2.3 Measured cell behaviour

**Measured** 2026-11-16/17, two games on one PC, a save in tall grass: one cell lit in one game burned 300 frames; a
cell whose state was written in the other game began 39 ms and ended 37 ms after it, the flame and the burnt patch at
the same place. A cell written at the owner's heat of 30 lit one frame early and flickered. A Fire Wizzrobe's 70 s of
attacks burned 30 cells; a lightning strike 8 m from a Link lit 29 cells, 2 m from an enemy 34. (element-spec 8)

## 3. Element state on an actor (was: element-design 1.3, element-spec 5.3)

### 3.1 Kinds and the update

- An element's kind, byte el+0x7d: 0 normal, 1 frozen, 2 burning, 3 burnt out, 4 detonated (0xff: none). The update
  main+0x547a2c runs its timers, evaluates the frame's requests and reactions, writes the old kind to el+0x7c and the
  new one to el+0x7d, with the cause from el+0x7e; a new kind 2 goes through the fire start main+0x79e6d0. The flame
  effect, the blackboard flag and the sound follow the kind. **read**
- The state block sb = el+0x50: sb+0x2d is the kind; the one-shot request bytes sb+0x35..0x3b (= el+0x85..0x8b) are
  cleared to 0xff by each update; sb+0x40 (u16) enable bits; sb+0x42 (u16) sticky request bits; sb+0x44 dirty bits.
  **read** (research elements/E2)
- Every request setter writes only when sb+0x40 bit 0 is set; otherwise the request is silently dropped. A request
  written before the element's update is consumed in the same frame. **read** (research elements/E1, E2)

### 3.2 The game's requests

| Request | Call | Mark |
| --- | --- | --- |
| Ignite | main+0x1b34a44(component, 2, element index or -1); 2 is the "force" cause, 1 "by contact" | read; measured |
| Extinguish | main+0xb2b804(component, 0, index) | read; measured |
| Burn out | main+0xb2b804(component, 2, index); 6 ends burning (used by the condition slot) | read; measured |
| Detonate | main+0x1b34b0c | read |
| Freeze | main+0x1b34e48(component, cause, index) -> main+0x18dcf9c on each element (sb+0x3a); any value but 0xff freezes and is kept as the cause | read; measured |
| Thaw | main+0x1b34f10(component, 0, index) -> main+0x18dcfb8 (sb+0x3b) | read; measured |
| Full electric charge | main+0x1b35098(component) -> main+0x18dcfd4(element+0x50) per element: sb+0x42 bit 3 | read |
| Full wet | sb+0x42 bit 1 (setter main+0x18dd008) | read |
| Dry | main+0x1b35038(component) -> main+0x18dd01c: sb+0x42 bit 2 | read |

Corrects element-design 1.3, which gave freeze as main+0x18dcf9c(state block, v) and the full charge at component
level only: those are the per-element setters behind the component calls above (research elements/E2; the code uses
the calls of the table).

- A **forced** ignite (cause 2) is not blocked by wetness, rain, submersion, ice magic or a pending extinguish in the
  ignite evaluator main+0x549998; those blocks apply to cause 1. A forced ignite is blocked when sb+0x40 bit 0 is clear,
  el+0x90 bit 1 is set, the kind is 0xff, comp+0x20 bit 22 is set (or bits 12 to 14 by the owner object's flags, its
  virtual +0x58, default main+0x549ec0), the component never updates, or the element is not in kind 0 (frozen, burnt
  out and detonated go another route). A wetness check on the inline path is **open**. **read** (research elements/E1
  Q3)
- An element that is burnt out or detonated does not burn again. **read**
- The sb+0x42 bits look sticky: no store that clears them was found, so a full charge would keep an element charged
  and a full wet would beat a dry. **inferred**; not run.

### 3.3 Frozen

- Kind 1 comes from a freeze request, from an accumulator flag (cause 0), or from a cold gauge above 120 (cause 3).
  The freeze branch runs only when the class's can-freeze virtual +0x168 (default main+0x18d17b0) is true. **read**
- Frozen has no timer of its own: the element stays frozen while it can freeze and no burn or thaw request comes. A
  burn-side request turns it to burning or detonated (fire thaws ice); a thaw request goes through main+0x549998 to
  normal or burning. **read**
- While any condition slot has its frozen bit, the component update (main+0x255160) asks freeze cause 4 of every
  element every frame; so the slot must clear for a thaw to stay. **read**
- The AI nodes OneshotResetChemicalStateNeutral and OneShotChemicalSetTemporaryNoFrozen touch only condition-slot
  bits (+0x24 bits 16 and 18), not elements. **read**

### 3.4 Electricity and wetness

- Not kinds. An element is electrified when el+0x95 bit 1 is set (reader main+0x1b3087c(component, -1); built by the
  update from the record's bit). The charge timer is f32 rec+0xa8 (rec = el+0xc0 + index*200), counting down by dt to
  0; its maximum is the class virtual +0x108 (a material parameter). **read**
- "Charged" (the element, QueryChemicalIsElectrified) and "shocked" (the condition slot, which drives the stun) are two
  different facts. **read**
- The wet amount is f32 rec+0x68, read by main+0x26253c(component, index). **read**

### 3.5 Reactions

Reactions come in through main+0x1906768(target element, 0, context, reaction), from the walkers main+0x18d3080 and
main+0x18d31c4 that go over what touches the element. A handler only adds to the frame's accumulator; the update
decides. The reaction's source (x3) is a chemical object; its actor is found as in section 2.2. **read**

## 4. Conditions on enemies and Link (was: element-spec 5.5a)

- The condition component C = [[actor+0x228]+0x1a8]: count C+0x140, slots of 0x100 bytes from [C+0x148]. A slot: +4
  element index (-1 all), +8 actor, +0x10 parameter, +0x24 control flags, +0x28 one-shot event flags (read by the AI),
  +0x2c state flags, +0x30..+0x44 who caused it (main+0x1c4e3cc). **read** (research elements/E2, enemy/verify/06)

| State bit (+0x2c) | Meaning | Event bit (+0x28) |
| --- | --- | --- |
| 0 | burning | 8 |
| 1 | shocked | 0x40 |
| 2 | wet | 0x800 |
| 3 | frozen | 1 |
| 5 | unnamed | 0x200 |
| 6 | unnamed; read as Electrified by the death-cause resolver | |

- The slot update main+0x322450 takes burn, wet and frozen from the elements every frame (burn through main+0x324b34,
  which itself asks ignite and end-burning of the elements; frozen from any element of kind 1; wet from the wet amount
  or a sensor value). So an actor shows those once its elements have them; no reaction is involved. **read**;
  **measured** 2026-11-17 (an enemy asked to ignite, freeze and thaw showed it).
- Shock is set once (main+0x325aa4) when an element's record carries the mark of an electric reaction of that frame
  (main+0x326434), and lasts by the slot's timer: on, the state bit, event bit 0x40 and the timer +0xac from its length
  +0xa8 (main+0x326464); off, the state bit cleared, bit 3 of +0x24 and the rest timer +0xb4 from +0xb0 (main+0x3264f0).
  **read**
- What clears the frozen state bit: **open**.
- Frozen in ice is separate: X = [[actor+0x228]+0x3a0], frozen-in-ice when X+0x24 == 3 and X+0x2c == 3
  (QueryConditionIsFrozenInIce -> main+0x1b3dc20); main+0x1bc980c sets +0x2c to 2 with an effect; main+0x1bca2b4 is its
  exit. Who sets (3, 3): **open**. "IceActor" and "ExpandActorIce" are variant names in a parameter loader, not an ice
  block actor. **read**
- The ice look is the model controller's float [[actor+0x228]+0x280]+0x4c, set by main+0x1b87a9c (with bit 0x1000 of
  +0xac) from AI nodes (main+0x25332c0 "set the frozen amount"; the break module main+0x24f05f4 calls main+0x1856cb0,
  which sets it to 0 and plays the fall-apart event). It is not automatic from kind 1. **read**; **measured** 2026-11-17
  (an enemy frozen without its look until the float was written).
- The stun and weapon drop of a shocked enemy are behaviour-graph reactions to the shock bit. **read**
- Damage: main+0x327968 is hit-driven bonus damage from an existing condition (called from the hit handler
  main+0x1cd9380 and the damage step), not a per-tick burn. Per-tick burn, freeze or shock damage was not found in the
  slot class. Wood burning alone loses no life and ends as death kind 9 when its element is burnt out. **read**
  (research elements/E2 6; corrects element-design 1.3, which called main+0x327968 the burn damage)
- **Measured** 2026-11-16/17: a Bokoblin asked to ignite burned 2.0 s; a lightning strike 2 m from an enemy electrified
  its element for one frame and it died of the blast.

## 5. Area effects (was: element-design 1.4)

| Effect | What it is | Mark |
| --- | --- | --- |
| Updraft over a fire | `GrassWarm` for a cell (section 2.1); `Chemical_Updraft`, `IsBurnUpdraft` strings | read for the cell element; consumer open |
| Wind of fans and gusts | `Chemical_WindCapsule` actors and wind elements; reach cells through the contact entry | in part (zonai research) |
| Flame and frost emitters | a pool of 35 elements and 35 attack sensors per emitter (main+0x1b37958, main+0x1b3882c) | read earlier |
| Shock emitter, electrified metal and water | electricity passed between touching elements (`Chemical_Electricity`, `Chemical_ElectricWater`, `Chemical_ElecConnect`) | open |
| Ice blocks, frozen water | `Chemical_IceMagic`, `Chemical_IceMelt*`; the frozen kind | open |
| Explosions | reactions of type blast and the detonate request | read earlier |
| Rain, water | wets cells (the step's rain factor) and elements; a wet thing does not burn by contact | read for cells |
| Weather | each game has its own | read |

## 6. Lightning (was: element-spec 5.6)

- The world manager wm = [[main+0x395dc20]] (also read as [main+0x3a83968]); the weather system W = [wm+0xd0]
  (vtable main+0x3737b08: +0x1b8 wind speed f32 [W+0x368], +0x1c0 the wind direction [W+0x36c], +0x1e0 overwrite the
  forecast, +0x370 the strike request); the thunder manager TM = [W+0x228]. **read** (research elements/E3)
- **The strike**: main+0x21c43b4(TM, position, level, sage, element or 0) makes the effect and records the strike; the
  level is clamped into the ThunderParam level table. Its only callers: main+0x21c3a6c (the storm's strike near a
  point), main+0x21c4074 (the storm's strike on request), and main+0x21c4358 (reached only through W's virtual +0x370;
  takes the position from the element). **read**
- Gameplay strikes go through one request flag on a chemical component: main+0x1b2f944(component, level, sage) sets
  comp+0xac bit 0 (bit 1 the sage flag) and the level byte comp+0xb0. Callers: the lightning arrow (main+0x1d27868,
  main+0x1d3310c), the nodes OneShotChemicalRequestStrikeLightning (main+0x250cdf0) and ...StrikeLightningTo
  (main+0x250cf7c, via main+0x1b08d40). The component update consumes it in the next frame: it strikes at the first
  active element's position through W's virtual +0x370 and clears the bit. **read**
- The storm is a state machine on TM (main+0x62aac4; state TM+0x34, time TM+0x38, duration TM+0x3c of 30, 240, 300 or
  150 to 300 frames). In striking states a random check against a per-check probability picks a point
  (main+0x21c4c74): the local player's position plus its motion over 90 frames, plus or minus 30 m random on each axis;
  then a 90-frame (3 s) warning countdown (TM+0xa0). So where a storm strikes is random per game and near the local
  player. **read**
- Static on metal before a strike is the metal's own chemistry: the chemical source's virtual main+0x18d32c4 starts
  `Chemical_LightningSign_OT` at main+0x18d43c0. It is not the equipment user's lightning wait (bit 16 of +0x4600),
  which only the Sage's thunder field and an AI node no 1.4.3 graph uses set. **read**
- `IsEquipmentAvoidLightningTarget` in an actor's chemical parameters keeps a storm off the gear that actor holds; in
  1.4.3 only the Lynels', Ganondorf's and the Sage golem's have it true. **read**
- A strike made at a place lights the cells there and its blast actor (`LightningStrike...`) hurts what is near.
  **measured** 2026-11-17. Two games' storms are independent; a storm built static on a real item another player's
  actor held and struck it there. **measured** 2026-11-17 (a tester's storm).

## 7. Not read

- Who calls the element update's vtable slot; what pushes an actor's calc task each frame.
- Writers of comp+0x20 bits 13, 22, 28, 15, 32; what the bits of actor+0x3fc mean one by one.
- The spread's listeners; `GrassWarm`'s consumer; the wind capsule detail.
- A wetness block on the forced ignite's inline path; the transition route for a request on a frozen, burnt-out or
  detonated element; the sticky request bits' clear.
- What clears a condition slot's frozen bit; who sets frozen-in-ice; the shock stun's nodes.
- Per-tick element damage; condition durations (ConditionParam is data only; its counter is not found).
- Electricity through water and metal beyond the element flag; ice blocks; frozen water.
- How the weather decides a storm's start; the blood-moon trigger (enemies.md).
