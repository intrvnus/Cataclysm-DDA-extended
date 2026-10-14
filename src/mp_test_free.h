#pragma once
#ifndef CATA_SRC_MP_TEST_FREE_H
#define CATA_SRC_MP_TEST_FREE_H

class npc;

namespace net
{
/**
 * Test hook (host, env CATA_MP_TEST_FREE=door|attack|talk|squeeze|swim|ledge|vehicle): free
 * movement phase 2.  ~30 s after the first call (and after the quiet-field teleport when
 * CATA_MP_TEST_BUBBLE is set) builds a walled corridor east of a player's body with the case's
 * obstacle 3 tiles east (squeeze: a diagonal gap north-east) and logs
 * "Multiplayer: test free ready ...".  Afterwards it logs "Multiplayer: test free state ..."
 * whenever the body's offset from the start, the door, the target's hp or the vehicle state
 * changes, for 60 s.
 */
void test_free( npc &guy );
} // namespace net

#endif // CATA_SRC_MP_TEST_FREE_H
