// Multiplayer test scene "console": a computer console next to the body; the player examines it
// (game::use_computer for a player's body).  See mp_test_npc.h.

#include "mp_test_npc.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "character.h"
#include "computer.h"
#include "creature_tracker.h"
#include "debug.h"
#include "line.h"
#include "map.h"
#include "mapdata.h"
#include "npc.h"
#include "type_id.h"

namespace net
{

void mp_test_console( npc &guy )
{
    static fluid::ms_t start = 0;
    static fluid::ms_t last_report = 0;
    static std::optional<tripoint_abs_ms> console_at;

    map &here = get_map();
    if( !console_at ) {
        if( !mp_test_due( start, 10000 ) ) {
            return;
        }
        const tripoint_bub_ms at = guy.pos_bub( here );
        static const tripoint_rel_ms order[] = {
            tripoint_rel_ms( 1, 0, 0 ), tripoint_rel_ms( 0, -1, 0 ), tripoint_rel_ms( -1, 0, 0 ),
            tripoint_rel_ms( 0, 1, 0 ), tripoint_rel_ms( 1, -1, 0 ), tripoint_rel_ms( -1, -1, 0 ),
            tripoint_rel_ms( 1, 1, 0 ), tripoint_rel_ms( -1, 1, 0 )
        };
        for( const tripoint_rel_ms &d : order ) {
            const tripoint_bub_ms p = at + d;
            if( !here.passable( p ) || here.veh_at( p ) || here.has_furn( p ) ||
                get_creature_tracker().creature_at( p ) != nullptr ) {
                continue;
            }
            // CATA_MP_TEST_CONSOLE_SEC=<n>: the computer wants a hack (default: open)
            const char *sec = std::getenv( "CATA_MP_TEST_CONSOLE_SEC" );
            computer *comp = here.add_computer( p, "TEST TERMINAL", sec ? std::atoi( sec ) : 0 );
            if( comp == nullptr ) {
                continue;
            }
            comp->add_option( "Emergency messages", COMPACT_EMERG_MESS, 0 );
            comp->add_option( "Tower status", COMPACT_TOWER_UNRESPONSIVE, 0 );
            console_at = here.get_abs( p );
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test console ready at " <<
                                       console_at->to_string() << " dir=" << mp_test_dir_key( at, p ) <<
                                       " computer=" << ( here.computer_at( p ) != nullptr ) <<
                                       " security=" << ( sec ? std::atoi( sec ) : 0 ) <<
                                       " body at " << guy.pos_abs().to_string();
            return;
        }
        if( mp_test_every( last_report, 5000 ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test console failed: no free tile next to the body";
        }
        return;
    }
    if( !mp_test_every( last_report, 4000 ) ) {
        return;
    }
    const tripoint_bub_ms p = here.get_bub( *console_at );
    computer *comp = here.computer_at( p );
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test console state: computer=" << ( comp != nullptr ) <<
                               " security=" << ( comp ? comp->security : -1 ) <<
                               " alerts=" << ( comp ? comp->alerts : -1 ) <<
                               " console furniture=" << ( here.furn( p ) == furn_str_id( "f_console" ) ) <<
                               " body next to it=" << ( rl_dist( guy.pos_bub( here ), p ) <= 1 );
}

} // namespace net
