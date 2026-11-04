# Playing co-op (test builds)

## Online (any number of players up to 8)
1. Everyone loads a save, opens the menu (F1) and goes to the **Co-op** tab, and types a name.
2. One player chooses **Host a session**. The tab then shows the address the others use on the same network. Over
   the internet the others use the host's internet address, and the host's router must forward UDP port 41900 to
   the host's PC.
3. The others type that address under **Host's address** and choose **Join**. Players appear in each other's games
   as they connect and disappear when they leave (**Leave the session**, or closing the game). When the host leaves
   the session ends for everyone.
4. The name and the last address are kept in `totk.ini` (`coop_name`, `coop_join`).

Everyone needs the same build (the packet version must match) and the same game update. A standalone relay still
works: `python server/coop_relay.py` on a PC everyone can reach, and `coop_relay = <address>` in `totk.ini` joins
it at start (the test scripts do this).

What works (2026-11-04): each player's Link appears in the others' games and moves with that player's input (walk,
run, jump, glide, attack); it is put back at its owner's position when it drifts, and goes away when its player
leaves. It carries its owner's weapon, bow and shield and wears their head, body and leg armor. Guests follow the
host's time of day and weather, and the host's placed enemies near the players (position, life, death; a guest's
hits count on the host's enemy). The host's HUD prompts stay on the host's screen. Not yet: items, enemy attacks
decided by the host, fused weapon parts and armor dyes, menus for remote players. Chests stay each player's own.
## One PC, two games, two gamepads (online co-op without a second PC)
Double-click `Play co-op (two pads).vbs` in the repo folder (or run `tools\coop\play_two.ps1`): it starts the relay and two game windows with no console windows; window 1
reads the first connected gamepad, window 2 the second (the log lists the gamepads it sees: "input: gamepad 0 of 2").
Load the save in both. Both play copies of the save (build\prof-play-1/-2, fresh each start): nothing is saved back.
Wake both gamepads before starting. The launcher frees the mouse (mouse_sens = 0: no mouse camera, no cursor capture) and hides the consoles (coop_hide_console = 1); logs stay in logs	otk.log. Setting: `pad_index = <n>` in totk.ini picks the n-th gamepad (0 = first).

## One PC, one game, two gamepads
Plug in two gamepads; the second is player 2. Press Numpad 3 once in the world to spawn player 2's Link.

## Test tools
- `tools/coop/run_test.ps1`: one game, scripted input, a copy of the profile.
- `tools/coop/run_pair.ps1`: two games on this PC in one session through a local relay.
