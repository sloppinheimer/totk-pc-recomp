# How the enemy sync works

This describes only what is built **and was measured in a two-game run** as of 2026-11-16. Everything that is built but
unconfirmed, switched off, or not built is in [enemy-sync-punch-list.md](enemy-sync-punch-list.md). The full reasoning,
addresses and run figures are in [enemy-sync-spec.md](enemy-sync-spec.md), sections A and B; the audit both documents
were made from compared the design and every spec section against the code and the commit log.

The sync is on with `TOTK_COOP_ENEMY=1`. It covers eleven families: Bokoblin, Moblin, Horriblin, Lizalfos, Chuchu,
Keese, Octorok, Wizzrobe, the two Zonai constructs and the Yiga footsoldier (swarms are left out). The older enemy
sync in `coop_world.c` still handles every other kind.

## The idea

Every enemy is **run by exactly one game**, its owner. There the game's own AI, physics and damage code run untouched.
In every other game the same actor exists as a **copy**: its AI is switched off and it is shown from what the owner
states about it. A copy is the game's own actor of that enemy, not a stand-in model, so it keeps its own animation
player, equipment, life component and hit sensors.

Two rules decide who is responsible for what:

- **The owner decides what the enemy does and what its attacks do to the world** (the ground, objects, other enemies).
- **Each game decides what an attack does to its own player**, from its own actor of the attacker. The owner's game
  only has a stand-in of the other player, and a stand-in takes no hit.

## Who owns an enemy

- A game claims an enemy it has an actor for; claims carry a pair of numbers that only ever rise, so every game ends
  up agreeing. Both games' tables were equal at the end of every measured run.
- Ownership moves in four cases: the owner offers the enemy to the game of a player who is clearly nearer; the owner
  offers it to the game of a player who has just struck it; the owner pauses in a menu (no AI has run for a second);
  the owner leaves.
- An enemy that nobody runs "rests" and is claimed again when a game runs it.
- An enemy that died stays dead for a game that arrives later (a tombstone per placement); that was seen under the
  test settings.
- Enemies made at run time (not placed on the map) are identified by the key the older spawn sync gives them.

## What a copy shows

The owner sends each enemy **whole, many times a second**, unreliably and latest-only: place, turn, velocity, life,
death state, ragdoll state, whether it is drawn, what each equipment slot holds, and for each animation slot the
command playing with its clock and rate.

- **Switching the AI off.** The copy's graph runners are turned away at entry and four values are written so the
  scheduler stays quiet. No runner got through in any run ("0 leaks").
- **Movement.** The copy's body is made static and its pose is written before its own update, between the owner's
  states. Measured: under 1 cm standing and 5 to 12 cm (95th percentile) while running; it held up on a test network
  with 120 to 180 ms delay and 8 % loss.
- **Animation.** When the owner's stated command differs from what the copy plays, the copy is asked to play it by
  the game's own play request, and its clock is bent toward the owner's. The same command was playing in 97 to 100 %
  of samples (Yiga footsoldier: 89 %).
- **Not drawn.** A Wizzrobe's unseen flight is carried as the model controller's request byte; the copy, its rod and
  the gem fused to the rod vanish and return with the owner's.
- **Equipment.** A copy lets go of what its owner's enemy no longer holds and is given what it lacks. A drop or a
  throw by the owner's enemy is told at once, so the copy lets go at once.
- **Ragdoll.** While the owner's enemy is a full ragdoll or getting up, the owner sends its bones and the copy writes
  them after its own model step. Three bones compared in both games agreed to about 2 cm (median).

## Hits and death

- **A player hitting a copy.** The copy takes the hit in its own game (so the blow shows at once); the life it lost is
  sent to the owner, which subtracts it and makes its enemy flinch. A kill by the non-owner's hits alone ended in both
  games.
- **Death.** The owner states the death with its causes; the copy is given the same death request and dies the game's
  own way.
- **One enemy hitting another** counts once: on a copy, hits from other enemies and their projectiles are dropped,
  because the struck enemy's owner counts the same hit itself.

## Attacks on players

- **Projectiles.** An enemy does not create a projectile when it shoots; its Shooter component prepares and shoots
  ones made in advance. The owner states each prepare and each shoot with the request it was made with, and the copy
  makes the same call on its own Shooter. This also works for a thing the enemy holds (a fire rod's flame is shot by
  the gem fused to the rod). Measured: a rod's flames within 3 cm of the owner's at 6 and 30 frames; arrows and a
  Wizzrobe's fireballs fly on copies.
- **Fire from a copy's projectile burns the local player.** The element sync keeps a copy's projectile from lighting
  the ground and objects (the owner states those) but lets it reach this game's own Link. Measured: 3 flame hits on
  the non-owner's Link where there had been none.
- **Melee.** A copy's swing is armed by the game's own animation events; one sword hit from a copied Bokoblin on the
  non-owner's Link was seen.

## Carrying on across a change of owner

A copy's AI never started, so without help a taken-over enemy would begin from "idle" and notice the player afresh.
The owner therefore also states what the enemy's brain is doing and which player it is fighting. The game that takes
it over fills the enemy's target with its own actor for that player (its Link or its stand-in), puts the brain back
into the stated state before the AI first runs, and turns away the AI's start-up reset once.

Measured on a Fire Wizzrobe, three hand-overs of three with both players staying: it stayed in battle with no
"notice" clip, kept its target, kept its "magic weather is on" memory, and the new owner went on dropping fireballs.
An enemy does fight a stand-in: the target slot's "time since seen" stays at zero.

## What a dying enemy leaves

A copy rolls and creates no drops. The owner's game notes each item its dying enemy's drop asks for and each weapon
an enemy lets go of, and hands them to the object sync as its own objects wherever they are. Seen: a Bokoblin's sword
and its horn made in the owner's game and copied in the other.

## Other things that hold

- Two games running different enemy syncs are told so in the log and by an on-screen notice (the notice was made and
  logged; it has not been seen on screen).
- The transport underneath recovers from lost messages (a fault that killed the stream under loss was found and
  fixed during this work).

## How it was measured

Two games on one PC through a local relay, driven by input scripts (`tools/coop/camp_*.txt`), with each game's log
compared by `tools/coop/en_report.py` (position error, animation agreement, table equality, leaks). Several findings
came from a tester's play sessions rather than scripts, among them the invisible flame, the missing fire damage and
an enemy flying into the sky.
