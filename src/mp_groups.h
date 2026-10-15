#pragma once
#ifndef CATA_SRC_MP_GROUPS_H
#define CATA_SRC_MP_GROUPS_H

#include <string>

class Character;

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
} // namespace mp_groups

#endif // CATA_SRC_MP_GROUPS_H
