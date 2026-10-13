# Multiplayer build: progress overview

Branch: `claude/fluid-proto` (repo intrvnus/Cataclysm-DDA-extended). Details and decisions: `doc/MULTIPLAYER_ROADMAP.md`.

## What it is
Host + clients over ENet. The host simulates the world. Each client has its own character (saved on the host, rejoin by key) and plays in a "fluid" real-time mode (WASD, mouse aim, throw at cursor). Client menus run on the client; the host repeats the chosen answers on the player's body (`src/remote_ui.*`, record/replay).

## Works (tested in game, Xvfb host + client)
- Join, own character, saved and restored on reconnect (A4).
- Fluid movement, melee, throwing at the mouse cursor.
- Menus: apply (lighter etc.), examine, selective pickup, advanced inventory, bionics, mutations, unload, disassemble, smash, inventory screen with item menu (wield, wear, eat, drop...), crafting, doors, look, character sheet, sleep.
- NPCs: walking into one opens the normal menu (talk, push, examine wounds, attack, disarm, steal, trade). Dialogue and trading run on the client and the host carries out the deal. No chat key.
- Stairs and ladders: a player may be on another floor than the host.
- Local views and settings (zoom, item list, medical, options, auto-pickup, colours...).
- Client actions never touch the host's avatar (checked for wield).

## Written but not tried
- Player cannot be seen, targeted or hurt while talking (for portal storms etc.): code only, no hazard tested.
- Mend, fluid fire key, push/disarm/steal outcomes.

## Already in the code from earlier sessions (found by reading it; not re-tested by me)
- A1 world clock: `REALTIME_DAY_MINUTES` scales calendar time against action time (`do_turn.cpp`).
- A2 dedicated server: `--dedicated` runs the host without a player; the host avatar becomes an invisible, invulnerable ghost that follows the group (also a first answer to C2: the bubble follows the players' centre, but players far apart are still not simulated).
- A3: every player leaves scent; monsters see player bodies as NPC targets. Many AI places still look only at the host avatar (about 30 in `monster.cpp`/`monmove.cpp`): monsters' anger, aggro and "see the player" checks are not converted.
- A5/B1: non-blocking mode (`alerts`): warning sounds, coloured messages and arrows instead of stopping.
- B3 crafting/work stations (`stations.cpp`), B5 reading as a timed action and focus minigame, B6 sleep skip when everybody sleeps, B7 timed actions cancel on movement, B4 sprint by holding a key, B9 turret/monster wind-up alerts, C1 downed players that others can help up.

## Not done
- **Separate reality bubbles (C2):** only the dedicated ghost following the group's centre; players far apart still need a design decision.
- Gameplay still open: stamina rework (B2) and weariness (B10) not checked; crouch/prone key layout not checked; C7-C9 tuning.
- Systems: vehicles (C3, with grab), shared faction/ownership and the faction menu (C4/C5), basecamps (C6), chat/player list (C15), network efficiency (C17).
- Menus still missing for clients: spells (not needed), classic (non-fluid) shooting, examining creatures/vehicles/consoles, factions menu, follower orders, NPC "sort armor".
- Wait is refused on purpose (the world does not stop).

## Next
Finish the remaining basic menus and untested items, then stop for a decision (C2) before the next phase. No Windows build until this batch is done.
