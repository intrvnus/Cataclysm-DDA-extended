# Multiplayer fork: handoff (2026-10-14)

Written by Claude (Sonnet 5.5) for Noah and for Opus, who takes the project over. Read this first,
then `MULTIPLAYER_ROADMAP.md` (STATUS table at the top is the live to-do list) and
`MULTIPLAYER_TESTING.md` (how Noah plays a build).

## 1. Goal
A real-time ("fluid") multiplayer mode for Cataclysm: DDA. Every player has a character and a
reality bubble of their own, clients can do everything the host can (menus, crafting, building,
driving, trading, ...), movement with WASD is smooth, stamina is reworked for real time, and
joining/leaving/dying hands over cleanly. Multiplayer **is** fluid mode (`fluid::enabled()` returns
`net::active() || option FLUID_MODE`). Fluid-only shooting and throwing (Noah does not want classic
targeting in multiplayer).

## 2. Repo, branches, rules
- Repo `intrvnus/Cataclysm-DDA-extended`, clone `/home/user/cataclysm-dda-extended`.
- Work branch: **`claude/fluid-proto`** (`claude/fluid-bubbles` has the same history). Side
  branches only with Noah's OK. **Never push to `intrvnus-mod`.** **No PR** unless asked.
- **No Windows builds unless Noah asks.** (He asked for two so far, see section 7.)
- **No gameplay/balance changes without asking**: report them as suggestions. Big design questions
  (faction rework C4, anything changing how the game plays) go to Noah.
- **Stop and consult Noah before Phase 7** of the roadmap.
- Language: English. Always say what was **verified and how**: compile-only / run in game / CI.
  Never say "done" without that.
- Quality over convenience: do not trade comfort or experience for an easier solution (Noah's
  example: no tether distance between players; the answer was per-player bubbles).
- Keep `doc/MULTIPLAYER_ROADMAP.md` current (STATUS table) and say which entries changed.
- Give progress updates to Noah at least every 20-30 minutes, never go silent for long.
- Noah's cdda skill (`cdda-engine-workflow`) has these rules; the copy in this container may be
  overwritten by a sync, so Noah should add them to his account skill: quality over convenience,
  sub-agents by default, keep roadmap current, regular updates.

## 3. How we work
- **Sub-agents by default** for anything parallelizable: research (read-only agents), code reviews
  (give the reviewer the code, not your conclusion), independent code packages in
  `isolation: "worktree"` (each agent builds/tests in its own tree and returns a branch/commit that
  is merged back; merge conflicts in `net_server.cpp` and `fluid.cpp` have happened and are
  resolved by keeping both sides), test-script writing. Check a sub-agent's first report against
  the logs before believing it.
- **Tester method**: log-based tests with two real game windows under Xvfb (host + client), keys
  by xdotool, verdict from `config/debug.log` lines, quoted exactly. Scripts are in
  `tools/mp_tests/` (README there). Env hooks `CATA_MP_TEST_*` create situations (NPC, STAIRS,
  ZOMBIE, VEHICLE, BEAT, DOWN, ARMOR, FURN, BUILD, BUBBLE=1|near|quiet, STAMINA, TRACE, CORNER,
  MONSTER, ZONE, PET, FACTIONS, MAPMEM, SCENE=disarm|steal|useitem|armor|console|recraft|workout).
  Hooks are in `src/mp_test_*.cpp` and `net_server.cpp`; they only act when the env var is set.
- **Local build**: build in a copy (`bld.sh`: copy `src/` to `/home/user/cdda-build`, `make -j
  TILES=1 RELEASE=1 PCH=1 CCACHE=1 LOCALIZE=0 TESTS=0`, promote the binary with `mv -f`), so the
  main tree can be edited meanwhile. Unit tests (`cata_test`) the same way in `cdda-test`.
- Debug log lines of the feature start with `Multiplayer:` (`config/debug.log`).
- Commit small, descriptive messages; end with the attribution lines the session asks for.

### Pitfalls
- Never `pkill -f <pattern>` matching your own command line (kills your shell, exit 144).
- Start long jobs with `(setsid nohup ... < /dev/null > log 2>&1 &)`.
- The machine restarts when the session idles: scratch dir, Xvfb, build copies may vanish.
- Escape twice makes a client leave the game; scene scripts use `space`.
- Windows MSVC: `near`/`far` are macros, do not use them as variable names. ENet is vendored
  (`src/third-party/enet`, already in the vcxproj).
- A zombie "can't move to its location" error in a strip only a bubble covers is a benign vanilla log.
- Redraws while a bubble `swap_map` is active draw the wrong map: `ui_manager.cpp` forces the main map.

## 4. Architecture (where things live)
- **Record/replay menus** (`src/remote_ui.*`): the client runs a menu on its mirrored character;
  the host replays the recorded answers on the player's body (an `npc` with value
  `mp_remote_player`, faction your_followers) under `remote_ui::replay_scope`.
- **Fluid mode** (`src/fluid.cpp/.h`, `fluid_device.cpp`): state-based input frames (move_x/move_y,
  primary, secondary, sprint, cursor); host-authoritative `start_step`; client prediction
  (`prediction`, `client_frame`, `client_event`); `tuning` struct holds constants.
  Client `move_resolver`: press order, last-pressed-wins, 40 ms chord window from standstill,
  60 ms release grace, tap latch (>= 100 ms) until `note_own_step_started()`. Host: corner
  sliding (`corner_slide_dir`), ease-back on rejected prediction, camera lead (option
  `FLUID_CAMERA_LEAD`), cursor-sector melee (`sector_dir`, 12 degrees neighbour tolerance, 0.3 tile
  dead zone). Walking into an NPC opens the NPC menu through `net::bump_npc` (throttled 1.5 s).
  WASD layout is the default in fluid mode (`FLUID_WASD_LAYOUT`; overlay
  `data/raw/keybindings/fluid_layout.json`: q apply, h wield, j drop, k smash, O close, c crouch, z prone).
- **Reality bubbles** (`src/bubbles.*`, `src/current_map.*`): an extra `map` per remote body over
  the shared `MAPBUFFER`; `swap_map` makes `get_map()` return the bubble; ownership by nearest
  centre (`owns_submap/owns/owner`, exact `holds`); `bubbles::view` RAII for client actions;
  `open_for/release/follow_all/tick/sync_caches/sync_vehicle_caches/unload_outside/close_all`;
  `game::update_extra_bubble`, `game::unload_outside_bubbles`. Scent per bubble, sounds stored with
  absolute origin and processed per bubble. `do_turn.cpp` runs monsters/NPCs per bubble (each
  creature simulated once). `map.cpp`/`map_field.cpp` skip submaps another bubble owns.
  `CATA_MP_NO_BUBBLES=1` turns bubbles off for comparison. Design/history:
  `MULTIPLAYER_BUBBLES_SPIKE.md`.
- **Stamina** (multiplayer/fluid only, via `alerts::nonblocking()`): `update_stamina_realtime`
  (regen delay 1.5 s, full bar 20 s, crouch x1.33, prone x1.55, load factor, hunger/thirst
  x0.75/x0.5); walking/crouch/crawl free; sprint about 105 stamina per tile, about 34 s / 83 tiles
  from full 8500; remote bodies are treated like players (`get_stamina`, `mod_stamina`, ...).
- **Server** (`src/net_server.cpp`, `net_session.cpp`, `net_*.cpp`): dedicated server (`--mp-server`)
  has no ghost avatar when bubbles are enabled; per-client own bubble is sent; reconnect reopens
  the bubble at the saved position; compression.

## 5. Done and how it was verified
Legend: **RUN** = run in the game by scripted two-window tests with log lines; **CI** = Linux/Windows
compile; **UNIT** = `cata_test`.

| Area | Status |
|---|---|
| Client menus (inventory, craft, construct, examine, pick up, missions, zones, map, chat F2, list F4), driving, passengers, grab (furniture and vehicles), downed/respawn/bleed-out, dedicated server, reconnect, compression | RUN (see `MULTIPLAYER_OVERNIGHT_REPORT.md`) |
| Dialogue side commands (look at, size up, assess, opinion, yell) | RUN |
| Per-player reality bubbles: far (263 tiles), overlap (36 tiles), car 490 tiles, two clients, stairs level change, reconnect far away, downed/respawn | RUN, 0 errors |
| Ownership (one owner per creature/submap/vehicle), scent/sound per bubble, hordes/NPC loading per player, review fixes (crash from coarse `inbounds`, sound pointers, leaks, shutdown order, stale vehicle caches) | CI + RUN (overlap with zombies + two clients) |
| Multiplayer = fluid; WASD default | RUN |
| Stamina numbers | RUN (`stam_trace.sh`) |
| Movement harness: hold, tap, chord, release grace, opposing keys, direction change, no stutter | RUN (`movetest.sh`, `movecheck.py`) |
| Corner sliding (3 slides, no step through wall); cursor-sector melee mapping | RUN |
| Zones round trip, factions window text arrives | RUN |
| Walking into NPC opens NPC menu (bump), recraft (`"re":true`), workout fixes, pet state in state message | CI + menu opens in RUN (screenshots); full scenes below still failing in scripts |
| Unit tests [vehicle] [monster] [npc] [construction] [iuse_actor] [mission] [faction] | UNIT pass; [melee] crashes in its own test helper (pre-existing) |
| Windows MSVC build | CI, two runs (section 7) |

## 6. Still open (ordered roughly by value)
0. **Noah's findings from his first hands-on test (build #1) come first**; the full text is in
   `MULTIPLAYER_ROADMAP.md`, section "Noah's test findings". In short: (a) timed actions such as
   pulping a corpse stay at 0%; (b) movement feels janky and too slow, sprint starts late;
   (c) diagonal steps clip through terrain / the sprite is partly hidden until the step ends;
   (d) **fundamental, top priority: make movement smooth and independent of the grid** instead of
   tile steps (move cost and speed must still count: the current step time comes from the native
   move's charged moves via `moves_to_ms`); (e) the "mouse view" panel covers the top of the
   sidebar; (f) the warning sound fires in harmless situations (death screen), it should only fire
   when danger is close. Noah asked for no more fixes before this handoff, so none of these are
   started. The grid-free movement is a design question: write a short plan for Noah first.
1. **Noah's hands-on test** of build #2: movement feel, ease-back, camera lead, general play.
2. **Not proven in game** (code is merged, compile-only or only partly run). Next job: fix the
   scene scripts and the real bugs they expose:
   - NPC prompts: disarm / steal / use item on NPC. The NPC menu opens and the confirm prompt
     ("You may be attacked! Proceed? (Do it again to confirm.)") appears; the scripts press the
     wrong confirm keys, so `scene.sh` reports FAIL with `replayed=0` (last batch). Fix the key
     sequence, then check `replayed`/`failed` counters and host errors.
   - Sort armor for a friendly NPC, computer consoles (`game::use_computer` for a body),
     repeat last craft, workout activity: same scene batch (`scenes_all.sh`), all FAIL so far.
   - Pets/mounts: `pet_test.sh`, hook line "test pet spawned" never appears; client pet menu unproven.
   - Map memory: client saves on leave (not seen in log) and load back at join is unproven.
   - Zones: disable/edit of a created zone not visible in the zones window test.
   - Melee neighbour tolerance cases (test pairing misaligned by extra swings).
   - Ease-back and camera lead feel (needs a human).
3. **Bubbles**: 3+ bubbles, bubbles on different z-levels, vehicles crossing between bubbles, saving
   with several far bubbles, performance with many players (timing log "bubble timing per turn").
   Known: rain scent decay only on the main bubble; `build_map_cache` static previous-position
   shared by maps (cosmetic); cache sync is a blunt per-turn invalidation for overlapping bubbles.
4. **C4 faction rework / C6 basecamps**: options paper `MULTIPLAYER_FACTIONS_OPTIONS.md` has 12
   decisions; **waiting for Noah**. Do not start before he answers.
5. **Weariness "take a break"** action: needs Noah's approval (gameplay change).
6. **Network deltas** (send only changed map parts): optional, bandwidth only.
7. **Starting a world with no character** crashed (build #1, `--mp-host`); fixed in `main_menu.cpp`
   (empty world goes to character creation; `--mp-server` shows a message, open question for Noah
   whether it should create a character itself). In a new Windows build only.
8. Docs: keep `MULTIPLAYER.md` / roadmap in step; say classic MP targeting is not used.
9. **Phase 7**: not started; consult Noah first.

## 7. Windows builds
- Workflow `.github/workflows/noah-windows-build.yml` ("Windows Tiles build (manual)"), MSVC,
  22-36 min, artifact `cdda-windows-tiles-custom`.
  Trigger: `gh workflow run noah-windows-build.yml --ref claude/fluid-proto`. **Only when Noah asks.**
- Logs are not fetchable through the proxy; read failures with
  `gh api repos/intrvnus/Cataclysm-DDA-extended/check-runs/<job id>/annotations`.
- Build #1: run 38604745945, commit 4555c7c (lacks the NPC menu on bump).
  Build #2: run 38620420420, includes the NPC-menu fix, recraft/workout/pet fixes and the test hooks.
  Link: https://github.com/intrvnus/Cataclysm-DDA-extended/actions/runs/38620420420
- Noah plays with `cataclysm-tiles.exe --world "<world>" --mp-host` and client
  `--mp-connect <host>:7777` (see `MULTIPLAYER_TESTING.md`), and sends back `config/debug.log` of both sides.

## 8. File map
| Topic | Files |
|---|---|
| Fluid input/movement | `src/fluid.cpp/.h`, `src/fluid_device.cpp`, `src/cata_tiles.cpp` (fluid_pad), `src/options.cpp` |
| Bubbles | `src/bubbles.*`, `src/current_map.*`, `src/game.cpp` (`update_extra_bubble`), `src/do_turn.cpp`, `src/map.cpp`, `src/map_field.cpp`, `src/sounds.cpp`, `src/mapbuffer.cpp`, `src/overmapbuffer.*`, `src/ui_manager.cpp` |
| Network | `src/net_server.cpp`, `src/net_session.cpp`, `src/net_*.cpp`, `src/third-party/enet` |
| Menus | `src/remote_ui.*` |
| Stamina/NPC bodies | `src/character.cpp`, `src/npc.cpp`, `src/grab.cpp` |
| Test hooks | `src/mp_test_*.cpp/.h` |
| Test scripts | `tools/mp_tests/` |
| Docs | `doc/MULTIPLAYER*.md` |

## 9. Noah's decisions on record
Multiplayer = fluid; WASD default; fluid shooting/throwing only; walking is free, sprint about 34 s;
per-player bubbles (C2); missions per player but progress shared inside a faction; zones shared and
editable from any client; overmap knowledge shared per faction; only allied players see each other's
health (C15); stations keep working when nobody is around (B3); world clock A1 and the other
decisions are in the roadmap's "Decisions" section.
