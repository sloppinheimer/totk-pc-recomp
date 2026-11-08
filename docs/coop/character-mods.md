# Character mods in co-op

## Design (2026-11-08)

Each player can play a different character: one player as Riju, another as Link. Each game shows its own Link as its
player's character, and every other player's stand-in as theirs. Tested with the Playable Riju Mod on 1.4.3, two games on
one PC (`tools\coop\run_pair.ps1`).

### What works

- **This PC's character** is the character mod switched on in the mods folder; its files are served as Link's own, like
  any mod. A character mod is a mod folder whose RomFS has `Pack/Actor/Player.pack.zs` or
  `Model/Link.Link_RaulSkin.bfres.mc`. The Co-op tab lists the character mods with a switch each; switching one on
  switches the others off, from the next start.
- **Other players' stand-ins.** `tools/coop/charmod.py sets` gives every character mod in the mods folder, and the game's
  own Link, a *set* in the mod `mods/zz-coop-characters`, each under a four-character tag made from the character's key:
  - `PlayerPuppet_<tag>`: a stand-in actor made from that character's `Player.pack`, as `make_puppet_pack.py` makes
    `PlayerPuppet` from the game's. Its members that `Player.pack` also has go in under names of its own, and so does its
    animation set (`AS/PlayerPuppet_<tag>.root.asb`);
  - its body model copied as `Model/<tag>.<fmdb>.bfres.mc`, with the copy's `ModelInfo` pointing at it. In the copy
    every texture is renamed (the tag in place of its first four characters, the `.txtg` copied under that name) and
    every material `Mt_<x>` becomes `<letter>t_<x>`, so the stand-in shows its own character's textures in any game
    (Textures, below);
  - the bare pieces Link wears with no armor (`Armor_999_Head`, `Armor_Default_Upper`, `Armor_Default_Lower`) as
    `<piece>_<tag>` actors with their own model copies;
  - the files the character mod *adds* (new names: animations, textures, AI), so that a game where the mod is off has what
    the stand-in needs;
  - a row for each new actor in `RSDB/ActorInfo` and `RSDB/GameActorInfo`, inserted in name order in both tables. (The
    PlayerPuppet note in `make_puppet_pack.py` says an added row crashed the game; inserted in place, in both tables at
    the same index, rows load and their actors spawn.)
  `characters.tsv` in that folder lists the sets for `runtime/coop_chars.c`.
- **The network.** `NetState.character` carries each player's key ("Link", `gb:<GameBanana file id>`, or the mod's folder
  name) and `char_mod` its GameBanana mod id (net version 28). A stand-in is spawned as its owner's set
  (`coop_players.c`, `spawn_slot`); for a character this PC has no set for, it stays the plain `PlayerPuppet`.
- **Armor.** This game only has armor fitted to its own Link's character: the Riju mod replaces 332 armor packs under the
  game's names. A stand-in of another character wears that character's bare pieces instead of its owner's armor, with
  no dye. A stand-in of the same character as this game's Link wears its owner's armor as before.
- **The Co-op tab** shows this PC's character and each player's ("Playable Riju Mod", "Link", or the mod's name and "not
  on this PC").
- **Fetching.** For a party member's character from GameBanana that this PC lacks, the tab looks the mod up
  (`charmod.py info`: GameBanana's API, no download) and asks with the mod's name, file, size and page. Nothing is
  downloaded before a yes. On a yes, `charmod.py fetch` downloads that file, checks its MD5 against GameBanana's, and
  refuses an archive with code patches or cheats (`exefs`, `.ips`, `.pchtxt`, `.nso`, `.npdm`, `subsdk*`, `main`,
  `cheats`) or with a path that would leave the mod folder (absolute, a drive, `..`). It unpacks only what is under the
  archive's `romfs` folder (zip in Python, 7z and rar through Windows' `tar`) and makes the sets again. The folder gets a
  `coop-character-off` file, and `runtime/mods.c` keeps such a folder off until the mods order names it (the menu writes
  the order when the folder is switched): so it stays the other player's character. A GameBanana link or mod id typed
  into the tab goes the same way and lands off too; its row under Characters switches it on as this PC's character.
  `charmod.py info --file auto` picks the mod's file for game 1.4 and skips TKMM packages. The job runs detached and the
  game polls its status file every frame (`coop_chars_tick`), so it finishes with the menu closed.
- The new set shows after a restart: the RomFS overlay is read at start. A character made this PC's from the tab needs
  that restart too.

### Textures (solved 2026-11-08)

The Riju mod replaces Link's own textures under their own names (`Link_RaulSkin_Alb`, `Link_Head_Alb`, the belt, the
eyes), and the game finds textures by name in `TexToGo/`, so one game holds one version of each. Each set therefore
renames the textures inside its models and copies the files under the new names. That alone made the game refuse the
stand-in; what it takes:

- **A model's `.bfres.mc`** is "MCPK", two version bytes, a word, a zstd frame without its magic holding the BFRES (names,
  materials, texture names), then the mesh data (`FMSH`). Same-length edits to the BFRES's strings, recompressed, with
  the mesh data kept as it is, load (dropping the mesh data made the game refuse the actor).
- **Materials are kept by name.** A second model with a material named like one of this game's Link's (`Mt_Belt_A`) whose
  textures differ was refused; the same edit with that material renamed was made. So the set renames every material.
- **The rename keeps the second letter.** With materials renamed to `Lt_`, `Xt_` or `WW_` the stand-in was made; with
  `Ln_`, `LN_` or `Mn_` it was refused, though every dictionary lookup still worked. Some lookup tests bit 3 of the second
  character; the set uses a capital letter and `t` (`charmod.py prefixes`).
- **Dictionaries.** A BFRES's dictionaries (ResDic: count, then nodes {bit, left, right, key}) find a name by testing
  bits counted from its end (all 223 keys of Link's body are found that way). The materials are keys of one, so the
  tool runs the lookup on every key after an edit and takes the next prefix when one fails.
- The texture resolver (main+0x52ddd0) builds `TexToGo/%s.txtg` and falls back to `DummyTextureForEvent` when a texture
  is missing: a missing texture never refused an actor.

Tested in a pair: game B (Link) shows the Riju stand-in with her own hair, skin and crown, game A (Riju) shows the Link
stand-in with Link's hair and skin.

### Loading a party member's character while the game runs (2026-11-08)

A character fetched in a session shows on its player's stand-in at once, with no restart:

- **Slots.** `charmod.py sets` reserves four stand-in slots (`SLOT_TAGS`: `JS01`, `KS02`, `XS03`, `ZS04`): rows for
  `PlayerPuppet_<slot>` and its bare pieces in the actor tables from the start (the game reads those tables once), and
  `slots.tsv`. The fetch then also builds the character's set under the next free slot's tag (`charmod.py slot`) into
  `coop-live/<slot>` next to `totk.exe`. Each slot is used once a run.
- **Files.** `romfs.c romfs_live_add` adds every file of that folder the RomFS lacks as a file of its own, numbered after
  all the files known at start (nothing is renumbered, open handles stay valid), in an append-only table that readers
  use without a lock.
- **Sizes.** The game loads a file into a buffer sized from its resource size table entry, and a file with no entry
  crashed it on load (the stand-in's model, `TOTK_TEST_RESTBL_SKIP`). The game keeps the table in its heap exactly as
  the file is laid out (one `RESTBL` header found in the heap) and reads it on every load: an entry set too small there
  made it refuse the stand-in. So the served table has 16384 spare entries (hash `0xffffffff`, size 0) after its hashed
  ones (`romfs.c restbl_spares`), and `coop_chars.c restbl_guest_add` merges the new files' entries (estimates, pack
  members included) into the game's copy in one write: a hash it has is raised, a new one takes a spare, the entries
  stay sorted.
- **The stand-in.** The character is added to the list; `coop_players.c` sees that its player's stand-in is now another
  actor, deletes the old one, and the existing "went away" path makes the new one. A remade stand-in starts with none
  of the gear its slot gave the last one, so it gets its character's bare pieces again (without that the new Riju
  stand-in wore this save's armor and Link's head over her body).

Tested in a pair: game B (Link) asked about player 1's Riju, the test answered yes and took the Riju zip on disk
(`TOTK_COOP_TEST_CHARYES`, `TOTK_COOP_CHARFETCH_FILE`), the job unpacked it, built the sets and slot `JS01`; 737 files
and 780 table entries were added and the stand-in became Riju, in the same run. `TOTK_COOP_TEST_CHARLIVE=<mod folder>`
fills a slot from a character mod already on disk, without the fetch.

**A second change, and back to Link.** A player's character as their states say is followed while the stand-in lives:
a new key (they switched character mods and came back) or a new set for the same key remakes the stand-in. Tested in
one run (`TOTK_COOP_TEST_CHARLIVE_AGAIN=1`, `TOTK_COOP_TEST_CHARKEY=Link@70` on the other game): Riju in slot JS01 at
10 s, Riju again in slot KS02 at 40 s (90 files added: the mod's own 647 were in already), Link at 70 s; each old
stand-in went away before the new one was made. A slot is used once a run and not freed (the game may keep what it
loaded under its name); the files added stay in the RomFS and the entries in the table until the game closes
(908 of the 16384 spare entries after the three changes). With all four slots used, the next character waits for a
restart.

**This PC's own character needs a restart.** The Player actor is made once, before the title screen: its pack
(`Pack/Actor/Player.pack.zs`), body model (`Model/Link.Link_RaulSkin.bfres.mc`), animation set
(`AS/Player.root.asb.zs`) and skin texture were opened 3.3 s after start. Loading another save from the menu
(`tools/coop/load_3rd.txt`) kept the same Player (its address unchanged, `coop: this PC's Link is the Player at ...`
logged once) and moved it 5061 m; only the armor packs were opened again. The System tab's **To Title Screen** and
Continue (`tools/coop/to_title_3rd.txt`) did not make it again either: the same address, and the four files opened
only at 3.1 s. So nothing in a run makes the Player again or reads its files again, and its resources stay loaded as
long as it exists.

**Hiding this PC's Link behind a stand-in of its own character**: no switch in the game that hides the Player's model
was found (the tries below), but the renderer can leave its models out (This PC's character in place, below).

- The actor's scale (+0x2e4, which shrinks a Zonai beam's copy) set to 0.02 every tick left the Player drawn at full size.
- The scope (right stick press) hides Link. `TOTK_COOP_TEST_MODELDIFF=18,25` read the Player actor, the heap objects its
  first 0x800 bytes point to and the ones those point to (456 objects), before and in the scope, against a run without
  it. Each flag-like byte and each float that went from 1 to 0 in the scope only was then held at its scope value every
  tick while Link walked (`TOTK_COOP_TEST_HIDEBYTE=<a>[/<b>],<offset>,<value>[,<from s>]`): none hid him. The tries were
  `[actor+0x168]+0x3d8`, `[actor+0x188]+0x199` and `+0x1b1`, `[[actor+0x88]+0x1f0]+0x112`,
  `[[actor+0x238]+0xc8]+0x21c`, `[[actor+0x3b8]+0x40]+0x140`, and the floats `[[actor+0x88]+0x1f0]+0x110`,
  `[[actor+0x608]+0x8]+0x2c8` and `[[actor+0x608]+0x1a8]+0x190`.
- The first two of those floats are the armor dye's blend: at 0 the worn tunic shows its save dye (10, pink), the pink
  tunic seen once after a stand-in was made again.
- A trace tick by tick (`TOTK_COOP_TEST_MODELTRACE=17,20,24,28` with `tools/coop/scope_3rd.txt`: the scope on at 20 s,
  off at 24 s): 1551 objects up to three pointers from the Player read every tick; 492 bytes changed exactly twice, at
  the scope's start and end. Most are pointer lists emptied and refilled (under `[actor+0x4e8]`, `[actor+0x48]/0x18`)
  and float bytes; the ones that look like states switch at 20.0 s and back at 24.0 s:
  `[[actor+0x238]+0xc8]+0x1d4` 1 → 2, `[[[actor+0x88]+0x1a0]+0x70]+0x1ec` 1 → 2, `[[[actor+0x88]+0x1c0]+0x70]+0x128`
  and `[[[actor+0x88]+0x1c0]+0xa8]+0x118` 2 → 3, `[[actor+0x130]+0x1a0]+0x88` 0 → 0x34, plus
  `[[actor+0x4d8]+0x138]+0x135` 3 → 1 and `[[actor+0x190]+0x8]+0x135` 0x20 → 0x1f a few frames later. All seven held at
  their scope values together (`TOTK_COOP_TEST_HIDESELF`) left Link drawn while he walked.
- Likely the scope changes the camera and the HUD state and Link is out of a first-person view, not hidden, so it is a
  poor probe. Next: a moment that hides Link in a third-person view (the start of Ascend under a ceiling, a cutscene),
  or Ghidra on the model unit's or material's visibility setters (the GhidraMCP backend was not running for this).

**Slots are not freed.** A stand-in made again under an actor name the game loaded before opens none of its files
again: after Riju (slot JS01) → Link → Riju in one run (`TOTK_COOP_TEST_CHARKEY=Link@40,gb:1600822@70`), the second
`PlayerPuppet_JS01` read no pack, model, animation set or texture, and showed Riju from what the game kept. A deleted
stand-in's resources stay cached, so a slot refilled with another character's files would show the old one; each slot
is used once a run, and switching back to a character already loaded costs nothing.
Serving other files under those names (the live overlay) would therefore not show either.

### This PC's character in place (2026-11-08)

The question: can this PC's own Link become another character while the game runs, with no restart and no save
reload? **Yes, by the renderer**: it can tell the Player's models from everything else and leave them out, which the
experiments below show; what is left to build is a stand-in of the new character in the Player's place. The other ways
are ranked after it.

**1. Leave the Player's models out in the renderer, draw a stand-in instead (tried; feasible).**

- How a draw is named. The game makes a memory pool for each model file it loads, and the pool's storage is the
  file's own page-aligned copy in memory: for every model pool logged the storage was the address of the file's
  `FRES` header (`SystemModel.*`, `Terrain.*`, `Grass.*`, `Item_Rupee.*`, `Item_Parasail`, `Link.Link_RaulSkin`, ...).
  The header's file name (at the offset in its word 0x10) names the pool, and a draw's index buffer lies in its
  model's pool. `TOTK_TEST_DRAWSKIP=<name part>[,...]` (nvn_api_core.c, test_pool) marks the pools of the model files
  whose names contain a part, and the replay skips every indexed draw whose index or first vertex buffer lies in one
  (vk_exec_draw.c op_draw_idx). The skip applies in every pass, the shadow's included.
- `TOTK_TEST_DRAWSKIP=Link_RaulSkin` (game A's Link was Riju, the Riju mod on; idle; shots at 14 and 20 s): the body
  skin went (torso, arms, face); the hair (`Armor_999_Head`), the legs (`Armor_Default_Lower`) and the shield stayed,
  and nothing else in the picture changed.
- `TOTK_TEST_DRAWSKIP=Link_RaulSkin,Armor_`: seven pools marked (`Link.Link_RaulSkin`, `_RightHand`,
  `Armor_999_Head`, `Armor_Default_RaulSkin_Upper`, `Armor_Default_Lower`, `Armor_Default_Extra_00` and `_01`); the
  whole Link went, only the shield (a weapon model) was left in the air.
- Before the name was used: the body model file is opened on one thread and its pool made on another, among dozens of
  others, so neither the thread nor the order tells which pool is the model's. Skipping every pool of 64 KiB or more
  made after the open hid the head and torso; the pool sizes alone did not find it.
- Why it is exact for the body: `Link.Link_RaulSkin` and `_RightHand` are drawn for the Player only, since the
  stand-ins of Link are `PlayerPuppet_LnkV` with their own copy (`LnkV.*`) and their own bare pieces. The pieces this
  PC's Link wears are not: an armor model is shared with every stand-in that wears the same armor and with that armor
  on a stand or in a shop, and a weapon model with the same weapon lying in the world. A skip by name would hide them
  all. The Player's own draws of a shared model need telling apart from the others (by the per-model-instance buffers a
  draw binds, the skinning matrices for one; not tried), or this PC's Link wears only pieces of its own (bare copies
  like the stand-ins', which leaves its armor unseen while it is another character).
- Weapons can stay drawn: they hang on the Player's bones, which are where the stand-in's bones are if it copies the
  Player's skeleton pose (the POSE packets already carry it for the other games).

What it would cost to finish:

- The pools named at creation (the test's code, kept to model pools), and a skip list the game thread sets: the body
  and whatever this PC's Link wears. Small.
- The Player's draws of shared armor told apart, or bare pieces only (above). The one open research question.
- A stand-in of this PC's new character: the set loaded while the game runs (done, as for party members), the stand-in
  made beside the Player and fed this PC's own state and pose every frame, with no network delay, by the code that
  applies the other games'. Whether a stand-in collides with the Player is not checked.
- Shown when the Player would be: the game hides Link at times (the scope; cutscenes may too, not checked) and no flag
  for that was found, but the renderer sees whether the Player's body was drawn this frame even while it skips it, and can show or
  skip the stand-in's pool to match, a frame late.
- Back to Link: stop skipping and delete the stand-in. The other games already follow this PC's character key.
- Nothing in the game's state changes (camera, input, save, equipment), so a mistake shows as a picture, not a crash.

**2. Change the resource under the Player (not tried; partial at best).** The Player's model object was made for its
model's shapes, materials and skeleton; another character's model has other counts, vertex layouts and sizes (Riju's
`Link.Link_RaulSkin` is a 0x5cd28-byte BFRES with its mesh inside, Link's a 0x106b0-byte BFRES plus 0xeeac bytes of
mesh data), so pointing the object at the stand-in's model needs the game's model setup mapped (Ghidra) and may still
fault. Swapping buffers under the Player's draws in the renderer fails for the same reason: the draws' counts and
formats are the old model's. Only textures could be swapped one for one, by name in the renderer's texture cache: that
covers a retexture mod, not a new body. Large for the mesh, medium for textures only.

**3. Make the Player again (not tried; the largest and riskiest).** No path in the game makes the Player again (save
load and To Title Screen keep it, above). Deleting and creating it ourselves means finding everything that holds it,
the camera, input, the HUD and the game data among them, none of it mapped here.

**4. A reload in the game (none found).** The game keeps a resource loaded under its name, and an actor made again
under a name it loaded before reads none of its files (Slots are not freed, above); the Player never lets go of its
own. TKMM merges mods into one folder before the game starts, which is the restart this avoids.

Ranked: 1, then 2 for retexture mods only, then 4 and 3. 1 is feasible; the shared armor is its one open question.

**Dropped by choice (2026-11-08):** a stand-in played as this PC's own character was judged not worth it, so the
renderer route is not built and this PC's own character keeps the restart. The `TOTK_TEST_DRAWSKIP` hook stays.

### What does not work, and why

- **Armor fitted to another character** is not shown. A stand-in of another character than this game's wears its bare
  pieces. Copies of the 332 refitted armor packs per character could now be made the same way as the bare pieces, with
  their own textures and materials; not done.
- **Material and eye animations** of a set's models: the game looks for `Model/<project>.anim.bfres` and
  `AnimationEvent/Animation/<project>.anim.baev` for the new project name, which the sets lack, and
  `Phive/EyeTextureScroll/Player_Link...` names the eye materials by their old names. A stand-in's eyes may not move.
- **Two characters on one material prefix** (the same capital letter): if both are shown in one game their same-named
  materials could clash again. The letter comes from the tag, so with few characters it is unlikely; not tested.
- **The mod's game changes** (new weapons, Gerudo Town logic, events, GameData, its UI) are not in a set: they belong to
  whoever has the mod switched on. A party member's weapon that only their mod adds is not shown here.
- **Code mods.** A mod with ExeFS is refused by the fetch. Installed by hand it works for this PC's own Link as before.
- The fetch runs the repository's Python (`.venv`, `TOTK_COOP_PYTHON` to change it) and needs oead and zstandard. So it
  works in a development checkout, where co-op runs, not in a release install.

### Bisect of 2026-11-08 (a stand-in of Link, each step nearer the set; status 3 made, 4 refused)

| Variant | Status |
|---|---|
| PlayerPuppet's pack, new name, new table rows | 3 |
| + its members under names of its own | 3 |
| + the body model copied under a new project name | 3 |
| + the copy repacked (first frame recompressed, mesh data kept) | 3 (4 when the mesh data was dropped) |
| + all its texture names changed (tag in place of the first four characters), the textures copied | 4 |
| + only the head and eye textures changed | 3 |
| + only the `Link_RaulSkin*` textures changed | 4 |
| + only the common textures (`CmnTex_*`) changed | 3 |
| + only the belt and earring textures changed | 4 |
| + only `Link_Belt_A_Alb` pointed at the game's `Link_Belt_C_Alb` | 4 |
| + the copies' 0x1c values changed as well | 4 |

The head, eye and common textures probably only seemed to work because those materials are hidden under the head piece.
The cause of the refusals was the material names (Textures, above).

### Testing

```
.venv\Scripts\python tools\coop\charmod.py sets --mods build\v143\mods
powershell -ExecutionPolicy Bypass -File tools\coop\run_pair.ps1 -Port 41957 -ScriptA tools\coop\idle_3rd.txt -ScriptB tools\coop\idle_3rd.txt -EnvB "TOTK_MODS_DISABLED=Riju" -ShotsA w24-32:8 -ShotsB w24-32:8 -ShotsDir build\charshots
```

With the Riju mod as `mods\Riju`, game A plays Riju and game B plays Link. The logs say `coop: characters: ...` and
`slot 0: PlayerPuppet_<tag> requested`. `TOTK_COOP_TEST_SPAWN=a+b+c@18` spawns several actors 4 s apart (a set's actors
on their own). `TOTK_MODS_DIR` gives game B a mods folder without the mod and without its set, to see the download
prompt (`TOTK_MENU_TEST=3600:150` shoots the menu's tabs, the Co-op tab last).

The fetch without a download: `TOTK_COOP_TEST_CHARYES=1` answers the prompt yes, `TOTK_COOP_CHARFETCH_FILE=<zip>` takes
that file in place of GameBanana's (`charmod.py fetch --from-file`; its MD5 is still checked against GameBanana's), and
`TOTK_COOP_TEST_CHARLOOKUP=<link>` looks a link up as the tab's button does. Tested on 2026-11-08 with the Riju zip
already on disk, game B on a mods folder with only Link's set:

| Run | Result |
|---|---|
| A plays Riju; B is asked, says yes, takes the local zip | MD5 matched, 7824 RomFS files unpacked into `Playable Riju Mod`, sets made again with `PlayerPuppet_WWN4` |
| B again (restart) | the fetched mod off ("a character mod fetched for another player"); B's Link stays Link; A's stand-in is `PlayerPuppet_WWN4` (Riju's shape) |
| B with the lookup of `gamebanana.com/mods/627813`, yes, the local zip | the same fetch, the mod off |
| B with the order naming the fetched folder (as the tab's switch writes it) | B's Link is Riju; with A Riju too, each stand-in wears its owner's armor with Riju's own textures |

This PC's Link left out of the picture (the in-place change above):

```
powershell -ExecutionPolicy Bypass -File tools\coop\run_pair.ps1 -Seconds 45 -ScriptA tools\coop\idle.txt -Port 41993 -BDir build\v143d -ShotsA w14-20:6 -ShotsDir build\shots-drawskip -EnvA "TOTK_TEST_DRAWSKIP=Link_RaulSkin,Armor_"
```

The log lists the pools marked (`nvn: test: drawskip: pool ... in model file ...: marked`).

Not tested: a real download from GameBanana (each needs the tester's yes), a 7z or rar archive, the tab's switch pressed in
the menu (the run above sets what it writes), three or more players.

## Research (2026-11-04)

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
  Since the merge of upstream 0.3.13 (2026-11-08) that code is upstream's own fix for the same failure
  (romfs.c restbl_packs, restbl.c restbl_pack_raise, b672c64), which replaced pack_members_raise; the Riju mod has not
  been tried on it.
- Earlier leads, now moot: why new references fail on 1.4.3. Leads: the 94 MB animation estimate against the heap; a 1.4.3 change in
  how AIInfo/light params are read; the port's reading of mod files that sit inside packs. A 1.2.1 test would split
  "the mod on 1.4.3" from "the port" but needs a 1.2.1 dump (only v0 and v143 are extracted). Ideas: the mod's 1.2.1 build on a
v121 build of the port (the port supports 1.2.1; co-op does not, its addresses are 1.4.3's), or bisecting the mod's
folders (Pack, Model, AS, RSDB) to find the file that crashes.
