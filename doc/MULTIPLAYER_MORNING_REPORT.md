# Morning report: overnight multiplayer work

Branch: `claude/fluid-proto` (everything committed and pushed). No Windows build, no PR, `intrvnus-mod` untouched, no gameplay or balance numbers changed.
The detailed log with quoted evidence is `doc/MULTIPLAYER_OVERNIGHT_REPORT.md`.

How to read "checked": **run in game** = started a host and client(s) under a virtual display and read the host/client debug logs. **compile-only** = builds, never exercised. **unit tests** = the vanilla test suite for the touched areas.

## Done and run in game
| Feature | Evidence |
|---|---|
| Driving a car as a client | engine starts, velocity up to 2400, about 48 tiles driven, client screen follows, 6 drive actions replayed, 0 failed |
| Passenger in a car | a second player seated in the car travels with it (checked with two clients) |
| Downed, helped up, bleed-out | body goes down, helper gets it up, or a new body spawns after the bleed-out timer |
| Dedicated server | keeps ticking with a client connected (33-35 heartbeat lines, 0 errors) |
| Chat (F2) and player list (F4) | for clients and for the host |
| Reconnect | killing a client and restarting it puts it back in its saved body |
| Sort armor | the worn order really changes on the host |
| Grab (`G`): furniture **and vehicles** | table pushed 2 tiles, shopping cart pushed 2 tiles, player follows. Vehicle grab reuses the avatar's own code (`game::grabbed_veh_move` now takes any character) |
| Construction for a player's body | components used up, build activity runs and finishes (host-side replay; a real client choosing in the menu was not driven) |
| Zombies chase a client | with line of sight a zombie went from 7 to 2 tiles toward the client's body |
| Map memory | saved on leave (files rewritten) |
| Network compression | about half the traffic (about 30 KB/s against 70 KB/s while driving) |
| Regression sweeps | car, downed cycle, dedicated server, two clients, 18-key tour, reconnect: 0 errors on the newest build |

Vanilla unit tests (unit tests): vehicle, monster, NPC, construction, item actions, missions, factions all pass. One melee test crashes inside its own helper (same as before, not my code).

## Compile-only (not run in game yet)
Factions read-only view, pets/mounts/friendly monsters, computers/consoles, disarm/steal/use-item prompts reaching the host, sort armor for friends, repeat last craft, workout, loading of saved map memory on the next visit, zones with a created zone.

## Not checked inside the vehicle grab
Pulling a vehicle (a wall was behind the player in the test), turning or zig-zag drags, heavy cars. The host's own grab was not re-tested; its path only gained an extra parameter.

## Problems found and fixed on the way
- Car never moved for clients: cruise-control and engine-start looked only at the host avatar.
- Host/clients froze on a blocking debug popup: in multiplayer they now log instead.
- "invalid npc NC_NONE" log spam from client bodies: lookup made safe.
- Drag bugs for furniture (wrong tile check, self-block, classic move path ignored drag).
- A 33 MB test binary was committed by mistake; as you allowed, removed from the history of this branch only (final tree identical, same 25 messages, backup branch `backup-before-scrub` kept locally).
- Test-environment only: missing UTF-8 locale crashed a client, focus issues with two windows, stale processes on port 7777. Note: the machine restarts when the session sits idle, which killed one sweep; harmless for the code.

## Decisions for you (not made overnight)
1. **Per-player reality bubbles (C2)**: still one simulated area; players must stay within about 60 tiles of each other.
2. **Faction rework (C4)** and basecamps (C6).
3. Balance work (stamina/weariness B2/B10).
4. Dialogue side commands (look at / size up / yell) and classic (non-fluid) shooting for clients are not available yet.
5. Windows build: none until you say the batch is done.
6. Stop and talk before Phase 7.

## What I can work on now
1. **Verify the compile-only list in game** (factions, pets, consoles, disarm/steal, workout, zones with a real zone, map memory loading). Cheap, no design questions. About 1-2 hours with the test helpers.
2. **Dialogue side commands** (look at / size up / yell): small, self-contained.
3. **Grab polish**: pulling vehicles, zig-zag drags, test with a heavy car.
4. **Monster AI part 2**: ranged monsters, animals, turrets aiming at the nearest player; NPC allies following the right player.
5. **Network deltas**: send only changed map parts (more bandwidth savings).
6. **C2 design talk**: if you want per-player bubbles, I would write a short options paper (cost, risk, what changes for the host) for you to decide on. This is the biggest open item before a Windows build.

Tell me which of these (or something else) to start with; without a preference I would do 1 first, then 2 and 4.
