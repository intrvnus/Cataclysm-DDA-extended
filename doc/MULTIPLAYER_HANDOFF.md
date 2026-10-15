# Multiplayer fork: handoff (2026-10-14, updated at night)

Written by Claude (Sonnet 5.5) for Noah and for Opus, who takes the project over. Read this first,
then `MULTIPLAYER_ROADMAP.md` (STATUS table at the top is the live to-do list) and
`MULTIPLAYER_TESTING.md` (how Noah plays a build).

## 0. Newest: overnight session 2026-10-14/15 (Opus, Noah asleep)
Noah's instructions for the night: delegate to Gemini where possible and review every diff; write
code for the roadmap and **only compile-check it** (no in-game tests until he asks); keep a morning
report (Claude Docs: "CDDA Multiplayer – Overnight Report", with a tick-off test checklist) and
have a fresh Windows Tiles build ready.

### Gemini delegation
Works through `/root/gemini` (LiteLLM proxy on :4000). Needed `DISABLE_PROMPT_CACHING=1` in the
Gemini instance's environment (free tier has no context-cache quota; added to
`delegate-gemini.sh` in the container, **Noah should add it to the setup script**). The free daily
quota per model is tiny (one Claude Code task = many requests): 3.8/3.7/3.6-flash ran out after two
tasks; 3.5-flash-lite still worked. Delegated and merged after review: a typo, the README row for
`freetest.sh`, two `MULTIPLAYER_FLUID.md` sections (network/soft walls, options table).

### Done (and how it was verified)
| Change | Verified |
|---|---|
| Free movement phase 2 test hook `CATA_MP_TEST_FREE` + `tools/mp_tests/freetest.sh`; `trace npc menu`, `trace free bump/soft` lines | Run in game: door, attack, talk, squeeze, vehicle PASS; swim enters water (slow, vanilla swim cost); ledge refused |
| Soft walls (approved plan): fire / known traps / ledges stop the body, 0.4 s push goes in without a query (`fluid::soft_wall_confirmed`, `game::walk_move`); client bodies step into danger (npc::move_to never would); ledge: host gets the climb-down menu, client a hint to examine | Compile + CI |
| Free movement phase 4 (network): `bp` message (channel 1, 20 Hz, protocol 3) with the applied input number `q`; client predicts its own body (`predict_free_move`) and reconciles (`client_reconcile`, 100 ms ease, jump over 2 tiles); other players interpolated `FLUID_INTERP_MS` behind and their shadows placed (`net::client_place_shadow`) | Compile + CI; one smoke run (host+client up, door case PASS) |
| Options `FLUID_SOFT_WALL_MS` (400), `FLUID_BODY_WIDTH` (60 %), `FLUID_INTERP_MS` (100); log line for corrections over 0.3 tiles | Compile + CI |
| Bugs: client actions acted on the HOST's character: pet/monster menus (`monexamine.cpp`), keg filling / mill / planting / trap examine (`iexamine.cpp`), haul menu, vehicle examine menus (`vehicle_use.cpp`): all use `remote_ui::acting_player()` now | Compile + CI |
| Client "submap already loaded" errors (saved copy vs host copy) silenced on clients; network batches replaced one by one | Compile + CI |
| Rain: scent washes out in every bubble; client bodies get wet; sun/snow glare reaches client bodies | Compile + CI |
| Lives: new character's starting items move into worn containers (`migrate_items_to_storage`) | Compile + CI |
| `fluid_layout.json` reformatted (repo JSON style; the local build's lint stopped on it) | formatter + JSON equality |
| Host crashes: a mounted client throwing or firing an energy weapon read the host's `mounted_creature` (null when the host was not riding) | Compile + CI |
| Construction (log to planks, digging refund), crafting component choice and disassembly ownership use the acting player; vehicle remote control refused for clients (it took over the host's view) | Compile + CI |
| Client notice when the host's world pauses (no message for 2 s: "The world waits: the host is busy") | Compile + CI |
| Prediction: sprint only when the character can run; no prediction while downed or asleep | Compile + CI |

### Open / for Noah
- Hands-on test of the night's build (checklist in the morning report).
- Item tick functions that use "the player" (pyromania morale for molotovs/incendiaries, fish
  trap skill, multicooker hint, music morale) use the host's avatar for client items: gameplay
  question, not changed.
- A2 (host menus stop the world for clients) still open; needs a design decision.
- Weariness "take a break", C4/C6: waiting for Noah. Phase 7: ask first.

## 0b. Previous: session of 2026-10-14 evening/night (Opus)
The next session's lead is again Claude Opus; it can delegate small, well-scoped tasks to a
Gemini-backed Claude Code (see `CLAUDE.md`, "Delegating to Gemini").
Read this section first; sections 1-9 below are the older handoff (still valid unless this section
says otherwise). Commits `f99adbe..6c81e05` on `claude/fluid-proto`, all pushed, Linux CI green up
to `1bb8f18` (the last commit's run was queued at handoff).

### First steps of the next session (Gemini delegation, set up but not yet tried with a real key)
1. `echo ${GEMINI_API_KEY:+set}` must print `set` (Noah adds the key in the cloud environment
   settings; never ask for it in chat). `ls /root/gemini` must list the scripts (the environment's
   setup script writes them; if missing, Noah has not pasted the setup script yet).
2. `/root/gemini/models.sh` lists the models the key may use; Noah is on the free plan, so pick the
   newest **Flash** model (Pro runs out of free requests within a task or two), tell Noah which,
   and use it via `GEMINI_MODEL=<id> /root/gemini/start-proxy.sh`.
3. Verify for Noah: (a) the proxy answers on localhost:4000; (b) a trivial delegated task:
   `/root/gemini/delegate-gemini.sh "In src/character.cpp, in the comment inside
   Character::place_corpse( map *here ), fix the doubled word 'their their' to 'their'. Change
   nothing else."`, then show the commit and `git diff claude/fluid-proto..gemini-work`;
   (c) your own session still uses the normal Anthropic API (`get_session` model fields; the
   Gemini instance runs with `env -i` and its own `CLAUDE_CONFIG_DIR=/root/.claude-gemini`).
   Already tested on 2026-10-14 with an invalid placeholder key: worktree created, Gemini-backed
   Claude Code started, request went through the proxy to Google and came back `API_KEY_INVALID`.

### Noah's rules (current)
- **No sub-agents** (they eat Noah's usage). Do research, reviews and tests yourself.
- Quality over convenience; no gameplay/balance change without asking (a short plan first for
  anything big); say how each change was verified (compile-only / run in game / CI); keep the
  roadmap STATUS table current; progress updates every 20-30 minutes; consult Noah before Phase 7;
  never push to `intrvnus-mod`, no PR, no force push, no Windows build unless Noah asks.
- Reply in English unless Noah writes German.

### Done in this session (and how it was verified)
| Change | Verified |
|---|---|
| Timed actions: one activity turn per world turn (host `do_turn.cpp`, client bodies `net_server.cpp` `run_activity`); option "Realtime: timed action speed" (`FLUID_ACTIVITY_SPEED`, x4); stamina counts per world turn during an action (`Character::update_stamina`); the blocking "You struggle to continue" menu is skipped in realtime (`player_activity.cpp`); pulp progress text | Run in game: `CATA_MP_TEST_ACTIVITY=pulp|wait` (host avatar + client body): pulp 3.65 s, wait 60 game min 38.5 s, no freeze |
| Mouse-view box hidden in fluid mode (`game::handle_mouseview`) | Compile + CI only |
| Warning sound only near danger (`alerts::danger_nearby`, `alerts.cpp`), silent after death; solo fluid death screen uses normal popups | Compile + CI only |
| Sprint is level based; continuous walking chains steps (`tuning::chain_ms`); north-sliding sprites drawn after the next row (diagonal clipping, `cata_tiles.cpp` `fluid_deferred`); fluid camera pad rows drawn | Compile + CI; the draw order was seen working indirectly in screenshots only |
| `FLUID_TURN_MS` default 600 -> 300 (pace only; existing option files keep 600: set it by hand) | Run in game (300 ms per tile at speed 100) |
| **Free movement phase 1** (`fluid.cpp`: `body_*`, `free_move`, `body_cross`; option `FLUID_FREE_MOVEMENT`, default on) | Run in game: solo walk/terrain cost/wall stop/wall slide; client body moves freely on the host (trace lines `Multiplayer: trace free cross`) |
| Actions removed in multiplayer: wait, pause (only stops an action), turn duration option (hidden, `COPT_REALTIME_HIDE`), quickload, abandon character, safe-mode toggle; debug menu host-only. Autoattack (TAB) = fluid swing at the weakest adjacent hostile | Run in game: client TAB hit the test zombie; client `\|` message. Others compile only |
| WASD layout matches vanilla keys (`data/raw/keybindings/fluid_layout.json`, table in `MULTIPLAYER_FLUID.md`) | JSON loads; Ctrl+R/C/Z not pressed in game |
| **Multiplayer lives** (`src/mp_lives.cpp/.h`, `data/json/multiplayer_lives.json`, world option `MP_LIVES_DIFFICULTY` default normal; after the mod Extra Lives by ProHere, credited): the host goes down like clients (`net::host_prevent_avatar_death`, hooked in `game::is_game_over`); bleeding out spends a life and the same character comes back healed with 30 s incorporeal, 90 s invisible/speed, 5 waves of force (durations in real seconds); respawn point = where the player last slept (host: `place_player_overmap`; client body: stored at the bed and respawned in its own bubble); hard/very hard drop the items where they died (map note); out of lives: client gets `out_of_lives`, disconnects, makes a new character (`mp_lives::make_new_character`) and reconnects (`net::request_reconnect`); host makes a new character in place | Run in game with `tools/mp_tests/lives_test.sh 1|bed|host|nolives`: client respawn lives 2->1 PASS, bed 30 tiles away PASS, host down/helped/bled out/respawn 2->1 (log checked), out of lives PASS up to "making a new character". **Not tested:** finishing the new character + rejoin, the host's new character, item drop, lives regained over days |
| Test tooling: `bld.sh` copies changed files with fresh time stamps (the old `cp -a` caused a heap-corruption binary); `pair.sh` finds windows with `xdotool search --pid` (no `xwininfo`); new `autoattack_test.sh`, `lives_test.sh`, hook `CATA_MP_TEST_ACTIVITY` (`src/mp_test_activity.cpp`), `CATA_MP_TEST_DOWN=1|bed|host|nolives` | used for the runs above |

### Open, in order (Noah asked for these)
1. **Free movement phase 2**, test each in game: doors (push into a closed door opens it), bump
   attack, talking to NPCs by walking into them, diagonal squeeze, stairs, vehicles (board/drive),
   ledges, swimming, dragging furniture. The code goes through `free_move` -> `body_bump_worthy` ->
   the old `start_step`. Write a scripted test per case (`CATA_MP_TEST_TRACE=1` gives
   `trace free cross` / `trace free refused` / `trace swing` lines).
2. **Free movement phase 4 (network):** clients still predict tile steps; send body positions and
   do prediction + reconciliation (plan: `MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md`).
3. Lives: test the untested parts above.
4. Older open items: scene scripts (disarm, steal, use item, sort armor, consoles, recraft,
   workout), pets, map memory, zones editing (section 6 below); C4/C6 decisions wait for Noah.
5. Known limit: any host menu (also the host's new-character screen) still stops the world for the
   clients (A2); the dedicated server avoids it.

### Environment notes (cloud container)
- Install once: `apt-get install -y libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev
  libsdl2-mixer-dev libfreetype-dev zlib1g-dev ccache xdotool imagemagick gdb p7zip-full`.
- Build copy: `cp -a` the repo to `/home/user/cdda-build` once, `make -j4 TILES=1 RELEASE=1 PCH=1
  CCACHE=1 LOCALIZE=0 TESTS=0` (about 75 min on 4 cores; a change to a widely included header such
  as `options.h` costs about 20 min), then `tools/mp_tests/bld.sh` and `mv -f cataclysm-tiles.next
  cataclysm-tiles` in the repo root.
- Test worlds: `/tmp/mp_test/ud_client/save/Leeds` with a character (made by hand under Xvfb:
  start the game with `--userdir ... --world Leeds`, the empty world opens character creation; Tab
  to DESCRIPTION, `n` random name, Tab, `Y`; then Ctrl+S, `Y`). `start_fresh` copies it to
  ud_host. Set `FLUID_TURN_MS` to 300 in both `config/options.json`.
- Editing a JSON file in `data/` while a game cache exists logs "Stale game data detected": test
  artefact only.
- A test zombie horde can kill the host's character in long runs; with lives that now respawns.

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
- **No sub-agents** (Noah, 2026-10-14 evening: they eat his usage; this replaces the older rule below). Older rule, kept for history: sub-agents by default for anything parallelizable: research (read-only agents), code reviews
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

### Sub-agents in detail (how Noah wants them used)
- Default: if work can run in parallel, use agents instead of doing it serially. Noah said "dont spare"
  when he had free usage; do not hold back on agents for research, reviews and test writing.
- Read-only agents (Explore/general-purpose) for research and code review. Brief them like a peer:
  goal, what is ruled out, files to read. For a review give the code, not your conclusion.
- Code packages: one agent per independent package in `isolation: "worktree"`, each builds and tests
  in its own tree; you merge the result back into `claude/fluid-proto`, resolve conflicts, rebuild,
  re-run the affected tests. Their first report is not proof: check it against the logs.
- Test-script agents write scripts into `tools/mp_tests/` style, quoting exact log lines as evidence.
- Tell Noah what the agents are doing in the regular progress updates.

### Pushing fixes and delivery
- Everything goes on `claude/fluid-proto`: small commits, `git push -u origin claude/fluid-proto`
  (retry up to 4 times with 2/4/8/16 s backoff only on network errors). No PR, no push to
  `intrvnus-mod`, no force pushes.
- Before a commit: it compiles (`bld.sh` build copy); for behaviour changes run the matching
  script from `tools/mp_tests/` and quote the log lines. State in the commit/report whether it was
  compile-only, run in game, or CI.
- Fixes for plain bugs (crashes, compile errors, typos, doc mistakes) can be made without asking and
  mentioned afterwards in one line. Anything that changes gameplay or balance, or is a big new
  feature, is a short plan or suggestion for Noah first (roadmap entry, wait for his yes).
- A fix reaches Noah only through a Windows build, so a fix list for him is collected in the
  roadmap and a build is made only when he asks for one. After a build, tell him the run link, which
  commit it contains, and what to test.
- When Noah says "no more fixes, only the roadmap", write findings into `MULTIPLAYER_ROADMAP.md`
  and do not touch code.
- Update the STATUS table in the roadmap with every relevant change and say which entries changed.

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
  `data/raw/keybindings/fluid_layout.json`, vanilla keys wherever possible: q apply, h wield, j drop, k smash, Ctrl+S save, Ctrl+R run, Ctrl+C crouch, Ctrl+Z prone; see the table in `MULTIPLAYER_FLUID.md`).
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
