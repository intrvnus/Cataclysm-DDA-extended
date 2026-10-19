# Multiplayer vehicle plan

Status: **phase 0 done** (`ff6683b`), later phases not started.

## The observed bugs

- A client could keep `controlling_vehicle` after the host ejected it on a crash.  It then sent drive
  commands (or fell through to walking) for a vehicle the host no longer let it drive; the body
  moved south / appeared to bug around.
- A full world turn moves a vehicle several tiles.  Vehicles are still advanced by
  `map::vehmove()` / `vehproceed()`; the client receives a new submap afterwards.  That is the
  "teleporting" vehicle.
- Vehicle input is one `drive` command per key action (`send_ui("drive")`), not a held
  steer/throttle stream.  Reversing and gentle acceleration therefore behave like discrete
  commands.
- `vehicle::cruise_thrust()` already supports negative cruise velocity and
  `vehicle::max_reverse_velocity()`, so the reverse math exists; the broken part is the client
  control/input path and authoritative state.
- `vehicle::handbrake()` exists in `vehicle_use.cpp` but is a one-shot menu action, not a held
  brake.

## Phase 0 - done (`ff6683b`)

- Host sends `veh` state at 20 Hz and immediately on entering/exiting/ejection/start/stop/reverse:
  `in_vehicle`, `controlling_vehicle`, seat, avatar/vehicle position, facing, velocity, cruise
  velocity, skidding.
- Client applies the flags, clears stale drive frames, stops sending drive commands after ejection,
  places the avatar at the host's position when leaving, and sends no walking frames while seated.
- Does **not** make vehicle motion smooth, fix reverse acceleration, or add the handbrake.

## Phase 1 - real-time vehicle input and reverse

- Replace one-shot `drive` commands with a vehicle input frame:
  `steer`, `throttle`, `brake/handbrake`, sequence, dt.
- Poll held A/D and W/S; send at frame rate.
- Host applies input every frame:
  - W accelerates forward.
  - S brakes to zero, then continues into reverse (negative velocity).
  - A/D steer; the existing steering skill/proficiency/fumble rules still apply.
- Add a test hook: drive into a wall, reverse away, assert the velocity sign, position and
  `controlling_vehicle`.
- Keep the phase 0 state packet as the authority.

## Phase 2 - smooth movement and camera

- Keep the logical vehicle on the tile grid; add a visual sub-tile offset for vehicle parts (the
  same idea as the player body slide):
  - logical tile changes at tile crossings/collisions,
  - sprites slide from old to new position,
  - camera follows the interpolated vehicle, not the tile center.
- Client predicts its own vehicle from input and reconciles against the host `veh` state.
- Remote vehicles interpolate between host samples.
- Collision and footprint remain authoritative on the host.
- Turn/facing interpolation can be added after position interpolation.

## Phase 3 - handbrake

- While `controlling_vehicle`, holding **Space** is the handbrake; outside a vehicle Space keeps
  its cancel/dodge/focus meaning.
- Each frame, target cruise velocity 0 and apply progressive deceleration:
  - brake force scales with current speed,
  - never flips the sign into reverse,
  - full stop below a small threshold,
  - steering stays possible; optional skid at high speed/turn.
- Releasing Space resumes normal throttle/brake.
- Show a handbrake indicator; predict it on the client and reconcile with the host.

## Test checklist for later phases

- Client crash/ejection: no phantom driving, no south movement, host and client agree on
  `in_vehicle` / `controlling_vehicle`.
- Reverse: S from forward brakes to zero, then negative velocity; W from reverse brakes to zero,
  then forward.
- Smooth: no multi-tile jump per world turn; camera follows;
  corrections are small and eased.
- Handbrake: hold Space from speed, decelerate progressively to zero; release and accelerate again;
  no sign flip; passenger Space does nothing.
