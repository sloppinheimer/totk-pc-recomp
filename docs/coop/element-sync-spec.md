# Element sync: specification

What `runtime/coop_elem.c` does, for the design in [element-sync-design.md](element-sync-design.md) (cited as D).
TotK 1.4.3; addresses are `main+offset`. Section 8 says what is built, what was measured and what is left.

## 1. Terms

- **Cell**: a record of the chemical system's ground grid (D 1.2). **Thing**: a placed actor with a chemical
  component, named by its placement hash `[[actor+0x370]+0x88]` (not 0, not all ones).
- **Mine**: this game decides it and states it. **Copy**: another game states it and this game makes it so.
- **Player id**: the relay id (`coop_net_id`), 1 and up. "Lower id wins" is by this number.
- The sync is on with `TOTK_COOP_ELEM=1` and off otherwise; with it off every hook passes through.

## 2. Hooks

All are function-entry hooks of `coop_hooks.h` `COOP_HOOKED_EL`, dispatched to `coop_elem_hook`. Those not listed
here are registered for later steps and pass through.

| Function | Use |
| --- | --- |
| main+0x2dd070 driver (x0 the system S, x1 the cell context, x2 the step record) | before it runs: copies written (4.3). After it: the cells read (4.1). The hook runs the function itself. |
| main+0x1f10adc spread (x1 the cell) | a copied cell: the function runs and the heat it gave to other cells is taken back (4.4) |
| main+0x1f0eb94 contact entry (x2 the shape, x3 the chemical object) | dropped when the shape is centred on a copied cell (4.5) |
| main+0x30efa0 component update (x0 the component) | a thing is read; a copy is given its state by requests (5) |
| main+0x1906768 reaction entry (x0 the target element) | dropped when the target is a copy's or an inert actor's element (5.4) |

The cell hooks run on the thread of the chemical system's update; the component hooks run on the actors' update
threads, several at once; phase 0 (`coop_elem_phase0`, at GameDataMgr::calc) runs on the game thread and does all
sending and receiving. Tables shared between them are under one mutex, held for plain copies only; the set of copied
elements is lock-free (5.4).

## 3. Messages

Unreliable, latest-only slots of the transport (`coop_ctr_send` with no flag, read with `coop_ctr_latest` by sender,
type and key). The key is the first 8 bytes. Types 90 to 109 are the element sync's.

```c
enum { ELM_CELLS = 90, ELM_THINGS = 91 };
typedef struct { uint64_t page; uint32_t round; uint16_t total, n; } ElmHead;            // 16 bytes
typedef struct { float x, z; uint16_t burn; uint8_t heat, state_wet; } ElmCell;         // 12 bytes
typedef struct { uint64_t hash; uint8_t n, el[7]; } ElmThing;                           // 16 bytes
```

- A statement is the sender's whole list in pages of at most 96 cells or 72 things. `round` counts statements;
  `total` is the number of entries in the round. The receiver uses the pages of one sender whose round is the newest
  it holds and that are younger than 1 s; a round is complete when its pages hold `total` entries.
- `ElmCell`: the cell's centre (R+0x2c, R+0x34), `burn` = burn time * 32 (saturating), `heat` = heat * 8,
  `state_wet` = state (2 bits) | wetness * 63 (6 bits, shifted left 2).
- `ElmThing.el[i]` for element i: kind (3 bits) | electrified (bit 3) | wet above one half (bit 4); 0x40 for an
  element that is switched off, of which nothing is said or asked.
- Sent every 6th frame while this game has anything to state, and for 2 s after the last entry went (an empty round
  tells the others that everything of this game's ended). Nothing is sent while this game is paused or loading.

## 4. Cells

### 4.1 Reading

After the driver, walk the live list (first S+0xc6d8, sentinel S+0xc6d0, link offset S+0xc6e4; at most 254 cells).
A cell is **active** when its state is 2 or 3, or 1 with heat above 0. An active cell that is not a copy is mine and
goes into the next statement.

### 4.2 Taking statements (phase 0)

For every other player whose session state is not gone: its newest complete round becomes that player's list. A
player gone (silent 3 s, or left) has no list. While a player is paused or loading its last list is kept. A cell is
**wanted** when a list names its place; when two players name one place the lower id is used.

### 4.3 Writing copies (before the driver)

For each wanted cell: its id in this game is computed from the centre as the game does it (main+0x1f0f71c:
`((x - origin) / scale + bias)` as a signed 16-bit number shifted right by S+0x3c, for x and z; scale S+8, origins
S+0xc and S+0x10, bias S+0x14), the record is found or made with main+0x1f0ff60(S+0x18, id), and a record in state 0
is set up with main+0x1f100e8(S, R, cell context). A place outside this game's window, or where this game has no
burnable ground, gives no record: the cell is not shown here, counted, and tried again every second.

Then, unless this game states the same cell and has the lower id: state, heat, burn time and wetness are written
into the record, and the cell is marked a copy for this frame. The state step that follows makes or removes the fire
element as the state says (D 1.2). A cell this game stated that a lower id states too becomes a copy from that frame.

### 4.4 A copy's spread

The spread of a copied cell runs, and afterwards the fire-contact bytes (R+0x80, R+0x81) of every other live cell are
set back to what they were before it; a cell that did not exist before gets zeroes.

### 4.5 A copy's fire and the ground

What touches the ground is a chemical object an element's update makes (class main+0x3625660), not an element. Its
+0x60 is its source (class main+0x3626048), whose +0x60 is an owner object with the chemical component at +0x10 and
so the actor (seen in a run: a fireball and Link were named this way). A contact does not reach the cells when

- that actor is inert, a projectile the enemy sync calls a mirror (`coop_enemy_shot_mirror`), or an enemy copy; or
- the chain leads to no actor (a burning cell's own fire, and the warm air over it, class main+0x36295d0) and the
  shape is centred within 0.25 m (x and z) of a copied cell that burns.

### 4.6 The end of a cell

A copy is never ended by this sync: when its owner states it burnt, or wet, it is written so and ends by the game's
own step. When no list names it any more it is no copy: it goes on from where it was last written, as this game's
own, and is stated by this game if still active. This is also what happens when its owner leaves (D 2.6): every game
that showed it states it, and the lower id is used.

## 5. Things

### 5.1 The table

Filled by the component hook: component, actor, placement hash, the number of elements (at most 7), their state as
last read, their rest state (as read during the first second after the first visit), and what is wanted. The game
updates a component only while something happens to it, so a thing at rest is visited once, when it is made: a row
lives until its actor no longer has the component (looked at every 30 frames), and the hash, which may not be there at
the first visit, is read again until it is. Actors without a placement hash, this game's Link, and actors the contraption sync drives or follows
(`coop_ctn_driven`, `coop_ctn_follows`) are not in the table.

### 5.2 Mine and copies

- An enemy the enemy sync knows (`en_cell_state`): a copy when its state is `EM_COPY`, else mine.
- Another thing: a copy while a list of another player names its hash (lower id first); else mine.
- A thing that is mine is stated while any element differs from its rest state, and for 2 s after, so that its
  copies are given the rest state before they are let go. A copy that is no longer stated goes on as it is, and how
  it then stands is its rest state. An enemy that is mine is stated while any element is not in state 0 with no flag;
  an enemy copy that nobody states is given state 0.

### 5.3 Giving a copy its state

Per element whose state differs from what is wanted, at most once a second per element: in the component hook,
before the update runs, when the component was updated in the last 3 frames; else from phase 0, where the thing is
read again first (a copy at rest is not visited by the hook).

| Wanted | Is | Request |
| --- | --- | --- |
| burning | not burning | ignite main+0x1b34a44(comp, 2, i) |
| normal | burning | extinguish main+0xb2b804(comp, 0, i) |
| burnt out | not burnt out | burn out main+0xb2b804(comp, 2, i) |
| frozen | not frozen | freeze main+0x1b34e48(comp, 4, i) |
| normal | frozen | thaw main+0x1b34f10(comp, 0, i) |
| electrified | not electrified | full charge main+0x18dcfd4(element+0x50) |

Not given: the end of a charge (it ends by the element's own timer, and a copy takes no reaction that would renew
it), wetness (stated and logged only; it is no claim either, or rain in one game would make every wet thing a copy
elsewhere), detonation.

### 5.4 A copy takes no reaction

Each visit of a copy's or an inert actor's component writes its elements into a set (4096 slots, element address and
frame number, written and read without a lock). The reaction entry drops a reaction whose target is in the set with a
frame number no older than 3 frames.

### 5.5 Inert actors

`coop_elem_inert(actor, on)` keeps a list of at most 64 actors. An inert actor is treated as a copy in 5.4, is never
asked anything, and has bit 32 of its component's word +0x20 set (the game's "does not interact with the world").

## 6. Load, pause, join, leave

- The system pointer S is taken at each driver call. When it changes, or this game is loading, the cell lists and the
  thing table are emptied.
- Join: nothing special. The newcomer takes the next round of each player.
- Leave or silence: 4.6 for cells. A thing whose stating player is gone stops being a copy and goes on as it is.

## 7. The record and the tests

- Log lines begin `coop: elem:`. Once a second while anything is stated or copied: cells mine / copied / not shown,
  things mine / copied, requests made, reactions dropped, contacts dropped. A line at each cell that starts or ends
  burning here, with its place, whose it is, and the frame stamp.
- `TOTK_COOP_ELEM_TEST_IGNITE=<s>[,<dx>,<dz>]`: at that many seconds, the game that joined first heats the cell
  that far from its Link (default 3 m ahead on x) as a fire contact does, until it burns.
- `TOTK_COOP_ELEM_TEST_THING=<s>[,<name prefix>]`: at that time the first game asks the nearest placed thing with a
  chemical component (of that name), within 150 m, to ignite.
- `TOTK_COOP_ELEM_TEST_DEAF=<s>`: until that world time this game takes no statement, as a game that arrives then.
- `TOTK_COOP_ELEM_TEST_LOG=1`: the classes of the objects kept from the ground, each once.
- `build/el_pairs.py <tag>` (a local script beside the run logs) pairs each cell event of the deciding game with the
  copy's and prints the delays; the cell lines carry the machine's clock in milliseconds for that.
- The aim (the task's four points; section 8 says which of them a run has shown): a Fire Wizzrobe's attack and a fire arrow each burn the same ground in both games,
  ending within a second of each other; a thing or an enemy burning, frozen or shocked in one game shows it in the
  other with the same end; a game that arrives late sees what burns; nothing burns in one game only after 60 s.

## 8. Built, measured, left

### Built

Sections 2 to 7 as written, in `runtime/coop_elem.c`, with one-line hooks in `coop_hooks.h` (`COOP_HOOKED_EL`) and
`coop_spawn.c` (the dispatch and the phase 0 call). `coop_elem_inert` is there for the enemy sync's mirrored
projectiles; nothing calls it yet.

### Measured (two games on one PC through the local relay, a save in tall grass 60 to 100 m from a Bokoblin camp)

- **One cell lit by the test in game A**: it burned 300 frames (10.0 s) in A; B's copy began 39 ms and ended 37 ms
  after A's. Pictures from both games show the flame and the burnt patch at the same place.
- **A Fire Wizzrobe spawned beside A's Link, both Links standing, 70 s of its attacks**: 30 cells burned and 26 burnt
  out, the same cells in both games (the sets of places are equal); of 100 paired cell events the copy followed the
  deciding game by a median of 59 ms, at most 125 ms, and 3 events had no pair within 3 s (the run's end). About 600
  contacts in A and 5,500 in B were kept from the ground (a copied fire touching its neighbours). Game B decided 8 of
  the burn events itself: its copy of the Wizzrobe makes real fireballs there, which is the enemy sync's projectile
  work; until those are mirrored and made inert, both games light cells and each states its own.
- **A game that arrives late** (B deaf for the first 29 s of its world, A's cell lit at 24 s): B showed the cell
  burning with 233 of its 300 frames gone and ended it 0.6 s after A.
- **A wooden crate asked to ignite in A** (99 m away): A's crate burned and burnt out; B's copy burned 146 ms after
  A's and was burnt out 0.44 s after A's; the grass the crate lit (3 cells) was copied. 61 reactions into the copy
  were dropped.
- Before the fixes of this step: a copied fire lit its neighbours in the copying game through the contact entry (B
  decided 27 of 66 burn events); a cell written at its owner's heat of 30 lit one frame early in the copy and
  flickered until the owner said it burned (now a heated copy is held at 29).

### Left, and known faults

- **A thing that refuses the forced ignition.** Of two crates that burned in A, the second (lit there one frame
  after the first, not by the test's request) was asked to ignite in B in every run and never did. Its element takes
  requests (bit 0 of its word +0x90 is set) and running its component's update by hand after the request changed
  nothing, so the element's evaluation refuses the forced cause for it; the reading names an owner virtual (+0x58,
  default main+0x549ec0) as the place. Until that is read, a thing that only a contact can light does not show its
  owner's fire.
- **A mirrored fireball is not inert yet.** The element sync asks `coop_enemy_shot_mirror` for every component's
  actor and would make a mirror inert, but for a Fire Wizzrobe's shot the query answers no: the projectile's shooter
  link is the ruby fused to the rod, not the enemy (reported to the enemy sync). So the copying game still lights
  cells by the mirror, and states them as its own.
- **From the review, not fixed**: enemies made at run time have no placement hash and are never stated (their copies
  are held at rest); a copy cannot pass a reaction of this game's to its owner (another player's fire on an enemy copy
  does nothing); an enemy copy is put out for a moment at each change of owner; a state that lasts (a brazier put
  out) is stated only while it changes and for 2 s after, so a game that arrives later keeps its own; the thing
  table and the element set have no report when they fill.
- **Not tested**: a player's fire arrow (no input script shoots one; by the code it is a local cause like the test's
  and the Wizzrobe's), an enemy set alight, frozen or shocked (the requests of 5.3 for frozen and for a charge have
  not run once), the take-over when the deciding game leaves, three games.
- **A few repeated burn events on copies** (52 copy events in B against 47 of A's own in the last run): not looked
  into.
- **Not built** (D 2.1): things made at run time that are not enemies (dropped and held weapons, arrows, each Link on
  the other games' stand-ins), lightning, ice blocks and frozen water, electricity through water and metal beyond
  the element flag, wetness on things, weather (rain and wind differ per game: the deciding game's rain puts a fire
  out for all, which is what state sharing gives without sharing the weather).
- **The contraption sync's burning parts** are left as they were; a part is not in the thing table while the
  contraption sync drives or follows it.
- **The review** (research notes, review1.md: 4 findings to fix, 11 that should be, 15 notes) was taken after the
  first measurements. Fixed from it: nothing is followed without a session; a burning copy is put out by wetness and
  the water contacts together (the step dries first); a copy takes no heat of this game's; a row is checked against
  its actor before every read and request; wetness is no claim; a burnt cell is never made anew for a statement (the
  two games would have kept each other's burnt cells alive); only a burning copy's place keeps contacts off; a
  loading player's fires end. After these, the Wizzrobe run gave the same 35 cells burned and 26 burnt out in both
  games, copies a median 98 ms behind (one event 1.8 s), and a later run 28 and 26 with a median 89 ms, at most
  159 ms.
