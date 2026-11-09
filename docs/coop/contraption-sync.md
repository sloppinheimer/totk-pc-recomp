# Contraption sync: redesign plan (2026-11-08)

The aim, in the tester's words: perfect sync between the two games, everything behaving exactly as if you placed it
yourself; builds made and used together; touching a build reacts as it normally would; a build only takes the battery
of the person who activated it; every device effect visible on both screens.

This plan replaces the current contraption code (runtime/coop_bonds.c, the contraption parts of coop_players.c and
coop_glue.c). It rests on a static reading of the game's Zonai code (kept outside this repository) and on the runtime
notes in re-notes.md. Claims below marked *unverified* have not been seen running; Phase 0 tests them first.

## Why the current design cannot get there
Today each part is copied as a separate object, put in place by teleport every tick, and glued to its neighbours
afterwards by guesswork. The fixes on top (bond_pairs, mark_autobuilt timing, came_apart remakes, dev_follow) are each
answers to a symptom of that:
- **Missing pieces, misplaced glue, tilt:** the copy's parts are made one at a time and bonded from poses that were
  never exactly the owner's. The game itself never builds a contraption that way: it builds from relative frames and
  joins whole bodies at once.
- **Unstable when touched, riders left behind:** a teleported body has no velocity. The game carries the Link by the
  velocity of the body under it (CharacterUpdateAttachGround), and its solver treats a teleported body as a body that
  appeared somewhere, not as one that moved.
- **Fans fighting, flicker, toggles undone:** both games run the device logic on the same contraption and the sync
  tries to win against one of them.
- **Battery drained from both players:** both games run the drain.
- **Copies coming apart, remake loops:** the copy game runs the "autobuilt part broken off" rule on its own copies.

The common cause: two games each simulate the same contraption. The redesign has one game simulate it at any time,
and the other games show that simulation with real, moving bodies.

## The model

### 1. One authority per contraption
- A contraption (one ContraptionGroup on its owner's side) has exactly one **authority**: the player whose game
  simulates it (physics, devices, glue, the break rules). Every other game holds a **copy**.
- The authority starts as the player who built or glued it. It moves to another player when that player does
  something that needs the simulation in their hands (section 5).
- Each contraption carries a sequence number that rises at every change of authority, so late packets from an old
  authority are dropped.

### 2. A copy is built the way the game builds one
- The authority describes the contraption with the game's own format: the history serializer (main+0x1913694) writes
  any held group as a CmbAct record. That is parts with three body matrices relative to the root, bond records
  body-to-body, scale, attachments and on-state, 21 parts at most. That covers autobuilt and hand-glued contraptions
  alike. The packet also carries, per part:
  - the owner's object number (for claims and messages);
  - the green bit (SPR +0x150 bit 0);
  - for hand glue, the four look vectors (GlueBond.look, already sent today).
- The copy game creates all parts first (base matrix composed with each record's frame 0, as build_create_parts does).
  Once every part exists, it makes every bond in one pass with the bond maker (main+0x1916830, the same call the glue
  pull ends with), from the record's frames, not from where the parts happen to be. The group then forms in the next
  graph job, exactly as for a real build.
- Green is set only after the group exists (the lesson of 4895176).
- A change on the authority's side (a part glued on or taken off) sends a new record version. The copy adds or
  removes only the parts and bonds that differ. It does not rebuild.

### 3. Copies move by velocity, not teleport
- Every copy part is made keyframed (main+0xae4300, type 2) and given, each tick, the velocity that takes it to the
  authority's pose by the next tick:
  - linear `(target - pos) / dt`;
  - angular from the rotation difference;
  - SetLinearVelocity main+0x567750, SetAngularVelocity main+0x25a1f0.
  A teleport is kept only for a large error (more than 1 m, say), followed by the velocity in the same tick.
- Because the parts are rigid relative to each other, the wire carries the root's pose and velocity (about 30 bytes
  per tick). Each part's target is the root composed with its record frame, so every part sits exactly where the
  authority has it.
- Keyframed bodies with real velocity are what the game uses for moving platforms. The Link riding a copy is carried
  by AttachGround, and a Link pushing a copy meets an immovable moving object, not one that tilts under them.
  *Unverified that this holds the Link without slip; Phase 0.*
- The bonds stay on the copy. They do nothing physically between two keyframed bodies, but they keep the group, and
  with it the device group state, the green look and the glue look.

### 4. Devices: the authority decides, the copy shows
- **The authority's state is the truth:** the authority's group state (group +0x9e8) and each device's on mark are
  sent with the root pose.
- **The copy follows through the game's own path:** each tick it requests its group's state through the game's own
  request (the hit path's request value 1/2 into comp166 +0x20, main+0x1c08774). The device logic, power ratio,
  effects and sound then run on the copy as on a real one, so every effect shows. This replaces dev_follow's
  toggle bit.
- **Touching a copy's device:** a hit, or the stick's on message, is not applied locally. The copy game sees the
  toggle bit (+0x8d bit 3) set by its own hit code, clears it before the device update consumes it, and sends a
  toggle request to the authority, which sets the same bit on its own part. The result comes back with the state
  stream within one round trip.
- **No thrust from copies:** WindGenerator_ApplyImpulse (main+0x2487104) queries the wind box and pushes each body
  hit. On a copy fan, the hook skips the bodies of copy contraptions, which the authority moves, and keeps every
  other body: the local Link, loose objects, enemies. The wind then still pushes what it pushes in the authority's
  game, and the effect (driven by the power ratio, not by the push) still shows. Rockets and wheels need the same
  treatment once their calc functions are read (not done).

### 5. Who gets the authority
- **Taking authority:** a player takes it by grabbing any part with Ultrahand, taking the stick, or gluing something
  to the contraption. Their game asks the authority. The authority grants it at once unless its own player is
  grabbing or steering at that moment; the first request wins.
- **On a grant:** the new authority turns its copy parts dynamic (type 1), takes over the simulation from the last
  pose and velocity it received, and starts sending. The old authority turns its parts keyframed and starts
  following.
- **Riding and standing on it:** these do not move the authority. The rider's game moves its Link on a velocity-driven
  copy; the authority's game sees the rider as a stand-in.
- **Stand-ins must push:** in the authority's game, the other players' Links must push what they stand on or walk
  into. Today a stand-in is a static body moved by teleport, which neither pushes nor weighs. For "touch reacts as it
  normally would", the stand-in becomes a keyframed body with the remote Link's velocity. *Unverified how much a
  keyframed character body pushes a dynamic contraption; Phase 0.*

### 6. Battery: the activator pays
- **The activator:** each contraption records its **activator**, the player whose action last turned it on (a hit,
  the stick, a switch). The activator travels with the state.
- **Drain on the authority's game:** the drain is the game's own EnergyGaugeMgr consume (main+0x1fa4ca8), called
  from the on/off node chain main+0x9f9088 -> main+0x9fad68, about once per fan per tick in the runtime check.
  consumeRate (main+0x1fa4ed4) did not run in either fan run. On the authority's game:
  - **Activator is the local player:** the drain runs as normal.
  - **Activator is remote:** a hook on consume, set while the node of a part of that contraption is running, takes
    the amount and adds it to that activator's account instead of the local gauge.
- **Drain on copies:** the same hook drops the amount for nodes of copy parts. Copies never drain anyone.
- **Paying it:** the accounts are sent to each activator, who pays them through its own consume. Its gauge, HUD and
  the game's empty-gauge rules then all behave as when you run your own build.
- **Running empty:** when the activator's gauge empties, its game reports it, and the authority turns that
  contraption's devices off by the normal request path.
- **Battery part:** a contraption with a battery part (group +0x878 bit 28, set by the group refresh main+0xd19b90
  when a member is kind 0x17) does not touch any gauge. The battery's own life is simulated on the authority and
  shown on copies.

### 7. Coming apart
- **The authority runs the rule:** it runs the game's own "an autobuilt part broken off destroys the rest" rule. When
  its group loses parts (taken off, destroyed), the new record version tells the copies, and they remove the same
  parts. A part taken off by another player is taken off by that player's game after it gets the authority from the
  grab, so it always happens on one side.
- **Copies never destroy on their own:** a copy part the copy game destroys by itself is a divergence. It is logged
  and made again from the record, not followed. The came_apart counting and the nogreen table go.

## What goes
- **Placement and glue guesses:** place_copy for contraption parts, bond_pairs, and the mark-after-group timing
  tables in coop_bonds.c. The record replaces them.
- **Device fights:** dev_follow, the follow windows, and the "owner's stick beats a claimer's stale state" rule. The
  authority model replaces them.
- **Remake loops:** the came_apart remake and strike counting. Copies do not come apart by themselves.
- **Claims:** today's claims (claimed_group, held_root) are kept as the base of section 5's hand-over.

## Phases
Each phase ends with a pair test in tools/coop. A phase passes on full-size screenshots and log values, not on small
frames.

**Phase 0: settle what this plan assumes (test hooks only, no sync change).**
1. A spawned contraption made keyframed and driven by velocity along a path: the Link standing on it stays on it,
   with no slip, at walking speed and at fan speed.
2. The serializer called from the co-op tick on a contraption that is not held. It may need the ability's buffer;
   the history writer gives it ability+0x52e0.
3. Parts made from a record and bonded in one pass with the bond maker: one group, poses exact, with no player
   holding anything.
4. The ApplyImpulse hook skipping some bodies: the fan still shows its effect and still pushes the Link.
5. The consume hook with the calling part known (the node's actor, as devices notes 1.5): amounts move, the gauge
   follows.
6. A keyframed stand-in pushing a dynamic contraption in the authority's game.

Any item that fails changes the plan before Phase 1 starts.

**Phase 1:** authority, record, velocity-driven copies. A contraption built in A appears whole in B, glue in the
right places, green right, and moves with A's while A carries it with Ultrahand. B's Link rides it.

**Phase 2:** devices through the authority, and toggle requests from copies. Hitting a copy fan turns all fans on in
both games, with no flicker and no fight.

**Phase 3:** thrust muted on copies, and the battery charged to the activator. A Fanplane flown by A drains only A;
B hitting it on drains only B; B's view shows the wind and the fans running.

**Phase 4:** authority hand-over on grab, stick and glue. B grabs A's build and carries it, B steers A's Fanboat,
and B glues a part to A's build.

**Phase 5:** record versions, coming apart, divergence repair, joining late. Taking a part off in either game
disintegrates an autobuilt build in both. A player who joins sees existing contraptions.

**Phase 6:** stand-ins that push. B's Link standing on A's raft weighs it down and pushes it in A's game.

## Open points that can change the plan
- Who writes group +0x9e8, the group's device state, was not found. Phase 2 uses the per-device request, which the
  hit path proves works on a group (one fan's request turns all fans on).
- What applies the fan force on the authority: ApplyImpulse most likely queues a velocity change. Whether that
  change goes to the part hit or to the group's root is unverified; it matters only for the authority, which is the
  normal game.
- Per-frame code whose behaviour changes inside a function (the push, the on/off node) was outside what the runtime
  check could see. A branch profile of those functions would settle what else they do.
- Not read: rockets, wheels, hover stones, other device calcs, the shake detector, the destroy reader of SPR +0x20
  bit 57, and anything about a copy game.
- Not run: glue by hand, unglue, rockets, wheels, a battery running empty, and anything in game B.

## Phase 0 results (2026-11-08/09)

**1. A body driven by velocity carries the Link: yes.** TOTK_COOP_TEST_KFRIDE (coop_spawn.c kfride_test): a spawned
plank (AsbObj_WoodRectangle_A_LL_01) held 1.2 m above the flat ground of the fan tests, the Link put on it, then
moved 4 s along -x.
| Drive | Speed | The Link's level drift on the plank over the 4 s |
|---|---|---|
| linear velocity each tick (path speed + error / dt) | 3 m/s | 0.01-0.1 m |
| the same, after a keyframed request (main+0xae4300 type 2) | 3 m/s | 0.01 m, then 1 m sideways when the plank hit something at x 24 |
| the same, keyframed request | 8 m/s | 0.18 m (a lag at the start), 0.45 m after the stop |
| teleport each tick (COOP_FORCE_MATRIX), keyframed request | 3 m/s | not carried: left behind at once, 12 m by the end |
- The keyframed request did not visibly take: the teleported plank still sank under gravity. The motion read used
  in the log (body +0x68 bits 6/7) said "keyframed" for every mode, the untouched one too, so it is not the motion
  type. Open: how to make a copy keyframed, or whether a dynamic copy driven by velocity (which gravity and the Link
  push, and the next tick corrects) is good enough.

**2. The history writer on a contraption nobody holds: yes, once the actor access check lets it.**
TOTK_COOP_TEST_SERIAL (coop_autobuild.c serial_run), on the Fanplane 9 s after it was built.
- The writer (main+0x1913694) takes x0 = [[main+0x395f6f8]] (the table of contraption groups: +0x20 count, +0x28
  array, 0xa70 a group, the group's id at +0x20 is index | generation << 16), x1 = an ActorLink to any part, x2 an
  alignment matrix, x3 the record, x4 a matrix out.
- An ActorLink made by main+0x155ec needs +0x10 = -1 and byte +0x14 set first (it does nothing otherwise).
- The writer refuses a part whose SPR +0x28 is empty (main+0x1c03a60 then gives -1): the wing's read 0 at 37 s,
  9 s after the build, while the fans' and the group table already listed it.
- The writer's first step, the link lookup main+0x45efd4, is the job system's access check: while the actor manager's
  phase byte ([[main+0x395da68]]+0x424) is 2, the actors' parallel update, it resolves a link only to an actor the
  calling job may touch (the actor's per-phase slot at +0x30 empty, or a dependency of the job's own actor through
  main+0x281168 / 0x2808f0). Ultrahand's hold gives the Autobuild ability that dependency. The co-op tick and the
  Link's own update (coop_spawn.c calc_wrap) both only ever saw phase 2, and the lookup gave nothing.
- With the phase byte set to 4 for the one call (TOTK_COOP_TEST_SERIALPHASE=1, a test only): the writer returned 1
  with 4 parts and 3 bonds (each fan to the wing, body 0 to body 0); the parts' relative places in the record match
  their world places to the millimetre. The alignment matrix's layout is still to sort out (the record's y values
  came out shifted with the identity given).
- For the sync: the writer is usable, but it must run where the access check allows it (outside phase 2, or in a
  job that depends on the contraption), or the co-op code reads the same data itself (the group table, each part's
  body matrices, the bond edges).
- Follow-up (TOTK_COOP_TEST_KFRIDE, motion type asked every tick, as the stand-in's is): asked once, the type did not
  stay; asked every tick, a teleported plank held its height (keyframed). Keyframed and driven by velocity at 8 m/s,
  the plank stood still for 0.5 s and then caught up in jumps, the Link drifted 1.6 m and was thrown off at 43.7 s:
  asking for the type every tick seems to drop the velocity. Dynamic and driven by velocity (above) is the better
  drive for copies.

**3. Parts made at their places and bonded in one pass: one group at once, then torn apart.** TOTK_COOP_TEST_RECBUILD
(coop_autobuild.c recbuild_test): the Fanplane made again 6 m to the side from its parts' body matrices, each copy
held there until all four were claimed, every fan bonded to the wing in one pass (coop_bonds_bond, main+0x1c05a98).
- The tick after: one new group of 4, every copy within 0.01 m of the original's layout, at rest.
- Two ticks later, when the hold (teleport each tick) ended, the fans took 24-47 m/s and the group was gone within
  two ticks; the fans flew 50-100 m. Their device state stayed 3 (not on): not thrust. The graph job sets the bonded
  bodies to ignore each other (main+0xacfa10, from main+0x181e40), and the group did form, so a missing ignore is
  unlikely. Still to run: the same without any hold (TOTK_COOP_TEST_RECBUILD=40,0; the copies are made at the right
  place already).

**4. A fan's push skipped for chosen bodies: the skipping works; the effect on a moving build is not measured yet.**
TOTK_COOP_TEST_NOSELFPUSH (coop_autobuild.c coop_phase0_hook; hooks on main+0x2487104, 0x567750, 0x29ae114): from
39 s, inside the Fanplane's fans' push, the writes to its own parts' bodies were skipped, about 180 a second (3 fans,
60 a second); nothing else was in the wind (0 let through). On the In-isa stairs the Fanplane does not move with its
fans on even unhooked, so this run cannot show the build staying put; a run on flat ground (TOTK_COOP_TEST_ABPLACE)
loaded the wrong save (see below). Next: TOTK_COOP_TEST_FANSET=9's loose fan and its four BombFruit wind probes
(the hook now also skips the pushing fan's own body). Still to see: a build that moves held still, a loose object in
the wind still pushed, the fans' effect still drawn.

**5. Charges to the gauge kept and charged later: yes.** TOTK_COOP_TEST_DRAINMOVE=39,48 (hooks on the on/off node
main+0x9f9088 / 0x9fad68 and consume / consumeRate): with the fans on, the gauge (EnergyGaugeMgr +0x54) stayed at
48000.0 from 39 to 48 s while the kept charges added up, then dropped once the kept total was charged through
consume, and fell 312 a second as usual after. The kept total mixed consume and consumeRate amounts, so the single
charge (about 2450) is not the drain the 9 s would have been (about 2800): the real one must keep each call with its
own function and arguments and replay them.

**6. A stand-in pushing a contraption: not settled.** TOTK_COOP_TEST_STANDPUSH (coop_spawn.c standpush_test), game
B's Link walking into a plank game A put ahead of its stand-in, the stand-in static (0, the default), keyframed (2)
or dynamic (1) (TOTK_COOP_STANDIN_MOTION_TYPE): the plank, a flat floor piece on the stairs, was walked over and fell
or jumped rather than pushed. With type 1 it moved 1 m. Needs a box on flat ground.

**Runs paused (2026-11-09, from about 01:00):** every pair test since then loaded the first save instead of the 100%
save (the Link at -3595 318 -1804): the scripted title-menu presses miss while another game (ocarina.exe) runs on the
PC. The flat-ground run of item 4, the no-hold run of item 3 and the box run of item 6 are to be redone with it closed.

## Phase 0, second round (2026-11-09, after 01:30, with the other game closed)

**3. Made at their places and bonded: torn apart by their own collision, whatever the bond.**
- Without any hold (TOTK_COOP_TEST_RECBUILD=40,0): the same. The tick after the bonds the copies were already 0.2 to
  0.55 m off and one fan moved at 16 m/s: parts made touching or inside each other (as they sit on the original)
  push each other apart before any bond acts. The two ticks of one group, then the throw, follow from that.
- With the attachment frames given in each body's space (coop_bonds.c coop_bonds_bond_at,
  TOTK_COOP_TEST_RECBUILD_LOCAL=1): still thrown. main+0x1c05a98 does take the two frames in each body's space (it
  computes the pivot with main+0x299daec from them, as doScotch does), so coop_bonds_bond, which passes the world
  matrices, is wrong as well; the copies' "glue placed incorrectly" may be this. Not separately proven.
- Marked a hologram on creation (main+0x1c071f0) and solid after the bonds (main+0x1c07114): worse (15-75 m/s at the
  first tick): the preview mark does not take the collision away.
- Made 3 m apart, every pair asked to ignore each other (main+0x2ae6cf4) and then put together: none of the 6 asks
  answered 1, thrown again.
- **The reader of the Autobuild's bond queue is main+0x46fe60** (a write watch on a fan's SPR +0x2a0 across the
  confirm, TOTK_COOP_TEST_BONDQ: three writes by the confirm's append main+0x1c07618, three by main+0x46fe60). It runs
  in each part's own per-frame update and walks SPR +0x2a0: for each queued BondEdgeDesc it resolves the two bodies by
  their ids (+0x28, +0x2c; main+0x29a5a08), fills a bond description from the queued matrices (+0x30 frame A, +0x60
  frame B, +0x90 the pivot; main+0x8063ec, stiffness 100/10, rigid), makes the bond (main+0x1916830), sets the edge's
  +0xec / +0xef, wakes it (main+0xa159e4) and registers it with the part's component (main+0x1b8dba8).
- So the game's way to bond a made contraption is: queue BondEdgeDescs on part A's SPR (main+0x1c07604) and let each
  part's own update make them. The co-op copy should do the same (the descriptor as the confirm fills it,
  main+0x1d3ccf0: body ids from main+0xa2d39c, the frames from the record and the build's base matrix, the pivot from
  main+0x299daec). What is still missing is the confirm's other half: the parts are made as see-through holograms
  first (build_create_parts' create flags) and only become solid at the confirm, so they never push each other apart.

**2b. The writer from inside a part's update:** the link lookup that failed from the co-op tick and the Link's update
works there (a part's job may touch the part), but the writer still returned 0: it resolves every member, and the
other parts are not this job's to touch (likely; not traced).

**Who writes the group's device state:** main+0x32f610(group): over the members (+0x58 count, +0x60 links), each live
one's DeviceSwitch request folded into the best {value, priority} (main+0x1c4da28), then group +0x9e8 (state), +0x9ec
(priority), +0x994; called by the group refresh main+0xd19b90 and by the group message sender main+0x32eb90. So a
request on any member becomes the group's state: the plan's "copies take on/off through the request path" holds.

**Device tracing (devices2/ in the research folder):** the fan's push writes only its own part's body (not the hit
bodies; the physics query only measures how much of its own contraption blocks the air), so item 4's "skip the
contraption's bodies, let others through" is simply "skip the push on a copy"; loose things are blown by the wind
capsule actor. Rocket, wheel and sled likewise write their own bodies or joints; all four first test small SPR
predicates (main+0x1c02990, 0x1c02914, 0x1c09284) that may stop thrust on a copy in one place (not tried).
- Made as the Autobuild makes its holograms (coop_request_create_autobuilt with TOTK_COOP_TEST_ABARGS=1,
  TOTK_COOP_TEST_RECBUILD_AB=1) and made solid by the confirm's call after the bonds: thrown again (20-80 m/s at the
  first tick; two fans ended at one place). So what keeps an Autobuild's parts from pushing each other apart is not
  in how they are made but in the ability's build itself (their preview state while the ability holds them, and the
  confirm). Making copies part by part from the co-op code does not work; the way left is the game's own Autobuild
  run in the copy's game with the record put into its BuildObject (re-notes.md "Running the game's own Autobuild":
  the parts were made, the confirm refused, it needs the ability's own job). That is the next research step for
  copies; until then the plan's section 2 is not settled.

**Device tracing, lift (devices2/lift):** balloon, wing, hover stone and stabilizer write only their own body (the two
velocity setters and an impulse); none charges energy (the hover stone and stabilizer through the on/off node only)
or makes an actor. A copy skips those writes; since the co-op velocity drive uses the same setters, the skip needs a
flag set while the device's node runs (as the Phase 0 hooks do), not a filter on the setter alone.

**Device tracing, all families (research folder devices2/README.md):** every device that moves the contraption does it
through its own part's body (or joints), each in its own node, so "no thrust on copies" is one skip per node kind
(about ten nodes; maybe one SPR predicate for the thrust family). The steering stick pushes the whole contraption
itself (a force at a point on the stick's body every frame while ridden) and reads the rider's own controller: the
rider must be the authority, as section 5 has it. Emitters (beam, flame, frost, shock, light) have no force and are
best left live on copies, with hits counted only in the game that owns the victim. Cannon balls and time bombs are
made / set off by the authority only. Energy is charged only through the on/off node chain, as Phase 0 item 5 used.

## The game's own Autobuild for a copy (2026-11-09, early morning)
coop_autobuild.c rebuild_test / rebuild_go, game A only, the Fanplane kept from its build (TOTK_COOP_TEST_CREATEDUMP=1,
now the whole CmbAct, bonds included), then at 40 s the blueprint put back, the build's base matrix (BuildObject
+0x120, rows [R|t]) moved 6 m in z (TOTK_COOP_TEST_REBUILDBASE=6), the part pool refilled and the Autobuild's own part
maker main+0x1d3b470 called; at 42 s (TOTK_COOP_TEST_REBUILDGO=42), from this Link's update: each part's create
handle (pool entry +0x60) claimed and linked at the entry's +0x48, each entry's state +0x1c set to 3, +0x3c70
cleared, the blueprint put back, and the confirm main+0x1d3c0d4 called.
- The parts made by the Autobuild's own maker do not push each other apart (unlike every part-by-part copy above).
- The confirm returns 1 from the Link's update, but in phase 2 it reaches none of the parts (no mark, no bond
  queued). With the phase byte set to 4 for the call (TOTK_COOP_TEST_REBUILDGOPHASE=1, a test only) it queues each
  bond on its part's SPR (one on each fan, as the record has them); its own mark still did not reach the parts.
- With the confirm's mark given to each part after it (TOTK_COOP_TEST_REBUILDGOMARK=1, main+0x1c07114): the parts
  hold the original's layout to the millimetre for 10 s; the queued bonds are taken 0.8 s later (the queue reader
  main+0x46fe60), and at that moment the green bit goes back to 0; no group forms (SPR +0x28 stays 0), and the copy
  floats where it was made (1.4 m above where the original settled): most likely still in the build's held state.
- So most of the game's own path works from outside: the parts, the layout, the confirm, the queued bonds. Missing:
  what makes the parts live, grouped and green. That is the ability's own job (its state after the confirm, and the
  task handles the ready check reads), or a step after the queue reader that fails on these parts. Next: compare one
  part made this way with one made by a real Autobuild at the same moment after its confirm (SPR +0x20, +0x27c, the
  body's motion type and flags, the edge the queue reader made), and watch what clears the green bit.
- **A copy made the game's way: works (in game A, with one test-only trick).** What was missing, found by comparing the
  parts with a real build's one second after each confirm (TOTK_COOP_TEST_PARTCMP): with the confirm's full mark the
  rebuilt parts were already destroyed (state 11): green with no group is "a part broken off". With the mark minus the
  green bit (TOTK_COOP_TEST_REBUILDGOMARK=2: SPR +0x27c bit 0, +0x20 bit 28 on and bit 51 off, actor +0x5ac bit 2
  off; the green bit only once the part has a group) the parts lived, but loose: claimed in the confirm's own tick they
  entered the world a tick after it and their SPR came back cleared. Claimed first (TOTK_COOP_TEST_REBUILDGOCLAIM),
  they fell apart in the second before the confirm and the bonds kept that layout.
- Claimed 0.03 s before the confirm (TOTK_COOP_TEST_REBUILDGOCLAIM=41.97, TOTK_COOP_TEST_REBUILDGO=42): 0.2 s after
  the confirm the four parts are one group (4 members), green, every part at the record's place from the wing to
  0.01 m ((1.60, 1.15, 0.00) against (1.61, 1.14, 0.00)), alive (state 4), at rest by 45 s; their SPR words are the real
  build's (+0x20 0x80000000000202, +0x150 0x81). As the copy lands it rocks, and each part's distance from the wing
  stays the same (1.97 m for fan 1 throughout): one rigid unit.
- Still test-only or open: the confirm reaches the parts only with the actor phase byte set to 4 for the call
  (TOTK_COOP_TEST_REBUILDGOPHASE=1); a real version needs a call where the access check allows it (the ability's own
  job, or the parts' dependency on the calling job). The confirm charges the currency for each part (the copy's game
  must not pay: zero the cost records' +0x48 or skip main+0x209b754 for it). It uses the Link's own Autobuild
  BuildObject (ability +0x51f0), which clashes with that player building at the same moment. Not tried in game B.
- **Without any trick:** which hooked functions run outside the parallel actor update (TOTK_COOP_TEST_PHASES, the
  phase byte sampled at every co-op hook): GameDataMgr::calc (main+0xa72b00), getController, setEventBool and a few
  more run at phase 0. Called from GameDataMgr::calc (TOTK_COOP_TEST_REBUILDGOIN=gamedata, coop_spawn.c coop_hook),
  the confirm returns 1 at phase 0 with nothing forced and queues the bonds itself; the copy comes out one group of 4,
  green, at the record's places ((1.59, 1.16, 0.00) at 0.2 s), rigid as it lands. The history writer from there
  (TOTK_COOP_TEST_SERIAL with REBUILDGOIN) returns 1 with 4 parts and 3 bonds, nothing forced either.
- **No currency:** each part's cost record (main+0xae6e04 on [[[main+0x395f900]]+8], keyed by the entry's name; +0x48)
  set to 0 around the confirm and put back after (TOTK_COOP_TEST_REBUILDGOFREE=1: wing 3, fan 3 before): the same
  result. That the confirm then takes nothing is read from the code (main+0x209b754 is given the record's cost), not
  measured.
- Left: the copy uses the Link's own Autobuild BuildObject (a player building at that moment would clash), and all of
  this ran in game A, making a second copy of A's own build; game B, with a record received over the network, is next.

**In game B (2026-11-09 morning): another game's contraption made with this game's own Autobuild.**
- The Autobuild ability is caught in its update (main+0x9a2c00, hooked; x0 the ability, its build object at +0x51f0,
  the menu's at +0x51f8): in both games of a pair the ability and the build object have the same addresses
  (0x13628b0d0, 0x1ec715548), and game B's build object exists though B never built anything. Searching from the Link
  (TOTK_COOP_TEST_FINDAB) does not find it within two pointers; the Ultrahand part (vtable main+0x3695798, in five of
  the Link's components) is another object, its +0x51f0 empty.
- Game A wrote its blueprint (the whole CmbAct) and its build's base matrix to a file at 33 s (TOTK_COOP_TEST_ABSAVE;
  the file stands in for the network); game B read it at 36 s into its own build object (TOTK_COOP_TEST_ABLOAD:
  [o+0x3c50] pointed at the buffer o+0xc50 when empty, the base matrix into o+0x120) and made it with the same steps
  as in A (REBUILD=40, REBUILDBASE=6, REBUILDGOCLAIM=41.97, REBUILDGO=42 from GameDataMgr::calc, REBUILDGOMARK=2,
  REBUILDGOFREE=1): the confirm returned 1 at phase 0, 0.2 s later the four parts were one group, green, each fan 1.97 m
  from the wing as in A's, and the copy came to rest. The full-size frame of game B at 46 s shows it assembled and
  green beside the old-style copy of A's build.
- Next for the sync (Phase 1 of the plan): the record over the network instead of the file, made from the owner's
  live contraption by the history writer (from GameDataMgr::calc, so any contraption, hand-glued ones too, not only
  an Autobuild's schematic); the copy put at the owner's place and moved by velocity; the old part-by-part copy of
  contraption parts left out; the copy's parts mapped to the owner's object numbers; and the build object kept free
  for the player's own Autobuild (made between the player's builds, or a second build object).

## Phase 1, first step (2026-11-09): the record over the network, the copy made by the Autobuild
TOTK_COOP_CONTRAPTION=1 (coop_autobuild.c, "Contraption copies made by the game's own Autobuild"; off by default).
- The history writer's alignment is a 3x3 rotation (nine floats): given identity, each record frame is the part's body
  matrix with its translation less the contraption's centre, and the matrix out is [I | -centre]. So a game that puts
  [I | centre] in its build object's base matrix makes the parts where the owner's are.
- The owner, from GameDataMgr::calc every 2 s, records each of its contraptions (one member as the root), fills in the
  CmbAct header the writer leaves to its caller ("CmbAct", version 2), finds which of its objects each record part is
  (the member whose body is where the part's frame puts it), and sends record, centre and those numbers as 8 chunks of
  800 bytes ("CBBP" hit packets; coop_world.c reads hit packets into a 968-byte buffer, a bigger packet is dropped).
- The other game puts the chunks together; once a shape (part names and bonds) is whole it is made with its own build
  object: the record in, base [I | centre], the part pool refilled, the part maker, the parts put in the world when all
  are made, the confirm the next frame with no cost, the mark without the green, the green once grouped. Its old
  copies of those parts are deleted and not made again (coop_players.c spawn_slot), and the parts it makes are not its
  own: not sent as objects (prop_fn) nor recorded back (without that, game B sent the copy back and game A made a copy
  of it).
- Pair test (autobuild_noy_3rd.txt in A, idle B, both TOTK_COOP_CONTRAPTION=1): B received the 8 chunks, deleted its
  four old copies, made the Fanplane at A's place (wing at 40.13 1512.6 1505.3, A's centre 39.98 1513.05 1505.29), the
  confirm returned 1, and for 3 s all four parts were alive, in one group, at rest. The full-size frame of game B at
  46 s shows one green Fanplane, assembled, where A's is. A normal start without the setting is unchanged.
- Next: the copy following the owner's moves (velocity), its devices' state, the copy's parts known by the owner's
  numbers (claims, messages, hits), a shape that changes (a part glued on or taken off), and the player's own
  Autobuild while a copy is made.

## Phase 1, second step: the copy follows its owner (2026-11-09)
- The owner sends the body matrix of its contraption's record part 0 (found when the record is made) about 20 times a
  second ("CBMV" hit packets, with the owner's world time). The other game drives its copy's part 0 by velocity each
  tick (coop_autobuild.c bp_follow): the owner's speed from its last two poses, plus the gap to its last pose carried
  forward (at most 0.3 s) closed at 6 per second; and an angular velocity from the turn left to the owner's
  orientation, closed at 6 per second. The bonds carry the other parts.
- Pair test (TOTK_COOP_TEST_ABPUSH=44,0,3,0: A's Fanplane given 3 m/s up for 3 s, then left to fall): at rest the
  copy's root was within 0.01 m of A's; rising at 3 m/s, within 0.03 to 0.09 m (one sample 0.46 m); when it fell and
  tumbled, 0.17 to 0.56 m. The full-size frames of both games at the landing show the two Fanplanes tilted the same way.
- Next: the copy's devices (on/off through the request path, no thrust, no battery), the owner's numbers on the copy's
  parts, a shape that changes, and the player's own Autobuild while a copy is made.

## Phase 1, third step: the copy's devices (2026-11-09)
- The pose packets carry which record parts have a switch and which are on. The copy's group is put on or off as the
  owner's by one toggle request on its first switched part (DeviceSwitch +0x8d bit 3; main+0x32f610 takes a member's
  request to the whole group), asked again only after 1.5 s.
- A copy's fan pushes nothing (WindGenerator_ApplyImpulse returns at once for a part made from a record: its push only
  ever moves its own part, which the owner's game moves), and a copy's on/off node charges no gauge (consume and
  consumeRate dropped while it runs), so only the owner's Link pays.
- Pair test (TOTK_COOP_TEST_ABON=42: A's Fanplane's devices toggled by the request a hit makes): A's three fans came on
  at 42.5 s; B's copy, off, was toggled once and its three fans read on (0111, as A's), and stayed so; B's log shows the
  copy's charges not made. A's second toggle at 52 s did not turn A's fans off (one request on one part does not turn
  a group off; the owner side, not the copy).
- Not done: other thrust devices (rocket, wheel, sled, balloon, wing, hover stone, stabilizer: research devices2/),
  a toggle asked on a copy (a hit on the copy's fan in game B) taken to the owner, and the devices' state in a record
  made while they are on.

## Phase 1, fourth step: the copy's parts by the owner's numbers (2026-11-09)
- A part made from a record answers to its owner's object number both ways: coop_obj_ident gives the owner and number
  from the record's part list, coop_obj_find gives the made part, and coop_is_copy counts it as a copy (coop_players.c,
  coop_autobuild.c coop_contraption_ident / coop_contraption_find). Claims, glue and hit messages that name the
  owner's object reach it.
- coop_bonds.c leaves alone a group whose parts are made here from a record (they are bonded, switched and followed by
  the record path); without that, its older copy sync would bond and switch the same parts.
- Pair test: each of the four made parts said "player 1's object <n>" for its own number, each number found its part,
  each counted as a copy; following and the device toggle unchanged.

## Phase 1, fifth step: toggles both ways, every device's push, changes of shape (2026-11-09)
- A toggle asked on a copy (the DeviceSwitch update main+0x4a3500 hooked: a copy part's +0x8d bit 3 set by this
  game's hit path, not by the follow) is cleared and sent to the owner ("CBTG"); the owner toggles its own contraption.
  Toggling follows the switch update's rule: on, one member's request; off, the request on every member that is on
  (one request did not turn the owner's group off). The copy's follow uses the same rule and marks its own requests.
- On a copy, every device node that pushes its own part (rocket, sled, balloon, wing, hover stone, stabilizer, spring,
  stake, homing cart, steering stick: research devices2/) has its body writes skipped (the two velocity setters and
  the three impulse / force calls), and the wheel's and the cart's motor nodes are skipped whole. Only the fan has run
  in a test so far.
- A new shape for a made copy (a part glued on or taken off there) deletes the copy and makes the new one; a copy whose
  owner sends no pose for 3 s (its contraption gone there; the owner sends poses only while the root lives) is deleted.
  A copy is not started while this game's player has an Autobuild design selected (ability +0x51e8).
- Found and fixed: deleting the old copies (and later the made parts) was told to the other game as "destroyed here"
  (coop_players.c, the keyed world objects watch): the owner then destroyed its own parts by those keys, its
  contraption fell apart, and the copies with it. Copies, made parts and every actor this code deletes
  (coop_deleted_here) are now kept out of that notice.
- Pair tests: (1) a toggle asked on B's copy at 46 s went to A, A's fans came on, the copy followed (0111); a second at
  54 s turned A's fans off and the copy followed (0000). (2) A pulled a fan off its Fanplane (autobuild_part_3rd.txt):
  A's contraption became a new 3-part shape, B made it (3 of 3 in one group) and deleted the old copy when its poses
  stopped. The first copy in that run lost its group 0.5 s after it was made and was destroyed: A's Link is put by a
  fan at 36 s by the test (TOTK_COOP_TEST_ABHIT), and its stand-in in B appeared inside the copy as it was being made
  (likely; open). No "destroyed here" notice any more.

## Phase 1, sixth step: naming the record's parts, splits (2026-11-09)
- The first copy that fell apart in the part-off test was not the test's teleport (the copy broke at 30 s, the teleport
  came at 36 s). The owner had sent the wing with no object number: its body's origin sat more than 5 cm from the
  actor frame the record holds, so the other game kept its old loose copy of the wing, and the copy was made around
  it. Each record part is now the group member of the part's name nearest to its frame (within 1 m, each member
  once), and a record with a part that matches nothing is not sent (tried again 2 s later).
- When a part comes off, the owner's contraption gets a new key. A copy of the owner's that shares objects with a new
  record is deleted before the new one is made; before, the two were made inside each other and the new one fell
  apart. A record whose copy was deleted no longer claims its objects, so the part that came off gets its loose copy.
- Pair tests: the toggle test as before (on at 46 s, off at 54 s, the copy follows). Part-off: 4 of 4 made in one
  group; the fan pulled off; the old copy deleted, the 3-part copy made 3 of 3 in one group, its root 0.00 m from the
  owner's; the loose fan copied. Normal start, no settings: both games load, 59 fps, no contraption lines.

## Phase 4, first step: the authority moves on grab (2026-11-09)
- Each contraption has one game that runs it (its authority, at first its owner) and a change count. A held part's
  SPR +0x20 bit 16 is set while Ultrahand holds it (research ultrahand/), and the co-op Links have no abilities, so a
  held part is always held by the game's own player. The game whose player holds a part of a contraption it does not
  run asks for it ("CBAU", every 0.3 s while held). The game that runs it hands it over at once unless its own player
  holds a part, and tells every game (a grant with the count one higher; older counts are dropped).
- The game that runs it sends the root pose ("CBMV" bit 0); every other game drives its root toward it by velocity.
  That now includes the owner (coop_autobuild.c drive_root, shared with the copies). The owner keeps sending its
  devices' on and off (bit 1): toggles still go to the owner, and the battery stays with the player who turned them
  on. The pushes of devices are skipped on every part another game runs (contraption_driven), the copy's and the
  owner's alike. The owner takes the authority back if the game running it sends no pose for 3 s.
- Not yet: the authority on the steering stick or glue, a part taken off by the game that runs a contraption it does
  not own (the owner's record still decides the shape), and the owner taking it back by grabbing it (run: see below).
- Pair test (autobuild_noy_3rd.txt in A, autobuild_carry_b_3rd.txt in B, B with TOTK_COOP_TEST_ABHIT=46,
  TOTK_COOP_TEST_ABHIT_B=46, TOTK_COOP_TEST_ABHIT_DIST=2.2): B grabbed its copy of A's Fanplane, A handed it over
  (change 1). B lifted it 3 m and carried it; A's followed within 0.03 to 0.12 m, and 0.55 m at worst while falling
  after B let go. At rest, A's was 0.02 m from B's, all four parts in one group.
- Found on the way: with the test's Link put 1 m from a fan, its stand-in in the other game appeared inside the
  contraption and shoved it 9 m off its place (the stand-in is a static body moved by teleport; section 5's
  "stand-ins must push" item). TOTK_COOP_TEST_ABHIT_DIST puts the Link further away.
- The owner takes it back by grabbing it (pair test: autobuild_grabback_3rd.txt in A with TOTK_COOP_TEST_ABHIT=64,
  TOTK_COOP_TEST_ABHIT_DIST=2.2 and TOTK_COOP_TEST_ABHIT_FANX=1, autobuild_carry_b_3rd.txt in B as above plus
  TOTK_COOP_TEST_ABHIT_FANX=1). B carried it (change 1) and let go; A grabbed it, asked, and B handed it back (change
  2). A lifted it 3 m and dropped it, and B's copy followed within 0.02 to 0.12 m, at rest 0.05 m. FANX puts each Link
  by the fan furthest along +x: put by the first fan found, which differs between runs, B's Ultrahand missed in one run.
- Seen once, not again: after B's carry, A's contraption stopped 1.0 m from B's pose at rest, while A's own Link had
  just been put next to it by the test (likely blocked by that Link; open).
