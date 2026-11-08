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
- **Same as the mod: enemies do not fight the stand-in.** TotK Online does nothing to make enemies target its
  PlayerPuppet (category Enemy, affiliation Enemy); each enemy runs in the nearest player's game. Here that is the
  hand-over (coop_world.c handover_to). Making enemies fight the stand-in natively was tried and left off as a test
  (`make_puppet_pack.py --target-fix`; "Enemies and the stand-in" below). the tester's call, 2026-11-07: hand-over.
- **Done, not seen end to end: enemies made at run time** (a night's Stalkoblins) are made in the other games: the
  create queues' getActor (main+0x50d364, 0x61a8b4, 0x6609b4, 0x89841c), an Enemy_ with no placement hash
  ([actor+0x370]+0x88, the same in 1.4.3), sent with its key; the others make one and give it that key. The test
  spawn claims its own actor and never passes the queues, so only the hand-over and the hash were checked.
- **Written, not run in a game yet: what a game holds for the others is let go of 1000 m from every player**
  (2026-11-07; runtime/coop_far.c; `TOTK_COOP_FAR=0` off, `TOTK_COOP_FAR_M` another distance). TotK Online's server
  disposes of a synced actor 1000 m from every player (actorAutoDisposeDistance). Its client deletes a runtime-made
  copy and only lets go of a map actor. Puppets are never disposed. Here each game checks against its own Link, each
  remote Link here and each other player's last NetState place:
  - copies made here by the spawn sync for another game (coop_world.c spawn_take): kept in `made_held` after made[]
    lets go of them, and deleted with the game's request delete by `far_sweep` once a second (log `coop: far: <name>
    <key> made here for player <n> is <d> m from every player: deleted`);
  - copies of another player's objects (coop_players.c arrow_copies): a live one deleted, a wanted one not made while
    far (`coop: far: slot <n>: the copy of ...`);
  - map enemies, horses and animals followed for another game (coop_world.c peer_one): owner set to this game, frozen
    puppet released, not sent; never deleted (`coop: far: ... not followed`);
  - placed objects another player moves (coop_players.c force_to): not moved; never deleted;
  - stand-ins (co-op Link, PlayerPuppet) and this game's own actors, runtime-made or placed: never.
  Nothing is far while a player's place is not known: a state more than 3 s old, world packets from a player with
  no state yet, and for 10 s after a player comes or goes (log `coop: far: the players changed`). Without that, a
  game that had just joined was not counted and what was held for it was let go of (code review, 2026-11-07).
  Test: tools/coop/far_3rd.txt (command and log lines). `TOTK_COOP_TEST_FAR_COPY` makes a copy through spawn_take as
  if the other game had sent a SPAWN. The test spawn passes no create queue, so it gives no spawn-sync copy.
  `TOTK_COOP_TEST_FAR` puts the Link 1100 m away with ForceSetMatrix. Unverified: the whole of it in a game; whether
  the request delete takes a copy that is far out and still loaded; that 1000 m is past where the game unloads
  copies on its own (if it unloads them first, the sweep only forgets them).
- **Done: map enemies keyed by their placement** (coop_world.c, actor_key; `TOTK_COOP_PLACE_KEY=0` the old way). An
  enemy placed in the map is keyed by its placement hash with its name, as TotK Online's (objects keep name and place); one made at run time keeps
  the name and the place this game first saw it. Suite: every case pairs its enemies and objects as before.
- **Fixed: copies over 1.5 m behind** (the sync audit's enemy-place in `handover` and `hits`, after sword hits on
  copies were taken out: a copy knocked back only in its owner's game trailed it by 2 to 4 m for seconds). The copy
  is now put all the way at its owner's place when over SNAP_M off, eased below that (native_follow). Then `handover`
  passed 2 of 2 and `hits` 1 of 1. One `handover` run before that saw player 2's co-op Link deleted in player 1's
  game (state 7) with player 1's Link 27 m away, and made again 4 s later; not seen again, cause not known.
- **Tried, off: in hand by the game's calls** (setEquipmentInHand 0x4f5700 / removeEquipmentInHand 0x4f5194,
  `TOTK_COOP_GEAR_INHAND=1`). On our co-op Link, whose own update runs in bursts, the sword sat on its back in more
  frames than when made again. TotK Online's stand-in has no AI and replays its owner's animation clips; that is
  what makes its gear stay in hand.
- **Done, opt-in: the stand-in as TotK Online's** (`TOTK_COOP_STANDIN=puppet`; needs
  `.venv\Scripts\python tools\coop\make_puppet_pack.py` after `tools/make_romfs_patches.py`, which writes the overlay's
  manifest anew). A PlayerPuppet actor of its own: the Player's pack with the Player component, its brain, camera,
  riding and life left out, under the category Enemy; its RSDB rows are an unused test actor's (Npc_Hylia_TestTube_001)
  renamed in place (a row added crashed the load: ActorInfo and GameActorInfo are paired by place). Each game sends its
  Link's first 7 animation slots with its state (NetState.anim: clip, weight, frame, frames a second); the others play
  them on the stand-in every tick with the play TotK Online calls (1.4.3: Q = [G+0x28], G =
  [[[actor+0x228]+0x40]+0x18]+0x18, Q's vtable +0x30 = main+0x1b569fc; the slots it writes are
  [[[actor+0x228]+8]+0x28]+0x40, count +0x38, 0xb0 a slot, clip u16 +2, weight +0x10, frame +0x28). Its gear is made
  once by the usual gear code and taken out or put away with the in-hand calls (the in-hand bit of the old puppet
  mode and the proxy's thaws are off for it). Pair test (equip_draw_3rd.txt): B's stand-in of A stood in A's pose
  holding A's Master Sword in hand, drawn by setEquipmentInHand (full-size frames), and played A's swing clip 1397.
  NetState grew for the slots: COOP_NET_VERSION 20 (relay too), so older builds do not meet this one.
  Body masks (2026-11-07): each slot holds its mask (bind) at +0x08; the bind sits at +0xc0 of a layer object the
  animation graph G points to ([G+0x1a8] whole body, [G+0x1f0], [G+0x2c8] upper body on an idle, walking and bow
  Link). The owner sends that offset per slot (CoopAnim.bind; COOP_NET_VERSION 21) and the stand-in plays the clip
  with its own bind from the same offset (the play's 4th argument, a record whose +8 is the bind). Pair test
  (bow_walk_3rd.txt): the stand-in's slots took the owner's clips with the owner's masks (0x1a8, 0x1f0, 0x2c8 by
  TOTK_COOP_TEST_ANIMSLOTS); not seen on a frame (the stand-in stayed far up the stairs). The mask's flags, which TotK
  Online copies too, are the stand-in's own.
  The masks' flags: the stand-in's graph has the same layer objects at the same offsets, but the flags its graph
  sets in them differ (a Link's whole-body mask 83 83 80 83 80 83 80, the stand-in's 83 83 80 80 83; TOTK_COOP_TEST_BINDS),
  and with its own flags it stood in a T pose in a sword fight. So the owner also sends each used mask's state
  (NetState.mask, CoopMask: offset, u16 +0x1c, byte flags [+0x10] count +8, word flags [+0x28] count +0x20;
  COOP_NET_VERSION 22) and the stand-in's masks take it before its slots are played, the mask's change count (+0x18)
  bumped so the play binds it again (coop_anim_masks_apply). With that, masks are on (`TOTK_COOP_STANDIN_MASKS=0`:
  off): in the sword fight (attack_enemy_3rd.txt) the stand-in stood and held its sword, and walking with the bow
  drawn (bow_walk_3rd.txt) it walked with the bow up (full-size frames, 2026-11-07).
  **Correction (2026-11-07, late), and the cause:** "held its sword" here and below was read from still frames. On the
  test save (nothing equipped by either player) the stand-in's sword stayed on its back and it swung an empty hand; in
  the tester's own play it was held. the tester found the rule: it is held only while this game's own Link has an item in that
  slot (equipped, even put away again in the same menu, it is not). The cause: the held item's placing, main+0x45a460
  (a method of the item's component, x1 the holder; called from main+0x455108 for every holder), follows the holder's
  EquipmentUser +0x1620, an object every EquipmentUser shares (the stand-in's and this Link's point to the same one),
  and with that object's +0x2cc set it asks game data (key 0x90b81999, by slot) for this game's player's item in that
  slot; with none it never reads the holder's in-hand byte (+0x4620), so the item is not put in the hand. Found by a
  read watch on the stand-in's in-hand bytes (TOTK_COOP_TEST_WATCHWHAT=coopeq: main+0x45a460 read them only when this
  Link was armed) and the hook's logs (TOTK_COOP_TEST_HELDCALL). Fix (coop_spawn.c, the main+0x45a460 hook): for an
  item a PlayerPuppet holds, the call runs with the stand-in's +0x1620 on a copy of that object with +0x2f4 bit 0 set
  and +0x2cc clear, so it reads the stand-in's own byte; 0 there crashed another thread (main+0xc9ad84). the tester saw
  the stand-in hold the Master Sword through its swings on the test save; `TOTK_COOP_STANDIN_HELDFIX=0` turns it off.
  Tried first and left off (test switches): the item made again in hand, the in-hand bit (TOTK_COOP_STANDIN_HANDBIT=1),
  a placeholder item in each empty slot (TOTK_COOP_STANDIN_PRIME=1), the item put at the hand bone
  (TOTK_COOP_STANDIN_HOLD=1; placing items by hand had led to taken and wrongly sized items on the old co-op Link).
  **Its sword's hits (2026-11-07):** the owner's weapon-attack byte (EquipmentUser +0x660 in 1.4.3 as in 1.2.1, 1 for
  about 0.1 s in each swing: TOTK_COOP_TEST_EQDIFF) goes as NetState flags bit 13 and is written onto the stand-in
  every tick. That alone made no contact from its swing in the owner's game. So, as TotK Online does, the contact of
  this Link's sword on a copy, taken out here (contacts_filter), is sent to the copy's owner ("CE" packet: the key,
  the contact, the body's place from the enemy's main body), and the owner's game adds it to its enemy's waiting
  contacts at that enemy's next hit update, its handles made the stand-in's and its sword's (ce_add). Pair test (B
  attacks A's blue Bokoblin, stand-in on): two contacts sent and added, and the Bokoblin died in A's game.
  **Its main body** is asked for motion type 0 every tick (requestChangeMotionType, 1.4.3 main+0xae4300, the same
  code as 1.2.1's 0xe8be30; `TOTK_COOP_STANDIN_MOTION=0` not), as TotK Online does; it still followed its owner to
  0.00 m and held its sword (equip_draw_3rd.txt). What type 0 changes is not checked.
  **Hits on the stand-in** are all taken out in the game that shows it (its owner's game has the attack on the real
  Link from its copy of the enemy).
  Not done: its arrows, the other
  co-op features on it (riding, shield surf, glider, hits it takes), and it is not seen moving in a frame yet.
- **Shared world progress, opt-in** (`TOTK_COOP_SHARE_WORLD=1`, runtime/coop_gamedata.c). Each player keeps their
  own save; with it on, the world's game data one game changes reaches the others while they play together. As TotK
  Online does (read for behaviour only), at the entry of GameDataMgr::calc (1.4.3 main+0xa72b00 = 1.2.1 main+0xee9704:
  the same code and offsets, its one caller main+0xa72abc as 1.2.1's 0xee96cc) this game's changes are taken and the
  others' pushed into the game's own pending set queues, so calc writes them and they are not sent back. The bools
  (manager +0x30 capacity, +0x38 entries {u8 value, u32 hash}, +0x40 a word whose low 20 bits count them) and the
  Bool64bitKey flags (+0xc68, +0xc70 {u8 value, i32 index}, +0xc78; the keys at +0xc60, 0x10 bytes: u64 key, u32 reset
  type, u8 value at +0xc) are read off their queues; setEventBool (1.4.3 main+0xc6a9f0 = 1.2.1 main+0x11616bc: its
  caller's caller looks up the same constant hash 0x2b05cbb9 in both) writes its table directly, so its writes are
  sent from its hook. Changes go out in "GMDT" packets on the HIT channel, again 1, 3 and 7 s later.
  - Bools and per-object flags: passed the pair test (2026-11-07, `tools\coop\gamedata_test.ps1`, on main).
  - Written 2026-11-07, not run in the game yet:
    - **numbers and states**: Int, Enum and four Floats. Value type t's block is at manager + t * 0x60 (2 Int,
      4 Float, 6 Enum; read in calc): entries 0x28 / 0x28 / 0x38 bytes with the value at +0x1c, queues of
      {value, hash, op} (op 1 adds) / {value, hash}. A copy of every shared value is compared each frame (the queue
      also carries "add" requests, and a compare sees direct writes too), so a change is sent as its result.
    - **what is shared**: only what GameDataList.Product.140 (read from the RomFS at start) puts in the progress save
      (SaveFileIndex 0; -1 is never saved, 1 is the options save). Of that, the player's are left out by name
      (PlayerStatus.*, the last wild horse, the statues' heart and stamina trades, the clock, which coop_players.c
      syncs, sort orders, the map layer, Mineru's construct...) and by family, names made from every RomFS actor name
      and the cooked dishes' names: EquipTime.*, EquipmentPouchTime.*, EquipmentDeathCount.*, RecipeCard.*,
      PictureBookData.* (the compendium: its photos are the player's), OwnedParasailPattern.*. Checked offline
      against TotK Online's name list: the families are found (all the gear ones, 508 of 509 compendium, 699 of 738
      recipe entries) and no other entry of the same type has one of their hashes. Floats: only
      SageOfGerudo_SliderBlockPos1..4. Arrays and strings: not shared (the pouch, horses, album, recipe log; an
      element has no name of its own). The reset type does not split the numbers either (EnemyBattleData and the
      compendium share 0x50).
    - **shrines and Lights of Blessing**: shrine states (DungeonState.DungeonNNN) are shared Enums. When one changes
      to Clear because of another player, this game works out its player's blessings as TotK Online's server does:
      shrines cleared here - (MaxLife - 12) - (MaxStamina - 1000) / 200 * 4, at least 0, and raises the pouch's
      Obj_DungeonClearSeal (Pouch.KeyItem.Content.Name / .StockNum, through the String64Array queue, 0x58-byte entries
      with their own text buffer, and the IntArray queue, {value, index, hash, op}) to that, in an empty key item
      slot when the player has none. It never lowers it.
    - **catch-up**: every shared change a game makes or takes while its Link is in the world is kept (latest value
      and stamp per entry), also while it is alone. A game that starts sharing (it joined, a player came back, its
      world was loaded again) says hello; the others send it their session log twice, 2 s apart, addressed to it, and
      answer with a hello so it sends its log back. So a player who joins late or comes back gets what the others
      changed meanwhile, also alone, not their whole saves. A caught-up value that would move this game's progress
      back (a set flag cleared, a number lowered, a cleared shrine changed) is left as this game has it unless this
      game changed it in the session. When a game's world is loaded again from a save (many of its log's values read
      otherwise when tracking starts again) its log is cleared and the others catch it up.
    - **order**: each change carries a Lamport stamp; a game takes another's change only when its stamp is above the
      one it has for that value, so a late resend of an older value does not undo a newer one, and two games that
      change one value at once end on the same value. While a game is not sharing, packets for it are dropped and
      what waited to be sent or applied is dropped too (the catch-up brings it). GMDT version 3: 24-byte entries
      (id, stamp, value, type), 25 a packet (608 bytes).
    - **settling** (2026-11-07, offline harness only): the tour case on two different saves failed the sync audit with
      sharing on; its logs showed each game taking its own save's load as changes (1300-1500 quest steps, challenge
      states and counts written after the Link appeared, before the world clock started) and the other applying
      them. Now nothing is tracked before the world clock runs, and after tracking starts, or after any frame with
      more than 32 changes (a load or a reset), values are only copied until 2 s pass with no frame of more than 4;
      a log entry a load undid leaves the log (the others' catch-up brings it back). Not found: what moved the time
      of day in that run (no clock entry is shared; perhaps a quest state from the other save, unverified).
    - review fixes (2026-11-07, offline harness only): changes made while no other player is there were never
      shared; state outlived a session (inbox applied later, a catch-up sent on to a player who had left, a rejoin
      within 10 s not caught up); bool and key requests over 256 in a frame were lost (now all taken; a full outbox
      sends the whole session log to everyone instead); a bool id's top 32 bits could fill the session log; one
      change no queue took stopped the inbox (dropped after 2 s); the number copies leaked on a failed allocation.
    - also fixed: coop_world.c peer_tick takes HIT packets into a 616-byte buffer and drops longer ones; a GMDT packet
      held up to 60 entries (968 bytes), so a burst of more than 38 changes was lost. It now holds 36.
    - test: `tools\coop\gamedata_test.ps1` (game A changes a key, a bool, an Int, a shrine and a Float and puts them
      back; game B joins the sharing late, `TOTK_COOP_TEST_LATE=B:50`); the lines a passing run shows are in the
      script. Checked offline on two fake managers in one process (a harness, not in the repo), with the real
      GameDataList and actor list: numbers both ways with no echo, the personal ones kept, an "add" shared as its
      result, the late join caught up (a lower kill count kept), the blessing slot filled, packets lost for 10 s made
      up by the resends, changes made apart exchanged when the players meet again, a stage change keeping the log, a
      reload clearing it and being caught up, one value changed in both games in one frame ending equal. Not run in
      the harness: a full outbox's resync, a queue that never takes a change.
    - unverified: that the game reads the pouch's key items from game data each time (a pouch kept elsewhere would
      overwrite the blessings), what a shared quest step does in a game whose quest is elsewhere, and that the Enum,
      Int and Float entries' value is at +0x1c in every one of them (read in calc's compare, the start lines' counts
      check the tables). Known gaps: a change pushed by another thread between the read and calc is applied but not
      sent; two games that changed one value while apart end on the one with the higher stamp, not the later one;
      live changes take the latest value, so a player further along can be moved back by one behind; bool families
      whose names cannot be made from the RomFS (ReadyMadeAutoBuilderDraft.*) are shared.
- **Stopped, not working: arrow trails on arrow copies** (2026-11-08, the tester's call; the code is not on main: branch
  claude/coop-arrow-tail-wip, 2daf64e, coop_players.c
  arrow_tail; `TOTK_COOP_ARROW_TAIL=0` off, `TOTK_COOP_TEST_TAILLOG=1` logs). A shot arrow emits its trail once as its
  flight starts: the flight update (main+0x1d23a4c, a method of an object at [[arrow+0x228]+0x3d8]+0x340, vtable
  main+0x3693970) calls main+0x1d24498, which emits "ArrowTail" (or "LongThrow_ArrowTail") through main+0x4f4e40
  (x8 the two handles out, as a returned struct; x0 the link user [[actor+0x228]+0x68], x1 &name, x2 2) and keeps the
  handles at +0x1b0, flag +0x1ed. On that branch the copy (moved by the sync, never shot) makes that call once as it starts to
  fly, runs its own update while it flies and keeps its actor at its shown place then (not 300 m under). The effect
  is made and stays alive in the pool (handle check as the SwordBlur update's: group bases [main+0x395f388], sizes
  [main+0x395f390], serial at +0x20), but nothing is drawn (the tester watched, 2026-11-08). The owner's and the copy's
  pool entries differ only in two flag words: +0x8 0x402 against 0x2 and +0x34 0xf against 0x3. Next: what sets
  those bits on the owner's (most likely the effect system once the emitter is created and follows its user).
- **Done: the stand-in's sword trail** (2026-11-07; coop_world.c "The stand-in's sword trail", COOP_NET_VERSION 24;
  `TOTK_COOP_STANDIN_BLUR=0` turns it off). How the game makes a swing's trail, found by a write watch
  (`TOTK_COOP_TEST_HITWATCH=<from>-<to>@0x28:0x4a8`) and the generated C:
  - The Player's pack has a SwordBlur component (SwordBlurRef, kept on the stand-in; vtable main+0x367e028, at
    [actor+0x228]+0x4a8). Its update, main+0x275954, runs every frame: it looks through the actor's animation graph
    ([[[actor+0x228]+0x40]+0x18]+0x18, the layers' nodes) for an ASB event of kind 17 ("WeaponBlur", game::WeaponBlurType
    the hand) that is on, and while one is it emits the weapon's "Blur_Default" effect (ELink: WeaponBlur_Sword,
    Blur_MasterSword...) on the weapon in that hand. The effect's handle is kept at +0xc8, the event at +0xe0, +0x2a is
    1 while the effect lives (about 0.17 s a swing). So the trail comes from the animation sequence's nodes, not from
    the weapon, the Player component or the BSA (no BSARef needed, and nothing of ours calls the effect system).
  - The stand-in's graph never has those nodes (its clips are played straight onto its animation player), so it never
    found the event. Now the owner reads its own active WeaponBlur event (hand, frame, first frame, parameter, a number
    per event) into NetState.blur_*, and the stand-in's update (hooked, COOP_HOOKED) runs once with a copy of the
    stand-in's own graph whose only layer holds one event made from those values, then gets its graph back. The game's
    own code then makes the trail on the stand-in's weapon.
  - Tried first and wrong: copying the owner's component bytes +0xc8..+0xe8 onto the stand-in's. They are effect
    handles (an effect pool index and serial), meaningless in another game; the stand-in's update cleared them each
    frame anyway.
  - Test: `$env:TOTK_COOP_STANDIN='puppet'; $env:TOTK_COOP_PUPPET=1; $env:TOTK_COOP_TEST_BLURWATCH=1;
    tools\coop\run_pair.ps1 -Seconds 80 -ScriptA tools\coop\equip_draw_3rd.txt -ScriptB tools\coop\idle_3rd.txt
    -BDir build\v143c -ShotsB "w18-26:0.034" -EnvB "TOTK_SHOTS_CROP=1100,300,1200,1000" -ShotsDir build\trailshots8`:
    in B the stand-in's +0x2a went to 1 for 0.17 s on each of A's swings (as A's own), and frame f02556 shows the
    white swing arc over the stand-in. TOTK_COOP_TEST_BLURWATCH also logs the component's words as they change and how
    many of the stand-in's updates had its owner's event.
  - Seen, not fixed: in both pair scripts tried (`attack_enemy_3rd.txt`, `equip_draw_3rd.txt`) the stand-in's sword
    stays on its back while it swings an empty hand, though "Weapon_Sword_070 taken out" is logged; the same with this
    change off (`TOTK_COOP_STANDIN_BLUR=0`, frames build\trailshots6), so it was there before. The trail is emitted on
    that weapon actor, so how it looks with the sword in hand is not seen yet.
  - Seen: the stand-in's SwordBlur update ran about 5 times a second for a while in one run (about 4 m off, 11 calls
    in 2 s) and 60 in others; at 5 a second a swing's 0.17 s can fall between two updates and show no trail.
  - Not done: arrow trails and the other action effects (dust, sparks). Not looked into whether they come from other
    ASB event kinds (the event kind names at main+0x3713400 list AtCollision, Noise, AwarenessScale and others) the same way.
- **Not done:**
  - the stand-in's arrows shot by it with its owner's camera (patched unproject): not ported, on purpose for now.
    TotK Online has the stand-in shoot because it syncs no arrows; here the owner's arrows (nocked and flying) are
    sent as objects and their copies follow them (the `fuse` suite case), placed on the PlayerPuppet's bow after its
    model phase as on the old co-op Link, and an arrow's damage on a copy goes to its owner as lost life;
  - the stand-in's paraglider and Ultrahand arm as actors of their own tied to it (Parasail, PlayerRaulHand, by
    requestRegisterRelation), with the glider's colour and open state; the glider goes as a moved object's copy now,
    only while open, and is placed on the PlayerPuppet after its model phase (2026-11-07: the copy was made in the
    other game, glide.txt; not seen on a frame, the owner glided out of view);
  - riding and shield surf on the PlayerPuppet (2026-11-07): it is put on its owner's ridden object (ride_place, in
    coop_puppet_place) and its shield placed in a surf (shield_place after its model phase), as the co-op Link is;
    in surf.txt the stand-in surfs on its shield under its feet (full-size frame, 2026-11-07, after the fix that
    read the stand-in from x0 after its phase had run, when x0 is the call's result); riding an object not tried.
    Hits it takes are all taken out where it is shown (contacts_filter): its owner's game has them on the real Link;
  - contacts: here a Link's Weapon_ on an enemy copy is taken out and sent to the owner (CE), and every hit on a
    PlayerPuppet is taken out. Not ported: TotK Online also takes out an enemy copy's body contacts on this Link and
    replays them in the owner's game. Here the copy's own attack (its weapon or body) hurts this Link in this game,
    and the owner's enemy's hit on the stand-in there is taken out, so each hit still counts once;
  - the actors TotK Online syncs by name (Animal, Korok, GameRomHorse, Barrel, BombFruit, TBox and more): here enemies
    and moved objects, and horses and animals (next item); chests are left out on purpose; actors past 1000 m deleted (here coop_far.c, the "1000 m from every player" item above);
  - **horses and animals: written, not run in a game yet** (2026-11-07; runtime/coop_mounts.c, `TOTK_COOP_MOUNTS=1`
    turns it on, off by default; the native enemy model only). They go through the enemies' sync as more kinds
    (coop_world_kind): GameRomHorse* and Animal_Horse* as horses (not GameRomHorseSaddle_/Reins_), the other Animal_
    that walk as animals; fish, insects, birds and swarms are left out (TotK Online syncs them), and horses and
    animals take at most 8 of a world packet's 28 entries. Same keys, owner choice, hand-over, easing of the copies,
    the owner's life and hits sent to the owner; horses' action requests are replayed on their copies as enemies'
    are (TotK Online's BSA list has GameRomHorse, not Animal); a horse made at run time (a stable's, a whistle's) is
    made in the other games by the spawn sync, animals only with `TOTK_COOP_MOUNTS_SPAWN=all` (an area makes its own
    in each game). A horse is no moved object then (prop_skip). Riding: this Link rides a horse when it stays 10 ticks
    in a box in the horse's frame (|x| < 0.45, -0.3 < y < 2.0, -0.4 < z < 1.5 m; the horse faces its +z, rotation
    column 2, and its origin is at its hindquarters about 0.5 m above its ground: a Link beside it was at -0.83 -0.5
    0, by its rump in a frame; the riding numbers are a guess, `TOTK_COOP_MOUNT_BOX=x,ylo,yhi,zlo,zhi`; no riding flag
    of 1.4.3 is known). It is sent in NetState as ride_owner
    COOP_RIDE_MOUNT (0xfe; an older game rides nothing), ride_id the horse's key, ride_off the offset; the ridden
    horse is its rider's game's and is not handed over (peer_one), and the other games put the stand-in (co-op Link
    or PlayerPuppet, through owner_matrix) at that offset on their copy of the horse. Not known: whether a horse
    passes main+0x50ad74, whether horses and animals keep a placement hash at +0x370 as enemies do, the box, how the
    copy and a stand-in on it push each other, and a called horse's look (its copy is made by name: mane, saddle and
    reins are the default). Test: suite case `mount` (tools/coop/mount_horse_3rd.txt; the lines it wants are in
    run_suite.ps1, `TOTK_COOP_TEST_MOUNT=1` logs the Link's place in the nearest horse's frame each second). First
    run (2026-11-07): the horse synced (one key in both games, B's copy run by A's game, its action requests
    replayed, later handed to B's), but the Link never got on: walking up, it reached the horse's side only after the
    A presses. So `TOTK_COOP_TEST_MOUNT_PUT=30:1` puts player 1's Link 0.9 m to the horse's right, facing it, at 30,
    32 and 34 s, and the script presses A just after each. Second run: put and facing the horse as meant, but at its
    origin, which is by its rump, and no Mount prompt showed. The puts now go 0.7, 0.4 and 1.0 m forward of the
    origin, by the saddle. Third run: put there (the log has the Link at -0.92 -0.45 0.72 in the horse's frame right
    after it, and a frame shows it crouched by the saddle; the Link behind the rump in that frame is B's co-op Link,
    left at the start), then after the A presses it went to -0.49 -0.92 1.13 and -0.45 -1.83 1.17 (a mount and a
    rearing horse, or a fall: not told apart), and the next put took it away. Now a Link that has moved over 0.35 m
    since a put is not put again, and the test log goes ten times a second around the puts, with both world
    heights; not run yet. A horse's mane (Horse_Link_Mane*), saddle and reins are no moved objects any more, with or
    without TOTK_COOP_MOUNTS: a mane's copy hung in the air beside B's Link. Fourth run: the Link stayed put by the
    saddle, facing it, and the A presses reached the game, and still no mount; no frames of that run. (The third
    run's move was most likely the Link picking up the mane's copy, not a mount.) Not known why: whether a horse made
    by the test spawn can be mounted at all, or the prompt's range and facing; no mount call of the game is known. So
    suite case `mount_fake` (`TOTK_COOP_TEST_MOUNT_FAKE=30:1`) counts A's Link as riding the nearest horse from 30 s
    and tests the rest (who runs it, the rider heard of, the stand-in put on the copy) without a mount (it passes).
    Frames of 29.8-31.2 s showed no Mount prompt; the presses make Link crouch by the horse's front. A's own game ran
    the horse through every press (its test lines say "run here" to 41 s, when it was handed to B's game with A's
    Link 8 m off after the ride-forward walk), so a copy refusing the ride is ruled out. `tools\coop\mount_solo.ps1`
    runs the same spawn, puts and presses in one game with no peer (coop_mounts_tick runs alone too), to settle
    whether a test-spawned horse can be mounted at all. Run (2026-11-07): no mount alone either, with GameRomHorse01,
    Animal_Horse_A or GameRomHorse00L, all nine presses beside the saddle; so not the sync. **Parked (the tester's call,
    2026-11-07)**: the way the test spawns the horse may be the cause; next is a save beside a wild or stable horse
    that the tester finds, then the same puts and presses on it;
  - shared world progress: arrays and strings (the pouch and the other per-player lists), and catching a player up on
    what the others did before this session (only the session's own changes are sent);
  - Ultrahand glue: the scripted test passes, a played glue is not tried yet (runtime/coop_glue.c; `TOTK_COOP_GLUE=0` off). The 1.4.3 functions, found
    by code (2026-11-07): the capture site is the tail call at main+0x1d61b04 in main+0x1d61a6c (1.2.1 0x16575dc in
    0x1657544: the same 39 instructions); doScotch main+0x1d66e34 (its only caller is that tail call); createScotch
    main+0x1c0b94c (the same 55 instructions as 1.2.1's 0x165710c; the matcher's two candidates were wrong);
    removeScotch main+0x1c042bc (called from the component's message handler main+0x1c0bf20 on message 0x2051000b
    with the same arguments as 1.2.1's 0x1650c30 from 0x12e59e0); TotK Online's destruction hook 1.2.1 0x13bf5dc is
    1.4.3 main+0x2999fe8, a generic message forward, reached for glue from the break step main+0x1c0bb54 (1.2.1
    0x16575e0, 25 instructions both), which is hooked instead. The request's argument (base class, constructor
    main+0x1c0c4ec, vtable main+0x367d5c8): +0x20 and +0x78 ActorLinks to the two actors, +0x38 and +0x90 their
    bodies' handles, +0x3c and +0x94 two 3x4 matrices. What it does: doScotch's entry captures a bond made here; its
    maker sends it ("GA") with both objects' numbers across the games (coop_players.c coop_obj_ident), where each
    glued body sits in its actor's physics controller, both matrices and the flag, again every 3 s, and its list of
    bonds every second ("GL"). Another game finds its own two objects (coop_obj_find), builds a request with its own
    body handles, calls createScotch and doScotch, and retries once a second until the entry is glued; a bond gone
    from two of its maker's lists is parted there with removeScotch; a break of a bond made again from another game is
    refused there (except 2 s after it parts one itself). Not known: what the two matrices are (each body's frame or
    the world's: the capture logs them against the actors' places); whether a parted bond leaves its maker's entry
    list or only changes its state (the maker drops a bond once its entry is gone or not in state 2); whether the
    copies' forced placement fights the glue (TotK Online stops its position correction for 1 s on each remake). Test:
    `$env:TOTK_COOP_TEST_GLUE='AsbObj_WoodSquare_A_M_01,AsbObj_WoodRectangle_A_M_01'` then run_pair.ps1 -Seconds
    80 -ScriptA tools\coop\glue_pair_3rd.txt -ScriptB tools\coop\glue_pair_3rd.txt: A makes the two at 30 s, glues
    them 1 s after both exist with the game's calls (its matrices a guess) and parts them 20 s later. A pass shows in
    A "coop: glue: made here: bond 1, ..." and in B "coop: glue: bond 1 from player N made again here: ..." and,
    after A's removeScotch, "... is gone there: parted here". First run (2026-11-07, a Zonai cart and wheel): the cart
    rolled off the In-isa stairs and went away 1.5 s after it was made, and the glue at 40 s read the gone actor
    (component 0, body 0); the wheel had its scotch component and a body handle (0x61). With the planks (and the test gluing 1 s after both exist) it passed: glued in A, made again in B on try 1,
    parted in both (pair test on main, 2026-11-07). Every bond takes the hooked path, by code: doScotch, its capture function main+0x1d61a6c,
    the glue step main+0x1c0ba8c and the component's message handler main+0x1c0bf20 are reached by direct branches
    only (no pointer to any of them in the data or the code), and the handler only through the component's message
    method (vtable slot main+0x367d560); the requests come from the Ultrahand's glue work (main+0x1d5565c, its own
    argument class of 0x1b8 bytes) and from the component (main+0x1c09388, base arguments from the pool main+0x1c0c480),
    so Ultrahand glue, autobuild and the game's own all reach doScotch's entry. (A bond both games make on their own,
    a structure built by a load in each, would be sent by both and each game's breaks refused by the other's copy:
    not seen, not handled.) A played test (a person at game A; game B can idle): set
    `$env:TOTK_COOP_TEST_GLUE='AsbObj_WoodSquare_A_M_01,AsbObj_WoodRectangle_A_M_01,make'` (made 1.5 m in front
    of A's Link at 30 s, not glued; "coop: test: glue: both made; glue them with Ultrahand"), run run_pair.ps1
    -Seconds 240 -ScriptA tools\coop\glue_pair_3rd.txt -ScriptB tools\coop\glue_pair_3rd.txt with game A's window in
    front and a pad or keyboard on it; once both planks lie there: choose Ultrahand (hold L, pick it, let go; or tap
    L if it is the one chosen), aim at one plank, A to take it, move it with the stick and R/ZR until it touches the
    other (the glue shows green), A to let go: they stick. Game A logs "coop: glue: made here: bond 1, ..." and the
    matrices line; game B "bond 1 from player N made again here". Then take the pair with Ultrahand and shake the
    right stick quickly left and right until they part: A logs "bond 1 ... is no longer glued here: taken off the
    list" within 2 s and B "... is gone there: parted here". Also try it the other way (the person at B). Review fixes (2026-11-07, not run yet): a game parting an actor for another game's bond makes its own bonds on
    that actor again for 5 s (removeScotch parts all of them); a break whose other side is not known is let through;
    the objects are checked by their actor id ([actor+0x10]) as well as state and name; full tables are logged. The packets
    need no protocol version change (old builds drop them);
  - rotating ground: written, the sync not seen working yet (coop_world.c, rotating ground; `TOTK_COOP_GROUND=0`
    off). What turns it in 1.4.3, by code: a turn helper (main+0x298a20c; +8 top speed, +0xc acceleration, +0x10 speed
    now, +0x14 the angle in radians kept in 0..2 pi, +0x18 the ground control's index) that two nodes drive: the
    actor's own logic node ActorLogicGroundControlRotate (named by Pack/Actor/GroundControlActor_Rotate's parameters;
    enter main+0x1cf0160, update main+0x1cf0260, node+8 the actor, helper at node+0x28, so the angle is at node+0x3c,
    TotK Online's 1.2.1 place) and the AI node ExecuteGroundControlRotate (update main+0x2329170, helper at
    node+0x20). Each update gives the helper two switch bytes, turn and turn the other way; the logic node reads them
    from the actor's blackboard (RotateAxisX, RotateAxisXInv, main+0x1b13c9c). With neither on the speed falls to 0
    and no turn is set (main+0x2988330 is called only while the speed is not 0). Pair test at H-5's (2026-11-07, 621b133):
    the GroundControlActor_Rotate at 2394.5 267.0 520.8 (ground control index 8952) was live, the helper ran once a
    frame, the AI node never, and no turn was set: its switch is off, so it does not turn in that save.
    The sync now hooks both updates: the host sends each node's angle, speed and two switch bytes ("GR", 16 bytes
    each), and in a guest the update first takes the host's angle (moved on by the host's speed for the time since it
    came) and speed, and its two blackboard reads answer the host's switch bytes. Placed ones in MainField (bcett, 25,
    none on the Great Sky Island): H-5_Static 2394.5 267.0 520.8 (RotateSpeedX 6), Set_SkyIsland_HyruleForest_F-2_South
    near 205.5 1062-1863 -2028 (10, 2.5-26), Firone H-7 near 2344 1689-1757 1750, Akkare J-2 4472.0 1548.3 -2026.5,
    Zora H-4 2910.6 1065.4 -525.0, gerudo A-6 -3484.9 2042.5 320.0, Rito SkyField -3070..-2844, LargeDungeonWater
    3502.8 1922.4 -704.7; which ones turn without a switch is not known. Test: `$env:TOTK_COOP_TEST_GROUND='25'` and
    `$env:TOTK_COOP_TEST_GROUND_SPIN='1'` (the host answers RotateAxisX 1 for its logic nodes, so H-5's turns there),
    run_pair.ps1 -Seconds 75 -ScriptA/-ScriptB tools\coop\ground_pair_3rd.txt. Each game logs "coop: test: ground:
    this Link put at 2394.5 271.0 520.8", "coop: ground: GroundControlActor_Rotate <key> at 2394.5 267.0 520.8 is
    turned by its logic node here" (the same key in both), then every second "coop: test: ground: this Link at ...;
    GroundControl actors within 80 m: ...", "coop: test: ground: in the last second: logic node update u, enter e; AI
    node update 0, enter 0; turn helper h; turn set s; main+0x298a3f4 xN (index 8952)" and "coop: test: ground
    GroundControlActor_Rotate <key> (logic node) angle a speed v (top 0.105) switch o i[ (host)]; host's now a' speed
    v' switch 1 0". The guest logs once "coop: ground <key> turns as the host's (angle ..., speed ..., on 1 0)". A
    pass: in the host switch 1 0, the speed reaches 0.105 (6 degrees a second) and the angle rises; turn set about
    60 a second in both games; in the guest the angle within about 0.02 of "host's now" each second. Without SPIN
    both stand still at the same angle. Pair test (c0bd158, 2026-11-07): the same key in both; the host turned (speed
    0.105, turn set 60 a second, index 8952) and the guest's angle followed it within 0.002, but its speed stayed 0
    and it set no turn: the host sent switch 0 0, the blackboard's own bytes from before SPIN's answer. The bytes
    sent are now the ones the node gets; not run again yet;
  - a thrown weapon or a shield bash on a copy is taken out of its contacts as a sword hit is, but the stand-in does
    not throw or bash in the owner's game, so that hit is lost (code review, a guess);
  - the Gloom Hands: left out, as TotK Online leaves them out.

## Enemies and the stand-in (2026-11-07; stopped, the hand-over does it)
- **Outcome.** The default pack is as before and as the mod's: the Player's resident emitter, no DefaultAffiliation
  in the GameActorInfo row, so enemies ignore the stand-in, and the hand-over gives each enemy to the nearest
  player's game. `make_puppet_pack.py --target-fix` builds the test pack: the emitter not resident and the Player's
  DefaultAffiliation. With it a Bokoblin notices the stand-in (c882) but fights this game's own Link, 30 m off,
  instead. Where it takes "the player" from is not found. The redirect of the perception manager's player field
  (TOTK_COOP_ENEMY_REDIRECT=1, main+0x45b930) reached one check in a whole run, so that hook is not on the target's
  path. It stays off. The runs that led here follow.
- **The "is a player" hook is not the target check.** 1.4.3 main+0x1d1fc70 is the right match for TotK Online's
  1.2.1 main+0x1736dec (the same code, side by side: the actor from a link, its component +0x120, its name looked up
  in a table [main+0x39601e8] through main+0xd62b58). Its one caller, main+0x1d1f9e8, is in the function that loads
  "IsAdditionalArrowBow": bow code. That is why a sword fight never called it. The hook stays (coop_spawn.c) for
  the stand-in's bow; its comment says what it is.
- **The likely cause: the stand-in's perception emitter is resident.** The stand-in had the Player's
  PerceptionEmitterParam, `IsResidentEmitter: true` (SubLayerSensor PerceptionPlayer). So did everything tried before
  (a second Player; a pigeon with the Player's or a Sage's emitter: SageCommon is resident too). The perception manager
  keeps one player emitter in a field of its own (+0x58, re-notes "Enemy targets: not found"), which enemies check
  instead of sensing it. Enemies' and horses' emitters (EnemyCommon) are not resident and are sensed through their
  sensor sublayer. A Bokoblin's TargetAffiliations hold "Player", and the stand-in has the Player's
  AffiliationParam (DefaultAffiliation Player). Evidence: the files and the param reader main+0x1abe6e0
  (IsResidentEmitter, SubLayerSensor). Where the game reads the flag is not found. Likely, not proven.
- **Tried** (now `--target-fix`): tools/coop/make_puppet_pack.py gives the stand-in its own emitter param (the Player's with
  IsResidentEmitter false, `Component/PerceptionEmitterParam/PlayerPuppet...`). `--resident-emitter` builds the old
  pack (a control); `--emitter-sublayer PerceptionEnemy` tries the enemies' sublayer if PerceptionPlayer is not
  sensed. No runtime change is needed for the fix itself.
- **Test** (rebuild the pack first: `.venv\Scripts\python tools\coop\make_puppet_pack.py` after
  tools/make_romfs_patches.py):
  `$env:TOTK_COOP_STANDIN='puppet'; $env:TOTK_COOP_PUPPET=1; $env:TOTK_COOP_TEST_SPAWN_AT='standin';
  $env:TOTK_COOP_TEST_FOELOG=1; $env:TOTK_COOP_ENEMY_HANDOVER=0; tools\coop
un_pair.ps1 -Seconds 80 -ScriptA
  tools\coop\walk_far_3rd.txt -ScriptB tools\coop\standin_foe_3rd.txt -SpawnB "Enemy_Bokoblin_Junior@40" -ShotsB
  w40-70:2 -ShotsDir build\standin-foe`.
  A walks about 20 m away. B stands, and at 40 s a Bokoblin is made 3 m in front of A's stand-in in B's game, facing it.
  Expected in B's log:
  - `coop: spawn Enemy_Bokoblin_Junior 3 m in front of player <id>'s stand-in at ...`
  - `coop: Enemy_Bokoblin_Junior claimed: ok`
  - every second, `coop: test: foe Enemy_Bokoblin_Junior at ...: awareness <w0> ...; <d> m from this Link, <s> m
    from player <id>'s stand-in`.
  Passing: w0 goes from c182 (unaware) to c082 (aware) within a few seconds, and the stand-in distance stays
  under about 3 m while the distance to this Link stays near 20 m. B's frames show it swinging at the stand-in.
  Hits on the stand-in are dropped in B (contacts_filter) and A's game has no Bokoblin, so nobody loses life;
  that is expected.
  Control: the same with the pack built `--resident-emitter`. The Bokoblin should stay c182, or walk to B's own
  Link.
  First run (merge 57f83f1): the spawn and the foe log read the stand-in's actor position (+0x2b4), which was
  -20.0 -6.4 72.0 while its body and its owner were at 43.3 1510.4 1504.8; the Bokoblin was made 2 km away and
  said nothing. Both now take the owner's reported place (coop_players.c coop_player_places). The foe log also
  prints the stand-in's actor position each second. If it stays off the owner's place, enemies may sense the stand-in
  there, and its actor position must be kept at the owner's too.
  Second run (a1294f6, places right): with the emitter not resident, the Bokoblin 3 m from the stand-in, facing it,
  B's Link 26 m off, stayed unaware. Its awareness words went c182 12 12 0 0 28, then for a few seconds at a time
  c182 12 12 1 0 5c (it noticed something), and never c082. Next cause, now fixed in the pack: the actor table
  GameActorInfo holds an actor's DefaultAffiliation (1356 rows; the Player's "Player", the Bokoblin's "Enemy"), and
  the stand-in's row (the test actor's) had none. The Bokoblin's receiver has DefaultFilter PerceptionFilterEnemy,
  IsReceiveFromSensorWorld true and IsReceiveResidentEmitter false, so it senses the stand-in through the sensor
  world and keeps targets by affiliation. make_puppet_pack.py now copies the Player's DefaultAffiliation into the
  stand-in's row (`--game-fields`, default DefaultAffiliation; '' for the old row). `--category Player` tries the
  Player's category instead of TotK Online's Enemy, if the affiliation alone is not enough. Not run.
  Third run (a8e6f03): with DefaultAffiliation Player, still unaware (c182, the same flicks); with `--category
  Player`, no flicks at all. Next try: `make_puppet_pack.py --keep AttentionReceiverRef` puts the Player's
  AttentionReceiver back (left out as one-Link-only). Unlikely to be the cause: attention is the Link's lock-on side
  (the Bokoblin has AttentionSender and LockOn client params), not the enemy's perception. Not run.
  Fourth run (cbea934, --keep AttentionReceiverRef): no change. Pack toggles stop here.
  Next, read watches: who reads the stand-in's perception data during the flicks, against who reads this Link's in
  a real notice. The functions that read the Link's data but not the stand-in's are where the stand-in drops out.
  Uses TOTK_COOP_TEST_WATCH5 with WATCHWHAT=coopdata or data, and TOTK_COOP_TEST_WATCHLEN for the window.
  Read watches (eeb6fe3): neither run was a fair test. Stand-in run: B's Link 2089 m off (A fell and reloaded
  elsewhere, so B did not load that area), three readers only (main+0x3020e0, 0x42c554, 0x328d80). Control (B's Link
  2 m from the Bokoblin, made facing the same way as the Link): 36 readers, but never c082 either, only
  "1 0 5c / f3" spells. A fight is c082 c c 1 1 (+0x84 is the action, not awareness). So the "1" spells the stand-in
  got are the same kind the Link gets from a Bokoblin that has not seen him, and the setup was the problem.
  Matched pair now:
  - control: TOTK_COOP_TEST_SPAWN_AT=self (3 m in front of this Link, facing it), B playing walk_late_3rd.txt
    (walks into it at 45 s);
  - stand-in: SPAWN_AT=standin, A playing standin_walk_3rd.txt (6 s away from B, then into the Bokoblin at 45 s).
  The control must reach c082 first; only then does the stand-in run, and its reader list against the control's,
  mean anything.
  Matched pair (c726237, with 6075057's later menu presses): control passes (c082 c c 1 1 at 42 s, 2.9 m, fights to
  the end). Stand-in: c882 c c 0 0 4 for 3 s (a state the control never showed) while it turned away from the
  stand-in 3 m in front of it, then c082 fighting toward B's own Link 30 m off, holding about 16 m from it. So it
  does notice the stand-in now (the non-resident emitter and the affiliation), but when it goes to fight "the
  player" it takes the perception manager's one player emitter (+0x58, B's Link). That is where the 2026-11-04
  slot tests pointed. Next test: the same run with TOTK_COOP_ENEMY_REDIRECT=1. That hook on main+0x45b930 puts
  the nearest stand-in's perception data in that field for each enemy nearer to it than to this Link (coop_spawn.c;
  off by default since it changed nothing for a second Player, whose resident emitter was never noticed). Expect
  "coop: perception: N checks taken to a co-op Link so far" in B's log and the foe staying within a few m of the
  stand-in in c082.
  Redirect run: the same with TOTK_COOP_ENEMY_REDIRECT=1. "1 checks taken to a co-op Link" once, no later count;
  the same behaviour (c882 for 3 s, then fighting toward B's Link). Stopped here (see Outcome).
  Unverified: that 20 m keeps B's own Link out of the Bokoblin's notice; the earlier note saw one stay unaware
  with the Link 44 m off.
- Not done: TotK Online's four crash-guard patches in the bow function family (1.4.3 near main+0x1d1f960, 0x1d1f9e4,
  0x1d1fa54, 0x1d1ffd0). They belong to the bow path, not to targeting.

## To remove once the TotK Online way is the default (2026-11-07)
Nothing here goes before what replaces it is the default, the tester has played with it, and the full suite passes
without it. One area per commit, the suite after each; the findings stay in this file and re-notes.md, the code in
git history. Sizes are not measured yet (coop_world.c 4848 lines, coop_players.c 5078, coop_spawn.c 1783).
1. **Enemy puppets, the frozen model** (`TOTK_COOP_ENEMY_MODEL=puppet`; replaced by the native enemy model, default
   since 424a896). In coop_world.c: `enemy_puppets_on`, the pose packets with bones and items (`epose_calc_start`,
   `epose_send_tick`'s bone and item part, `epose_take_tick`'s pose part, PupRec's bones), the enemy weapons frozen
   and put in a copy's hand (`weap_collect`, `weap_one`, `weap_let_go`, `pups[].weap`), the awareness words
   (`pup_aware_apply`, AWARE_*), the frozen copy's hits (`pup_hits`), the skeleton writers and phases
   (`coop_enemy_puppet_skel`, `coop_enemy_puppet_writer`, `coop_enemy_puppet_calc`, `coop_enemy_puppet_phase` and
   their lines in coop_spawn.c), the jitter logs, `enemy_poke_test`. Keep: the pose packet's header, EAction, the
   owner and follow tables, `native_follow`, `coop_enemy_action`.
2. **Hit relays the contact sync replaces**: hits on a proxy replayed in its owner's game (`link_hits_send`,
   `link_hit_take`, `link_hit_deliver`, LinkHitPacket, `coop_hit_drop`, the guard-bit and shield tests), the
   proxy-contact relay test (`contact_send`, `contact_add`, CONTACT_KIND, `contact_on`). Keep: `contacts_filter`,
   the CE packets (`ce_queue`/`ce_take`/`ce_add`). The lost-life HitPacket stays until the stand-in shoots its own
   arrows (it carries arrow and bomb damage now).
3. **The co-op Link as a second Player** (replaced by the PlayerPuppet stand-in once it is the default): the
   controller swap and input (`coop_players_calc`'s controller part, `set_input`, `coop_fill_controller`), what
   kept a second Player from changing this game (the HUD guide, tips and game-data setters skipped inside its
   update, coop_spawn.c), proxy mode (`proxy_drive`, `proxied`, thaws and attack thaws, `PROXY_THAW_TICKS`, the
   gear retries and remakes on draw, `gear_fixes`, the in-hand bit writes of puppet mode), the skeleton record and
   pose copies (`coop_proxy_put_records`, `coop_puppet_before_writer`, pose packets for Links, `snap`, the shake
   and body-top logs). Keep: `coop_puppet_place` (the stand-in is placed by it), gear made once (`regive_gear`),
   `gear_hand`, NetState and its slots.
   **Before this goes, on the PlayerPuppet:** riding objects (`ride_place`, `ride_carry`), shield surf and the fused
   shield's scale (`shield_place`, `proxy_fuse_scale`), the glider (a moved object's copy now), lightning sparks,
   life shown on it, the name tags (check they find it), its arrows, and the audit's checks of a co-op Link
   (sync_audit.py), and the suite cases that test the old one (`tour`'s swings, `equip`, `hands`) made for it.
4. **One-off probes** (104 `TOTK_COOP_TEST_*` switches in runtime/coop_*.c): each whose finding is written down
   here or in re-notes.md and that no suite case uses.

## Wanted (the tester, 2026-11-07)
- **Map and minimap markers** for the other players (2026-11-07). Full map: works (pair test, merge 9f4178d: B's map
  showed A's dot named "A" about where A walked, and "(you)" by B's arrow). Minimap: wrong in that test, every marker
  held at the corner of its square, because the ground pane's matrix puts this Link at (0, 0) there, not at the
  minimap (765, -360): the minimap is the map drawn centred and shown in the corner as a picture. Fixed on branch
  `claude/coop-markers-fix` (not run yet): on the minimap a marker goes through N_IconArea_00's matrix, as the
  game's own icons do (moved to (765, -360), scale 0.9), and is held on the minimap's circle. Expect "off" near 0
  and the "(you)" dot on B's arrow in the minimap frames; whether 0.9 is the right scale shows as A's dot matching
  where the minimap's terrain puts A.
  - What it does: runtime/coop_markers.c finds the game's map screen (AppMap_00, which is also the HUD minimap) and
    reads the global matrix of its ground pane, then ui/coop_markers.cpp draws a coloured dot and the player's name
    for each other player with the overlay, clipped to the map's area; a player off the view is a ring on its edge.
    The minimap shows the dots alone, no names (the tester, 2026-11-07: the dot's colour says who it is). The name tags over the Links are hidden while the full map shows. Nothing is written
    to the game or the save: no pin, no stamp, no pane. `TOTK_COOP_MARKERS=0` turns it off. Evidence and offsets:
    re-notes.md "Map and minimap markers".
  - New hook: main+0x96c1ac (the map screen's set-up) in COOP_HOOKED, so the generated code rebuilds.
  - Unproven: that the minimap is the AppMap screen in its MiniMap pose; the 3 layout units a metre; that a hidden
    minimap (menus, cutscenes) shows as a hidden or transparent pane (if the game stops drawing the screen instead,
    the markers stay on screen there); the layout-to-screen scale on a screen not 16:9 (assumed: the layout's 1080
    rows fill the height, centred); that this PC's Link ("Player") is a new actor after a save load and absent on
    the title, which is how coop_markers.c `resolve` drops a freed map screen (log "coop: markers: this Link is
    gone/another one: the map screen ... is taken as gone"). If the game keeps the map screen across a load without
    setting it up again, the markers stay off until the next set-up.
  - Test: `$env:TOTK_COOP_PUPPET=1; tools\coop\run_pair.ps1 -Seconds 75 -ScriptA tools\coop\walk_far_3rd.txt
    -ScriptB tools\coop\map_markers_3rd.txt -MarkShotsB 30-62:2 -ShotsDir build\markers -MarkSelf` (overlay shots in
    build\markers\B-overlay). Expect in B's frames: before 40 s the minimap (bottom right) with a white dot on B's own
    arrow (MarkSelf) and a blue or orange dot about 20 m away for A with the name "A"; 41-55 s the full map with both
    dots and names, nearer each other after the zoom at 46 s; from 57 s the minimap again and no name tag drawn while
    the full map was open. B's log: "coop: markers: the map screen was set up", "map panes: root ..., N_Cut_00 ...,
    ground ..." (none 0), "the minimap shows (... layer), N_Cut_00 at (765, -360) 249x249 ... off <small>" and "the
    map shows ..." after 40 s. A large "off" on the minimap means the transform is wrong; a "no map shows" while the
    minimap is on screen means a pane check fails.
- **One colour per player, and name tags inside the frame.** Written 2026-11-07 (the tester's ask), branch
  `claude/coop-player-colours`.
  - Colours: ui/coop_colour.h, one colour per player id (the relay gives ids lowest free first, so a player has the
    same colour on every screen): the name tag, the name in a notice, the map dot and name, the minimap dot, and the
    names in the Co-op tab's "Here:" line.
  - Tags in the frame (runtime/vk_tags.c, shaders/coop_tag_vs.vert, coop_tag_fs.frag): the replay draws the tags into
    the game's HUD target right before the HUD's first draw, so the HUD, the map and the game's menus cover them, and
    each tag fades out where the scene's linear depth around its anchor is nearer than the player (behind walls and
    hills). The anchor is projected with the frame's own camera block, not the overlay's 50-degree guess. Where the HUD
    starts was read from a frame dump (vk_tags.c's header). While the replay finds no HUD start for 30 frames the
    overlay draws the tags over everything as before; `TOTK_COOP_TAGS_INFRAME=0` keeps it that way.
  - Logs: "vk: co-op name tags: the HUD target is 3840x2160 (format 122), copied into an sRGB window texture" once,
    then "vk: co-op name tags drawn in the frame: HUD target ..., far <f>, hidden by the scene depth" at the first
    tag drawn. "nothing drawn (...)" says what was missing. With TOTK_COOP_TEST_TAGSELF the "coop: tag ..." lines end
    in "(in the frame)" or "(overlay)".
  - First run (a play session, 3840x2160, 2026-11-07): both games hit a GPU fault and device lost right after the
    first tags were drawn. vk_tags.c called vkCmdSetViewport, vkCmdSetScissor and vkCmdBindVertexBuffers, which the
    recorder thread has no packets for, so they ran at once, out of order and racing the recorder on the same command
    buffer. Fixed on `claude/coop-tags-recorder` with the *WithCount and *2 forms. The HUD target also flipped
    between frames to mid-frame copies into plain RGBA8 targets (1636x920, 2048x1152); only an sRGB copy counts now.
    Compile check only.
  - Not run with tags yet (the frame structure was read from one dump at a 4096x2304 scene and 3840x2160 output). To
    check: a tag over a player in the open; the same player behind a wall (the tag fades out); the inventory or the
    map opened over a tag (the tag is under it); a dialogue box over a tag; the far plane in the log above 1. Not
    checked: a scene as large as the output (the tags go after the game's FXAA draw-back, from vk_dlaa.c's notes,
    not a dump), DLSS modes, HDR (the HUD target is read as linear colour there too).
- **Notices** when a player joins or leaves, and when one fast travels, with the place's name. Written 2026-11-07
  (runtime/coop_notices.c, ui/coop_notices.cpp; `TOTK_COOP_NOTICES=0` turns them off). **Not run in the game yet**:
  compiled with clang, and the place loading and naming ran on the real 1.4.3 files in a host test (below).
  - **Shown with the port's overlay**, a line at the bottom left of the screen for 6 s (moved from top centre, 2026-11-07) (drawn as the name tags are), not
    the game's banners. The game's location banner (layout `Pa_LocationName`, opened by the sequence node
    `SeqOneShotRequestOpenLocation`, main+0x1ff46f4 its name getter) takes a location, not a text, and no way to put
    our own text in it was found. The sentence is in the menu language (6 new menu texts in en, ru, pt-BR, zh-Hans);
    the place's name is in the game's language, and with a Japanese, Chinese or Korean game its glyphs are added to
    that language's system font (fonts_up).
  - **Joins and leaves**: each overlay frame compares `coop_net_players` with the last frame's. A join waits up to
    3 s for the player's name (the relay's WELCOME). Players already there when this game takes its seat (the first
    5 s) are "is here", not "joined". Leaving the session yourself, or the session ending, shows nothing.
  - **Fast travel, detected by its effect (fallback)**: the game's warp request was not found (looked at: the
    sequence's `WarpMgr` name getter main+0x1fd1bf4, `Game/WarpInfoTable` and its warp events `DmF_SY_WarpOut` /
    `DmF_SY_WarpIn`, the `SeqQueryGetCurrentLocation` node). A travel is this Link more than 250 m from where it stood a
    tick before, not within 20 s of its life reading 0 (a death's respawn), and 1.5 s later within 40 m of a shrine,
    Skyview Tower or lightroot. Elsewhere (a travel medallion, a save loaded with the clock still running) it is
    "is now near" the nearest named place within 600 m (shops and districts left out), or nothing.
  - **Place names** come from the game's map locations: `Banc/MainField/LocationArea/MainField.locationarea.byml`
    and MinusField's (1256 + 303 points; `LocationName` "Work/Location/<label>.game__location__Location.gyml"),
    named from `Mals/<lang>.Product.<n>.sarc.zs`: `LocationMsg/Location.msbt` by label, shrines (`Dungeon<nnn>`) from
    `LocationMsg/Dungeon.msbt` label `0<nnn>`. Russian and other languages keep a name's grammatical forms in tag
    group 0xc9 type 10; the first form is used. The host test named 1533 of 1559 points in each of the 15 languages
    (the 26 left: Dragon's Tears, minigame goals; no travel point among them), e.g. Tower01 "Lookout Landing Skyview
    Tower", Dungeon000 "Tajikats Shrine", CheckPoint012 "Kimnaz Lightroot", Japanese ruby left out.
  - **Wire**: the traveller sends the label (not the name) in a HIT packet of kind "TR" (`CoopTravelPacket`, 64
    bytes), three times 0.4 s apart; each receiver tells it once (by its count) in its own language. NetState is
    unchanged, so `COOP_NET_VERSION` stays 21: an older game drops the packet (no kind it knows).
  - **Test**: tools/coop/notices_3rd.txt (with -EnvA/-EnvB, new in run_pair.ps1) has the command, the log lines and
    the frames (`TOTK_COOP_TEST_NOTICESHOT=1` saves each notice's frame with the overlay as notice-<n>.png;
    `TOTK_SHOTS` frames have no overlay). `TOTK_COOP_TEST_TRAVEL=<s>[,x,y,z]` puts this Link beside Ukouh Shrine
    (or at x y z) with ForceSetMatrix, as a travel would, since a real fast travel cannot be scripted yet.
  - **Unverified / to check in the game**: that a real fast travel's load leaves the Link more than 250 m away in one
    co-op tick and standing within 40 m of the travel point's map marker 1.5 s after; whether leaving a shrine (its
    inside is another stage) reads as a travel to that shrine (a false notice); whether a death's game over reads
    life 0 before the respawn; whether a menu save load restarts the world clock (then no notice) or not ("is now
    near"); the frame layout (bottom left) against the game's own HUD and its location banner.
  - Entering a shrine (2026-11-07, the tester saw "A is now near Mabe Village Ruins" there): the elevator's jump lands
    on no travel point, so a jump that leaves from within 60 m of a shrine says "A has entered <shrine>" (near = 2 in
    the travel packet). Leaving that shrine (landing back at its travel point) says nothing. Compile check only. To
    check: enter a shrine and look for the notice and "into the shrine Dungeon<nnn>" in the traveller's log; leave it
    and see no notice ("out of the shrine").
- **Character mods**: branch `mod-pack-restbl` (30f0a50, 2026-11-04) is now on main (cherry-picked 2026-11-07,
  no conflict). With it the full Playable Riju mod (7795 files served, 2278 changed files inside 70 packs) loads into
  the world as Riju on this build (sandbox C:\Dev\totk-sandbox, its exe replaced by this build's; the old one kept as
  totk-sandbox.old.exe; its bisection subset moved to mods-bisect-subset). Combat, menus and co-op with it are not
  tried. Per-player characters (one player Riju, the other Link) are still to do (docs/coop/character-mods.md); the
  PlayerPuppet stand-in is the way in: tools/coop/make_puppet_pack.py builds it from the game's own Player.pack, so a
  stand-in could be built from a mod's Player.pack instead, per player.

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
