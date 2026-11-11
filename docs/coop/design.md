# Co-op design (decisions and plans)

## Shape
- Shared world, online, two PCs (play test, 2026-11-03). One host runs the world (enemies, physics, quests, time); the
  guest plays a second Player actor in it. Local two-gamepad play is the test bench on the way there.
- Each PC renders its own view, so the online camera simply follows that PC's own Link. Split-screen on one PC is not
  planned (it would render the world twice); local testing switches which Link the camera follows.

## Sessions: drop in, drop out, more than two (play test, 2026-11-04)
- Seamless, plug-and-play joining and leaving at any time; more than 2 players.
- Transport: a small relay server (stdlib Python, server/coop_relay.py) that every game connects to; it forwards each
  player's state to the others. Anyone can join or leave a session at any time, there is no fixed pairing, and it
  works behind home routers (only the relay needs a reachable port). LAN and one-PC tests run it locally.
- Game side: up to 8 players (1 local + 7 remote). Each remote player is a spawned Player actor with its own
  controller and calc wrapper; it appears when the player's first state arrives and goes away when they leave or
  time out.
- Version 1 (no world sync): every PC runs its own world and save; remote Links are driven by their owner's input
  (camera-relative stick turned into this PC's camera frame) and pulled back to the owner's reported position when
  they drift. World sync (enemies, items, host-authoritative state) and the save rules below come after.

## Saves: drop-in co-op, like Elden Ring (play test, 2026-11-03) — plan, not built
- Everyone plays their own save alone. At any time a player can join another player's game.
- The host's save is the game state: world, quests, map, time, shrines. The guest's world state is not touched.
- The guest brings their own character from their save: inventory (weapons, bows, shields, materials, food), armor,
  hearts and stamina, abilities they have unlocked, Zonai devices and battery.
- What the guest gains in the session (items picked up, weapons, armor, materials, rupees) goes back into the guest's
  own save when they leave; their world progress does not (it happened in the host's world).
- Open questions (decide before building): durability and lost items (does a weapon that breaks in co-op stay broken
  back home — likely yes); quest items and key items picked up in the host's world (likely not carried home); hearts
  or stamina gained from the host's shrines (likely no); death of the guest (respawn at the host, no item loss?);
  what the guest may do to the host's world (open chests: who gets the item).
- Building blocks this needs (none researched yet): where the game keeps the player's inventory and stats in memory
  (GameData flags, the "PorchItem" style arrays of BotW are a lead), reading them from the guest's save file without
  loading it as the world, giving Link#2 that inventory instead of Link#1's, and writing the guest's changed inventory
  back into the guest's save on leave.

## Later (nice to have, the tester, 2026-11-04)
- A notification when a teammate fast-travels, naming where they went.
- Live map markers for teammates.
