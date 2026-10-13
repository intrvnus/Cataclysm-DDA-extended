# Overnight run (agreed with Noah)

Rules
- Branch `claude/fluid-proto` only. Commit + push after every step that builds. No force-push, no PR, never touch `intrvnus-mod`, no Windows build.
- Small design calls are mine (cheap to redo): pick the sensible default, write it in the report, move on.
- Never silently change balance/gameplay numbers; anything like that goes into the report as a suggestion.
- Big or risky design questions (faction rework C4, per-player bubbles C2, anything that changes how the game plays for everybody) are NOT decided overnight: they go into the report.
- Loop: code -> build in the separate copy (`bld.sh`, binary lands in `cataclysm-tiles.next`) -> hand the tester a checklist -> read its report -> fix -> repeat. The tester never edits source.

Queue (in this order, roughly)
1. Vehicles: boarding/passengers, shooting from a vehicle, grab/drag (furniture first, then vehicles).
2. Monsters/NPC AI for several players: convert the checks that only look at the host avatar (anger, aggro, "can see the player"), test with the zombie helper in a spot with line of sight.
3. Stabilise: a full checklist run on every build (zones/loot with a zone, construction placement, workout, haul, sort armor order on the host, use item on NPC, disarm/steal, pets, consoles, vehicles, dedicated server).
4. Docs: keep `MULTIPLAYER_PROGRESS.md` and the roadmap accurate; add a short "play with friends" guide.

Expanded queue (10-12 h, about 15-20 build/test cycles of 15-25 minutes each)
5. Vehicles part 2: client view of a moving car, passengers boarding another player's car, shooting from a car, gunner/driver handover.
6. Monster AI part 2: whatever the zombie diagnostics show; animals/ranged monsters/turrets aim at the nearest player; NPC allies follow the right player.
7. Verification sweeps with the tester on every build: death + respawn, helping a downed player up, sleep skip with two players, stations, zones + loot with a created zone, construction placement (needs materials: helper hook), mend (helper hook), pets (helper hook), consoles, reconnect after leaving, host-avatar leaks (a client wielding/wearing must never change the host avatar).
8. Remaining menus: dialogue side-column commands, NPC "sort armor", faction menu as a read-only view of the group's factions from host data.
9. Grab/drag on a SIDE branch `claude/fluid-grab` (refactor of `walk_move` is big and risky; the main branch stays stable). Only merged into fluid-proto if it builds and the tester passes it.
10. Per-player map memory/overmap notes (A4 open item) if time remains.
Not done overnight: stamina/weariness/balance changes (B2/B10), the faction rework (C4), per-player bubbles (C2), Windows builds, anything that needs your design call.

Morning report: `doc/MULTIPLAYER_OVERNIGHT_REPORT.md` (what was done, verified vs not, open design questions, anything odd).
