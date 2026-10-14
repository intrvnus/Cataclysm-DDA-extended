// Multiplayer test scene "recraft": the body has crafted something before (its "last recipe") and
// carries the materials, so that the client's "repeat last craft" key has something to repeat.
// See mp_test_npc.h.

#include "mp_test_npc.h"

#include <string>

#include "character.h"
#include "debug.h"
#include "item.h"
#include "npc.h"
#include "player_activity.h"
#include "recipe.h"
#include "type_id.h"

namespace net
{

void mp_test_recraft( npc &guy )
{
    static fluid::ms_t start = 0;
    static fluid::ms_t last_report = 0;
    static bool ready = false;
    static const recipe_id recipe( "rolling_paper" );
    static const itype_id material( "paper" );
    static const itype_id product( "rolling_paper" );

    if( !ready ) {
        if( !mp_test_due( start, 8000 ) ) {
            return;
        }
        if( !recipe.is_valid() ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test recraft failed: no recipe rolling_paper";
            ready = true;
            return;
        }
        // 20 sheets: a craft of one batch uses one
        for( int i = 0; i < 20; i++ ) {
            guy.i_add( item( material ) );
        }
        guy.learn_recipe( &recipe.obj() );
        guy.lastrecipe = recipe;
        guy.last_batch = 1;
        ready = true;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test recraft ready lastrecipe=" << guy.lastrecipe.str() <<
                                   " knows=" << guy.knows_recipe( &recipe.obj() ) <<
                                   " paper=" << mp_test_count( guy, material ) <<
                                   " rolling_paper=" << mp_test_count( guy, product ) <<
                                   " activity=" << ( guy.activity ? guy.activity.id().str() : std::string( "none" ) );
        return;
    }
    if( !mp_test_every( last_report, 3000 ) ) {
        return;
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test recraft state: activity=" <<
                               ( guy.activity ? guy.activity.id().str() : std::string( "none" ) ) <<
                               " lastrecipe=" << guy.lastrecipe.str() <<
                               " paper=" << mp_test_count( guy, material ) <<
                               " rolling_paper=" << mp_test_count( guy, product );
}

} // namespace net
