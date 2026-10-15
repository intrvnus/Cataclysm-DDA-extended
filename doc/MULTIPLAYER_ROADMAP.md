# Multiplayer roadmap (to-do list for discussion)

## STATUS (read this first; updated 2026-10-15 night, Opus overnight session)
Handoff for the next session: `MULTIPLAYER_HANDOFF.md` (section 0 is the newest); Gemini delegation: `CLAUDE.md`.
Branches: `claude/fluid-proto` (the line; contains everything below since 2026-10-14 evening, `claude/fluid-bubbles` is the same history). Windows build run on `claude/fluid-proto` for Noah's testing: see `MULTIPLAYER_TESTING.md`.

| Topic | State |
|---|---|
| Client menus (record/replay), missions/zones/map/chat/F4 list, driving, passengers, grab (furniture and vehicles), construction, downed/respawn, dedicated server, reconnect, compression | **Done, run in game** (see `MULTIPLAYER_OVERNIGHT_REPORT.md`) |
| Dialogue side commands (look at/size up/assess/opinion/yell) | Done, run in game |
| Monsters/specials/turret friend-or-foe target the nearest player | Done (compiled, regression sweep passes) |
| **C2 per-player reality bubbles** | **Done, merged, regression-tested** (`MULTIPLAYER_BUBBLES_SPIKE.md`): two clients, cars, stairs, reconnect far away, review fixes. Open: 3+ bubbles, perf with many players. |
| **Multiplayer = fluid mode** | Done: every multiplayer session is fluid (the `FLUID_MODE` option is for solo play only). Fluid-only shooting/throwing is by design; classic targeting is not planned. |
| **B2 stamina / B10 weariness** | **Stamina done and run in game** (walking free; sprint ~34 s / ~83 tiles; regen delay 1.5 s, full bar 20 s; load/hunger/thirst slow regen). Weariness: walking no longer tires; "take a break" on the wait key in multiplayer (compile only). |
| **Movement rework M-A..M-D** (WASD feel) | **Implemented and merged.** Run in game (scripted keys): hold, tap = one tile, chord from rest, release grace, opposing keys, direction change, no stutter. Run in game: corner sliding (3 slides, no step through the wall), cursor-sector melee (swing direction = sector of the host's cursor). Still needs Noah's hands-on test: ease-back, camera lead, feel. WASD is the default layout in fluid mode. |
| Compiled-only, not yet run in game: factions view, pets/mounts, consoles, disarm/steal/use-item, sort armor for friends, repeat craft, workout, zones with a created zone, saved map memory | Open: needs test scripts |
| C4 faction rework, C6 basecamps | Decided (2026-10-15). Done, compile + CI only: followers serve the player who gave the order; player groups (`/group ...` in the chat, option `MP_GROUPING`); F4 health only for the own group; missions per group. Per-group factions (each group gets its own faction: item ownership, theft across groups, zones in the zone menu, camps founded by a group) done 2026-10-15, compile only. Overmap knowledge per group done (compile only): the shared group uses the world's overmap, other groups keep their own list of known tiles (`mp_players/group_maps.json`); joining merges your map into the group, leaving keeps a copy; a new group does not inherit the world's (shared) map. A group's faction and map stay with the world when it empties. NPC reputation per group (compile only): in a multiplayer conversation the NPC uses its opinion of the speaker's group (kept on the NPC, a new group starts as a stranger; the shared group uses the normal opinion). Limit: what the NPC does outside conversations (getting angry, fleeing) still follows the shared opinion. Weariness: walking no longer tires, "take a break" on the wait key in multiplayer. |
| Network deltas (send only changed map parts) | Done at submap level: a submap is only sent when its content hash changed, and messages are compressed. Finer (field-level) deltas: not planned unless the internet test needs it. |
| C20 traps | A trap a player sets becomes known to everybody in their group (compile only). Noise already reaches every player's surroundings (A3). |
| C13 sound per player | Sound effects a client's body hears are played on that client, with volume and direction from its own position (compile only); sound texts already reached clients. Music on clients: not done. |
| C22 fire/explosions/fields | Fields sync with the map. Explosions: in fluid mode the blocking explosion animation is replaced by a flash that grows and fades (350 ms) without stopping the game, also on clients (compile only). |
| C18 spells | Clients cast spells (cast / recast key): the spell is chosen on the client and aimed at the mouse like a shot; the host casts it for the body (own tile if the mouse tile is not a valid target), compile only. |
| B5 group reading | Players of the reader's group close by, awake and not busy, listen in and learn at half speed (compile only); reading messages reach the client who learned. Focus minigame for reading: done earlier. |
| C11 auto-travel | Clients: overmap travel and auto-move routes are walked by the client pressing the movement keys for the player, leg by leg over the overmap; any key, a click or a danger warning stops it (compile only). |
| B6 sleep screen | The map dims while the own character sleeps (chat and log stay readable), compile only. Sleep skip when everybody sleeps: done earlier. |
| B7 progress bars | A small green bar above every character doing a timed action (own, other players, NPCs), from the host's state on clients (compile only). |
| C16 server rules | PvP and friendly fire world options (`MP_PVP` off, `MP_FRIENDLY_FIRE` on) done, compile only: with PvP off shots and explosions (blast, shrapnel) spare players of other groups and melee does not target them; with PvP on melee/shove can target them. Debug menu for clients: `MP_CLIENT_DEBUG`. |
| Weariness "take a break", B3 stations polish, B9 turret telegraphs | Take a break: done (wait key). B9: turrets' targeting delay (~2 s, done earlier) now draws a red aim line; other ranged monsters (acid/sap spitters, boomers, shockstorm, metal pull, gun monsters without a targeting delay) aim visibly for 0.8 s before attacking a player ("takes aim" warning), compile only. The shot still goes where the target is when it fires: breaking line of sight dodges it. B3: check before starting. |
| **Noah's test findings (build #1)** | 1 timed actions: **fixed and run in game** (stamina freeze fixed, new option timed action speed x4). 5 mouse view, 6 warning sound: **fixed (compile-only so far)**. 2 slow/janky/late sprint: sprint and step-chaining bugs fixed (compile-only); the slowness itself is a speed decision for Noah. 3 diagonal clipping: draw-order fix (compile-only). 4 grid-free movement: **approved by Noah, in progress** (`MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md`). Speed: `FLUID_TURN_MS` default now 300 ms (was 600; pace only, day length unchanged). Details in the section below. |
| **Grid-free (free) movement** | Phase 1 **done, run in game**. Phase 2: **run in game** (`tools/mp_tests/freetest.sh`, client body): door opens and is walked through, bump attack hits, bump talk opens the NPC menu, diagonal squeeze, boarding a bicycle: PASS; deep water entered (slow: vanilla swim cost); ledge refused (found: client bodies could never enter danger/ledges). Then **soft walls (compile + CI only)**: fire, known traps and ledges stop the body, pushing 0.4 s goes in without a query (host avatar: ledge opens the vanilla climb-down menu; client body: steps into danger, at a ledge gets "Examine it to climb down"). Stairs and furniture dragging not re-run under free movement. **Phase 4 network (compile + CI only)**: host sends body positions 20x/s with the applied input number (`bp`, protocol 3); client predicts its own body and reconciles (100 ms ease), other players interpolated 100 ms behind. Open: phase 5 tuning, Noah's hands-on test. Plan: `MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md`. |
| **Timed actions** | **Done, run in game**: one activity turn per world turn (host + client bodies), option "Realtime: timed action speed" x4, stamina per world turn during actions, no blocking "struggle to continue" menu. Measured: pulp 3.65 s, wait 60 game min 38.5 s. |
| **Actions removed in multiplayer** (Noah) | **Done**: wait, pause (only stops an action), turn duration option hidden, quickload, abandon character, safe-mode toggle. Debug menu: back for clients, acts on their own body (2026-10-15, option `MP_CLIENT_DEBUG`). Autoattack (TAB) = fluid swing (run in game). WASD layout now matches vanilla keys (run/crouch/prone on Ctrl+R/C/Z, those three not run in game). |
| **Multiplayer lives** (after the mod Extra Lives, Noah) | **Done, run in game** (`src/mp_lives.cpp`, world option `MP_LIVES_DIFFICULTY`, default normal): host and clients go down, can be helped up, bleed out, respawn as the same character with a life less, at their bed (last place slept) if any; host respawn too. Out of lives: the client is told and character creation opens (**creating the character and rejoining not tested**; host's new character not tested; item drop on hard/very hard and lives regained over days not tested). |
| **Client actions on the wrong character** (found 2026-10-15) | **Fixed, compile + CI only** (more found later the same day: eating finished on the host, vehicle work went to the host, map reading marked the host, drop actions, construction helpers, granade/geiger/RC; all fixed, compile only) (note: `get_player_character()` is already the client's body during a replay, so only `get_avatar()` / `as_avatar()` uses and code running outside a replay were real bugs): trap examine, vehicle menus' `get_avatar()` calls, disassembly ownership, construction results. Also: rain/glare reach client bodies and every bubble's scent; client "submap already loaded" errors; lives new-character items. Host crashes fixed (mounted client throwing/firing, fitness band, glide off a ledge); clients can pick locks; vehicle remote refused for clients. |
| Testing tools + combat feel (Noah 2026-10-15) | Compile only: debug menu for clients (acts on the own body, option `MP_CLIENT_DEBUG`); monster wind-up warning tile; hit-stop, hit flash and recoil; melee knockback for heavy blunt weapons (option `FLUID_MELEE_KNOCKBACK`); shove (F / middle mouse); tracers; aim bloom while moving; reloading while walking. Weapon identity: on hold (styles and weapon techniques cover it). |
| **Noah's test of 2026-10-15 + decisions** (compile + CI only) | Host = server + client (`--mp-host`; old mode `--mp-host-classic`); question boxes back (no "do it again to confirm"; fixed the options screen), menus back out on danger on clients; WASD in menus; soft-wall warnings, client climb-down menu; diagonal as fast as straight; dragging furniture in free movement; smoothed camera; item effects use the item's user; client body messages reach the client; lives: respawn at bed/home, new character can act and starts at the old one's bed/home. Phase 6 (approved): monsters/NPCs glide by speed, monster melee wind-up (step back to dodge), player melee by reach and 90 degree arc: compile + CI only. Next: weariness break, C4 factions/C6. |
| Phase 7 | Not started: consult Noah first |

## Noah's test findings, build #1 (2026-10-14): OPEN, read before anything else
Noah's own words (lightly formatted, meaning unchanged). Nothing in this block is fixed yet unless
it says so; Noah asked for **no more fixes before the handoff to Opus**, only for the roadmap.

1. **Timed actions need to be implemented.** For example smashing (pulping) a zombie corpse stays
   basically stuck at 0%.
2. **Movement is janky and feels way too slow. Sprinting is also delayed**, which can cause real
   issues in combat situations.
3. **When moving diagonally, the character bugs through terrain and part of it is not visible until
   it has fully moved to the next tile.**
4. **FUNDAMENTAL, IMPORTANT:** "I think we really need to take the extra step and separate movement
   from the grid by making it smooth instead of grid-based. Otherwise the jank will continue and
   make the game more annoying than fun to play." This is the top priority of the roadmap.
   Noah's question: **is move cost (shown in the sidebar beneath Focus) taken into consideration?**
   Answer from the code (read, not tested in game): yes for the current step-based movement. A
   step's duration is the moves the native move charged (terrain move cost, clothing, load, etc.)
   converted to milliseconds by the character's speed (`moves_to_ms` in `src/fluid.cpp`), and the
   client predicts with the host's last step duration. A smooth, grid-free movement has to keep
   using the same move cost and speed (speed in tiles per second = f(move cost, speed)); this is a
   design point for the rework.
5. **The "mouse view" panel covers the top of the sidebar** when the mouse moves over anything but
   the sidebar, hiding vital sidebar information. It has to be removed or changed.
6. **The warning sound needs work.** It plays in situations that are not dangerous (example: the
   death screen). Differentiate dangerous from harmless situations, ideally by checking whether
   danger is close by and only then triggering it.

### Work on the findings (Opus, 2026-10-14 evening)
- **1 Timed actions (fixed, compile-only):** pulping was not frozen but ran ~14x too slowly in
  multiplayer and its bar always read 0%. Cause: the extra world-time moves were given in one lump,
  and most activities spend all moves in one call (pulp hits once per call, rooted ones pause),
  so the bonus was thrown away. Now the extra moves are handed out one turn at a time, for the host
  (`do_turn.cpp`) and for clients' bodies (`net_server.cpp` `run_activity`). Same fix helps every
  activity that does a fixed amount per call (wait, pulp, ...). Pulp got a real progress text
  (corpse damage %, corpses left).
- **1, continued (Noah: "all timed actions", "they need to become faster"):** run in game with
  the new hook `CATA_MP_TEST_ACTIVITY=pulp|wait|meditate` (host avatar and a client's body at the
  same time). Found and fixed: after ~11 s of pulping the blocking "You struggle to continue"
  menu froze the host: in realtime play the world-time activity drained stamina every world turn
  while stamina only came back in real seconds. Now, while a timed action runs, stamina uses the
  classic per-world-turn rules (drain and rest both in world time), and the menu is never shown
  in realtime (its default answer: a break, then the action goes on, with a message).
  New host option **"Realtime: timed action speed"** (`FLUID_ACTIVITY_SPEED`, default x4, 1-20):
  timed actions run that many times faster than the world clock. Measured (60 min day, 300 ms
  turns, x4): pulping a zombie corpse 3.65 s; waiting 60 game minutes 38.5 s; both for the host
  and the client's body. Noah to judge the value by playing.
- **Actions without a purpose in multiplayer (Noah's decisions, 2026-10-14):** removed in
  multiplayer: wait (`|`, the whole wait/rest menu), pause (`.`/`5` now only stops a timed action),
  turn duration option (hidden while realtime), quickload (host too), abandon character, safe mode
  toggle (danger warnings always on), debug menu for clients (the host keeps it). Classic
  fire/throw targeting was already mouse-only for clients. Autoattack (`TAB`) works in fluid mode:
  a fluid swing at the weakest adjacent hostile creature (host and clients). Run in game: client
  `TAB` -> host `trace swing id=6 dx=1 dy=1 hit=1` against the test zombie; client `|` -> "Waiting
  is not possible in multiplayer". (There is no separate "rest-only sleep" in this version: resting
  is part of the wait menu.)
- **WASD layout redone to match vanilla** (Noah): only the eight actions on `w a s d`/`W A S D`
  move (wield `h`, wear `H`, apply `q`, use wielded `Q`, drop `j`, drop adjacent `J`, smash `k`,
  save `Ctrl+S`); close `c`, loot `O`, zoom `z`/`Z`, fire mode `F`, autoattack `TAB` are vanilla
  again; the new movement modes: run `Ctrl+R`, crouch `Ctrl+C`, prone `Ctrl+Z`. Table in
  `MULTIPLAYER_FLUID.md`.
- **(Fixed later the same night: multiplayer lives, see the STATUS table.)** Found: the host's death froze the session. When the host's own character dies, the host
  shows the end screen ("The End", last words) and the world stops for every client. Clients'
  bodies have downed/respawn; the host avatar has not. Needs a decision (host respawn like
  clients, or the dedicated server only).
- **5 Mouse view (fixed, compile-only):** in fluid mode the mouse is the aiming cursor and always
  over the map, and fluid input never reached the code that hides the box. The mouse-view box is
  no longer shown in fluid mode (`game::handle_mouseview`); look (`x`) still works.
- **6 Warning sound (fixed, compile-only):** danger alerts (hostile spotted, laser lock, a player
  down/dead, monster wind-ups) still sound; plain warnings ("do it again to confirm", distractions
  like pain/noise, loud noises) sound only when a hostile is visible within 12 tiles, a laser
  lock is on you or you stand in a dangerous field (`alerts::danger_nearby`). Nothing sounds once
  you are dead. Solo fluid mode: the death screen questions (death cam, keep world) are normal
  popups again; before, "do it again to confirm" made the death cam unreachable.
- **2 Movement (partly):** plain bugs fixed (compile-only): holding Shift while winded now starts
  the sprint as soon as stamina allows (before it needed a new press); while walking continuously
  the next step chains exactly on the end of the last even after a slow world turn (no pause on
  the tile, tuning `chain_ms` 150). **Not changed (needs Noah):** the speed itself. With
  `FLUID_TURN_MS` 600 a speed-100 character walks 1.67 tiles/s (diagonal 1.17, run 3.3). See the
  plan's decision 1. Also known: a sprint pressed mid-step only applies from the next step (the
  grid-free rework removes this).
- **3 Diagonal clipping (fixed, compile-only):** rows are drawn top to bottom; a sprite sliding
  north reaches into the row below and that row's terrain painted over it until the step ended.
  Such sprites are now drawn after that row (`cata_tiles.cpp` `fluid_deferred`). Also fixed: the
  two extra rows built for the moving camera were never drawn (blank strip at the screen edge).
- **4 Grid-free movement:** approved by Noah; decisions recorded at the top of `MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md`. Default turn 300 ms (walk 3.3 tiles/s at speed 100).
- Found on the way, not fixed: the "You struggle to continue. Keep trying?" menu when stamina runs
  low during an activity (`player_activity.cpp`) is a blocking menu; in multiplayer it would
  freeze the host. Pulping a corpse that takes over 10 minutes asks "do it again to confirm".

Other open items found on the way (not from Noah's test):
- **Crash when starting a world with no character** (Noah, build #1, `--mp-host`, SIGSEGV in
  `main_menu::opening_screen`): cause was `world_saves.front()` on an empty list (and a null world
  pointer for an unknown world name) in the `--world` start path. A fix is committed on
  `claude/fluid-proto` (empty world goes to character creation; unknown world name shows a popup;
  `--mp-server` on an empty world shows a message because a dedicated server needs a character).
  It reaches Noah only with a new Windows build, which he has to ask for. Until then: create a
  character in the world first. Open decision for Noah: should a dedicated server on a world
  without a character create one automatically instead of refusing?
- The scripted NPC scenes (disarm, steal, use item, sort armor for friend, console, repeat craft,
  workout), pets, saved map memory, created zones editing: see the handoff, section 6.

### Movement rework spec (Noah + Opus, reviewed by Claude)
- M-A: client input resolver: held-key set (not raw keydowns), press order (last pressed wins for opposing keys), tap = one tile, hold = continuous; the host samples the held direction at the step boundary (next step starts the instant the last ends); from standstill a short chord window (default 40 ms, tunable, 0 = off) lets two-key diagonals form; release grace (default 60 ms) keeps a diagonal latched so a near-simultaneous release leaves no stray cardinal step.
- M-B: host: corner sliding (blocked diagonal slides along the free cardinal component); rejected/lost steps ease back ~100 ms instead of snapping.
- M-C: cursor-sector melee (8 x 45 degree sectors around the player; click hits the adjacent tile in the cursor's sector; bump-to-attack stays).
- M-D: camera lead toward the cursor (small, toggle), smooth linear interpolation with easing only at start/stop, remote players keep per-step interpolation.
- Not doing: mid-step retarget (risky: collides with committed steps/prediction) unless the chord window proves too laggy; "client sends intents at tick T" (state-based input is kept, with press order and timestamps added).


Status markers: **[N]** = Noah's item, **[C]** = added by Claude. Each item has a short
proposal and the decisions Noah needs to make. Nothing here is implemented yet unless noted.
Rule from now on: big batches of changes, checked by the Linux build/CI; Windows builds only
when a batch is worth playing.

## Decisions (Noah, 2026-10-13)

* Everything not commented below: implement as proposed. Order as in section D.
  No Windows builds until the whole batch is done; Linux build/CI checks every big change.
* A1: yes. Seasons keep the same number of days (SEASON_LENGTH unchanged); days are short,
  so everything (hunger, thirst, healing, evolution) moves at a playable pace.
* A2: the goal is a dedicated server.
* A3, A4: do it (A4 is important).
* A5: no windows at all: a short warning sound and a coloured message in the sidebar log.
* B2: regen slower than 5-10 s (stamina takes a while to run down); rest as proposed.
* B3: stations keep working when nobody is around.
* C1: implement. C2: every player gets their own reality bubble. C3: implement.
* C4: reworked faction menu: players can ally with each other and do missions as a faction.
* C5, C6: implement. C7: not a concern (everything is faster).
* Missions/factions (Noah, client menus): missions are per player, but mission progress is shared
  inside a faction: one completion counts for every member (no separate runs, no duplicate
  quest-item spawns).
* Zones/loot sorting/basecamps (C6): shared zones for the whole group, edited from any client.
* Grab/drag: done together with vehicles (C3).
* Overmap knowledge is shared by every player of the same faction.
* C15: faction UI: only allied players see each other's health.

---

## A. Foundations (other items depend on these)

### A1 [N3] World clock: 1 day = 60 real minutes (Project Zomboid style)
* Today 1 game turn = 1 game second, and the realtime/fluid clock runs ~1 turn per 0.6 s.
  A 60-minute day means the calendar must run ~24x faster than movement/combat.
* Proposal: **two clocks**. *Action time* (movement, attacks, effects measured in turns, stun,
  aiming) stays as it is. *World time* (calendar, hunger/thirst/sleepiness, healing, rot,
  plant growth, weather, crafting/reading progress, monster evolution) advances by a
  `WORLD_TIME_SCALE` (default 24) per action turn.
* Clock shows minutes; the HUD can round to 10-minute steps like PZ.
* Decisions: day length (60 min default?), season length (option exists: 14-127 days; with
  1-hour days maybe 7 days per season), whether night length is scaled too.
* Risk: biggest engine change on the list. Every "per turn" vs "per game time" use must be
  sorted. Plan: introduce the scale, then audit systems one by one (needs, effects, items,
  monsters, weather, activities).

### A2 [C] Host menus must not freeze the world
* Today any menu on the host (inventory, crafting, look, popups) stops the simulation and the
  network, so every other player freezes. Already the #1 problem in the fluid prototype too.
* Proposal (short term): keep the world ticking from inside menu input loops (the prototype's
  option (a)). Long term: **dedicated server** (headless host, every human is a client), which
  also fixes "host dies -> everybody freezes".
* Decision: is a dedicated server the goal for the first public version?

### A3 [C] Remove the "one player" assumptions
* Monster/NPC AI and many systems use the single avatar (86 places in monster/NPC AI alone):
  who monsters chase, who NPCs talk to, scent (only the avatar leaves scent), noise, visibility,
  mission givers, factions, map memory, achievements.
* Proposal: a `players()` list and a "nearest/most interesting player" helper, then convert
  the AI, scent and noise first (otherwise zombies ignore clients).

### A4 [C] Real player characters, saving and reconnecting
* Today a client is a random NPC on the host and nothing is saved for clients.
* Proposal: the client's own character (made in character creation) is sent to the host on
  join; the host saves each player character in the world save; reconnect by name/key puts you
  back into your body. Map memory and overmap notes per player.
* **Done:** the client's character is sent on join; the host keeps each player's body in
  `<world>/mp_players/<key>.json` (the key lives in the client's `config/mp_identity.txt`);
  rejoining puts you back into your body where you left (if that spot is loaded, else next to
  the host); the client gets its full character every 3 s. Map memory per player: done earlier. Overmap
  notes: kept by the host per group (2026-10-15, compile only): the shared group writes the world's
  notes, other groups keep their own (`mp_players/group_notes.json`); a client gets them with the
  map and sends back what it added, changed or deleted when it closes the map.

### A5 [C] 353 yes/no popups and "Really do X?" prompts
* Every `query_yn` stops time on the host. Most must become non-blocking (default answer,
  toast message, or a confirm key that does not pause).

### A6 [C] Client UI (inventory, crafting, eat, wear, wield, examine, ...)
* The client can only move/attack/wait/pick up. Every menu has to run on the client and send
  the chosen action to the host. Needed before most of the items below are playable as a
  client. Also needed for mod features (music player etc., already on the list).
* **Done:** wield, wear, take off, eat/drink, read, drop, pick up (everything on the tile),
  crafting, open/close doors, look, character sheet, sleep, movement modes; apply only for
  uses that never ask anything (switching things on/off).
* **Done (record/replay, `src/remote_ui.*`, tested with Xvfb host + client):** apply with
  targets/choices (the lighter asks for a direction: the host uses it and the charge drops),
  examine terrain/furniture, choosing the tile and the items to pick up (`g`, `e`),
  advanced inventory (`/`: the client captures the item-move activities its menu starts and
  the host starts them on the body; only pickup/drop/move/insert/wear/wield/stash are accepted).
  The client does the action on its copy of the character and records every menu answer; the
  host repeats it on the body without opening a window.
* **Done (also tested):** bionics (`p`) and mutations (`[`): the client records which one it
  (de)activated, the host repeats it on the body. Actions that only start an activity run
  their menus on the client and the host starts the captured activity: unload (`U`),
  disassemble (`(`), butcher, mend, insert (`v`), reload (`r`). Views that need nothing
  from the host run locally: messages (`P`), morale, help.
* **Done and tested in game (Xvfb host + client):** smash (`s`), the inventory screen (`i`:
  its item menu sends wield/wear/eat/read/drop/apply/unload/reload/mend/disassemble/insert/
  favorite/change-side to the host; wield was tried), throwing in fluid mode (the throw key
  throws the wielded item at the mouse cursor; the classic fire key counts as the left mouse
  button), talking to NPCs (`C`) and trading: the host runs the conversation turn by turn, the
  client shows the normal dialogue window, the trading window opens on the client and the host
  carries out the agreed trade (a knife for a volleyball was tried). While a player talks they
  cannot be seen, targeted or hurt (portal storms etc.): this part is only code-checked, no
  hazard was tried. Mend only goes through the same capture path as disassemble (no mendable
  item was available). Classic (non-fluid) shooting and throwing are not available to clients:
  they need the mouse-aimed fluid mode. Test helper: with `CATA_MP_TEST_NPC` set the host
  spawns an NPC beside every player who joins.
* **Done and tested (client parity batch):** walking into an NPC (enemies are attacked, anybody
  else opens the normal menu: talk, push, examine wounds/status shown on the client, attack,
  disarm, steal, trade; "use item on" and "sort armor" say not available yet; no chat key),
  stairs and ladders (`<` / `>`: a player can be on another floor than the host: the client got
  "You climb up." onto the roof and back), the action menu (`.`-style) dispatches to the client
  actions, local views and settings (zoom, item list `V`, medical `N`, body status, options,
  auto-pickup/notes, safe mode lists, colours, distraction manager, sky, active mods, compare),
  and host actions: drop in a direction, consume menu, martial-arts style, fire mode, movement
  mode cycling/reset, thief mode. Wait is refused on purpose (the world does not stop).
  Test helpers: `CATA_MP_TEST_NPC` (an NPC beside every joining player), `CATA_MP_TEST_STAIRS`
  (a joining player stands on the nearest stairs).
* **Not done: separate reality bubbles (C2).** The game simulates one reality bubble around
  the host's avatar; a player's body only exists while it is inside it (any floor, within
  roughly 60 tiles). Giving every player their own simulated area needs several maps
  simulated at once, which this engine does not support; it needs its own design (options:
  a tether distance with a warning, or a dedicated server that follows the player group).
* **Done (compiled, not run in game):** the item action menu (`*`-style, host runs the chosen use), haul toggle, diary, option toggles (auto-pickup etc.) and the panel manager.
* **Since done (see STATUS):** vehicles (C3), spells (C18), construction, zones/loot/haul/grab,
  overmap map and notes, missions/factions, "use item on" and "sort armor" for NPCs, dialogue
  side-column commands, consoles, pets, intercoms. **Still open:** talking monsters for clients
  (the conversation opens on the host), translocators (avatar-only data), classic (non-fluid)
  shooting/throwing (not planned). Not yet tried in game: mend, a real hazard during dialogue,
  push/disarm/steal outcomes.

---

## B. Your list, refined

### B1 [N1] Safe mode: warn, never stop
* Replace the "safe mode stops the game" with: warning sound + message + flashing edge arrow
  pointing at the threat (per player, client side). Optional: auto-cancel the current timed
  action (setting).
* Same system serves B9 (distractions) and B10 (turret warnings).

### B2 [N2] Stamina that does not need minutes of standing still
* Ideas (from Dark Souls/PZ/Tarkov style games): short regen delay (~1-2 s) after spending,
  then fast regen (full bar in ~5-10 s walking, faster standing); sprint drains, walking is
  neutral, crouch/prone regen faster; cardio raises the max and the regen speed; heavy load and
  thirst/hunger slow regen. "Exhausted" state at 0 (cannot sprint until 30% back).
* Long-term tiredness moves to weariness/sleepiness (B11), not stamina.

### B3 [N4] Crafting for multiplayer: stations + minigame
* **Passive stations**: workbench/forge/kiln/chem lab etc. take a queue of recipes and work on
  their own (with fuel/power where it makes sense), progress shown on the station. The player
  can walk away.
* **Active boost**: standing at the station and playing a short minigame (timing bar / hold-
  release / sequence) speeds it up (e.g. x2-x4), skill makes the minigame easier and the
  passive speed higher.
* Hand crafting (no station) stays short and active.
* Decisions: which recipes need a station, crafting time scale with A1, can stations work
  while nobody is in the reality bubble.

### B4 [N5] Movement modes in the fluid system
* Shift held = sprint (drains stamina, B2), C = toggle crouch, Z = toggle prone (keys
  configurable). Walk/crouch/prone change speed, noise and visibility like now.
* Fits the fluid prototype's WASD layout (toggle run is Ctrl+R there; TAB is autoattack as in vanilla).

### B5 [N6] Reading and martial arts training
* Reading: progress bar in world time (A1) plus a focus minigame (e.g. keep a marker in a
  moving zone, or pick key words) that speeds learning; interruptions do not lose progress.
  Bonus: group reading (one reads, nearby players learn slower) for co-op.
* Martial arts: practice sequences (input combos at a rhythm), success speeds learning; can
  also train against a dummy/sparring partner (another player!).

### B6 [N7] Sleep with several players
* Minecraft style: when all players (or a configurable %) sleep, time skips to morning; anyone
  awake cancels the skip for everyone. Otherwise a sleeping player sleeps in normal world time
  (with 1-hour days that is only a few real minutes).
* While sleeping: screen dims, you can still see the chat; damage/noise wakes you.

### B7 [N8] Timed actions short and cancellable
* All "quick" actions (pick up, eat, drink, wear, reload, open containers) shortened to
  ~0.3-2 s real time with a progress bar above the character. Cancel with movement keys,
  Esc or the cancel key; taking damage cancels optional per action.
* Long activities (butchering, first aid, disassembly, construction) become stations/minigames
  (B3) or short repeating steps.

### B8 [N9] Remove all "time stop" interruptions
* Every distraction (monster spotted, hostile in view, noise, pain, hunger...) that today asks
  "Stop doing X?" becomes a B1 warning. The action keeps going unless the player cancels.

### B9 [N10] Turrets and auto weapons: telegraphed attacks
* "Hostile detected" voice/beep + warning arrow, then an aim period (e.g. 1.5-2.5 s, shorter
  for military turrets) with a visible laser/aim line; leaving line of sight resets it.
  Damage stays as is (still deadly), only the instant-kill-on-sight goes away.
* [C] Same for other ranged monsters (spitters, soldier zombies, robots): short wind-up so
  fluid combat is fair. Also melee monsters get small wind-ups (grabs, big hits telegraphed)
  to match the fluid melee system.

### B10 [N11] Weariness
* Ideas: weariness only from heavy work (crafting, construction, carrying heavy loads,
  sprinting), not from walking; drops fast while resting at a camp/bed/campfire (not only
  sleep); a short "take a break" action (sit down 30-60 s) clears a chunk. Ties into A1
  so it is measured in world time.

---

## C. More issues found [C]

| # | Issue | Proposal |
|---|---|---|
| C1 | **Death and respawn** | Downed state with bleed-out timer, other players can revive; otherwise respawn (new character in the same world, or spectator). Host death must not end the server (A2). |
| C2 | Several bubbles | Players far apart: today the client is tied to the host's bubble. Later: one bubble per player group (expensive) or a hard tether distance. Decision needed. |
| C3 | Vehicles | Driving in realtime, several players in one vehicle (driver + passengers/gunners), entering/leaving. |
| C4 | NPCs and dialogue | Talking to NPCs opens menus (A6); missions/followers per player or shared faction. |
| C5 | Shared faction and ownership | All players one faction, so items in your friends' base are not "stolen". |
| C6 | Basecamps, zones, loot sorting | Shared or per player. |
| C7 | Monster evolution/spawning | Tied to days; with 1-hour days zombies evolve 24x faster in real time. Needs its own scale. |
| C8 | Item rot, plant growth, fermenting | Follow world time (fine), check balance with short days. |
| C9 | Healing, hunger, thirst, sleepiness rates | Follow world time; with 1-hour days you eat several times per real hour. Check feels right. |
| C10 | "Wait" and "rest" menus | Disabled in MP (or only "wait until everyone sleeps"). |
| C11 | Auto-travel, auto-move, auto-pickup | Work in realtime, cancel on danger warning. |
| C12 | Look/examine/aim modes | Do not freeze the world (A2); look mode on clients. |
| C13 | Sound and music per player | Sounds and music played on each client for its own position. |
| C14 | Weather, light and time on clients | Today the client uses its own world's weather/light. Sync from host. |
| C15 | Chat, player list, ping | Basic chat (Enter/T), list of players and their health. |
| C16 | Server rules/options | Who may use the debug menu, PvP on/off, friendly fire, difficulty options set by the server. |
| C17 | Network efficiency | Only send changed parts of submaps, compress (zstd is already in the game), send monsters as deltas. Needed before playing over the internet. |
| C18 | Throwing, spells, reach weapons, turrets you own | Not in the fluid prototype yet. |
| C19 | Bionics and mutations activation | Menus on the client (A6); activation costs in realtime. |
| C20 | Traps and stealth | Traps you placed are visible to friends; noise from one player aggroes for all. |
| C21 | Achievements, scores, memorial | Per player. |
| C22 | Fire, explosions, fields | Already work (host simulates); just need syncing visuals on clients. |
| C23 | Encumbrance/speed fairness | Fluid speed comes from move cost; check that heavy/slow characters are not unplayable in realtime. |

---

## D. Suggested order

1. Fluid prototype: build, link, run and fix (in progress).
2. A2 (world keeps running behind menus) + A5 (no blocking popups) + B1/B8 (warnings instead
   of stops). Without these nothing else feels right in realtime.
3. A3 (multi-player AI/scent/noise) + C1 (death/respawn).
4. A1 (world clock) + B2/B10 (stamina/weariness) + C7-C9 tuning.
5. B7 (short timed actions) + B4 (movement modes) + B9 (turrets/monster wind-ups).
6. A4 (real characters, saving) + A6 (client UI).
7. B3 (crafting stations/minigames) + B5 (reading/training) + B6 (sleep skip).
8. Dedicated server, network efficiency (C17), chat (C15), the rest of C.
