#include "mp_test_menus.h"

#include <array>
#include <cstdlib>
#include <string>

#include "bubbles.h"
#include "calendar.h"
#include "creature_tracker.h"
#include "current_map.h"
#include "debug.h"
#include "effect.h"
#include "fluid.h"
#include "game.h"
#include "map.h"
#include "monster.h"
#include "npc.h"
#include "point.h"
#include "type_id.h"

namespace net
{

// A tame dog next to the body of a client: examining it (key E, then the direction) must open the
// pet menu on the client ("What to do with your ...?"), and what the player chooses there is done
// by the host.  CATA_MP_TEST_PET=tied also ties it up so that it stays where it is.
void mp_test_pet( npc &guy )
{
    static const char *mode = std::getenv( "CATA_MP_TEST_PET" );
    if( mode == nullptr ) {
        return;
    }
    static fluid::ms_t first_seen = 0;
    static fluid::ms_t last_report = 0;
    static bool spawned = false;
    const fluid::ms_t now = fluid::now_ms();
    if( first_seen == 0 ) {
        first_seen = now;
        return;
    }
    if( !spawned && now >= first_seen + 8000 ) {
        spawned = true;
        if( bubbles::bubble_of( guy ) != nullptr ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test pet: the body is in an extra bubble, skipped";
            return;
        }
        map &here = get_map();
        struct side {
            const char *name;
            tripoint_rel_ms delta;
        };
        const std::array<side, 4> sides = { {
                { "east", tripoint_rel_ms( 1, 0, 0 ) }, { "west", tripoint_rel_ms( -1, 0, 0 ) },
                { "south", tripoint_rel_ms( 0, 1, 0 ) }, { "north", tripoint_rel_ms( 0, -1, 0 ) }
            }
        };
        for( const side &s : sides ) {
            const tripoint_bub_ms p = guy.pos_bub( here ) + s.delta;
            if( !here.passable( p ) || here.veh_at( p ) ||
                get_creature_tracker().creature_at( p ) != nullptr ) {
                continue;
            }
            if( monster *dog = g->place_critter_at( mtype_id( "mon_dog" ), p ) ) {
                static const efftype_id effect_pet( "pet" );
                static const efftype_id effect_tied( "tied" );
                dog->friendly = -1;
                dog->add_effect( effect_pet, 1_turns, true );
                if( std::string( mode ) == "tied" ) {
                    dog->add_effect( effect_tied, 1_turns, true );
                }
                dog->unique_name = "TESTPET";
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test pet spawned " << s.name << " of the body at " <<
                                           dog->pos_abs().to_string() << " (body at " << guy.pos_abs().to_string() <<
                                           ") is_pet=" << dog->is_pet();
                last_report = now;
                return;
            }
        }
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test pet: no room next to the body";
        return;
    }
    if( spawned && now >= last_report + 5000 ) {
        last_report = now;
        for( monster &z : g->all_monsters() ) {
            if( z.unique_name == "TESTPET" ) {
                const tripoint_rel_ms rel = z.pos_abs() - guy.pos_abs();
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test pet report: the pet is at offset " << rel.x() <<
                                           "," << rel.y() << " from the body, is_pet=" << z.is_pet() << " hp " <<
                                           z.get_hp() << " body at " << guy.pos_abs().to_string();
                return;
            }
        }
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test pet report: the pet is gone";
    }
}

} // namespace net
