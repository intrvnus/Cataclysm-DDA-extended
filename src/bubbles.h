#pragma once
#ifndef CATA_SRC_BUBBLES_H
#define CATA_SRC_BUBBLES_H

#include <optional>
#include <vector>

#include "coordinates.h"
#include "current_map.h"

class map;
class Character;
class scent_map;

// Experimental multiplayer: extra reality bubbles.
// The game's own map ("the main bubble", game::m) follows the host's avatar.  Every other
// player can get a bubble of his own: another map over the same shared world data
// (MAPBUFFER).  Creatures and items keep absolute coordinates, so there is still one world;
// each bubble is only the loaded and simulated area around one place.
namespace bubbles
{
/** Multiplayer hosts give every player's body a bubble of its own (CATA_MP_NO_BUBBLES switches that off). */
bool enabled();
/** Opens (loads) a bubble whose south-west-most submap is @p origin. */
map &open( const tripoint_abs_sm &origin );
/**
 * Opens a bubble of its own for the player's body @p body_id, centred on @p centre. A bubble the
 * body had before is closed.
 */
map &open_for( const tripoint_abs_ms &centre, int body_id );
/** The bubble of this player's body; nullptr for everybody else (and for bodies without one). */
map *bubble_of( const Character &who );
/** The map that simulates @p who: the body's own bubble, else the owner of its position. */
map &map_for( const Character &who );
/** The player's body leaves: its bubble is closed. */
void release( int body_id );
/** True if the position is inside the main bubble or an extra one. */
bool in_any( const tripoint_abs_ms &p );
/** Every body's bubble follows its body (shifts, loads NPCs, spawns monsters); once per turn. */
void follow_all();

/** Runs the following code as seen from the body's bubble (get_map() is that bubble). */
class view
{
    public:
        explicit view( const Character &who );
    private:
        std::optional<swap_map> swapped;
};
/** The extra bubble that contains @p p, or nullptr (then it is in the main bubble or nowhere). */
map *extra_containing( const tripoint_abs_ms &p );
bool in_extra( const tripoint_abs_ms &p );
/** True if an extra bubble has this submap / overmap terrain loaded: it must stay in memory. */
bool keeps( const tripoint_abs_sm &p );
bool keeps( const tripoint_abs_omt &p );
const std::vector<map *> &extras();
/** True if the submap is really inside the grid of the map (the map's own inbounds() is coarser). */
bool holds( const map &m, const tripoint_abs_sm &sm );
/** Stores monsters and NPCs that are in no bubble any more. */
void unload_outside();
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
/**
 * The scent grid of the bubble @p m: game::scent for the main bubble, otherwise the one owned
 * by that extra bubble (created in open(), freed in close_all()).  Its coordinates are the
 * bubble-local ones of @p m.  get_scent() returns this for get_map().
 */
scent_map &scent_of( const map &m );
/**
 * Shifts the scent grid of the bubble @p m by @p shift (in map squares, as game::shift_map
 * passes it to scent_map::shift).  The code that moves an extra bubble (bubbles::follow) must call this, the way
 * game::shift_map shifts game::scent for the main bubble.
 */
void shift_scent( const map &m, const point_rel_ms &shift );
/**
 * Bubbles that share submaps with another bubble keep their own caches (vehicles, transparency,
 * floors ...); a change made through one is not seen by the other until its caches are rebuilt.
 * Called once a turn before the simulation.
 */
void sync_caches();
/** Rebuilds the vehicle caches of every bubble that shares submaps with another (after vehicles moved). */
void sync_vehicle_caches();
/** Per-turn upkeep of the extra bubbles (what do_turn does for the main one). */
void tick();
void close_all();
} // namespace bubbles

#endif // CATA_SRC_BUBBLES_H
