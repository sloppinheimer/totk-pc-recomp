# Contraption sync: what the game does

What reading the game code established about contraptions, devices and their lifetime (TotK 1.4.3, addresses are
main+offset). These sections were moved as they were from [contraption-sync-v3.md](contraption-sync-v3.md) on 2026-11-12
and keep the section numbers of that file, because code comments and the other sections cite them. They describe
the game; where a section also says what our code does with a fact (the P6 and P7 designs at the end of 41, for
one), that part describes the v3 code and is replaced where [contraption-sync-v4.md](contraption-sync-v4.md) says so.
R is the research folder outside this repository. Facts read between sections 41 and 49 are in v3 sections 44.1,
45.1, 47.4 and 48.6.

## 32. What the structure reading established (read 2026-11-09, R/sync-v3/m2/p4/, six folders, all read in full)
Static reading only; nothing here has been run. Each line names the folder that holds the code and addresses.

**Bonds (bond-make)**
- A bond is fully described by the relative pose of its two bodies: `rel = inverse(poseA) * poseB`, with which body
  slot of each part. Passing both bodies' world matrices holds them where they stand; nothing is pulled.
- The game makes a bond two ways, both ending in the same maker: at once (main+0x1c05a98), or queued on the first
  part and made by that part's own update in a frame where its SPR +0x27c bit 0 is set (what an Autobuild confirm
  does). The queue needs no build object. Setting the bit by itself runs the reader once and does not make the part green.
- The queued way also asks an active device on (request {1, priority 5}) and raises a structure flag on the group.
- A body torn down once carries a mark (body +0x68 bit 24) that nothing clears; a bond on such a body is dropped silently.
- An older note ("the maker takes frames in each body's own space") is wrong; the matrices are poses in one common space.

**Groups (graph-groups)**
- No group survives a change. Adding or removing one bond dissolves the connected sets at both ends and rebuilds them,
  and every member gets a new group record: SPR +0x28 and the group id change at every rebuild even when parts and
  bonds are the same. Matching a build by its group can never hold; 27.3 R5 stands.
- The group pointer of a part left alone becomes 0, written last in the same physics step.
- The new members collide with each other from the bond maker's return until the end of the graph job that takes the
  bond: the push-apart seen when bonding touching parts.
- A record's part order: sorted by name; equal names keep the group's member order, which is the order of first
  appearance in the set and changes when a bond is added. So "uid = record index" (the code today) is not stable.
- A hook on the record bind (main+0xc16b94) sees every regrouping once, with the member list filled.
- The history record: at most 21 parts and 21 bonds; bonds past 21 are dropped silently; an end whose part is not in
  the record leaves its index byte unwritten.

**Breaking (bond-break)**
- The edge between two named bodies is found by listing one body's edges and comparing ends; exact code exists.
  Breaking it "as the game does" posts a message carrying the edge pointer to the owner of end A.
- One shake of a hand-glued part breaks every glue bond on that part and no other. One shake of an Autobuilt part with
  both Autobuilt and plain neighbours breaks only its bonds to the plain ones.
- Six families of code remove edges; only three pass the one function a hook could watch. A torn-down controller takes
  all its edges with it and passes none.
- The strain rule (the stick's loss, 28) is read in full: tolerance 0.3 m scaled by the parts' bond strength.

**Death and delete (death-delete)**
- For a Zonai part every death kind but five ends in a plain delete inside the request call. A death breaks no bond and
  leaves no group by itself; only the delete's teardown does, in a later physics flush.
- The kind asked is rewritten before it is stored (kinematic collision becomes life depleted). A replay must use the
  stored kind, and a delete made at once leaves the death word untouched.
- Delete reason 0 is "delete this actor"; reason 3, which the code passes today, is "far delete", the unload reason.
- The delete request reads the actor's lock object before anything else, and that object is dropped once the actor
  passes state 7. This is the read that crashed player 2's game (31, note 7).
- No single read is proven for "this actor's bodies have left the world"; body +0x70 == 0, taken through a pointer
  known to be valid, is the best supported.

**The part maker (part-maker)**
- The game's confirm already has a branch for "an existing actor stands in for this record part" (pool entry state 2)
  and bonds it to newly made parts from its real body. It accepts a hand-filled entry. A record still needs at least
  one bond, so a single part cannot go through it.
- The confirm sends the set-off message to an existing device it takes in, breaks that actor's glue edges first, and
  does not make it green.
- The "preview" marks are never put on the parts the co-op recipe makes, and none of them was shown to stop parts
  colliding while they are assembled. What does is not established.
- The player's character controller pushes what it touches through a query filter that never runs the pair test, so
  the stand-in veto does not reach a controller's pushes. (A stand-in has no controller; the local player has.)

**Identity (placement-id)**
- Every actor has a placement record; a map-placed actor carries a 64-bit hash copied from the map's own data, and an
  actor made at run time carries all ones. The game keys saved flags by that hash, so it is the same in two games on
  the same map data. "Has a hash" is the test for "came from the map".
- Nothing tells "made by a player" from "spawned by the game at run time"; that has to come from the mod's own record.
- NPC and enemy vehicles are not made by the bond maker; how their parts are joined is not read.

### 32.1 What this changes in the design
- 27.2's "uid = record index" goes. A part's uid is given once by the runner and travels with the version (6.1).
- Structure changes can be sent as what they are (a bond: two uids, two slots, a relative pose; a break: two uids, two
  slots; a death: a uid and the stored kind) and made in place with the read functions, instead of delete-and-remake.
  Remake stays as the fallback.
- A delete the mod makes passes reason 0 and is made only on an actor whose state is 2 to 6 and whose lock object is
  still there.
- A followed build whose parts are gone is not repaired: parts that vanish in a follower are an unload until the
  runner says otherwise (10.3).

### 32.2 Still not read after this pass (each blocks the piece named)
- In which frame the graph takes a new bond after the maker returns (the scheduler's order): bounds how long new
  neighbours collide. Blocks nothing; the made bond is correct either way.
- What keeps parts from colliding while the game assembles them: blocks a clean in-place add of a touching part.
- The shake detector and the bit that arms a part's bonds for breaking (SPR +0x20 bit 55's meaning).
- What holds the delete lock of reason 0 for a part.
- How NPC and enemy vehicles are joined.

### 32.3 Where the work stood when this was written (2026-11-09, late)
- Committed and run: the rebuilt core coop_ctn.c (27, 28), device on/off by the set messages sent together at phase 0
  (28), the stand-in veto over every body (29.2, 29.3).
- Read, not yet written: everything in 32. Play-test notes 1 to 7 in 31, of which 7 (a crash in the delete request)
  and 6a (a build taken from under its driver on a false touch) come first.
- Being read: the device half, eleven Sonnet readers writing into R/sync-v3/m2/p5/ (engine-access, rocket-timebomb,
  spring-stake, lift-devices, wheels, stick-head, emitters, hits-status, spawns, energy, setoff-not-taking). Their
  folders are to be read in full, README first, before anything of P5 is written.
- Then: implement milestone 2 whole (30), list it in 29.2 as implemented and not tested, and ask the tester for the run.
- Rules in force: 29 (no guessing, no run to find out, read first, test only when all is implemented and with a yes).

## 34. What the device reading established (read 2026-11-09, R/sync-v3/m2/p5/, eleven folders)
Static reading only; nothing here has been run. Each block names the folder that holds the code and addresses.

**The steering stick and "held" (stick-head) - notes 6a, 6b**
- A stick's rider is the occupant of the one seat of its Ridable component; the seat is filled only by the rider's own
  ride start. Nothing fills the seat of a fresh copy. The mod's stick_rider reads the right fields but does not test
  that the occupant's actor is in state 2 to 6.
- The mirror on the rider: the rider's controller component ([actor+0x228]+0x410) holds a handle to the ridden actor at
  +0x20 (slot +0x28, id +0x30, valid +0x38 == 1). The controller's state word 3 means steering.
- "Held" (SPR +0x20 bit 16) is set only by main+0x1c0b83c, from three message paths of a Player's Ultrahand part; the
  holder is stored as a handle at [SPR+0x30]+0x10, so a hold by this game's Link can be told from any other. The
  confirm sets no hold. What made one frame of "held" on a fresh copy is one of three candidates, not decided by
  reading; reading the holder handle and requiring it to be this game's Link closes all three.
- Why the car never switched off (6a): the "everything off" is raised only by the stick's own node in the game where
  the seat was occupied, once a frame for 2.0 s after the rider leaves (event 0x12 on the group, group +0x880). The
  game that ran the build had no rider on its stick, so nothing raised it. A game that runs a build whose rider is in
  another game has to raise event 0x12 itself (group +0x880 |= 1 << 0x12) when the rider leaves, or the runner must be
  the game of the rider.
- While a rider steers, the stick raises event 0x11 each frame and the group sends ON to the switchable devices.
- The stick's tilt: floats in the stick's blackboard, ControlStick_StickX/Y (0x6f1afb02, 0xde18d120) and
  ControlStick_ScaledStickX/Y (0x4b809ce9, 0x9479c43d); the stick's animation reads the Scaled pair, the rider's pose
  node reads StickX/Y of the stick it rides. Writing them on a follower each frame shows the tilt; whether the
  follower's own graph writes 0 over them each frame, and in which order, is not read.
- Four group bools named: ControlStick_ForceAllPartsOn 0xe7ee94d2, ForceAllPartsOff 0xd169fcb2,
  SpecialParts_IsLogicOrderOff 0x0f548ac3, SpecialParts_IsChangeableOnOffNow 0x8d0ccacd.
- Only the spring and the time bomb take separate on/off messages (IsSeparateOnOffMessage); the stick, cart, pot,
  mirror and stake are not switchable by a hit.
- The construct head has no brain in its graph: it turns toward a perception target by posing a constraint, applies no
  force and spawns nothing.

**Set-off not taking (setoff-not-taking) - note 3**
- A set-off that reaches the node cannot fail inside it: the off trigger is tested before every on trigger. A fan that
  still reads on was either not reached, or switched on again, or the bit the mod reads is not the latch.
- The mod's "on" read ([comp 11]+0x348 bit 0x2000) is not the latch, and its writer was not found. The latch is the
  node's +0x9a bit 0; the game itself reads the blackboard bool SpecialParts_IsOn (hash 0xbcd1015f), written on every
  accepted change. The mod must read that bool.
- What switches a just-switched-off fan back on in a two-fan build: the group's mixed rule (one on, one off: ON to
  all), the group's hit rule (a hit while any fan is off: ON to all), a hit's flip, and an ON message (the mod's own
  included). In the failing stretch of the logs every ask came with a swing of the other player's stand-in in that
  game: a stand-in's weapon hitting the real fans is a hit there.
- The failure followed the game (player 1's), not hand-glued against Autobuilt: nothing the node, the group or the
  handler reads differs by a part's origin; green is read by none of them.
- An ON is refused while the energy gate is closed (DeviceSwitch +0x8d bit 0 clear).
- Not read: who drains the message ring and when, in frame order against the node's calc and the group sender; the
  writer of the comp 11 word.

**Hits (hits-status) - note 2**
- A jump or fall attack is a normal weapon contact plus the damage attribute ZonauGearChangeStateOn (bit 54 of the
  contact's 64-bit mask); no other attack of the player has it. None of the five hit-toggle functions reads the mask.
  What consumes bit 54 was not found, so whether a jump attack switches a device by message or by the same toggle bit
  is not decided by reading. If by message, the mod's follower branch (which watches the toggle bit only) cannot see it.
- The shockwave is not a toggle path (its damage type is rejected by the hit filter).
- The toggle sites keep no reference to the attacker; the filter passes weapon, shield and arrow contacts of an actor
  whose affiliation is Player. Whether a stand-in's affiliation is Player is not read.
- Status effects on parts, what kills a burning part, damage and explosions on parts: not read in this pass.

**Spring and stake (spring-stake) - notes 4, 5**
- A spring ignores the set-on / set-off messages: it is in separate-message mode and takes only 0x2ca80000,
  0x2ca80002 and 0x6b510003. Its latch is set by writing the on/off node's event byte (node +0x40 bit 0 on, bit 1
  off) between two calcs, which needs the path from an actor to its node (engine-access).
- The spring's state is four layers: the latch; the launch, which is the tick the latch changes and not a state; the
  post-launch timers; and the extension, which is the head body's pose against the base and no node field. A follower
  set to the runner's latch produces the pulse itself; the head's pose comes with the bodies.
- The launch changes only the velocities of the spring's two bodies (head +v, base -v/3); it never touches the player.
- The mod drops the launch on a followed spring: the spring's apply main+0x221a7dc is in force_nodes, so both velocity
  writes are skipped in coop_phase0_hook after G1 has let them through. The fix is to pass return addresses
  main+0x221a9c0 and 0x221aa14 in that rule too, or to take the apply out of force_nodes.
- The old toggle path (coop_bonds.c) sends edges, and tests "on" with the comp 11 word: two games differ by one edge
  whenever they disagree on the count. That is note 4.
- A stake's hold is its own authored constraint "ToGround", re-pointed at the body it hit by main+0x2215364 and
  disabled by main+0x2215118; neither is hooked, so a followed stake connects itself to the follower's ground while
  the runner's pose already carries the hold. A followed stake must not connect: main+0x2215364 returns for it.

**Wheels, sled, carts (wheels) - note 6a**
- The wheel node the mod skips is a twin no shipped graph uses. motor_nodes names main+0x22fc5f8; the wheel that runs
  is ExecuteFastWheelMotion, wrapper main+0x22fa1b0, body 0x22fa3fc, and it is untouched today. Its inert test is the
  call at 0x22fa5e4 (return address 0x22fa5e8), not 0x22fc918.
- The wheel has no notion of a rider: it sees IsWorking (the on/off latch), the inert test (held, carried, just
  released), and the group steering bit 5 with the stick numbers. Its disabled branch un-anchors and disables its three
  motor constraints and writes its five model floats to 0; the next working tick re-anchors by itself.
- The visible spin is the blackboard float FastWheel_ModelRotateSpeed (0x4b7368f5), integrated by the model node
  main+0x220b9d4; steering pose is FastWheel_ModelSteeringAngle (0xbc8c3de4). Only the motion node writes them. A
  follower shows the right spin by writing those two floats, or by its node running.
- A wheel drives only while the group has steering (group +0x878 bit 5), which exists only in a call that saw event
  0x11 and a stick member whose ForceAllPartsOn bool was true. The group steering numbers at group +0x928/+0x92c/+0x930
  are the stick blackboard floats StickX, StickY, ForwardBackMove, set to NaN at the start of every group pass.
- Wheels spinning for ever (6a): the wheels stay on until an OFF reaches them; the OFF comes from the stick release
  chain, which did not run in the game that ran the build (stick-head).
- Classes that run, with their gates: sled main+0x2458320 (inert test at return 0x2458428; writes its own body
  velocities and side-edge constraints), cart 0x2251440 (inert at return 0x2251a8c; constraints only), homing cart
  0x225c52c (inert at return 0x225c6c0; three constraints and an impulse), merged wheel 0x236d2e8 (no body write, queues
  message 0x08e5001a to what it touches), LargeWheel 0x246ba9c / 0x246cb88 / 0x246d930 (hinge motor and steering, gated
  by IsActive only, no inert test).
- Cart and wheel visuals (post-calc nodes) are bone posing only and work on a copy.
- Not read: the tuning values at [main+0x395f3a0]; what a wheel does at command 0; the consumers of the comp 81 marks.

**Energy (energy)**
- The per-device "can be on" bit (DeviceSwitch +0x8d bit 0) is rewritten at the end of every call of main+0x4a3500 from
  this game's own gauge manager: blocked = mgr[0x50] bit 0 or not mgr[0xe8] bit 0 (the game flag Obj_Battery_Get). A
  follower's copy therefore consults the watching player's gauge, not the payer's.
- It is also cleared for a device out of range of this game's Link (range 20, mgr +0xc4, against mgr +0xa0..0xa8) when
  the group's bit 6 is clear and no exemption holds, and for 6 s after a "substitute energy" event. A watcher far from
  a build would switch its copy off by this bit.
- The node forces OFF and refuses ON while the bit is clear. So for a part this game does not pay for, the bit must be
  set after each call of main+0x4a3500 (the function rewrites it every call).
- The group sender has its own gauge-cannot-pay OFF (main+0x32f4e8..0x32f4fc, needs mgr[0xe8] bit 1, group +0x9e8 == 1
  and no battery in the group).
- Draw = ds+0x3c * ds+0x40 * ds+0x44; +0x44 is the group factor carried by message 0x6b510002.
- A battery part: its node only publishes cur/max of its life cell; the code that lowers the cell was not read.
- Not read: the writer of mgr[0x50]; mgr[0xe8] bit 1; the frame order of the DeviceSwitch update and the node calc.

**Balloon, hover stone, wing, stabilizer (lift-devices)**
- All four are force nodes whose body writes go to the part's main body only: balloon (impulse main+0x29ae7a8, two
  linear and two angular velocity writes), wing (force 0x29ae7fc, linear velocity at two sites, angular at six, the
  rail path included), hover stone (linear at 0x230aa08 and 0x230a530, angular at 0x230aa30), stabilizer (0x40e490 and
  0x29ae7fc). No constraint setter, no other body. force_nodes drops exactly these on a follower.
- Balloon: the swell shown is node +0x20, not +0x2c. It follows "any heat" from the balloon's own fire sensor, so a
  follower swells by itself when its own copy of the fire is under it. With no fire a balloon gives no lift, only a
  brake on falling.
- Hover stone: the target pose is local to each game's node, captured from the body at the node's own working edge
  and replaced by the body's pose whenever it is out of tolerance or held. Nothing needs to be sent or handed over.
  Its energy range is the one device with a custom range (50), tested against the watching game's Link (energy).
- Stabilizer: works whether switched on or not; needs only the group's count of stabilizers (group +0xa1e), which the
  group refresh sets in each game.
- Wing: reads no on state; publishes Wing_LiftRate and Wing_IsOnRail to its blackboard; the full lift law is not read
  (not needed: the runner's motion is what a follower shows).
- Not read: the consumer of the model component's request table (comp 81), so whether those requests are physical.

**Rocket and time bomb (rocket-timebomb)**
- Rocket: on by the ordinary set messages. After the working edge it waits 20 frames, probes for its own build in
  front of the nozzle (a blocked launch fails with a tumble), then thrusts every tick. Its burn is its own life
  (MaxLife 90, 1 a frame once RocketEngine_IsSucceededLaunch is true), and it ends by death kind 16 (life depleted) and
  a plain delete with the vanishing effect: no blast. The rocket node asks for no death itself.
- A follower rocket derives effects and animation from the on state alone. Not dropped today: its gravity-cancel
  requests to comp 81 each thrust tick, and two draws of the global random generator on a failed launch.
- Time bomb: ignores the set messages (separate-message mode). Armed by message 0x2ca80000; message 0x2ca80002
  detonates at once. Fuse: node +0xdc (now), +0xe0 (previous), total 110 frames, stepped only while armed and not
  held or attached; a value written from outside is honoured. At 0 the node asks death kind 8 (exploded) and calls its
  blast virtual every tick. The blast is the chemistry explosion of size 20.
- A follower bomb must have its fuse written each tick from the runner's and kept above 0, or it detonates by itself;
  the runner's detonation is then replayed as death kind 8.
- The death kinds are named (table at main+0x366f8b0): 6 eaten, 7 picked up, 8 exploded, 9 burned, 15 kinematic
  collision, 16 life depleted, 20 the Autobuild part's own death.
- A mod bug to check: the death-component hook in coop_life.c takes the actor from component +0x8, while the game's
  function main+0x1b4b738 takes it from component +0x18. If +0x8 is not the actor, G4 refuses nothing for deaths
  asked straight on the component (the bomb's own, and the broken-off rule's inlined one).
- Not read: the blast virtual's target; who subtracts the rocket's life; who sends the two bomb messages.

**Emitters and visual kinds (emitters)**
- Beam, flame, frost, shock, light, mirror, hydrant and the wind capsule read nothing only the owner has: given the
  on and working state a copy runs the same code and shows the same. Each game makes its own beam actor, element pool
  and wind capsule.
- Harm is not done by the emitter nodes but by the attack sensors and chemistry of the game that runs the emitter, so
  a copy's beam or flame harms in the copy's game as a real one does: hits by a copy's emitter have to be told from
  the runner's by their attacker so they are not counted in two games.
- The light also takes forced on/off messages 0x6b510004 / 0x6b510005, sent only by the contraption controller update
  main+0x1917ac0 to members of kind light.
- The pot's flame state is a debounced "upright, not held" test of each game's own copy; no switch.
- Emitters hold fire while their group is in hold state 11 and 0.5 s after; groups are computed in each game.
- Not read: the chemistry steps that burn, freeze and electrify; who writes hold state 11.

**Spawns (spawns)**
- The cannon and the beam make no actor when they fire. Each has a Shooter component that makes its projectiles in
  advance (the cannon up to 8 Obj_CannonBall_A_01, the beam one BeamosBeam), in every game that has the part, copies
  included. Firing is one call, Shooter::shoot main+0x1bf8df4 (cannon: return address 0x224f538; beam: from
  0x2232d0c), which launches a ball already made. A copy whose node runs fires its own ball unless that call is not
  reached for it.
- The cannon the game runs is ExecuteCanonMotionPostCalc (calc main+0x224e0b8), not the node the earlier notes named.
  The launch velocity is data the cannon writes into the ball's blackboard (Obj_CannonBall_InitVel) before its first tick.
- The ball and the time bomb explode through the chemistry system (a Blast element), not through an actor.
- The actor-create function has 108 direct call sites; none belongs to a device node. Ice from the frost device was
  not traced.

**Engine access (engine-access)**
- No path from an actor to its AI nodes was found. A node is reached only by noting it when its calc runs (x0 = node;
  node -> actor is [[node+8]+8]+0x1f8), or from another node of the same actor through the graph (count G+0x60, array
  G+0x68). So the mod keeps its own table of (actor, calc) -> node, filled from the calc hooks it already has.
- The blackboard is reached from the actor alone: bb = [[[actor+0x228]+0]+0x10]. Bool section at bb+0x198 (values
  bb+0x1a0, hash table bb+0x1c8, size bb+0x1d4), float section at bb+0x120 (values bb+0x128, table bb+0x150, size
  bb+0x15c); slot = hash % size, {hash, index}, linear probe, hash 0 ends. Keys are murmur3_32 of the name, seed 0.
  The game's own bool write also bumps a change counter (bb+0x1f0) and honours a lock byte (bb+0x1e0).
- Writing IsOn / IsWorking from outside is overwritten by the node each tick; the latch itself must be set.
- The on/off event byte in the blackboard (entry 0xc9efe622, byte +8) also charges the gauge: not for a follower.
- Comp 81 requests live one frame; whether they act on physics is not read.
- The two inert tests are pure reads of this game's own state (held, carried, comp 36 state 4, the group's held bit).

### 34.1 What this changes in the code (each item written from the folder named; none run)
1. **"On" is read from the blackboard bool SpecialParts_IsOn**, not from the comp 11 word (setoff-not-taking,
   engine-access). Every on/off compare in coop_ctn.c uses it.
2. **Touch is "this game's Link holds it"**: the holder handle at [SPR+0x30]+0x10 resolved and compared with this
   game's Link, with the held bit; the stick rider test also requires the occupant in state 2 to 6 (stick-head). This
   is the fix of 6a's false touch.
3. **The game of the rider runs the build.** With touch right, steering makes the rider's game the runner, and the
   stick's own release chain then switches the devices off in the game that runs them (stick-head, wheels).
4. **A stand-in's weapon does not hit devices.** In the game where a player is only a stand-in, its swing reaches the
   real parts and flips them, while the same swing in its own game is sent as a wanted state: two switches for one hit.
   That matches every failing ask of note 3. The hit filter must refuse a stand-in as attacker (hits-status,
   setoff-not-taking). Needs one more read: which actor the filter sees as "other" for a weapon contact.
5. **On a followed build the group sender sends nothing**: the mixed rule and the hit rule would switch a copy back
   on against the runner. Today only the hit event is taken off at the sender's entry.
6. **A part this game does not pay for always may be on**: after each DeviceSwitch update main+0x4a3500 its +0x8d
   bit 0 is set, so a watcher's empty gauge or distance does not switch a copy off (energy).
7. **Spring**: its latch is set through its on/off node's event byte (node +0x40), found by the node table; the two
   launch writes are let through in the phase-0 rule (return addresses main+0x221a9c0, 0x221aa14); lone springs leave
   coop_bonds.c (spring-stake).
8. **Time bomb**: armed with 0x2ca80000; its fuse written each tick from the runner's and kept above 0; never
   0x2ca80002; the detonation comes as the runner's death of kind 8 (rocket-timebomb).
9. **Wheels**: the hook moves from the unused twin to the real node (main+0x22fa1b0 / 0x22fa3fc); on a follower the
   wheel takes its disabled branch (inert test forced at return 0x22fa5e8) and its two model floats are written from
   the runner's (wheels). The same per-part float channel carries the stick's four tilt floats (stick-head).
10. **Stake**: main+0x2215364 returns at once for a followed stake (spring-stake).
11. **Cannon**: Shooter::shoot is not reached for a followed cannon; the runner's ball is the runner's object (spawns).
12. **To check in coop_life.c**: the death-component hook's actor offset (+0x8 against the game's +0x18).

### 34.2 Not decided by this reading (a reader goes back for each before its code is written)
- What consumes the jump attack's attribute (bit 54 of the contact mask): decides note 2.
- Which actor the hit filter resolves as attacker for a stand-in's weapon, and a stand-in's affiliation: decides
  item 4's exact test.
- Who drains the message ring and where in the frame, against the node's calc and the group sender.
- Whether comp 81 requests are physical (rocket gravity-cancel on a follower).
- What consumes a copy emitter's hits (beam, flame) so harm is not counted in two games.
- Status effects on parts, damage and explosions on parts (not touched by this pass).
- The seat release function; whether a stand-in ever performs a ride start (rider placement, 6c).

### 34.3 Written from this reading so far (not run; continues the list of 33)
- **"On" from the blackboard (34.1 item 1)** - coop_ctn.c part_on, actor_bb, bb_index. Every on/off compare and the
  state sent now read SpecialParts_IsOn; the comp 11 word is the fallback for an actor without that bool. A run
  shows: the "there on / here" lines agree with what is on screen in both games, and set-off is not repeated.
- **Touch by this game's Link only (34.1 item 2; 31 note 6a)** - coop_ctn.c part_held, stick_rider. Held needs the
  held bit and the holder handle resolving to this game's Link; a rider must be in state 2 to 6. A part that reads
  held by someone else logs "a part reads held, and not by this Link". A run shows: carrying another player's build
  still takes it over (the carry scenario), and a build is not taken by a game whose player is nowhere near.
- **The group sender silent on a followed build (34.1 item 5)** - coop_life.c coop_life_v3 (main+0x32eb90 wrapped,
  its queue pushes main+0x2b76c34 dropped inside), coop_ctn.c coop_ctn_sender_entry. A run shows: a copy's devices
  change only when the runner's do; "set off" is not sent again and again.
- **A device another player pays for may always be on (item 6)** - coop_autobuild.c, the DeviceSwitch update: +0x8d
  bit 0 set after each update for a part this game does not pay for. A run shows: a watcher with an empty gauge, or
  more than 20 m away, still sees the build's devices on. Not done: the payer's own empty gauge switching the build
  off where another game runs it.
- **Spring (item 7)** - coop_autobuild.c lets the two launch writes through for a followed spring; coop_ctn.c dev_msg
  sets a separate-message device's latch through its on/off node's event byte (node +0x40), the node found in a
  table filled when each on/off calc runs. A run shows: a spring in a build fires in both games on the same switch
  and both end extended or both retracted. Lone springs are still on the old path until lone devices enter the core.
- **Wheels and stick tilt (item 9)** - cpu.h, coop_life.c, coop_autobuild.c: the real wheel node main+0x22fa1b0 runs
  once on a followed wheel with its inert test (return main+0x22fa5e8) forced true, then is skipped; coop_ctn.c sends
  each wheel's model spin and steering floats and each stick's four tilt floats with the state and writes them on the
  copy at phase 0 and at the tick. A run shows: wheels turn on both screens while driving and stop when the car
  stops; the stick leans on the other screen. Not read: whether the stick's own graph writes 0 over the tilt on a
  follower in between (it would flicker).
- **Stake (item 10)** - coop_life.c: main+0x2215364 returns at once for a followed stake.
- **Cannon (item 11)** - coop_life.c: a followed cannon's fire function main+0x224e690 runs with Shooter::shoot
  main+0x1bf8df4 answering -1 inside it. Not checked: that the runner's ball reaches the other game as an object.
- **The death-component hook (item 12)** - coop_life.c reads the actor at component +0x18, falling back to +0x8.

## 35. What the second reading established (read 2026-11-09, R/sync-v3/m2/p6/, seven folders, all read in full)
Static reading only; nothing here has been run.

**Who hit (hit-attacker)**
- The hit filter of the device toggle (main+0x1d38a00) takes only the device; it walks the device's own contact
  records. A record names its attacker by two handles: +0x60 the owner of the other body (the weapon), and, when byte
  +0x94 bit 0 is set, +0x78 the weapon's topmost owner (who wields it; main+0x1d19380 follows weapon holder, shooter
  link, hold link). A sword has no affiliation of its own, so a sword contact counts only through its wielder.
- Whether a stand-in's own affiliation reads as Player is not read (the pack says the same as a Player's, the actor
  table row does not). The test written does not depend on it.
- Three of the five toggle functions read no actor at all; a hit through them cannot be attributed.
- An arrow the mod makes as a copy has no shooter link and very likely never passes the filter.

**The jump attack (jump-attack) - note 2**
- The jump and fall attacks' mark is consumed by one function on the attacker's side, main+0x1b640f4. It writes the
  device's switch event byte (blackboard entry 0xc9efe622, byte +8): bit 0, "switch on". It sets no toggle request and
  sends no message. The on/off node reads the byte at the start of its calc and clears it; a device already on ignores
  bit 0. So a jump attack is a set-on that the mod, which forwards only the toggle request, never saw.
- A plain swing goes through the same function too (event bit 0 or 2), besides its toggle request.
- Not read: what the two do together on a device that is already on.

**Message delivery (message-order)**
- Messages queued with main+0x2b76c34 go into one ring, drained by main+0x2b772b0 in push order, in one pass, under
  the ring's lock; both set-offs of a build are delivered back to back and both devices change in the same frame.
- A full ring drops a message silently (capacity 100 unless a parameter block says otherwise).
- The latch changes in each device's own node calc, on a job thread; between two devices' calcs the group's mixed rule
  can in principle see one off and one on. With the group sender now silent on followed builds (34.3) that rule can
  act only where the build runs, on messages of the same pass.
- The on/off node handler is reached through the node's own message interface; no direct call exists. Which component
  forwards to it, and the order of the drain against the mod's phase-0 sends, are not read.

**Riding (seat-rider) - notes 6b, 6c**
- A stand-in cannot ride: the stand-in actor is built without the Rider component, and the ride start reads it with no
  check. Nothing is steered by a stand-in, rightly or wrongly. What draws the rider at the stick in the other game is
  the mod's own placement (coop_players.c ride_place); 6c is to be looked for there.
- The stick node's "engaged" input is the stick's own on/off latch, whose condition is a boolean of the seat test.
  It is true only where the seat is really occupied: the rider's game.
- A seat is emptied by the rider's end of ride, by the rider's teardown, or by the ridden actor's own per-frame check
  when the occupant is dead or gone.
- Not found: the code that puts a rider on the saddle each frame.

**Comp 81 requests (comp81-consumer)**
- Its consumer is its own update main+0x447df0. The requests are physics properties of the body (mass, inertia scale,
  the gravity/damping preset, five factors), never a force or a velocity. The rocket's "gravity cancel" is a mass
  and inertia override. A follower whose velocity the mod writes is not pushed by them, and with its node running it
  has the same mass and inertia as the runner's. Nothing to change.

**Harm by emitters (emitter-harm)**
- The damage of a beam or flame is the game's generic hit path in the game that runs the victim; the record's
  attacker is the beam actor or the pool sensor, and through the owner chain the device, and the Link only when the
  Link holds the device.
- Each game runs its own beam and flame on a build it has. On an enemy, the copy's hit is kept and plays the reaction;
  the enemy's life is its owner's. It is counted twice only where a hit is attributed to this game's Link and sent to
  the enemy's owner while the owner's own emitter hits too. The fix belongs in coop_world.c contacts_filter: a contact
  from a device this game only follows is taken out. A compilable test "this contact comes from device D" is in the
  folder.
- Not read: the life and status consumers; who writes a shot's creator link.

**Status and damage on parts (part-status-damage)**
- Zonai device parts take no damage from weapons or blasts (all their damage rates are 0) and cannot burn (stone).
  The exceptions are the parts that drain their own life (rocket, battery, time bomb, spring, wing, balloon).
- Wooden parts burn: the chemistry update asks death kind 9 every frame once an element is burned out, and ignition
  damage lowers life, whose writer main+0x4400b0 asks death kind 16 itself at life 0, straight on the death component.
- Burning is read as the element state byte (2 burning, 3 burned out) in the chemical component's element list;
  electrified as a bit beside it. How to write a status so its effects start and stop is not read.
- On a follower every such death is asked each frame and refused; the copy burns or stands at life 0 by its own
  local causes until the runner's delete comes.
- Blast pushes on parts are main+0x312718 and message 0x08e5001a, both ending in the impulse call the mod already
  keeps off followed bodies.

### 35.1 Written from it (not run; continues 34.3)
- **A stand-in's hit on a device is no hit here** - coop_life.c, the hit filter main+0x1d38a00 run and its answer
  turned to 0 when every contact on the device is a stand-in's (its weapon, or anything it wields or shot). A run
  shows: the line "a stand-in's hit on a device here is not taken as a hit", and fans switched by the other player go
  off once and stay off (31, note 3). Not covered: a frame with both players' contacts on one device, and the three
  toggle paths that read no actor.
- **Jump attack and other set-ons (31, note 2)** - coop_ctn.c coop_ctn_event_seen, events_out, events_put: the
  switch event byte put on a followed part is taken off before the node here reads it and sent to the runner (message
  EVENT), which puts it on its own part at phase 0. Not sent when the same hit asked a toggle. A run shows: the other
  player's jump attack switches a build on in both games.
- **Time bomb in a build** - coop_life.c notes the fuse node (calc main+0x2474478); coop_ctn.c sends fuse and armed
  with the state and, on the copy, arms it and writes the fuse, never under 2 frames. Not done: the blast shown on
  the copy (it needs the runner's death replayed with its kind, part of 33.3).

### 35.2 Still to write (nothing blocks them but the writing; none needs a reader first unless said)
- Structure in place (33.3): bonds made and broken where the parts stand; a part in a build leaves the object system
  in every game; deaths replayed with their kind (the bomb's blast, a burned board). Needs our own mirror code in
  coop_players.c read first.
- Lone devices into the core (31, notes 4 and 5); coop_glue.c and coop_bonds.c stand down for registry parts.
- Emitter hits of a followed device taken out in coop_world.c contacts_filter.
- Rider placement and the stick car's lag (31, 6c): coop_players.c ride_place.
- Wooden parts on a copy: burning and life sent by the runner. Needs a read of how a status is written.
- The payer's own empty gauge switching off a build another game runs.
- **A structure change applied in place (33.3, first part)** - coop_ctn.c in_place, part_edges, bond_queue,
  bond_pool_recycle, orphans. When every part of an incoming version is an actor already in the registry here (a bond
  more or less, a part pulled off, a build split in two, two builds joined), the follower breaks the glue edges the
  record lacks, queues the bonds it has and this game lacks, and the entry takes the actors: nothing is deleted, no
  device loses its state. The relative pose of a bond comes from the record itself (each part's three body poses), so
  nothing new is sent. A part the change leaves over waits one second for another entry's version (the other piece
  of a split), then is deleted. Anything else (a part not here yet, a refused bond) is made with the build object as
  before. The follow drive takes root samples only of the version it holds.
  A run shows: the line "applied in place: N parts kept, B bonds made, K broken" when a part is pulled off or two
  builds are joined, with the build not blinking and its devices keeping their state.
  Not done by this: a loose object glued on (it is not in the registry: made again as before); a part of this game's
  own glued into another game's build; the handing of a pulled-off part to the object mirror (it is deleted and comes
  back as the runner's object, one blink). Not shown by reading: that the record's body number is the physics slot
  for a part's second and third body.
- **Lone devices in the core (31, note 4)** - coop_ctn.c lone_tick, lone_ask, coop_ctn_follows. A device in no
  build stays its owner's object, moved by the object mirror; its on state is sent by the owner as a state (five
  times a second, at once on a change) and set on the copies with the set messages (a spring through its node); a hit
  or jump attack on a copy is sent to the owner as the state wanted or the switch event; the copy's owner pays. The
  older flip path (coop_bonds.c, TOTK_COOP_DEVICE_SYNC) is switched off by setting that key to 0 when the contraption
  core is on and the key is not set by hand. A run shows: a single spring or fan switched by either player ends the
  same in both games, with the line "lone device N is on there, off here: set" at most once per switch.
  Not checked: that the key is set before coop_bonds.c first reads it (it keeps the first value it reads); a
  launcher that sets TOTK_COOP_DEVICE_SYNC=0 itself removes the doubt. Not covered: devices placed by the map (both
  games have their own actor of those; neither path follows them).
- **Still not written of 33.3**: a part of one player's glued into another player's build (31, note 5). The object
  mirror would have to keep a mirror alive, and stop placing it, while it is a part of a build, and the owner would
  have to stop sending its object without the others deleting theirs; the older claim code in coop_glue.c and
  coop_bonds.c does a version of this and has to be read and retired piece by piece, not patched around.
- **Emitter hits of a followed device (35, emitter-harm)** - coop_world.c contacts_filter, coop_ctn.c
  coop_ctn_emitter_of_followed: a contact on an enemy's copy whose attacker is a device this game only follows, or a
  shot of one (by the shot's creator link or the device's Shooter list), is taken out. A run shows: an enemy hit by a
  followed build's beam loses life once, and the line "by a followed device's ...: taken out". Not read: what a
  flame's sensor names as its owner; if it is neither the device nor a shot of it, flame contacts are still kept.
- **The rider on a build's stick (31, note 6c)** - coop_players.c ride_place: a ridden object with no mirror copy (a
  part made from a record) is looked up in the registry and the rider placed on it where it is in this game. Likely
  the cause of the rider floating behind the car, not shown by a read of a log.

## 40. What the third reading established (read 2026-11-09/10, R/sync-v3/m2/p7/, five folders, all read in full)
Static reading only.

**The refused delete (delete-refused) - note 12**
- The delete request answers 0 for three reasons only: the actor is going (state 7 to 11), a delete was already asked
  and accepted (actor +0x28 bit 17), or lock lane 2 is held. A second ask after an accepted one answers 0. The mod
  took that 0 as a refusal and asked for ever; the fan of note 12 may simply have been accepted the first time.
- An accepted delete waits, with no time limit, until lane 0 is free and the actor's latch (+0x1b0) is 0.
- Who holds lane 2 on a Zonai part is not settled: the holders found keep a reference to the actor.

**The spring (spring-bodies) - notes 4, 14**
- The head is a second body on a slider joint with a one-way motor (pulls the head in) and a spring (pushes it out).
  The value the node writes is the joint's upper limit, not a target; the two flags are spring on and motor on.
- Idle, the motor is on and the head is in. On a launch the node switches the motor off, kicks the head, and for
  ordinary launch speeds writes nothing more: after that the head is where physics leaves it. The joint holds no
  extension. So two games agree only if the head's pose is sent.
- Nothing moves the head when the base is placed from outside: it lags and is dragged by the joint.
- The joint is found from the actor by name, its state read by plain reads, and set by the game's setters.
- Other parts with a second body that is not fixed to the main one: wheel (suspension), big wheel (spin, steering,
  suspension), construct head (neck), cooking pot (pan).

**Off screen (offscreen) - note 14**
- Every actor update phase opens with one gate: a per-actor calc mask (+0x3fc; bits 0 and 1 skip the whole update,
  0xc4 its components) built from "reasons" that code sets, and a blocked bit (+0x5ac bit 6). A skipped actor still
  gets one full update every 150 (or 30) counts. Messages are still taken while an actor is skipped and applied at its
  next update, off before on.
- What sets the mask from view or distance was not found, nor which phase runs the AI nodes. So "the game stops a
  copy's device logic when it is out of view while the mod keeps placing its body" is supported as a mechanism and
  not shown. The mod now logs the gate of each lone copy when it changes; the next session's log decides it.

**The stick's lean (stick-graph) - 6b, 14**
- On a stick with nobody in its seat nothing writes the four tilt floats each tick; they are zeroed once when the
  seat empties. So the mod's writes are not being overwritten, unless the copy's seat reads as occupied.
- The stick's animation reads the float through a pointer; whether that is the actor blackboard's float is not read.
  The game's own float write also bumps a change counter the mod's bare store skipped.
- The cause of the missing lean was not found. The stand-in's own riding pose follows the Player's left-stick
  inputs of its animation, not the stick's floats.

**A device staying off (gate-stuck) - 14**
- Nothing in the on/off node, the DeviceSwitch or the group latches from a closed energy gate or a refused set-on;
  the gate is worked out again at every update. For a loose device the gauge is consulted only once its own switch
  state is 1, which a local hit toggle makes it.
- The logs of the session show each loose emitter's on state following its owner, one set per change. So the gate
  did not hold them off. Two candidates remain, neither confirmed: a hit on a copy taken by the copy itself before
  the copy was in the mod's lists; or the copy on but dark, its beam lost (the object mirror made and destroyed
  copies of the owner's beam actor beside it).
- In the mod: a lone copy first heard of was in the "follows" list a tick before it was in the no-charge list.

### 40.1 Written from it (not run)
- **Delete (note 12)**: coop_ctn.c part_delete takes an already accepted delete (actor +0x28 bit 17) as done, and
  logs the lock lanes when the game does refuse one.
- **The spring's head (notes 4, 14)**: coop_ctn.c springs_phase0, spring_joint, spring_read, spring_apply. The game
  that runs a spring reads its extension, upper limit and the two enables ten times a second and sends them (with a
  build's state, or with the lone device's); a copy sets the joint with the game's setters when they differ and puts
  the head at that extension from its base when it is more than 5 cm off. A run shows: a spring fired by either
  player is extended on both screens and retracted on both, also after one player looked away.
- **Floats by the game's own write (6b)**: at phase 0 the wheel and stick floats go through main+0xc3e260, which
  bumps the change counter. Not known to be the cause of the missing lean.
- **Lone copies**: lone_tick runs before the lists are rebuilt (no tick without the no-charge rule); each lone copy's
  update gate is logged when it changes ("its update is not run by the game now").
- **A device's own beam is not sent as a plain object** (coop_players.c report): each game's copy fires its own.
- Not written: the stick's lean beyond the float write (cause not found); the off-screen difference itself (to be
  decided by the gate log); the rider's lag.

### 40.2 Written while the next reads run (not run; ordered by the tester: "do all of that")
- **Deaths made the same on the copies (36 item 4)**: coop_ctn.c coop_ctn_death_seen, deaths_put; coop_life.c. The
  game that runs a build sends each death one of its parts is asked, with the kind; the others ask the same death of
  their copy with the game's own request at phase 0, the G4 refusal lifted for that call. A run shows: a time bomb in
  a build blasts on both screens; a part broken off an Autobuild vanishes on the copy with the game's own effect.
  To watch: a copy's blast is a real blast in that game (it pushes and hurts there too).
- **The payer's empty gauge (36 item 3)**: coop_ctn.c gauge_blocked, dev_follow. The gauge manager is noted from the
  game's own charge calls; when this game's player pays for a build another game runs and the manager reads blocked,
  the build is asked off where it runs, once a second. Not known: that "blocked" is what an empty gauge reads (the
  energy reading has it as likely).
- **A build glued only from another player's objects (39.5)**: record_pass also starts from this game's copies of
  the other games' objects that no build has and that are in a group here; coop_players.c coop_copies_all lists them.
- Reads started (Sonnet, R/sync-v3/m2/p8/): wood-status (burning parts alike), weight-push (P6: standing on, weighing
  down and pushing a build another game runs), unload-travel (P7: what removes and restores builds), limits
  (capacities a session with several builds can reach).

## 41. What the fourth reading established, and the designs for P6 and P7 (read 2026-11-10, R/sync-v3/m2/p8/, four folders, all read in full)
Static reading only.

**What removes a build (unload-travel)**
- Nothing sweeps. Every actor tests itself each frame against its own radius from the game's observers (the player
  and camera slots): DisplayRadius + DeleteRadiusOffset of its kind, 60 m for a capsule's item form, 90 m for a fan,
  wheel, rocket or stick, 100 to 130 m for cart, sled, balloon and wing, up to 310 m for a spring. Out of range it
  asks its own delete with reason 3.
- So a build goes part by part, each at its own radius, and a follower's copy is deleted by the engine well inside
  the 160 m the plan's section 10 assumed.
- A part made at run time (from the pouch, by Autobuild, glued by hand) is gone for good: no code was found that
  makes it again, and no save record of placed parts. Only the Autobuild history designs persist.
- Fast travel has no "world unloaded" call for such parts: the observers jump, every part fails its range test and
  deletes itself in the next frames. Map actors go with reason 6 when their map unit unloads.
- No "oldest part is removed" rule was found; at the run-time placement pool's limit a new actor is simply not made.
- The unload callers are listed by return address; a delete of reason 3 (also 5, 6, 10) from them is an unload, a
  delete of reason 0 from the death procedure is a destroy.

**Burning (wood-status)**
- A status is not written into an element: reactions add into a per-frame accumulator, a tick advances the burn
  timer, and an evaluator derives the status. The step that stores the result was not found.
- The game's own nodes set a status by request: ignite main+0x1b34a44(comp, 2, -1), extinguish and burn out
  main+0xb2b804(comp, 0 or 2, -1). The flame, the blackboard flag and the sound follow the state.
- A wooden part burning alone loses no life; it ends when it burns out, by death kind 9. Zonai device parts cannot burn.
- Every reaction into an element passes main+0x1906768; a hook there could stop a copy igniting from local causes.
- Corrections to 35: the "request A" of the chemical component is a lightning strike, not a status setter.

**Weight and push (weight-push) - P6**
- The game sends no force for a Link standing on something or walking into it. The Link's body (10 kg) is moved by
  velocity and its weight and push are what the contact solver does between two real bodies. There is nothing to
  relay. The one impulse the controller applies by code (PushObject) was not shown to run for the player.
- What the Link stands on is in no field; it can be read each frame from the controller's contact list, or at the
  ground ray's call (main+0x29a9398 with return address main+0x1f556c4).
- A stand-in has the Player's bodies by data, but today its main body is static and teleported, which weighs
  nothing down.
- Ranking of the three ways by what the code supports: let the stand-in's body touch the build (a moving, velocity
  driven body and a narrower veto) > an explicit force at the ground body (works, its size would be tuned, not read)
  > relaying impulses (nothing to relay).
- A fault in the mod: G1's entry for main+0x29a4afc compares x0 with a body, and that function's x0 is the world;
  that write is never dropped on a follower. Only the stick's force node uses it and that node is skipped whole.

**Capacities (limits)**
- All overflows are silent. Bond slots 512 (a 21-part build uses up to 21: about 24 such builds); group records 256;
  the actor message ring 100 by default (a 21-part build toggling pushes up to 21 in one frame: four such builds
  toggling in the same frame fill it, and a full ring drops messages with no trace); the Autobuild descriptor pool 21
  per build object. The number of connected sets and of graph node ids could not be read; plain-read functions for
  every pool are in the folder for a status line.
- The actor create function answers with an error code when a table, the heap or its queue is full.

### 41.1 Written from it (not run)
- **A build unloaded where it ran is handed on (10.3)**: coop_ctn.c record_pass sends RELINQUISH when none of a
  build's parts is left in the game that ran it; a game that still has two or more of its parts takes it (GRANT) and
  sends its version. With the unload rule of 33.1 this makes "walk away, the other player keeps the build" whole for
  two players. For three, two games could both take; not handled.
- **Burning parts (36 item 2)**: coop_ctn.c burning_phase0. The runner reads, five times a second, which parts burn
  (the game's own test) and sends the mask with the state; a copy that differs is set alight or put out with the
  game's own requests. The burned-out end comes with the death relay of 40.2. Not shown by reading: that a request
  brings the copy to the same state. Not done: keeping a copy from igniting by local causes (the hook at
  main+0x1906768), loose wooden objects that are no build's part.

### 41.2 Design for P6 (riding, weight, push), not written
1. A player on a build another game runs is, in the runner's game, a stand-in whose body really touches the build:
   while that player stands on or rides a part, the stand-in's standing body is made a moving body driven by
   velocity toward the rider's place on the part (ride_place), and the veto lets that one body meet that build.
   Weight and push then happen in the runner's solver as they do for its own Link.
2. The player's own game keeps dropping writes to its copy; its Link stands on the copy as on any moving floor (the
   existing ride-along).
3. What the Link stands on comes from its controller's contact list, so "rides part X of build Y" is sent as an
   identity the runner can find (the part's origin numbers), not a position.
4. To read before writing: how the contact-mass scale enters the solver (it decides whether the Link weighs 1 kg or
   100 kg on a build); whether the stand-in gets a controller record; the Player's riding pose inputs (6b/6c).
5. To fix with it: G1's body test for main+0x29a4afc.

### 41.3 Design for P7 (lifetime and session), partly written
1. Walk away: written (33.1 unload at the follower; 41.1 hand-on at the runner). The follower's "back within half
   the distance" rule should use the part's own delete radius (readable per kind) in place of the distance at which
   it went.
2. Fast travel: the same two rules apply, since each part deletes itself after the observers jump; to tell travel
   from walking away use the delete reason and caller at the mod's delete hooks, as listed in the folder.
3. Nothing persists across a reload in the game itself, so nothing is restored by the mod either: a build exists
   while some game has it loaded. When no game has it, it is gone, as in single player.
4. Leaving: the build passes to the lowest-numbered game that has it (written before, 5.9). Joining late: the runner
   sends every build's version to a game that joins (written). Rejoining with a new incarnation: to be checked
   against the registry's keys.
5. Capacity: a status line of the pools every 10 s, and the ring's fill; builds toggled in the same frame are spread
   over frames if the ring would overflow.
6. Three players: the two-game assumptions to remove are the take-over on RELINQUISH (41.1) and the toggle and
   event messages sent to all.

## 49. The readings for v4 (read 2026-11-12, R/sync-v4/, seven folders; static reading, nothing run)

Numbered on from the sections above. "Not read" is said where a reading stopped.

### 49.1 Ending an Ultrahand hold (hold-end)
- The part's hold control (main+0x1d58e80, from part_update main+0x1d58b78) never ends a hold: no distance test, no
  test of the SPR's holder or bit 16. A hold ends only through part_end main+0xb9b3a4.
- part_end runs on message 0x20510006 whose first word equals the held actor's id (+0x380), on a failed accept
  (main+0x85f7d8: target state 7 to 11, or actor +0x28 bit 17), and when a glue request's actor is dead.
- To end a hold from outside: queue {receiver = Link +0x380, id 0x20510006} with main+0x2b76c34(target +0x378, msg,
  0). The SPR does this itself at main+0x1c0662c, 0x466860 and 0xa99030. The send is queued under a mutex. part_end
  then sends 0x20510005, and the SPR runs main+0x1c0b8a8 (holder and bit 16 cleared, body woken).
- Clearing only the SPR side leaves the part in its hold state for good.
- main+0x1a45ba0 (called on a refusal) queues a feedback message 0x3dc6000N and changes no state.
- Not read: what moves the held body each frame. The part calls none of the nine body writers and makes no
  constraint; SPR-side writers are main+0x1c07728, 0x1c0812c, 0x1c07f70 from the physics routine at main+0x46fe70.

### 49.2 How a glue edge is removed, and the strain check (edge-removal)
- Every removal passes the graph's removeEdge main+0x2995024 (it takes the graph edge, bond edge +0xc0). Its
  callers: main+0x299454c (unregister) and main+0x18a1a4. main+0x2999fe8 sees only the wiggle message and strain.
- Paths: the break message main+0x1cf54d4 (wiggle, a part's death, an SPR reset, AI nodes); strain; a strand that
  fails its validity test; a body's teardown (main+0x18ac48, past 0x2999fe8); the bond manager's bulk teardown;
  two physics-side paths (body bit 41, a force check main+0x299dff0). A group rebuild and an Autobuild pull-in
  remove nothing.
- The strain check main+0x190f034 measures how far the two rest frames have moved apart (distance and angle), not a
  force. Tolerance 0.3 * k (0.5 * k for simple or tight bonds), k from the bond's strength, 0.02 when the two SPRs
  differ in main+0x1b0f00c. Strain rises at max(1, c * over * m) a second, falls at 10 a second, and at 1.0 the
  edge is removed. Its first line returns when strand +0x1e2 is set; nothing else reads the strain fields but the
  look. So a bond with +0x1e2 = 1 never breaks by strain and breaks by every other path.
- After a removal the bond edge has +0x10 bit 2 set, its ends cleared, and both sets are rebuilt (a lone part gets
  SPR +0x28 = 0). A new bond between the same bodies is independent of the old.
- Listing: main+0x27c1c0(body, list) per body; per SPR the lists at +0x158 (entries), +0x170 (strands, strand +0x50
  the edge), +0x188 (temporary joints, not bonds). Edge ends at +0x100 and +0x108.

### 49.3 The direct bond call (direct-bond)
- Edge +0xef, +0xf1 and +0xcc..+0xe0 are read only by the strand (the per-edge look and strain object). +0xef puts
  the strand in the glue manager's list and gives the lasting glue look; +0xf1 is a look branch. The strand's own
  geometry pass writes +0xcc..+0xe0. Breaking, strain, groups, the history writer (+0xec or +0xed) and the save do
  not read them. So a bond from main+0x1c05a98 differs from a hand glue in its look only.
- main+0x1915600 removes the oldest strands when more than 21 with +0xef exist in the whole game.
- SPR +0x20 bit 18 is a one-shot "structure changed" mark taken by the SPR update main+0x271190.
- The glue tick refuses when the two groups have 22 or more parts; the direct call does not, and the SPR update
  then breaks the bonds. The direct call does not check a null body slot.
- matA and matB are world matrices at the wanted pose; the bodies need not be there, the bond pulls them.
- A hand glue names bodies by global id, not slot. Slots: main+0x44bba4 (key 0 = [set +0x150], else a byte map at
  set +0x108); the history writer reads body +0x1e8 bits 1 to 4 as slots 0 to 3.
- Both callers of the bond maker run in actor-update jobs; a pool mutex is held around the maker.
- Not read: when the objects the call needs come to exist on a new actor; the call against the physics step.

### 49.4 Who creates an actor (create-callers, device-origin)
- The create request main+0x599670: x1 name, x2 argument (position +0, rotation +0xc, scale +0x30, a parameter
  list at +0x58, up to three parent actors at +0x68 to +0x78), x3 handle, x5 a placement record. There is no field
  that says why. Handle status 2 queued, 3 created, 4 failed.
- x5 is not 0 only for a map actor or its successor. On the actor the record is P = [actor +0x370], hash at P +0x88
  (all ones for a run-time create), copied from the map file.
- main+0xa1bc30 is a synchronous create that returns the actor at once and does not pass 0x599670 (the map create
  with a request byte set, and a few others; three callers through a vtable are not read).
- An inventory device is a capsule actor SpObj_<Device>_Capsule_A_01; the capsule becomes its device in a Player
  attachment method main+0x1b7f2d8, which calls 0x599670 with return address main+0x1b7f8c8, no parents, x5 = 0,
  the position of the capsule. The Autobuild part maker's create returns to main+0x1d3bb98; a Shooter's to
  main+0x1d1dbd8.
- Zonai devices have a blackboard bool ZonauGear_IsCreatedByPlayer (hash 0xa0b06281), read at main+0xa14a0c and
  0x1c4d810. No code or data that writes it was found.
- Not read: how the capsule itself is made from the pouch; a dispenser's create; drops; which job sets status 3.

### 49.5 The delete request's reasons (delete-reasons)
- main+0x63baa8(actor, reason): 0 a destroy (every death procedure, the lapse and green-kill fallbacks, the
  Autobuild hologram); 1 and 2 a failed or cancelled create; 3 the far delete; 4 a mass reset by scene nodes
  (whether a run-time part can get it: not read); 5 far, by a flag or an AI node; 6 a map unit's unload; 7 and 10
  unloads (from the earlier notes); 8 and 9 a holder deleting what it holds (inference: destroy).
- The engine's kill mark (P +0x98 bit 7) is set for 5, 6 and 8 too, so it does not tell a destroy from an unload.
- Every death ends in a delete with reason 0.
- The far unload is tested per actor every frame against DisplayRadius + DeleteRadiusOffset, to observer slots
  whose writer is not read. No exemption for a held, ridden or glued actor. A global ([[main+0x395e008]] +0xd0 not
  0) switches the far unload off for every actor. A run-time part that unloads is not made again by any code read.
- Not found: what removes actors on a load screen or a fast travel.

### 49.6 Refusing a grab, a glue, a mount (grab-refuse)
- main+0x9fb7fc(spr, kind, out) has ten callers; the grab handler's call (return address main+0x1c0c228) is the
  only one of kind 1. Answering 1 there makes the game send 0x20510006, and the part ends.
- Aiming (0x20510002) sets flags only and never asks main+0x9fb7fc. What colours the highlight: not found.
- The glue tick main+0x1d67060 ends a glue when main+0x1c070a0 (SPR(A) +0x24 & 0x2380) answers 1, when SPR(B)
  +0x150 bit 4 is set, at 22 parts, and after 1.5 s. The glue request 0x2051000f asks main+0x9fb7fc with kind 8.
- A stick is mounted through the seat claim main+0x502e60, refused by main+0x592264 or the rider's component 129.
  Whether a Player stand-in has that component: not read.
