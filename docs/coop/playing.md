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
run, jump, attack); it is put back at its owner's position when it drifts more than 3 m. Not yet: shared enemies and
items (each game keeps its own world), outfits (everyone looks like the local save's Link), removing a Link when its
player leaves, menus for remote players.

## One PC, two gamepads
Plug in two gamepads; the second is player 2. Press Numpad 3 once in the world to spawn player 2's Link.

## Test tools
- `tools/coop/run_test.ps1`: one game, scripted input, a copy of the profile.
- `tools/coop/run_pair.ps1`: two games on this PC in one session through a local relay.
