#pragma once
#ifndef CATA_SRC_MP_TEST_CORNER_H
#define CATA_SRC_MP_TEST_CORNER_H

class npc;

namespace net
{
/**
 * Test hook (host, env CATA_MP_TEST_CORNER): builds a wall corner next to a player's body so that
 * a step north-east is blocked while the east tile is free (corner sliding must take the step
 * east).  Once per body, ~30 s after the first call; needs the quiet-field teleport
 * (CATA_MP_TEST_BUBBLE=quiet) to be done first.  Logs "Multiplayer: test corner ready ...".
 */
void test_corner( npc &guy );
} // namespace net

#endif // CATA_SRC_MP_TEST_CORNER_H
