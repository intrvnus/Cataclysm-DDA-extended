#pragma once
#ifndef CATA_SRC_MP_TEST_NPC_H
#define CATA_SRC_MP_TEST_NPC_H

#include <string>

#include "coordinates.h"
#include "fluid.h"
#include "type_id.h"

class Character;
class npc;

/**
 * Multiplayer test scenes (host side, only with CATA_MP_TEST_SCENE set).
 *
 * The host sets a scene up next to the body of the first connected player and logs
 * "Multiplayer: test <name> ready"; the run script of that scene (see doc/MULTIPLAYER_TESTING.md)
 * drives the client's keys and reads the replay lines of the host log.
 *
 * CATA_MP_TEST_SCENE=<name>:
 *   disarm    a neutral NPC with a wielded weapon next to the body (menu entry "Disarm")
 *   steal     a neutral NPC with items in its pack next to the body (menu entry "Steal")
 *   useitem   a hurt neutral NPC next to the body, the body carries bandages ("Use item on")
 *   armor     a friendly NPC wearing two layers next to the body ("Sort armor")
 *   console   a computer console next to the body (examine it)
 *   recraft   the body knows what it crafted last and has the materials (repeat last craft)
 *   workout   the body has empty hands and rested (the workout action)
 * The disarm, steal, useitem and armor scenes use the NPC that CATA_MP_TEST_NPC spawns (or any
 * other NPC within 8 tiles of the body).
 */
namespace net
{

/** The one call of host_end_of_turn: runs the scene named by CATA_MP_TEST_SCENE, if any. */
void mp_test_scenes( npc &guy );

// ---- the scenes (mp_test_<name>.cpp); each is called every turn and keeps its own state ----
void mp_test_npc_scene( npc &guy, const std::string &scene ); // disarm, steal, useitem
void mp_test_armor( npc &guy );
void mp_test_console( npc &guy );
void mp_test_recraft( npc &guy );
void mp_test_workout( npc &guy );

// ---- helpers shared by the scenes (mp_test_npc.cpp) ----
/** The nearest NPC within @p radius of @p guy that is not played by a client, else nullptr. */
npc *mp_test_find_npc( const npc &guy, int radius );
/** Moves @p other to a free tile next to @p guy (north first) unless it already stands next to
 * it.  Returns false if there is no free tile. */
bool mp_test_put_next_to( npc &guy, npc &other );
/** The xdotool key name that walks / aims from @p from towards @p to (arrow keys and keypad
 * digits: they work in the classic and in the WASD layout), "none" if it is the same tile or
 * not next to it. */
std::string mp_test_dir_key( const tripoint_bub_ms &from, const tripoint_bub_ms &to );
/** How many of @p id @p who carries (wielded, worn, in the pack and in containers). */
int mp_test_count( const Character &who, const itype_id &id );
/** Name of what @p who wields, or "none". */
std::string mp_test_weapon( const Character &who );
/** True from @p delay_ms after the first call on (@p start is 0 before the first call). */
bool mp_test_due( fluid::ms_t &start, int delay_ms );
/** True every @p period_ms (for the state lines of a scene); @p last starts at 0. */
bool mp_test_every( fluid::ms_t &last, int period_ms );

} // namespace net

#endif // CATA_SRC_MP_TEST_NPC_H
