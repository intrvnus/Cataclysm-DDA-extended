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

Morning report: `doc/MULTIPLAYER_OVERNIGHT_REPORT.md` (what was done, verified vs not, open design questions, anything odd).
