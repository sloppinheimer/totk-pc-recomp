# Proxy puppet: status and plan (2026-11-05)

the tester decided on 2026-11-05 to replace the live puppet with a proxy: a display-only copy of the other player's Link,
as co-op games build them. This note is the handover. It says what is proven, what is not, and how to work.
`docs/coop/re-notes.md` has the full findings (sections from "Puppet spike" onward).

## Why
Puppet mode (`TOTK_COOP_PUPPET=1`, off by default) shows a real Player actor that gets no input. We pose it from the
network and pin it to its owner. Its own systems keep running and fight those overrides:
- physics (it falls, can't hold a climb);
- foot placement and IK;
- equipment state (it puts the sword away);
- its own animation layers.

Each fix overrode one system, then the next problem showed up. A day of this did not end with a puppet that looks
right in the tester's own two-pad session.

## Parity with TotK Online (2026-11-07)
the tester asked for its methods everywhere, peer to peer and with each player's own save. What it does is written up
outside the repo (the session notes, "totk-online-how-it-works"; read for behaviour only, the mod has no licence).
Its addresses are for 1.2.1; `tools/coop/xver_match.py` finds them in 1.4.3 (1.2.1's main from the v393216 update;
its addWeapon and isEquipmentInHand came out where we had found them ourselves).
- **Done: enemies run in every game** (coop_world.c, the native enemy model, on; `TOTK_COOP_ENEMY_MODEL=puppet` the
  old frozen copies). The copy's AI runs, it is eased to the owner's place (min(10 dt, 1)) and given its rotation and
  life, and its action requests (main+0x50ad74, 1.2.1 0xa29340) are replaced by the owner's last one. Pair test:
  game B's copy of a Bokoblin took player A's hits with its own reaction, died with its own effect and dropped its
  horn and weapon; Chuchus' copies played the owner's actions (id 105). Hit replays onto proxies are off with it.
- **Done: a sword hit counts in the owner's game only** (coop_world.c, contacts_filter; `TOTK_COOP_CONTACT_FILTER=0`
  off). TotK Online deletes the contacts with actors another game owns. Here the contacts waiting in a copy's hit
  update (main+0x300a30) from a Link's Weapon_ are taken out, so the copy is not hurt and sends no lost life; the
  player's co-op Link in the owner's game lands the hit there. Pair test (suite case `hits`): B's sword on its copy of
  A's blue Bokoblin was taken out, and the Bokoblin died in A's game with no hit sent from B. Arrows and bombs are not
  taken out: their damage still goes to the owner as lost life, as the co-op Link does not shoot in the owner's game.
- **Done, not seen end to end: enemies made at run time** (a night's Stalkoblins) are made in the other games: the
  create queues' getActor (main+0x50d364, 0x61a8b4, 0x6609b4, 0x89841c), an Enemy_ with no placement hash
  ([actor+0x370]+0x88, the same in 1.4.3), sent with its key; the others make one and give it that key. The test
  spawn claims its own actor and never passes the queues, so only the hand-over and the hash were checked.
- **Done: map actors keyed by their placement** (coop_world.c, actor_key; `TOTK_COOP_PLACE_KEY=0` the old way). An
  actor placed in the map is keyed by its placement hash with its name, as TotK Online's; one made at run time keeps
  the name and the place this game first saw it. Suite: every case pairs its enemies and objects as before (one run of
  `handover` had its copy 2.9 m off its owner's for 14 s; it passed when run again).
- **Tried, off: in hand by the game's calls** (setEquipmentInHand 0x4f5700 / removeEquipmentInHand 0x4f5194,
  `TOTK_COOP_GEAR_INHAND=1`). On our co-op Link, whose own update runs in bursts, the sword sat on its back in more
  frames than when made again. TotK Online's stand-in has no AI and replays its owner's animation clips; that is
  what makes its gear stay in hand.
- **Not done:**
  - the stand-in as TotK Online's: an actor of its own with Link's model and animation set and no AI, its owner's 7
    animation slots (clip, weight, frame, partial masks) replayed into its animation player (1.2.1: player
    [[[actor+0x228]+8]+0x28]+0x40, slots 0xb0 bytes, play by vtable +0x30); gear made once and drawn with the in-hand
    calls; arrows shot by the stand-in with its owner's camera (patched unproject);
  - Ultrahand glue made again in the other games, and breaks refused there; rotating ground's time copied;
  - the Gloom Hands (TotK Online leaves them out too).

## Still wrong in a play session (last build, 075576d)
- **Shake:** the puppet jumps 3-13 cm on some frames, even standing still. Its physics body now matches its owner
  exactly (position and rotation), so physics drift is not the cause. Almost every jump is in the same tick as a
  body teleport (ForceSetMatrix main+0x45ce70 from `snap()`), so suspect the teleport itself. Unproven.
- **Contorted body:** when the owner holds a weapon, the puppet leans back with its arm out. Never reproduced in a
  test.
- **Clothing after loading another save:** neither game logs its own gear again after a save load, so the other
  game keeps showing the old outfit. The fix in 075576d (`match_my_eq`) never ran. The new Link's gear is not built
  the way that fix assumed. No test script loads a second save yet.

## What is proven (usable as is)
- **Body pose:** main+0x283c750 (x0 the input pose, x1 the SkeletonObj) builds the body's local matrices.
  - Input pose: u16 bone count at +0xa; [pose] is the bones, 0x50 each: +0 scale, +0x10 translation, +0x20 and +0x30
    the first two rotation rows.
  - The pose is sent as POSE packets (type 8, 1948 bytes for 127 bones). Sent and applied poses are smooth
    (`TOTK_COOP_POSE_LOG`).
- **Skeleton buffers:** SkeletonObj +0x18 holds local records, 0x60 each (scale +0x10, rows +0x20/+0x30/+0x40,
  translation +0x50). +0x20 holds 0x40-per-bone matrices (translation in the 4th row).
- **Body skeleton path:** actor +0x48, +0x2a8, +0x20, +0x20, +0, +0xf8, +0x58, +0xb0 (127 bones). Head and hair are
  another skeleton, not copied yet.
- **Model:** [[[actor+0x228]+8]+0x28]. World matrix at +0x1f8 (3x4 rows), dirty bit +0x240 bit 0. main+0x24cc30,
  called in update phase main+0x245030, pushes it to the model's parts.
- **Physics to model:** main+0x257310 (x0 = actor) copies the physics body's transform into actor +0x2b4/+0x2c0 and
  the model. Skipping that call for a puppet breaks the skeleton (2.8 m jumps): it does more than the copy.
- **Physics body:** [[[actor+0x228]+0x50]+0x20]+0x150. Its mLastTransform is at +0x98 (3x4 rows).
- **Freeze:** `TOTK_COOP_TEST_FREEZE=<s>` stops a co-op Link's calc and phases. It stays drawn in its last pose, but
  ForceSetMatrix no longer moves it: the move is applied inside its update.
- **Items in hand:** EquipmentUser +0x4620 + slot (0 weapon, 3 bow, 4 shield), bit 1 = in hand (main+0x3e322c reads
  it). Setting it puts the item in the puppet's hand: checked with a faked sword. Item placement functions:
  main+0x25cf88 and main+0x269f70.
- **Dye:** the armor create's 4th argument (main+0xc48ac0 / main+0xc4961c). Synced and checked on screen.
- **Smoothing:** the puppet is shown 60 ms behind its owner, blended between reports (`TOTK_COOP_PUPPET_DELAY_MS`).

## Tools
- **Pair test:** `tools\coop\run_pair.ps1 -Seconds N -ScriptA ... -ScriptB ... [-ShotsA/-ShotsB w<lo>-<hi>:<step>
  -ShotsDir <abs>] [-FakeGearA name] [-FakeHandA bits] [-FakeDyeA ",,,,3,3"]`.
  - Set `TOTK_COOP_PUPPET=1` in the environment first.
  - Saves: 1st Rito tower (`idle.txt`, `tour.txt`, `show.txt`, `show_jump.txt`, `pose_walk.txt`), 3rd In-isa stairs
    (`idle_3rd.txt`).
  - Each game's world clock starts at its own load, so screenshot windows need slack.
  - Full-size screenshots slow the games and hid the shake: measure with logs instead.
- **Suite:** `tools\coop\run_suite.ps1 [-Only tour,fuse]`: the pair tests in one go (tour, fuse, equip, pickup), each
  passing on the log lines it expects, no FATAL, and the sync audit. Each case's logs stay in `build/suite/<case>/A` and
  `B`.
- **Sync audit:** each game writes one snapshot a second to `logs/audit.jsonl` (`TOTK_COOP_AUDIT=0`: none):
  - this Link, each co-op Link as shown, the objects sent and the copies shown, the held placed objects;
  - the enemies near a Link, the time of day and weather.

  `tools/coop/sync_audit.py <A> <B>` pairs the two games' snapshots by time and lists what does not match, as a Link
  (place, life, gear, hands, fuse, dyes), an object (missing, off, stale, two copies, a dead copy), an enemy (place,
  life), time or weather. A mismatch counts when it holds two snapshots in a row. `play_two.ps1` runs it when the
  session ends (`build/audit_report.txt`). First run (2026-11-06) found the proxy going to full life in each of its
  updates, an apple's copy rolling on past the owner's, and the guest's clock running ahead in the host's menus.
- **Frame tracer:** `TOTK_COOP_TRACE_AT=<s,...>`, `TOTK_COOP_TRACE_MS`, then `tools/coop/trace_report.py <csv>`. A
  window stalls both games, so don't judge positions across one.
- **Logs:**
  - `TOTK_COOP_TOP_LOG=<s>`: the highest body bone every frame, for this Link ("mine") and puppets.
  - Jitter watch: a log line for each jump over 3 cm, always on in puppet mode.
  - `TOTK_COOP_BONE_LOG`: replaced by the jitter watch, gone.
- **Build:** `.venv\Scripts\python tools\build_game.py --version v143` with `C:\msys64\ucrt64\bin` on PATH. Close
  totk.exe and the coop_relay python first. A cpu.h change rebuilds everything.
- **a play session:** `tools\coop\play_two.ps1` (two pads). Puppet mode needs `TOTK_COOP_PUPPET=1` in the
  environment of the process that starts it.

## How to work (agreed with the tester)
- Each problem first gets a test that fails in a measurable way, matching what the tester saw. Then fix it. Nothing is
  reported fixed until that same test passes, with numbers and a screenshot.
- If a stage fails its test twice, stop and write up the results for the tester. Don't keep trying.
- Button mirroring stays the default until stages 1 and 2 pass.

## Status (2026-11-05, overnight session)
- **Stage 1 passed** (`TOTK_COOP_PROXY=1`, commit b0a3969; numbers in re-notes "Proxy mode, stage 1"): on the In-isa
  stairs no standing frame steps more than 1 cm off its owner (largest 0.53 cm of 1569), position error mean 0.5 cm,
  body top mean 0.7 cm; on tour.txt position mean 0.9 cm, body top mean 1.1 cm; the faked sword is in hand. One gap:
  drawing far away is shown only to 24 m; the glide test could not frame the Link in either mode.
- What it took beyond the plan: the pose sent is made from the owner's final bone matrices (feet on stairs, hand on
  a wall), not the writer's input, and it is shown from the same delayed moment as the position.
- **Stage 2 passed**: held items and outfit / dye changes work with a thaw (the proxy's own update runs for 30 ticks
  after each change; a 1-3 cm twitch for about 6 frames). Loading a second save: the load re-dresses "the player's"
  EquipmentUser ([[[main+0x39601e8]]+0x480]), which the re-made co-op Link had taken over; this Link's is put back.
  load_3rd_up1 / up2: both Links in the new outfit and dye on both screens, proxy and default mode
  (re-notes "Second-save gear, traced").
- **Stage 3 needs nothing**: head and hair are bones of the 127-bone body, which the proxy already copies.
- **Stage 4 (research) found the call**: main+0x4dcc1c plays an AS by name in a slot on the Player's AS manager; its
  calls line up with every action of tour.txt (re-notes "Animation by name"). Replaying one on the co-op Link's
  manager is accepted (returns 1) but shows nothing on screen: two attempts, stopped.
- **Stage 5 (research) stopped**: a heap diff finds the words that start pointing at this Link when an enemy appears,
  but rewriting them changed nothing, and a spawned Bokoblin never made a real fight to test against (re-notes
  "Enemy targets, heap scan"). Needs a save next to a placed camp.
- **Proxy mode is the default** (2026-11-05, the tester's call after playing it): TOTK_COOP_PROXY=0 brings back button
  mirroring (TOTK_COOP_PUPPET=1 with it: the puppet).
- **Arrows** (protocol 8, the tester's list): a game reports the arrows its own Link shoots (NetState.objs, up to 8: id,
  name, position, rotation); the others show a frozen copy of each where the owner's is, nocked, in flight and stuck.
  An arrow is the shooter's when it is made within 3 m of its Link, nearer it than any co-op Link, and within 3 s of
  ZR (the first on the press, the next one on the bow 46 ticks after the release). bow_shoot_3rd.txt: A's four arrows
  show on B and nothing else is sent (before the ZR rule each game also sent two arrows made beside each Link at load).
  Arrow types go by name (any actor with "Arrow" in it). A fused material is an actor of its own (BombFruit for a
  bomb flower), sent as one more object when it appears within 1.5 m of an arrow while ZR is in use; it shows on the
  arrow on B. Not shown: what a fused arrow does on hit (the blast). Re-notes "Arrows".
- **World objects** (protocol 9; dropped items, Ultrahand, autobuild, Zonai devices from the tester's list; written for
  him to test, checked only by logs): what a Link makes (an actor first seen within 6 m of it) shows in the other
  games as a frozen copy; a placed object it moves goes to the same place in the other games (found in the pool
  of every actor's reference, at rest too; a copy only when a game has none). Objects go in packets of their own
  (protocol 10), sent when they change: up to 256 a player. Not done: what Zonai devices do (fans, flames), the bomb blast,
  picking up a copy in the other game (unknown whether it can be taken). Re-notes "World objects".
- **Riding** another player's contraption (protocol 12): this Link is carried by the object it stands on; the copy
  is moved by physics speed so it holds the Link up; the other games put the rider on their one of the object. Ride
  test at 5 m/s: on for 50 m on both screens. Not tested with a real glider flown in play, turning, or two riders.
- **Zonai devices** on the other screens: copies of devices run their own logic, and a player's attack makes their
  proxy swing in the other games, so a hit turns the copy on there too (a fan in the pair test). Effects are the
  copies' own (not yet looked at on screen). Not covered: devices turned on other ways (arrows, a steering stick,
  contact for rockets), the copy drawing the other player's battery, the swing hitting other things there.
- **Shield surf** (protocol 13): the surfing owner's shield is under its feet and out of hand; the frozen proxy kept
  it on its back. The owner sends where its shield is (in its own frame) while it is out of hand and below 0.5 m;
  the other games put the proxy's shield there after its model push, with the shield's own update and body held
  (shield_place). surf.txt pair test (TOTK_COOP_TEST_SURFLOG=1): the proxy's shield matches the owner's sample for
  sample through the surf (-0.15 0.15 0.00 at the feet) and is back on its back after.
- **Picking up another player's item**: an item's copy (Item_, Weapon_) runs its own update, so it can be picked
  up; when it goes away near this Link, the owner is told (an OBJS packet, header word 1) and deletes its own, and
  the copy is not made again. pickup_b_3rd.txt with TOTK_COOP_TEST_PICKUP=1: B picks up the copy of A's apple at
  the first A press, A deletes its apple. A weapon picked up with the hand empty is not deleted (the copy becomes
  the Link's own), so a copy now in this Link's gear counts too, and the owner deletes a dropped weapon it sends by
  key as well as one it made (a play session: the owner had kept its dropped sword). TOTK_COOP_TEST_PICKUP=sword:
  B's hand takes the copy of A's sword, A deletes its own. Not checked: the apple in B's pouch (no log of the pouch),
  a dropped weapon sent by key in a scripted test (dropping needs the menu), two players picking up one item.
- **Open (the tester's list, 2026-11-05):** the proxy shows no paraglider (Parasail is left out of world objects: a copy
  of it took the other game down with the Ultrahand crash, since fixed); the Ultrahand beam and Recall effects;
  a bomb arrow's blast on the other screen; hair and clothes still blown after a fan (re-notes); the autobuild
  green tint (re-notes); arrow copies cannot be picked up (frozen, their body kept away: a copy was taken from
  the bow).
- **The Master Sword's beam** (2026-11-06): the proxy makes none, so the owner's beam (PlayerBeam, a pooled actor,
  awake while it flies) is sent as a made object while it is within 6 m of its Link. Its copy is given the beam's
  scale (0.3, the owner's actor +0x2e4) and its flight (50 m/s along the way it first moved), and runs its own update
  (beam_copy). Checked by the tester: size, flight and spin look right. Open: no burst where it hits on the other
  screen. The copy finds no wall of its own (flown on, it went through), and what makes the owner's burst is not
  found (TOTK_COOP_TEST_BEAMLOG, beam_end_watch). Left there for now (the tester's call).
- **A bomb arrow's blast hurts the far Link** (open, 2026-11-06): P2 shoots bomb arrows at one spot on a wall far
  from P1; on P1's screen the copies go off at that spot (10-12 ticks after the owner's blast), yet P1 sometimes
  loses hearts, once until a fairy revived it with the same hearts as before, mostly on the second shot. Not the
  copies' place (logged at the hit), and not a shared life (the proxy's and P1's are kept apart). P1's life
  component ([[actor+0x228]+0x2f8]+0x1830, +8) did not go down when it was hurt, and its update (main+0x4400b0)
  took nothing off it: a Player's hearts are kept elsewhere. Next: find where, then log what hurts P1.
- **Open after the 2026-11-06 sessions** (the sync audit on a play sessions, kept in `build/session-*`):
  - Proxy hands: the proxy keeps its weapon and shield out after its owner put them away, for up to minutes
    (`link-hands`, in every session). The biggest one.
  - Dropped items rest at another place in each game, 1 to 12 m apart, for minutes (`object-place`); some copies'
    actors went away while still shown (`dead`).
  - Enemies: place and life match within 1 to 3 m, but each game runs its own AI, so what they do differs (the tester:
    "desync BADLY"). Done since (protocol 15, below): one game runs each enemy, the others pose it.
  - A Zonai device fused to a shield: the proxy's shield made with it shows it at full size; it is not sent for now.
    TOTK_COOP_TEST_FUSELOG logs the actor on each shield, its scale and place, to compare the owner's with the
    proxy's (not run yet).
  - Weather differs at times with the two Links together; the sword beam's burst; the proxy may no longer flinch when
    hit (its life update is skipped).
- **Enemies shared** (protocol 15, 2026-11-06; the tester's calls: "host decides all", and "whoever was there" first):
  - **Whose:** each enemy near more than one Link is run by one game, the one that has had it longest (its age since
    that game first saw it run; within 2 s, the lower player id). A player fighting alone keeps the fight when the
    others arrive. The others follow that game while it reports the enemy; when it stops (its player left the area),
    the next game runs its own from where it stands.
  - **Poses:** the owner sends an EPOSE packet per enemy each frame (the 12 nearest another Link, every third frame
    beyond 40 m): place, rotation and the body skeleton's local records (model +0x18 +0 +0x58 +0xb0; a Link's path
    from the actor finds none on an enemy). The others' one runs no calc and no update phase; in place of main+0x245030
    it gets the pose of 100 ms ago, the model push and its body put there (coop_world.c, enemy puppets).
  - **Deaths:** reported 5 s to all, and for the session to anyone who comes near where the enemy stood, so one
    killed while alone stays dead.
  - **Hits:** a following game's sword hit lands through its player's proxy swinging in the owner's game (one Master
    Sword swing killed the owner's Bokoblin, the copy died after it); its own swing does nothing to its frozen copy.
    The proxy's own swing takes the owner's attack button as of the pose shown (TOTK_COOP_PUPPET_DELAY_MS after it
    arrived; coop_players.c, y_delayed): taken as it arrived, that swing ran ahead of the blade seen and its game drew
    its sword trail apart from the blade, white arcs where no blade was (frames saved with TOTK_SHOTS_CROP). Synced,
    those arcs are gone and its hits still land (one kill in two runs either way). TOTK_COOP_PROXY_SWING_SYNC=0 takes
    the button as it arrives.
  - **Tested** (pair runs, TOTK_COOP_TEST_SPAWN with TOTK_COOP_TEST_SPAWN_AT so both games make one enemy at one
    place; tools/coop/enemy_compare.py on the audit): a Bokoblin chasing a walking Link for 83 m was 0.09 m off on
    average (0.61 m most) and its body's height within 0.1 m; the Chuchus near the save's start within 2 cm.
  - **Its hits on the others:** they land on the other players' proxies in the owner's game, which take none of it
    (and the frozen copy in their own game never attacks). The hit's record on the proxy (damage component +0x1a4,
    damage in quarter hearts at +0x1a8, the attacker's reference at +0x1c0) is sent to that player, whose life goes
    down by it when its game follows that enemy. The hit itself goes too: main+0x31dbc0 (x0 a damage component's
    receiver at +0x190, x1 the attack, x2 the hit, 0xe8 bytes: damage, kind, attacker handle, directions, flags) is
    the game's own delivery, found by a write watch on the Link's hit state (TOTK_COOP_TEST_HITWATCH). The owner's
    game keeps the one delivered to a proxy (the call with damage and a kind: the hit functions call it up to three
    times) and sends it with the record (protocol 17); the player's game delivers it to its own Link, with a blank
    attack that reads as not guarded and no attacker, from inside that Link's hit update (main+0x300a30, on its own
    thread). Its update then takes it as a hit of its own: pair test, two hits of 3, life 14 -> 11 -> 8 by the
    update itself, the Link knocked back down the stairs in that game's frames. What did not work: the record's words
    copied into the Link (not taken, as with a fan); flag +0xe0 bit 3 to skip the guard check (it is the guard's
    result: every hit blocked); the delivery from the co-op tick (it raced the update on another thread and broke an
    object at +0x1238: two crashes). The attacker handle (TOTK_COOP_LINKHIT_ATTACKER=1 names the frozen copy) is
    untried with the update-time delivery.
  - **A raised shield does not block them** (guard_3rd.txt in both games: four hits of 3 through the shield, life
    14 -> 2). A blocked hit reaches main+0x31dbc0 with damage 0 and nothing else of it different (TOTK_COOP_TEST_LINKHIT
    on a guarding Link's own hits, five of them, life kept), so the block is made where the hit is built, from the
    attack's contact with the shield. Not by: the guard check the delivery runs (main+0x1cd380c from main+0x1cceaac;
    with the Link's own attack object, dc+0x18d0, still never guarded); the Link's listener (vtable +0x48,
    main+0x1cca59c, the Player's damage filter: damage 3 -> 3 with the shield up, and no second armor cut without it);
    the proxy's own update with ZL let through in the owner's game (its hits still unguarded there). 
  - **Contact replay** (on by default; TOTK_COOP_LINKHIT_CONTACT=0 the hit replay only): the contact records waiting on the proxy's
    damage component ([dc+0x1910]+0x50, 0x1c0 bytes each) go to the player's game, which adds them to its own Link's
    list as that Link's hit update begins (main+0x300a30); its update builds the hit itself (damage, knock-back).
    The attacker handles name this game's frozen copy, the body is put at the same place from the Link's main body
    (+0xb558). TotK Online does it this way too (read for behaviour only). A hit on a proxy within 0.4 s of a contact
    sent to that player is the contact's; one without (a kind the contact list does not carry) still goes as the hit
    replay. Fight test: two hits, each taken once (14 -> 10 -> 6), no hit replay beside them.
  - **The block** (TOTK_COOP_CONTACT_GUARDBIT=1, off: see the end of this item): the Player's hit filter (main+0x319120 from
    main+0x1cca59c) cancels a hit when the equipment component's +0x4600 has bit 17, which the game sets when an
    attack meets the raised shield. With this Link's shield up (component +0x3b8 word +0x160 bit 28; action 0x41 at
    component +0x158 +0x84) and the attacker within 60 degrees of its facing, the bit is set: 26 hits and 3 hits, life
    kept at 14 (without it 14 -> 10 -> 6). Off by default: the hit is cancelled with no reaction (frames at a
    block show the Link standing as before). The shield-up test was ZL held alone at first; it is now ZL held, the
    sword drawn and the actor's +0x328 bit 1 (shield_raised: game B's Link with ZL and the sword on the back,
    drawn, and both): guarding, a hit blocked; ZL with the sword on the back, hits landed (14 -> 13 -> 10). Not checked either: the shield's wear, a perfect guard.
  - **The copy's awareness** (TOTK_COOP_ENEMY_AWARE, on): a frozen copy never noticed the follower's Link, so its game
    showed the Sneakstrike prompt beside an enemy fighting its player and its Link kept its sword sheathed and could
    not raise the shield. The owner sends six words of the enemy's component +0x158 with each pose (+0x24, +0x58,
    +0x60, +0x64, +0x70, +0x84: 0xc182 0x12 0x12 0 0 0x28 unaware, 0xc082 0xc 0xc 1 1 0x83 fighting) and the copy takes
    them. Put on as fixed values they took the prompt away. Synced, the copy follows its owner: the owner's Bokoblin
    stayed unaware (0xc182, 0x12) until 60 s while it wandered, then noticed (0xc082, 0xc), and the copy with it
    (+0x84 is its action, changing all the time, not awareness).
  - **Its weapons** (a play session: one lagged in the frozen enemy's hand and could be taken from it): the owner
    sends the Weapon_ actors within 2.5 m that are no Link's with each pose; the other games freeze theirs of that name
    nearest that place and put it there after the enemy's model push, its body 300 m under it. Pair test on the tester's
    save: a Dark Bokoblin's sword and shield and a Bokoblin's bow found and held. Not seen on screen; the camp stood
    still. A weapon lying within 2.5 m of a frozen enemy is held too and cannot be picked up while it is there.
    a play session (2026-11-07) still saw the weapons lag. Measured on a Bokoblin's sword (TOTK_COOP_TEST_WEAPLAG=1):
    in the owner's game its actor place is where its model is drawn (0.0 cm apart, the model moving up to 1.4 m a
    frame in a swing); in the follower's, the place put on the copy's model was still there a frame later (nothing
    else writes it) and moved as much a frame as the owner's. So the weapon follows the owner's weapon; what is left
    is where it is against the copy's hand: the pose is packed at the start of the enemy's calc (the jiggle, below),
    which may be a frame from where the weapon was put. Not checked on screen. The enemy's final matrices (skeleton
    +0x20) hold no world places in either layout for an enemy (the nearest "bone" 1.5 to 8 m from the hand), so the
    weapon is not yet put in the copy's hand bone's frame.
  - **Enemies fight only their own game's player.** A second Player actor (a co-op Link) is never noticed by an
    enemy: a Bokoblin spawned beside one stayed unaware (0xc182) with the game's own Link 44 m away, and so with an
    actor of our own beside it made from the Player's parameters under the category Enemy (TotK Online's
    PlayerPuppet is one such; read for behaviour only), or from a pigeon's with the Player's or a Sage's perception
    emitter and affiliation put on the co-op Link each tick (a Bokoblin targets players and Sages). So the enemy goes
    to the game of the player it should fight: **the hand-over** (TOTK_COOP_ENEMY_HANDOVER, on; coop_world.c,
    handover_to). The game running it gives it to another player's game when that player's Link is 5 m nearer to it
    than its own and within 30 m, at most once in 5 s; it stops running and reporting it, the other game finds its
    owner gone and runs its own one from where it stands (with the owner's awareness words already on it). If that
    game does not take it up within 3 s, the first takes it back and does not offer it again for 30 s. When two games
    claim one enemy (one let it go and takes it back, or both had it from the start), the one whose Link is 3 m nearer
    to it than every other's, within 30 m, keeps it, before the longest-held: in a play session a game whose Link was
    30 m off let its enemies go and took them back as they went in and out of the part of the world it updates, and
    the nearer player's game went each time from its own one, fighting, to the far game's, which had stood still (one
    stayed knocked down and could not be locked on to for a while). TotK Online's
    server gives every actor to the nearest player's game. Pair test (the same Bokoblin spawned in both games, A's
    Link 36 m from it, B's 15 m): A handed it to B, it hit B's Link (14 -> 11) and A's proxy of B lost the same.
    Without the hand-over B was not hit. An enemy fighting between two players may change games as they move
    (seen once: back to A at 12.5 m against 17.6 m, then to B again). A spawned enemy is in one game only (its key is
    its name and first place: the same in both only when spawned at one place, TOTK_COOP_TEST_SPAWN_AT).
  - **Enemy arrows** (an arrow made within 3 m of an enemy this game runs, 1 m from every Link, is sent as this
    game's, in the world): untested. Enemy_Bokoblin_Sniper_Junior spawned with no bow (it threw Rock_Weapon, which
    the prop sync shows) and Enemy_Assassin_Junior_Rider_Bow did nothing without its horse: a map's archer is needed.
  - **Arrows and bombs** from a following game: their hit's record on the frozen copy (its damage component, as on a
    proxy) goes to the owner as a hit, when this Link made it and it lacks the melee flag 0x100000 (a sword's has
    it; the proxy's swing lands those). Pair test: one arrow, 90, the owner's Bokoblin 25 -> 0, the copy died after.
  - **The jiggle** (a play session): the poses were read in the tick while the enemy's update ran on a worker
    thread. Now each is packed at the start of the enemy's own calc and stamped with the owner's clock. Per-bone jerk
    on a chasing Bokoblin's copy: 100 to 175 mm a frame before, 4 to 10 mm now (the enemy itself 1.4 to 3 mm).
    TOTK_COOP_EPOSE_TICKPACK=1 brings the old read back (control).
  - **Not done:** an enemy with no skeleton found (a bee swarm) is only put at the owner's place; the copy still
    jerks a few times more than the enemy; not seen on screen.
- Measuring tool for all of this: tools/coop/top_compare.py on TOTK_COOP_TOP_LOG from both games.

- **The Gloom Hands are not synced** (to do, the tester's call 2026-11-07: "we need to sync that"). MiasmaSwarm and its
  MiasmaTentacle hands are made in one game only, when its Link comes near (a spawner, not a map enemy), and their
  names are no Enemy_, so the enemy sync never sees them. They went as moved objects instead, and the other game's
  copy of a hand crashed it (access violation in main+0x4f2894, from main+0x2371630). Since 8cdaede the prop sync
  skips "Miasma" (coop_players.c, prop_skip): no crash, and the other player does not see them. To do: run them as
  enemies of the game that made them, and in the other game make the swarm (not single hands) and drive it as an
  enemy copy, with its hands placed from the owner's; then hits both ways, and the Phantom Ganon that follows.

## Plan
1. **Frozen proxy (make or break).** Passed (see Status). The co-op Link's own update is fully off in proxy mode. Each frame we drive:
   - its model transform, and the push to the model's parts;
   - its body pose (main+0x283c750 with the received pose);
   - its held items.

   Pass:
   - On `tour.txt` and standing on the In-isa stairs it matches its owner, with no frame jumps over 1 cm while
     standing (jitter watch, TOP_LOG against the owner).
   - It holds the sword in hand.
   - It still draws far away.

   If it can't draw or pose with the update off, record that and fall back: switch off only physics, foot
   placement and equipment logic, one at a time, each with the same tests.
2. **Gear on the proxy** (partly passed, see Status): outfit, dye and held items, including after a save load, with a test that loads a second
   save.
3. **Head and hair skeleton.** Not needed (see Status).
4. **Animation state instead of bones** (research): find how the Player plays animations by name, so a proxy can
   replay them and get effects and sounds for free.
5. **Enemies targeting the other player**: done by the hand-over (Status, "Enemies fight only their own game's player").
6. **The Gloom Hands in both games** (see "Still wrong").
