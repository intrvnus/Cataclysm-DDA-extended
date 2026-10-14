#pragma once
#ifndef CATA_SRC_MP_TEST_MENUS_H
#define CATA_SRC_MP_TEST_MENUS_H

class npc;

// Automated test hooks for the client menus (zones, pets, factions, map memory).  Every hook does
// nothing unless its environment variable is set; the scripts that use them are in the test
// scratchpad (zones_test.sh, pet_test.sh, factions_test.sh, mapmem_test.sh).  All output lines
// start with "Multiplayer: test <name>".
namespace net
{

/** Host, CATA_MP_TEST_ZONE: creates the zone "TESTZONE" of the group next to @p guy (a client's
 * body) and logs the zones the host holds every few seconds. */
void mp_test_zone( npc &guy );
/** Host, CATA_MP_TEST_PET: spawns a tame dog next to @p guy and logs where it is. */
void mp_test_pet( npc &guy );
/** Host, CATA_MP_TEST_FACTIONS: makes two factions known to the group, so the factions window
 * has something to show. */
void mp_test_factions( npc &guy );
/** Host, CATA_MP_TEST_ACTIVITY=wait|meditate|pulp: starts that timed action on @p guy (a
 * client's body) and on the host's avatar once, then logs their progress every second. */
void mp_test_activity( npc &guy );
/** Client, CATA_MP_TEST_MAPMEM: logs how much map memory the avatar has around it. */
void mp_test_mapmem_client();

} // namespace net

#endif // CATA_SRC_MP_TEST_MENUS_H
