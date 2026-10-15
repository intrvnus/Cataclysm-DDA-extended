# Player factions (C4): options paper for Noah

Status (2026-10-15): Noah agreed to all recommendations, with one change: a recruited NPC follows the player who gave it the order. Done (compile + CI only): followers per commander (`mp_leader`, `npc_player_for`), player groups via chat (`src/mp_groups.cpp`, world option `MP_GROUPING`), health in F4 only for the own group, missions per group (`mission_groups.json`). Open: per-group factions (needed for theft across groups, per-group zones/map/camps); they change how NPCs see players and need a play test first. This paper lists how factions work today, what a player-faction feature needs, and the decisions only you can make. My recommendation is given for each.

## How it works today
- Every remote player's body is in the faction `your_followers` (like the host's avatar). So every player is automatically allied to every other player, and nothing a friend owns counts as stolen (item/vehicle `owner` is a faction id).
- NPC allies/followers follow the **host avatar only** (`npcmove.cpp` uses `get_player_character()`); monsters and specials already use the nearest player.
- Missions are one list on the host avatar (group list); `mission::owner_of` maps every remote player to the host. Your decision "missions per player, one list per faction with shared progress" is not implemented yet.
- Zones are one global `zone_manager`; overmap knowledge is one global buffer (so "shared per faction" is today "shared by everybody").
- Fluid melee/ranged can only target hostile creatures, so allied players cannot hit each other (no PvP). Explosions/fire probably hurt everybody (not verified).
- The F4 player list shows health/distance of all players (your C15 "only allies see health" is not implemented).
- Runtime creation of factions already exists in the engine (`faction_manager::add_new_faction`).

## What the feature needs (minimal path)
1. A `player_group` record (name, leader, members, friendly-fire flag) created as a runtime faction; persist the group per player so a reconnecting player returns to it.
2. Commands as client menu messages: create, invite, accept, leave, kick, rename, dissolve.
3. Replace the hard-wired "is `your_followers`" checks by "same faction as the viewer" (`npc::is_ally`, `avatar::get_faction`, follower logic); keep monster faction `player` for all groups (simplest).
4. Missions move from the host avatar to the faction (one mission object per faction, per-member "assigned to").
5. NPC followers get a "leader" (follow the nearest member of the group).
6. Zones, overmap knowledge, factions view become per faction; F4 health filtered by faction.
7. Ownership transfer when a player leaves; basecamp owner stays the faction.

## Decisions I need from you (with my recommendation)
1. **Default grouping:** (a) everybody starts in one shared group (today's behaviour), (b) everybody starts alone and must invite/accept, (c) server option, default (a). **Recommend (c) with default (a).**
2. **Friendly fire inside a group:** always off / leader's flag / server option. **Recommend server option, default off**; explosions and fire need an explicit rule either way.
3. **Players of different groups:** never hostile / neutral with optional "declare war" / hostile by default with a PvP server flag. **Recommend never hostile first**, "declare war" later.
4. **NPC reputation:** per group / one global / per group but copied from the founder. **Recommend per group** (one player's behaviour does not hurt everybody).
5. **A player leaves or is kicked:** keeps no missions; may claim items/vehicles he owns within a time window; camps stay with the group. **Recommend exactly that.**
6. **A group dissolves:** destroy / archive (kept with zero members, can be recreated) / transfer to the leader. **Recommend archive** for camps and ownership, with a clear warning.
7. **Recruited NPC followers:** belong to the group and follow the nearest member / belong to the recruiter. **Recommend the group** (needs an AI change in about 30 places in `npcmove.cpp`).
8. **Roles:** flat / leader plus members / creator is leader with promotion. **Recommend leader plus members** (flat invites invite griefing).
9. **Overmap knowledge when joining/leaving:** joiner receives the group's map, a leaver keeps a copy. **Recommend that.**
10. **Theft across groups:** another group's vehicles/storage/camps count as theft with witnesses (as for NPC owners today). **Recommend yes.**
11. **C15 scope:** only same-group players see each other's health (alliances between groups later).
12. **Admin:** unique group names, a cap on the number of groups, leader-only rename.

## Suggested order once decided
1. Per-player faction ids with default "everybody in one group", the health filter, per-faction zones/overmap/factions view.
2. Missions moved to the faction.
3. Create/leave/invite/kick.
4. NPC follower leader.
5. PvP and friendly-fire server options.
