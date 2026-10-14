#pragma once
#ifndef CATA_SRC_MP_TEST_MONSTER_H
#define CATA_SRC_MP_TEST_MONSTER_H

class npc;

namespace net
{
/**
 * Test hook (host, env CATA_MP_TEST_MONSTER): puts one immobilised, very tough zombie on the
 * south-east tile of a player's body (once per body, ~30 s after the first call, after the
 * quiet-field teleport of CATA_MP_TEST_BUBBLE=quiet).  Logs "Multiplayer: test monster ready at
 * <abs pos>" and every 5 s where the monster is.
 */
void test_monster( npc &guy );
} // namespace net

#endif // CATA_SRC_MP_TEST_MONSTER_H
