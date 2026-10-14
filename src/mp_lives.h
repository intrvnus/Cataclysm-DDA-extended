#pragma once
#ifndef CATA_SRC_MP_LIVES_H
#define CATA_SRC_MP_LIVES_H

#include <optional>
#include <string>

#include "coordinates.h"

class Character;
class avatar;

/**
 * Multiplayer lives (C1, Noah 2026-10-14), after the mod "Extra Lives" by ProHere
 * (https://github.com/ProAtWorkHere/CDDA-Extra-Lives), adjusted for multiplayer:
 *  - a player who bleeds out (see the downed state in net_server.cpp) comes back as the same
 *    character where they fell, if a life is left: healed, bad conditions removed, a short time
 *    of speed / invisibility / incorporeality and waves of force pushing enemies away;
 *  - the world option MP_LIVES_DIFFICULTY (tourist .. very hard) sets the lives, the maximum,
 *    the cap, how often a life comes back and whether the items stay where the player died;
 *  - no popups (they would stop the world for everybody); the respawn is always "here";
 *  - with no life left the player has to make a new character.
 * Every player (the host's avatar and each client's body) has their own count, kept in the
 * character's values (saved with the character).
 */
namespace mp_lives
{

/** Sets the counters of @p who from the difficulty, once per character. */
void init( Character &who );
/** Lives left; a negative number means unlimited. */
int lives_left( const Character &who );
/** Uses a life if there is one.  False: no life left (permanent death). */
bool spend_life( Character &who );
/** Comes back to life where the character fell (after spend_life): healed, effects, waves of
 * force.  The caller then moves it to its bed (bed_of), if it has one. */
void respawn_here( Character &who );
/** Where the character last fell asleep (the respawn point), if it ever slept. */
std::optional<tripoint_abs_ms> bed_of( const Character &who );
/** Every turn for every player: lives that come back over time, the respawn grace (healing
 * again for a few seconds) and the waves of force. */
void tick( Character &who );
/** Text for the player: "2 extra lives left" / "unlimited lives". */
std::string lives_text( const Character &who );

/** Runs the character creation for a player who has no life left and puts the new character
 * into @p u (keeping its id and position).  Blocks while the player makes the character. */
void make_new_character( avatar &u );

} // namespace mp_lives

#endif // CATA_SRC_MP_LIVES_H
