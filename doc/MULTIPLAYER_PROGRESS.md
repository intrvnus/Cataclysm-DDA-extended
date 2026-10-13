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

## Not done
- **Separate reality bubbles (C2):** the engine simulates one area around the host. Needs a design decision: tether distance with warning, or a dedicated server.
- Foundations: 60-minute day clock (A1), world keeps running behind host menus / dedicated server (A2), AI for several players (A3), no blocking popups (A5).
- Gameplay: safe-mode warnings, stamina, crafting stations, movement modes, reading/training, sleep skip, short timed actions, turret warnings, weariness (B1-B10).
- Systems: death and respawn, vehicles, shared faction/ownership, basecamps and zones, chat/player list, network efficiency.
- Menus still missing for clients: vehicles, spells, construction, zones/loot/haul/grab, overmap, missions/factions/diary, classic (non-fluid) shooting, "use item on" and "sort armor" for NPCs, follower orders, examining creatures/vehicles/consoles.
- Wait is refused on purpose (the world does not stop).

## Next
Finish the remaining basic menus and untested items, then stop for a decision (C2) before the next phase. No Windows build until this batch is done.
