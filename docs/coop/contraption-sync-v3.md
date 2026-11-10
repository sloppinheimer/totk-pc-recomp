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

## 30. Milestone 2 under the rule of 29 (2026-11-09)
Milestone 2 is P4 (structure) and P5 (devices). Its experiments X5, X7, X8, X9, X11, X12 and X13 were written as runs.
Under 29 each becomes a question answered by reading code. The one test of this milestone is asked for when all of it
is implemented.

### 30.1 What the notes already hold, and what they do not
Two surveys of the research folder, counting as known only what a note states from code or from a logged run:
`R/sync-v3/m2/p4_known_unknown.md` and `p5_known_unknown.md`.

| Topic | State |
|---|---|
| making a bond between two existing parts | partly: the maker and its record are known; the space of its frames, and how a part already built gets its bond read, are not |
| breaking one bond | partly: the functions take the edge pointer; finding the edge between two named bodies, and what a shake breaks, are not known |
| a structure change as an event | partly: the points where it fires are known; the graph job's merge and split, and when membership is final, are unread |
| a part's death and its replay | partly: the request can be refused; the replay's effect on edges and group is unread |
| bodies gone after a delete (X7) | not established; the plan's citation for it was wrong |
| identity of map and NPC parts, loose devices | partly: a placement hash exists, checked on enemies only |
| a small copy for one added part; a copy around the local Link | partly: a record needs at least one bond; reusing an existing actor is unread |
| per device kind: what moves it | named for 13 kinds; kept off followers for about 10 |
| per device kind: internal state | fully described for 0 of 26; no path from an actor to its AI node is in the notes |
| hits, harm, status on parts | partly: the toggle path is read; no status-apply function is identified |
| spawns | not established |
| energy and battery | partly: the callers and the payer are known; the gauge's "blocked" bit, the battery's drain and CustomEnergyRange are not |

### 30.2 Where the plan's text is wrong or the code does not do what it says (found by the surveys)
- 8.1 still switches by a DeviceSwitch request; that does not switch a device (28). The code uses the set messages.
- 6.6 says the break functions name the peer body; they take the edge.
- X7's body test cites a note that does not say it.
- The plan has part uids that never change; the code uses the record index, and parts of one name keep the group's
  order, which can change.
- "Event-driven" structure: the hooks only log; the runner polls every 2 s.
- The spring's launch is dropped on followers: G1 lets it pass, but the spring's apply function is among the wrapped
  force nodes.
- The wheel and cart motor nodes are skipped whole; the notes say to use the game's own "inert" test.
- The game switches a follower's devices off through its own gauge (DeviceSwitch +0x8d bit 0); 8.7 does not cover it.
- The contact filter covers enemy copies and stand-ins only, not parts.
- The delete reason the code passes (3) is the far-delete reason in the P0 notes.
Each is settled in the implementation of this milestone, from what the readers return, not from the plan's wording.

### 30.3 Readers (Sonnet), each writing readable code into R/sync-v3/m2/
- P4: bond-make, graph-groups, bond-break, death-delete, part-maker, placement-id.
- P5, after P4's are read: engine access (actor to node, blackboard), rocket and time bomb, spring and stake, balloon /
  hover stone / wing / stabilizer, wheels / sled / carts, stick and construct head, emitters, hits and status, spawns,
  energy and battery.

## 31. Play-test notes (the tester, from 2026-11-09)
Noted as they come, not acted on until their turn. Each gets its cause read before anything is written (29).

| # | What was seen | What is already known about it | Status |
|---|---|---|---|
| 1 | Two fans placed by hand and glued with Ultrahand turned green for the other player. The green tint belongs only to parts an Autobuild made from zonaite. | The copy maker gives every part it makes the green mark (coop_ctn.c make_pass: SPR +0x150 bit 0 at the confirm and again once grouped), whatever the runner's part has. The plan's rule is "green only where the runner's part is green" (7 step 7, D5); the record sent between games does not carry which parts are green. Read since (R/autobuild/README.md section 3, R/sync-v3/m2/p4/part-maker): green is SPR +0x150 bit 0, set only by main+0x1c07114, which the confirm calls only for parts made from zonaite (pool entry state 3); a reused existing part gets main+0x1c070c0 and no green; main+0x1b0f7d4 reads the bit. Fix: the runner sends each part's green bit with the version and the copy maker sets it only there. Green also decides the broken-off kill, the shake's mixed rule and the lapse timer, so a copy wrongly green is under rules it should not be once this game runs it. Still to read: whether the broken-off kill in main+0x271190 tests the green bit itself. | read; to implement |
| 2 | p1 = the player who placed the devices, p2 = the other one. p2 cannot switch p1's devices on with a jump attack. p2's regular attacks and arrows do switch them; p1's jump attacks do. Later in the same session: after p2 picked the build up with Ultrahand, p2's jump attacks switched it on and off. | Picking a build up makes that player's game the one that runs it (the grab hand-over), and it stays so after the build is put down. So the two observations are one: a jump attack works in the game that runs the build and not in a game that follows it. A hit switches a device through five game functions of two kinds (R/sync-v3/onoff/b_hit.md); on a build another game runs, the hit's toggle bit is taken off in the device's update and the wanted state sent to the runner. Which of the five a jump attack goes through, and whether it sets the toggle bit at all or switches the device another way, is not read. | noted |
| 3 | After the hand-overs of note 2, a regular attack no longer switches the fans off, and the fans are off on one screen and on on the other. | From the session's logs (kept as build/play1_p1.log and play1_p2.log): the build is two hand-glued fans; player 1's game runs it. Player 2's hits arrive as "wanted off" and player 1's game sends the set-off to both fans every time ("they were 11, 2 set"), yet it goes on reporting both fans on. So the set-off message is sent and does not take, in the game that runs the build. In the gate runs of 28 the same message did take, on an Autobuilt Fanplane. What differs for this build (hand-glued, no wing, handed over twice) and which rule of the on/off node puts the fans back on is not read. That one screen shows them off while both logs read "on" is not explained either. | noted |
| 4 | A lone spring went out of step: it worked at first, then one game's spring flipped back and the other's did not (one extended, one compressed). the tester reads it as a sign of a wider fault in how Zonai devices are handled, not a spring fault. | From the logs (build/play1b_p1.log, play1b_p2.log): a spring by itself is not in the new core at all. coop_ctn.c only takes groups of two parts or more, so a lone device is still a per-object copy handled by the older coop_bonds.c, which follows it by a toggle ("turned off here (1 of 1 devices switched)"). Two general gaps meet here, both already in the plan and neither built: one-part entities (6.8), and every device's internal state (8.4), which is synced for none of the 26 kinds; only on and off travel. A spring's on/off is an edge that fires it and its extension is state, so a toggle that follows a state can leave the two games one flip apart. The toggle-as-follow fault was removed from builds in 28 and is still how lone devices work. | noted |
| 5 | p2 tried to attach a log to p1's stake. It did not work, it glitched the other player's game too, and the log is now stuck and cannot be moved. | From the logs (build/play1c_p1.log, play1c_p2.log): the log is a map tree trunk that p2's game moves ("I move ... Trunk"), the stake is p1's object, and each game holds the other's as a per-object copy. The glue went through the older coop_glue.c, not the new core: p2's game made bond 2 between its trunk and its copy of the stake, and p1's game made the same bond again between its copy of the trunk and its stake. No registry entry was made for the pair. The result is one glued pair whose two halves are each placed by a different game's object sync, so neither game can move it. This is the case the plan's structure phase exists for: a bond between two players' things has to make one build with one runner (6.4, 6.7), and coop_glue.c is to stand down for it (14). Not built yet. | noted |
| 6 | A three-wheeled car with a steering stick, driven by the player who made it (p1). (a) After p1 got off, the wheels spin forever on p2's screen. (b) The stick does not move on the watching screen, though the driver's body moves as if steering. (c) The driver's figure lags behind the machine on the watching screen: it stays with it roughly but floats when the machine bounces over rough ground. | From the logs (build/play1d_p1.log, play1d_p2.log). (a) The car is 8 parts, 4 with a switch. p1's game made and ran it; then p2's game logged its own player touching it, asked to run it and was handed it; when the logs were taken p2's game ran it and reported all four switched parts on, and p1's game followed with them on. Nothing switched it off after the rider left. Not known: why p2's game saw its own player touch the car while p1 drove (the tester: each game had its own pad and p2's Link was nowhere near the car, so p2's game was wrong: it read its own player as holding or steering a build that the other player was driving, and took the build from under its driver. The test is any_held in coop_ctn.c: a part with the Ultrahand-held bit, SPR +0x20 bit 16, or a stick whose rider is this game's own Link. Which of the two read true in p2's game, and why, is not known: how the stick names its rider when the rider is another player's stand-in, and whether anything sets the held bit on a copy, are to be read. The final logs add two facts: p2's game logged the touch and "let go" in the same moment, a few seconds after its copy was made, so the false touch lasted about one tick; and p1's game, from then on a follower of its own build, never logged its own player touching the car while p1 drove it, so it never asked for the build back. Both readings of any_held were wrong in this session, in opposite directions), and what the game does to switch a build off when its rider gets off and why that did not happen in the game running it (the group's "everything off" request at stick release, R/sync-v3/onoff/c_group.md, is the path to read). (b) The stick's tilt is device state (8.4), not sent; only on and off travel. (c) A rider's stand-in is placed from its own player's stream, not on the local copy of the part it rides (9, ride info), which is not built. | noted |
| 7 | The play session ended with player 2's game crashing (found in its log; not something the tester reported). | build/play1_final_p2.log: "45cfc61b is not whole here (0 members, 2 parts): made again", and in the same moment an access violation reading address 0x50 inside main+0x63baa8, the delete request, called with an actor and reason 3. The chain is in coop_ctn.c: the repair rule of 27.3 R3 found a followed two-part build whose parts read as no group at all, queued a remake, and make_pass asked the game to delete each of the old parts without first checking that the actor still exists (part_delete has no such check; the older code it was moved from had none either). The parts were already gone, so the delete request read through a dead actor. Two things to settle from the death-delete reader's output before writing: the read that says an actor is safe to pass to the delete request, and why a followed build's parts vanish (the build was player 1's, far from player 2 at the time: an unload is the likely reading, not proven), since a build that unloads in a follower must not be "repaired" at all (10.3). | noted; a crash, first in line |

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

## 33. Milestone 2 as written (nothing in this section has been run)
Each entry: what the code does now, what it was written from, and what a run has to show. Added as each piece lands.

### 33.1 The delete crash and unloads (31, note 7) - coop_ctn.c part_delete, parts_delete, authority
- From: R/sync-v3/m2/p4/death-delete (the delete request reads [actor+0x1a8] first; the game drops that object after
  state 7; reason 0 is "delete this actor", reason 3 the far delete; a held lock lane makes the request return 0).
- A delete is asked only of an actor in state 2 to 6 whose lock object is there, with reason 0. A refusal is not an
  accepted delete: the part is asked again the next frame, and the new version is not made until every old part's
  delete was accepted. An accepted delete is not asked twice.
- Each entry keeps the id of every part's actor (actor +0x10, the number an ActorLink compares). A part that has left
  state 2 to 6, or whose id is another's now, is taken out of the entry at the next tick, so the drive, the device
  follow and the lists no longer read or write through it.
- A followed build with no part left here while this game's Link is more than 30 m from it is an unload (10.3): the
  entry is kept without parts and nothing is made there. When the Link is back within half the distance it was at,
  the entry asks the runner for its version again (PULL) and the build is made from it. Nearer than 30 m, parts that
  all went are made again by the repair rule as before.
- Not read: who holds the delete lock of reason 0 on a part (32.2). If it is held for long, the log line "the game
  refused the delete of a part here" repeats every 5 s and the remake waits.
- A run shows: no crash after a build's parts unload in a follower; the line "unloaded, not made again until back";
  the build back after "its version asked again".

### 33.2 Green as where the build runs (31, note 1) - coop_ctn.c part_green, record_pass, make_pass
- From: R/autobuild and R/sync-v3/m2/p4/part-maker (SPR +0x150 bit 0, set only by main+0x1c07114 for a part the
  confirm made from zonaite).
- The runner reads the bit of each part when it records the build and sends the mask with the version; a change of the
  mask is a new version. The copy maker sets green only on the parts whose bit is set, at the confirm; the second
  "green once grouped" pass is gone.
- Not read: whether the broken-off kill main+0x271190 tests green. A copy part's death by a game rule is refused by G4
  either way.
- A run shows: two fans glued by hand are not green for the other player; an Autobuild made from zonaite is.

### 33.3 Structure in place: the design (not written yet)
Worked out from 32 and from coop_ctn.c as it is; replaces "stable uids given by the runner" and "structure as events".
- **A part already has a name every game shares.** A version carries, per part, the game and object number the part
  began as (`ids`, `oinc`), and record_pass keeps them through every later version and hand-over (origin_of). That pair
  is the stable uid; no new number is needed. The one hole: a part made here from a record whose entry is gone has none.
- **A new version is applied in place when every part of it is found here.** For each part of the incoming version the
  follower looks for the actor it already has: in this entry, in another entry (a merge or a split), or in the object
  system (the mirror of that game's object, or this game's own object). If all are found alive:
  - bonds of the old version that the new one lacks are broken with find_edge / break_edge (32, bond-break);
  - bonds of the new version with no edge here are made with make_bond, queue path, from the relative pose the runner
    sends (32, bond-make);
  - the entry's parts become the actors found; nothing is deleted, no device loses its state.
  If any part is not found (a part newly made from zonaite at the runner, or a mirror not here yet), the version is
  made as today, with the Autobuild build object. The confirm's own "existing actor" entry (state 2) is not used: it
  needs a hologram per part and the actor already standing in place, and it sets the device off.
- **The runner sends, with each version, the relative pose of every bond** (inverse(pose A) * pose B of the two bodies,
  read from its live bodies when it records), since the record holds part poses, not the poses of bodies other than
  the main one.
- **A part in a build belongs to the build, not to the object system, in every game.** Today a part glued on in one
  game is made a second time in the others while their mirror, or the other player's own object, stays (31, note 5).
  Needed in coop_players.c: (a) a mirror taken into a build is released to the build, not deleted; (b) a game whose own
  object another game glued into a build stops sending it as its object without the others deleting theirs; (c) a part
  that leaves a build becomes the runner's loose object, and the others' actor of it is given to the mirror of that
  object, or deleted and made again as a mirror where that cannot be done.
- **Lone devices (31, notes 4 and 5)** then enter the same way: a one-part entity is an entry with one part and no
  bond, followed and switched by the core; coop_glue.c and coop_bonds.c stand down for registry parts.
- Still to read before (a) to (c) are written: our own mirror code in coop_players.c (the slots' `mir` table, the
  claim at its line with coop_contraption_owns, prop scan, coop_obj_find / coop_obj_ident).

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

## 36. Milestone 2 before its run (2026-11-09)
Written and built, none of it run: 33.1, 33.2, 34.3 (all entries), 35.1 (all entries) and the entries after it.
Each says what a run has to show.

Not written:
1. A part of one player's glued into another player's build (31, note 5; the last part of 33.3). The older claim code
   in coop_glue.c and coop_bonds.c has to be read and retired for this, and the object mirror changed.
2. Wooden parts burning and losing life the same in both games (35, part-status-damage). Needs a read of how a status
   is written.
3. The paying player's own empty gauge switching off a build another game runs.
4. The blast of a time bomb shown on the copy, and any death replayed with its kind.
5. A loose object glued onto a build applied in place (today: made again with the build object, as before).

Known limits of what is written, to watch for in the run: the stand-in hit rule when both players hit one device in
one frame; the stick's tilt flickering if the copy's own graph writes over it; the record's body number taken as the
physics slot for second and third bodies; the order in which TOTK_COOP_DEVICE_SYNC is switched off.

## 37. Play-test notes of the milestone 2 build (the tester, 2026-11-09 late; logs build/v143/logs/totk.log = player 1, build/v143play/logs/totk.log = player 2; one gamepad moving both games)
Recorded, not acted on yet (rule of 31).

**8. Two fans fused by hand: no green on the other screen (note 1 fixed, seen).**

**9. Fusing and breaking off reach the other game slowly.**
- Logs: player 1 glues at line 1510; its record of the build is taken at 1519; player 2's copy is "made, 2 of 2 parts"
  at 1611. In between player 2's two mirror copies are deleted and the build is made new with the build object.
  The older glue path also bonds the two mirrors first (player 2 line 1555), and those are then deleted.
- Known causes in the code: the runner records builds every 2 s; a first fuse is two loose objects, which the
  in-place path does not take (36, item 5), so it is delete and make; the maker's last step waits 3 s before the
  copy counts as made.
- Unfusing: player 1's build "is no build any more ... retired" comes 3 s after the group is gone (the wait in
  record_pass), then player 2 deletes its copy and the mirror copies come back.

**10. After player 1 unfused the fans, player 2 could not move them with Ultrahand; after player 1 fused them again
it could.**
- Logs: after the retire player 2 has plain mirror copies of player 1's two fans (lines 1717, 1718). Fused, the
  build is taken over by touch as designed (player 2 lines 2937 to 2940).
- Not known: whether a loose mirror copy of another player's object could ever be moved by Ultrahand in this build,
  or whether that went through the older claim code and broke now. To be read before anything is changed.

**11. Player 2 cannot unfuse the fans: they come apart for a moment and join again, "as if magnetized".**
- Logs: player 2 runs the build by its touch (change 2, line 2939) and no line shows a new version, a retire or a
  repair there; player 1 then takes it (player 2 line 3193, player 1 lines 3313 to 3315: "touched ... held here"),
  and player 2 again (3267 to 3270). With one gamepad moving both games both Links grab at once, and the build
  changes hands each time.
- Not known: what joins the parts again. Candidates, none confirmed: the older glue path making player 1's bond
  again on player 2's parts (it does so once at line 1555); the other game, whose copy is still whole, taking the
  build and its state winning; the game's own rule for bonds between two parts made by a confirm. To be read.

**Added to notes 10 and 11 after the tester's answer and a read of our own coop_glue.c (2026-11-09, late)**
- the tester moves one game at a time; the two gamepads are used only at the title screen. Each hand-over in the logs
  lines up with the window that had the focus (player 1's log: "window gained the focus" at 22:38:17, then its
  touch; player 2's at 22:38:22), so those were his own grabs and the hand-over worked as designed. "Both Links grab
  at once" is withdrawn.
- **The likely cause of note 11**, read from coop_glue.c's own header and its retry code: the older glue sync still
  runs beside the core. It refuses a break of a bond it made again here from another game's glue, and it makes that
  bond again once a second for as long as its maker still lists it. Player 1's own fans stay fused in player 1's
  game, so player 2's game puts the bond back on its parts (found through coop_obj_find, which answers with the
  core's made parts). Player 2's log line 1555 is that path making player 1's bond. Not confirmed by a run with
  TOTK_COOP_GLUE=0.
- The same path is the likely cause of note 5 (the log on another player's stake).
- Note 10 (a loose copy of the other player's object cannot be moved with Ultrahand) is not explained yet.

## 38. Where the whole plan stands, and what comes next (2026-11-09, late; agreed with the tester in conversation)

**Decision: the older glue path is retired, not rewritten.** Two systems decide the same thing: coop_glue.c syncs
"this bond exists" between two objects that each keep their own owner, the core syncs "this is one build, run by one
game". The replacement is one rule in the core: two things glued are a build, whoever owned them.
- A loose object glued on, this game's or another's, is taken into the build in place (no delete and make).
- The object mirror stops moving an object, and does not delete it, while it is a part of a build, in every game.
- A part that comes off is a plain object again, owned by the game that ran the build.
- coop_glue.c and the bonding half of coop_bonds.c are then switched off from the core. coop_bonds.c holds another
  session's uncommitted changes in this checkout and is not edited.
- The fixed waits go at the same time: the 2 s between records, the 3 s before a made copy counts, the 3 s before a
  build that is gone is retired (note 9).
- To read first, all our own code: coop_glue.c, coop_bonds.c, the object mirror in coop_players.c.

**Phases (the table of section 20)**
| phase | stands |
|---|---|
| P0 to P3 | done and run before this session's milestone 2 |
| P4 structure | splits, merges and pull-offs written (in place); the glue rework above is what is missing |
| P5 devices | mostly written (34.3, 35.1 and after); missing: wooden parts burning alike, the time bomb's blast on the copy, the payer's empty gauge |
| P6 riding, weight, push | only pieces: the stand-in veto (run), rider placement (written); the rest not started |
| P7 lifetime and session | only the unload case (33.1, written); fast travel, leave and rejoin, late join, three players not started |
| P8 a long session | not started |

**Named in the plan's P4 and P5 and neither built nor flagged until now:** vehicles placed by the map and NPC
vehicles entering the registry; spawned things (a cannon ball) as tracked entities of the build. Each is either
written with the P5 leftovers or dropped by a stated decision.

**Order**
1. the tester finishes the play-test list of the milestone 2 build; notes go into 37.
2. The glue rework (finishes P4), with the waits removed.
3. The P5 leftovers (the burning one after one more read).
4. One test of P4 and P5 together.
5. P6, then P7: each read first, written whole, tested once.
6. The long session.

**12. An Autobuild by player 2 never appeared for player 1 ("disintegrated").** (37, continued)
- Logs, player 1: the new build's record came whole and its four mirror copies were deleted (lines 9786 to 9790),
  and no "being made here" follows. From line 9169 on, every 5 s: "6e0aa1ac: the game refused the delete of a part
  here (asked 0x1 of 2): asked again". 6e0aa1ac is the earlier two-fan build; the fan on the ground in the
  screenshot is its part.
- Cause, in our code: 33.1 made a refused delete wait and ask again, and make_pass takes a build waiting to be made
  again before any other. One part whose delete the game refuses for good therefore holds up every later build in
  that game. The delete of that fan was refused for at least 60 s; why (a held lock of reason 0, or a delete already
  asked) is not read.
- Player 2's log: "6e0aa1ac, run here, is no build any more (1 of 2 parts alive): retired" (8799). Player 1 had
  already started to make it again ("not whole here (0 members, 2 parts)", 9168) and never got to the retire.
- Also seen in these logs, working: the stand-in's hit not taken (player 1 9373 and after), a lone device set by
  state (player 2 8805, 8827), a toggle asked across games (player 2 8488 to 8497).
- **Fix after note 12 (not run)** - coop_ctn.c parts_delete, make_pass: a part whose delete the game refuses for 2 s is
  given up and left as it is, and a build waiting on a refused delete steps aside for 0.3 s so the others are made.
  The logs of the session up to here are kept as build/play2_p1.log and build/play2_p2.log.

**13. Breaking an Autobuilt build: a fan (or the wing) "respawns" on the other screen and slowly flies away; glitchy
either way round.** (37, continued; session of 23:0x, logs build/v143/logs/totk.log = player 1, build/v143play = player 2)
- Seen working in the same logs: on/off from either player on an Autobuilt plane; the stuck-delete fix (every build
  was made); a part pulled off applied in place ("27601be7 version 2 applied in place: 3 parts kept, 0 bonds made,
  1 broken", then version 3 the same, player 1 lines 3680 and 3727; player 2 line 3152).
- What goes wrong, from the logs: when a part of a green build is broken off, the game where it runs kills that part
  (the game's own rule for Autobuilt parts). While it is dying it is no longer in the build, and the object sync of
  that game sends it as a plain object ("I move SpObj_... (key ...)", player 2 lines 2373 to 2376, 3023 to 3026).
  The other game then makes a mirror copy of it (player 1 line 3040, 3139 to 3141), and the older copy code makes
  that copy again each time it dies there ("the copy of autobuilt ... came apart here (state 7) ...; made again",
  player 1 lines 3189, 3218; "came apart here twice in 30 s", 3219). That copy is a live fan, switched on: the fan
  that flies away.
- So the core handled the break; the object mirror and the older copy code then brought the dead part back.
- Same root as the decision of 38: a part that is or was a build's belongs to the core, and the object mirror must
  not show it. The narrow rule here: an actor made by an Autobuild (the game's "imaginary" class) is never sent or
  mirrored as a plain object while the core is on; loose, it is a dying part.

**14. Results of the list of the milestone 2 build (same session; the tester's words, then what the logs show).**
- *Jump attack works.* The logs have no "switch event ... sent" line in either game, so it did not go through the
  event forwarding of 35.1; which change made it work is not known (the on read of 34.3 is a candidate).
- *Springs (and likely every item) go out of step when one player is looking away.* The lone-device path ran: 30
  "lone device N is ... there, ... here: set" lines in player 2's log, one per change, and player 2's hits on a
  copy reached the owner (player 1 lines 8997 to 9048). So the state arrives; what a copy does with it while its
  player looks away is not known. To read: whether the object mirror or the game stops a copy's update (its AI
  calc, its physics) when it is off screen. The screenshot shows the spring extended in one game and retracted in
  the other.
- *The stick does not lean.* Not explained; 34.3 named the risk that the copy's own stick node writes over the
  floats. To read: the order of the stick's graph against the writes, or skip the node on a copy.
- *Wheels stop spinning.* The wheel change of 34.3 held for that.
- *The rider still lags behind.* The ride_place change of 35 did not fix 6c; its cause is still open.
- *A watcher with an empty battery: beam emitters did not turn on; after the battery refilled a hit still did not
  turn them on until new ones were spawned.* Not explained. Whether those emitters were lone devices or parts of a
  build, and which game's gauge the copy read, is to be taken from the logs with the time of the attempt.
- *Cannon, stake, time bomb, beam (items 9 to 12) look right; not tried on enemies.*
- Counted in the logs of this session: stand-in hits not taken, 8 in each game; versions applied in place, 2 and 3;
  no refused delete; no part read as held by someone else.
- the tester on note 14: the player looking away had only turned around next to the spring (not far from it); the beam
  emitters of the battery case were loose, not in a build.
- Read in our own coop_players.c (copy_place, place_copy): a copy of a Zonai device runs its own update, and its main
  body is put at the owner's place every tick; nothing of ours stops a copy that is off screen. A spring's extension is
  its second body, which is not placed. So the off-screen difference is either the game's own (what it stops for an
  actor out of view: not read) or the second body going its own way. To be read before anything is changed.

## 39. The rework: what is to be read, written and shown (2026-11-09, late; ordered by the tester: fill the plan, then read and write)
Rule of 29 holds: nothing below is written before its read is done, and no game is started for a test without
the tester's yes. Every item names the notes it answers.

### 39.1 Reads
Our own code, read by me in full before 39.2 is written:
- coop_glue.c (the older glue sync: its lists, its veto, its retries, what it expects of coop_obj_find).
- coop_bonds.c as it is in this checkout, read only (its bonding, claims, device sync, "came apart" handling).
- The object mirror in coop_players.c: the slots' mir table, arrow_copies, copy_place and place_copy, prop scan and
  report (what is sent as an object and when), the "came apart ... made again" rule for autobuilt copies,
  coop_obj_find / coop_obj_ident, ride_place and how a ride is reported.

Game code, by Sonnet readers into R/sync-v3/m2/p7/ (one folder each):
- offscreen: what the game stops or changes for an actor that is out of view or not drawn (its AI calc, its
  physics, sleeping bodies), and what wakes it (14, the spring out of step when a player turns away).
- spring-bodies: a spring's second body when its main body is placed from outside each tick: what holds the head,
  whether the extension can be set (the joint's target and flags) so a copy shows the runner's (14, 4).
- stick-graph: the order of the stick's nodes against outside writes to its tilt floats, and where the rider's pose
  reads them; what a copy needs so the stick leans (14, 6b).
- gate-stuck: what keeps a device off after its energy gate closed once: every latch the on/off node and the
  DeviceSwitch keep from a closed gate, and what clears them (14, the loose emitters that stayed off).
- delete-refused: who holds the delete lock of reason 0 on a part, and how long (12).

### 39.2 Writes, in this order
1. **One owner for anything glued** (38's decision; notes 5, 9, 10, 11, 13).
   - A glue in any game makes a build in the core at once (the glue hook reports it; no 2 s wait for the record).
   - A loose object taken into a build is taken in place: the mirror copy, or this game's own object, becomes the
     part; nothing is deleted and made.
   - The object mirror: an object that is a part of a build is neither placed nor deleted by the mirror, and is not
     sent as a plain object, in every game; an actor made by an Autobuild is never sent or mirrored as a plain
     object (13); a part that comes off is the runner's plain object again and the others' actor becomes its mirror.
   - coop_glue.c and the bonding of coop_bonds.c switched off from the core (TOTK_COOP_GLUE, TOTK_COOP_BONDS set
     to 0 by the core when it is on, as TOTK_COOP_DEVICE_SYNC is); the old "came apart ... made again" rule off for
     core parts.
   - Moving another player's loose object with Ultrahand (10): decided after the mirror read; either the grab hands
     the object over, as a build's does, or it is said plainly that it cannot.
   - The waits out: record on the event, not every 2 s; the 3 s after a make; the 3 s before a retire (9).
2. **What the reads of 39.1 settle**: the off-screen difference, the spring's head, the stick's lean, the device
   stuck off after an empty gauge, the refused delete.
3. **The rider on a build (6c, 14)**: after the mirror read shows how a ride is reported and placed.
4. **The P5 leftovers of 36**: wooden parts burning alike (one read first), the bomb's blast on the copy, the
   payer's empty gauge, and the two things 38 named (map and NPC vehicles; spawned things as entities), each
   written or dropped by a stated decision.

### 39.3 What the next test has to show (the tester's list, to be run by him)
- Fusing by hand, either player's objects, appears at once on the other screen and can be unfused by either.
- Breaking an Autobuild leaves no stray copy on the other screen.
- A loose object of the other player's can be moved, or the plan says why not.
- A spring, a fan and an emitter stay in step with a player looking away.
- The stick leans; the rider sits on it.
- A watcher with an empty battery sees the other's devices on, loose or in a build.

### 39.4 Read: our own three files (2026-11-09, late; read by me: coop_glue.c in full, coop_bonds.c's header and public functions, the mirror's copy loop, place_copy, copy_place, the object report and coop_obj_find / coop_obj_ident)
- coop_glue.c: a glue made by Ultrahand is caught at doScotch (main+0x1d66e34); its maker numbers it, sends it and
  lists it every second; the others find the two objects by coop_obj_find and glue their own; a bond missing from
  two lists is parted with removeScotch; a break of a bond made from another game's is refused at main+0x1c0bb54
  unless this game parted it for the maker in the last 2 s. The lasting bond of a hand glue is made at Ultrahand's
  release through the bond maker main+0x1916830; the glue entry goes about 2 s later.
- coop_bonds.c: the owner sends the groups its own objects form; the others bond their copies to the group's first
  member, mark them autobuilt, follow device state, and keep claims (who holds or rides another's contraption).
  TOTK_COOP_BONDS=0 and TOTK_COOP_GLUE=0 switch each off whole; with both off their public functions answer 0.
- The mirror: a slot per object the owner sends; a copy is made when wanted, placed at the owner's pose every tick,
  deleted when the owner stops sending it; a copy that dies here is taken as picked up or destroyed and the owner is
  told, except an Autobuilt part's, which is made again. A Zonai device's copy runs its own update. An actor of the
  game's Autobuild class is sent with a flag and made the same way.

### 39.5 Written from it (not run)
- **One owner for anything glued (39.2 item 1)**: coop_ctn.c, coop_players.c, coop_life.c.
  - The core switches the older glue and bond paths off (TOTK_COOP_GLUE, TOTK_COOP_BONDS set to 0 when the core is
    on and the key is not set by hand), as it does the device flips. Not checked: that each key is set before the
    older file first reads it; a launcher that sets the three keys removes the doubt.
  - A bond made or broken in a game (the bond maker, the two break functions) makes that game record its builds
    every 0.1 s for 3 s instead of every 2 s. A build that is no group any more is retired after 0.6 s, not 3 s. A
    made copy counts as made once all its parts are grouped (or after 1 s), not after 3 s (note 9).
  - in_place takes a part no entry has from the objects: the mirror copy of the other game's object, or this game's
    own object when another player glued it into a build. Such parts are marked taken in (Ent.adopt): never deleted
    by the entry, let go when they leave it, their glue broken first when the build is retired. So a first fuse by
    hand, and a loose object glued on, are applied in place.
  - The mirror neither places nor deletes a copy while it is a part of a build (coop_ctn_part), and does not delete
    it when its owner stops sending it; coop_contraption_owns names only what this game makes with the build object.
  - An actor of the Autobuild class is not sent as a plain object while the core is on (note 13).
  - A part of a followed build takes no game-rule death whether it was made here or taken in.
- Not done in this step: a build glued only from another player's objects, with none of this game's own, is not
  recorded by the game that glued it (record_pass starts from this game's own objects); moving another player's
  loose object (note 10) is not decided; the handing of a let-go part back to the mirror relies on the mirror slot
  still being there.
- A run shows: two loose fans fused by either player appear fused on the other screen within a second, with "applied
  in place: 2 parts kept (0x3 taken from the objects), 1 bonds made"; unfused by either, they come apart on both and
  stay apart; a part broken off an Autobuilt build leaves no stray copy.

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

## 42. Review 1 and its repairs (2026-11-10)
Eight reviewers read the whole contraption code (coop_ctn.c, coop_life.c, coop_autobuild.c, the core's parts of
coop_players.c and coop_world.c, coop_ctr.c) and gave 88 findings, about 55 distinct: 1 crash (latent), 28 wrong,
42 risks, 17 minor. Each finding was checked against the code before it was changed. The findings and the triage
are kept with the research notes (sync-v3/review1). Nothing below has been run.

### 42.1 Repaired, by what it does in play
- **The settings file**: the core switched the older glue, bond and device paths off with the menu's setting call,
  which totk.ini keeps after the session; now in the process environment, set right after the settings are read,
  before the older files read theirs (84, 77).
- **A player's own objects are never deleted**: only a part this game made from a record is deleted; objects taken
  into a build are let go (1, 2).
- **Versions are not lost or mixed**: in place commits the version it read; a record is used only while whole; a
  RETIRE or RELINQUISH that comes while a build is made is kept; a new runner numbers on from the highest version
  heard (3, 15, 29, 38, 39, 40, 70, 71).
- **Part numbers land on their own version**: state, events and deaths carry or are tested against the version
  (17, 59).
- **Walking away**: a build unloaded part by part where it runs is handed to a game that still has it, not shrunk
  version by version and retired; a destroyed build is retired and a follower does not make it again; the repair
  rule waits for every part to be there (36, 37, 44).
- **Hits on map devices**: a stand-in's hit is refused only on devices the core syncs (85).
- **Lone devices**: the five-a-second refresh is read from the channel's latest-only slot, so a copy made later is
  set and the entry stays (14, 58, 69, 67).
- **Builds the writer leaves a part out of** are sent, without that part and its bonds (48).
- **Breaking**: an edge glued only by its second flag is broken with the call's fourth argument; a break records
  the builds at once as a bond did (49, 68, 79).
- **Splits**: every piece is read before any entry changes, the piece with most parts keeps the identity, the
  others keep their origin numbers (47).
- **Who pays**: a payer whose copy cannot run on its battery (range, substitute window) asks the build off or leaves
  those parts off (61).
- **Two deaths of one part** are both sent (63, 87).
- **The maker**: "later" is bounded to 3 s; a version partly here waits for the missing part instead of making
  twins; bonds are checked before any is queued; one edge per record bond; a timed-out making is asked again; the
  entry is marked as being made at the moment it is picked (4, 5, 6, 7, 8, 10, 41).
- **Threads**: retiring and orphans run at phase 0 beside in place; phase 0 reads the tick's copy of the object
  tables and finds objects through the tick; floats are written after the lock is let go (9, 23, 30, 32, 52, 72,
  73, 74, 76, 81).
- **Capacity**: the gate lists and the made-part table hold every part the registry can; G1's body list has no
  gaps; the queues are larger and say when full; a build sent while the registry is full is asked for again;
  wheel slots are taken back (19, 20, 24, 26, 31, 60, 65, 75, 80, 82, 83).
- **Stale pointers**: queued sets, events and deaths are checked against the same actor; on/off and fuse nodes are
  used only while their actor lives; the node table index stays unsigned (25, 27, 28, 66).
- **Three players**: of two games that take a build with the same change number the lower relay id keeps it; every
  holder takes a build whose runner left (13, 21, 45). A game given a build whose copy is not whole offers it
  back (42).
- **Smaller**: 11, 12, 16 (the channel's first stream to a player), 33, 34, 35 (the maker's part list), 46, 50, 54,
  56, 64, 88.

### 42.2 Not repaired, and why
- 62: at the runner, the group sender's own "cannot pay" off uses the runner's gauge while another player pays. It
  needs a hook on the gauge test main+0x1fa4bfc for the sender's call (return address main+0x32f500); not written.
- 78: G1 never matches main+0x29a4afc (its x0 is the body table, the body comes from a handle). Covered today by the
  force-node rule; to fix with P6 (41.2 point 5).
- 51: body slots beyond 0 to 2 and the record's body numbers (body +0x1e8 bits): to read before changing.
- 18: a failed send is not retried (the channel reports success if one peer took it); 22 and 86: guest calls and
  memory reads on hot paths (cost, not correctness); 43: a build first heard of is made wherever it is.
- 35 (second half): the death kind from the wire is not range-checked; the table's size was not read.
- 53, 55, 57: facts the comments use without their research tag; to mark.
- The three-player rules for toggles and events sent to all (41.3 point 6).
- Hooks on game threads still ask coop_is_copy, and phase 0 still calls coop_deleted_here; the object-table copy
  covers record_pass, in place and the springs only.

### 42.3 What the next run has to show, beyond 39.3
- totk.ini has no TOTK_COOP_GLUE, TOTK_COOP_BONDS or TOTK_COOP_DEVICE_SYNC line after a session with the core on.
- Walking away from a build: "is being unloaded here ... offered" at the runner, "run here now" at the other game,
  and no RETIRE.
- Unfusing a build made of both players' objects: "applied in place" with no "made again", and no twin parts.
- A loose emitter of the other player's, turned on before it was copied here, is on here.

## 43. Play-test notes, run 4 (2026-11-10, after the review repairs)

**Note 15: a glued build taken by the other player flies apart.** the tester glued three fans together (P1's own objects),
switched them on and off from each player (works), then picked them up with Ultrahand on P2. They went wild, flew off
and destroyed themselves.

What the logs show (play4 logs kept as build/play4_p1.log, play4_p2.log):
- P2 took the build in place from the copies of P1's fans (applied in place, all three taken from the objects), was
  asked to run it on the grab and was given it (change 2). The touch ended in the same tick.
- From then P1 followed P2's state. P1's follow log shows the error growing to 2 to 3 m with the root velocity
  swinging between about +60 and -65 m/s every half second: a loop, not a lag.
- 4.5 s later P2 found its copies in no group (3 of 3 parts alive) and retired the build as broken apart; P1 then let
  its fans go, minted them as a new build and retired that one as broken apart too.
- Not known: what moves P2's copies against P2's own run of them. The object mirror's receiver leaves build parts alone
  (coop_players.c, both places). Candidates to read, not to guess: whether P1's game still sends its own fans as
  objects (or as held by P2) and something places them, and whether P2's copies (another player's objects, made by the
  mirror) are still written by the mirror or another path while P2 runs them.

**Note 16: a spring P1 stands on.** P1 made a spring and stood on it. On P2's screen the spring pressed in under P1's
stand-in (the tester: it should not), and when it was hit, it did not spring out on P2's screen.

What the logs show:
- P2's game says "I move SpObj_SpringPiston_A_01 (key 6a6e45ab)", and P1's game says "a co-op player moves
  SpObj_SpringPiston_A_01 ..., not updated here: a copy". So the object mirror handed P1's spring to P2 as a moved
  placed object (keyed), and from then P1's own spring was a copy of P2's. Why P2's game took it as moved by its player
  is not known (the stand-in standing on it pressing it in P2's game is a candidate, not shown).
- The lone-spring sync (state sent by the owner, set on the copy) then ran the wrong way round: P2 owned the spring
  P1 stood on.
- Off-screen gate (note 14, the line that was awaited): a lone device of P2's at P1 switched between "update run" and
  "not run by the game (blocked 1)" several times, and once to calc mask 0x2e041. The blocked bit (actor +0x5ac bit 6)
  does toggle on a followed lone device.

**Run 4, the tester's list (39.3 and 42.3)**
1. Fusing by hand, not reached: objects spawned from a capsule by one player cannot be picked up with Ultrahand by
   the other (note 10 again, now for loose capsule objects). the tester: either player moves any object, as their own;
   this was never open. To do: read why a copy of the other's object cannot be grabbed, then write it.
2. Breaking an Autobuild from either player: no stray copy. Works.
3. Walking away from a build: the other player kept it and it worked (how far is not known; check the log for
   "offered" and "run here now").
4. Looking away: the spring still goes out of step. The awaited gate line came (note 16): the game blocks a followed
   lone device's update on and off (actor +0x5ac bit 6). The fix is to read what sets that bit and write against it.
5. Empty battery watching: the other's devices show on. Works. **Note 17:** the player with the empty battery can still
   switch a device on by hitting it while the gauge recharges, and the other player's battery then pays. In the game a
   player whose gauge cannot pay does not switch a device on; a hit here that asks the runner or the owner to switch on
   should be refused while this game's gauge is blocked (gauge_blocked) and the device would be on this player's bill,
   and the payer rule should not move the bill to a player who could not have switched it on.
6. A loose emitter turned on before it was copied: not reproducible, the copy appears at once.
7. **Note 18:** the stick now moves on the copy. The rider still lags behind the build (no cause yet; P6, 41.2).

**Note 17, written (1133e2c, not run):** a hit that would switch on a device another game runs is not sent while
this game's gauge is blocked (gauge_blocked: manager +0x50 bit 0, or +0xe8 bit 0 clear). Not shown by reading: that
the blocked bit stays set while the gauge recharges after running empty.

## 44. Objects: one owner, handed over by a grab (notes 10, 15, 16; read 2026-11-10, R/sync-v3/run4/, four folders, all read in full)

### 44.1 What the readings established
- **Note 10 (grab, run4/grab):** nothing refuses a grab of a copy. The object mirror puts the copy back at its owner's
  place every tick (place_copy: actor place, model, body velocity, a teleport once a second), and prop_fn skips every
  copy, so a move of it is undone and never reported. The mirror had no way for an object to change owner; builds
  have one (TAKE/GRANT), loose objects did not.
- **Note 15 (takeover, run4/takeover):** the loop that threw the build apart was the mirror's keyed path, not the
  core: P1 kept sending its glued fans as moved objects; P2's props_receive found its copies of them by key and drove
  them with force_to toward P1's places. While P2 followed, G1 dropped those writes; once P2 ran the build they
  landed, and the two games chased each other's positions one network delay apart.
- **Note 16 (springowner, run4/springowner):** "moved by this game's player" was any change of place over 1 cm, with no
  cause; a device made at run time has a different key in each game (name plus where each game first saw it). So a
  game took a spring it had as moved by its own player and sent it by key; the other game, finding no actor with that
  key, made a copy of it, and from then the spring's state ran from the wrong game.
- **Note 14 (blocked, run4/blocked):** "blocked 1" on the lone devices was the game-wide pause of a menu open in that
  game (every entry is followed by "this Link is paused"), not the camera. The owner's devices stop the same way in
  single player: nothing to unblock; the copy is set again on resume (the lone set every 0.5 s, spring_apply). A
  second thing, a calc mask 0x2e041 that comes and goes on single devices with blocked 0, skips the device's whole
  update; what sets it is not read. One log line at the next entry into it (the actor's calc-reason tree, +0x4b0,
  +0x440, +0x5ac) would name the key.

### 44.2 Design (written as one change in coop_players.c and coop_world.c)
1. **A build's parts are the core's.** The mirror neither sends (report_props) nor moves (props_receive, props_hold) an
   actor that is a part of a build known here (coop_ctn_part) or a device followed here (coop_ctn_follows). A part that
   leaves its build is a plain object again: sent again if this game made it.
2. **Moved by this player means a cause.** A placed object (one with a map placement) moved here is sent by key only
   when this game's Link holds it, or when no co-op player's stand-in is within 3 m of it (a stand-in standing on it or
   walking into it moves it in its own game, which sends it). An object made at run time (no map placement) is never
   sent by key: it is sent by its maker's number, or handed over (3).
3. **A grab hands a loose object over.** When this game's Link holds a copy of another game's object (SPR +0x20 bit 16,
   the holder this Link), this game:
   - takes it as its own made object under a new number of its own, and sends it from then on;
   - frees the copy's mirror slot without deleting the actor, and does not make that object again while its old owner
     still reports it (taken list, 10 s);
   - tells the old owner (an OBJS packet with header word 2: the old number, the old owner's id in flags, the new
     number in the entry's first position word).
   The old owner, told, stops sending its object and makes its actor the copy of the new owner's new number (a mirror
   slot taken with that actor, not made anew), kept 2 s while the new owner's first report comes. Both ways, any
   number of times. A keyed object (a map object a game moved) is handed over the same way, by its key.
4. **The calc-mask log line** of 44.1 is added to the lone gate log (no change of behaviour).

### 44.3 Play-test notes, run 5 (2026-11-10)
**Note 19: three fans spawned by P1, two seen by P2.** P1's log has "I made SpObj_WindGenerator_A_01" for numbers 1 and 2
only; P2 made copies of those two. The third fan was never taken as P1's made object, so it was never sent. The made
gate (report_props, prop_fn) takes a new object as this game's only if it was first seen within 6 m of this Link and
nearer this Link than any stand-in (within 1 m), with no copy pending within 1 m, at most 20 new objects near, after 20 s
of world time and not within 10 s of a warp. P2's stand-in stood beside the fans; which test refused the third is not in
the log. Before 44.2 an object refused there was still sent by key once it moved (it reached the other game as a copy
of a key); 44.2 item 2 stopped run-time objects being sent by key, so now it is not sent at all until someone grabs it.
To fix: a run-time object (no map placement) that is not a copy here exists only because this game made it; it should
be this game's made object without the nearer-than-a-stand-in test. Log which gate test refuses a candidate.

**Run 5, the hand-over:** works. P2 grabbed P1's fan: P2 "the copy of SpObj_WindGenerator_A_01 (00000002) is held here:
this game's now (number 1)", P1 "player 2 took my ... it is theirs now (their number 1), mine follows it". P2 could then
move P1's objects.

**Note 20: P2 cannot fuse P1's objects.** P2 grabs P1's things but cannot glue them. P2's log has no contraption line
at all after the grab (no version, no build), so no glue group formed in P2's game. Not known: whether the glue is
refused when one end is a copy of the other game's object, or forms and is torn at once (the copy is placed by the
mirror every tick: place_copy sets its body's speed and teleports it once a second). To read: the glue maker
(doScotch main+0x1d66e34, the bond maker main+0x1916830) on what it refuses, against what place_copy does to a copy.
  the tester: the glue line shows while the object is held, and pressing Attach does nothing. In his picture the target is
  a pair of P1's fans (outlined orange), so likely a build of P1's that P2 follows; whether the attach is refused by a
  rule of ours on a followed build's part (the break and bond hooks of coop_life.c, G1, G4) or by the game for a copy is
  not read.

**Note 21: a device whose game is in a menu.** P1 opened a menu while a device of P1's ran. On P2's screen it kept
running, and P2 could not switch it off until P1 left the menu. P1's game runs the device and is paused, so the asks
from P2 wait there (the pause of 44.1). In single player the paused game stops everything, but here the other player
plays on. A game in a menu (the heartbeat's paused flag, COOP_CTR_HB_PAUSED) could hand the builds and lone devices near
the other player to that player's game; not designed yet.

### 44.4 Written after run 5 (not run)
- **Note 19:** a Zonai device with no map placement that is no copy is this game's made object without the
  nearer-than-a-stand-in test; a new object not taken is logged with the test that kept it.
- **Note 21:** while the game that runs a build is in a menu (heartbeat paused flag), a game whose Link is within 60 m
  takes the build after 1 s; a hit on a lone device whose owner is in a menu takes the device over (as a grab) and
  switches it here.
- **Note 20 (read: R/sync-v3/run5/attach):** none of our hooks refuses a glue. Strongest lead: a copy's body has +0x68
  bit 24 set, which keeps a bond out of the game's graph (main+0xa2d39c answers no node), so the glue made no build.
  The glue tick hook (main+0x1d67060) now clears that bit on the bodies of live actors before the tick, and logs how
  each glue ends (made, timed out, aborted by a test).
- **6.7 edit requests, written:** a glue this game's player finishes (BondMaker called from the glue tick, return
  address main+0x1d677a8) with one end a part of a build another game runs is sent to that game (EDIT_REQ: the build,
  its version, the part and slot, the other end by part or origin number, the relation of the two bodies); that game
  makes the same bond in place with its build object's bond queue at phase 0, even while its own player holds the
  build, and records the new version. Not written: the gluer breaking its edge when the runner refuses.

**Decision (the tester, 2026-11-10):** map-placed and NPC vehicles and spawns as entities are left out of milestone 2.
NPC vehicles are to be looked at in a play-test; if they look wrong, they get done.

### 44.5 To test later (the tester, 2026-11-10: not run now)
Everything since run 5 (66a2a23, e5439fc), and 1133e2c's empty-battery rule:
- P2 glues its object onto a single loose object of P1's; onto a build P1 runs; and onto it while P1 holds it.
- Three fans spawned with the other player's stand-in beside them: all three on both screens.
- A menu opened in one game while the other player is near one of its builds or loose devices: the build is run by
  the other game after 1 s, a hit on the device switches it.
- Hitting a device with an empty or recharging battery does not switch it on, and the other battery does not pay.
- An NPC vehicle, if one comes along (the decision of 44.4).

## 45. P6: riding, weight, push (read 2026-11-10, R/sync-v3/p6/, four folders, all read in full; written, not run)

### 45.1 What the readings established
- **Rider lag (riderlag):** the stand-in is placed from its owner's state 60 ms behind, the build at its runner's
  present, so it trails by about 0.06 v. The fix that puts it on the ridden part (ride_place) was almost never used:
  the rider's game listed only loose copies as ridable, so a Link on a build sent no ride; and the build lookup sat in
  a branch two players never reach.
- **Pose (pose):** the stand-in replays its owner's animation slots, so it takes the owner's riding pose with no state
  of its own. It cannot be put in a stick's seat (its pack drops the Rider component), and should not be: the stick
  would see a driver here and switch devices on a stand-in's say. Not shown: that a stick ride's clips fit the 16 slots.
- **Controller (controller):** the stand-in most likely has no character-controller record; nothing of ours stops one.
  Its body is made static and teleported to the owner's delayed place every tick. What the local Link stands on can be
  read from its contact list on the tick, with no hook.
- **Mass (mass):** the contact-mass scale is a per-contact pair of mass factors, applied only between two Dynamic
  bodies and, for a Link, only when it pushes (0.1: 1 kg), never when it stands. Standing, a Link meets a build with
  its real 10 kg: the load is m = 10 m_part / (10 + m_part). Making the stand-in a Dynamic character body risks a null
  read in the game's foot-band test (main+0x2a86318) when its controller holder is missing.

### 45.2 Written (not run)
- **The rider on the build:** build parts are listed as ridable at their place here and named as the other games know
  them; the viewer's game finds its actor of the ridden part on the tick (all cases) and puts the stand-in on it at
  the part's place now (b4642f3).
- **The rider's weight:** the game that runs a build adds a co-op rider's weight (the derived m times g) at the
  stand-in's feet on the ridden part's body, main+0x29ae7a8, once a frame. Exact only at one physics step per frame
  (not read). The stand-in stays static and out of contact with builds (I16).
- **Not written:** a Dynamic stand-in that touches the build (crash risk above); a walking rider's push; the stand-in's
  facing on the part (still from its delayed state); "what the Link stands on" from the contact list (the 1.6 m test
  stays).

### 45.3 To test later
- A player standing on and riding a build the other game runs: on the build on the other screen, not trailing.
- A player standing on a plank or a balloon build the other game runs: it dips under them as under that game's Link.
- A stick ride: the stand-in's pose on the stick.

## 46. P7: lifetime and session (written from R/sync-v3/m2/p8/unload-travel and limits; not run)

### 46.1 Written
- **Back in reach by the parts' own radius (41.3 item 1):** each tick a build's entry keeps the smallest delete radius
  of its live parts here (ActorInfo DisplayRadius + DeleteRadiusOffset through [[actor+0x370]+0x18]+0xc8, +0xac and
  +0xa8: 60 m for capsule items, 90 m for a fan, 130 m for a wing, 310 m for a spring). A follower whose copy the engine
  unloaded makes it again once its Link is 20 m inside that radius; with no radius known, within half the distance it
  went at, as before.
- **Fast travel against walking away (41.3 item 2):** both end the same way here, by design: every part deletes
  itself (reason 3) once the observer is beyond its radius, the game that ran the build hands it to a game that still
  has it (36, 41.1), and a follower marks the build absent. Nothing in the core needs to tell them apart; not written.
- **The tables' fill (41.3 item 5):** every 10 s, "tables: builds n of 24 (n run here), parts, made here, device sets
  queued, edit requests". The game's own pools (bond slots, the actor message ring) are not read here.
- **Device sets spread over frames (41.3 item 5):** at most 40 set messages a frame; the rest wait for the next frame,
  unless a newer set for the same build has been queued meanwhile.
- **Three players (41.3 item 6):** toggles and switch events go to the game that runs the build, not to every game;
  two games that take a build at once settle on the lower relay id (42, findings 13, 21, 45).

### 46.2 Not written
- A game that leaves and joins again under a new incarnation: its old builds' entries are not matched to it (41.3
  item 4).
- The game's own pool fill (bond slots, the actor message ring) in the status line.

### 46.3 To test later
- Walking away and back: the build is made again only once well inside its parts' radius, and not deleted at once.
- Many builds switched at once (four or more): every device switches.
- The tables line every 10 s in both logs.

## 47. Review 2 and its repairs (2026-11-10)
Seven reviewers read the code written since review 1 (c929ef5..6bf8cfd) and gave 38 findings (8 wrong, 17 risk, 13
minor, no crash). The findings and the triage are kept with the research notes (sync-v3/review2). Nothing below has
been run.

### 47.1 Repaired
- **Gluing onto another player's object:** the bond queue clears the copy flag (body +0x68 bit 24) on a live actor's
  body instead of refusing it, so an edit request onto an object, and an in-place bond onto an adopted copy, are made
  (29, 34 for that cause).
- **Grabbing a part of another player's build** is no object hand-over: the build's TAKE decides (13).
- **A device taken over while its owner is in a menu** is queued from the hook and taken over on the co-op tick, which
  sets the hit's toggle then (12).
- **The old number after a hand-over** is kept from being made again while its old owner still sends it, up to 60 s
  (14).
- **Riding:** build parts are listed only within 4 m and only alive; a part of a build this game runs is not carried
  by ride_carry (its own physics carries the Link); the ridden actor is checked by its id and a miss is cached; the
  stand-in must be alive for its weight; the weight is the Link's 10 kg, from a point within 3 m of the part (5 to 11).
- **Walking away and back:** the smallest delete radius seen is kept; the radius rule only when the build went beyond
  it; a follower that lost some parts to an unload lets the rest go and waits as absent; the runner's unload offer
  lasts 5 s, then the smaller group is recorded (1, 2, 24, 25).
- **Hand-overs:** the equal-change tie-break at every game that holds the build; a RELINQUISH that comes while the copy
  is not whole is kept and taken up only if the same game still runs it; it is dropped when the runner changes (16, 27).
- **Messages:** toggles and events fall back to all games when the runner's route fails; a queued event keeps its
  actor's id read under the lock; queued device sets keep their actors' ids; edit requests check their slots (3, 17,
  23, 32).
- **Queues and tables:** the 40-message cap counts an entry's own messages; full queues are logged; the event-seen
  bound is the queue's; the abandoned table, when full, forgets at once; the object lookups have 128 slots and never
  evict a fresh one; a record that finds the registry full says so; the death window is 10 s over 128 entries (4, 20,
  22, 26, 33, 38).
- **Smaller:** owns() matches the old version's ids too during a remake's wait; "later" and the in-place give-up reset
  when a new version is armed; a lone refresh older than the last reliable change is not taken; the wheel table is
  locked (18, 21, 35, 36).

### 47.2 Not repaired, and why
- 15: two games each taking the same unplaced device near a dispenser: a dispenser is used by a Link in its own game,
  so both games do not spawn one; left as it is.
- 19: a gate list read while it is rebuilt can miss a part for a frame; left.
- 28: an explicit totk.ini value 1 for the older paths is overridden with the core on, without a log line (the core
  needs them off; ctn_on runs before the log is set up).
- 30: the glue tick clears the copy flag on any live actor; a body torn down on a live actor is not read.
- 31: a hit by something other than this game's Link is refused too while this gauge is blocked (the hit's source is
  not known at that point).
- 34 (rest): bond_queue's other refusal points (no graph id, no descriptor) still come after earlier bonds are queued.
- 37: a glue onto an object copied since the last tick is not sent (the snapshot misses it).

### 47.3 Play-test notes, run 6 (2026-11-10)
**Note 22 (note 19 again): three fans spawned by P1 from the pouch, two seen by P2.** P1: "I made SpObj_WindGenerator_A_01"
numbers 1 and 2 only, right after leaving the menu; no "not taken as made here" line, so the third fan was never a
candidate at all (the 19 fix only dropped the stand-in test). Not known which test kept it out: first sighting, within
6 m, a copy pending within 1 m, the table full, or an existing entry at its address. Written (not built while the
games run): a log line for each new Zonai device saying each of those values.

**Note 23: P2 still cannot glue P1's objects.** P2 took P1's fan ("held here: this game's now (number 1)"), and the glue
ended at once: "a glue ended after 0.02 s, progress 0.00 (aborted by a test)". No "bit 24 ... cleared" line, so the
bodies did not carry that flag (or the actor was not found). One of the tick's first tests refused it (joint or
bodies missing, B's link dead, the joint dead, the group counts, SPR A +0x24 & 0x2380, SPR B +0x150 bit 4, or the
entry released by the body flags 10/26). Written (not built while the games run): the glue's end logs each of those
values.

**Note 24: an Autobuild by P2 never reached P1.** P2: "a group of 8 members, record of 8 parts: a part is no object the
other games know: not sent" (twice). Cause, in our code: review 1's repair of findings 23/30/72 made record_pass name a
part through the tick's copy of the object tables (objs_ident), which holds only the objects the mirror sends and the
copies; coop_obj_ident's last rule (any named actor: this game's, by its place key) was left out, and an Autobuild's
parts are no object the mirror sends. So no Autobuild has been sent since that repair (bcc0b2f). Written: objs_ident
falls back to the place key as coop_obj_ident does (not built while the games run).

### 47.4 Play-test notes, run 7 (2026-11-10, logs play7_p1/p2)
- **Note 22 settled:** all fans showed; each new fan was a candidate (first 1, 3 to 5 m from the Link).
- **Note 25: P2's Autobuild never reached P1.** P2's new parts (two fans and a stick) were first seen 7.4 to 8 m from
  P2's Link and not on their first sighting ("first 0 ... candidates 0"): the Autobuild makes them farther than 6 m and
  shows them before it confirms. So none was an object the mirror took as made here, record_pass never had them among
  its candidates, and the build was never recorded (no "run here", no "not sent"). Written (not built while the
  games run): the tick lists this game's own actors that are in a glue group (from the actors updated in the last
  second), and record_pass takes them as candidates, so any build this game made is recorded.
- **Note 26: a glue onto a copy of the other player's object.** Two glues of P2's fan onto a copy of P1's fan ended at
  once; their tests read: joint, bodies, links, SPRs present, counts 1 + 1, both SPR flags clear, body A (the held fan,
  just taken over) +0x68 bits 10/24/26 = 1 0 0, body B (the target, still a mirror copy) = 1 0 1. Bit 26 is set on the
  copy, and the game releases a glue entry unless both bodies have bit 10 set and bit 26 clear (research notes
  sync-v3/run5/attach, 0x1d62f44). One glue worked: onto a fan P2 had taken over before (no longer placed by the
  mirror), and it made a build (e3c3a9e4) that P1 then made too. So bit 26 comes with what the object mirror does to a
  copy each tick (place_copy), and is gone once a copy is taken over. What bit 26 is and what sets it: not read.
