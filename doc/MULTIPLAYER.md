# Experimental multiplayer (milestone 1)

Status: compiles (Linux CI). Tested on Linux, curses build, host + 1 client on localhost:
connect/join, client movement seen on the host, client view following, pick up reply,
time running on both sides with nobody pressing keys. Not yet tested: attacking monsters,
the Windows tiles build, more than one client.

## How to start

Both players need the same game build, and the host's and the client's worlds must have
the same mods enabled (the client gets map data with the host's mod items in it).

Host (plays normally, owns the world):

    cataclysm-tiles --world <host world> --mp-host [port]

Client (only shows the host's world; use a throwaway world, it is never saved):

    cataclysm-tiles --world <any world> --mp-connect <host address>[:port]

The default port is 7777 (UDP). After loading, the host's message log says
"Multiplayer: hosting…", and the client is placed next to the host as soon as it joins.

## What works in milestone 1

- Host + up to 4 clients in one reality bubble (the host's).
- Continuous time: while a session runs, realtime turns are on (the `TURN_DURATION`
  option, or 1 second per turn if it is off). Nobody waits for anyone's input; a player
  who does not press a key just waits that turn.
- Client: move, attack by walking into a monster, wait (`.`), pick up everything on the
  own tile (`g`/`,`), look around (`x`). Escape leaves the session.
- The client sees terrain, furniture, items and vehicles (whole submaps sent in the
  savegame format), monsters, the host and the other clients, and the game time.

## How it works

- `src/net_session.*`: ENet setup, connections, command line requests.
- `src/net_server.cpp` (host): each client is an NPC on the host. In `monmove()` that NPC
  does not use the NPC AI; it executes the commands its client sent (or waits). At the end
  of every turn the host sends each client the changed submaps and the state
  (own position and HP, other players, monsters, turn).
- `src/net_client.cpp` (client): `do_turn()` does not simulate anything; it applies what
  the host sends and sends key presses.
- `src/net_protocol.h`: message list. Every message is one JSON object.
- ENet 1.3.18 is vendored in `src/third-party/enet` (MIT license).

## Known limits (later milestones)

- No saving or reconnecting for clients; the client's NPC on the host is a random NPC.
- Most menus (inventory, crafting, …) are not available to clients; the host's menus
  still pause the world.
- No sleep or long activities for clients.
- Monsters on the client are recreated every update (no animations/effects shown).
- Parts of the client's map outside the host's reality bubble come from the client's
  own world.

## Planned (wished for by Noah)

- Mod features for clients: each player can use their own mod items and windows, e.g.
  their own mp3 player with the Own Music Player window and album choice, mod hotkeys,
  item marks menus. Needs client menus/item use forwarded to the host first.
