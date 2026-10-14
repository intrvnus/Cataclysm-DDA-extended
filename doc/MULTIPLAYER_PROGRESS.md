# Multiplayer build: progress overview

Branch: `claude/fluid-proto` (repo intrvnus/Cataclysm-DDA-extended). Plan and decisions: `doc/MULTIPLAYER_ROADMAP.md`. How to play: `doc/MULTIPLAYER.md`. Overnight run notes (with the test evidence): `doc/MULTIPLAYER_OVERNIGHT_REPORT.md`.

## What it is
Host + clients over ENet (messages compressed). The host simulates the world. Each client has its own character (saved on the host, rejoin by key) and plays in a real-time "fluid" mode (mouse aim, throw at cursor). Client menus run on the client; the host repeats the chosen answers on the player's body (`src/remote_ui.*`, record/replay).

## Works (checked in the running game, host + client(s) under a virtual display, logs quoted in the overnight report)
- Join, own character, saved and restored on reconnect (killed client rejoins its body).
- Driving: take the controls, start the engine, drive; a passenger rides along.
- Downed players: go down, are helped up, or bleed out and respawn.
- Dedicated server: runs without a player and keeps the world ticking.
- Chat (F2) and player list (F4), for clients and the host.
- Sort armor (the host's worn order really changes), grab and push furniture and vehicles (a cart was pushed 2 tiles), haul, item action menu, zones window, map (what any player has seen), missions list (group), peek, NPC menu entries, dialogue and trade, stairs, local views.
- Earlier: apply, examine, pickup, advanced inventory, bionics, mutations, unload, disassemble, smash, inventory item menu, fluid throw.
- Vanilla unit tests that touch my changes pass (vehicle, monster, NPC, construction, item actions, missions, factions); in a full-suite run 5 assertions failed in a monster-spawning test helper (they pass when those test files run alone: probably leftover state between tests); one melee test case crashes inside the test's own helper (not my code).

## Compiled but not yet run in the game
Factions read-only view, pets/mounts/friendly monsters, computers/consoles, disarm/steal/use-item (the confirmation prompts reach the host), sort armor for friends, repeat last craft, workout (construction placement: host-side replay checked), map memory loading on the next visit.

## Already in the code from earlier sessions
World clock (`REALTIME_DAY_MINUTES`), dedicated server, scent for every player, warnings instead of stopping (alerts), crafting stations, reading as a timed action + focus minigame, sleep skip when everybody sleeps, timed actions cancel on movement, sprint, turret/monster warnings.

## Not done
- Separate reality bubbles (C2): players far apart are not simulated (the dedicated server follows the group's middle).
- Spells (not needed), dialogue side commands (look at / size up / yell), classic (non-fluid) shooting for clients.
- Faction rework (C4), basecamps (C6), stamina/weariness (B2/B10) and other balance work.
- Sending only changed map parts (more network savings).
- Windows build: none until the whole batch is done.

## Next
Finish verifying the compiled-only items, then a decision about C2 (bubbles) before the next phase.
