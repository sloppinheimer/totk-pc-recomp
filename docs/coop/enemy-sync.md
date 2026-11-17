# The enemy sync, as built

This page says what the enemy sync does now, read from the code on `main` (2026-11-17). The plan for
where it goes next is [PLAN.md](PLAN.md). The older design and spec pages were removed on 2026-11-17 (their game facts are in
[facts/](facts/README.md); the pages themselves are in git history, commit 2604efb8); a "B.NN" or "S n.n" in a code
comment names one of their sections.

Code is cited as file and function. "Not confirmed" means the code says it but no run or play session that this page
could check shows it.

## In one paragraph

Every enemy that two games both have is run by one game, its **owner**. In every other game the same actor is a
**copy**: the game's own actor of that enemy, with its thinking switched off, put each frame where the owner says, and
given the owner's animation commands. What the owner's enemy does to the shared world (dies, drops loot, shoots,
throws, picks up) is done for real in the owner's game and replayed or carried to the copies. A player's blow on a copy
is decided in that player's game, and the life it took is sent to the owner. The owner changes when another player
comes nearer, strikes the enemy, or the owner leaves, loads or pauses.

## Files

| File | What it holds |
|---|---|
| `runtime/coop_enemy.h` | Shared types: `Enid`, `EnState` (what STATE carries), `EnCell` (per actor), `En` (per entry); message numbers 60 to 89. |
| `runtime/coop_enemy.c` | The table, identity, ownership, the reliable messages, peers, the record, and the order of phase 0. The kinds list (`kind_of`). |
| `runtime/coop_enemy_hooks.c` | The entry hooks: `coop_enemy_shared` (actor calc and action request) and `coop_enemy_hook` (every other `COOP_HOOKED_EN` address). |
| `runtime/coop_enemy_copy.c` | The copy switch (`en_copy_hook`), placing and animating copies (`en_copy_precalc`), STATE both ways (`en_copy_phase0`), equipment, AI memory, attention, the brain hold. |
| `runtime/coop_enemy_hit.c` | Life, hits, death, tombstones, the outcome of a copy's blow on this game's Link. |
| `runtime/coop_enemy_shot.c` | Shots (prepare and shoot replayed on the copy), a Shooter put to sleep, the "foreign address" log. |
| `runtime/coop_enemy_fx.c` | Effects started by AI nodes, magic weather, a weapon picked up, the ambient-spawn veto. |
| `runtime/coop_enemy_arm.c` | Attacks armed by AI nodes. |
| `runtime/coop_enemy_pose.c` | Ragdoll poses sent as bones. |
| `runtime/coop_enemy_loot.c` | Drops and thrown weapons noted for the object mirror; kill counters refused on copies. |
| `runtime/coop_enemy_act.c`, `coop_enemy_act_deny.inc` | The action layer (off by default). |
| `runtime/coop_contact.c` | Which contacts of a blow count, and where. |
| `runtime/coop_spawn_sync.c` | Enemies made at run time, made in the other games too. |
| `runtime/coop_mount.c` | Horses and land animals as kinds of this sync (off by default). |
| `runtime/coop_check.c` | The mismatch checker's record of what this game shows of every listed actor near a Link (not part of the sync: it reads the game, never the sync's tables). |

## Threads

- **The net tick** (`coop_enemy.c coop_enemy_net`, called from `coop_spawn.c coop_tick` at every gamepad poll, also in
  a menu or a load): peers, join and leave, the reliable messages, offers that end, INDEX, the record.
- **Phase 0** (`coop_enemy.c coop_enemy_phase0`, once a frame at GameDataMgr::calc, outside the parallel actor
  update): binds actors to entries, claims, offers, takes over, reads and sends STATE, fills what copies show.
- **The actor's own update thread** (`coop_enemy.c coop_enemy_calc` -> `coop_enemy_copy.c en_copy_precalc`, and the
  function-entry hooks): reads only its own map cell.

Phase 0 and the net tick hold `en_lock`. Some hooks on actor threads take it too (the outcome, the action owner, FX,
ARM, SHS), and so do this game's blood moon (`en_moon_local`) and the wild-herd hook (`coop_mount.c
coop_mount_herd_hook`).

## Which actors are enemies here

`coop_enemy.c kind_of` takes an actor by the start of its name: `Enemy_Bokoblin`, `_Moriblin`, `_Horablin`,
`_Lizalfos`, `_Chuchu`, `_Keese`, `_Octarock`, `_Wizzrobe`, `_Zonau_Robot`, `_Zonau_Golem`, `_Assassin`, `_LikeLike`,
`_Lynel`, `_Treant`, `_Zombie_`, `_Toby`, `_Giant`, `_Golem`, `_Mogurudo`, `_CaveMaster`, `_Drake`,
`_Zonau_BlockMaster`, `_DungeonBoss_`, `_Ganondorf`, `_PhantomGanon`, `_Kohga_`, `_MiddleBoss_`, `_Sandworm`,
`_CDungeon_`, `_Bee_Swarm` (in the code's words, "every whole enemy"; spec B.32 says what each was seen to do). Weak points, dummies, bullets, tornadoes, thorns, event starters and secret
stones are left out. With `TOTK_COOP_MOUNTS=1`, horses and land animals are taken too (`coop_mount.c
coop_mount_kind`). `TOTK_COOP_EN_KINDS=<prefix>,...` adds prefixes for a test.

## Identity

An entry's id is `Enid` (`coop_enemy.h`):

- A **placed** enemy: its placement hash (`coop_enemy.c en_hash_of`, `[actor+0x370]+0x88`) and the session's **life
  number**. The life number starts at 1 and rises by one at each blood moon (`coop_enemy.c en_moon_local`,
  `moon_apply`; MOON tells the others). A moon drops every tombstone, resting record and entry of the life before.
  A game told of a moon less than five minutes ago does not raise the number again for its own moon.
- An enemy **made at run time**: the key the spawn sync gave it (`coop_spawn_sync.c coop_world_spawn_key`) and the
  marker `EN_RT`.

If two games' actors for one placement have different names, the placement is **unsynced** for the session
(`coop_enemy.c unsynced_set`, UNSYNCED): each game keeps its own.

## The life of one enemy

### 1. Seen

Its first calc (main+0x41bb20) goes through `coop_enemy_hooks.c coop_enemy_shared` to `coop_enemy.c
coop_enemy_calc`, which makes a map cell (`map_touch`) if `kind_of` takes the name. While a session is on, the cell
starts as **NEW** (held); with no session it is **OWN** (the game's own, not synced).

A new actor thinks for up to 30 frames until its interrupt graph has started (`coop_enemy_copy.c en_copy_precalc`,
the `warm` flag). Only then may the switch turn it off. Switched off earlier, it lost its interrupt graph for good.

### 2. Claimed or bound

Phase 0 looks at every NEW or OWN cell (`coop_enemy.c cell_resolve`):

- An entry for its id exists: the actor is bound as **OWNED** (this game runs it) or **COPY**.
- Its placement is tombstoned (it died in another game this life): the actor is put away (`coop_enemy_hit.c
  en_hit_precalc`, the placement's own dead call main+0x195321c, then a delete request).
- Nobody has it and this game runs it (its calc ran in the last 30 frames, its far gate is open, a graph runner was
  entered in the last 90 frames): this game **claims** it (CLAIM, DECL) at epoch 1 (or one above a resting record's).
  A new actor is first held for the session's hold time (`coop_sess_hold_s`, state **HELD**), standing still.

Claims wait after a join or a load (`coop_enemy.c claims_allowed`): 2 s after the load ended, 2 s after each peer was
ready (half a second more for the game with the smaller incarnation), and until each peer has stated everything
(SYNC_END) or 5 s have passed.

An enemy made at run time in this game for another game's announcement is not claimed here for 5 s: its maker
claims it, and this actor becomes the copy (`coop_spawn_sync.c coop_spawn_made_for_other`).

### 3. Owned

Ownership is an (epoch, owner) pair that only rises. `coop_enemy.c en_apply_owner` is the one place it changes: a pair
is taken only if its epoch is greater, or equal with a greater incarnation.

Each frame the owner reads its enemy (`coop_enemy_copy.c en_copy_phase0`, pass 1) and sends **STATE** (unreliable,
the latest per enemy): place, rotation, velocity, life, death word, the animation slots (command hash, rate, the
blend clocks), what each equipment slot holds, what is fused to it, whether it is drawn or put away and shown, the
ragdoll state, the model's hidden mask and looks, the brain verb and target player, the battle-info words, the swift
attack bit, the character controller's move state and velocity, the attack layer bytes, the magic weather type, two AI
memory bools. It is sent every frame while a player is within 40 m or the enemy moved, for 3 frames after a change,
else once a second. A jump of over 1.5 m in a frame counts as a teleport and is not smoothed.

The target is kept from flickering (`en_copy_phase0`, B.22): a change from one player to another less than 3 s after
the last change is put back.

### 4. Copied

A copy does not think. `coop_enemy_copy.c en_copy_hook` turns away its behaviour-graph runners (main+0x50b404,
0x2beb80, 0x42e0c8, 0x44ff30) and its action request (main+0x50ad74), and refuses any node tick (main+0x1920) on its
thread. `four_writes` empties its interrupt graph's runner before each calc. Its main body is made static.

Before each calc, `en_copy_precalc`:

- **Place.** Phase 0 picks the two states around T, the time this game shows that owner at (`coop_sess_clock`), and
  interpolates (Hermite on place, normalised lerp on rotation). With no later state, the newest is held. `put` writes
  the matrix (COOP_FORCE_MATRIX) and the actor's own transforms.
- **Animation.** `slots_apply` plays the owner's command in each slot by the game's own play request (main+0x4dc628)
  when it differs, at most 5 times a second per slot. Clocks more than 3 units off are written; smaller errors bend the
  rate by up to 10 %.
- **Life and death.** `coop_enemy_hit.c en_hit_precalc` (below).
- **Equipment.** Every 16 frames phase 0 compares what the copy holds with what the owner's enemy holds. After three
  differing looks, a thing the owner does not hold is deleted, and a missing one is made by the game's equipment create
  (main+0xaf100c), at most once in 2 s and twice per slot. A thing with the wrong fused attachment is let go of and made
  again. Draw and sheathe follow the owner's (main+0x4f59ec, at most once a second). The slot's shown byte follows the
  owner's. A drop is told at once (EQUIP) and the copy lets go at once.
- **Throws.** When the owner's enemy throws (EQUIP, thrown), the copy makes the same throw with the owner's request
  (main+0x1b73be4). The thrown thing in this game is this game's own; it can hurt this game's Link. It is deleted once
  it rests (`coop_enemy_loot.c en_loot_phase0`).
- **Shots.** `coop_enemy_shot.c en_shot_precalc` replays the owner's prepare (main+0x1bf9af8) and shoot
  (main+0x1bfbc44) on the copy's own Shooter record of the same key and index, at T. The 0x1980-byte request block
  travels with its zeros squeezed out; the bone name a prepare holds the shot at travels as text.
- **A Shooter put to sleep** (B.36): `en_shs_precalc` makes the owner's calls of main+0x1bfa698, 0x1bfa87c and
  0x1bfa4e0 on the copy's Shooter. Those hooks were registered with a wrong address until commit 6287624e; this replay
  only started running then. Not confirmed in play.
- **AI memory.** `mem_write` gives the copy's blackboard the owner's `IsMagicWeather` and `IsShootMagicObj`, so a
  game that takes the enemy over finds them.
- **Prompts.** `attn_copy` gives the copy the owner's attention reasons (Sneakstrike, lock-on, auto-aim), through the
  game's own setter (main+0x4e603c), every 8 frames.
- **Effects.** `coop_enemy_fx.c en_fx_precalc`: effects and sounds an AI node started by key, one-shot or held, made
  on the copy's own effect and sound users. Effects that the animation starts happen on the copy by themselves and
  are not sent. The owner's magic weather is asked of the weather manager for the copy.
- **Node-armed attacks.** `coop_enemy_arm.c en_arm_precalc`: while the owner keeps sending ARM, the copy calls the arm
  function (main+0x1bf20cc) once a frame with the owner's request, and writes the attack layer and hit-receive bytes.
  Attacks armed by the animation (main+0x42ec98) happen on the copy by itself, since its clip plays.
- **Looks.** The model controller's looks (ice, the death effect, the fade, the hidden mask, model enabled) and the
  hidden mask of the copy and what it holds follow the owner's, each with its dirty bit.
- **Ragdoll.** A copy starts no ragdoll of its own (`en_copy_hook`, main+0x1bddcb0 refused). While the owner's enemy
  is in any ragdoll state, `coop_enemy_pose.c` sends its skeleton every other frame (POSE) and the copy's skeleton is
  overwritten with the two newest, blended, after its model phase.
- **The brain hold** (B.21). When the owner states a verb, the copy's brain is asked for it with force and its target
  slot 0 is filled with this game's actor of the player the owner's enemy fights (`brain_hold`). This runs brain
  procedures on a copy (the head's look-at). It is not asked of a brain whose graph has not started (B.37).

What a copy does run: its calc, its animation (with the attacks and effects its clips start), its damage step on this
game's blows, its equipment user, its own physics on a static body, its death once asked, its blows on this game's
Link.

What a copy does not run: its brain, utility and scheduler graphs, its interrupt graph, any AI node, its own ragdoll,
its drops, its kill counters.

### 5. Hit

**A player's blow on a copy.** The game of that player applies the blow to its copy as the game wrote it, so the blow,
the flinch and the numbers show at once. Before the next calc `en_hit_precalc` sees the copy's life below what it last
wrote; the difference goes into `hit_dmg`. Phase 0 sends it to the owner as **HIT** (reliable, `en_hit_phase0`). For
1.5 s the copy shows the lower of the stated life and its life after that hit, so the life does not jump back while
the hit travels.

The owner adds the points (`en_hit_msg`), writes its enemy's life, asks a flinch (byte +0x149 of the ragdoll
component) and at 0 life asks the death (main+0x1b4b738). A HIT that reaches a game after the enemy changed owner is
passed on, at most twice.

**Contacts** (`coop_contact.c contacts_filter`, run at each damage component's hit update main+0x300a30):

- On a **stand-in**: every contact is removed. Its player's own game has the blow on the real Link.
- On an enemy this sync knows: a stand-in's contacts are removed (that player's game counts the same blow on its own
  actor).
- On a **copy**: contacts of other enemies, what they hold, their shots, and devices of another player's contraption
  are removed too. The game that runs the struck enemy counts those.
- `TOTK_COOP_CONTACT_FILTER=0` turns all of this off.

**A copy's blow on this game's Link.** It is built in this game from this game's copy, as any enemy's blow, from
an attack armed by the copy's clip, by the ARM replay, or by the action layer when it is on. How it ended (hit, guarded, guarded at the right moment, avoided) is
told to the owner as **OUTCOME** (`coop_enemy_hit.c coop_enemy_outcome`). On a guarded outcome the owner's enemy
waits 10 to 15 s before its next attack, as the game's own node does after a guard (`en_hit_msg`).

### 6. Dying

When the owner's death word becomes valid, the owner sends **DEATH** (reliable) and keeps a **tombstone** for a placed
enemy (`en_hit_phase0`). Every game keeps the tombstone; a placement's actor seen later is put away.

A copy never dies by itself. Its death component's bits 0x81 are set (no drops, no requests but the sync's). On DEATH
its death is asked with the owner's causes (main+0x1b4b738), it waits at its third phase, and it is given the "Die
ran" signal when the owner's death has passed that phase (`en_hit_precalc`). A copy's kill-counter writer
(main+0xb28394) is turned off by setting its done bit (`coop_enemy_loot.c en_loot_hook`).

Loot is the owner's: `coop_enemy_loot.c` notes each drop item as the owner's drop makes it, and each weapon an owned
enemy drops or throws. The object mirror then sends them as this game's objects (`coop_enemy_loot_take`).

When the owner's actor is gone, **DEATH_END** frees the entry everywhere (`en_dead`). A copy still dying ends by
itself.

### 7. Handed over

`coop_enemy.c handover` (on unless `TOTK_COOP_EN_HANDOVER=0`) offers an enemy this game runs to another player's game
(OFFER to that game) when:

- that player's Link is within 35 m of it and at least 6 m nearer than this game's Link, and this game has had it for
  at least 6 s, and no offer of it failed in the last 10 s (to any game: `no_offer_until_ns` is the enemy's); or
- that player struck it in the last 5 s (HIT), this game has had it for at least 3 s, that player's Link is within
  80 m, and it is nearer than this game's Link or this game's Link is over 15 m away.

Never in the middle of an interrupt action, and never while the enemy is not drawn (a Wizzrobe in its unseen flight).
An offer not taken in 2 s is withdrawn, and not made again for 10 s.

When this game no longer runs the enemy (for half a second) or no longer has its actor, it offers it to everyone. In
a load it offers everything at once. An offer to everyone that nobody takes in 2 s lets the enemy **rest**
(`gone`, GONE and REST records).

A copy's game **takes** the enemy (`coop_enemy.c own_pass`, `take`, epoch + 1) when it runs it and:

- its owner offered it in the last 3 s; or
- its owner left, or is in a load; or
- its owner has been paused for 5 s, or for 1 s while this game's player hit it in the last 5 s (not a horse whose
  rider is in a menu).

On a take-over (`en_copy_precalc`) the interrupt graph gets its saved fields back, the body becomes dynamic again, the
copy and what it holds are drawn again, the AI memory bools are written, and the brain is forced to the owner's verb
and target for 4 frames. After that a narrower hold turns away only the brain's idle verbs and the reset of the target
slots, for up to 600 frames, until the brain's own start has come and gone.

A game whose world stands (a menu: no graph runner entered for any enemy it runs for a second) says PAUSED in its
heartbeat (`en_world_stands`, `coop_sess.c`), which is what lets another game take a fight over.

## Joins, loads and checks

- A peer that comes, or runs again after a load, gets a **burst** (`coop_enemy.c burst`): every claim, declaration,
  unsynced placement, tombstone and resting record of this game, the command names, then SYNC_END. Entries of that
  peer it did not name again are let go (`stale`).
- A game in a load lets its copies go and offers its enemies (`deactivate(1)`).
- Every 2 s each game sends an **INDEX** (count and hash of what it runs). Two differing INDEXes in a row and the peer
  is asked for everything (NEED with a zero id). An entry with no declaration is asked for by NEED.
- Every game must run this sync. A peer whose heartbeat lacks the enemy flag stops the sync for everyone while it is
  there, with a notice on screen (`coop_notices.c coop_notice_enemy_sync`).

## Enemies made at run time

`coop_spawn_sync.c` (on unless `TOTK_COOP_SPAWN_SYNC=0`):

- An enemy with no placement in the map (a night's Stalkoblins, a spawner's) is announced to the others (SPAWN: key,
  name, place, rotation) from the game's create hand-over (`coop_actor_created`) and a scan twice a second
  (`spawn_scan`). Part actors of an enemy (weak points, dummies, bullets) are announced too.
- Each other game makes one by name at that place (`spawn_take`, `coop_request_create`), or gives the key to its own
  actor of that name within 3 m.
- When the maker's enemy goes away without dying (unloaded), SPAWN_GONE deletes the others'. An enemy seen dead is left
  to the enemy sync, whose death the copies are still playing.
- A copy made here for another game is deleted when it is more than 1000 m from every player (`far_sweep`,
  `coop_far.c`; `TOTK_COOP_FAR=0`, `TOTK_COOP_FAR_M`).
- These packets ride the HIT channel (`coop_net_hit_send`), not the enemy stream. Nothing orders a SPAWN against a
  CLAIM for the same enemy; the 5-second wait in `cell_resolve` covers it.

A game's ambient placement (main+0xa80f28) is not asked for an enemy name when another game's enemy of that name
already stands within 60 m of this game's Link (`coop_enemy_fx.c en_fx_hook`). Wild horse herds are made in one game
only (`coop_mount.c coop_mount_herd_hook`).

## Messages

All go through `coop_msg.c` on the enemy stream. "Reliable" ones go through the sync's own per-peer outbox
(`coop_enemy.c en_send`, `out_flush`), in order.

| No. | Name | Kind | What |
|---|---|---|---|
| 60 | STATE | latest per enemy | the owner's state of one enemy (`coop_enemy_copy.c en_copy_phase0`) |
| 61 | ARM | reliable | a node-armed attack request (`coop_enemy_arm.c`) |
| 62 | DECL | reliable | name, kind, life, pose of a claimed enemy |
| 63 | PLAY | reliable | a command or held thing's name for its hash, once per name and peer |
| 64 | SHS | reliable | a Shooter put to sleep (`coop_enemy_shot.c en_shs_hook`) |
| 65 | HIT | reliable | life a player's blows took from a copy |
| 66 | ACT | reliable | the action layer's request (off by default) |
| 67 | OUTCOME | reliable | how a copy's blow on a Link ended |
| 68 | DEATH | reliable | the owner's enemy began dying |
| 69 | DEATH_END | reliable | the owner's actor is gone |
| 70 | MOUNT | latest per entry | horse gait, tack, rider (`coop_mount.c`, off by default) |
| 71 | TOMBS | reliable | tombstones, in a burst |
| 72 | CLAIM | reliable | (id, epoch) rows; the first of a burst marks the rest stale |
| 74 | REST | reliable | resting records |
| 75 | OFFER | reliable | offered to one game or to all |
| 76 | GONE | reliable | let go: it rests |
| 77 | NEED | reliable | ask for one entry, or everything (zero id) |
| 78 | INDEX | latest | count and hash of what a game runs, every 2 s |
| 79 | SYNC_END | reliable | end of a burst, with the life number |
| 80 | PROJ | reliable | a prepare or shoot with its request block |
| 82 | EQUIP | reliable | the owner's enemy dropped or threw what a slot holds |
| 83 | MOON | reliable | a blood moon: the new life number |
| 84 | UNSYNCED | reliable | a placement whose actors differ by name |
| 86 | POSE | latest per enemy | ragdoll bones (`coop_enemy_pose.c`) |
| 87 | ATTN | reliable | attention reason bytes |
| 88 | FX | reliable | an effect by key, one-shot, held start or stop |
| 89 | PICK | reliable | the owner's enemy picked up a weapon: the other games delete their lying one |

73, 81 and 85 are free. SPAWN and SPAWN_GONE (the spawn sync) and the contact filter use the HIT channel instead.

What a message may do is checked against who sent it: ATTN, EQUIP and PICK are taken only from the game that owns
that enemy here, DEATH and DEATH_END never from another game than its owner here (an enemy with no entry yet is taken
as stated); PICK deletes only a `Weapon_`; MOON and SYNC_END raise the life number by 64 at most;
a PLAY name must hash to the hash it is given for; a copy is given no equipment named as a player, character, enemy,
animal, chest or armor. SPAWN is taken at most 32 times a second from a player and, when its last place is under 2 s
old, only within 500 m of it, with a finite place and rotation.

## Settings

For players and testers (all read with `cfg_get`, so `totk.ini` can set them as `coop_...` keys):

| Setting | Default | What |
|---|---|---|
| `TOTK_COOP_ENEMY` | on (`0` off) | the whole enemy sync; every game in the session must agree |
| `TOTK_COOP_EN_HANDOVER` | on (`0` off) | the hand-over by nearness and by a blow |
| `TOTK_COOP_SPAWN_SYNC` | on (`0` off) | enemies made at run time made in the other games |
| `TOTK_COOP_CONTACT_FILTER` | on (`0` off) | which contacts count where |
| `TOTK_COOP_EN_FX` | on (`0` off) | node effects, weather, pick-up, ambient veto |
| `TOTK_COOP_EN_ARM` | on (`0` off) | the node-armed attack replay |
| `TOTK_COOP_EN_POSE` | on (`0` off) | ragdoll poses |
| `TOTK_COOP_EN_SHS` | on (`0` off) | the Shooter-sleep replay |
| `TOTK_COOP_EN_ACT` | off (`1` on) | the action layer (below) |
| `TOTK_COOP_MOUNTS` | off (`1` on) | horses and land animals |
| `TOTK_COOP_FAR`, `TOTK_COOP_FAR_M` | on, 1000 | deleting far copies of run-time enemies |
| `TOTK_COOP_DEBUG` | off (`1` on) | the owner's and copy's `coop: en: pos:` lines that `tools/coop/en_report.py` reads |

For finding a fault (tests):

| Setting | What |
|---|---|
| `TOTK_COOP_EN_KINDS=<prefix>,...` | more kinds by name prefix |
| `TOTK_COOP_EN_NOSWITCH=1` | ownership runs, no copy is switched off or moved |
| `TOTK_COOP_EN_TEST_SKIP=<letters>` | leave a part of the copy out: `b` take-over brain, `v` copy brain hold, `s` static body, `r` ragdoll refusal, `h` life write, `d` death switches, `t` target no-flicker |
| `TOTK_COOP_EN_TEST_BLIND=<s>` | take none of this game's own actors for that long after the session began |
| `TOTK_COOP_EN_TEST_SHADOW=1` | keep this Link where the first other player's stand-in stands |
| `TOTK_COOP_EN_TEST_KILL=<s>` | the nearest owned enemy loses all its life |
| `TOTK_COOP_EN_TEST_HIT=<s>[,<points>]` | a made-up hit on the first copy |
| `TOTK_COOP_EN_TEST_OUTCOME=<s>[,<class>]` | made-up outcomes sent to an owner |
| `TOTK_COOP_EN_TEST_POSE=1`, `TOTK_COOP_EN_TEST_POSEROOT=1` | bone place log; write the root from the pose (known to throw copies into the sky) |
| `TOTK_COOP_EN_TEST_GMC=<prefix>` | model controller dumps (read per cell inside `held_rebuild`) |
| `TOTK_COOP_EN_TEST_SHOTS=1` | log Shooter entries |
| `TOTK_COOP_TEST_FAR_COPY=<s>[,name]` | a made-up SPAWN, to test `far_sweep` |

## Checks

The mismatch checker judges what both games show. With `TOTK_COOP_DEBUG=1` each game writes `logs/check.jsonl`
(`runtime/coop_check.c`), read from its own memory: once a second every actor of a listed kind within 120 m of a
Link (name, place, rotation, life, actor state, death word, ragdoll state, drawn, each animation slot's command and
clock, each equipment slot's held thing and state), and every frame the place, rotation and first slot of those
within 40 m, with the attack types it has armed (the arm call main+0x1bf20cc, from the clip, an AI node or the
arm replay); and as they happen, the effects and sounds an enemy starts by key (ELink main+0xa98cf8, SLink
main+0xa98d00, hooked for this) and the blows between an enemy and a Link (the contacts of a body's hit update,
main+0x300a30, read before `coop_contact.c` takes any out). An actor is named by its placement hash, else by the spawn
sync's key, else by its address (this game's alone); a Link by its player's id. `tools/coop/check.py <dir A> <dir B>` pairs the two records by time and identity and lists each difference
that holds two seconds (shown in one game only, two or no owners, place, rotation, life, death, ragdoll, drawn,
command, equipment), per copy against its owner the lag, the place error at that lag, stutter, the share of frames
with the owner's command, the animation's phase and the attacks armed with the owner, the owner's effects by key
found in the copy's game, and every blow between an enemy and a Link found in both games with the same outcome;
PLAN.md step 1 has the targets. `run_pair.ps1`,
`run_session.ps1` and `play_two.ps1` run it at the end. `run_pair.ps1 -Nudge <t>,<s>,<m>` plants a desync to show
it is found (`TOTK_COOP_TEST_NUDGE`: game B shows its first enemy copy m metres off for s seconds).

`tools/coop/en_bench.ps1` runs fixed two-game cases on the camp save and judges each from both games' logs with
`tools/coop/en_report.py`: the two tables equal at the end, no leak, the copy's actor within 1 cm of its owner's
enemy standing and 12 cm moving (95th percentile, measured where the copy's actor stands, `en_copy_precalc`'s
`coop: en: pos:` lines, `TOTK_COOP_DEBUG=1`), the same animation command in 95 % of samples. A FATAL line fails a
case; a case whose event did not happen is "not judged", and a run with nothing judged exits 2. The tables and the
shot counts are the sync's own records, kept for finding a fault; a test passes on the checker's report (PLAN.md,
rule 3), which en_bench does not run yet.

## Built but off or not confirmed

### The action layer (`TOTK_COOP_EN_ACT=1`, off)

`coop_enemy_act.c`. The owner sends its enemy's Main-slot action request (ACT) each frame it asks for a listed verb
(`en_act_owner`; default verbs 131, 145, 108; `TOTK_COOP_EN_ACT_VERBS=all` for every verb;
`TOTK_COOP_EN_ACT_FAMILY` limits it to name prefixes). The copy makes the same request on its own Main slot when this
game shows the owner at the request's time (`en_act_precalc`), after writing the owner's controller move state and
velocity (`cct_write`). Under that call (`tl_act_actor`) `en_copy_hook` lets the one call tree through the switch. A
node tick whose class is in `coop_enemy_act_deny.inc` (267 classes in 10 categories) is turned away and marked
succeeded (`en_act_tick_seen`). A play the action asks for is answered "played" and not made, since the copy's slots
already play the owner's commands (`TOTK_COOP_EN_ACT_PLAYS=1` lets them through). When the owner's verb leaves the
list, or nothing comes for half a second, the copy is asked for Wait (verb 40) once. While a copy runs an action the
ARM replay is skipped and an effect its own node started is not started again.

Not built, from the file's own list and PLAN.md: the other four slots, interrupt requests (`flags & 1` are not sent),
the form key, and every engine-level block of PLAN step 2. `TOTK_COOP_EN_ACT_ALLOW` and `TOTK_COOP_EN_ACT_DENY` change
the deny table for a test. PLAN.md lists what was measured with it.

### The Shooter-sleep replay

Runs since commit 6287624e (see above). Not confirmed in play.

### Horses and land animals (`TOTK_COOP_MOUNTS=1`, off)

`coop_mount.c`. Horses and animals become kinds of this sync, with their gait values, a horse's tack and riders in
MOUNT; a ridden horse belongs to its rider's game (`coop_mount_claim`, `coop_mount_keep`, `coop_mount_ridden` in
`own_pass`). The file says it stays off "until the spec's runs pass". No run of it is recorded on this page's sources.

## Leftovers in the code

- `runtime/coop_hooks.h COOP_HOOKED_EN` still lists game functions with no handler: the three PLAN step 2 will block
  (remove main+0x1b70a04; main+0x1bb16a0, the AI's memory of a weapon; main+0x1ba7710, the lifter's release setter;
  kept on purpose, PLAN.md step 2), and also
  main+0x2314ab0, 0x42ec98, 0x1b87a9c, 0x195321c, 0xa70a1c, 0x4e603c, 0x4f59ec and 0xc448c8. Each costs one test on every
  call of that function.
- Comments that no longer match: `coop_enemy.c`'s file comment and the net tick's first log line still say
  `TOTK_COOP_ENEMY=1` turns it on and that an older enemy sync in `coop_world.c` keeps other kinds (it is on unless `0`,
  and the older sync is gone); `cell_resolve` names `coop_world.c` for `coop_world_spawn_key` (it is in
  `coop_spawn_sync.c`); `coop_enemy_arm.c` names `EnState.bpad` (the field is `arm`); `coop_mount.c` gives MOUNT as
  type 66 (it is 70); the flinch comment in `en_copy_phase0` pass 2 describes a request no longer made;
  `coop_contact.c`'s file comment still names a device hit test and the HIT channel's contact packets, both removed.
- Logging not behind `TOTK_COOP_DEBUG`: `held_rebuild`'s "(test)" lines, the copy's hidden-mask and battle-info lines
  in `en_copy_phase0`, the action layer's SensorHit line.
