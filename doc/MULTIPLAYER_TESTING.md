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
- [ ] Hosting a brand-new world (no character yet) with `--mp-host`: you get the character
      screen; the world's clock does not move until your character is in the game (check the time
      before and after a long stay in the character screen). The same with a dedicated server
      (`--mp-server`): it starts by itself, the first player to connect brings their character.
- [ ] Stations (B3): start a long craft at a workbench, walk away a bit (stay nearby): it keeps
      going slowly and stops at 99%; "Work on craft" finishes it. Workbench menu "5 Queue a recipe
      here": pick a long recipe (30 min+), the components go, the station makes it alone and you
      get "The station finished your ...", the item lies on the workbench. A short recipe is just
      made by you.
- [ ] Groups: in the chat (F2) `/group create Red` on one client, `/group invite <name>` and
      `/group accept` on another, `/group` lists the groups. F4 shows health only for your group.
- [ ] Groups and items: a player of another group taking items from your group's base counts as
      stealing; your own group's members can take them freely.
- [ ] Groups and zones: a zone made in the zone menu (`Y`) is seen by your group only; sorting
      loot uses your group's zones.
- [ ] Groups and the map (M): a player in their own group sees only what their group explored;
      after `/group accept` the group's map shows up; after leaving, the map stays.
- [ ] Groups and NPCs: an NPC that likes the shared group treats a player of a new group as a
      stranger in conversation; trading/favours with one group do not change its opinion of another.
- [ ] Map notes as a client: a note made on the map (M, then N) is still there after reconnecting,
      other members of your group see it, other groups do not; deleting it works the same way.
- [ ] Server rules: with "Multiplayer: PvP" off, shooting through a player of another group does
      not hurt them; on, you can also hit them in melee. With "friendly fire" off your shots pass
      through your own group.
- [ ] Traps: a hidden trap you set (e.g. a beartrap buried) shows up for the other members of
      your group, not for other groups.
- [ ] Sound as a client (sound pack installed): gunshots, zombies, doors near your character are
      heard on your computer, louder when close, from the right side.
- [ ] Explosions (grenade, propane tank): an orange flash grows and fades; the game does not stop
      for it; clients see it too.
- [ ] Spells as a client (a character with a spell, e.g. via the debug menu "learn all spells"):
      the cast key opens the spell list, the spell goes where the mouse points; recast repeats it.
- [ ] Group reading: one player reads a skill book, another of the same group stands close and
      does nothing: the listener gets "You learn a little about ..." too (slower).
- [ ] Auto-travel as a client: on the map (M) pick a place a few tiles away and travel: your
      character walks there by itself; a movement key or a zombie coming stops it.
- [ ] Timed actions as a client after the 2026-10-15 change ("activities run with the body as the
      player"): picking up several items, butchering, crafting, disassembling, reloading: the
      items/results end up on your character; nothing pops up on the host.
- [ ] Progress bars: during a timed action (pulping, butchering, reading) a small green bar fills
      above the character; other players see yours too.
- [ ] Eating as a client (something that takes a while, e.g. a can of food): your hunger goes
      down, not the host's.
- [ ] Vehicle work as a client (examine a car, install/remove a part): the work runs on your
      character with your skills and tools; the host's character keeps doing what it did.
- [ ] Reading a map item (e.g. a road map) as a client: "You learn ..." and the places show up.
- [ ] Groups and missions: a mission accepted by one group is not listed for another group.
- [ ] Recruited NPC follows the player who recruited it, not the nearest player.
- [ ] Debug menu as a client (debug key): spawning an item puts it on your character, teleport
      moves you, overlays show on your screen; quit/quickload/map editor say "Not available in
      multiplayer". World option "Debug menu for clients" turns it off.
- [ ] Combat, wind-up: a zombie about to hit makes the tile it hits glow red and gets a red frame;
      stepping out of the red tile dodges.
- [ ] Combat, hit feel: a hit freezes the swing for a moment, the target flashes white, shakes and
      jerks back. Heavy blunt weapons (bat, sledge) sometimes push the target a tile back (option
      "Melee knockback").
- [ ] Combat, shove: F or the middle mouse button pushes the zombie in front back, sometimes to
      the ground; costs stamina, "too winded" when out.
- [ ] Shooting: tracers from the gun to where the shot ended (also for other players' shots);
      the aim circle grows while walking (more while running) and shrinks while standing.
- [ ] Ranged monsters: a spitter/boomer/turret draws a red aim line (dashed, then solid) and you get
      "takes aim at you" before it fires; ducking out of sight dodges.
- [ ] Reloading while walking (free movement, not running): the reload goes on, a bit slower.
- [ ] Waiting (`|`) in multiplayer is a short break that takes away weariness; walking alone no
      longer makes you weary.

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
