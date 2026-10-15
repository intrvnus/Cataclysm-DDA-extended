#pragma once
#ifndef CATA_SRC_MP_GROUPS_H
#define CATA_SRC_MP_GROUPS_H

#include <string>

#include "coords_fwd.h"

class Character;
enum class om_vision_level : int8_t;

/**
 * Player groups (C4, Noah 2026-10-15).  Every player belongs to one group, kept on the character
 * (value "mp_group", empty = the shared group everybody starts in with the world option
 * MP_GROUPING "shared").  Groups are run from the chat: /group create|invite|accept|leave|kick.
 * Players of different groups are never hostile to each other; only players of the same group
 * see each other's health.
 */
namespace mp_groups
{
/** The group of @p who ("" is the shared group). */
std::string group_of( const Character &who );
bool same_group( const Character &a, const Character &b );
/** A player came into the world: with MP_GROUPING "solo" they start in a group of their own. */
void on_join( Character &who );
/** A "/group ..." chat line of @p who (the text after "/group"); returns the answer for them. */
std::string command( Character &who, const std::string &args );
/** Missions belong to the group of the player who accepted them (kept with the world). */
void note_mission( int mission_id, const std::string &group );
/** The group a mission belongs to ("" = the shared group, also for older missions). */
std::string mission_group( int mission_id );
/** Overmap knowledge per group (C4, Noah's decision: shared per faction).  The shared group
 * uses the world's overmap; every other group keeps its own list of known overmap tiles.
 * Whoever joins a group brings their map into it; whoever leaves keeps a copy. */
void reveal( const Character &who, const tripoint_abs_omt &p );
om_vision_level seen_by( const Character &who, const tripoint_abs_omt &p );
/** Mission groups and group maps from / to mp_players/ of the world. */
void load();
void save();
} // namespace mp_groups

#endif // CATA_SRC_MP_GROUPS_H
