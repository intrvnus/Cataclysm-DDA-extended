#include "mp_test_monster.h"

#include <cstdlib>
#include <map>
#include <string>

#include "avatar.h"
#include "bubbles.h"
#include "calendar.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "current_map.h"
#include "debug.h"
#include "fluid.h"
#include "game.h"
#include "line.h"
#include "map.h"
#include "mapdata.h"
#include "monster.h"
#include "npc.h"
#include "point.h"
#include "type_id.h"

namespace net
{

namespace
{
const efftype_id effect_immobilization_test( "immobilization" );

struct monster_state {
    fluid::ms_t first_seen = 0;
    bool done = false;
};
} // namespace

void test_monster( npc &guy )
{
    static const char *mode = std::getenv( "CATA_MP_TEST_MONSTER" );
    if( mode == nullptr ) {
        return;
    }
    static std::map<int, monster_state> states;
    monster_state &ms = states[guy.getID().get_value()];
    const fluid::ms_t now = fluid::now_ms();
    if( ms.done ) {
        if( calendar::once_every( 5_seconds ) ) {
            for( monster &z : g->all_monsters() ) {
                if( z.unique_name == "TESTMON" ) {
                    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test monster at " <<
                                               z.pos_abs().to_string() << " hp " << z.get_hp() <<
                                               " body at " << guy.pos_abs().to_string();
                }
            }
        }
        return;
    }
    if( ms.first_seen == 0 ) {
        ms.first_seen = now;
        return;
    }
    if( now - ms.first_seen < 30000 ) {
        return;
    }
    // with the quiet-field test the body must have been teleported far from the host's avatar
    if( std::getenv( "CATA_MP_TEST_BUBBLE" ) != nullptr &&
        rl_dist( guy.pos_abs(), get_avatar().pos_abs() ) < 100 ) {
        if( now - ms.first_seen > 150000 ) {
            ms.done = true;
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test monster failed: body never left the host's bubble";
        }
        return;
    }
    ms.done = true;

    map &bm = bubbles::map_for( guy );
    swap_map swap( bm );
    const tripoint_bub_ms p = guy.pos_bub( bm );
    const tripoint_bub_ms at = p + tripoint_rel_ms( 1, 1, 0 );
    // a clear tile for the target
    bm.furn_set( at, furn_str_id( "f_null" ) );
    bm.ter_set( at, ter_str_id( "t_dirt" ).id() );
    if( get_creature_tracker().creature_at( at ) != nullptr ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test monster failed: tile taken";
        return;
    }
    monster *z = g->place_critter_at( mtype_id( "mon_zombie" ), at );
    if( z == nullptr ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test monster failed: could not place the zombie";
        return;
    }
    z->unique_name = "TESTMON";
    // it stays put (no moves while immobilised) and survives a lot of punches
    z->add_effect( effect_immobilization_test, 600_minutes );
    z->set_hp( 100000 );
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test monster ready at " << z->pos_abs().to_string() <<
                               " body=" << guy.getID().get_value() << " body at " <<
                               guy.pos_abs().to_string();
}

} // namespace net
