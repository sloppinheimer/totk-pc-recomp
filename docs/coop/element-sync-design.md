# Element effects in co-op: what the game has, and the design of their sync

TotK 1.4.3. Addresses are `main+offset`. Each fact is marked **read** (seen in the code or data in this pass),
**earlier** (from the contraption research, read then and relied on here) or **not read**. The specification that is
built from this is [element-sync-spec.md](element-sync-spec.md).

The aim: every player sees the same fire, ice, electricity and wetness, whatever caused them. The rule that worked for
enemies is kept: one game decides each thing and states it; the other games make it so and never decide by themselves.

## 1. What the game has

### 1.1 The chemical system

The game calls it "chemical" (`game__chm__WorldParam`, `ObjectParam`, `ElementParam`, `MaterialParam`). It has three
kinds of thing.

- **Elements on actors.** An actor's chemical component is `[[actor+0x228]+0x180]`; the component points back at its
  actor at `+0x18`. Its elements are an intrusive list (head `+0x40`, first `+0x48`, count `+0x50`, link offset
  `+0x54`; an entry's element at `+0x10`). The element class has its vtable at main+0x3625f60. **read**
- **The ground cells**, a grid of burnable ground kept by the chemical system itself (1.2). Not actors. **read**
- **Free elements** the system or an emitter makes: the fire of a burning cell (class `GrassFire`, vtable
  main+0x36bdc60), the pool of a flame or frost emitter (main+0x1b37958, main+0x1b3882c, **earlier**), wind capsules,
  water objects. **read** for the cell fire, **not read** for the others.

### 1.2 Fire on grass: the ground cells

- The chemical system object S holds a pool of cell records (S+0x18) and a list of live cells (head S+0xc6d0). A cell
  is named by a 32-bit id: two 16-bit grid coordinates. Its place follows from the id alone (scale S+8, origin S+0xc
  and S+0x10, cell size S+0xc700), so the same id is the same patch of ground in every game. **read**
  (main+0x1f100e8)
- A cell record R: state byte R+0xa (0 new, 1 heating, 2 burning, 3 burnt), flags R+9 (bit 1 from the terrain: a
  cell that burns longer), time in the state R+0x10, wetness R+0x28 (0 to 1), centre R+0x2c, the inputs of this frame
  (fire contacts R+0x80 and R+0x81, water contacts R+0x82, a wind vector R+0x68), heat R+0x84 (ignites at 30), burn
  time R+0x88 (ends at 300, later for a long-burning cell), burning frames R+0x8c, its elements' holder R+0xb0. **read**
- Once a frame the driver main+0x2dd070(S, ctx, step) runs, for every live cell, the state step main+0x1f105e0, frees
  one cell idle for more than 45 units, then runs the spread main+0x1f10adc for every cell. Times are in frames at 30
  a second: a contact heats a cell for one second before it burns, and it burns ten seconds. **read**
- The state step: wetness rises with water contacts and with rain (a factor in the step record) and falls otherwise;
  heat rises by the contacts of the frame and falls without them; at 30 the cell burns unless it is wet or covered;
  when it starts to burn the step makes the cell's two elements (the `GrassFire` one, main+0x1f1202c, and a second
  one, main+0x1f121d8, **not read**: by its place in the code the warm air above a fire) and puts them in the holder;
  when the state leaves 2 it removes them. So a cell whose state byte and timers are written from outside makes and
  removes its own fire, effect and sound. **read**
- The fire element shows `Chemical_Fire_Grass` (or `..._OnBoost`), sized by the cell, and registers the fire with a
  second module (main+0x202d6b4; by its strings the burning sound). **read**
- The spread (main+0x1f10adc), for a burning cell and each of its eight neighbours: the neighbour is heated when
  `distance * random < (wind term + 1) * 0.75`, with the game's global random generator (main+0xc8b920) and the
  world's wind (direction and speed, plus the wind an element blew on the cell). Every tenth burning frame it also
  sends a record (centre, radius, strength) to listeners through main+0x1f14f9c (**not read**: who listens; by the
  strings the grass that is burnt away and the navigation mesh's `NavMeshGrassFire`). **read**
- Everything that touches the ground reaches the cells through one function, main+0x1f0eb94(S, ctx, shape, object):
  the object is a chemical object (two classes seen in runs, main+0x3625660 and main+0x36295d0), not an element, and
  a burning cell's own fire is one: this, more than the spread function, is how a fire grows in a run. By the
  object's material type it heats cells (fire, main+0x1f0effc, which also draws a random number), blows on
  them (wind, main+0x1f0f71c), or wets them (main+0x1f0fb78, main+0x1f0fd6c). **read**
- Random or different per game: the spread's random draw, the random draw of a fire contact, the wind, the rain. Two
  games that both simulate a fire spread it differently. **read**

### 1.3 Element state on an actor

- An element's status ("kind", byte el+0x7d): 0 normal, 1 frozen, 2 burning, 3 burnt out, 4 detonated. The element's
  own update (main+0x547a2c, vtable slot +0x158) runs its timers, evaluates the requests and the reactions of the
  frame and stores a changed kind there, with the cause in el+0x7e and after; a new kind 2 goes through the fire start
  main+0x79e6d0. The flame effect, the blackboard flag and the sound follow the kind. **read** (this is the step the
  contraption research could not find)
- Electricity and wetness are not kinds. They are in the element's record (a charge timer, a wet amount 0 to 1) and
  published as flags: an element is electrified when bit 1 of el+0x95 is set (main+0x1b3087c); the wet amount is
  main+0x26253c(component, -1). **read**
- Reactions come in through main+0x1906768(element, 0, ctx, reaction), from the two passes main+0x18d3080 and
  main+0x18d31c4 that walk what touches the element. A handler only adds to the frame's accumulator; the update
  decides. **read**, the handlers **earlier**
- The game's own requests, used by its behaviour nodes: ignite main+0x1b34a44(comp, 2, -1); extinguish
  main+0xb2b804(comp, 0, -1); burn out main+0xb2b804(comp, 2, -1); detonate main+0x1b34b0c; dry main+0x1b35038;
  freeze main+0x18dcf9c(state block, v); full electric charge main+0x1b35098(comp); full wet (request bit 1 of the
  state block's word +0x42). A request written before the element's update is consumed by it in the same frame.
  **read**
- The game's own switches on a component (bits of the word at comp+0x20): bit 32 "does not interact with the world"
  (node run main+0x250cc48), bit 29 "unaffected by elements" (main+0x1b35118), bits 28 and 15 "no burning" and "no
  burnt" (**earlier**). Where the bits are tested: **not read**.
- Burn damage: an actor loses life by its own damage step from the elements that touch it (main+0x327968,
  **earlier**); wood burning alone loses none and ends as death kind 9 when its element is burnt out.
- Enemies and Links show burning, shock, wet and frozen through the condition component `[[actor+0x228]+0x1a8]`,
  whose flags follow the chemical state (**earlier**, enemy research); "frozen in ice" is two words of
  `[[actor+0x228]+0x3a0]`; the ice look is a model float. The step from element to condition: **not read**.

### 1.4 Area effects

| Effect | What it is | Read? |
| --- | --- | --- |
| Updraft over a fire | by the strings (`Chemical_Updraft`, `IsBurnUpdraft`) an element made with the fire; for a cell, the second element of 1.2 | not read |
| Wind of fans and gusts | `Chemical_WindCapsule` actors and wind elements; they reach cells through the contact entry | earlier, in part |
| Flame and frost emitters | a pool of 35 elements and 35 attack sensors per emitter, each game's own | earlier |
| Shock emitter, electrified metal and water | electricity as element state passed between touching elements (`Chemical_Electricity`, `Chemical_ElectricWater`, `Chemical_ElecConnect`) | not read |
| Ice blocks, frozen water | strings `IceActor`, `ExpandActorIce`, `Chemical_IceMagic`, `Chemical_IceMelt*`; the frozen kind of an element | not read |
| Lightning | a strike is main+0x21c43b4(manager, position, level, ...): it makes the effect and records the strike; asked by two behaviour nodes and by a lightning arrow; the weather's own strikes | earlier; the weather's caller not read |
| Explosions | reactions of type blast and the detonate request; a bomb's blast is the contraption sync's death kind 8 today | earlier |
| Rain, water | wets cells (the step's rain factor) and elements (the wet amount); a wet thing does not burn | read for cells |
| Weather | each game has its own: no co-op code shares it | read (the co-op code) |

### 1.5 What the co-op code shares today

- A contraption's burning parts: the game that runs a build states which parts burn; a copy that differs is asked to
  ignite or go out with the game's requests (runtime/coop_ctn.c, "Burning parts"). A burnt-out part ends by the death
  relay. Not kept from igniting by local causes.
- Other players' arrows and thrown objects are frozen copies: their update does not run, so their chemistry does
  nothing. A fire arrow lights grass only in the shooter's game.
- Enemy copies (the enemy sync): motion, animation, life, ragdoll and death come from the owner. No element state.
- Nothing else: not the ground cells, not the element state of objects and enemies, not weather.

## 2. Design

### 2.1 What is stated, and by whom

| Thing | Identity | Who decides | Stated |
| --- | --- | --- | --- |
| Ground cell | the cell id | the game in which it was first heated by a cause that game decides (2.3); when two games claim one cell, the lower player id | state, heat, burn time, wetness |
| Enemy the enemy sync knows | its placement hash | the enemy's owner (`en_cell_state`) | per element: kind, electrified, wet |
| Placed object (a tree, a crate, a brazier) | its placement hash `[[actor+0x370]+0x88]` | the game that first states a state other than the object's rest state; the claim ends when it rests again | the same |
| A build's parts | the entity sync's | the game that runs the build | burning (the existing code; unchanged) |
| A projectile mirrored by the enemy sync | the enemy sync's | nobody here: the mirror is inert (2.4); what the real one lights is stated as the cell or the object it lit | nothing |

Left for a later step, and said so in the specification: things made at run time that are not enemies or parts
(dropped items, weapons in the hand, each Link shown on the other games' stand-ins), lightning, ice blocks and frozen
water, electricity through water, and weather.

### 2.2 Why state and not causes

Sending the cause (an ignition here, a lightning strike there) and letting every game play it out fails for the same
reason it failed for enemies: the spread draws random numbers and reads wind and rain that differ per game, so two
games given the same cause burn different ground. A cell's whole state is four numbers and an element's is a byte and
two flags, so the state itself is cheap to send, and a game that joins late or missed the cause gets the same thing as
one that saw it. The game makes the visible part (flame, sound, burnt grass, the warm air) from the state by itself,
which was read in 1.2 and 1.3.

### 2.3 Never deciding twice

A game that shows a copy must not let the copy decide anything:

- **A copied cell** has its state, heat, burn time and wetness written before the driver runs, every frame, so
  whatever touched it locally is overwritten. Its spread is run (the burn broadcast in it makes the grass burn away),
  but the heat it gave its neighbours is taken back, so only the deciding game's spread lights the next cell.
- **The fire element of a copied cell** is kept from heating cells through the contact entry: otherwise a copied fire
  would light the cell beside it in the copying game, which would then claim it.
- **A copied actor** (an enemy copy, an object another game has claimed) takes no local reaction: the reaction entry
  drops reactions into its elements. Its state is given by the game's requests before its component's update.
- A copied fire still hurts and lights what the copying game itself decides: its own Link, the objects it has
  claimed, the enemies it owns. That is the meeting point agreed with the enemy sync: each game decides the hits on
  its own player and on the enemies it owns.

### 2.4 Inert things

`coop_elem_inert(actor, on)`: the actor's elements neither take nor give reactions and do not touch the cells. The
enemy sync calls it for a mirrored projectile, so that only the real projectile, in the game that owns the enemy,
lights anything; what it lit is then stated as a cell or an object like any other fire. Agreed with the enemy sync
session on 2026-11-16: the mirror is the same actor class made by the normal create, its damage is kept against that
game's own Link only, its identity is (owner's incarnation, serial) and its owner states its end.

### 2.5 Time

A cell's timers run in every game by its own frames; each statement sets them again, so a copy is behind its owner by
the network delay only and both burn out within a fraction of a second. A game that is paused sends nothing; its cells
and objects are held as they were last stated, and are not timed out, while its heartbeat says paused or loading.

### 2.6 Joining, loading, leaving

- A statement is the whole state, sent again several times a second in latest-only slots. A game that joins, or
  comes near after a load, takes the next one: there is no history to replay.
- A game in a load has no chemical system: it drops what it follows and takes the statements again afterwards. Its
  own fires end with its world, and the others see them end when its statements stop naming them.
- When the deciding game leaves or falls silent for 3 seconds, what it decided is orphaned: the remaining game with
  the lowest player id that shows the thing takes it over as it stands and states it from then on.
