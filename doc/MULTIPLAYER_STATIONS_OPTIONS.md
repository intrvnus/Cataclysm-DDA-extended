# B3 crafting stations: options for Noah (2026-10-15)

Roadmap B3: "passive stations take a queue of recipes and work on their own; standing at the
station and playing a minigame speeds it up". Your decision on record: **stations keep working
when nobody is around**. Still open (roadmap B3 "Decisions"): which recipes need a station, and
how crafting time scales. **Decided (Noah 2026-10-15): option C, built in `src/mp_stations.cpp`** (queue: recipes of 30 minutes or more).

## What exists today
- Crafting is an activity of one character; the half-made item ("in progress craft") stays on
  the workbench or the ground when interrupted and can be continued later.
- Multiplayer: crafting runs as the body's activity on the host, x4 with the option "timed action
  speed", x2 more while the focus minigame (SPACE on "Focus!") is hit.
- Nothing crafts without a character standing there.

## Options
**A. Unattended progress on in-progress crafts (small, ~1 day).**
A craft left on a workbench/forge/kiln keeps progressing slowly on its own (e.g. 25% of normal
speed) while its station has what it needs (fire/power for forge, kiln, smoker; nothing for a
workbench). Coming back finishes it at full speed. No queue, no new UI.
- Pro: small, uses the existing "continue craft" flow, matches "keeps working when nobody is around".
- Con: no queue; skill checks/failures happen when the player resumes.

**B. Station queue (medium, ~3-4 days).**
Examining a station opens "Queue a recipe": components are taken from the player at once and
the station makes the items one after another in world time (speed from the queuing player's
skill, 50% of normal), results land on the station tile. Minigame at the station doubles speed.
- Pro: the full B3 idea; good for co-op bases.
- Con: new UI and save data; needs rules for which recipes are allowed (below).

**C. Both: A now, B later.**

## Which recipes (for B)
1. Only recipes that already need a station quality/furniture (forge, kiln, chemistry set,
   workbench-only big items): smallest change, recommended.
2. Every recipe longer than e.g. 30 minutes.
3. Everything.

## Time scale
- With "timed action speed" x4 and 1-hour days, a 2-hour craft takes ~30 real seconds when
  attended. Unattended at 25-50% that is 1-2 real minutes: probably fine as is.

## My recommendation
C with recipes option 1: build A first (small, testable), then B for station recipes only.
