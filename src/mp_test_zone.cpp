#include "mp_test_menus.h"

#include <cstdlib>
#include <string>

#include "clzones.h"
#include "coordinates.h"
#include "debug.h"
#include "fluid.h"
#include "npc.h"
#include "point.h"
#include "type_id.h"

namespace net
{

// A zone of the group is created next to the body of a client, so that the client's zones window
// (key Y) has something to show: the client asks the host ("zones_req"), edits the copy and sends
// it back ("zones_set").  The report line shows the state the host holds, so an edit made in the
// window (disable with D, leave, answer yes) can be seen in the log.
void mp_test_zone( npc &guy )
{
    static const bool on = std::getenv( "CATA_MP_TEST_ZONE" ) != nullptr;
    if( !on ) {
        return;
    }
    static fluid::ms_t first_seen = 0;
    static fluid::ms_t last_report = 0;
    static bool created = false;
    const fluid::ms_t now = fluid::now_ms();
    if( first_seen == 0 ) {
        first_seen = now;
        return;
    }
    zone_manager &mgr = zone_manager::get_manager();
    if( !created && now >= first_seen + 8000 ) {
        created = true;
        const tripoint_abs_ms start = guy.pos_abs() + tripoint( 3, 0, 0 );
        const tripoint_abs_ms end = guy.pos_abs() + tripoint( 4, 1, 0 );
        mgr.add( "TESTZONE", zone_type_id( "LOOT_UNSORTED" ), your_fac, false, true, start, end, nullptr,
                 true );
        mgr.cache_data();
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test zone created at " << start.to_string() <<
                                   " (body at " << guy.pos_abs().to_string() << "), the host holds " <<
                                   mgr.size() << " zones";
        last_report = now;
        return;
    }
    if( created && now >= last_report + 5000 ) {
        last_report = now;
        std::string line = "Multiplayer: test zone report: the host holds " + std::to_string( mgr.size() ) +
                           " zones:";
        for( const zone_manager::ref_zone_data &ref : mgr.get_zones( your_fac ) ) {
            const zone_data &zone = ref.get();
            line += " [" + zone.get_name() + " enabled=" + std::to_string( zone.get_enabled() ) + " at " +
                    zone.get_start_point().to_string() + "]";
        }
        DebugLog( D_INFO, D_MAIN ) << line;
    }
}

} // namespace net
