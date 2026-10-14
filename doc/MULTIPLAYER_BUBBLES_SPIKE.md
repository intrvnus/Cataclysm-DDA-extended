# Spike: one world, a reality bubble per player

Branch: `claude/fluid-bubbles` (side branch; `claude/fluid-proto` is unchanged by this work except for the unrelated commits listed at the end).

## Idea
One world (shared `MAPBUFFER`, one creature list with absolute coordinates). Every player gets his own loaded and simulated area (a `map` over the shared data). Each creature is simulated exactly once, by the bubble it is in. Same principle as Minecraft/Zomboid/Valheim ("loading zones around every player, one world").

## What the spike contains (all compiled and run in the game)
- `src/bubbles.{h,cpp}`: opens extra `map`s at any place, finds which bubble owns a position, per-turn upkeep of extra bubbles (floor caches, falling, vehicles, fields, items, map cache).
- `do_turn.cpp`: monsters run once per bubble (the main bubble skips creatures an extra bubble owns); player bodies are simulated inside their own bubble (`swap_map`).
- `game.cpp`: despawn/shift of monsters and NPCs leaves creatures in extra bubbles alone.
- `mapbuffer.cpp`: saving and "clear outside the bubble" keep the submaps of every extra bubble (before, they would have been freed under it).
- `net_server.cpp`: a client whose body is in an extra bubble is sent that bubble (terrain, creatures); test hook `CATA_MP_TEST_BUBBLE=1` (far, about 290 tiles) or `=near` (overlapping, 36 tiles).

## Results (run in the game, logs quoted)
- **Far bubble (263 tiles from the host's avatar):** the client body was moved into a second bubble. A zombie next to the body (`sees 1`, `dest body`) walked up and attacked it (body hp 1032 -> 971 within seconds) while another zombie in the host's own bubble was simulated at the same time. The client's screen shows the other place (field, zombie next to the player). 0 errors.
- **Overlapping bubbles (36 tiles apart, most of the area shared):** no crash, no errors; the body was mauled by the zombie, went down, and respawned through the normal death/respawn path.
- Earlier research (read-only agents) predicted this would work: two `map`s over one `MAPBUFFER` is already how tinymap/smallmap work, `swap_map` makes `get_map()` follow the bubble, positions of creatures are absolute.

## Known gaps (what a full build still needs)
1. **Scent** is one grid in the host bubble's coordinates: monsters in another bubble cannot follow scent (they still see and hear). Needs one scent grid per bubble.
2. **Vision/anchor of the host avatar:** `build_map_cache`, vehicle caching (`reality_bubble()`), weather, `load_npcs` and overmap simulation (hordes, mongroups, travelling NPCs) are anchored on the host's avatar only. Hordes and NPC activation have to run around every player.
3. **A bubble follows its player:** the spike places a bubble once. A real bubble must shift with the body (what `game::update_map` does for the main bubble), spawn/despawn monsters at its edges, and load NPCs.
4. **Saving and rejoin:** a body saved inside an extra bubble must reopen that bubble on rejoin (now only bodies near the host come back in place).
5. **Overlap ownership:** a vehicle in the overlap of two bubbles has its stored position rewritten by whichever map loaded it last (`veh->sm_pos`); the overlapping case needs one owner per vehicle and per submap activity (fields, items).
6. **Host avatar as an ordinary player:** the dedicated-server "ghost avatar that follows the group" disappears; the host avatar gets its own bubble like everyone else.
7. **Network:** per-client areas are now computed from the client's own bubble (done for terrain and creature lists); the delta scheme (only changed submaps) is a separate, independent step.
8. Static state: `map::build_map_cache` keeps a static "previous player position" shared by all maps (cosmetic cache), `ter_view_p` and other bubble-coordinate members.

## Estimate
The risky unknown ("can two maps over one world run side by side?") is answered: yes. The remaining work is a list of mostly mechanical per-bubble versions of existing single-bubble code (scent, spawn/shift, NPC loading, overmap anchors, save/rejoin). My estimate is several focused work sessions, with the overlap/ownership rules (gap 5) as the part most likely to need care.

## Unrelated commits made on the way (also on `claude/fluid-proto`)
- Dialogue side commands (look at, size up, assess personality, check opinion, yell) now work for clients (run in game: "Size up stats" returned the NPC's stats).
- Monsters, specials and the turret friend-or-foe check use the nearest player instead of only the host avatar (compile-checked; regression sweep re-run).
