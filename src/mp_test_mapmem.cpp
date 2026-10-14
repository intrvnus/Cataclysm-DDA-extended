#include "mp_test_menus.h"

#include <cstdlib>
#include <string>

#include "avatar.h"
#include "coordinates.h"
#include "current_map.h"
#include "debug.h"
#include "fluid.h"
#include "line.h"
#include "map.h"
#include "map_memory.h"
#include "point.h"

namespace net
{

// Client side: how much of the area around the avatar is in its map memory, and how much of that is
// not in sight right now (so it can only come from earlier: a previous visit, loaded from the
// saved map memory).  The scripts compare a first visit with a second one.
void mp_test_mapmem_client()
{
    static const bool on = std::getenv( "CATA_MP_TEST_MAPMEM" ) != nullptr;
    if( !on ) {
        return;
    }
    static fluid::ms_t last = 0;
    static tripoint_abs_ms last_pos = tripoint_abs_ms::invalid;
    const fluid::ms_t now = fluid::now_ms();
    avatar &u = get_avatar();
    // a report every 4 s, and at once when the avatar is put somewhere else (the join)
    const bool jumped = last_pos != tripoint_abs_ms::invalid && rl_dist( u.pos_abs(), last_pos ) > 8;
    if( !jumped && now < last + 4000 ) {
        return;
    }
    last = now;
    last_pos = u.pos_abs();
    map &here = get_map();
    constexpr int radius = 25;
    int remembered = 0;
    int hidden = 0;
    for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub( here ), radius ) ) {
        const memorized_tile &mt = u.get_memorized_tile( here.get_abs( p ) );
        if( mt.get_ter_id().empty() ) {
            continue;
        }
        remembered++;
        if( !here.pl_sees( p, radius ) ) {
            hidden++;
        }
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test mapmem: " << ( jumped ? "avatar moved, " : "" ) <<
                               "at " << u.pos_abs().to_string() << " memory valid=" <<
                               u.is_map_memory_valid() << " remembered " << remembered <<
                               " tiles within " << radius << ", out of sight " << hidden;
}

} // namespace net
