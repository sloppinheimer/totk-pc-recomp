# Playing co-op (test builds)

## Online (any number of players up to 8)
1. Everyone loads a save, opens the menu (F1) and goes to the **Co-op** tab, and types a name.
2. One player chooses **Host a session**. The tab then shows the address the others use on the same network, and
   asks the host's router (UPnP) to forward UDP port 41900 to this PC for players over the internet: when it works,
   the tab shows the internet address to give them. When the router has UPnP off, or the provider shares one
   address among customers (CGNAT, the tab says so), forward UDP 41900 by hand or use a virtual network (Tailscale,
   ZeroTier). The forward is removed on Leave; if the game is closed while hosting it expires within an hour.
3. The others type that address under **Host's address** and choose **Join**. Players appear in each other's games
   as they connect and disappear when they leave (**Leave the session**, or closing the game). When the host leaves
   the session ends for everyone.
4. The name and the last address are kept in `totk.ini` (`coop_name`, `coop_join`).
5. The others' names show over their Links (**Show player names** in the tab, `coop_tags` in `totk.ini`). The name and the address
   fields open an on-screen keyboard, so a gamepad is enough. When the host leaves, a guest's tab says the session ended,
   and the guest's game goes back to its own time of day and weather.

Everyone needs the same build (the packet version must match) and the same game update. A standalone relay still
works: `python server/coop_relay.py` on a PC everyone can reach, and `coop_relay = <address>` in `totk.ini` joins
it at start (the test scripts do this).

What works (2026-11-08): each player shows in the others' games as a stand-in (a PlayerPuppet actor, TotK Online's
kind; docs/coop/proxy-plan.md) that plays its owner's animations and final pose, is placed where its owner is a
moment behind, and goes away when its player leaves. It holds and carries its owner's weapon, bow and shield, fused
parts included, wears their armor and dyes, and sparks when lightning is about to strike its owner. Guests follow the
host's time of day and weather. An enemy near more than one player is run by the game that has had it longest (a
player who was fighting alone keeps the fight), and goes to another player's game when that player's Link is 5 m
nearer to it (within 30 m): an enemy fights only the player of the game that runs it; in the other games its copy runs its own AI and plays the owner's actions (TotK Online's way), and enemies made at run time are made there too; the others show its moves, weapons, life and death; their sword hits,
arrows and bombs count on it, and its hits on them count, knock-back included: the hit player's own game builds the hit from the contact (a raised shield does not block them yet). The host's HUD prompts stay on the host's screen. Not yet: menus for remote players, arrow trails on the others' arrows; riding is untested. Chests stay each player's own.
An autobuilt contraption acts as one unit in the other games too: one hit turns all its fans on there as well (the copies are bonded the game's own way, runtime/coop_bonds.c). A part taken off and two contraptions at once stay right there too. Its copies are not green yet, and contraptions glued by hand are untested.
Each player can play their own character mod: the Co-op tab shows every player's character, looks a mod up from a GameBanana link, and offers to fetch a party member's character (it asks first, takes RomFS files only, and a fetched character starts switched off for your own Link). The others show your stand-in with your character's own body and textures; armor refitted for a character is not shown yet (such a stand-in wears its character's bare body). Zip and 7z archives work; rar is untested. docs/coop/character-mods.md has the details.
## One PC, two games, two gamepads (online co-op without a second PC)
Double-click `Play co-op (two pads).vbs` in the repo folder (or run `tools\coop\play_two.ps1`): it starts the relay and two game windows with no console windows; window 1
reads the first connected gamepad, window 2 the second (the log lists the gamepads it sees: "input: gamepad 0 of 2").
Load the save in both. Both play copies of the save (build\prof-play-1/-2, fresh each start): nothing is saved back.
Wake both gamepads before starting. The launcher frees the mouse (mouse_sens = 0: no mouse camera, no cursor capture) and hides the consoles (coop_hide_console = 1); logs stay in logs	otk.log. Setting: `pad_index = <n>` in totk.ini picks the n-th gamepad (0 = first).

## Test tools
- `tools/coop/run_test.ps1`: one game, scripted input, a copy of the profile.
- `tools/coop/run_pair.ps1`: two games on this PC in one session through a local relay.
