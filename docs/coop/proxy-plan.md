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
- Measuring tool for all of this: tools/coop/top_compare.py on TOTK_COOP_TOP_LOG from both games.

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
5. **Enemies targeting the other player** (separate research; see "Enemy targets: not found").
