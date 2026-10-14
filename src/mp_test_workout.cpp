// Multiplayer test scene "workout": the body has empty hands and is rested, so that the workout
// action of the client can start.  See mp_test_npc.h.

#include "mp_test_npc.h"

#include <string>

#include "character.h"
#include "debug.h"
#include "item.h"
#include "npc.h"
#include "player_activity.h"
#include "type_id.h"

namespace net
{

void mp_test_workout( npc &guy )
{
    static fluid::ms_t start = 0;
    static fluid::ms_t last_report = 0;
    static bool ready = false;

    if( !ready ) {
        if( !mp_test_due( start, 8000 ) ) {
            return;
        }
        // "Empty your hands first": workouts need free hands
        if( guy.is_armed() ) {
            guy.remove_weapon();
        }
        ready = true;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test workout ready weapon=" << mp_test_weapon( guy ) <<
                                   " armed=" << guy.is_armed() << " sleepiness=" << guy.get_sleepiness() <<
                                   " thirst=" << guy.get_thirst() <<
                                   " activity=" << ( guy.activity ? guy.activity.id().str() : std::string( "none" ) );
        return;
    }
    if( !mp_test_every( last_report, 3000 ) ) {
        return;
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test workout state: activity=" <<
                               ( guy.activity ? guy.activity.id().str() : std::string( "none" ) ) <<
                               " moves_left=" << ( guy.activity ? guy.activity.moves_left : 0 ) <<
                               " armed=" << guy.is_armed() << " stamina=" << guy.get_stamina();
}

} // namespace net
