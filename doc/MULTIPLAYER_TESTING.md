# Testing the multiplayer build (for Noah and friends)

Build: GitHub Actions workflow "Windows Tiles build (manual)", artifact `cdda-windows-tiles-custom`
(a zip with `cataclysm-tiles.exe`, `data/`, `gfx/`, ...). Unzip it into its own folder (do not
mix it into a normal game folder).

## Start

Open `cmd` or PowerShell in the unzipped folder (the exe prints to that console).

- **Host** (plays and owns the world):
  `cataclysm-tiles.exe --world "<name of an existing world>" --mp-host`
- **Dedicated server** (nobody plays on it, it keeps the world running and saves itself every 10 min):
  `cataclysm-tiles.exe --world "<world>" --mp-server`
- **Client** (own character, saved on the host; the client's world is only a view):
  `cataclysm-tiles.exe --world "<any world with the same mods>" --mp-connect <host address>:7777`

Same machine: host with `127.0.0.1`. Over the internet the host opens UDP port 7777 in the
firewall/router. Everybody needs the same build and the same mods. Your character is identified by
the key in `config/mp_identity.txt`: rejoining puts you back in your own body.

## Controls (multiplayer is always "fluid", real time)

| Input | Effect |
|---|---|
| W A S D (held) | Walk. Two keys = diagonal (press them almost together). Last pressed wins if you hold opposing keys. |
| Tap a key | exactly one tile |
| Shift (held) | Sprint (costs stamina, about 34 s from full) |
| Mouse | Aim and facing. Left click = melee toward the cursor (8 sectors of 45 degrees) or fire. Right click held = steady the aim. |
| Space | Cancel a wind-up / drop the aim |
| F2 | Chat (say something to everybody) |
| F4 | List of players (health, distance) |
| G + direction | Grab furniture or a vehicle (push by walking into it, pull by walking away) |
| Throw key | Throw the wielded item at the cursor |
| Menus | inventory, crafting, construction, examine, pick up ... run on your own computer; the result is sent to the host |

Stamina: walking, crouching and crawling are free; sprint, melee, throwing cost. Regeneration
starts 1.5 s after spending (full bar ~20 s standing), slower under a heavy load or when hungry
or thirsty. At 0 you cannot sprint until 30% is back.

## What to try (please break it)

1. **Movement feel:** hold a direction, tap, diagonals from standstill, change direction while
   moving, hold two opposite keys, bump into corners (you should slide along a wall), walk into
   a monster (attacks). Look for stutters, stray steps, jumps back ("rubber-banding").
2. **Melee:** click with the cursor around you (8 directions), next to zombies.
3. **Camera:** slight lead toward the mouse; option `FLUID_CAMERA_LEAD` switches it off.
4. **Bubbles:** every player has a reality bubble of their own: walk or drive far away from the
   host (hundreds of tiles), the world around you keeps running; monsters near you chase you.
5. **Cars:** take the controls (the vehicle key), start the engine, drive; a second player can ride.
6. **Menus and actions:** craft, build, pick up, inventory, talk to NPCs (walk into them), trade,
   zones, map, missions, sort armor, grab.
7. **Down and out:** get downed (a friend helps you up by walking into you), bleed out (new body).
8. Leave and rejoin: you should be back where you were, with your things.

## New in the build of 2026-10-15 (please check)

Host and client need this build (network protocol 3).

- [ ] Free movement, solo: walk, run, diagonals, sliding along walls, slipping round corners.
- [ ] Fire, a known trap or a roof edge stops you at the edge; keep pushing about 0.4 s to go in
      (roof edge: the climb-down menu opens). No "Really step into ...?" question any more.
- [ ] Client: your own character moves at once and smoothly, also when sprinting; walls and closed
      doors do not make it jump back and forth.
- [ ] Client: other players glide smoothly (they are drawn about 0.1 s in the past).
- [ ] Client: at a roof edge you are told to examine it (e) to climb down.
- [ ] Client: a washing machine / dishwasher in a vehicle uses your detergent, examining a known
      trap works for you, building planks from a log puts them at your feet, and you can pick locks.
- [ ] Client riding a horse or mech: throwing and firing an energy weapon do not crash the host.
- [ ] Client: rain makes you wet, bright sun or snow can dazzle you (as for the host).
- [ ] Client: when the host opens a menu you get "The world waits: the host is busy", and
      "The world goes on." afterwards.
- [ ] Options page, fluid section: "Free movement: push time into danger", "body width" and
      "other players' delay" can be changed (keep body width the same on host and clients).
- [ ] Lives: running out of lives, making a new character and joining again.

## When something breaks

Send me (or write down): what you did, what you expected, what happened, and the debug logs:
`config/debug.log` of the host AND of the client (in the game folder under `config/`). Mention
whether you were host, dedicated server or client. Screenshots help.

## Known limits

- Classic (non-fluid) shooting/throwing targeting is not used in multiplayer.
- The Windows build is new: expect rough edges. Windows firewall may ask for permission for UDP.
- Not run in a real game yet: factions window, pets/mounts, computers, disarm/steal prompts, sort
  armor for friends, repeat last craft, workout, a created zone in the zones window, loading
  saved map memory.
- Stray log line "monster can't move to its location" near map edges is a harmless game message.
