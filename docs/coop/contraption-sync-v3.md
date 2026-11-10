# Contraption sync v3: the plan

Status, 2026-11-09: a plan. Nothing in it is built or tested yet. It replaces the approach of the phase 1-4 sections
of [contraption-sync.md](contraption-sync.md), whose motion-type names are inverted (see the note at the end of that
file).

How it was made: a fact base was first checked against the game code and this project's pair-test logs. Among other
things it found that motion types are 0 static, 1 keyframed, 2 dynamic, and that the ABLOG write watch made the
co-op's reads of the watched part fail, which explains several earlier "game behaviours". Four independent designs
were written from it, judged, merged, attacked three times and revised into this plan.

**One runner per epoch. Dynamic followers moved by one rigid velocity field. Generic "not simulated here" gates with intent marks. Structure as versions. Stand-ins never collide with builds. A disappearance is never a death. Energy is switched off only on positive evidence.**

Notation. A1..N and RC1..RC7 are sections of the fact base. GQ1..GQ5 are the answers checked in Ghidra (Q1 motion
types and flush, Q2 kill and lapse, Q3 bond queue, Q4 presets and wake, Q5 SetTransform). ER is the engine reference,
SI the stand-in notes, IM the implementation map, FC the failure catalogue and CM the coverage matrix. Those notes
quote game code, so they live with the reverse-engineering notes outside this repository (R/, folder sync-v3). CS is
docs/coop/contraption-sync.md. Addresses are main+offset. AS = assumptions (19), X = experiments (15), T = tests (17),
RK = risks (18). [v] marks what was read in the code or the notes while the plan was written (list in 0.3). Nothing
was built or run.

## Review notes (2026-11-09)
- Start with P0, which restores HEAD's dynamic root drive. The working tree's keyframed attempt (per-part poses, the
  0x6b51000a drop) was built on the inverted motion-type names and is not to be committed. Its diff is kept in
  R/sync-v3/wt-keyframed-attempt.diff.
- The central bet is the rigid velocity field over the follower's own layout (3.6). X2 measures it against HEAD's
  root-only drive. That drive already passed the grab, grab-back and stick hand-overs (0.01-0.26 m) and is the
  fallback.
- I16 is a visible compromise. In the runner's game a remote player's stand-in passes through builds, and a remote
  rider's weight is an approximate force (D3) until X14d drives the stand-in's own character controller.
- The [v] items are readings of code and notes, not runtime checks. When this file was written, three were re-checked:
  - the net version 28 in coop_net.h;
  - the same version in server/coop_relay.py;
  - the NetState ride fields.
- D1-D7 (section 22) were decided by the tester on 2026-11-09: D2-D7 as proposed, D1 with a 60 s cap on menu time.

## 0. Decision

### 0.1 Base
The physics core is unchanged from the merged design: the runner simulates natively; every other game holds the same build, made by its own Autobuild pipeline (E1), with every body dynamic (type 2, B1) and moved by one rigid velocity field per frame (C1, GQ5). Three rounds of attacks found no flaw in that core. They found flaws around it, and this plan changes these things:

| Area | Change | Answers |
|---|---|---|
| Wake | Body flags 44/45 are never set. They make a velocity last one step and the next flush zeroes it [v]. | fatal: stop-dead at promotion |
| Stand-ins | They never collide with registry parts in any world (I16). Weight is an explicit force, pushes travel as claims and PUSH. No keyframed stand-in anywhere. | fatal: infinite-mass passenger |
| Unload | Only a recorded death request or edge removal is structural (I15). Any other disappearance is an unload and hands the build on. | fatal: fast travel dissolves builds |
| Copies | A BuildObject not tied to the player's progress. X6 on an early save is a P0 gate. | fatal: no builds before Autobuild |
| Spring | The two launch calls pass on followers, so the local Link is launched natively. | fatal: springs never launch |
| Gates | Asynchronous sync work is recognised by intent marks keyed by actor, not by a thread-local flag. | DETACH and blast replays dropped |
| Touch frame | Touch hooks open the gates in the same frame; T3 runs local-touch transitions before the drive. | stick one-shot lost |
| State requests | Motion type, preset and freeze requests pass on followers; presets are mirrored from the runner. | hold preset persisting |
| Prediction | Measured acceleration instead of free fall; hold targets; horizon in the runner's world time. | hover bob, rotation overshoot, hitch |
| REST | Only for builds supported by the ground in both worlds. | bob under a held build |
| Energy | No "not charging" switch-off. A build stops only on the activator's reported blocked gauge, or when the activator leaves. A stick mount is an activation. | battery builds, menus, pilot pays |
| Damage | Follower parts take no local damage or ignition; promotion checks damage state. | damage cashed at promotion |
| Glue | Gluing onto another player's build is an edit request, not a claim. | build-together while held |
| Collisions | Followers touching a build run in this world go free for the contact. | doubled bumper-car bounces |
| Spawns | Runner-made spawns become one-part entities; harm to enemies is decided in the runner's world and forwarded. | cannon, beam, ice |
| Network | Join incarnations; per-pair sequences; control separate from bulk; ack only after storing; session-end handler; elections only on LEAVE or BYE. | id reuse, 3-player acks, inbox drops, session end |
| Versions | Ordered by (epoch, v) with crc; event-driven commits; interest-based delivery; masks applied only on equal versions. | stale versions, circular settle, late range |

### 0.2 Requirements and what delivers them
| Requirement | Mechanism |
|---|---|
| Perfect sync, as if placed yourself | One native simulator per build (I1). Followers are the same actors made by the game's own Autobuild (E1), moved by one rigid field (3). Structure through versions applied with the game's own functions (6). Device state through the game's request path (8.1). Bounded tracking error (T6-T9). |
| Built and used together | Touch moves authority with a handshake (5). Glue onto any build is an edit any player can make (6.7). Splits and merges are runner events. |
| Touch reacts as normal | The toucher's game runs the build natively from the touch frame (PENDING, 5.3), devices included. Refusals use the game's own refusal or let the newcomer wait (5.6). |
| Only the activator's battery | Charges pass only in the activator's game (J1, J2). Only the activator's blocked gauge can switch the build off (8.7). |
| All effects on both screens | On/off mirrored. Emitters and visuals run everywhere. Harm counts once (8.6). Deaths, detonations and launches replay by kind (6.6, 8.5). Internal state streamed (8.4). Riders placed on the local part (9). |

### 0.3 Read in the code and notes while planning (nothing run)
- **Flags 44/45** (not reproduced at runtime: section 23, X1). In the flush apply f_7000182eb0, when a linear velocity is applied and body+0x68 bit 44 is set, it sets request+0xd0 bit 5 and returns 0x200; bit 45 does the same for angular (+0xd0 bit 6). The caller stores the word in request+0xd4 and re-queues the block. The next flush, with +0xd0 bit 5 or 6 still set, writes zero velocity through world slots 0x190, 0x198 and 0x1a0. The setter clears +0xd0 bit 5 on each new set, and clamps speed to the body's preset limit (R/physics/functions.c body_set_linear_velocity).
- **Relays.** server/coop_relay.py hard-codes VERSION 28, times out after 5.0 s, hands out the lowest free id, writes 0 into header byte 7 and forwards every packet to every other peer. runtime/coop_relay.c does the same (5 s timeout, lowest free id, put_header writes byte 7 = 0, forward to all).
- **Session.** coop_net.c ends the session after 6 s without hearing the relay: my_id = 0, every other player marked gone, the thread exits. WELCOME sets my_id with no notice when it changes. The net thread sends HELLO once a second. The HIT queues hold 64 and drop silently.
- **NetState** has ride_owner u8, pad u8, ride_id u32 and ride_off[3] (coop_net.h).
- **The co-op tick runs in menus** (coop_players.c coop_players_tick, its pause detection) and through loads (coop_players.c follow_host_time). There is no warp hook; a warp is a jump of more than 250 m (coop_notices.c header).
- **Death call sites.** 0x1953184 called with return address 0x7000271f00, delete fallback 0x7000272178. Lapse death returns to 0x70002f8dcc, fallback 0x70002f9264.
- **Predicate call sites.** The wheel's 0x70022fc918 calls f_7001c02914. The beam (0x70022319d4), light (0x7002309c0c) and sled (0x7002458428) call f_7001c02990.
- **Ultrahand** (R/ultrahand/README.md sections 3-4): the grab refusal goes through main+0x9fb7fc (flag 0x35). The hold target is part+0x1c (3x4) with rotation at +0x1f0, 45-degree steps, a slerp limit of 270 deg/s and vel_target capped at 3 m/s.
- **Spring** (R/devices2/mechanisms/README.md, spring section 2): the launch is a one-shot SetLinearVelocity on the part's own two bodies, at 0x221a9bc and 0x221aa10. Retraction is the slider joint's motor.
- **comp +0x288 writers.** The thrust notes read them as a per-body force-override list (rocket gravity cancel, sled, wheel). The lift notes read the same writers as animation parameters (balloon swell, hover servo, stabilizer). This is unresolved.
- **Energy** (R/devices2/energy/c-gauge-callers section 3): with group bit 28 the on/off writer 0x9fad68 does not charge the Link and only sets mgr[0xe9] bit 1. 0x32eb90's gauge-blocked OFF needs bit 28 clear. 0x6b510002 carries (MultipleBonusEnergyRate*(n-1)+1)/n.
- **Glue precedent** (runtime/coop_glue.c part_here): breaks that removeScotch posts arrive later as messages, and coop_glue keeps a 2 s "parting" window per actor so its veto lets them through.
- **Stick** (R/devices2/control/README.md): the rising edge is 0x221913c. OneShotSendMessageRideOnCombinedControlStick (0x2578820) sends 0x6b510003 once, on the take.
- **Wheel and homing cart** (R/devices2/thrust wheel section 2; control section 5): when inactive, the wheel's disabled branch zeroes and disables its constraints (0x5f9dd8(c,0,0), 0xa14340(c)) and publishes the stored speed node+0x2c. The homing cart's impulse is 0x29ae114 at 0x225cbcc.

### 0.4 Not claimed
- Exact pose equality while moving. The residual is the prediction error over one sample age.
- A remote player's weight is an explicit force, and pushes are forwarded or claimed, until X14d gives native character contact.
- Hand-glued joint strands may be missing on followers (M11).
- Quest effects of glued NPCs complete per world.

## 1. Invariants
- **I1.** One decider per contraption per epoch, the runner. It owns physics, devices, structure, the kill and lapse rules, spawns and energy decisions (RC3).
- **I2.** The co-op never changes a part's motion type or freeze state and never sets body flags (B1, B3, B4, F4, C6, RC1). Presets are mirrored from the runner through the game's own preset request, because they set gravity scale and the setter's speed limit [v].
- **I3.** Follower bodies are written only by three things (C1, GQ5, F2, F3, RC6):
  - the sync's rigid field;
  - whole-build snaps in one flush;
  - the native writes listed in G1 as passed on purpose.
- **I4.** One version per world. A remake waits until the old bodies have left the physics world (merged design 0.1 evidence).
- **I5.** Identity is minted. A cid is (minter incarnation, serial) and a part is a uid. Group pointers, names and positions are caches (C4, G1-G3, RC4).
- **I6.** Structure changes through runner commits and accepted edit requests only. Followers converge idempotently (G4, RC4).
- **I7.** Every transition is numbered by (epoch, holder), acknowledged and reversible. Silence never deletes, transfers or switches off. Only LEAVE or BYE, session end, or a positive report acts (F5, F6, RC5).
- **I8.** No decision on a failed read or on a single observation (A1-A4, RC2).
- **I9.** A non-runner originates nothing (RC3, D1, D2, J1, J2). The exceptions are its own player's touch (PENDING), its glue completions as edit requests (6.7), and its attackers' hits, impulses and pushes, which it forwards.
- **I10.** Protection is lifted only on verified state: the integrity check of 5.4, including damage state and presets.
- **I11.** Each consequence counts once:
  - toggles in the attacker's game;
  - harm to a Link in that Link's game;
  - harm to enemies and objects in the runner's world, forwarded to the enemy's owner;
  - charges in the activator's game.
- **I12.** One owner path per actor: a registry actor is invisible to every other co-op path from claim to deletion (H1-H4).
- **I13.** T3 owns contraption state. There are two documented writers outside it:
  - touch hooks set per-cid atomic "touched here" words;
  - T2 writes transfer proposals into a single-consumer slot.
  Snapshots are read with a generation check (13).
- **I14.** No measurement uses instrumentation that changes what it measures (A1-A5).
- **I15.** A disappearance is never a death. Only a death request recorded by the death hooks (with its kind) or an edge removal recorded by the structure hooks becomes a structural op. Anything else is an unload (10.3).
- **I16.** Stand-ins never collide with registry parts, in any world. Remote players act on builds only through their own game (claims, forwarded pushes, hits and impulses) and through the explicit weight force (9).

## 2. Roles and physics representation
| Role (per game and cid) | Who writes bodies | Kill, lapse, deaths | Structure | Device physical outputs | On/off | Charges | Spawns |
|---|---|---|---|---|---|---|---|
| RUNNER | the game | native | native, committed as versions (6.4) | native | decides | activator only | native |
| PENDING (toucher before GRANT) | the game (native touch); drive off | guarded; the toucher's own deletes queued | edits queued | native | native | activator only | native for its spawns (8.5) |
| HANDING_OVER | sync drive on the SPEC stream | guarded | none | gated | mirrored | activator only | allow-list |
| FOLLOWER (TRACK, REST, HOLD, FREE, SNAP) | sync drive only (G1) | guarded | versions only; own glue as edit requests | gated except G1's passed list | mirrored | activator only | allow-list |
| ASSEMBLING | recipe, then the pure rigid field | guarded from the claim | recipe | gated | applied after completion | none | allow-list |
| ABSENT / DORMANT | none | - | - | - | - | - | - |

Why every body stays dynamic:
- Impulses, the stick force and thrust act only on type 2 (B1); a keyframed runner cannot be moved (F4).
- Type 1 parts froze or coasted and their groups split about 2 s later (B4, C2). Non-dynamic bodies drop out of the unglue fill, the pivot calc and impulses (C6).
- A velocity-driven dynamic plank carried the Link (B7). Every working hand-over used dynamic copies (F1).
- Keyframed (type 1) appears only as X2 regime (d) and as RK3.

Bodies and motion-type read-back:
- **Bodies.** The main body is [[[actor+0x228]+0x50]+0x20]+0x150 (main+0x2b08920). Others come through main+0x44bba4 and the controller list +0x110/+0x118, dumped per kind in X1. The rigid core is body slot 0 plus every slot a bond record names (E2). Internal bodies (wheel, spring head, cart grip, construct neck, pot spring) move with their part on a SNAP and are not written by TRACK.
- **Applied motion type** (GQ1, B2). The pending type is [body+0x60]+0x80 when [body+0x60]+0xd4 bit 2 is set. Otherwise body+0x68 bit 6 means 0, bit 7 means 1, and neither means 2. Cross-check with hknpBody+0x54.

## 3. Follower drive
**3.1 Samples.** STATE (12.4) carries the runner's root:
- COM position (hknpMotion+0x10 via hknpBody+0x80, AS5), orientation, linear and angular velocity;
- measured acceleration a_r: a finite difference of its own root velocity over 3 frames, clamped to 2 g;
- the "supported" flag: floor contact by main+0x1c04dd8, or every core body inactive (hknpBody+0x54 ACTIVE clear);
- while its action is an Ultrahand hold, the held uid and the hold target (part+0x1c and +0x1f0) [v];
- world time, send wall time and the T3 frame counter.

**3.2 Clock and horizon.**
- Clock offset = the minimum of (arrival - send) over 10 s. Sample age = now - (send + offset). The reliable layer's RTT is not used.
- HB (T2, 30 Hz) carries the runner's current world time and T3 counter. The horizon is tau = clamp(W_hb - W_sample + min(HB age, 1/30 s), 0, 0.25 s).
- So extrapolation never runs more than one HB period past runner time that HB has confirmed. A runner whose world clock stops (menu, load, hitch) freezes the follower's target within about 33-66 ms.

**3.3 Prediction.**
- **Hold target present.** Step from (p_r, q_r) toward the target with the game's own limits, 3 m/s and 270 deg/s [v], over tau. The target is mapped to the root through the follower's rigid layout. This removes overshoot at the 45-degree steps (AS34).
- **Otherwise.** p* = p_r + v_r tau + 0.5 a_r tau^2. q* = exp(w_r tau_w) q_r, where tau_w = tau only if w changed by less than 20% over the last 3 samples, else 0. Never extrapolate past a reported target.

**3.4 Errors.** e_p = p* - c_root. e_r = the full log map of R* R_f^T, valid past 90 degrees (F2). The sign convention is pinned in X1 (AS4).

**3.5 Modes**, checked in this order every frame:
1. **OFF**: RUNNER, PENDING, or this cid's "touched here" word is set (5.3).
2. **ASSEMBLING**: the pure rigid field, V = v*, W = w*, with no correction terms, until completion (7).
3. **FREE**: the follower's AABB (main+0x1c04b8c) overlaps, within 0.2 m, the AABB of a build this game runs.
   - It writes nothing while overlapping and for RTT + 0.2 s after, so the contact resolves once per world against true masses. It then blends back over 0.3 s.
   - Without this, a follower whose velocity is rewritten every frame acts as a moving wall. (Answers the build-collision attack.)
4. **HOLD**:
   - The runner's HB says paused or loading: keep the target where tau froze, with gravity compensation. Indefinite.
   - Samples are lost while HB says running (no STATE for max(3 announced periods, 0.3 s)):
     - continue the last v and a for up to 1 s;
     - then decelerate to rest at 3 m/s^2 with a local rider (10 m/s^2 without);
     - then hold the pose.
   - HOLD entered from REST writes nothing. HOLD never deletes, transfers or remakes (F5).
5. **SNAP** for one frame, when allowed (3.7), at error > 1.5 m + 0.15 s x |v_r| (or > 45 degrees) for 0.4 s, or after creation when error > 0.3 m.
6. **REST** writes nothing, so bodies may sleep.
   - Entered when the runner is supported and has rested 0.5 s (|v_r| < 0.05 m/s, |w_r| < 0.05 rad/s), the follower itself has floor contact, |e_p| < 3 cm and the angle < 1.5 degrees.
   - Left when the runner moves, |e_p| > 6 cm or the angle > 3 degrees.
   - A build at rest in the air (held, hovering, balloon) stays in TRACK.
7. **TRACK** otherwise.

**3.6 TRACK law** (phase 0, once per frame).
- V = v* + g_c + clamp(Kp e_p + I, dVmax). W = w* + clamp(Kr e_r, dWmax).
- For every core body j: v_j = V + W x (c_j - c_root) and w_j = W, using the follower's own current COMs, so the field is rigid over its own layout (C1).
- **Gravity compensation g_c** comes from a vertical acceleration observer. It is the mass-weighted mean over core bodies of (velocity after the step - velocity written), vertical part, filtered over 5 frames and clamped to between -g x (largest gravity scale in the build) and 0. Then g_c = -a_obs dt.
  - In free flight this gives about +g dt. In water, on the ground or under a gravity-cancelling preset it gives about 0.
  - It handles per-preset gravity, buoyancy and two physics steps per frame (AS3).
- **I** = clamp(I + Ki e_p dt, Imax). It models the local environment, so it is kept across epoch changes and snaps, decays at 1/s in REST, and is reset only when the copy is (re)made.
- **Constants.**

  | Constant | Value |
  |---|---|
  | Kp | 5/s up to 5 m/s, falling to 2.5/s at 15 m/s; x0.6 with a local rider |
  | Kr | 5/s up to 2 rad/s, falling to 3/s at 6 rad/s; x0.6 with a rider |
  | Ki, Imax | 1.5/s^2, 0.8 m/s |
  | dVmax, dWmax | 3 m/s and 2.5 rad/s beyond the runner's motion (2 m/s and 1.5 rad/s with a rider) |
  | STATE rate | 30 Hz moving, 4 Hz resting |

  X2 tunes all of them.
- AttachGround reads the pending velocity (R/physics body_point_velocity). X1 logs what a Link standing on a follower reads. If g_c shows in jumps, X2's force variant replaces it: AddForceAtPoint m_j a at each COM, under an intent mark.

**3.7 SNAP.**
- T = (p*, q*) composed with the inverse of the follower's current root pose, applied to every body of every part with SetTransform in one flush, plus TRACK velocities with zero error. The flush applies transform, then type, then velocity, and SetTransform keeps velocity and touches no constraint (GQ1, GQ5).
- Allowed only when all hold:
  - no local touch and no rider: no local Link whose AttachGround support is any body of any registry part, core or internal;
  - the game's placement check main+0x1c0a43c at the target reports clear (AS35);
  - the co-op's actor scan finds no actor (Link, stand-in, horse, NPC, enemy, loose object) within the build's current AABB + 0.3 m;
  - at least 1 s since the last snap, and the copy is complete.
- Otherwise TRACK continues, and velocity carries cargo and riders.
- **With a rider, error > 6 m:** the caps rise to 10 m/s and 6 rad/s. A rider-inclusive snap (the Link moved by the same transform in the same flush) is an X2 variant, adopted only if it passes T9.

**3.8 Wake.** Bits 44 and 45 are never set [v].
- Leaving REST writes a velocity that differs from the cached one by more than 1e-4, which queues the request, and the flush activates the body (GQ4). A target equal to the cached velocity means the body already has it.
- X1 confirms that a sleeping follower wakes, and logs any bit 44/45 the game itself sets on parts.

**3.9 Why it cannot tear a build apart.**
- The field is rigid over the follower's own layout, never the runner's per-part poses (F3).
- A snap moves every body in one flush.
- No body has infinite mass, here or in any stand-in (I16).
- One version per world (I4), and guarded followers cannot self-destruct.
- Corrections stay within the hold's own speed cap.

**3.10 Thread and cost.** T3 (GameDataMgr::calc, phase 0), dt from the world clock; frames with no world advance are skipped (F6). That is two setter calls per core body. If X1 shows the setters misbehave at phase 0 (AS6), the drive moves to a hook at the flush entry main+0x182410 on the physics thread. No guest physics call is ever made from T2.

## 4. Gates: "not simulated here"
**Shared machinery.**
- Every gate reads the snapshot T3 publishes (13): body to (cid, uid, role) and actor to (cid, uid, role, activator here), uid-checked at actor+0x10 (IM R6), plus comp #81 pointers to actors.
- T3's synchronous guest calls carry a thread-local sync flag.
- **Intent marks** are for anything that completes later or on another thread.
  - Before T3 posts or calls such work, it records {actor uid, operation, operand, expiry}. Operations are edge break (peer body), death (kind), delete, event (id), spawn (name list) and edge make (peer body).
  - A gate on any thread lets a matching call through and consumes the mark. Death marks also cover the death component's follow-up delete for 5 s.
  - coop_glue's per-actor "parting" window is the working precedent [v].
- **"Touched here" words.** A small array of atomic words, indexed by registry slot. Touch hooks set them in the touch frame (5.3), and G1, G2 and G6 treat a set word as PENDING.
- Every drop is counted by (function, LR chain: x30 plus three x29 frames).

**G1 body writes** (FOLLOWER, HANDING_OVER; log-only during ASSEMBLING until AS18).
- **Dropped:**
  - velocity setters 0x567750, 0x25a1f0 and 0x40e490;
  - impulses 0x44be18, 0x29ae114 and 0x29ae7a8;
  - force writes 0x29ae7fc and 0x29a4afc;
  - SetTransform 0x25a450, and ForceSetMatrix 0x45ce70 on registry actors;
  - the rocket's gravity-cancel and thrust function 0x24353c4, skipped whole.
- **Passed on purpose:**
  - state requests: motion type 0xae4300, preset 0x29adf9c and freeze. These are the game's own state (I2); dropping them left one-shot states such as UltraHandLowGravity on followers into promotion (AS9);
  - the spring's two launch calls (0x221a9bc head, 0x221aa10 base) [v], so a local Link on a follower spring is launched natively (AS36). X5 decides whether the base's reaction call passes.
- **Captured and forwarded:**
  - drops whose LR chain is a hit reaction or explosion caused by an attacker owned here (classified in X11) become IMPULSE{cid, uid, point and vector in the part frame, nonce}, applied natively by the runner with 0x29ae114;
  - the local Link's PushObject impulses (LR chain through 0x1f591b8) become PUSH.
- **comp #81 requests** (0x1b8add8, 0x1b8ad38, 0x1b8ac9c, 0x1b8aea8, 0x1b8af20, 0x1b8af98, 0x1b8a9e0, 0x1b8b290, 0x1b8b234) on follower actors are dropped by default, because the research disagrees on whether they are forces [v]. X5 ungates each slot it proves cosmetic; the visual inputs travel as TLVs (8.4).

**G2 motors** (FOLLOWER, HANDING_OVER).
- The inert predicate is forced only at verified call sites [v]: 0x1c02914 from 0x70022fc918 (wheel) and 0x1c02990 from 0x7002458428 (sled). It is never forced wholesale, because 0x1c02990 also gates the beam and the light [v].
- X5 adds the cart (0x2251440), LargeWheel (18), the merged wheel (0x236d2e8) and the homing cart.
- Where no predicate exists, the constraint is disabled at demotion and re-enabled at promotion the way the wheel's disabled branch does it (0x5f9dd8(c,0,0), 0xa14340) [v], instead of freezing a stale motor target.

**G3 kill and lapse guards** (FOLLOWER, PENDING, HANDING_OVER, ASSEMBLING).
- **Primary:** set the bit each function reads, at that function's entry:
  - f_7000271190: SPR+0x27c bit 0;
  - f_70002f8b70: SPR+0x20 bit 57;
  - f_700046fe60: SPR+0x27c bit 0 when SPR+0x2b0 > 0.
- **Backstop:** refuse 0x1953184 from 0x7000271f00 or 0x70002f8dcc, and 0x63b6b0 from 0x7000272178 or 0x70002f9264 [v]. A backstop refusal is logged; clean runs log 0.
- Lifted only through I10 (D1-D3, GQ2, GQ3; AS7, AS8).

**G4 deaths and deletes** (FOLLOWER, PENDING, HANDING_OVER, ASSEMBLING).
- Deaths (0x1953184, 0x1b4b738, every kind: fire, lava, time bomb events 8 and 6) without a mark are refused.
- In PENDING the toucher's own deaths and deletes (a Fuse taking a part) are queued and re-posted after GRANT, or discarded on REFUSE.
- Deletes (0x63b6b0) are allowed except at the G3 fallback LRs and the game-rule delete LRs X4 and X15 name. Every allowed delete of a registry actor is logged with its LR chain and classified as an unload (I15, 10.3), so engine unloads and cap evictions never leave zombie actors (AS32).

**G5 structure** (FOLLOWER, HANDING_OVER).
- Drop unmarked calls of removeScotch 0x1c042bc, the 0x2051000a handler 0x1c0aefc and the single-edge break 0x1c0bb54 (the coop_glue veto hook).
- Drop BondMaker 0x1916830 on follower bodies, except:
  - marked edge makes;
  - the reader 0x46fe60 during a marked sync bond;
  - the local player's own glue tick (LR in 0x1d67060, ScotchEntry made by this game's Ultrahand). That glue completes natively and becomes an edit request (6.7).
- removeEdge (0x2995024, 0x2995068, 0x2999fe8) is logged with LR chains. The remover X3 names is dropped on followers unless marked.

**G6 switching** (FOLLOWER, HANDING_OVER).
- 0x32eb90 sends of 0x6b510000, 0x6b510001 and 0x6b510003 to follower groups are dropped; 0x6b510002 (the bonus rate) passes [v].
- The stick's one-shot 0x2578820 passes when the stick's rider (comp #129 read by 0x42cbdc's rules) is the local Link and a TAKE would be granted (runner strength below 3). Otherwise it is held and the new runner switches the build on at promotion (5.6).
- At 0x4a3500, +0x8d bit 3 on follower parts is cleared and forwarded only after a marked local hit (8.2).
- 0x1917ac0's sends (0x6b510004 to 0x6b51000a, 0x8e5000x) to follower parts are dropped; X5 ungates cosmetic ones, and values come from TLVs.

**G7 energy** (every role).
- consume 0x1fa4ca8 and consumeRate 0x1fa4ed4 at node LRs 0x9fb71c, 0x9fa068 and 0x9fb764 pass only in the activator's game.
- 0x1fa4bfc answers "not blocked" inside 0x32eb90 or the on/off nodes for a cid this player does not pay for (AS26).

**G8 spawns** (FOLLOWER, HANDING_OVER).
- requestCreateActor 0x599670 is refused while a registry part's update or node runs (current actor from the phase hooks 0x41bb20, 0x245030, 0x2f9910, 0x194fd7c), unless:
  - the name is allow-listed (Chemical_WindCapsule, BeamosBeam, UltraHand_Connection_Effect2, effect internals X12 finds); or
  - a spawn mark matches (a replayed blast).
- Refused requests are released (0x2ae4d2c, then 0x5557e0).

**G9 hits, harm, status** (every role): the contacts_filter extension (8.6).

## 5. Authority and hand-over
**5.1 Claim strength** (computed each frame; STATE carries the runner's strength and action).
- **4, stick:** the local Link rides a stick (comp #129 by 0x42cbdc's rules).
- **3, deliberate:**
  - an Ultrahand hold on a part (0x1c0b83c, SPR+0x20 bit 16; L1);
  - a Recall or Fuse accepted on a part (X16 hooks; until then a G1 drop whose LR chain lies in that ability's code);
  - Autobuild pulling in a registry part (0x1c070c0).
  - Glue start is no longer a claim (6.7).
- **2, contact:**
  - standing on any body of a part for 0.3 s;
  - pushing it for 0.3 s (PushObject target or AABB contact);
  - aiming Ultrahand at an idle follower for 0.15 s (0x20510002 at 0x1c0bf20, prefetch).
- **1:** proximity (5.8). **0:** none.

**5.2 Order.**
- Authority is (epoch e, holder h), where h is an incarnation (10.6).
- Every game adopts the highest (e, h) seen only from GRANT, COMMIT, RECLAIM, OFFER_ACK and non-SPEC STATE. SPEC is a drive source for the granting runner, never an adoption.
- Only the holder issues e+1. A missing COMMIT yields RECLAIM e+2. An election claims e+1.

**5.3 Pull transfer** (make-before-break).
1. **Touch frame.** The touch hooks set the cid's "touched here" word: grab accept 0x1c0b83c, stick rising edge 0x221913c, Recall and Fuse accept (X16).
   - G1, G2 and G6 open for this game's actors at once, so the native hold, steering, devices and the stick's switch-on act in that frame.
   - Next frame T3 runs local-touch transitions before the drive (13): PENDING, the drive OFF.
   - The toucher sends TAKE{cid, uid, e_seen, strength, action, version_held, t_req, nonce, what it refers to (held actor uid, ScotchEntry, stick)}, resent while touching, and streams STATE flagged SPEC.
2. **The runner decides at phase 0 (5.5).**
   - GRANT{e+1, T, version, final STATE, blobs (8.4), per-part life and burn, per-body presets, activator, applied TOGGLE nonces, nonce echo}.
   - The runner becomes HANDING_OVER: guards on, outputs gated, drive on the SPEC stream.
   - If SPEC is missing for 0.25 s, it sends RECLAIM{e+2} at once and resumes native simulation (its integrity held a moment ago).
   - TAKEs older than t_req + 1.5 s (by the clock offset) are refused as stale.
3. **On GRANT the toucher:**
   - applies any newer version;
   - writes the runner's presets (except on bodies its own touch controls), life, burn state and device blobs;
   - runs the integrity check (5.4).
   - Pass: guards off, DeviceSwitch requests back to {3, 8}, RUNNER at e+1, COMMIT.
   - Fail: ABORT{integrity}, native release (5.6), FOLLOWER, repair (11).
4. **The old runner** becomes FOLLOWER on COMMIT or on non-SPEC STATE at e+1. On ABORT it sends RECLAIM{e+2}.
5. **Third games** switch source at GRANT or at the first STATE at e+1, blending over 0.3 s.
6. **Timeouts** (wall clock):
   - no GRANT or REFUSE within 1.5 s: rollback (5.6);
   - no COMMIT within 1.5 s: RECLAIM e+2.

**5.4 Integrity check (I10).** It passes when, on two consecutive frames with valid reads:
- every uid has a live actor (state 4, uid unchanged);
- every part's SPR+0x28 is one non-null group whose +0x58 equals n (GQ3);
- no body has flag 44 or 45 set;
- every body's preset equals the runner's, except bodies the local touch controls;
- no part is damaged here: lapse records +0x1b5 bit 4 clear, life not below the runner's, burn state equal (AS33).

**Repairs before guards come off:**
- **Missing uids:** made by a small Autobuild copy and bonded in place (PART_ADD, 6.6). This is allowed under a rider.
- **Split groups:** re-bonded by the queue path.
- **Damaged parts** keep their guard and are replaced by PART_ADD plus deletion of the damaged actor.

Still failing after 1 s: ABORT. A failure never requires a full REMAKE under a rider.

**5.5 Decision rule.**
- **Grant** when the requester's strength is above the runner's.
- **Strength 1 against 1:** the proximity rule decides.
- **Ties of 2 or more:** the incumbent keeps it. A deliberate touch in progress (3 or 4) is never pre-empted.
- **Several TAKEs:** highest strength, then earliest t_req, then lowest incarnation.
- **REFUSE{busy}** only while assembling, during another transfer, or when the requester lacks the version of this cid.
- **Detached parts:** the touched uid is resolved through each version's uid forwarding table, so a TAKE on a part detached since version_held reaches the cid that now holds it.

**5.6 Refusals and waiting.**
- **Grab on a build whose runner has strength 3 or more, or that is assembling:** the 0x9fb7fc hook answers no for flag 0x35 with LR in 0x1c0bf20's grab path, and the SPR replies 0x20510006 itself [v] (AS24).
- **Stick mounted while another player holds or steers:** the rider stays seated on an inert stick (G1 drops its force, G6 holds its one-shot), and its TAKE is resent until the incumbent lets go. Then the grant arrives and the new runner switches the build on through requests, as the mount would have.
- **Lost race or late REFUSE:** the local action ends natively only if it still matches the TAKE's stored reference.
  - Ultrahand: part_end 0xb9b3a4.
  - A glue entry: 0x1d61758.
- **A glue that completes while the gluer is not runner** is an edit request (6.7), never refused for authority.

**5.7 Push transfer (OFFER).** OFFER{e+1, T, cause, snapshot} is used for proximity, leaving, pause and unload.
- The receiver accepts when interested and not assembling (integrity, RUNNER, OFFER_ACK).
- The offerer becomes FOLLOWER on OFFER_ACK or STATE at e+1.
- No answer in 3 s: RECLAIM{e+2}, never a silent void.
- **Proximity:**
  - the build untouched for 3 s;
  - a candidate within 40 m for 2 s;
  - the runner's Link beyond max(60 m, twice the candidate's distance);
  - at most once per 10 s per cid.

**5.8 Paused runner.** T2 marks HB paused when T3 has not run for 0.3 s while T2 runs (AS27).
- After 0.5 s paused, T2 OFFERs every build this game runs to the best interested game with a live follower, from T3's last snapshot. TAKEs are answered the same way.
- T2 writes the new (e, h) into a single-consumer slot that T3 reads first on resume, and T3 discards TAKEs with an older e_seen (I13).
- No interested game: the build stays (menus) or is relinquished (unload, 10.3).

**5.9 Election.**
- Only on relay LEAVE or BYE for the holder, or on 5 s without HB from it while other peers are heard. If every peer is silent, this game suspects its own link and does nothing.
- Candidates are games whose CENSUS lists a live version. Rank: highest (epoch, v) with matching crc, then touching, riding, distance, lowest incarnation.
- The best sends RECLAIM{e+1, self, elected} after 300 ms and keeps the last on/off state and activator.

**5.10 Partition and rejoin.** See 10.7.

## 6. Identity, versions, structure
**6.1 Identity.**
- **Incarnation:** a random 64-bit value drawn at each join and at each save load. CTR headers carry it, and a peer table maps incarnation to relay id. Holders, activators, minters, ranks and ride info name incarnations; the relay id is only a route (relay ids are reused [v]).
- **cid** = {incarnation u64, serial u32, kind u8 (0 build, 1 single entity, 2 map-native)}. cid32 = FNV-1a of the cid, used only in NetState ride_id; a ride id that matches two live cids is ignored and logged.
- **Map-native builds:** kind 2, a hash of the sorted placement identities of their members (AS14). Their uids are derived from those identities.
- **uid** (u8): never reused within a cid; at 255 a split re-mints.
- **Lineage check:** a cid's first VERSION in a game is compared with any local build of that cid (minter, creation time, part names). On a mismatch the local build is re-minted instead of demoted.

**6.2 Registry** (T3, heap, keyed by cid).
- **Per cid:** epoch, holder, role, version, crc, member order, stream ring, drive state, device state, activator and activation serial, timers.
- **Per part:** actor, actor uid, bodies, core mask, comp #166, SPR, comp #81 pointer, flags (green, autobuilt, existing, map, multi-body), kind, origin (per-object id, placement identity, or previous (cid, uid)).
- **Registration at the runner:** its own Autobuild confirm, its first lasting bond (BondMaker with LR in 0x1d67060 or in 0x46fe60 after its own confirm), or a device or spawn it makes.
- **Registration at a load or join:** groups already in the world register as **candidates**. They become runners only if no live entry for the cid arrives within 1.5 s (CENSUS, HELLO_C answers). A live entry always wins over a fresh registration, and the candidate's game re-adopts its own instances as a follower.
- **Never registered:** world or enemy builds (bonds not made by this game's player; CM A14). NPCs are registered like map objects (6.6) but quest effects stay per world.

**6.3 Records.**
- The history writer main+0x1913694 runs at phase 0 with identity alignment into a private buffer (E2).
- A record is attached to a version when each resulting piece is settled: each piece is one group with +0x58 equal to its own count, read twice, with valid reads.
- Each record part maps to a uid by its main-body matrix within 1 mm in the same call (AS11).
- **Extension:** per part (uid, flags, origin, kind, body count, core mask, preset per body); the root uid; the capture pose and time; bonds beyond 21.

**6.4 Structural events at the runner** (event-driven; I15).
- **REMOVE_DIED(uid, kind):** committed at once from the death hook (0x1953184 or 0x1b4b738 on a registry actor, with its kind). A live uid is never classified as dead.
- **DETACH and SPLIT:** from edge-break and removeScotch hooks. The resulting pieces are read from groups once the graph job has run; the commit waits at most 0.5 s for per-piece settling, and the record snapshot may follow later.
- **ADD and MERGE:** from BondMaker with its LR, and from accepted edit requests (6.7).
- **Deletes without a recorded death, and state leaving 4 without one,** are unloads: never ops (10.3).
- A 1 Hz scan of the group table [[main+0x395f6f8]] catches anything missed and logs "uncaptured". A re-slot with the same uids and bonds produces no version (C4).
- **Piece and merge rules:** the largest piece keeps the cid (tie: lowest uid), and each version carries a uid forwarding table (uid to new cid). In a merge the larger build survives, and the other gets RETIRE{merged into}.

**6.5 Versions.**
- VERSION v is a full snapshot (record, extension, crc32) plus ops from v-1. v continues across epochs: a new runner commits max seen + 1.
- Followers order by (epoch, v). Every STATE carries (epoch, v, crc). A follower whose (v, crc) differs in either direction sends PULL.
- **Delivery by interest:** a runner unicasts the current VERSION to any peer whose NetState position comes within 130 m and whose CENSUS lacks (cid, v). A game with a directory entry within 120 m and no complete version sends PULL{cid, 0}. Retired versions stay cached.

**6.6 Follower apply** (T3, never while assembling; I4 throughout).
- **REMOVE_DIED:** 0x1953184(actor, kind, 0) under a death mark, so the same death plays everywhere (D1, GQ2).
- **DETACH and SPLIT:** native single-edge breaks (0x1cf54d4, then 0x2051000c, then 0x1c0bb54) under edge-break marks naming the peer body; the actors are relabelled; nothing is made or deleted (AS22).
- **ADD and MERGE:**
  1. Re-parent the local actor the origin names: a per-object copy handed over by coop_players.c, a map or NPC instance by placement identity, or an absorbed follower.
  2. Move it rigidly into its record pose.
  3. Queue the bond (BondEdgeDesc from 0x1d3ccf0, pushed with 0x1c07604) under an edge-make mark; G3 keeps the reader's gate open (AS21).
  - Missing actors come from a small Autobuild copy (PART_ADD). A state-2 pool entry's confirm sends 0x6b510001 (OFF); that send is dropped under a mark, or the device bits are re-applied in the same T3 pass.
  - Fallback: REMAKE.
- **REMAKE:**
  1. Delete every local part of the cid under delete marks. Map and NPC instances are never deleted; they are re-adopted.
  2. Wait until the old bodies have left the world (phive body+0x70 = 0 or hknpBody+0x88 bit 0 clear, GQ1), at most 3 s, then park leftovers 2 km below the map.
  3. Make the new copy (7).
  - Under a rider, REMAKE is deferred. PART_ADD and re-bonding are never deferred.
- **Afterwards:** green only where the runner's part is green (D5); presets mirrored; devices re-asserted.

**6.7 Edit requests** (glue and edits made outside the runner).
- A glue the local player completes on a follower (G5 lets the local glue tick's BondMaker through) is sent as EDIT_REQ{base (epoch, v), ADD or MERGE, the new part's origin and relative pose, the edge frames}.
- The runner applies it in place with 6.6's ADD path, even while its own player holds the build, and commits.
- A rejection (missing target, part limits, conflict) makes the gluer's game break its new edge under a mark, and the part falls off.
- Other edits completed before a grant (a Fuse that took a part, a shake in a race) use the same path.

**6.8 One-part entities** (kind 1).
- Covers loose devices, detached parts and runner-made persistent spawns (8.5).
- Run by the game that made, threw or detached them. Followers create them by name (the Autobuild refuses 0 bonds) and drive them with the same law; every gate applies.
- An actor that was ever a part stays registered until it dies or leaves interest (I12).

## 7. Copy creation
1. **Preconditions.** The build is in interest (10.1), its version is complete, and no other creation runs here. The player's design selection is deferred for the duration. I4 holds.
2. **BuildObject.** Use one that does not depend on the player's progress (AS17, decided by X6 on an early save before P2), in this order:
   - (a) the ability object's +0x51f0 or +0x51f8, if they exist without the ability unlocked;
   - (b) a BuildObject made with the game's own constructor;
   - (c) the part maker's create argument and the confirm's per-part marks and bond queueing, reproduced at phase 0 (ER part maker and confirm).
   - Whatever object is used, save and restore the fields the recipe touches: +0x120, +0xc50, [o+0x3c50], the pool +0x4d10..+0x4f58, and +0x3c70.
   - If it is the player's own object, the design-selection lock works both ways and a slipped selection aborts and retries after 1, 2, 4 s (never terminal).
3. **Make** at phase 0: record into +0x3c50..+0xc50; base +0x120 = the predicted pose at make time; refill the pool (0x1d3cfc8) and run the part maker 0x1d3b470.
4. **Wait** for every pool handle (+0x60), at most 6 s. Then release and back off 1, 2, 4 s, then 30 s after 3 failures; failed creates are released.
5. **Refresh** 0.03 s before the confirm. If the prediction moved more than 0.3 m or 3 degrees since the make, move the held parts by one rigid SetTransform to a fresh prediction and set +0x120 to match, so the queued bond frames agree (AS15). Claim the parts and register them as ASSEMBLING (G3, G4, G8 on).
6. **Confirm** 0x1d3c0d4 at phase 0 with entries at state 3, cost records zeroed, +0x3c70 = 0, and currency removal 0x209b754 blocked for this call (AS16).
7. **Mark:**
   - SPR+0x27c bit 0;
   - autobuilt bit 28 where the runner's part has it; bit 51 cleared;
   - actor+0x5ac bit 2 cleared;
   - green only where the runner's part is green (D5), at the confirm once X4 passes, else once grouped.
8. **Until complete**, the pure rigid feed-forward field moves the parts (3.5 item 2). Bonds lock the layout they form in (CS:347-348).
9. **Complete** when every part's SPR+0x28 is one group with +0x58 = n, read twice (GQ3). Not complete in 2 s: re-bond by the queue path, then REMAKE (bounded, 11).
10. **Finish:** restore the BuildObject, apply the on-mask through requests (only the spring keeps its saved state, ER), mirror presets, FOLLOWER; the creation SNAP is allowed.
11. **Copy made around the local Link:** the copy is made at its true pose and made to ignore the local Link with the per-pair ignore of I16 (AS30) until the Link has been clear of its AABB for 0.5 s. Fallback: make it one build-height up and hold that target until the Link is clear, then lower it.

## 8. Devices, spawns, harm, energy
**8.1 On/off follow.** Applied only when STATE's (v, crc) equals the local version; otherwise device follow and health wait for the version.
- For each switchable follower member whose group state +0x9e8 differs from the runner's bits, call the request function 0x1c08774 with {1 or 2, priority 0}; the lowest priority wins (ER DeviceSwitch).
- At promotion the requests go back to {3, 8} (AS12).

**8.2 Local toggles.**
- A hit on a follower part by an attacker owned here is marked for 2 frames (8.6).
- The 0x4a3500 hook clears +0x8d bit 3 on follower parts. For a marked part it sends TOGGLE{cid, uid, from, want, nonce} until STATE echoes the nonce.
- The runner applies it as "set to want" (idempotent), records the sender as activator, and passes applied nonces in GRANT.

**8.3 Per device kind on followers** (the runner is always native).
| Device | Follower behaviour |
|---|---|
| fan | impulses dropped (G1); blades, glow and wind capsule run |
| rocket | 0x24353c4 skipped; tumble 0x2435480 and kick 0x25536d8 dropped; flame from IsOn; RocketFireEndDie refused (G4); burn TLV; the runner's end arrives as REMOVE_DIED |
| small wheel | gate at 0x70022fc918 [v]; rolls passively; airborne spin written to node+0x2c, which the disabled branch republishes [v] (X5 screenshot) |
| LargeWheel, merged wheel, cart | call-site gate if X5 finds one, else constraints disabled at demotion (G2) |
| sled | gate at 0x7002458428 [v] |
| balloon, wing, stabilizer, hover stone | own-body writes dropped; comp #81 per G1; swell, flap, tilt, servo target as TLVs; hover target also in the GRANT |
| spring | launch calls pass (G1); slider runs locally from the mirrored on-edge; extension TLV; X5 counts follower launches against the runner's EVENT SPRING_LAUNCH. Fallback: the launch passes only with a token set by that EVENT |
| stake | landing write dropped; ground link gated at its call site or by the constraint disable (X5); stuck-state TLV |
| cannon, frost | spawns per 8.5 |
| time bomb | countdown TLV; detonation per 8.5 |
| beam, flame, shock, light, mirror, pot, hydrant | run for visuals; harm per 8.6 |
| homing cart | impulse 0x29ae114 at 0x225cbcc dropped (G1) [v]; target from TLV |
| construct head | forces dropped; 0x1917ac0 look sends dropped (G6); look target TLV |
| stick | AddForceAtPoint dropped; tilt TLV |
| battery | bit 28 native everywhere; life from the runner's TLV; the follower's drain writer gated once X13 finds it |
| others (27) | generic gates; per-kind residual logged in X5 |

**8.4 Internal state**, as TLVs in STATE on change, in BLOB at 1 Hz, and in the GRANT:
- spring position; wheel omega; rocket burn and node state; time-bomb countdown node+0xdc; hover target node+0x20..+0x48;
- stick x/y; balloon swell; construct look target; battery life; stake state;
- per-part life and burn state; per-body presets; comp #81 visual parameters X5 identifies.

A missing field costs a visual or a snap at promotion, never a duplicate action.

**8.5 Spawns and discrete effects.**
- **Persistent or harmful spawns** (cannon balls, ice blocks, anything X12 finds) are made only by the runner's version. The runner mints each as a kind-1 entity at creation and sends EVENT{SPAWN, kind, cid, pose, velocity, t_world}.
  - Followers create it by name and drive it; every gate applies, so it cannot explode or harm on its own.
  - Its end is replayed from EVENT{DETONATE or DESPAWN} under a mark.
  - In PENDING the toucher creates its own spawns natively, as the simulator from the touch frame.
- **Time bomb:** the follower's events 8 and 6 are refused (G4). EVENT{DETONATE} replays 0x1953184(actor, 8, 0) under a death mark plus a spawn mark for its blast, so the follow-up and the blast pass (G8).
- **Rocket end, green kill, lapse, burning:** replayed by kind (6.6).

**8.6 Hits, harm and status** (contacts_filter extension, coop_world.c; I11).
| Attacker \ victim | Follower part here | Runner part here | Local Link | Stand-in | Enemy owned here | Enemy copy |
|---|---|---|---|---|---|---|
| local Link and its weapons and projectiles | dropped, marked; TOGGLE, HIT or IMPULSE to the runner | native | - | as today | as today | as today |
| stand-in | dropped | dropped (its owner's game forwards) | as today | - | as today | as today |
| enemy owned here | dropped, marked; HIT or IMPULSE | native | native | as today | native | - |
| enemy copy | dropped | dropped | as today | as today | - | - |
| part or spawn of a build run here | dropped | native | native | dropped | native | forwarded to the owner as lost life (existing path) |
| part or spawn of a follower build | dropped | dropped | native | dropped | dropped | dropped |

- Damage, ignition and elemental status (fire, ice, shock, wet) on follower parts are never applied locally. An attacker owned here forwards them as HIT{cid, uid, damage, element, from} and the runner applies them natively.
  - Hits go through contacts_filter (AS13).
  - Chemistry status needs the actor status-apply hook X11 finds (AS33).
- The runner's per-part burn and status are mirrored as TLVs.

**8.7 Energy.**
- **Activator.** The player whose action most recently produced an "on" command the runner can attribute:
  - a TOGGLE to on (its sender);
  - a stick mount (the rider, for the ride);
  - the runner's own player's hit, hold or switch.
  - It is carried in STATE with an activation serial. A DETACH or SPLIT keeps it on both pieces; a MERGE keeps the surviving build's.
- **Charges.** Every game runs its own nodes; only the activator's game lets device charges through (G7; J1, J2).
- **Switch-off on positive evidence only.** The activator's HB lists, per cid it activated: {serial, status}. Status is paying, battery (group bit 28 [v]), blocked (its 0x32eb90 OFF for that group, or mgr+0x50 bit 0 while the group is on) or not running here (paused, assembling, remaking, out of update radius).
  - The runner switches the build off, through requests on every member that is on, only on "blocked" with the current serial in two consecutive HBs.
  - Absent or late HBs change nothing. No other game's gauge can switch it off (G7).
- **Activator leaves the session** (LEAVE or BYE): activation passes to the current stick rider if any, otherwise the runner switches the build off (decision D2).
- **Activator not running the game** (decision D1). While the activator's game is loading or rebuilding its copy, the build keeps running and nobody pays, however long that takes. While it is in a menu (HB says paused, not loading), the build runs free for 60 s of menu time; after that the runner treats the activator as gone: activation passes to the stick rider if any, otherwise the build switches off. The 60 s count restarts when the activator leaves the menu.
- **Battery part.** Bit 28 stops Link charges in every game (J3). Its life runs on the runner and is written into followers from the TLV; X13 finds the drain writer to gate on followers.
- **Range.** If X13 shows CustomEnergyRange measures distance to a player, the runner evaluates it against the activator's Link (distance carried in HB).

## 9. Stand-ins and riding
- **Look path unchanged:** owner_smoothed, coop_puppet_place, put_pose and clip timing (K4).
- **Ride info** (no layout change [v]):
  - ride_owner = 0xFD for a build, ride_id = cid32, the pad byte = uid, ride_off = the Link's place in the part frame;
  - set from the AttachGround support when it is any body of a registry part;
  - every game places the stand-in's look and body target at the local part pose composed with ride_off (fixes K1).
- **No collision with builds (I16).**
  - In every world the stand-in's bodies ignore every body of every registry part through the game's per-pair ignore. The graph job's bonded-pair mask (main+0xacfa10 via 0xacf904, per-body mask +0x28) is the first candidate; then filter words and the ignore-pair manager (X14b, AS30).
  - The stand-in keeps today's body motion (type 0, placed every tick), so smoothness is untouched. It no longer shoves builds, and builds pass through it in the runner's world. A remote Link is still hit by the build in its own world, where its game decides the harm.
- **Weight.** In the runner's world, while ride info names a part of a build run here, apply AddForceAtPoint 0x29a4afc on that part at the ride point, F = m_eff g, with m_eff tuned against a real Link on a floating plank (X14, AS29).
- **Push.** The remote player's own game drops its Link's PushObject impulses on follower bodies and sends PUSH (coalesced per frame), which the runner applies with 0x29ae114. Pushing for 0.3 s is a strength-2 claim, after which the push is native.
- **Fallbacks** if no per-pair or per-body ignore exists:
  - A: a layer-level ignore of dynamic objects for stand-ins, accepted only if the existing world-sync suite passes;
  - B: park the stand-in's bodies 2 km below the map while its owner is within 10 m of a registry part, accepted only if X14c shows the body does not drive the drawn mesh.
- **Later (X14d):** drive the stand-in's own character controller for native weight and push (SI option 4).
- **Riding through a hand-over:** no motion change. The promoting game continues from its follower's motion; the old runner blends onto SPEC.

## 10. Lifetime, unload, session
**10.1 Interest.**
- A follower is made within 120 m of the build's centre and retired silently beyond 160 m, or at the engine's measured unload distance - 30 m if that is smaller (AS28).
- Never retired while ridden or touched, or while this player activated it and it is on (kept up to R_keep).
- A follower part the engine unloads marks the follower ABSENT, not damaged: no repair outside interest.

**10.2 Directory.** Every game keeps (cid, e, h, v, crc, centre, n, live bits) for every build, from STATE and CENSUS.

**10.3 Unload at the runner (I15).** Unload triggers:
- a delete of a registry actor with no recorded death request (by LR: the engine unload path X15 finds, or any unknown LR);
- members leaving state 4 with no death record;
- a T3 stall of 0.5 s or more;
- a Link jump of more than 250 m, or a world-clock restart.

On any of these, the first T3 frame runs lifetime transitions before runner duties (13). The runner hands each affected build on from the last good snapshot: OFFER to the best interested game with a complete follower, else RELINQUISH (DORMANT everywhere, kept 10 min; the first player within 120 m claims it and makes it). It never commits REMOVE_DIED or RETIRE for an unload. A partial unload hands the whole build on (X15 checks whether parts unload one by one).

**10.4 RETIRE**, the only global delete, is sent when:
- all parts died by recorded death requests;
- the build was merged; or
- a RELINQUISHed build stays unclaimed for 10 min.

**10.5 Capacity.**
- Follower limits are set from X15's measurements of the bond pool, the group table and the live-device cap, keeping at least half of each for the local player (AS38).
- BondMaker returns of 0 are counted, and the farthest followers are evicted before the reserve is reached. Never evicted: builds ridden, touched, or activated and on.
- If the engine's device cap evicts, its delete LR is allow-listed (G4) for follower parts; that follower is retired, not remade, while the count is near the cap.

**10.6 Session.**
- **coop_net changes:** it reports any change of my_id. CTR treats a change as leave plus rejoin and re-announces its incarnation (HELLO_C).
- **Session end** (coop_net inactive, which happens after 6 s of silence [v]), in every game:
  - promote every follower here locally (integrity check, guards off, requests reset);
  - abort ASSEMBLING (release creates, delete claimed parts, restore the BuildObject);
  - drop DORMANT entries;
  - make its own player the payer for whatever is on here;
  - stop CTR.
  - Roles are stored explicitly, never derived from coop_net_id().
- **Save load:** registry reset and a new incarnation; the others re-elect its builds.

**10.7 Rejoin and partitions.** On rejoin the game receives CENSUS first. For each cid both sides hold:
- if only the rejoiner touched it during the gap, the rejoiner re-claims with RECLAIM{e_max + 1};
- otherwise the session side wins (decision D5), and the rejoiner demotes and converges by version and drive (rider-inclusive snap only if X2 adopts it).
- Lost edits are logged.

**10.8 Network stall** below the relay timeout: HOLD (3.5), then blend on resume.

**10.9 Late join:** HELLO_C, then interest-based VERSION delivery (6.5), creations queued nearest first.

**10.10 Three to eight players:** per-cid runners, per-pair streams (12.3), a global (e, h) order, one creation queue per game.

## 11. Repair
- **CENSUS** at 1 Hz per game: per cid within some peer's interest, {e, h, v, crc, role, live, centre}, chunked with a sequence.
- **Follower health** at 2 Hz, on valid reads only, and only when (v, crc) matches the runner's. An anomaly counts after 3 checks over 0.5 s with nothing pending. Checks:
  - the live set against STATE's alive mask;
  - one group with +0x58 = n;
  - layout within 2 cm and 2 degrees;
  - device bits 1 s after a change;
  - type 2 on every body;
  - presets equal the runner's.
- **Repairs, in order:**
  1. re-request devices;
  2. mirror presets;
  3. re-bond by the queue path;
  4. re-adopt;
  5. PART_ADD;
  6. REMAKE (not under a rider).
  - At most 1 REMAKE per 5 s and 5 per 5 min per cid; beyond that the follower is STUCK (kept, logged, retried after 60 s).
- **Versions:** a (v, crc) mismatch either way sends PULL.
- **The runner** never repairs its own version.
- **Every repair** logs one line with its cause. Clean runs log 0.

## 12. Transport
**12.1 Channel.**
- New net kind CTR = 11 in coop_net.c and both relays. COOP_NET_VERSION 28 becomes 29, and server/coop_relay.py's hard-coded VERSION changes with it [v].
- For CTR only, both relays copy header byte 7 as the destination id (0 = everyone) and send to that peer only; today both write it as 0 [v].
- Queues of 256 each way with drop counters logged every 5 s. Payloads at most 1200 B.

**12.2 Payload header** (24 B): 'CTR1', type, flags (reliable, SPEC, channel), sender incarnation u64, msg_seq u16, ack u16, ack_bits u32. Acks name the stream (sender, receiver, channel).

**12.3 Reliability** (T2).
- **Streams.** One sequence space per (sender, receiver, channel), with two channels: control (TAKE through RECLAIM, OFFER, TOGGLE, HIT, IMPULSE, EVENT, EDIT_REQ, RETIRE, RELINQUISH) and bulk (VERSION, BLOB, PULL). Control never waits behind bulk.
- **Broadcasts.** A reliable broadcast is one unicast copy per receiver.
- **Acks.** RTO = clamp(1.5 SRTT + 4 RTTVAR, 80 ms, 1 s); ACKs carry their own delay. T2 acks a message only after storing it in a reliable inbox that never drops what it acked. When the inbox is bounded per cid and full, it does not ack, so the sender retransmits.
- **Latest-only slots.** STATE, HB and CENSUS go to "latest per (sender, cid)" slots, not FIFO entries.
- **Ordering.** A newer VERSION, GRANT or OFFER supersedes older unacknowledged ones. Handlers are idempotent by (cid, epoch, version, nonce).

**12.4 Messages.**
| Type | Name | Delivery |
|---|---|---|
| 1 | STATE: cid, e, h, strength, action, v, crc, flags (resting, supported, airborne, touching, paused, SPEC, rider), activator and serial, t_wall, t_world, T3 counter, root uid, pos, quat, v, w, a, hold target (optional), alive/on/switch masks (member order, applied only on equal versions), nonce echoes, TLVs | unreliable, 30 Hz moving, 4 Hz resting, at once on change |
| 2 | ACK | when owed |
| 3 | CENSUS | 1 Hz, chunked |
| 4 | HB: world_us, T3 counter, paused/menu/loading, activator status list | 30 Hz from T2 |
| 5 | HELLO_C | reliable |
| 10-15 | TAKE, GRANT, COMMIT, REFUSE, ABORT, RECLAIM | reliable control |
| 16-18 | OFFER, OFFER_ACK, OFFER_DENY | reliable control |
| 19 | RELINQUISH | reliable control |
| 20 | VERSION (chunks up to 1100 B, about 6.7 KB for 21 parts) | reliable bulk |
| 21 | PULL | reliable bulk |
| 22 | EDIT_REQ | reliable control |
| 23 | RETIRE | reliable control |
| 30 | TOGGLE (want) | reliable control |
| 31 | HIT (damage, element) | reliable control |
| 32 | PUSH | unreliable, coalesced |
| 33 | IMPULSE | reliable control |
| 34 | EVENT (DETONATE, SPAWN, DESPAWN, BURN_START, BURN_END, CANNON_FIRE, SPRING_LAUNCH) | reliable control |
| 35 | BLOB | 1 Hz bulk |

Budget: about 4 KB/s per moving build, 0.5 KB/s resting, 1 KB/s HB per game.

## 13. Threads and data
- **T1, net thread:** UDP and the CTR queues.
- **T2, co-op tick:** the reliable layer, inbox, HB, paused responder (5.8). It makes no guest physics calls.
- **T3, GameDataMgr::calc at phase 0 (main+0xa72b00)**, in this order:
  1. inbox drain and registry refresh;
  2. local-touch transitions (PENDING) and lifetime transitions (unload, warp, load, session end; 10.3);
  3. runner duties (commits, STATE, activator, versions);
  4. follower duties (drive, device follow, version apply, assembly, health);
  5. protocol transitions;
  6. snapshot publish and outbox.
- **T4, job hooks:** read the snapshot, the "touched here" words and intent marks, and post to single-consumer rings.
- **Snapshots** come from a fixed pool of 16 blocks that is never freed. A reader reads the generation, the data, then the generation again (seqlock) and retries on a change. No lock is held across a guest call (IM R3).

## 14. What to remove or change
**coop_autobuild.c**
- Remove:
  - the owner/runner split and key = group >> 3;
  - name-and-nearest matching and the stability gate;
  - made = 3, the 6 s give-up, the memset eviction, the 3 s copy delete and the 3 s take-back;
  - the single-packet CBAU, adopt_tick, given_away and CBOW;
  - drive_parts, parts_dynamic, kin and ANGSIGN;
  - the static hold and green-on-every-part;
  - the "shares objects" delete;
  - the 0x6b51000a drop and its 0x1c067bc registration;
  - driven[], nocharge[], made_parts[] and ours[];
  - the 2 s activator clear and the 1.5 s follow throttle;
  - MvPacket and "still mine"; the CB* kinds on HIT.
- motor_nodes are replaced by G2; force_nodes stay behind a switch until X5 passes.
- The ABLOG generic watch is off by default and never on in pair runs. Every read goes through a checked accessor.

**coop_players.c**
- Registry gates in arrow_copies, prop_fn/report_props, dwatch_fn/destroy_apply, props_receive/force_to/props_hold, objs_receive, old_items, place_copy, coop_copy_remake and ride_carry.
- Remove the WT's 3 s born_us delay.
- Stand-ins: the per-pair ignore against registry parts; owner_matrix applies the ride frame; the weight force.
- An API that hands per-object actors (and map instances) to the registry.

**coop_bonds.c:** contraption logic off when TOTK_COOP_CONTRAPTION=1; revert the WT timing changes.

**coop_glue.c:** off for bonds with a registry end; its veto hook on 0x1c0bb54 is reused by G5.

**coop_world.c:** extend contacts_filter (8.6); remove the CB* dispatch.

**coop_net.c and both relays:** the CTR kind and queues, byte-7 routing for CTR, the version bump in both relays, and my_id change reporting.

**coop_spawn.c and cpu.h:**
- Register the new hooks.
- Fix the motion-type labels (coop_spawn.c: the KFRIDE test comment and BODY_MOTION_TYPE; CS:56, 92, 99; R/physics/README.md:33-35).
- Re-read KFRIDE and STANDPUSH with the corrected numbers (B2, K3).

## 15. Experiments (decisive first)
**15.1 Harness rules.**
- **H1.** No write watch, page protection or single-stepping on pages a measurement reads (A1-A5). A watch run measures only its own cell.
- **H2.** Every read reports success; failures are counted per call site, apart from zeros.
- **H3.** Logging is rate-limited and buffered; uncapped logs only where needed.
- **H4.** Each test names its pass lines. Clean runs show 0 repairs, 0 G3 backstop refusals, 0 non-sync writes landed on followers and 0 CTR drops.
- **H5.** TOTK_COOP_TEST_* switches are read with cfg_get (project memory).
- **H6.** State repeats; report p95 and max.

**15.2 Order.**
| # | Experiment | Unblocks | Gate |
|---|---|---|---|
| 1 | X0 reads, threads, pause, load | everything | P0 |
| 2 | X6 copies without the ability (early save first) | creation for every player | P0 |
| 3 | X1 conventions, bodies, wake without bit 44, presets | drive, G1 | P0 |
| 4 | X15a unload path, caps, pools | I15 classification, 10.5 | P0 |
| 5 | X2 follower regimes | drive form and constants | P1 |
| 6 | X3 separation remover | G5, caps | P1 |
| 7 | X14b collision ignore for stand-ins and the local Link | I16, 7 step 11 | P1 |
| 8 | X4 guards, damaged promotion | G3, G4, 5.4 | P1 |
| 9 | X5 device gates | G1, G2, comp #81, spring | P1 |
| 10 | X7 bodies after delete | REMAKE | P2 |
| 11 | X10 hand-over, contention, touch frame | 5 | P3 |
| 12 | X8 capture and identity; X9 in-place edits | 6 | P4 |
| 13 | X11 follow, hits, impulses, status; X12 spawns; X13 energy | 8 | P5 |
| 14 | X14 stand-ins (weight, push, ride) | 9 | P6 |
| 15 | X15b lifetime; X16 Recall and Fuse; X17 network, session, scale | 10, 5.1 | P7 |

**15.3 The experiments.**

**X0. Reads, threads, pause, load.**
- Setup: watch off, checked reads. Rerun the k, wg_off, rg_stick, rg_tog and p1_carry pairs. Entry and exit counters with thread ids on T2 and T3. A 30 s menu and a loading screen.
- Pass: 0 failed reads on parts; the wing grouped in the same frame as the fans; no "3 of 4" and no false adoption.
- Decides: AS20, AS27, and whether T2 and T3 overlap.

**X6. Copy creation, cost, ability.**
- (a) On an early save without Autobuild: whether +0x51f0 and +0x51f8 exist and whether the recipe works through them; find the BuildObject constructor; try variant (c) of section 7.
- (b) A copy of a build moving at 3 m/s and turned 90 degrees, using the rotated base and the pre-confirm rigid refresh, also with B's Link inside the volume. Log every write to made parts before the confirm (AS18).
- (c) Item_Ore_L, capsule and history counts ([[main+0x395f6f8]]+0x20) before and after; the player opening Autobuild mid-copy.
- Pass: grouped within 0.5 s in 20 of 20 runs, on both saves; release error under 5 cm and 2 degrees; counts unchanged.
- Decides: AS15-AS18.

**X1. Conventions and bodies** (single game).
- COM (hknpMotion+0x10) against 0x29a9398; the angular sign.
- Body dumps per kind, with joint names and presets (body+0x17c and the library's GravityScale).
- Set bit 44 on a test plank, write one velocity, stop writing, and log the velocity after 1 and 2 flushes. Log physics steps per frame. Log flags 44/45 the game sets on parts during hold, ride and devices.
- A follower asleep on the ground: does the first TRACK write wake it?
- What does AttachGround read on a driven plank, compared with the post-step velocity?
- Pass: COM within 1 cm; the zeroing reproduced; wake without bit 44.
- Decides: AS4, AS5, AS6, and the rigid core per kind.

**X15a. Unload and capacity** (single game).
- Walk and fly away from a build and fast travel; log every registry actor's delete LR chain and state, and whether parts unload one by one.
- Fill the bond pool (count BondMaker returns of 0), the group table and the device count until a device vanishes; log the evicting LR.
- Decides: AS28, AS32, AS38 and the 10.5 limits.

**X2. Follower regimes** (self-mirror, 6 m aside, 100 ms delay, ±20 ms jitter, 10% loss).
- Recordings: an autobuilt Fanplane through rest, a 3 m/s carry, an 8 m/s push, a tumble, a hold with 45-degree rotation steps, held still in the air, a hover-stone platform, the Fanboat in water, 0.5 m target jumps.
- Regimes:
  - (a) the rigid field with the observer;
  - (b) HEAD's root-only drive;
  - (c) WT per-part targets;
  - (d) type 1 with the rigid field (RK3 only);
  - (e) (a) with gravity compensation as a force.
- Second pass with a Link on deck at 3, 8 and 15 m/s; the rider-inclusive snap variant; placement-check snaps with an actor in the target.
- Logs: layout error; group calls; removeEdge with LR chains; type bits; tracking; Link drift; frame time.
- Pass for (a):
  - layout under 1 cm; 0 x2 0; 0 removeEdge;
  - tracking p95 ≤ 0.10, 0.25 and 0.4 m at 3 and 8 m/s and tumbling;
  - rest and held-in-air ≤ 3 cm with no bob;
  - water ≤ 3 cm;
  - rotation-step overshoot ≤ 3 degrees;
  - Link drift ≤ 0.1, 0.3 and 0.6 m per 4 s;
  - ≤ 0.2 ms per frame.
- Decides: AS1, AS3, AS34, AS35 and the constants.

**X3. What separates bonds.** Scenarios:
- (i) two overlapping versions driven toward one trajectory;
- (ii) Ultrahand on one part with the others type 1;
- (iii) one part teleported 0.3 m;
- (iv) root-only with jumps;
- (v) the rigid field with jumps.

Hooks with LR chains on 0x2995024, 0x2995068, 0x2999fe8, 0x18ac48, 0x1c0bb54, 0x1c042bc, 0x1cf54d4 and the calc of ExecuteActorRemoveTBoxBondWhenConstraintYielded. Pass: (v) shows 0 removals over 5 min, and (i)-(iv) name the remover. Decides: AS2 and G5's remover gate.

**X14b. Collision ignore.** Test, between a stand-in's bodies and a part, and between the local Link and a part:
- the graph's pair mask (0xacfa10/0xacf904, +0x28);
- filter words +0x8c..+0xa0;
- the ignore-pair manager 0x2ae6cf4, including why Phase 0 got 0 back.

Pass: a stand-in teleported into a resting build moves it by 0; collision returns when the ignore is removed. Decides: AS30 and fallbacks A and B.

**X4. Guards.**
- A green Fanplane with one fan treated as a follower: its bonds broken natively, then 30 s; repeat with no guards as a control.
- A follower rocket burning, with and without the lapse guard.
- A part damaged and set burning while guarded, then promoted.
- Frame counters in the confirm, 0x46fe60, 0x181e40, 0x1d93978, 0x271190 and 0x2f8b70.
- Pass: the guarded fan lives 30 s with 0 backstop refusals and is re-bonded; the control dies within 2 s (kind 0x14); a damaged part is replaced before guards lift and never dies after promotion.
- Decides: AS7, AS8, and green at the confirm.

**X5. Device gates** (self-mirror).
- Builds: fan, rocket, wheel and stick cart, sled, cart, balloon, hover stone, stabilizer, spring (with the local Link on the follower's head), stake, cannon, time bomb, LargeWheel, homing cart, construct head.
- Per comp #81 slot: body acceleration with and without it.
- Pass:
  - 0 non-sync writes landed;
  - residual velocity within 1e-3 of the commanded;
  - visuals within ±1 event per 10 s;
  - the follower's spring launches the local Link within 10% of the runner's launch height, with follower and runner launch counts equal.
- Decides: AS9-AS11, AS36, the G2 site list and comp #81's ungated slots.

**X7. Bodies after delete.** Delete a copy; log body+0x70, hknpBody+0x88 and the actor state every 0.1 s; drop a plank on the spot. Decides AS23.

**X10. Hand-over and contention** (pair, then with 10% loss and 150 ms latency).
- Grab, grab-back, stick on and off, prefetch, two grabs within 50 ms, a grab while held, a stick mount while held, a dropped GRANT, a silent granter, a forced integrity failure.
- Count frames between the touch and the first native response.
- Pass: T10-T16. Decides: AS24, AS34 and the timers.

**X8. Capture and identity.**
- Autobuild; hand glue by a person; a shake-off; stick take and release (re-slot); a fast drop.
- Placement identities for map objects in both games, including a stack of identical planks.
- An unload during capture.
- Pass: one settled change per structural change, 0 on re-slots, 0 ops from unloads, equal identities.
- Decides: AS14, AS19.

**X9. In-place edits.** Queue-path bonds at relative poses; a single-edge break; PART_ADD; a glue completed on a follower followed by EDIT_REQ and the runner's apply while its player holds; a rejected edit reverted. Pass: one group within 3 frames, layout under 1 cm, no death. Decides: AS21, AS22.

**X11. Follow, hits, impulses, status.**
- 50 toggles; 50 real hits; 20 stand-in swings; a Bokoblin.
- Arrows, bombs and a fire arrow on follower parts; a beam on an enemy B owns.
- Find the actor status-apply hook.
- Pass: every toggle once; 0 flicker in 10 min; impulses applied once; each victim damaged once; fire shown on both screens.
- Decides: AS12, AS13, AS33.

**X12. Spawns.** Hook 0x599670 during cannon fire, frost, time-bomb countdown and detonation, and rocket end. Pass: one ball and one detonation per event across games, each victim hurt once. Decides AS31 and the allow-list.

**X13. Energy.**
- A watch run on the battery life cell and on mgr+0x50.
- B activates A's build with a low gauge.
- The activator at 0, 50, 100 and 150 m.
- The activator in a menu; a battery build; a multi-fan build (bonus rate); CustomEnergyRange's consumer.
- Pass: battery builds never switched off by the sync; switch-off only on a blocked gauge; drain within 10% of single-player (CS:549-551).
- Decides: AS26.

**X14. Stand-ins.**
- (a) Body dumps.
- (c) Tour walk with the body moved against unchanged: does the body drive the mesh?
- m_eff tuned against a real Link on a floating plank and raft.
- (d) Probe the controller's move vector in 0x298c9f8.
- Decides: AS29 and fallback B.

**X15b. Lifetime baselines.** Fast travel away and back; save and load; single-player behaviour on unload.

**X16. Recall and Fuse.** The fndiff profiler; target-accept points; whether the Recall recorder records follower motion and can be fed. Decides AS37.

**X17. Network and scale.** Three games; 30% CTR loss; simultaneous grabs; 10 moving builds; a 4 s stall; a 7 s link loss (session end and rejoin); relay id reassignment.

## 16. Phases and gates
| Phase | Content | Gate |
|---|---|---|
| P0 | Checked reads; watch off; CTR in coop_net and both relays; reliable layer, HB, incarnations; registry and snapshot pool; HEAD's dynamic root drive restored; X0, X6, X1, X15a | X0 passes; copies made on an early save (or a chosen fallback in place); pair tests no worse than HEAD (F1); 0 CTR drops with 3 builds |
| P1 | Self-mirror lab; drive with observer; gates and intent marks; X2, X3, X14b, X4, X5 | X2(a) meets every line; X3(v) 0 removals; X14b settles I16's mechanism; X4 and X5 pass; T1, T2 |
| P2 | Ids, versions, records, creation, drive modes, G1-G9, device follow, TOGGLE, activator; old paths off | T3-T9, T17 |
| P3 | Authority: touch words, PENDING, TAKE through RECLAIM, OFFER, integrity, prefetch, refusals, paused responder, LEAVE elections (X10) | T10-T16, T45 |
| P4 | Event-driven structure, versions by (e, v), in-place apply, PART_ADD, EDIT_REQ glue, splits, merges, entities, map and NPC adoption, repair, REMAKE (X8, X9, X7) | T18-T23, T44 |
| P5 | TLVs, spawns as entities, replays, contacts_filter, impulses, status, energy rules, battery (X11-X13) | T24-T30, T41-T43 |
| P6 | Ride info, stand-in ignore, weight, PUSH (X14) | T31-T33 |
| P7 | Interest, unload hand-off, session end, rejoin, late join, capacity, 3 players (X15b-X17) | T34-T38 |
| P8 | Soak | T39, T40 |

## 17. Tests
| Test | Scenario | Pass |
|---|---|---|
| T1 | self-mirror flying and falling | layout ≤ 2 mm; tracking ≤ 2 cm / 0.5 degree; 0 group changes; 0 non-sync writes landed |
| T2 | Link on the self-mirror at 3, 8, 15 m/s | drift ≤ 0.1, 0.3, 0.6 m per 4 s; never thrown |
| T3 | pair copies of Fanplane and Fanboat, on a full save and an early save | one group in 0.5 s, 20 of 20; within 2 cm / 1 degree; inventory and history unchanged |
| T4 | copy of a build moving at 3 m/s and turning | release error < 5 cm / 2 degrees |
| T5 | copy made around B's Link | Link not moved over 0.1 m, no damage; collision back after the Link leaves |
| T6 | follow at rest, 3 and 8 m/s, and in water | p95 ≤ 0.03, 0.10, 0.25 m; water ≤ 3 cm |
| T7 | fall and tumble from 6 m | p95 ≤ 0.4 m and 12 degrees; 0 splits |
| T8 | rest on ground; held still in air; hover platform | ground: 0 writes after 1 s; air: ≤ 3 cm with no bob |
| T9 | rider on a follower at 3, 8, 15 m/s | drift ≤ 0.1, 0.3, 0.6 m; 0 snaps |
| T10 | grab, grab-back, stick on/off at rest, moving and tumbling, 20 runs each | native response in the touch frame; two deciders for at most RTT + 50 ms; spike ≤ 0.3 m; 0 deaths; never a stop at promotion |
| T11 | prefetch | grant before the grab in 80% of 50 grabs |
| T12 | two grabs within 50 ms (2 and 3 games) | one runner; the loser let go natively within 0.3 s |
| T13 | rider through a hand-over | drift ≤ 0.2 m |
| T14 | hand-overs with 10% loss, 150 ms, 50 ms jitter | each within 1.5 s; no epoch regression; 0 deletions; never a frame with no simulator |
| T15 | forced integrity failure, including under a stick rider | ABORT, RECLAIM; repaired by PART_ADD within 2 s; 0 deaths; no deadlock |
| T16 | a stick mount while another player holds | the rider waits; granted on release; devices switch on |
| T17 | 50 toggles each way | each once; IsOn within RTT + 100 ms; 0 unexplained changes |
| T18 | runner shakes a green part off | same death kind on every screen within RTT + 0.1 s; 0 remakes |
| T19 | follower's player takes a part off | claim, then as T18 |
| T20 | middle part removed | two cids, both followed within 5 cm |
| T21 | merge of two builds | one cid; 0 duplicate actors |
| T22 | forced local deletion of a follower part | repaired within 2 s |
| T23 | follower bond broken on purpose | re-bonded within 1 s |
| T24 | every device kind in pairs | effect on the follower within 0.3 s; 0 gated writes landed |
| T25 | B activates A's build | B's drain within 10% of single-player; A's gauge unchanged |
| T26 | activator's gauge empties while it is a follower | off everywhere within RTT + 0.2 s |
| T27 | non-activator's gauge empty; activator in a menu 30 s; battery build | stays on in every case |
| T27b | activator in a menu 90 s, once with the other player on the stick and once with nobody on it | at 60 s the rider pays, or the build switches off; never before 60 s |
| T28 | battery part | no gauge change; follower battery within 2% of the runner's |
| T29 | 50 stand-in swings at a device | 0 toggles from swings; 1 per real hit |
| T30 | enemy hits a device; beam on an enemy B owns | 1 toggle per hit; damage once, in the owner's game |
| T31 | stand-in walks into and stands in a build its world runs | build displacement 0; no spike |
| T32 | remote rider on a raft and a 20 m/s car | raft tilt within 30% of a real Link's; car speed unaffected by the passenger |
| T33 | remote rider shown on a build | look and body within 5 cm of part pose x ride_off |
| T34 | runner walks 120 m away, another player near | authority moves within 3 s; never freezes |
| T35 | runner fast travels or loads, with and without a player near | hand-off within 3.5 s or DORMANT; 0 deaths replayed; 0 deletions under a rider |
| T36 | runner disconnects; runner in a menu 30 s | election within 0.5 s of LEAVE; OFFER within 1 s of the menu; 0 deletions |
| T37 | 4 s stall; 7 s link loss and rejoin | HOLD and blend; session end promotes locally; rejoin reconciles without chasing a rider |
| T38 | late join among 5 builds; a player walking into range of a build 500 m away; 3 players with simultaneous grabs | versions delivered within 1 s of entering range; one winner per cid |
| T39 | 30 min soak, all kinds, fire and lava, 3 games | 0 deaths not from a version; 0 repairs; 0 duplicates; 0 CTR drops |
| T40 | cost | hooks ≤ 0.3 ms per frame; CTR ≤ 40 KB/s with 10 builds |
| T41 | B on A's spring, both directions | B launched in its own world within 10% of single-player height |
| T42 | A's cannon at B's enemies; frost ice | one ball, one blast, enemy damaged once; ice the same on both screens |
| T43 | two driven builds collide at 10 m/s | each bounces once; no doubled bounce |
| T44 | B glues a fan while A holds the frame; two simultaneous glues | both parts attached in every game within 1 s; A's hold continues |
| T45 | a moving build that once rested is promoted | 0 velocity loss at promotion; flags 44/45 clear |

## 18. Risks and fallbacks
| Id | Risk | Fallback |
|---|---|---|
| RK1 | the rigid field still splits builds | gate X3's remover on followers; else root-only with feed-forward |
| RK2 | followers chatter against local contacts | wider REST band, lower Kp, FREE on contact |
| RK3 | fast passenger vehicles miss T9 | keyframed followers only if X2(d) passes, with type 2 restored at the grant in one flush |
| RK4 | a guard has side effects | green once grouped; backstop refusals; repair |
| RK5 | a device force escapes G1/G2 | constraint disable; node lists from an actor-pack inventory |
| RK6 | a gate turns a visual off | stream the visual's inputs as TLVs |
| RK7 | native refusal unreachable | wait inert and resend TAKE |
| RK8 | no BuildObject without the ability | section 7 (c); then (logged) no copies for such players, with stand-ins still ghosted |
| RK9 | in-place add fails | REMAKE (not under a rider) |
| RK10 | old bodies linger | park them |
| RK11 | no per-pair ignore | fallback A, then B (9) |
| RK12 | elections flap or a partition leaves two runners | (e, h) order, LEAVE-only elections, rejoin rule |
| RK13 | COM or velocity reads wrong | body origins, lower dWmax |
| RK14 | hook cost on hot setters | role lookup by hash in the snapshot; measured in T40 |
| RK15 | gauge override side effects | drop it; rely on blocked status and the mirror |
| RK16 | unknown engine delete paths | classified as unload by default (I15) |
| RK17 | more than 21 bonds | extension bonds queued after assembly |
| RK18 | comp #81 gating hides visuals | TLVs for those parameters |
| RK19 | spring launches differ between worlds | EVENT-token gating of the launch calls |
| RK20 | the chemistry hook is not found | fire shown only where it burns, and logged |
| RK21 | the Recall recorder cannot be fed | shorter rewinds on fresh copies (D6) |
| RK22 | caps leave too few followers | farthest-first eviction, logged |

## 19. Assumptions
| Id | Assumption | Experiment | Fallback |
|---|---|---|---|
| AS1 | the rigid field keeps one group, layout under 1 cm | X2, X3 | RK1, RK3 |
| AS2 | splits need constraint error | X3 | gate the remover |
| AS3 | the vertical observer compensates gravity, buoyancy and per-preset gravity | X2 | velocity g_ff plus the integral |
| AS4 | HEAD's angular sign | X1 | flip it |
| AS5 | COM readable via hknpBody+0x80 | X1 | body origins |
| AS6 | setters work from phase 0 | X1 | flush-entry hook |
| AS7 | the G3 entry bits are side-effect free | X4 | dispatcher hook plus backstop |
| AS8 | backstop refusals only cause retries | X4 | green once grouped |
| AS9 | passed state requests leave followers consistent | X5 | replay the last state at promotion |
| AS10 | call-site motor gates keep visuals | X5 | constraint disable plus TLVs |
| AS11 | G1, the rocket skip and comp #81 gating remove every physical effect | X5 | node lists |
| AS12 | priority-0 follow without flicker | X11 | toggle-bit follow |
| AS13 | contacts_filter sees attackers on parts | X11 | toggles only within 2 frames of this Link's attack |
| AS14 | placed actors expose a placement identity equal across games | X8 | name plus record-pose match, unique within 0.3 m |
| AS15 | rotated base and pre-confirm rigid refresh work | X6 | identity base plus a snap after completion |
| AS16 | copies spend nothing | X6 | block 0x209b754 |
| AS17 | a BuildObject is usable without the ability | X6 | 7 (c), then RK8 |
| AS18 | the held state writes no bodies | X6 | G1 log-only during assembly |
| AS19 | structural changes hit hooks; re-slots do not | X8 | 1 Hz scan |
| AS20 | settled reads reliable without the watch | X0 | wait 0.5 s after bond events |
| AS21 | queue-path bonds in place | X9 | REMAKE |
| AS22 | the single-edge break reproduces splits | X9 | REMAKE |
| AS23 | deleted bodies leave the world | X7 | park |
| AS24 | the 0x35 + LR refusal is native; part_end works from T3 | X10 | wait inert |
| AS26 | mgr+0x50 bit 0 means blocked | X13 | RK15 |
| AS27 | T2 runs in menus and loads | X0 | heartbeat from the net thread |
| AS28 | 120/160 m inside the keep range | X15a | unload distance - 30 m |
| AS29 | a force reproduces a Link's weight | X14 | tune or omit (decision D3) |
| AS30 | a per-pair ignore exists | X14b | RK11 |
| AS31 | spawns come from requestCreateActor in the device update | X12 | delete follower-made spawns by name |
| AS32 | the unload delete LR is identifiable | X15a | unknown LRs treated as unload |
| AS33 | an actor status-apply hook exists | X11 | RK20 |
| AS34 | part+0x1c is the pose the held part converges to | X10, X2 | ω-steady extrapolation only |
| AS35 | 0x1c0a43c tests arbitrary targets against all occupants | X2 | actor scan alone |
| AS36 | follower spring launches reproduce the runner's | X5 | RK19 |
| AS37 | Recall and Fuse accept points can be found | X16 | LR-chain classifier |
| AS38 | pools and cap allow the follower budget | X15a | RK22 |

## 20. Attack answers (index)
| Attack | Answer |
|---|---|
| bit 44 zeroes velocity at promotion | 3.8, 5.4, T45 |
| type-1 stand-in at speed | I16, 9, T32 |
| springs | G1 passed list, 8.3, T41 |
| no Autobuild ability | 7 step 2, X6 at P0, T3 |
| unload counted as death | I15, 10.3, G4, T35 |
| REST in air; hover bob | 3.5 REST, 3.3, T8 |
| battery and menu switch-offs; pilot pays | 8.7, T27 |
| state requests dropped | G1, I2 presets, 5.4 |
| touch frame | 4 touched-here words, 13 order, G6, T10 |
| asynchronous DETACH and blasts | intent marks (4), 6.6, 8.5 |
| damage at promotion | 8.6, 5.4, X4 |
| snap into occupants | 3.7 |
| cannon, beam, ice harm | 8.5, 8.6, T42 |
| device cap and pools | 10.5, X15a |
| relay id reuse; nonce collisions | 6.1 incarnations, 10.6 |
| silence elections | 5.9 |
| session end and rejoin | 10.6, 10.7, T37 |
| inbox drops; 3-player acks; control behind bulk | 12.3 |
| masks across versions | 8.1, 11 |
| version order after election | 6.5, 5.9 |
| settled-frame circularity | 6.4 |
| integrity deadlock under rider | 5.4 repairs, 6.6 |
| map-native re-registration | 6.2 candidates, 6.1 |
| late range entrants | 6.5 interest delivery |
| snapshot freed under T2 | 13 pool and seqlock |
| glue while another holds | 6.7, T44 |
| stick against a holder | 5.5, 5.6, T16 |
| colliding driven builds | 3.5 FREE, T43 |
| remote bombs and knockback | G1 capture, IMPULSE |
| runner hitch | 3.2 |
| rotation-step overshoot | 3.3 |
| comp #81 | G1, X5 |
| Fuse and Recall | 5.1, G4 queueing, X16, D6 |
| fire and ice status | 8.6 |
| copy lowered onto a Link | 7 step 11 |
| glued NPCs | 6.2, 6.6 |

## 21. Coverage (row ids from CM)
- **A, creation.**
  - A1-A2 and A4: recorded at settled pieces; followers made by section 7 with costs blocked and a progress-independent BuildObject (high for the recipe, medium for the BuildObject).
  - A3, A12 and A13: existing, map and NPC parts adopted by placement identity (medium-low).
  - A5: never registered.
  - A6: selection deferred.
  - A7-A10: glue is native for the gluer, then EDIT_REQ and in-place ADD or MERGE (medium).
  - A11: kind-1 entity.
  - A14: not registered.
- **B, Ultrahand.**
  - B1-B3: PENDING in the touch frame, then grant (medium-high).
  - B4: native refusal.
  - B5: native runner, followers on predicted motion.
  - B6-B7: runner events replayed in place (medium-high).
  - B8: the holder runs it; the rider rides its follower, and its weight is a force in the holder's world.
  - B9: refused while assembling.
  - B10: local.
- **C, Recall and Fuse.**
  - C1: strength-3 claim; the rewind uses local history (D6) (low-medium).
  - C2: claim, with the delete queued and re-posted (low).
  - C3-C4: outside contraption sync.
- **D, switching.**
  - D1-D2: marked hits become TOGGLE (high).
  - D3: stand-in swings dropped.
  - D4: enemy owner's game only.
  - D5-D6: the stick one-shot in the touch frame; stick mount is an activation.
  - D7: rider waits inert.
  - D8: follower self-starts dropped.
  - D9: 0x1917ac0 and 0x32eb90 sends gated; TLVs.
- **E, energy.**
  - E1 and E7: activator at each on-command, including stick mounts.
  - E2: activator's own nodes.
  - E3: blocked status ends it.
  - E4: battery never switched off by the sync; life mirrored.
  - E5-E6: native.
- **F, devices:** per 8.3. F2 wind capsules per game. F13-F16 harm per 8.6. F19 spring native launch. F20-F21 runner spawns and replays. F28-F29 TLVs.
- **G, riding.**
  - G1-G2: velocity carry and rider gains.
  - G3: weight force.
  - G4: ride info with uid.
  - G5: no motion change.
  - G6: AttachGround native.
- **H, contact.**
  - H1: local push on the follower, PUSH, then claim.
  - H2: stand-ins never collide with builds.
  - H3: FREE on contact; unoccupied builds unified by strength, then distance.
  - H4: runner's world.
  - H5: harm per 8.6.
  - H6: per-object owner (low).
  - H7: runner physics.
- **I, damage.**
  - I1, I7 and I8: guards plus replays.
  - I2-I4: forwarded HIT and status.
  - I5-I6: physics.
  - I9: never deleted on silence.
- **J, lifetime.**
  - J1-J5: interest, proximity OFFER, RELINQUISH.
  - J6: measured caps.
  - J7-J9: unload hand-off, load reset.
  - J10-J11: LEAVE elections.
  - J12: interest delivery.
  - J13: per-pair streams.
  - J14: HOLD below 5 s; session end above 6 s.
  - J15-J16: incarnation cids and uids.
- **M, mechanics:**
  - M1-M20 as before, with these changes:
    - M8: I4;
    - M14: the self-mirror, test only;
    - M15: state requests pass; presets mirrored;
    - M16: copy made with a per-pair ignore of the local Link.
  - M21: session end promotes locally.
  - M22: relay id change becomes leave plus rejoin.

## 22. Decisions (the tester, 2026-11-09)
- **D1.** While the activator's game is loading or rebuilding its copy, the build keeps running and nobody pays. In a menu it runs free for 60 s; then the stick rider pays, or the build switches off (8.7).
- **D2.** When the activator leaves, the stick rider becomes the payer, otherwise the build switches off.
- **D3.** A remote rider's weight is an approximate force until X14d.
- **D4.** A stick rider who mounts while another player holds the build waits on an inert stick until the holder lets go.
- **D5.** After a network split, the session side's version wins for builds both sides touched.
- **D6.** Recall on a copy made seconds ago rewinds only what that copy recorded until X16 shows the recorder can be fed.
- **D7.** Quest effects of glued NPCs complete per world.
## 23. P0 results (2026-11-09)
Built: checked reads (coop_game.c counts failed reads per calling site and logs them every 5 s); the ABLOG write watch
is opt-in (TOTK_COOP_TEST_GLUEWATCH); HEAD's dynamic root drive restored (the keyframed attempt dropped); the CTR
channel (coop_net.c kind 11, routed by header byte 7 in both relays, net version 29) and coop_ctr.c (incarnations,
reliable streams per sender, receiver and channel, latest-only slots, HB at 30 Hz from the co-op poll); coop_life.c
(hooks on the delete and death requests, BondMaker and the confirm's item charge); the X1, X6 and X15a tests.
Deferred to P2: the registry and its snapshot pool, which have no user before P2.

- **X0 (reads without the watch): pass.** Five pair scenarios (toggle, part off, carry, stick, normal) with the watch
  off. Every copy grouped whole (4 of 4, 3 of 3, 6 of 6, 7 of 7); no "3 of 4", no record left unsent, no false
  adoption (AS20). Failed reads remain at sites that read stale actor pointers (coop_autobuild_tick, dev_on, eq_gear
  and other player code): unknown now, never 0. The stick run still deletes and remakes a copy when the owner's group
  is re-keyed ("shares objects"), which minted ids (6.1) remove.
- **X6 (copies without the Autobuild ability): pass for creation.** The Autobuild ability is a game manager made at
  boot, [main+0x3a62ef0], with its build object at +0x51f0 and its menu object at +0x51f8 (+0x3c80 1) on every save.
  The first save has IsGet.Obj_AutoBuilder 0 (read through main+0x3d01a0, hash 0x5d930d12), and its ability, build
  object and update (main+0x9a2c00) are all there. bp_make now reads the singleton. Pair run with B on that save, moved
  next to A's Fanplane: B made the copy, all 4 parts in one group, without the ability. Some seconds later the group
  was re-slotted and one part died (state 11): the follower-physics failure P1 addresses (X2, X4), not creation.
  The confirm's item charge main+0x209b754 is skipped while a copy's confirm runs (coop_life.c); that run did not
  reach it, so the skip is unexercised.
- **X1 (bodies): one velocity write wakes a resting body; flag 44 is harmless here.** A resting plank given 3 m/s up
  once: the body's velocity (+0xf8) reads 2.52 m/s the next frame (a write lands one frame later) and falls by
  0.48 m/s a frame (29 m/s^2 at one physics step per 60 Hz frame for that plank). The same with body flag 44 set gave
  the same frames: the velocity was not zeroed. Section 0.3's reading that flags 44/45 make a velocity last one step
  is not reproduced for this case; I2 (the co-op never sets body flags) stands as a rule anyway.
- **X15a (unload, capacity): partly.** The group table has 256 slots and the bond pool 512 (a 4-part build took 4).
  Far deletes arrive through main+0x63b6b0 with reason 3 from main+0x249174 (GameActor calc) and main+0x636994 (the
  engine's base calc), asked again every frame while deferred. A plank moved by teleport to 150, 300, 600 and 1200 m
  was never deleted in 35 s: moving the actor does not unload it, so the unload test has to move the Link (X15b).
- **Channel (P0 gate "0 CTR drops with 3 builds"): pass.** Two games, 60 s, each sending three 200-byte states 30
  times a second, a control message twice a second and 7 bulk chunks of 1100 bytes per build every 10 s: every
  reliable message arrived once and in order (119 control, 126 bulk), 0 gaps, 0 duplicates, 0 net drops, 0 resends,
  and the three state slots stayed fresh. A normal two-game start on the final build joined both games, each with
  its own incarnation.

Next is P1: the self-mirror lab, X2 (the rigid velocity field against HEAD's root drive), X3, X14b, X4 and X5.

## 24. P1 results so far (2026-11-09)
Built: the self-mirror lab (TOTK_COOP_TEST_MIRROR=<dz>,<delay ms>,<jitter ms>,<loss %>,<dy>: one game hands its own
records and root poses to its own copy path as a player 7, the copy made aside, the poses delayed, jittered and partly
dropped; TOTK_COOP_TEST_MIRROR_SCENE=<s> moves the original through rest, lift, carries at 3 and 8 m/s, a turn, a drop
with a tumble and rest, and logs the copy's tracking and layout per part of the scene); the rigid-field drive
(coop_autobuild.c follow_rigid; TOTK_COOP_FOLLOW=root keeps HEAD's root drive); the X3 hooks (edge removals, the
bond-strain check, glue messages posted to a part); G4 (coop_life.c: a follower part takes no game-rule death).

- **Why copies came apart: their green came late.** The copy maker gave the confirm's mark without the green bit and
  added green only once the parts shared a group. For the first frames the parts were not one contraption to the
  game's collision filter: a fan's own spinning blade (a kinematic body) pressed the wing it is glued to, the wing died
  of PhysicsKinematicCollision (death kind 15, from the actor update at main+0x27b300), its glue edges broke as it died
  (main+0x455e10 from the part update), and every other part then died of the broken-off rule (kind 20). Green at the
  confirm, as the game's own confirm gives it, removes the chain: 0 refused deaths and 0 bond strain afterwards, the
  copy grouped whole. The deaths it once avoided are refused for copies by G4.
- **X3, what separates bonds:** glued bonds are breakable strands in each glue component's +0x170 list. Each frame
  main+0x190f034 compares the distance between a strand's two pivots with a threshold scaled by the parts' bond strength;
  the strain at strand+0x1d4 rises while it is over and the edge is removed at 1; strand+0x1e2 exempts a strand. A copy
  made inside the ground strained its bonds to 1 in three frames (errors of 1.5 to 2.4 m). The other path is a dying
  part breaking its own edges. With green at the confirm and the copy in free space, 0 removals.
- **X2, rigid field against HEAD's root drive** (mirror 15 m aside and 12 m up, 100 ms delay, 20 ms jitter, 10 % loss,
  speed changes ramped over 0.5 s; tracking p95 / layout p95):

  | Part of the scene | rigid field | root drive | plan (X2 a) |
  |---|---|---|---|
  | held still in the air | 0.023 m / 0.2 mm | 0.091 m / 0.2 mm | 0.03 m |
  | carry 3 m/s | 0.074 m / 1.7 mm | 0.351 m / 1.7 mm | 0.10 m |
  | carry 8 m/s | 0.165 m / 1.7 mm | 0.971 m / 1.8 mm | 0.25 m |
  | turn 1.5 rad/s | 0.120 m / 6.7 mm | 0.215 m / 7.6 mm | - |
  | drop and tumble | 0.383 m / 3.0 mm (max 4 cm) | 0.540 m / 2.8 mm | 0.4 m |
  | at rest after | 0.007 m / 0.5 mm | 0.093 m / 0.8 mm | 0.03 m |

  The rigid field meets the plan's lines here and is now the default. Two changes made it so: a step observer on all
  three axes (what a physics step takes off the velocity written: gravity, damping), and an integral gain of 3. On the
  ground at rest the sideways part of the observer read friction and made the copy chatter; it is now used only while
  the build moves, and the rest band is wider (enter under 10 cm and 4 degrees, leave over 25 cm or 8 degrees, or when
  the runner moves).
- **Pair test** (A builds its Fanplane and turns its fans on and off; B follows): B made the copy whole, followed it
  within 3 cm, then rested next to it within millimetres, its devices switched with A's.

Still to do in P1: X4's remaining guard cases (the lapse, a damaged part promoted), X5 (device gates), X14b (stand-in
collision ignore), and T2 (a Link riding a follower).

### 24.1 P1, second part (2026-11-09)
- **All remaining plan hooks registered at once** (cpu.h COOP_HOOKED_V3: the G1-G8 gates, the touch hooks of section 5,
  the stand-in hooks of section 9), dispatched to coop_life.c coop_life_v3, which lets the game run as is until a phase
  gives a hook its behaviour. Later phases rebuild only the runtime. A full rebuild with 24 jobs takes about 4 minutes.
- **G1 is on** (coop_autobuild.c coop_g1): every write to a core body of a part another game runs is dropped, except the
  drive's own, the spring's two launches, and a part this game's player holds (until P3's touch words). In the pair runs
  it kept off the copy's fan impulses and the wing's own every-frame velocity and force writes (main+0x221d5ac..0x221dbf8,
  its lift and stabilizer node), with no effect on the copy's shape.
- **The clean line** (coop_autobuild.c clean_line), every 10 s: refused deaths, copies not whole, copy parts gone,
  unreadable copy parts, channel drops, and G1's dropped writes by function. Every gate run below logged 0 for every
  failure count.
- **The drive** adds an alternating 2e-4 m/s to what it writes, so a write always differs from the body's stored value
  (the setters skip equal values).
- **Gate suite** (two games each: devices toggled from the other game, a part taken off, a grab hand-over and carry, a
  steering-stick hand-over, the channel under load): copies made whole (4 of 4), toggles sent to and decided by the game
  that runs the contraption, both hand-overs granted, the channel lost and repeated nothing, every clean count 0.
- **Open: a copy held in mid-air while its runner rests on the ground can stall 0.275 m below its place** in the mirror
  lab (its root body's stored velocity -3.34 m/s and its place frozen while the drive asks +2.2 m/s up). It is not a
  regression (the e421be1 build does the same at the same lab setting), not G1, not the build object and not the
  scenery. It never showed in a pair run, where copy and original rest on the same ground. To check in P3: a runner
  holding its contraption perfectly still in the air with Ultrahand, the real case closest to it.
- Still to do in P1: X5 for device kinds other than the fan (it needs builds with them; the in-place bond path of P4
  can make them), X14b (the stand-in collision ignore), the rest of X4 (the lapse, a damaged part promoted) and T2 (a
  Link riding a follower).

## 25. Review of the plan and of how it is carried out (2026-11-09, evening)
Asked for by the tester after P0, P1 and the first milestone-1 work took a day of mostly waiting on pair runs.

### 25.1 Verdict
The design of sections 1-14 stands. Its core was measured (24, X2), copies stay whole, both hand-overs pass and the
channel loses nothing. No failure since came from the design. The failures came from how it was carried out:

- **Runs were used to find out how the game works.** Device off and the stand-in veto were written against guesses and
  tried, several runs each. The notes already answered most of both (25.3). A run is for confirming, not for finding out.
- **Plan pieces were bolted onto the old code.** There is no registry (6.2) and no minted cid (6.1): coop_autobuild.c
  still matches builds by shared actors and by position within 4 m. Device follow re-asks a toggle every 0.6 s until
  the state reads right, where 8.1 says the game's own request path. Both are the piece-by-piece patching this plan
  was written to end, and both fail in ways only a run shows (the stick explosion, the misshapen copy).
- **The self-mirror lab made problems real play does not have** (24.1's mid-air stall). It cost several runs.
- **Section 17 is too large for this stage**: 45 tests, many with 20 or 50 repeats, a soak, three players and network
  splits, before two players can play.

### 25.2 Process from here (replaces the gate column of 16 and the repeat counts of 17 until the last milestone)
1. **Notes first.** Before a piece is written, its game-side questions are looked up in R/ (devices, devices2,
   autobuild, physics, runtime, sync-v3). What the notes do not answer is read in Ghidra and added to the notes. No
   code is written against a guess; an assumption that cannot be read is listed in the milestone's results.
2. **A milestone is written whole, then run once.** No run between its pieces.
3. **A failed run is explained from its logs before the next one.** The next run confirms a stated cause; it never
   tries a guess. Two failures in a row on the same piece mean back to step 1.
4. **One scripted gate run per milestone** (tools/coop/gate_contraption.ps1, about 4 minutes), judged by the clean line
   and the pass lines, then the tester's play-test. Nobody has to watch the scripted run.
5. **The mirror lab is for drive tuning only**, which is done. A lab-only oddity is noted and left.
6. **The structure of 6.1 and 6.2 is built as written**: registry keyed by minted cid, part uids, versions. The
   overlap and position matching goes.
7. **Put off to the last milestone:** repeat counts (T10's 20 runs, T17's 50 toggles), T11 prefetch, T14 loss and
   jitter, T38 three players and late join, T37 link loss, T39 soak, T40 cost. They check the robustness of code that
   has to exist first.

### 25.3 What the notes already say about the two open pieces
- **Switching a group off.** The game does it with messages, not with a DeviceSwitch request: the group's sender
  main+0x32eb90 sends 0x6b510001 (off) or 0x6b510000 (on) to each member's on/off node (handler main+0x245c114),
  whose calc main+0x9f9088 turns the device's bools off (R/devices2/energy/c-gauge-callers section 4, R/devices
  section 3.1). A request through main+0x1c08774 is only taken by a powered device and keeps the lower priority, and
  a hit only ever asks for value 1. Left to read: which of the two paths a hit on a running device takes to switch
  it off, and who sets the sender's flag bits in group+0x878.
- **The stand-in veto.** main+0x40a240 compares the two bodies' filter words (layer and group bits at +0x18, a group
  id at +0x2a, an ignore bitset at +0x20; R/runtime/triage/rate_fans_on.md). Its arguments are those filter records,
  so matching them against a body holder plus 8 was a guess. Left to read: how a body reaches its filter record, so
  the two sets are built from the same pointers the predicate receives.

### 25.4 Milestones (the "playable soonest" order; phases of 16 in brackets)
| Milestone | Content | Done when |
|---|---|---|
| 1 | registry and minted ids (P2); touch hand-overs (P3); device on and off by the game's own path; stand-in veto (I16); rider on a follower (T2); the rest of X4 | one gate run clean; the tester's play-test: build, switch, grab, steer and ride each other's builds |
| 2 | building together (P4); the common devices (P5) | the same, with gluing onto each other's builds and every common device kind |
| 3 | P6-P8 and everything 25.2 point 7 put off | section 17 in full |

State when this was written: milestone 1's hand-overs pass in the gate; device off, the veto and the registry are open;
that work is not committed.

## 26. Milestone 1, first run (2026-11-09, late)
One gate run (tag m1a), every clean line 0. carry and partoff passed; tog and stick did not.

**Confirmed by the run**
- **Stand-in veto (I16).** A rigid body's collision filter record is `[body+0x140] + 8`; the veto had read `+0x148`,
  which is another class. With the right record it refused 26 and 78 stand-in pairs in two scenarios (0 before).
- **Record part order.** The history writer sorts the group's members by name and keeps the group's order among equal
  names. bp_send makes the same order and checks count and names; the nearest-within-4 m matching is gone. Every
  record lined up and every copy was made.
- **One key mint.** A copy taken over after it changed shape here is entered under a key bp_send then uses. That path
  had announced a key made from the group pointer, which bp_send stopped using.

**Refuted by the run: device on/off by the request pair at the group's fold.** The group of an idle Fanplane already
reads state 1 while its fans are off, so "group state 1" is not "devices on" and the code found nothing to do. The
reading behind it had stopped before the part that turns a device on, which is the mistake 25.2 point 1 is there to
prevent. The earlier switching (the hit's toggle bit) is back in place: on works, off is unreliable.

**What is known and what is not** (research notes sync-v3/m1_device_off_and_filter.md)
- Known: a hit sets the group flag [0x1a] in `group+0x880` (main+0x1c02a98) and then the member's toggle bit. The
  fold turns that flag, with flag bit 22 of `group+0x878`, into bit 27, and the group's sender then sends OFF (or ON
  with bit 21) to every member when bits 27, 10 and 26 are set. The toggle bit alone only ever asks "on".
- Not known: what bits 22, 10, 26 and 21 stand for and who sets them, and how the on/off node turns its message bits
  and the switch state into the on bool. This is one read of main+0x9f9088, 0x9fa28c, 0x9fad68, 0x9fb82c and the hit
  code main+0x18d2b48, start to finish, before device switching is written again.

**The registry cannot be milestone 1 on its own.** coop_autobuild.c keeps two tables, "mine" and "copies of theirs",
and only a build's first owner sends its records. 6.2's single table needs whoever runs a build to send its versions,
which is P4. So 25.4 changes: milestone 1 is what this section confirmed plus device switching; the registry moves to
milestone 2 with P4, as one rewrite of the contraption half of coop_autobuild.c into its own file.

## 27. The rebuilt core (agreed with the tester, 2026-11-09)
Not a restart. What was measured stays as it is: the history record and the Autobuild copy (7), the rigid-field drive
(3), the gates G1 and G4, the stand-in veto, the touch hand-over, the contraption channel. What is rebuilt is the part
of coop_autobuild.c that holds contraptions, because its shape is what section 14 set out to remove: two tables
(own_c, "mine"; bp_in, "copies of theirs") with the same logic written twice, only a build's first owner sending its
structure, and a copy that changes shape in another game being re-keyed and made again there (CBOW).

### 27.1 Files
- `runtime/coop_ctn.c` (new): the registry, records and versions, copy creation, the drive, device state, authority,
  the gates' "is this a follower" answers. `runtime/coop_ctn.h`: what other files call.
- `runtime/coop_autobuild.c`: keeps the Autobuild experiments and test switches only.
- The names other files call (`coop_contraption_made`, `_follower`, `_ident`, `_find`, `_owns`, `coop_g1`) stay.

### 27.2 One entry per build, in every game
| Field | Meaning |
|---|---|
| cid | {minter incarnation u64, serial u32}, minted once by the game that first records the build (6.1) |
| runner, epoch | the incarnation whose game simulates it now, and the count of its changes (5) |
| role here | RUN (this game simulates it) or FOLLOW |
| v, shape | the version this game holds: a count kept across runners, and the record's shape hash (6.5) |
| record | the CmbAct, its centre, and for each part the minter's object number while the part is still the minter's original |
| parts | this game's actor for each record part, in record order: uid = record index for this version |
| local state | NONE, ASSEMBLING, MAKING, LIVE, REMAKING (old parts leaving) |
| stream, drive | the runner's last three root samples, the clock offset, the drive's state (3) |
| devices | which parts have a switch, which are on at the runner, and who pays (8) |
| touched | this game's player holds or steers it |

### 27.3 Rules
- **R1 The runner sends the structure.** Whichever game runs a build records it at phase 0 every 2 s. A changed shape
  is version v + 1. It is sent to every game. This holds for a build minted elsewhere too.
- **R2 Everyone else applies it.** A game that does not run the build and holds an older version makes the copy if it
  has no parts, or deletes its parts and makes it again (REMAKE, 6.6) if it has. The minter's original actors are
  deleted like any others when a newer version comes from another runner; from then on the minter holds a copy.
- **R3 A change made on a follower.** When a follower's parts stop being one group of the record's size for 1 s: if
  this game's player caused it (holding it, or Ultrahand within 2 s), this game asks to run the build (TAKE) and,
  once granted, R1 sends the new version under the same cid. Otherwise the copy is made again from the runner's
  version. This replaces CBOW, given_away and adopt_tick. EDIT_REQ (6.7) can replace the TAKE later without
  changing the entry.
- **R4 Pieces.** After a split the largest piece keeps the cid (a tie: the piece holding record part 0). Each other
  piece of two parts or more is minted by the runner as a new build that it runs. A merge keeps the larger build's cid.
- **R5 Identity never comes from position or from a group pointer.** A group is known by the registry entry most of
  its member actors belong to; record parts are ordered as the game's writer orders them (26).
- **R6 Silence deletes nothing** (I7): an entry leaves only on RETIRE from its runner, or when its runner's game
  leaves and no other game holds it.

### 27.4 Messages (on the contraption channel, 12.4)
| Message | Reliable | Content |
|---|---|---|
| VERSION | yes, in order | cid, epoch, v, shape, centre, object numbers, the record in 1,100-byte pieces |
| STATE | latest only, 30 a second | cid, epoch, v, runner clock, root matrix, switch and on bits, who pays, rest flag |
| TAKE / GRANT | yes | cid, epoch wanted / given, to whom |
| TOGGLE | yes | cid, part, wanted on or off, from |
| RETIRE | yes | cid |
The "CB??" packets on the hit channel (CBBP, CBMV, CBAU, CBOW, CBTG) go.

### 27.5 Order of work
1. The device read of 26 finished and written into the notes (three readers).
2. coop_ctn.c written whole from the table and rules above; the measured pieces moved in unchanged.
3. One build, one gate run, then the play-test.

## 28. The rebuilt core: results (2026-11-09, night)
Section 27 is written: `runtime/coop_ctn.c` holds the registry, versions, copy making, the drive, authority and device
state; `runtime/coop_autobuild.c` keeps the experiments. The five "CB??" packets on the hit channel are gone.

**Device on and off (the open piece of 26), read before it was written** (research notes sync-v3/onoff/)
- A device's on state is a latch in its on/off node. The DeviceSwitch request and the group state do not switch it.
- The node takes two true set operations, messages 0x6b510000 (on) and 0x6b510001 (off): on only when off, off only
  when on. Co-op sends them per device with the call the build's own sender makes.
- A hit writes a hit event on the build and a flip bit on the device and does not look at the on state. On a build
  another game runs, both are taken off and the wanted state is sent to the runner (TOGGLE).
- The game keeps a build's devices in one state: while some are on and some are off, the build sends "on" to all.

**Runs**
| Run | Result | What it told |
|---|---|---|
| core1 (gate) | carry, partoff, stick pass; tog fails | "on" works by the set message in both games; B never asked for off |
| core2, core3 (tog) | pass | B's fans read off 1.5 s after A's and stay off |
| core4 (gate) | carry, partoff, stick pass; tog fails | A, asked off, sent three set-offs and kept reporting on |
| core5 (tog, three times) | pass, pass, pass | the fix below |
Every clean line in every run reads 0. partoff ran the version path whole: version 2 with three parts, the four deleted,
the three made.

**The fault and its fix.** The three set-off messages of one build were sent from the co-op tick, which is not lined
up with the frame. When they landed on different sides of the devices' node updates, one fan went off a frame before
the others and the game's one-state rule switched all three back on. The wanted states are now queued and sent
together at actor phase 0, before any device update.

**What went wrong in the process again, and the rules added to 25.2**
- core2 was needed only because the lines showing each game's view of the other had been dropped; core3 only because
  the scenario passed on B asking for off, not on the fans being off; core4 because one pass was taken as proof.
- 8. Every write into the game states its thread and its point in the frame before it is built.
- 9. A scenario passes on the final state in both games, never on a request.
- 10. Each game logs its own view and the other's, every second while something is changing.
- 11. A failure that is not explained counts as intermittent and gets its repeat runs once.

**Open**
- **The stick breaks off in the first owner's game when another player's stand-in arrives at it**, before any
  hand-over: the build sinks about 19 cm in that half second, the game takes the stick out of the group 0.9 s later and
  the loose stick dies. 6 of 16 stick runs today (h1, m1c, m1e, x0, core1; not m1a, core4). The veto refused 382
  stand-in pairs in core1, so what presses the build is something of the stand-in the veto does not cover. The new
  repair rule (27.3 R3) then makes the build again, which is what is seen as an explosion. To read first: how the
  player's bodies are set up, and which of them the veto's holder lists miss.
- The tog scenario sets the flip bit a hit sets; it does not swing a sword. A scenario with real swings at a fan,
  judged on the fans' final state, is to be added.
- A follower whose own Link's gauge is empty may refuse or drop the on state (the node's gauge gate; 8.7).
- A build that unloads or is destroyed at its runner lapses there only; the others keep their copy (10.3).
- B's first core1 run never asked for off; not seen again in six runs, not explained.
- Not carried over: the self-mirror lab, ABON, ABPUSH and the root-only drive mode.

## 29. The rule of work from here (the tester, 2026-11-09, night)
This replaces 25.2 points 2 to 4 and 11, and every "one run to see" in sections 15 to 17.

1. **No guessing.** Nothing is written against a theory of how the game works.
2. **No run to find something out.** No instrumentation run, no repeat-until-it-breaks loop, no quick check.
3. **What is not known is read.** Sonnet readers turn the game code in question into a readable form (commented
   pseudocode with addresses, in the research folder). That is read, and the notes say what was learned.
4. **Then it is implemented.**
5. **Testing starts only when everything planned is implemented**, and each run needs the tester's yes.

What the evening before this rule cost and showed is in 28 and in the research notes (physics/README.md section 8):
two fixes for the stick breaking off were written on theories and were wrong; the measurements that followed name a
second stand-in body and five more bodies that the collision veto does not cover, without naming their owners.

### 29.1 Open questions handed to readers (no code until they are answered)
- How a Player actor holds its rigid bodies: every body of a stand-in, and the path from the actor to each.
- What bodies a Player's gear has (weapon, shield, bow, as their own actors), on which layers, and how the game keeps
  them from pushing things the Player stands by.
- The collision layers: their names and numbers, the layer matrix the filter main+0x40a240 reads, and the game's own
  ways to make two bodies ignore each other (X14b's candidates).

### 29.2 Implemented and not yet tested
Scope agreed with the tester: "everything" before the next test is the stick fix.

- **The stand-in veto covers every body (I16), coop_life.c.** Read first (research notes player-physics/,
  gear-physics/, collision-filter/): a Player's bodies sit in six places of its physics object, and the game has its
  own walk over all of them (main+0x3cc9b4 with main+0x2962c58); a Player's sword, shield and bow are actors of their
  own with one solid body each; a body's filter record is at +0x140 or, for the second physics world, +0x148. The veto
  now lists, by that walk, every body of each stand-in and of the gear actors it holds, and every body of each part,
  in both worlds. It listed one body per actor before.
  - What it rests on that a run has shown: the pair test does see the stand-in's second body (28, the last run).
  - What it calls without its body having been read: the provider's four getters (vtable +0x80, +0xa0, +0xd0,
    +0xe0), with the arguments the game's walk passes, from the co-op tick, four times a second.
  - What changes besides the stick: a stand-in's sensors no longer meet parts either, so a remote player's swing no
    longer reaches a build through the stand-in in another game. It reaches it through that player's own game (8.2).
  - Not used, because not read to the end: the filter's tag (record +0x2a) and the per-owner masks.
- **One run is asked for when the tester says so:** the stick scenario. It passes when the stick is still part of the
  build in the first owner's game after the other player has taken it, the i16 line counts more than one stand-in
  body, and every clean line reads 0. If the stick still breaks, the cause is not a stand-in or gear body meeting a
  part, and the next step is reading, not another run.

### 29.3 The run (tag veto2, one pass of the stick scenario, 2026-11-09)
- The stick stayed part of the build in the first owner's game to the end of the run, the other player took it, and the
  fans came on at that player's say in both games. Every clean line 0.
- The veto listed 26 records for the one stand-in and its 9 gear actors (1 before) against 36 records of the 7 parts,
  and refused 13,016 pairs (a few hundred before).
- The stick did not move when the stand-in arrived: 1514.55 before, 1514.53 to 1514.57 after. In every earlier run,
  those where it broke off and those where it did not, it dropped about 0.2 m in that half second. So the shove itself
  is gone, which one run can show; that the break is gone follows from it.
- Not looked at in this run: the other three scenarios on this build, and a remote swing reaching a build now that
  the stand-in's sensors no longer meet parts.
