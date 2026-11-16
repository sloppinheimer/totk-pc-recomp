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
| main+0x1f0eb94 contact entry (x3 the element) | dropped when the element is the fire of a copied cell or an inert actor's (4.5) |
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
- `ElmThing.el[i]` for element i: kind (3 bits) | electrified (bit 3) | wet above one half (bit 4) | rest state:
  this entry ends the claim (bit 7).
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

A contact whose element is a `GrassFire` (vtable main+0x36bdc60) of a copied cell (its record is `[element+0x60]`)
does not reach the cells. Nor does a contact by an element of an inert actor.

### 4.6 The end of a cell

A copy is never ended by this sync: when its owner states it burnt, or wet, it is written so and ends by the game's
own step. When no list names it any more it is no copy: it goes on from where it was last written, as this game's
own, and is stated by this game if still active. This is also what happens when its owner leaves (D 2.6): every game
that showed it states it, and the lower id is used.

## 5. Things

### 5.1 The table

Filled by the component hook: component, actor, placement hash, the number of elements (at most 7), their state as
read this frame, their state when first seen (the **rest state**), and what is wanted. A row not visited for 2 s is
dropped. Actors without a placement hash, this game's Link, and actors the contraption sync drives or follows
(`coop_ctn_driven`, `coop_ctn_follows`) are not in the table.

### 5.2 Mine and copies

- An enemy the enemy sync knows (`en_cell_state`): a copy when its state is `EM_COPY`, else mine.
- Another thing: a copy while a list of another player names its hash (lower id first); else mine.
- A thing that is mine is stated while any element differs from its rest state, and for 2 s after, the last 2 s with
  the rest bit set. An enemy that is mine is stated while any element is not in state 0 with no flag.

### 5.3 Giving a copy its state

In the component hook, before the update runs, per element whose state differs from what is wanted, at most once a
second per element:

| Wanted | Is | Request |
| --- | --- | --- |
| burning | not burning | ignite main+0x1b34a44(comp, 2, i) |
| normal | burning | extinguish main+0xb2b804(comp, 0, i) |
| burnt out | not burnt out | burn out main+0xb2b804(comp, 2, i) |
| frozen | not frozen | freeze main+0x18dcf9c(element+0x50, 1) |
| electrified | not electrified | full charge main+0x18dcfd4(element+0x50) |

Not given: the end of a frozen state and of a charge (they end by the element's own timers, and a copy takes no
reaction that would renew them), wetness (stated and logged only), detonation.

### 5.4 A copy takes no reaction

Each visit of a copy's or an inert actor's component writes its elements into a set (4096 slots, element address and
frame number, written and read without a lock). The reaction entry drops a reaction whose target is in the set with a
frame number no older than 3 frames.

### 5.5 Inert actors

`coop_elem_inert(actor, on)` keeps a list of at most 64 actors. An inert actor is treated as a copy in 5.4 and 4.5,
is never stated, and has bit 32 of its component's word +0x20 set (the game's "does not interact with the world").

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
- `TOTK_COOP_ELEM_TEST_THING=<s>`: at that time the first game asks the nearest placed thing with a chemical
  component, within 15 m, to ignite.
- Done (the task's four points): a Fire Wizzrobe's attack and a fire arrow each burn the same ground in both games,
  ending within a second of each other; a thing or an enemy burning, frozen or shocked in one game shows it in the
  other with the same end; a game that arrives late sees what burns; nothing burns in one game only after 60 s.

## 8. Built, measured, left

(Filled in as the steps are built and run.)
