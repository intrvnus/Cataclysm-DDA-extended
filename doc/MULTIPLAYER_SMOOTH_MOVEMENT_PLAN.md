# Grid-free smooth movement: plan for Noah's approval (2026-10-14)

Status: **proposal, not started.** Nothing below is implemented until Noah says yes.
Answers Noah's build #1 finding 4 ("separate movement from the grid").

## Recommendation: players move freely, the game still sees them on one tile
- Each player gets an exact position anywhere inside a tile (a round body, about 0.6 tile wide).
- Every game system (traps, fields, items, monsters, vision, sound, vehicles) keeps seeing the
  player on one tile: the tile under the body's centre. Crossing into a new tile runs the normal
  "stepped onto this tile" code once (traps, item messages, doors, boarding, ledges, water).
- Monsters and NPCs stay on tile steps with the existing smooth slides. Their AI is tile-based
  through and through; changing it is a much bigger, riskier job.
- The alternative (keep tile steps but let them overlap / change direction mid-step) only hides
  the jank: you still cannot stop or turn inside a tile. Not recommended.

## Move cost still counts (your question)
- The native step cost (terrain/furniture move cost, run/crouch/prone, encumbrance, load,
  swimming, dragging) is computed exactly as today, for the tile you are moving through.
- Speed in tiles per second = 1000 x speed / (step cost x turn length in ms). It is re-checked
  about 30 times a second, so rubble, mud or a heavy pack slow you at once, not at the next tile.
- Stamina: sprint drains the same native amount per tile as now; sprint starts the instant
  Shift is held.

## Collision and interaction
- Walls, closed doors, windows, solid furniture block. Moving diagonally into a wall slides you
  along it; clipping a corner nudges you round it. Two creatures never share a tile.
- Dangerous tiles (fire, known traps, ledges) act as "soft walls": you stop at the edge and keep
  pushing ~0.4 s to go in (replaces the "Really step into fire?" question).
- Pushing into a door opens it, into an enemy swings at it, into a friendly NPC opens the menu.
- Attacks start from your tile (reach, hits and dodges unchanged); aim direction uses your exact
  position, so it follows the mouse more precisely.
- Your position snaps to the tile centre whenever the game moves you itself: stairs, falling,
  knockback, grabs, teleport, riding a moving vehicle, mounting, loading. Driving is unchanged.

## Multiplayer
- The host moves every body in fixed 20-33 ms ticks; clients send only keys plus a sequence
  number (never a position, so no speed or wall cheats).
- Host sends each player's position/velocity ~20x per second. Your own client moves instantly and
  smoothly corrects small differences over ~100 ms (no snapping). Other players are drawn
  ~100 ms behind with interpolation.

## Drawing
- Sprites at their exact pixel position; the camera already follows smoothly. The row-order
  draw fix (already done for the current tile steps, see roadmap) carries over.

## Decisions for you (these change gameplay/balance)
1. **Walking speed.** Today a turn is 600 ms (option `FLUID_TURN_MS`), so a speed-100 character
   walks 1.67 tiles/s (run 3.3 tiles/s; diagonal walk 1.17 tiles/s). That is the main reason it
   feels slow. Options:
   a. a separate player step scale (e.g. ~220 ms per 100 moves: walk ~4.5 tiles/s, run ~9),
      only players get faster, monsters relatively slower (balance change);
   b. shorten `FLUID_TURN_MS` to ~250 ms: the whole world runs faster, monsters too
      (relative balance unchanged, but everything gets harder to react to);
   c. something in between. My suggestion: (a) with walk ~3.5-4 tiles/s, then tune by playing.
2. **Diagonal speed.** CDDA charges diagonal steps 1.42x (circular distances on). With free
   movement I suggest the same speed in every direction (realistic, no zig-zag tricks).
3. **Squeezing diagonally between two walls**, which CDDA allows: block it (a round body does not
   fit), or allow a forced squeeze when pushing straight at the gap?
4. Soft-wall push time (0.4 s) and body size (0.6 tile): tunable, fine to leave to testing.

## Phases (rough size; each compiles and is tested on its own)
1. Movement maths on the host, single player (~2 days): body, cost-based speed, collision,
   wall sliding, corner nudge, tile-entry hook, snap to centre.
2. Edge cases (~2 days): doors, soft walls, bump attack/talk, stamina/sprint, dragging,
   vehicles, stairs, ledges, swimming, mounts.
3. Drawing (~1 day).
4. Network (~3 days): sequence numbers, position messages, prediction and correction,
   interpolation of other players, protocol version bump.
5. Tests and tuning (~2 days): scripted runs (time over 10 tiles of road/rubble/water equals 10
   native steps, stamina per tile equals native, no position inside a wall after 10,000 random
   inputs, traps fire exactly once, 150 ms lag test without visible snaps). Tile-step mode stays
   behind an option for comparison until you approve the new one.

About 10 working days in total.

## Risks
- Some tile-entry code depends on the order of things inside the native move function. Running
  it at the boundary crossing and refunding its own move charge avoids duplicating it, but needs
  careful testing.
- Host and client maps can briefly differ (a door opened elsewhere): small corrections.
- Very fast characters or long frames could tunnel through thin walls: movement is split into
  sub-steps of at most 0.25 tile.

## Technical appendix
- `src/fluid.h/.cpp`: `body_state` (fixed-point position/velocity) in `actor_state`; tuning
  `body_radius`, `tick_ms`, `soft_wall_ms`; `start_step` becomes `integrate_body` + `enter_tile`
  (keeps the `native_scope` / `avatar_action::move` / `body_drag_step` / `npc::move_to` call and
  pushes the body back if the native move refuses); new `step_cost()` mirroring
  `game::walk_move` without side effects; `corner_slide_dir` replaced by circle-vs-tile sweep;
  `client_frame`/`client_event` prediction replaced by input replay and reconciliation;
  `sprite_offset` returns the body offset for players.
- `src/fluid_device.cpp`, `src/net_client.cpp`: input sequence number, `pos` message.
- `src/net_server.cpp`: `host_drive_remote_players` runs the fixed tick and sends `pos`.
- `src/net_protocol.h`: protocol version bump. `src/cata_tiles.cpp`: exact player position.
- Tests: `tools/mp_tests/movetest.sh` plus new scenes; docs `MULTIPLAYER_FLUID.md`, roadmap.
