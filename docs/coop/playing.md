# Playing co-op (test builds)

## Online (any number of players up to 8)
1. Everyone loads a save, opens the menu (F1) and goes to the **Co-op** tab, and types a name.
2. One player chooses **Host a session**. That game runs the relay itself (`runtime/coop_relay.c`) and joins it. The
   tab then shows the address the others use on the same network, and asks the host's router (UPnP) to forward UDP
   port 41900 to this PC for players over the internet: when it works, the tab shows the internet address to give
   them. When the router has UPnP off, or the provider shares one address among customers (CGNAT, the tab says so),
   forward UDP 41900 by hand or use a virtual network (Tailscale, ZeroTier). The forward is removed on Leave. If the
   game is closed while hosting, a forward with the usual one-hour lease expires within the hour; a router that only
   takes a lease "until removed" keeps it until it is removed by hand.
3. The others type that address under **Host's address** and choose **Join**. Players appear in each other's games
   as they connect and disappear when they leave (**Leave the session**, or closing the game). When the host leaves
   the session ends for everyone.
4. The name and the last address are kept in `totk.ini` (`coop_name`, `coop_join`).
5. The others' names show over their Links (**Show player names** in the tab, `coop_tags` in `totk.ini`), and they show
   on the map and the minimap (`coop_markers = 0` hides them). A line on screen says when a player joins, leaves or
   fast travels (`coop_notices = 0` hides it). The name and the address fields open an on-screen keyboard, so a
   gamepad is enough. When the host leaves, a guest's tab says the session ended, and the guest's game goes back to
   its own time of day and weather.

Everyone needs the same build (the packet version must match) and the same game update. A standalone relay still
works: `python server/coop_relay.py` on a PC everyone can reach, and `coop_relay = <address>` in `totk.ini` joins
it at start. The test scripts and `play_two.ps1` use this relay, not the in-game one.

## What works

- **Other players.** Each player shows in the others' games as a stand-in (a PlayerPuppet actor, TotK Online's kind;
  docs/coop/facts/player.md) that plays its owner's animations and final pose, is placed where its owner is a moment
  behind, and goes away when its player leaves. It holds and carries its owner's weapon, bow and shield, fused parts
  included, wears their armor and dyes, and sparks when lightning is about to strike its owner. Guests follow the
  host's time of day and weather.
- **Enemies.** Every enemy is run by one game, its owner; in the other games it is a copy that does not think: it is
  placed and animated as the owner's enemy, holds and draws what it holds, shoots, throws and shows its effects as the
  owner's does. An enemy near more than one player stays with the game that has it (a player who was fighting alone
  keeps the fight). It moves to another player's game when that player's Link is at least 6 m nearer to it than the
  owner's Link and within 35 m, after the owner has had it for 6 s; or sooner when that player strikes it (within
  80 m, after 3 s). It also moves when its owner leaves, loads, or stays in a menu. An enemy fights the player of the
  game that runs it. Your blows, arrows and bombs on a copy count: the life they take is sent to its owner. A copy's
  blows on you count, knock-back included, and are decided in your own game, so a raised shield and a perfect guard
  work as the game decides them there. Death, drops and kill counts happen once, in the owner's game. Enemies made at
  run time (a night's Stalkoblins, a spawner's) are made in the other games too. Sneakstrike and lock-on prompts on an
  enemy follow its owner's. docs/coop/enemy-sync.md has the details.
- **Contraptions.** Zonai devices, Autobuild builds and anything glued to them are kept the same in every game. The
  game that runs a contraption moves it; the others show a copy moved toward it. A part taken off, glue made or
  broken by hand, and two contraptions at once stay right in the other games. Grabbing another player's contraption,
  or taking its steering stick, makes your game run it once that player's game agrees (not while they hold or ride it).
  Turning it on or off (the stick held or let go, a hit) is followed in the other games, and a hit on a device another
  player runs switches it there. Wheels, sticks, springs, time bombs and burning parts follow too. Whether beam
  emitters, batteries and hydrants follow is not known. Its copies are green as the builder's are.
  docs/coop/entity-sync.md has the details.
- **Character mods.** Each player can play their own character mod: the Co-op tab shows every player's character,
  looks a mod up from a GameBanana link, and offers to fetch a party member's character (it asks first, takes RomFS
  files only, and a fetched character starts switched off for your own Link). A party member's character fetched during
  the session shows on their stand-in at once; changing your own Link's character still needs a restart. The others
  show your stand-in with your character's own body and textures; armor refitted for a character is not shown yet
  (such a stand-in wears its character's bare body). Zip and 7z archives work; rar is untested.
  docs/coop/character-mods.md has the details.

Not yet: menus for remote players, arrow trails on the others' arrows. Chests stay each player's own.

Off by default, not for play yet: horses and animals (`coop_mounts = 1`), fire, ice and lightning shared between the
games (`coop_elem = 1`), shared world progress (`coop_share_world = 1`).

## One PC, two games (online co-op without a second PC)
Double-click `Play co-op (two pads).vbs` in the repo folder (or run `tools\coop\play_two.ps1`): it starts the relay
(`server/coop_relay.py`) and two game windows side by side, with no console windows. Load the save in both.

- **One gamepad** (what the `.vbs` starts): the first gamepad moves both games. Minus with the right stick click, or F9
  in any window, switches to moving only the game whose window has the focus, and back.
- **Two gamepads**: run `tools\coop\play_two.ps1 -TwoPads`. Window 1 reads the first connected gamepad, window 2 the
  second (the log lists the gamepads it sees: "input: gamepad 0 of 2"). Wake both gamepads before starting.
  `pad_index = <n>` in `totk.ini` picks the n-th gamepad (0 = first) for a game started by hand.

Both games play copies of the save (`build\prof-play-1` and `-2`): nothing is saved back to the real profile. The
copies are kept from one start to the next; `-Fresh` makes them again from the real profile, and `-FrozenSaves` gives
both the real profile's saves again at each start. The launcher frees the mouse (`mouse_sens = 0`: no mouse camera, no
cursor capture), hides the consoles (`coop_hide_console = 1`) and turns on the session's records
(`TOTK_COOP_DEBUG=1`). Game 1's log is `build\v143\logs\totk.log`, game 2's `build\v143play\logs\totk.log`. Home or
numpad 0 in any window writes a mark in both logs, with a picture each.

## Records

A plain game writes no session records. `TOTK_COOP_DEBUG=1` (or `coop_debug = 1` in `totk.ini`) turns them on: the
sync audit file, the contraption session record, the enemy position lines and the stand-in's measurements that the
reports in `tools/coop/` read. The launchers below and `play_two.ps1` set it.

## Test tools
- `tools/coop/run_test.ps1`: one game, scripted input, a copy of the profile.
- `tools/coop/run_pair.ps1`: two games on this PC in one session through a local relay (`server/coop_relay.py`).
