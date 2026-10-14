#pragma once
#ifndef CATA_SRC_BUBBLES_H
#define CATA_SRC_BUBBLES_H

#include <vector>

#include "coordinates.h"

class map;

// Experimental multiplayer: extra reality bubbles.
// The game's own map ("the main bubble", game::m) follows the host's avatar.  Every other
// player can get a bubble of his own: another map over the same shared world data
// (MAPBUFFER).  Creatures and items keep absolute coordinates, so there is still one world;
// each bubble is only the loaded and simulated area around one place.
namespace bubbles
{
/** Opens (loads) a bubble whose south-west-most submap is @p origin. */
map &open( const tripoint_abs_sm &origin );
/** The extra bubble that contains @p p, or nullptr (then it is in the main bubble or nowhere). */
map *extra_containing( const tripoint_abs_ms &p );
bool in_extra( const tripoint_abs_ms &p );
/** True if an extra bubble has this submap / overmap terrain loaded: it must stay in memory. */
bool keeps( const tripoint_abs_sm &p );
bool keeps( const tripoint_abs_omt &p );
const std::vector<map *> &extras();
/** The main bubble first, then every extra bubble. */
std::vector<map *> all();
/**
 * Every creature, submap, field, item and vehicle is simulated by exactly ONE bubble even where
 * bubbles overlap: the bubble (of those that contain it) whose centre is nearest, the main
 * bubble winning ties.  Per-bubble processing loops must skip what they do not own.
 */
bool owns_submap( const map &m, const tripoint_abs_sm &sm );
bool owns( const map &m, const tripoint_abs_ms &p );
/** The map that owns the position @p p: an extra bubble or the main one. */
map &owner( const tripoint_abs_ms &p );
/** Per-turn upkeep of the extra bubbles (what do_turn does for the main one). */
void tick();
void close_all();
} // namespace bubbles

#endif // CATA_SRC_BUBBLES_H
