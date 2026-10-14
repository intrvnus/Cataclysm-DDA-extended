#include "mp_test_menus.h"

#include <cstdlib>
#include <string>

#include "debug.h"
#include "faction.h"
#include "fluid.h"
#include "game.h"
#include "memory_fast.h"
#include "npc.h"
#include "type_id.h"

namespace net
{

// The factions window of a client (key #) shows the factions the group knows.  A fresh game knows
// none, so two are made known here; the host logs the answer it sends ("factions sent").
void mp_test_factions( npc & )
{
    static const bool on = std::getenv( "CATA_MP_TEST_FACTIONS" ) != nullptr;
    if( !on ) {
        return;
    }
    static fluid::ms_t first_seen = 0;
    static bool done = false;
    const fluid::ms_t now = fluid::now_ms();
    if( first_seen == 0 ) {
        first_seen = now;
        return;
    }
    if( done || now < first_seen + 5000 ) {
        return;
    }
    done = true;
    int marked = 0;
    for( const char *id : { "free_merchants", "isherwood_family" } ) {
        if( faction *fac = g->faction_manager_ptr->get( faction_id( id ), false ) ) {
            fac->known_by_u = true;
            marked++;
        }
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test factions: " << marked <<
                               " factions made known to the group (of " <<
                               g->faction_manager_ptr->all().size() << " factions)";
}

} // namespace net
