#include "alerts.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>

#include "avatar.h"
#include "fluid.h"
#include "game.h"
#include "messages.h"
#include "net_session.h"
#include "options.h"
#include "path_info.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"

#if defined(SDL_SOUND)
#   if defined(_MSC_VER) && defined(USE_VCPKG)
#      include <SDL2/SDL_mixer.h>
#   else
#      include <SDL_mixer.h>
#   endif
#   include "sounds.h"
#endif

static const efftype_id effect_laserlocked( "laserlocked" );

namespace alerts
{
namespace
{

using clock_t_ = std::chrono::steady_clock;

struct alert_state {
    bool in_game = false;
    std::string pending_question;
    clock_t_::time_point question_time;
    std::map<std::string, clock_t_::time_point> last_text;
    std::map<level, clock_t_::time_point> last_sound;
    std::optional<tripoint_abs_ms> threat;
    clock_t_::time_point threat_time;
};

alert_state &state()
{
    static alert_state s;
    return s;
}

int ms_since( const clock_t_::time_point &t )
{
    return static_cast<int>( std::chrono::duration_cast<std::chrono::milliseconds>(
                                 clock_t_::now() - t ).count() );
}

// How long a "do it again to confirm" stays valid.
constexpr int confirm_window_ms = 4000;
// The same message is not repeated faster than this.
constexpr int repeat_text_ms = 2000;
// Hostiles within this many tiles count as close danger for the warning sound.
constexpr int danger_range = 12;

// Danger alerts always sound; warnings only when a threat is close; notices are a soft cue.
// Nothing sounds once the player is dead (death screen, "keep world?" questions).
bool wants_sound( level lvl )
{
    if( !state().in_game || get_avatar().is_dead_state() ) {
        return false;
    }
    switch( lvl ) {
        case level::notice:
        case level::danger:
            return true;
        case level::warning:
            return danger_nearby();
    }
    return false;
}

#if defined(SDL_SOUND)
const char *sound_file( level lvl )
{
    switch( lvl ) {
        case level::notice:
            return "notice.wav";
        case level::warning:
            return "warning.wav";
        case level::danger:
            return "danger.wav";
    }
    return "warning.wav";
}
#endif

} // namespace

namespace
{
bool &unattended_flag()
{
    static bool u = false;
    return u;
}
} // namespace

void set_unattended( bool unattended )
{
    unattended_flag() = unattended;
}

bool unattended()
{
    return unattended_flag();
}

bool nonblocking()
{
    if( !state().in_game ) {
        return false;
    }
    if( net::active() ) {
        return true;
    }
    // Solo realtime play: once the player is dead the world stops anyway, so the death screen
    // questions (death cam, keep world) are asked normally instead of "do it again to confirm".
    return fluid::enabled() && !get_avatar().is_dead_state();
}

bool danger_nearby()
{
    if( !state().in_game ) {
        return false;
    }
    avatar &u = get_avatar();
    if( u.is_dead_state() ) {
        return false;
    }
    return u.has_effect( effect_laserlocked ) ||
           !u.get_hostile_creatures( danger_range ).empty() ||
           g->is_in_dangerous_field() != nullptr;
}

void set_in_game( bool in_game )
{
    state().in_game = in_game;
    if( !in_game ) {
        state().pending_question.clear();
        state().threat.reset();
    }
}

void play_sound( level lvl )
{
    alert_state &s = state();
    const auto last = s.last_sound.find( lvl );
    if( last != s.last_sound.end() && ms_since( last->second ) < 700 ) {
        return;
    }
    s.last_sound[lvl] = clock_t_::now();
#if defined(SDL_SOUND)
    int frequency = 0;
    Uint16 format = 0;
    int channels = 0;
    if( !sounds::sound_enabled || Mix_QuerySpec( &frequency, &format, &channels ) == 0 ) {
        return;
    }
    static std::map<level, Mix_Chunk *> chunks;
    auto iter = chunks.find( lvl );
    if( iter == chunks.end() ) {
        const std::string path = PATH_INFO::datadir() + "sound/alerts/" + sound_file( lvl );
        iter = chunks.emplace( lvl, Mix_LoadWAV( path.c_str() ) ).first;
    }
    if( iter->second == nullptr ) {
        return;
    }
    const int volume = get_option<int>( "SOUND_EFFECT_VOLUME" );
    Mix_VolumeChunk( iter->second, MIX_MAX_VOLUME * volume / 100 );
    Mix_PlayChannel( -1, iter->second, 0 );
#endif
}

void warn( const std::string &text, level lvl, const std::optional<tripoint_abs_ms> &source )
{
    alert_state &s = state();
    if( source ) {
        s.threat = source;
        s.threat_time = clock_t_::now();
    }
    const auto last = s.last_text.find( text );
    if( last != s.last_text.end() && ms_since( last->second ) < repeat_text_ms ) {
        return;
    }
    s.last_text[text] = clock_t_::now();
    if( s.last_text.size() > 64 ) {
        s.last_text.clear();
    }
    add_msg( m_alert, text );
    if( wants_sound( lvl ) ) {
        play_sound( lvl );
    }
}

bool confirm( const std::string &question )
{
    alert_state &s = state();
    if( s.pending_question == question && ms_since( s.question_time ) < confirm_window_ms ) {
        s.pending_question.clear();
        return true;
    }
    s.pending_question = question;
    s.question_time = clock_t_::now();
    // A fresh question always shows, even if it was asked a moment ago and timed out.
    s.last_text.erase( question );
    warn( string_format( _( "%s  (Do it again to confirm.)" ), question ), level::warning );
    return false;
}

std::optional<tripoint_abs_ms> recent_threat( int max_age_ms )
{
    const alert_state &s = state();
    if( s.threat && ms_since( s.threat_time ) <= max_age_ms ) {
        return s.threat;
    }
    return std::nullopt;
}

} // namespace alerts
