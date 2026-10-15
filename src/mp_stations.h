#pragma once
#ifndef CATA_SRC_MP_STATIONS_H
#define CATA_SRC_MP_STATIONS_H

#include "coords_fwd.h"

class Character;

/**
 * B3 crafting stations for multiplayer (Noah 2026-10-15: "stations keep working when nobody is
 * around"; doc/MULTIPLAYER_STATIONS_OPTIONS.md, option C).
 *
 * A: a half-made craft left on a workbench keeps progressing on its own at a quarter of the
 *    normal speed while nobody works on it, up to 99%: the player finishes it (skill checks and
 *    failures happen then).
 * B: a station recipe can be queued at a workbench (see queue_menu): the components are taken at
 *    once and the station makes the items one after another in world time, results land on the
 *    station's tile.
 * Host only; kept with the world in mp_players/stations.json.
 */
namespace mp_stations
{
/** A craft is being worked on at @p where (a tile with a workbench): remember the station. */
void note_craft_at( const tripoint_abs_ms &where );
/** Host, once per turn: unattended progress and queued recipes. */
void tick();
/** True while queue_menu runs: Character::start_craft queues the craft instead of starting it. */
bool queue_mode();
/** The station menu of @p who at the workbench @p where (queue a recipe, see the queue). */
void queue_menu( Character &who, const tripoint_abs_ms &where );
void load();
void save();
} // namespace mp_stations

#endif // CATA_SRC_MP_STATIONS_H
