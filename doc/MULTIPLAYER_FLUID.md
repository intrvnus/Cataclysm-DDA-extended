# Fluid movement and real-time combat (prototype)

Handover for Opus. Written by Sonnet 5.5 for Noah. Branch `fluid-proto`, based on
`claude/multiplayer-m1` @ `0b01907`. Nothing was pushed. Delivered as patches.

## TL;DR

* The game stays on the tile grid. Only **when** things happen changes.
* A step starts when the actor is ready (not when a key event arrives). The logical position
  changes at once, so collisions, reservations and every combat rule keep working. The sprite
  then slides from the old tile to the new one while the actor is busy. Busy time = native move
  cost converted to milliseconds, so terrain, encumbrance, speed, move modes and diagonals keep
  their meaning.
* Diagonals work: they are 8 direction steps with the native diagonal cost.
* World time is a real-time clock. A turn is `FLUID_TURN_MS` (default 600 ms). The world never
  waits for a key.
* Melee = wind-up, strike, recovery. The blow is resolved by the native `melee_attack`. Native
  attack time is kept (no balance change).
* Ranged = mouse aim. Hold RMB to steady (native `aim_per_move` runs on real time, so skill and
  sights decide the speed), LMB fires with whatever steadiness there is.
* The host avatar and remote players use **one** code path (`fluid::drive`). Clients send their
  input state on an unreliable channel and predict their own steps.
* Two keyboard layouts, switchable with an option: classic and experimental WASD.

### Verification status

| Item | Status |
|---|---|
| Compiles, tiles build (clang, gcc, project warning flags) | yes, syntax-only (`-fsyntax-only`) |
| Compiles, curses build | yes, syntax-only |
| Linked | **no** |
| Run | **no, nothing has been run** |
| Multiplayer with two processes | **no** |
| Tuning values | educated guesses |

Treat everything below as "designed and compile-checked". Expect bugs in the first run.

## Controls (fluid mode)

| Input | Effect |
|---|---|
| WASD / arrows / numpad / digits (held) | Walk. Two keys = diagonal. Speed from move cost. |
| Walking into an enemy | Starts a swing at it |
| LMB, no gun, combat mode on | Melee swing towards the cursor (with target assist) |
| LMB held, no gun, combat mode on | Keeps swinging while something is in range |
| LMB, gun wielded | Fire at the tile under the cursor |
| LMB held, gun with burst/auto mode | Keeps firing |
| RMB held, gun wielded | Steady the aim (recoil falls over real time) |
| RMB held, no gun | Combat mode; it does not swing by itself |
| SPACE (`FLUID_CANCEL`) | Cancels a wind-up / drops the aim |
| `FLUID_PRIMARY`, `FLUID_SECONDARY` | Unbound keyboard alternatives for LMB / RMB (context `FLUID`) |

Click received while busy (walking, recovering) is remembered for 300 ms (`buffer_ms`) and used
the moment the actor is ready. This is the "fast paced combat" answer: no lost clicks, chained
actions start at the exact end of the previous one, and cancelling is cheap (half the remaining
wind-up).

## Architecture

### Time model

```
world turn (FLUID_TURN_MS)   |-------- turn N --------|-------- turn N+1 ------|
avatar ledger                 moves += speed (native process_turn), native calls refunded
actor timeline               step(120ms) | step | swing(windup, strike, recovery) | step ...
```

* `native_scope` runs a native action (move, melee_attack, fire_gun), measures the moves it
  charged and **refunds** them, then `ready_at` is set from `moves * turn_ms / speed`. So the
  per-actor timeline is in milliseconds, and the native ledger keeps working for everything else
  (monster turn order, effects, regen).
* The world turn is a clock window: `begin_turn()` (start of `do_turn`) opens it,
  `finish_turn_window()` runs input frames until `turn_due()`. Monsters move once per window and
  slide for the window length (`snapshot_before/after_monsters`).
* Render loops are frame-capped (`frame_ms`, 16 ms).  The old idle loop redrew whenever an
  animation was active, which in a town full of moving monsters could reach hundreds of
  software-rendered frames per second.  A dedicated server skips map rendering entirely and
  only pumps its window events; the client also sleeps to the frame cap when held input makes
  `handle_input()` return immediately.
* Per-step network work is bounded: the server sends a full bubble on join/map shift, the 3x3
  around a newly entered submap once, and the current submap about once a second, instead of
  serializing nine submaps every turn.  Monsters carry a host-assigned `net_id`; the client
  updates the existing creature by ID instead of clearing and reallocating all monsters on
  every state message.  A moved client body refreshes only the seen/draw caches; the full
  map/light cache is rebuilt on terrain or weather changes.  A remote player gets an extra
  reality bubble only while outside the main one, so nearby players share the main bubble's
  per-turn cache/field work.
* Characters expose `control()` / `is_player_controlled()`: local avatar, remote player, or
  AI.  Player-sensitive code asks that instead of treating every non-avatar as an NPC.  This
  is the start of the controller separation, without introducing multiple real avatars.
* Remote crafting uses the normal `craft_activity_actor`, not the NPC `ACT_MULTIPLE_CRAFT`
  path.  A client body keeps the in-progress craft in its inventory (or drops it nearby if it
  does not fit), so it is not forcibly wielded and the craft can progress and be dropped.
* The client no longer imports the host's auto-move route.  A stale route made every manual
  step stop travel and print the message; stopping now clears the route as well, and ordinary
  movement with no active travel is silent.
* Client consumption uses the same native `avatar_action::eat_or_use` flow as the host:
  bandages and first aid open their body-part menu locally and replay the `ACT_FIRSTAID`
  activity, while pills, cigarettes and other medications replay `ACT_CONSUME`.  The old
  direct `Character::consume` command rejected medical tools ("You can't consume ...").
  Menu bookkeeping activities (`ACT_EAT_MENU` and the consume submenu ids) are not replayed.
  `firstaid_activity_actor` stores its healing item and chosen body part itself, because the
  client capture runs inside `assign_activity()` before the base activity targets/str_values
  can be filled; `finish()` also rejects missing values instead of dereferencing empty vectors.
  When a character restored from an older save has an `ACT_FIRSTAID` with no valid body part,
  the client clears that stale activity before use; `heal_actor::use_healing_item()` also
  validates the stored body part before reading it, so the consume menu cannot crash on an
  empty vector.
* Client monster state updates move existing bodies in bulk without per-monster `on_move()`
  and rebuild the creature-position index once, so two monsters swapping tiles do not report
  a collision or corrupt the tracker.  The state entries are parsed once into plain values
  (reusing a `JsonObject` copy re-read its cursor and produced malformed-field errors), and
  duplicate monster tiles are dropped before they reach the tracker.
* Long activities and sleep clear the window (`clock.valid = false`): they run as fast as the
  machine allows, as before.
* After the player was away (menu), the clock resets instead of running a burst of turns.

**Deviation from the earlier plan (midpoint commit):** the step is committed at the *start*, not
at the midpoint of the slide. Reason: committing early keeps every collision/reservation rule
trivially correct (nobody can enter a tile that is "being left"), and combat resolves on the
logical position. The cost is visual only: a hit on a sliding target resolves at the new tile
while the sprite is still on the way. The slide is short, so this was judged acceptable.

### Melee

State machine in `actor_state`: `idle -> windup -> recovery -> idle`.

* The body keeps moving during the wind-up and the recovery.  A swing is walked, not run: the
  sprint key resumes its effect after the recovery.  The blow still lands from the body's current
  position, so stepping out of reach during the wind-up makes it whiff.
* total = `max(attack_min_ms, attack_speed(weapon) -> ms)`
* wind-up = `clamp(total * 0.40, 80, 700)`
* At the end of the wind-up `do_strike`: target = creature under cursor direction if adjacent,
  else who stands where the swing was aimed, else the best creature in a cone (assist,
  `assist_cos = 0.5`, ±60°). So an enemy that sidesteps during the wind-up is still hit if it is
  near the direction. Missing (nothing there or `melee_attack` returned false) shortens the
  recovery to 60 % (`whiff_recovery`).
* Cancel: `FLUID_CANCEL` (SPACE).  RMB holds combat mode, so it no longer cancels.  Cancelling
  costs 50 % of the wind-up that was left.
* If the character cannot act any more (stunned, dead, activity), the swing is cancelled.
* Lunge sprite: back 0.10 tile during the wind-up, forward 0.38 at the strike.
* The tile about to be hit is highlighted (`melee_view`).

### Ranged

* `update_aim` runs every frame, also while busy.
* Firing does not root the body: the shot cooldown only limits the next shot.  Walking while
  firing keeps the 35% aim bloom, running keeps 70%; the aim only settles while standing.
* RMB held and a gun wielded: the cursor tile is the target. If the character is idle,
  elapsed time is converted to native moves (`ms_to_moves`) and for each move
  `recoil -= aim_per_move(gun, recoil)` down to `calculate_aim_cap(target)`. This is exactly the
  native aim loop, so skill, sights, gun and range matter the same way. `ALWAYS_AIMED` guns have
  recoil 0.
* Not holding RMB: recoil relaxes towards `MAX_RECOIL` in `aim_relax_ms` (1400 ms).
* Sweeping the cursor across the screen throws steadiness away
  (`aim_sweep_loss * radians`). Taking a step while aiming loses 35 % (`aim_step_loss`),
  running loses everything (`aim_run_loss`).
* LMB: `fire_at_cursor` calls `Character::fire_gun( here, target, qty, *mode )` inside a
  `native_scope`. Duration = max(`fire_min_ms`, charged moves in ms).
* Overlay: crosshair and a spread circle (`total_gun_dispersion` converted to tiles at the
  cursor range). `steadiness` comes from `calc_steadiness`.
* Not handled: `RELOAD_AND_SHOOT` weapons (a message is shown), reach attacks, throwing, spells,
  turrets. The classic keys for all of these still work.

### Visuals

* `slides` and `lunges` are maps keyed by absolute tile. `cata_tiles` asks
  `fluid::sprite_offset( pos )` per sprite and shifts it by a pixel offset
  (`fluid_shift` in `draw_sprite_at` / `draw_critter_at`, reset by RAII).
* Smooth camera: `cata_tiles::draw` shifts `op` by `camera_offset()` and pads the range by one
  tile. Only when the view is centred on the avatar, not isometric, and `FLUID_SMOOTH_CAMERA`.
* Mouse cursor to world: `read_cursor` in `fluid_device.cpp`, fractional tile, accounting for
  the camera offset.

## Multiplayer

### Protocol v2 (`net_protocol.h`, `protocol_version = 2`)

Channel 0 is reliable (as in m1). Channel 1 is unreliable and sequenced (`send_json_fast`).

| Msg | Dir | Channel | Content |
|---|---|---|---|
| `in` | client -> host | 1 | Whole input state: `mx,my` (-1..1), `p,s` held, `pc,sc` press counters, `c` cancel, `cv,cx,cy,cz` cursor in absolute tiles |
| `fx` | host -> clients | 0 | Batch of events: `k` type (step, swing, strike, cancel, shot, aim), `a` actor id, `f,o` from/to, `d` ms, `w` wind-up ms, `x,y` direction, `h` hit |
| `aim` | host -> owner | 1 | `s` steadiness, `r` spread, `f` can fire, `a` aiming (so the client crosshair has the true spread) |
| `welcome` | host -> client | 0 | gains `"fluid": bool` |

Rules:

* The client sends `in` when something changed, plus a heartbeat. Press counters mean a click
  shorter than a packet interval is never lost.
* The legacy `move` message is ignored in fluid mode.
* The host decides the mode. A client follows the host (`client_set_mode`).
* Protocol 1 and 2 are not compatible: the handshake refuses a mismatch.

### One code path for everybody

* The host avatar: `host_frame()` polls the device and calls `drive( avatar, avatar_state(), ... )`.
* Remote players are puppet NPCs on the host. `net::host_drive_remote_players` calls the same
  `drive()` with the last received `in`. `host_drive_npc` (fluid branch) caps moves to the speed
  and returns true so the NPC AI does not act on its own.
* Events are the only thing sent per action. Clients animate them (`apply_visual_event`).

### Client smoothness

* The client **predicts** its own steps (`client_frame`): when it presses a direction and the
  tile looks free it starts the slide locally. The host's `step` event for the own actor confirms
  it and keeps the slide continuous (`client_event( ..., mine )`). On disagreement the slide
  restarts from the visual position, so no snap.
* Remote actors animate from events with `arrival` time, so a late packet shortens the slide
  instead of delaying it.
* The own crosshair is drawn at the local cursor (zero latency); only spread and steadiness
  come from the host `aim` message.

### Free movement over the network (protocol 3)

* With free movement on (option `FLUID_FREE_MOVEMENT`) the host sends `{"t":"bp"}` on channel 1
  about 20 times a second: every free-moving body (`fluid::host_body_samples`) as
  `[actor, x*1000, y*1000, z]` (0.5 = tile centre) and `q`, the newest input frame number of that
  client (`"q"` in its `in` messages).
* Own body: the client runs the same movement maths on its map copy (`predict_free_move`), keeps
  the predicted position per input frame, and on each `bp` compares it with the host position for
  frame `q`; the difference is applied at once and the sprite eases over 100 ms
  (`client_reconcile`). Over 2 tiles it jumps (stairs, falls). Corrections over 0.3 tiles are
  logged ("free movement correction").
* Other players are drawn `FLUID_INTERP_MS` (default 100 ms) in the past, between two `bp`
  positions; their shadow NPC is moved to the tile under the interpolated body.
* Soft walls: fire, known traps and ledges stop a body at the edge; pushing on for
  `FLUID_SOFT_WALL_MS` (default 400 ms) goes in without a query. A client body at a ledge gets a
  hint to examine it (climb down).

### The avatar issue (host avatar)

The host's avatar is special because the process that simulates the world is also the process that
shows menus. Handled:

* Every input wait loop calls `host_frame()`: `get_player_input` (both loops), the
  `handle_mouseview` MOUSE_MOVE loop, and the idle waits in `finish_turn_window`. The avatar
  therefore keeps being driven while the mouse moves, and the world keeps its clock.
* Held keys are read by polling SDL (`SDL_GetKeyboardState`, mouse state), by scancode, derived
  from the active bindings. Classic key *events* for movement/mouse actions are swallowed in
  `handle_mouseview` (`fluid::swallows`), so they cannot double-move.
* A key that confirmed a menu would otherwise count as movement afterwards. `classic_action_begin/end`
  parks fluid input completely while a classic action, menu or direction prompt is open, and
  suppresses every key held when it closes until it is released.  WASD therefore cannot move the
  body behind an inventory screen or a "Close where?" prompt.  The movement resolver is reset at
  both ends of that scope, so the direction key cannot replay as a tap.
* In the inventory/pickup UI, Tab is a local confirm shortcut (`inventory_selector::get_input`);
  it is not a global keybinding and does not change Tab outside those menus.
* Ordinary glass windows (window flag, bash strength <= 12) shatter on the first solid hit at
  average strength; boarded and reinforced windows keep their full bash strength.
* Vehicle driving falls back to classic input (`controlling_vehicle`), also dead/watch states.
* The tiles build only (`fluid_device.cpp` has curses stubs; `enabled()` is false there).

**Not solved (needs your decision):**

1. **Host menus freeze the world for everybody.** An `uilist`/inventory on the host blocks the
   main thread; `net::poll()` is not called, remote players stop. The m1 code has the same
   limitation, but in real time it is much more visible. Options: (a) pump `host_frame()` /
   `net::poll()` from `input_context::handle_input` while a menu is open (cheap, makes the world
   run behind the menu, host can die while in a menu: probably wanted for MP), (b) run the sim
   on a worker thread (big change), (c) accept it and document. I recommend (a) plus an
   "inventory is free" rule only if you want the host to be safe.
2. Click-to-travel (`ACTION_SELECT` on a far tile) is disabled in fluid mode (swallowed).
3. Iso tilesets: cursor and camera are disabled.

### Movement feel (2026-10-20)

* Movement is at full speed immediately.  The first frame after standing still or closing a menu
  uses a normal frame length instead of a large catch-up step, and a one-frame gap between
  movement frames does not look like standing still.
* Fluid walking is 10% faster than the native cost; running is unchanged.
* Fluid/multiplayer stamina has a 1.5x maximum pool and 25% stronger regeneration (the realtime
  bar refills in 16 s instead of 20 s).  Classic turn-based balance is untouched.
* Closing a direction prompt or menu resets the movement resolver, so the direction key that
  answered it cannot become a phantom tap.  A short 120 ms movement lock also covers the first
  UI frame; the client wraps its own prompts in the same classic-action scope as the host.
* Direction prompts accept WASD diagonals by reading the held cardinal keys when the direction
  arrives.
* Walking/running mode switches no longer write "You start walking/running." to the log.
* Melee wind-up is canceled by SPACE (FLUID_CANCEL), which is swallowed from the classic loop so
  it cannot turn into a wait action first.
* Holding RMB without a gun keeps combat mode on; it does not swing by itself.  While the mode
  is held, the left button swings toward the cursor (holding it repeats while something is in
  range), with a gold direction arrow following the cursor rather than only eight tile
  directions.  The actual strike still uses the existing 90-degree reach/arc.  Player melee
  cadence is 20% slower in fluid/multiplayer, balancing the 25% slower monsters.
* World/calendar time passes 25% slower in fluid/multiplayer, while movement and combat timing
  stay unchanged; timed activities are compensated so their real-time speed is unchanged.
* Wounded monsters that have spotted the local player draw a small health bar above them (green,
  yellow, or red); full-health and unaware monsters show no bar.  The bar pass culls monsters
  beyond the nearby screen radius before the expensive visibility checks, avoiding town lag.
* In fluid/multiplayer, monsters are 25% slower (movement and action rate both use `get_speed`).
* Monster grab attacks are disabled.  Grab-only specials do nothing; attacks that also bite or
  claw still apply their normal damage.
* Adjacent target answers (smash, examine, open/close) travel in absolute coordinates, so a client
  body that is one frame ahead of the host still targets the same tile.  Client smash now sends a
  direct `smash_at` tile command instead of replaying `smash_menu`; the host bashes that exact
  tile and the client no longer shows an optimistic broken window that can be reverted.
* A remote player's host body can have a missing arm-limb modifier; if that makes its smash
  ability less than half of its body strength, smashing falls back to full body strength.  The
  fallback is applied again after the final enchantment step, which can zero a remote NPC body's
  melee-damage value.  In fluid/multiplayer, smash strength is then multiplied by 2.5 (with body
  strength as a floor), so ordinary furniture takes a few solid hits rather than becoming
  effectively indestructible.
* Examine actions (e.g. "Pick a fern") use the same absolute target path.  Remote UI actions
  run with the player's own reality bubble swapped in, and harvest prompts use
  `remote_ui::acts_as_player` instead of `is_avatar`, so a remote body is treated as the player
  rather than an NPC.
* Tab confirms the current menu/pickup/drop action universally, and `a` switches to the previous
  inventory submenu/column (contents, wielded, worn, map).  `d` in category-selection mode toggles
  the whole category.
* Vehicle phase 0: the host sends authoritative `in_vehicle` / `controlling_vehicle` / seat and
  vehicle pose state at 20 Hz, and immediately when either flag changes or the vehicle starts,
  stops or reverses.  The client clears stale drive input on ejection/unboarding and no longer
  sends walking frames while seated.  Vehicle interpolation, held throttle and the handbrake are
  still later phases.
* The camera follows the visual body only.  It never follows the mouse: moving the cursor does not
  pan or jitter the view.  `FLUID_CAMERA_LEAD` defaults off.

### Combat feel and telegraphs (2026-10-15)

* Monster melee wind-up warning: while a monster winds up a blow, the tile it will hit is filled red (stronger as the blow nears) and the attacker gets a red frame. Code: `fluid::incoming_blows()`, drawn in `cata_tiles::draw_fluid_overlays()`.
* Hit feedback: when a blow connects, the attacker's lunge holds for 60 ms (hit-stop), the target flashes white for 120 ms and is drawn knocked back 0.22 tile for 180 ms. Code: `fluid::register_hit()`. Clients get it from the "strike" event, which now carries the target tile in "to".
* Melee knockback (world option `FLUID_MELEE_KNOCKBACK`, on): bash weapons of at least 1 kg can push a target up to medium size one free tile back; chance 10% per kg plus 2% per strength above 8, at most 50%.
* Shove: key `FLUID_SHOVE` (`f`) or the middle mouse button. Pushes the creature in front one free tile back, may knock it down (20% + 4% per strength above 8, 5..60%, +20% for small creatures), no damage, 300 stamina, 450 ms. Input frame field `shove_presses`, sent as `"vc"`.
* Shooting: every projectile draws a 160 ms tracer (`fluid::note_projectile()`, event type `"tracer"`). Aim bloom: while moving the aim stays at 35% of the worst (running 70%) and only settles while standing.
* Reloading is not cancelled by walking in free movement (not while running).
* Attacks do not root the body: a melee wind-up/recovery or a shot cooldown still lets the player
  walk (a melee swing walks only, see Melee); shove and throw still commit the body.
* Combat timing bar: a small tapered curved bar above the local player follows the body and shows
  wind-up (gold), recovery (blue), shot cooldown (cyan) or a shove/throw (orange).  The filled part
  and the bright dot at its end show exact progress.
* Ranged monsters: acid/sap spitters, boomers, shockstorm, metal pull and gun monsters without a targeting delay aim for 0.8 s at a player before their attack (`fluid::ranged_windup_ready()`); a red aim line is drawn (event type `"aimline"`). Turrets' existing ~2 s targeting delay draws the same line.
* Explosions: in fluid mode a 350 ms growing, fading flash replaces the blocking explosion animation (`fluid::note_explosion()`, event type `"boom"`).
* New event types are appended to `fx_type` in `fluid.h`: `tracer`, `aimline`, `boom`.

## Keyboard layouts

Switch: option `FLUID_WASD_LAYOUT` (Options > Fluid), or call
`fluid::apply_key_layout( fluid::key_layout::fluid_wasd / classic )`. The option is synced at the
start of `get_player_input` (`sync_key_layout`, cheap). Implementation: `input_manager::reload_keybindings( overlay )`
reloads stock keybindings, `fluid.json`, then the optional overlay `fluid_layout.json`, then the
user's own `keybindings.json`, so the player's customisation still wins.
The layout is independent of `FLUID_MODE`: you can test WASD in classic turn-based play.

Rules used (Noah, 2026-10-14: "match vanilla as closely as possible"): every action keeps its
vanilla key unless that key is needed for movement. WASD moves (Shift held = sprint, so the
shifted W A S D are not used for actions either); arrows and numpad keep moving. Only the eight
actions that lived on `w a s d` / `W A S D` move, onto keys that WASD freed (the old `h j k`
movement keys) or next to WASD. The three movement modes vanilla has no key for get Ctrl keys.

| Action | Vanilla | WASD layout |
|---|---|---|
| Move N / S / W / E | `k j h l`, arrows, numpad | `w s a d`, arrows, `8 2 4 6`, numpad |
| Diagonals | `y u b n`, numpad | `7 9 1 3`, numpad |
| Wield | `w` | `h` (hold) |
| Wear | `W` | `H` |
| Apply / use item | `a` | `q` |
| Use wielded | `A` | `Q` |
| Drop | `d` | `j` |
| Drop to adjacent | `D` | `J` |
| Smash | `s` | `k` (kick) |
| Save and quit | `S` | `Ctrl+S` |
| Toggle run | - | `Ctrl+R` |
| Toggle crouch | - | `Ctrl+C` |
| Toggle prone | - | `Ctrl+Z` |
| Cancel attack / drop aim (new) | - | `SPACE` (context `FLUID`, both layouts) |

Everything else is vanilla: close `c`, loot `O`, zoom `z` / `Z`, fire mode `F`, autoattack `TAB`
(in fluid mode a fluid swing at the weakest adjacent enemy), examine `e`, throw `t` (at the
cursor), fire `f` (= left mouse button) ...

The table shows only the actions the overlay changes (the Classic column was read from
`data/raw/keybindings.json` of the base commit).

Caveats:

* `save()` of the keybinding manager may persist overlay bindings into the user's
  `keybindings.json`. Fix before shipping: write only entries that differ from the *currently
  loaded layout*, or skip saving when the overlay is active. (Not done, to avoid touching the
  input system more than needed.)
* The held-key poll uses the *first* keyboard binding of each movement action that maps to an SDL
  scancode. Shifted letters are ignored (shift state is not tracked).

## Integration checklist (all hooks are small)

| File | Hook |
|---|---|
| `src/fluid.h/.cpp`, `src/fluid_device.cpp` | New. Add to the build (CMake globs `src/*.cpp`; check the Makefile list too) |
| `data/raw/keybindings/fluid*.json` | New |
| `src/do_turn.cpp` | `fluid::begin_turn()` after `net::poll()`; `fluid::finish_turn_window()` after the avatar phase; `snapshot_before/after_monsters()` around `monmove()` |
| `src/handle_action.cpp` | `user_turn::has_timeout_elapsed` -> `fluid::turn_due()`; `moves_elapsed` 0 in fluid; `sync_key_layout()`; `set_timeout( tune().frame_ms )`; `host_frame()` in both loops; `ACTION_TIMEOUT` branch; RAII `fluid_action_scope` around `do_regular_action` |
| `src/game.cpp` | `handle_mouseview`: `fluid::swallows`, `host_frame()` during MOUSE_MOVE |
| `src/cata_tiles.h/.cpp` | `fluid_shift`, `draw_fluid_overlays()`, camera shift, sprite shift |
| `src/input.h/.cpp` | `reload_keybindings`, `fluid.json` loaded in `init()` |
| `src/path_info.*` | two new paths |
| `src/options.cpp` | group `fluid_opts`: `FLUID_MODE`, `FLUID_TURN_MS`, `FLUID_SMOOTH_CAMERA`, `FLUID_WASD_LAYOUT` |
| `src/net_*` | protocol v2, channel 1, `in` / `fx` / `aim`, remote-player driving, client prediction |

If you rebase onto newer m1 work, conflicts are expected in `net_server.cpp`, `net_client.cpp`,
`handle_action.cpp`.

## Options

| Option | Default | Meaning |
|---|---|---|
| `FLUID_MODE` | off | Master switch (host decides in MP) |
| `FLUID_TURN_MS` | 300 | Real-time length of a turn at speed 100. Changes pace, not balance. 100-3000 |
| `FLUID_SMOOTH_CAMERA` | on | Camera follows the sliding avatar |
| `FLUID_ACTIVITY_SPEED` | 4 | Timed actions run this many times faster than the world clock (multiplayer and fluid mode). 1-20 |
| `FLUID_FREE_MOVEMENT` | on | Players move freely inside tiles instead of tile steps (host decides) |
| `FLUID_SOFT_WALL_MS` | 400 | Free movement: push time into fire, known traps or ledges before going in. 0-2000 |
| `FLUID_BODY_WIDTH` | 60 | Free movement: body width in % of a tile. 30-95 |
| `FLUID_INTERP_MS` | 100 | Free movement, client only: other players are drawn this far in the past. 50-400 |
| `FLUID_WASD_LAYOUT` | off | Experimental keyboard layout |
| `FLUID_MELEE_KNOCKBACK` | on | heavy blunt weapons can push targets back |
| `MP_PVP` | off | players may hurt players of other groups |
| `MP_FRIENDLY_FIRE` | on | shots and explosions can hurt the own group |
| `MP_CLIENT_DEBUG` | on | clients may use the debug menu on their own character |

## Tuning knobs (`fluid::tuning`, compile time for now)

`frame_ms 16`, `windup_share 0.40`, `windup_min/max_ms 80/700`, `attack_min_ms 200`,
`whiff_recovery 0.6`, `cancel_cost 0.5`, `buffer_ms 300`, `assist_cos 0.5`, `lunge_back 0.10`,
`lunge_forward 0.38`, `aim_step_loss 0.35`, `aim_run_loss 1.0`, `fire_min_ms 120`,
`aim_relax_ms 1400`, `aim_sweep_loss 0.6`, `max_aim_moves_per_frame 400`, `slide_max_ms 1500`.

Open decisions for Noah:

* **Default `FLUID_TURN_MS`.** 600 ms means a normal step takes 0.6 s (about 1.7 tiles/s). Faster
  feels more roguelike-action, slower is more tactical. Pace only.
* **Aim sweep penalty** and **step penalty** are a design choice (not native rules). Set
  `aim_sweep_loss = 0` and `aim_step_loss = 0` to be purely native.
* **Whiff recovery** (60 %) is also not native. Set to 1.0 for exact native timing.

## Known limitations

* Nothing run. No automated tests.
* Host menus freeze the world (see above).
* Reach, throw, spells, turrets, `RELOAD_AND_SHOOT`: classic only.
* `start_step` skips some checks the classic move path does after the key (auto-move interrupts,
  safe-mode prompts may be shown in the message log only).
* "Simple fault" gun checks are skipped in `fire_at_cursor`; `can_fire_weapon` is used for the
  avatar, a plain ammo check for NPCs.
* Fast speeds: very short step times (below 40 ms) are clamped. Slides longer than 1.5 s are clamped.
* A step onto a different z level (stairs) has no slide.
* Monster slides over more than 3 tiles (teleport, fast zombies) are not animated.

## Test plan for tomorrow

1. Build tiles. Start a new world, enable `FLUID_MODE`. Walk: 8 directions, diagonals, stop on
   release, tile centre at rest. Compare speed on road vs. rubble vs. water.
2. Camera: no jitter, pixel offset when stopped is zero.
3. Melee against a zombie: click, see wind-up, hit message. Walk away during its approach and
   click early (buffer). Cancel with SPACE. Try with a fast weapon (knife) and a slow one.
4. Gun: hold RMB and watch the circle shrink with a trained vs. untrained character. Click to
   fire. Move while aiming. Burst/auto holding LMB.
5. Open the inventory with a zombie near: the world freezes (known).
6. Toggle `FLUID_WASD_LAYOUT`: confirm moved keys work, no action lost, classic restored.
7. Multiplayer: host fluid, one client. Client walks (prediction, no rubber banding), fights,
   aims. Disconnect/reconnect.
8. Debug: set `FLUID_TURN_MS` to 1500 to see the phases slowly.
