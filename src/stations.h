#pragma once
#ifndef CATA_SRC_STATIONS_H
#define CATA_SRC_STATIONS_H

/**
 * Crafting stations (doc/MULTIPLAYER_ROADMAP.md B3).
 *
 * In realtime / multiplayer play an unfinished craft that lies on a workbench-type furniture
 * (workbench, forge, kiln, chemistry station ...) keeps progressing by itself, at half the
 * speed of a crafter working on it by hand.  Stations keep working while nobody is around:
 * every craft remembers when it was last updated and catches up when its area is loaded again.
 * A finished craft waits on the station; resuming it completes it at once.
 * The places of known station crafts are kept in <world>/mp_stations.json.
 */
namespace stations
{

/** Host: called every turn; does its work once per game minute. */
void process();
/** Host: writes the list of known station crafts (called when the game is saved). */
void save();
/** Forgets the list (the game ended; the next world has its own). */
void reset();

} // namespace stations

#endif // CATA_SRC_STATIONS_H
