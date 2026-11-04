# Playing co-op (test builds)

## Online (any number of players up to 8)
1. One player runs the relay on a PC everyone can reach (UDP port 41900 open or forwarded):
   `python server/coop_relay.py` (Python 3, no packages).
2. Everyone adds to `totk.ini` next to `totk.exe`, then starts the game and loads a save:
   ```
   [coop]
   coop_relay = <relay PC address>
   coop_name = <your name>
   ```
3. Players appear in each other's games as they connect, and stay behind idle when they leave. Join or leave any time.

What works (2026-11-04): each player's Link appears in the others' games and moves with that player's input (walk,
run, jump, attack); it is put back at its owner's position when it drifts. It carries its owner's weapon, bow and
shield and wears their head, body and leg armor, updated when they change them. Not yet: shared enemies and items
(each game keeps its own world), fused weapon parts and armor dyes, removing a Link when its player leaves, menus for
remote players.

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
