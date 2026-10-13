#include "focus_minigame.h"

#include <map>
#include <string>

#include "alerts.h"
#include "character.h"
#include "fluid.h"
#include "net_session.h"
#include "player_activity.h"
#include "rng.h"
#include "translations.h"
#include "type_id.h"

static const activity_id ACT_CRAFT( "ACT_CRAFT" );
static const activity_id ACT_READ( "ACT_READ" );

namespace focus_minigame
{
namespace
{

// how long the player has to react, and how long the bonus lasts
constexpr fluid::ms_t window_ms = 1200;
constexpr fluid::ms_t boost_ms = 4000;

struct focus_state {
    fluid::ms_t next_prompt = 0;
    fluid::ms_t window_until = 0;
    fluid::ms_t boost_until = 0;
};

std::map<int, focus_state> &states()
{
    static std::map<int, focus_state> s;
    return s;
}

bool supported( const Character &who )
{
    return who.activity && ( who.activity.id() == ACT_READ || who.activity.id() == ACT_CRAFT );
}

} // namespace

void tick( Character &who )
{
    const int id = who.getID().get_value();
    // single player runs long actions as fast as the machine allows: nothing to speed up
    if( !net::active() || !supported( who ) ) {
        states().erase( id );
        return;
    }
    const fluid::ms_t now = fluid::now_ms();
    focus_state &st = states()[id];
    if( st.next_prompt == 0 ) {
        // the first prompt comes a little after the start
        st.next_prompt = now + rng( 3000, 5000 );
        return;
    }
    if( now >= st.next_prompt ) {
        st.window_until = now + window_ms;
        st.next_prompt = now + rng( 4000, 7000 );
        net::alert_player( who, _( "Focus!  Press SPACE now." ),
                           static_cast<int>( alerts::level::notice ), std::nullopt );
    }
}

void press( Character &who )
{
    const auto iter = states().find( who.getID().get_value() );
    if( iter == states().end() ) {
        return;
    }
    focus_state &st = iter->second;
    const fluid::ms_t now = fluid::now_ms();
    if( now <= st.window_until ) {
        st.window_until = 0;
        st.boost_until = now + boost_ms;
        net::alert_player( who, _( "Focused!  You work faster for a moment." ),
                           static_cast<int>( alerts::level::notice ), std::nullopt );
    }
}

double multiplier( const Character &who )
{
    const auto iter = states().find( who.getID().get_value() );
    if( iter == states().end() ) {
        return 1.0;
    }
    return fluid::now_ms() < iter->second.boost_until ? 2.0 : 1.0;
}

} // namespace focus_minigame
