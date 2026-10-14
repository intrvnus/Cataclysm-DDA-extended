#include "mp_test_corner.h"

#include <cstdlib>
#include <map>
#include <string>

#include "avatar.h"
#include "bubbles.h"
#include "coordinates.h"
#include "current_map.h"
#include "debug.h"
#include "fluid.h"
#include "line.h"
#include "map.h"
#include "mapdata.h"
#include "npc.h"
#include "point.h"
#include "type_id.h"

namespace net
{

namespace
{
struct corner_state {
    fluid::ms_t first_seen = 0;
    bool done = false;
};
} // namespace

// The wall runs along the row north of the body (x-2 .. x+14), the row of the body itself is
// open dirt to the east (x+1 .. x+14): holding north+east then has to slide east along the wall
// again and again, and must never be reported as blocked.
void test_corner( npc &guy )
{
    static const char *mode = std::getenv( "CATA_MP_TEST_CORNER" );
    if( mode == nullptr ) {
        return;
    }
    static std::map<int, corner_state> states;
    corner_state &cs = states[guy.getID().get_value()];
    if( cs.done ) {
        return;
    }
    const fluid::ms_t now = fluid::now_ms();
    if( cs.first_seen == 0 ) {
        cs.first_seen = now;
        return;
    }
    if( now - cs.first_seen < 30000 ) {
        return;
    }
    // with the quiet-field test the body must have been teleported far from the host's avatar
    if( std::getenv( "CATA_MP_TEST_BUBBLE" ) != nullptr &&
        rl_dist( guy.pos_abs(), get_avatar().pos_abs() ) < 100 ) {
        if( now - cs.first_seen > 150000 ) {
            cs.done = true;
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test corner failed: body never left the host's bubble";
        }
        return;
    }
    cs.done = true;

    map &bm = bubbles::map_for( guy );
    swap_map swap( bm );
    const tripoint_bub_ms p = guy.pos_bub( bm );
    int built = 0;
    for( int dx = -2; dx <= 14; dx++ ) {
        const tripoint_bub_ms wall = p + tripoint_rel_ms( dx, -1, 0 );
        const tripoint_bub_ms open = p + tripoint_rel_ms( dx, 0, 0 );
        if( !bm.inbounds( wall ) || !bm.inbounds( open ) ) {
            continue;
        }
        bm.furn_set( wall, furn_str_id( "f_null" ) );
        if( bm.ter_set( wall, ter_str_id( "t_wall" ).id() ) ) {
            built++;
        }
        if( dx != 0 ) {
            bm.furn_set( open, furn_str_id( "f_null" ) );
            bm.ter_set( open, ter_str_id( "t_dirt" ).id() );
        }
    }
    bm.build_map_cache( p.z(), true );
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test corner ready body=" << guy.getID().get_value() <<
                               " at " << guy.pos_abs().to_string() << " walls=" << built <<
                               " n_pass=" << bm.passable( p + tripoint_rel_ms( 0, -1, 0 ) ) <<
                               " ne_pass=" << bm.passable( p + tripoint_rel_ms( 1, -1, 0 ) ) <<
                               " e_pass=" << bm.passable( p + tripoint_rel_ms( 1, 0, 0 ) );
}

} // namespace net
