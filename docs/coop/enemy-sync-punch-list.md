# Enemy sync: punch list

Everything about the enemy sync that is **not confirmed done** as of 2026-11-16: built but never seen in a run, built
but switched off, built differently from the plan, or not built. What is confirmed is in
[enemy-sync-how-it-works.md](enemy-sync-how-it-works.md). Section and research references are to
[enemy-sync-spec.md](enemy-sync-spec.md) and its design document.

Items are ordered by how much they show in normal play.

## 1. Needs a player's eyes (built, numbers look right, not seen)

| Item | State | What to check |
| --- | --- | --- |
| A player's blow sent whole to the owner (B.10) | Off unless `TOTK_COOP_EN_FWD=1`. The first version launched the struck enemy into the sky; the corrected one passed a two-game check (4 blows, no rise beyond a normal jump). | Play with the setting on: does a hit enemy still fly in the other game? Do knock-downs from the other player's heavy hits now happen? |
| Block and parry against a copy | Not built as its own piece. The game records a parry as a contact from the shield on the attacker, so with the setting above it may already reach the owner. | With the setting on, parry a copy's swing: is the enemy repelled in both games? |
| Ragdoll on a copy (B.7) | On. Bones agree to about 2 cm; never seen up close in a picture. An earlier version put the whole model in the wrong place. | Knock an enemy down in one game and watch it in the other. |
| A stuck, invisible Wizzrobe | Fixed by not handing over while it is unseen; not seen to hold in play. | Fight a Wizzrobe with two players for a few minutes. |
| Meteor damage to the non-targeted player | Same fix as the rod's flame; no meteor hit was ever logged on either Link. | Stand under the meteors as the non-targeted player. |
| On-screen notice for mismatched syncs | Made and logged, never seen on screen. | Start two games with different enemy sync settings. |

## 2. Built, never seen in a run

- **Blood moon (B.12).** The life number rises and old tombstones are dropped in every game. No blood moon came up.
  A tombstone the moon does not reset in the save (cave and sky enemies) is wrongly dropped too. A game is not made
  to start its own moon when told of another's.
- **Kill counters (B.9).** The copy's counters are suppressed so shared counters go up once; the value was never read
  in a run.
- **A hit in flight at a hand-over (B.8)** is passed on to the new owner. Needs a hit in transit at that moment.
- **Equipment: swap, fused attachment, drawn or sheathed (B.11).** The rules are in; none was seen firing correctly.
  A bow was dropped and re-given twice by an earlier form of the attachment rule, since narrowed.
- **Resting, offers, the "not in the middle of a reaction" rule, a loading owner, three or more players.** Code
  exists; no run states them.
- **The copy switch's back-up hooks** were never reached in a run (the main ones did the work).

## 3. Built differently from the plan (decide: update the plan or the code)

- **Hand-over by proximity.** The design forbade it; it was built and works. The plan text was not updated.
- **Hits as points.** The plan sends the whole hit entry and takes it out of the copy's list; what runs by default
  leaves the copy's damage step alone and sends lost life.
- **No event stream.** The plan's PLAY/EVENT messages, capture ring and play numbers do not exist; the sync is
  state-based. Eight declared message types have no sender or reader (BONES, EVENT, HITACK, OUTCOME, ORPHAN_ACK,
  PROJ_END, KILL, DEATH_SIGNAL).
- **Target and brain state** travel only at a change of owner, not as a continuing state.
- **Equipment** is state plus two events (drop, throw), not the plan's event set.
- **The bench of spec section 15 does not exist.** Runs used input scripts and `en_report.py`.
- **The older enemy sync is still live** for other kinds and for run-time enemies' creation; the plan deletes it and
  moves shared code out of `coop_world.c`. Neither is done.

## 4. Not built

**Affects ordinary fights**
- The owner's enemy reacting to a dodge; the flurry-rush slowdown (it is a whole-world time scale in the dodging
  player's game, and a dodge against a copy may not start a rush at all); the attack pause after a guard.
- A throw made by the copy itself, so a thrown weapon or rock can hurt the copy game's player. A picked-up weapon
  taken from the copy's ground (today the copy is given a new one and the ground one stays).
- Attack arming for the few kinds armed by an AI node rather than the animation.
- Knock-back, jump and fall as their own motion (they ride on the stated pose), and a copy standing on moving ground.
- A hit sent again when the owner changes mid-flight beyond the pass-on above; the plan's hit acknowledgement.
- The shield slot stays empty after the copy is given a shield (cause unknown, capped at two tries).
- Whether a copied projectile's plain impact damages objects in the copy's game (fire is handled, impact is not
  filtered for non-enemies).

**Awareness and feel**
- A continuing target rule ("one target, no flicker"); with two players at similar distance the game's own rule can
  swap the target back and forth.
- Sneakstrike and lock-on prompts on a copy, the "?" and "!" marks, look-at, battle music, hit-stop time rate.
- Effects and sounds an AI node starts, camera shake, rumble, boss gauge.
- A Wizzrobe's sky change (the magic weather request) outside the owner's game.
- The whole AI blackboard. Only a Wizzrobe's two flags travel, and one of them reads false where it is read.

**World and save**
- Save flags for a game with no actor at a dead enemy's placement; the plan's 165 shared enemy flags (only with
  `TOTK_COOP_SHARE_WORLD=1`).
- A camp spread across two owners; a camp's "cleared" record.
- The spawner rule (no local spawn while a copy of the same name stands near); a guard that a copy never announces a
  child (copies create nothing today).
- The orphaned death rule; an enemy that exists in one game only.

**Families**
- Yiga footsoldier: its blinks are followed poorly and its vanishing is untested. Octorok rocks and Lizalfos spit
  were never run. Hand-overs were checked on a Wizzrobe (and briefly a Keese) only.
- Everything outside the eleven families: Lynel, Hinox and Talus parts, Like Like, Gibdo, riders and mounts, dragons,
  bosses, swarms.

## 5. Known defects and loose ends

- One run in about thirty froze the co-op tick in both games just after they met. Not reproduced; a log line marks it.
- The 8 cm running bar of the plan is not met at the top of the measured range (5 to 12 cm).
- A weapon's reference in the held-things read still uses an id check that a Wizzrobe's rod does not pass.
- An attack restarted by a hand-over mid-swing was not looked at on screen.
- `IsShootMagicObj` reads false in the owner's blackboard while fireballs are shot: it is kept somewhere else.

## 6. Runs the plan still owes (spec section 17)

Done: 22. Partly done: 14, 16, 18, 19, 20, 21. Not done: 13 (declared not needed), 15 (slot verbs and the physics
flag word), 17 (the blackboard table on four families), 23 (a crate dropped on, and a fire lit under, a switched-off
enemy), 24 (a release during a knock-down). About 45 `[open]` and `[run]` marks remain in the spec; the audit lists
each with whether a later section answered it.

## 7. The documents themselves

The audit found 27 places where the spec contradicts itself, mostly plan text never updated after the as-built
sections were written. The ones that would mislead a reader:

- The state rate is given three different ways.
- Section A says the whole-hit path is "not built" while B.10 says built but off.
- Section A says the tombstone put-away is both "not yet seen" and seen under test settings.
- Section A lists four families in one place and eleven in another.
- `coop_enemy_pose.c`'s header says the actor's place is written from the pose packet; the code no longer does.
- The element sync's spec may still say a mirrored projectile gets the "does not interact" bit; it no longer does.
- Table sizes in the spec differ from the code.

## 8. Shipping

- Nothing on this branch is pushed and there is no pull request.
- The enemy sync (`TOTK_COOP_ENEMY=1`) and the element sync (`TOTK_COOP_ELEM=1`) are both off by default.
