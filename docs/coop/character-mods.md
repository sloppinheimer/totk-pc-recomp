# Character mods in co-op (research, 2026-11-04)

Read-only look at the "Playable Riju Mod" (GameBanana 627813, file rijumod_140_a5b2b.zip, MD5
aa9918fea8fd0adc5204fcc94ff99ff9, kept in C:\Dev\downloads\riju-mod). Nothing installed or run yet.

## What the mod is

romfs only: no exefs, no executables. 7862 files, mostly UI (6315) and textures (722).
- Riju's body replaces Link's own body model: `Model/Link.Link_RaulSkin.bfres.mc` and
  `Model/Link.Link_RaulSkin_RightHand.bfres.mc`, plus `Pack/Actor/Player.pack.zs` and `AS/Player.root.asb.zs`
  (her animations).
- 332 armor packs and 65 armor model groups are re-fitted to her body (heads, uppers, lowers).
- New items (Weapon_Sword_RJM_002, Weapon_Shield_RJM_001), Gerudo Town logic and events, GameData.

So it is global: every Player actor in a game, the remote players' Links included, becomes Riju.

## Version problem on 1.4.3

The mod ships its resource tables as `RSDB/*.Product.140/141/142.rstbl.byml.zs`. The 1.4.3 romfs has
`RSDB/ActorInfo.Product.143...` (checked in build\v143\romfs-index.bin), so on 1.4.3 the game would not see the
mod's table edits: its new actors (the Scimitars, edited actor info) would be missing. The models and packs it
replaces would still load. Fix: apply the mod's 142 table changes onto the 143 tables (needs the original 1.4.2
tables to diff against; only v0 and v143 are extracted here) or let TKMM merge it for 1.4.3. Untested.

## Per-player characters (one player Riju, another Link)

Models are local files, so every player needs the mod installed; nothing is sent but names.
1. A second player actor: copy the mod's Player.pack under a new name (e.g. `Player_Riju`) whose model points at
   renamed body files, and give it an ActorInfo entry. Our spawn then requests that name for a player who
   announced "Riju" in HELLO. The real Player stays Link. Risk: game code that finds the player by the name
   "Player" (our own code does: coop_resident_named("Player")); not checked in Ghidra yet.
2. Armor: the Riju-fitted armor replaces the same armor names Link uses, so per-player needs a renamed copy of
   each fitted piece (Armor_001_Upper -> e.g. Armor_001_Upper_RJM) and the gear sync mapping a Riju player's
   items to those names. Scriptable (rename packs and their internal references) but large: 332 packs.
3. Animations: Player.root.asb is per actor, so the copied pack can keep the mod's.

## Test on 1.4.3 (2026-11-04, sandbox copy C:\Dev\totk-sandbox, run.ps1 there)

The sandbox is a copy of build\v143 with its own exe name (totk-sandbox.exe, so the test scripts' "close every
totk.exe" leaves it alone), profile, mods folder and logs, and a copy of the shader translator (without TOTK_XLATE
pointing at it every frame is black: "translator unavailable"). It boots the saves fine.

With the mod: the port takes it ("7793 files served (6634 replaced, 1159 added), 31 for another game version"), then
the game crashes at boot before the title: access violation writing address 0 in main+0x10584a0, called on the
actor-creation path (main+0x2110f8 <- 0x8928ec <- 0x3d4410, the one Link's gear is built on). Copying the mod's
Product.142 RSDB tables to Product.143 names did not change it. Not yet known whether the cause is the mod's 1.4.0-1.4.2
files on 1.4.3, a heap too small for its larger files, or the port's mod loader.

Bisection (sandbox bisect.ps1 / autobisect.ps1, a second copy C:\Dev\totk-sandbox2 for parallel runs):
- Everything but Pack loads into the world (7420 files: models, textures, UI, events, sound, AI, RSDB).
- Pack/Actor/Player.pack.zs alone crashes; so does Armor_999_Head.pack.zs (Link's bare head) alone. Every other
  pack, alone or in halves, loads.
- Inside Player.pack (hybrids of the 1.4.3 pack with single members from the mod, scratchpad hybrid.py; a rebuilt
  unchanged pack loads, the mod's full pack crashes): 4 of its 13 changed members crash alone, and each names a file
  the mod adds elsewhere: ActorParam -> AI/AIInfo/PlayerAIMerge (crash main+0x10584a0, the full mod's crash),
  BlackboardParamTable -> LightningChargeAmount, AnimationParam -> Model/Riju_Animation(_SpearFix), PointLightParam
  -> its ChargeAttack light (main+0x173130). The other 9 (AttackParam, EquipmentUserParam, ...) load.
- The whole mod with only ActorParam put back to 1.4.3's still crashes, now at main+0x173130 (the light). So the
  mod's own files are there but what they add does not load on 1.4.3: not one bad file but every new reference.
- Ruled out: the resource size table. The port's merged table lists the added files (PlayerAIMerge 17 KB,
  Riju_Animation 94 MB estimated, 1088 entries more than the game's).
- SOLVED (same day): the resource size table entries of the files *inside* the mod's packs. The crash sites are
  the game's parameter reader (main+0x173130, ppPropBuffer) and its neighbours writing into an allocation that came
  back null: each resource gets memory from its size-table entry, and the port's merge (romfs.c restbl_overlay) only
  raises entries for files served on their own, not for pack members. The mod's grown members (Player.pack's
  ActorParam, PointLightParam, Armor_999_Head's, ...) overflow their 1.4.3 entries. The mod ships no table (TKMM
  makes one). A table with every pack member's entry raised (scratchpad restbl_members.py: the 1.4.3 table, each
  member at max(entry, size x factor + 8 KiB), 2693 raised and 36 added, shipped in the mod as
  System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs) makes the whole mod load: Riju in the
  world on 1.4.3 (C:\Dev\totk-sandbox2). The RSDB 142 -> 143 copies are still in that sandbox; untested whether they
  are needed.
- Fixed in the port (runtime/romfs.c, pack_members_raise): the size-table merge opens every pack a mod serves,
  compares each member with the game's pack and raises the changed ones (restbl_grow over the game's entry, else
  restbl_estimate). The plain mod as downloaded (no table, no RSDB renames) then loads into the world as Riju: "2380
  changed files inside 76 packs". Combat, menus, the Scimitars and Gerudo Town are untested.
- Earlier leads, now moot: why new references fail on 1.4.3. Leads: the 94 MB animation estimate against the heap; a 1.4.3 change in
  how AIInfo/light params are read; the port's reading of mod files that sit inside packs. A 1.2.1 test would split
  "the mod on 1.4.3" from "the port" but needs a 1.2.1 dump (only v0 and v143 are extracted). Ideas: the mod's 1.2.1 build on a
v121 build of the port (the port supports 1.2.1; co-op does not, its addresses are 1.4.3's), or bisecting the mod's
folders (Pack, Model, AS, RSDB) to find the file that crashes.
