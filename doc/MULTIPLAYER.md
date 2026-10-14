# Experimental multiplayer: playing with friends

Status: works between a host and clients on one machine (Linux, Xvfb tests) and is meant for
friends on a LAN or over the internet (UDP port 7777). The Windows tiles build has not been
tried yet. For what is done and what is open see `MULTIPLAYER_PROGRESS.md` (short) and
`MULTIPLAYER_ROADMAP.md` (full plan).

## How to start

Everybody needs the same game build, and the host's and the client's worlds need the same mods.

Host (plays normally and owns the world):

    cataclysm-tiles --world <host world> --mp-host [port]

Dedicated server (nobody plays on the server itself; it follows the group and keeps running
without any menus, saves itself every 10 minutes):

    cataclysm-tiles --world <world> --mp-server [port]

Client (makes its own character; the client's world is only a view and is never saved):

    cataclysm-tiles --world <any world> --mp-connect <host address>[:port]

Open UDP 7777 on the host's router/firewall for play over the internet.

Your character is saved on the host. A secret key in your `config/mp_identity.txt` identifies
you: when you come back, you are back in your own body where you left it.

## How it plays

- The world runs in real time for everybody; nobody waits for anybody. The host's own menus do
  not stop the clients' game on a dedicated server (on a normal host they still do).
- Days are short in multiplayer: the option `REALTIME_DAY_MINUTES` sets the real-time length of
  one day (needs, healing, plants, weather follow the calendar; fights and walking follow
  action time).
- Multiplayer is always fluid mode (real-time movement and combat; the `FLUID_MODE` option only matters for solo play): walk, swing
  at enemies, aim and fire with the mouse, throw with the throw key. See
  `MULTIPLAYER_FLUID.md`.
- Menus run on your own computer and only their result is sent to the host: inventory and item
  menu, crafting, construction, examine, pick up, advanced inventory, bionics, mutations, sort
  armor, smash, haul, workout, peek, item action menu, computers, pets, vehicle controls and
  driving, disarm/steal/heal on NPCs. Missions are a group list; zones are shared; the
  overmap shows what any player has seen.
- Talk to people by walking into them: you get a menu (talk, push, examine, attack, trade ...).
  While you talk you cannot be seen or hurt.
- Chat: `F2` says something to everybody, `F4` lists the players with health and distance (the host has the same keys).
- Dangers warn you with a sound and a coloured message instead of stopping the game.
- Downed players can be helped up by walking into them.
- Everybody sleeping skips the night.

- Network: messages are compressed; one client uses roughly 15-30 KB/s while moving fast (about half of that without compression).

## Known limits

- Players must stay within about 60 tiles of each other (one simulated area); the dedicated
  server follows the middle of the group.
- Grab (`G`) works for furniture and vehicles (push it by walking into it, pull it by walking away; stepping sideways lets go; vehicles use the same code as the host's own grab). Spells are not available; the factions window is a read-only list.
- Shooting and throwing work in fluid mode only (by design; the classic targeting menus are not used in multiplayer).
- "Wait" is refused on purpose (the world does not stop).

## How it works

- `src/net_session.*`: ENet setup, connections, command line requests.
- `src/net_server.cpp` (host): each client is an NPC body on the host. Its client sends input
  and menu answers; the host repeats them on the body. At the end of every turn the host sends
  changed submaps and the state.
- `src/net_client.cpp` (client): does not simulate anything; shows what the host sends and
  sends input and menu results.
- `src/remote_ui.*`: record/replay of menus (the client answers a menu on its own copy of its
  character and the host repeats the answers on the body).
- `src/net_protocol.h`: the messages (one JSON object each). ENet 1.3.18 is vendored in
  `src/third-party/enet` (MIT license).

## Test helpers (environment variables on the host)

`CATA_MP_TEST_NPC`, `CATA_MP_TEST_STAIRS`, `CATA_MP_TEST_ZOMBIE`, `CATA_MP_TEST_VEHICLE`,
`CATA_MP_TEST_BEAT`: spawn an NPC / put a joining player on stairs / spawn a zombie / place a car
and seat the player / log a heartbeat of the dedicated server. Used by the automated checks.

## Stamina (multiplayer)
- Walking, crouching and crawling never cost stamina. Running (sprint), swimming, melee, throwing and the like do, with the usual modifiers: your maximum comes from cardio fitness and traits, encumbrance and load raise the cost, bionics and enchantments apply.
- Regeneration starts 1.5 s after the last spending; a full bar takes about 20 s standing, about 15 s crouched, about 13 s lying down. A heavy load slows it (full speed up to half your carrying capacity, half speed at full load); hunger and thirst slow it too (x0.75 hungry/thirsty, x0.5 very hungry/very thirsty).
- At 0 you are exhausted: no sprinting until 30% is back. A default character sprints about 34 s (about 80 tiles) from full.
