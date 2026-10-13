// Experimental fluid movement and real time combat: the driver, the clocks and the animations.
// See fluid.h and doc/MULTIPLAYER_FLUID.md.

#include "fluid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#include "action.h"
#include "avatar.h"
#include "avatar_action.h"
#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "creature_tracker.h"
#include "dispersion.h"
#include "flag.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "game_constants.h"
#include "gun_mode.h"
#include "input.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "line.h"
#include "map.h"
#include "math_defines.h"
#include "messages.h"
#include "monster.h"
#include "net_session.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "player_activity.h"
#include "ranged.h"
#include "translations.h"
#include "ui_manager.h"
#include "units.h"

static const move_mode_id move_mode_run( "run" );
static const move_mode_id move_mode_walk( "walk" );

namespace fluid
{

// ---------------------------------------------------------------------------------------
// configuration and clocks

bool enabled()
{
#if defined(TILES)
    return get_option<bool>( "FLUID_MODE" );
#else
    return false;
#endif
}

bool authority()
{
    return !net::is_client();
}

ms_t now_ms()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>( clock::now() - origin ).count();
}

int turn_ms()
{
    return std::clamp( get_option<int>( "FLUID_TURN_MS" ), 100, 3000 );
}

bool smooth_camera()
{
    return enabled() && get_option<bool>( "FLUID_SMOOTH_CAMERA" );
}

const tuning &tune()
{
    static const tuning t;
    return t;
}

namespace
{

struct turn_clock {
    bool valid = false;
    // end of the turn that is running
    ms_t turn_end = 0;
};

turn_clock &clk()
{
    static turn_clock c;
    return c;
}

// ---- animations --------------------------------------------------------------------------

struct slide {
    // where the sprite is at t0, absolute tile coordinates (0.5 = tile centre)
    double from_x = 0.0;
    double from_y = 0.0;
    ms_t t0 = 0;
    int dur = 1;
};

struct lunge {
    int dx = 0;
    int dy = 0;
    ms_t t0 = 0;
    int windup = 0;
    int total = 0;
};

std::map<tripoint_abs_ms, slide> &slides()
{
    static std::map<tripoint_abs_ms, slide> m;
    return m;
}

std::map<tripoint_abs_ms, lunge> &lunges()
{
    static std::map<tripoint_abs_ms, lunge> m;
    return m;
}

aim_view &aim_view_rw()
{
    static aim_view v;
    return v;
}

melee_view &melee_view_rw()
{
    static melee_view v;
    return v;
}

// Client side prediction of the own step, drawn before the host confirmed it.
struct prediction {
    bool active = false;
    tripoint_abs_ms from;
    tripoint_abs_ms to;
    ms_t t0 = 0;
    int dur = 1;
};

prediction &pred()
{
    static prediction p;
    return p;
}

struct client_info {
    bool on = false;
    int my_id = 0;
    // end of the last confirmed step / attack of the own character
    ms_t confirmed_end = 0;
    ms_t busy_until = 0;
    int last_step_ms = 0;
    ms_t last_send = 0;
    bool aiming = false;
    double steadiness = 0.0;
    double spread_tiles = 0.0;
    bool can_fire = false;
};

client_info &cinfo()
{
    static client_info c;
    return c;
}

offset slide_offset_at( const tripoint_abs_ms &p, ms_t now )
{
    const auto iter = slides().find( p );
    if( iter == slides().end() ) {
        return offset();
    }
    const slide &s = iter->second;
    const double progress = std::clamp( static_cast<double>( now - s.t0 ) / std::max( 1, s.dur ),
                                        0.0, 1.0 );
    if( progress >= 1.0 ) {
        return offset();
    }
    // linear on purpose: chained steps then look like one constant speed movement
    offset o;
    o.x = ( s.from_x - ( p.x() + 0.5 ) ) * ( 1.0 - progress );
    o.y = ( s.from_y - ( p.y() + 0.5 ) ) * ( 1.0 - progress );
    return o;
}

// slides and the prediction of the own step: where the sprite is, relative to its tile
offset travel_offset_at( const tripoint_abs_ms &p, ms_t now )
{
    const prediction &pr = pred();
    if( pr.active && pr.from == p && now >= pr.t0 ) {
        const double progress = std::clamp( static_cast<double>( now - pr.t0 ) / std::max( 1, pr.dur ),
                                            0.0, 1.0 );
        offset o;
        o.x = ( pr.to.x() - pr.from.x() ) * progress;
        o.y = ( pr.to.y() - pr.from.y() ) * progress;
        return o;
    }
    return slide_offset_at( p, now );
}

double ease_out( double t )
{
    t = std::clamp( t, 0.0, 1.0 );
    return 1.0 - ( 1.0 - t ) * ( 1.0 - t );
}

offset lunge_offset_at( const tripoint_abs_ms &p, ms_t now )
{
    const auto iter = lunges().find( p );
    if( iter == lunges().end() ) {
        return offset();
    }
    const lunge &l = iter->second;
    const tuning &tn = tune();
    const double len = std::sqrt( static_cast<double>( l.dx * l.dx + l.dy * l.dy ) );
    if( len < 0.5 || now < l.t0 || now >= l.t0 + l.total ) {
        return offset();
    }
    const double ux = l.dx / len;
    const double uy = l.dy / len;
    const double t = static_cast<double>( now - l.t0 );
    double amount = 0.0;
    constexpr double strike_ms = 70.0;
    if( t < l.windup ) {
        // pull back
        amount = -tn.lunge_back * ease_out( t / std::max( 1, l.windup ) );
    } else if( t < l.windup + strike_ms ) {
        // the blow
        const double k = ( t - l.windup ) / strike_ms;
        amount = -tn.lunge_back + ( tn.lunge_forward + tn.lunge_back ) * ease_out( k );
    } else {
        const double rest = std::max( 1.0, l.total - l.windup - strike_ms );
        amount = tn.lunge_forward * ( 1.0 - ease_out( ( t - l.windup - strike_ms ) / rest ) );
    }
    offset o;
    o.x = ux * amount;
    o.y = uy * amount;
    return o;
}

void prune_animations( ms_t now )
{
    for( auto iter = slides().begin(); iter != slides().end(); ) {
        if( now >= iter->second.t0 + iter->second.dur ) {
            iter = slides().erase( iter );
        } else {
            ++iter;
        }
    }
    for( auto iter = lunges().begin(); iter != lunges().end(); ) {
        if( now >= iter->second.t0 + iter->second.total ) {
            iter = lunges().erase( iter );
        } else {
            ++iter;
        }
    }
}

// ---- helpers -----------------------------------------------------------------------------

int moves_to_ms( const Character &who, int moves )
{
    const int speed = std::max( 1, who.get_speed() );
    const int64_t ms = static_cast<int64_t>( std::max( 0, moves ) ) * turn_ms() / speed;
    return static_cast<int>( std::min<int64_t>( ms, 60000 ) );
}

double ms_to_moves( const Character &who, double ms )
{
    return ms * std::max( 1, who.get_speed() ) / turn_ms();
}

/**
 * Wraps a call into the native turn based code.  The call may charge moves (and in the
 * avatar's case be blocked by safe mode); here the cost is measured and refunded: time is kept
 * by the millisecond scheduler, not by the move ledger.
 */
class native_scope
{
    public:
        explicit native_scope( Character &who ) : who( who ), moves_before( who.get_moves() ) {
            if( who.is_avatar() && g->safe_mode == SAFE_MODE_STOP ) {
                // Safe mode stops a walking player; in real time there is nothing to stop.
                g->safe_mode = SAFE_MODE_ON;
                restore_safe_mode = true;
            }
        }
        // Moves the call charged. Refunds them.
        int finish() {
            if( finished ) {
                return charged;
            }
            finished = true;
            charged = moves_before - who.get_moves();
            who.set_moves( moves_before );
            if( restore_safe_mode && g->safe_mode == SAFE_MODE_ON ) {
                g->safe_mode = SAFE_MODE_STOP;
            }
            return std::max( 0, charged );
        }
        ~native_scope() {
            finish();
        }
    private:
        Character &who;
        int moves_before;
        int charged = 0;
        bool finished = false;
        bool restore_safe_mode = false;
};

// 8 direction from a vector, (0,0) if the vector is too short
std::pair<int, int> snap8( double dx, double dy )
{
    if( std::abs( dx ) < 1e-6 && std::abs( dy ) < 1e-6 ) {
        return { 0, 0 };
    }
    const double angle = std::atan2( dy, dx );
    const int octant = static_cast<int>( std::lround( angle / ( M_PI / 4.0 ) ) );
    static const int table_x[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int table_y[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    const int idx = ( ( octant % 8 ) + 8 ) % 8;
    return { table_x[idx], table_y[idx] };
}

tripoint_abs_ms tile_of( double x, double y, int z )
{
    return tripoint_abs_ms( static_cast<int>( std::floor( x ) ), static_cast<int>( std::floor( y ) ), z );
}

bool is_valid_target( const Character &who, const Creature &c )
{
    if( c.is_dead_state() || &c == &who ) {
        return false;
    }
    const Creature::Attitude att = who.attitude_to( c );
    if( c.is_monster() ) {
        return att != Creature::Attitude::FRIENDLY;
    }
    return att == Creature::Attitude::HOSTILE;
}

Creature *target_at( const Character &who, const tripoint_abs_ms &p )
{
    map &here = get_map();
    if( !here.inbounds( p ) ) {
        return nullptr;
    }
    Creature *c = get_creature_tracker().creature_at( here.get_bub( p ) );
    if( c != nullptr && is_valid_target( who, *c ) ) {
        return c;
    }
    return nullptr;
}

// A hostile creature next to @p who in the cone around (dx, dy); null if there is none.
Creature *assist_target( const Character &who, double dx, double dy, std::pair<int, int> &dir )
{
    const double len = std::sqrt( dx * dx + dy * dy );
    if( len < 1e-6 ) {
        return nullptr;
    }
    const tripoint_abs_ms pos = who.pos_abs();
    Creature *best = nullptr;
    double best_score = tune().assist_cos;
    for( int ox = -1; ox <= 1; ++ox ) {
        for( int oy = -1; oy <= 1; ++oy ) {
            if( ox == 0 && oy == 0 ) {
                continue;
            }
            Creature *c = target_at( who, pos + tripoint_rel_ms( ox, oy, 0 ) );
            if( c == nullptr ) {
                continue;
            }
            const double olen = std::sqrt( static_cast<double>( ox * ox + oy * oy ) );
            const double cosine = ( dx * ox + dy * oy ) / ( len * olen );
            if( cosine >= best_score ) {
                best_score = cosine;
                best = c;
                dir = { ox, oy };
            }
        }
    }
    return best;
}

void emit( std::vector<event> *out, const event &ev )
{
    if( out != nullptr ) {
        out->push_back( ev );
    }
}

// ---- aim ---------------------------------------------------------------------------------

item *wielded_gun( Character &who )
{
    item_location loc = who.get_wielded_item();
    if( !loc ) {
        return nullptr;
    }
    item *it = loc.get_item();
    if( it == nullptr || !it->is_gun() || it->is_gunmod() ) {
        return nullptr;
    }
    return it;
}

// Updates recoil, steadiness and the numbers the overlay needs.
void update_aim( Character &who, actor_state &st, const input_frame &in, ms_t now, bool idle )
{
    const double dt = static_cast<double>( std::clamp<ms_t>( now - st.aim_last, 0, 250 ) );
    st.aim_last = now;
    item *gun = wielded_gun( who );
    const tuning &tn = tune();
    map &here = get_map();

    st.holds_gun = gun != nullptr;
    if( gun == nullptr ) {
        st.aiming = false;
        st.aim_angle_valid = false;
        st.can_fire = false;
        return;
    }

    const tripoint_abs_ms pos = who.pos_abs();
    const bool have_cursor = in.cursor_valid && in.cursor_z == pos.z();
    const bool wanted = in.secondary && have_cursor;

    if( wanted ) {
        st.aiming = true;
        st.aim_x = in.cursor_x;
        st.aim_y = in.cursor_y;
        st.aim_tile = tile_of( in.cursor_x, in.cursor_y, pos.z() );

        // sweeping the aim across the screen throws the steadiness away
        const double cdx = in.cursor_x - ( pos.x() + 0.5 );
        const double cdy = in.cursor_y - ( pos.y() + 0.5 );
        if( std::abs( cdx ) + std::abs( cdy ) > 0.25 ) {
            const double angle = std::atan2( cdy, cdx );
            if( st.aim_angle_valid ) {
                double delta = std::abs( angle - st.aim_angle );
                if( delta > M_PI ) {
                    delta = 2.0 * M_PI - delta;
                }
                if( delta > 0.01 ) {
                    const double loss = std::clamp( delta * tn.aim_sweep_loss, 0.0, 1.0 );
                    who.recoil += ( MAX_RECOIL - who.recoil ) * loss;
                }
            }
            st.aim_angle = angle;
            st.aim_angle_valid = true;
        }

        const bool tile_ok = here.inbounds( st.aim_tile ) && st.aim_tile != pos;
        if( tile_ok ) {
            const tripoint_bub_ms target = here.get_bub( st.aim_tile );
            const double min_recoil = calculate_aim_cap( who, target );
            if( gun->has_flag( flag_id( "ALWAYS_AIMED" ) ) ) {
                who.recoil = 0.0;
            } else if( idle ) {
                // aim runs in whole native moves; time is turned into moves
                double moves = ms_to_moves( who, dt ) + st.aim_move_carry;
                int whole = static_cast<int>( std::floor( moves ) );
                st.aim_move_carry = moves - whole;
                whole = std::min( whole, tn.max_aim_moves_per_frame );
                for( int i = 0; i < whole && who.recoil > min_recoil; ++i ) {
                    const double step = who.aim_per_move( *gun, who.recoil );
                    if( step <= 0.0 ) {
                        break;
                    }
                    who.recoil = std::max( min_recoil, who.recoil - step );
                }
            }
            st.steadiness = std::clamp( calc_steadiness( who, *gun, target, who.recoil ), 0.0, 1.0 );
            const dispersion_sources disp = who.total_gun_dispersion( *gun, who.recoil_total(), 0 );
            const double range = std::sqrt( std::pow( in.cursor_x - ( pos.x() + 0.5 ), 2 ) +
                                            std::pow( in.cursor_y - ( pos.y() + 0.5 ), 2 ) );
            st.spread_tiles = std::tan( disp.max() / 60.0 * M_PI / 180.0 ) * range;
            st.can_fire = true;
        } else {
            st.can_fire = false;
        }
    } else {
        st.aiming = false;
        st.aim_angle_valid = false;
        st.aim_move_carry = 0.0;
        st.can_fire = have_cursor && here.inbounds( tile_of( in.cursor_x, in.cursor_y, pos.z() ) );
        // without the button held the aim falls apart
        if( who.recoil < MAX_RECOIL && tn.aim_relax_ms > 0 ) {
            who.recoil = std::min( MAX_RECOIL, who.recoil + dt * MAX_RECOIL / tn.aim_relax_ms );
        }
        st.steadiness = 0.0;
        if( have_cursor ) {
            st.aim_x = in.cursor_x;
            st.aim_y = in.cursor_y;
            st.aim_tile = tile_of( in.cursor_x, in.cursor_y, pos.z() );
            const double range = std::sqrt( std::pow( in.cursor_x - ( pos.x() + 0.5 ), 2 ) +
                                            std::pow( in.cursor_y - ( pos.y() + 0.5 ), 2 ) );
            const dispersion_sources disp = who.total_gun_dispersion( *gun, who.recoil_total(), 0 );
            st.spread_tiles = std::tan( disp.max() / 60.0 * M_PI / 180.0 ) * range;
        }
    }
}

// Fires the wielded gun at the aim tile. Returns the time the shot takes, 0 if nothing happened.
int fire_at_cursor( Character &who, actor_state &st, ms_t now, std::vector<event> *out )
{
    item *gun_item = wielded_gun( who );
    if( gun_item == nullptr ) {
        return 0;
    }
    map &here = get_map();
    const tripoint_abs_ms pos = who.pos_abs();
    if( !st.can_fire || !here.inbounds( st.aim_tile ) || st.aim_tile == pos ) {
        return 0;
    }
    gun_mode mode = gun_item->gun_current_mode();
    if( !mode || mode.melee() ) {
        return 0;
    }
    if( mode->has_flag( flag_RELOAD_AND_SHOOT ) ) {
        who.add_msg_if_player( m_info,
                               _( "This weapon has to be loaded for every shot: not available in fluid mode yet." ) );
        return 250;
    }
    if( who.is_avatar() ) {
        if( !avatar_action::can_fire_weapon( get_avatar(), here, *gun_item ) ) {
            return 250;
        }
    } else if( mode->ammo_remaining() <= 0 ) {
        return 250;
    }

    const tripoint_bub_ms target = here.get_bub( st.aim_tile );
    int charged = 0;
    {
        native_scope scope( who );
        who.fire_gun( here, target, mode.qty, *mode );
        charged = scope.finish();
    }
    const tuning &tn = tune();
    const int duration = std::max( tn.fire_min_ms, moves_to_ms( who, charged ) );

    event ev;
    ev.type = fx_type::shot;
    ev.actor = who.getID().get_value();
    ev.from = pos;
    ev.to = st.aim_tile;
    ev.dur_ms = duration;
    const auto dir = snap8( st.aim_x - ( pos.x() + 0.5 ), st.aim_y - ( pos.y() + 0.5 ) );
    ev.dx = dir.first;
    ev.dy = dir.second;
    emit( out, ev );
    ( void )now;
    return duration;
}

// ---- melee -------------------------------------------------------------------------------

void cancel_swing( Character &who, actor_state &st, ms_t now, std::vector<event> *out )
{
    const tuning &tn = tune();
    const ms_t remaining = std::max<ms_t>( 0, st.phase_end - now );
    st.melee = phase::idle;
    st.ready_at = now + static_cast<ms_t>( remaining * tn.cancel_cost );
    end_lunge( who.pos_abs() );
    event ev;
    ev.type = fx_type::cancel;
    ev.actor = who.getID().get_value();
    ev.from = who.pos_abs();
    emit( out, ev );
}

void start_swing( Character &who, actor_state &st, ms_t t0, int dx, int dy, std::vector<event> *out )
{
    const tuning &tn = tune();
    item_location weapon = who.used_weapon();
    int moves = 0;
    if( weapon ) {
        moves = who.attack_speed( *weapon );
    } else {
        moves = who.attack_speed( null_item_reference() );
    }
    const int total = std::max( tn.attack_min_ms, moves_to_ms( who, moves ) );
    const int windup = std::clamp( static_cast<int>( total * tn.windup_share ),
                                   std::min( tn.windup_min_ms, total / 2 ),
                                   std::min( tn.windup_max_ms, total - 40 ) );
    st.melee = phase::windup;
    st.melee_started = t0;
    st.melee_total_ms = total;
    st.melee_windup_ms = windup;
    st.phase_end = t0 + windup;
    st.ready_at = t0 + total;
    st.strike_dx = dx;
    st.strike_dy = dy;
    st.face_dx = dx;
    st.face_dy = dy;
    st.struck = false;
    st.hit_something = false;
    st.buffered_until = 0;
    begin_lunge( who.pos_abs(), dx, dy, t0, windup, total );

    event ev;
    ev.type = fx_type::swing;
    ev.actor = who.getID().get_value();
    ev.from = who.pos_abs();
    ev.dx = dx;
    ev.dy = dy;
    ev.dur_ms = total;
    ev.windup_ms = windup;
    emit( out, ev );
}

// The blow lands: whoever is there now is hit, the direction can still be steered a little.
void do_strike( Character &who, actor_state &st, const input_frame &in, ms_t now,
                std::vector<event> *out )
{
    const tripoint_abs_ms pos = who.pos_abs();
    std::pair<int, int> dir = { st.strike_dx, st.strike_dy };
    Creature *target = nullptr;

    // 1. the creature under the cursor direction, if it is next to us
    if( in.cursor_valid && in.cursor_z == pos.z() ) {
        const double cdx = in.cursor_x - ( pos.x() + 0.5 );
        const double cdy = in.cursor_y - ( pos.y() + 0.5 );
        const auto cursor_dir = snap8( cdx, cdy );
        if( cursor_dir.first != 0 || cursor_dir.second != 0 ) {
            target = target_at( who, pos + tripoint_rel_ms( cursor_dir.first, cursor_dir.second, 0 ) );
            if( target != nullptr ) {
                dir = cursor_dir;
            }
        }
    }
    // 2. whoever stands where the swing was aimed
    if( target == nullptr ) {
        target = target_at( who, pos + tripoint_rel_ms( st.strike_dx, st.strike_dy, 0 ) );
    }
    // 3. whoever is close to the aimed direction
    if( target == nullptr ) {
        target = assist_target( who, st.strike_dx, st.strike_dy, dir );
    }

    bool hit = false;
    if( target != nullptr ) {
        native_scope scope( who );
        hit = who.melee_attack( *target, true );
        scope.finish();
        st.face_dx = dir.first;
        st.face_dy = dir.second;
    }
    st.struck = true;
    st.hit_something = hit;

    const tuning &tn = tune();
    int recovery = std::max( 40, st.melee_total_ms - st.melee_windup_ms );
    if( target == nullptr || !hit ) {
        recovery = static_cast<int>( recovery * tn.whiff_recovery );
    }
    st.melee = phase::recovery;
    st.phase_end = now + recovery;
    st.ready_at = st.phase_end;

    event ev;
    ev.type = fx_type::strike;
    ev.actor = who.getID().get_value();
    ev.from = pos;
    ev.dx = dir.first;
    ev.dy = dir.second;
    ev.hit = hit;
    emit( out, ev );
}

// ---- steps -------------------------------------------------------------------------------

// Starts a step in direction (dx, dy). Returns true if time was spent.
bool start_step( Character &who, actor_state &st, int dx, int dy, ms_t now, ms_t t0,
                 std::vector<event> *out )
{
    map &here = get_map();
    const tuning &tn = tune();
    const tripoint_abs_ms from = who.pos_abs();
    const tripoint_abs_ms dest = from + tripoint_rel_ms( dx, dy, 0 );

    st.face_dx = dx;
    st.face_dy = dy;

    // walking into a downed player helps them up
    if( net::host_try_revive( who, dest ) ) {
        st.ready_at = t0 + moves_to_ms( who, 100 );
        return true;
    }
    // walking into an enemy attacks it
    if( target_at( who, dest ) != nullptr ) {
        start_swing( who, st, t0, dx, dy, out );
        return true;
    }

    const double recoil_before = who.recoil;
    int charged = 0;
    bool moved = false;
    {
        native_scope scope( who );
        if( who.is_avatar() ) {
            moved = avatar_action::move( get_avatar(), here, tripoint_rel_ms( dx, dy, 0 ) );
        } else if( npc *guy = dynamic_cast<npc *>( &who ) ) {
            guy->move_to( here.get_bub( dest ) );
            moved = true;
        }
        charged = scope.finish();
    }
    const tripoint_abs_ms now_pos = who.pos_abs();
    const bool changed = now_pos != from;

    if( changed ) {
        // a step keeps part of the steadiness unless the character runs
        const double loss = who.is_running() ? tn.aim_run_loss : tn.aim_step_loss;
        who.recoil = std::min( MAX_RECOIL, recoil_before + ( MAX_RECOIL - recoil_before ) * loss );
        const int dur = std::clamp( std::max( 40, moves_to_ms( who, charged ) ), 40, tn.slide_max_ms );
        st.ready_at = t0 + dur;
        st.aim_move_carry = 0.0;
        const int dist = std::max( std::abs( now_pos.x() - from.x() ), std::abs( now_pos.y() - from.y() ) );
        if( dist == 1 && now_pos.z() == from.z() ) {
            begin_slide( from, now_pos, t0, dur );
            event ev;
            ev.type = fx_type::step;
            ev.actor = who.getID().get_value();
            ev.from = from;
            ev.to = now_pos;
            ev.dur_ms = dur;
            ev.dx = dx;
            ev.dy = dy;
            emit( out, ev );
        }
        return true;
    }

    who.recoil = recoil_before;
    if( charged > 0 ) {
        // e.g. opening a door: it takes time but nobody moved
        st.ready_at = t0 + std::max( 40, moves_to_ms( who, charged ) );
        return true;
    }
    // blocked: do not retry (and spam the message log) every frame
    st.ready_at = now + 150;
    ( void )moved;
    return false;
}

bool can_act_now( const Character &who )
{
    return !who.is_dead_state() && who.get_moves() > 0 && !who.activity && !who.in_sleep_state();
}

} // namespace

// ---------------------------------------------------------------------------------------
// visuals API

void begin_slide( const tripoint_abs_ms &to, double from_x, double from_y, ms_t t0, int dur )
{
    slide s;
    s.from_x = from_x;
    s.from_y = from_y;
    s.t0 = t0;
    s.dur = std::clamp( dur, 1, tune().slide_max_ms );
    slides()[to] = s;
}

void begin_slide( const tripoint_abs_ms &from, const tripoint_abs_ms &to, ms_t t0, int dur )
{
    // a slide that is still running continues from where the sprite is now, so chained steps
    // never jump
    const auto iter = slides().find( from );
    double fx = from.x() + 0.5;
    double fy = from.y() + 0.5;
    if( iter != slides().end() ) {
        const offset o = slide_offset_at( from, t0 );
        fx += o.x;
        fy += o.y;
        slides().erase( iter );
    }
    begin_slide( to, fx, fy, t0, dur );
}

void begin_lunge( const tripoint_abs_ms &at, int dx, int dy, ms_t t0, int windup_ms, int total_ms )
{
    lunge l;
    l.dx = dx;
    l.dy = dy;
    l.t0 = t0;
    l.windup = std::max( 0, windup_ms );
    l.total = std::max( 1, total_ms );
    lunges()[at] = l;
}

void end_lunge( const tripoint_abs_ms &at )
{
    lunges().erase( at );
}

offset sprite_offset( const tripoint_abs_ms &p )
{
    if( slides().empty() && lunges().empty() && !pred().active ) {
        return offset();
    }
    const ms_t now = now_ms();
    offset a = travel_offset_at( p, now );
    const offset b = lunge_offset_at( p, now );
    a.x += b.x;
    a.y += b.y;
    return a;
}

offset camera_offset()
{
    if( !smooth_camera() || !tune().camera_follows_slide ) {
        return offset();
    }
    return travel_offset_at( get_avatar().pos_abs(), now_ms() );
}

bool animating()
{
    return !slides().empty() || !lunges().empty() || pred().active || aim_view_rw().active;
}

const aim_view &local_aim_view()
{
    return aim_view_rw();
}

const melee_view &local_melee_view()
{
    return melee_view_rw();
}

// ---------------------------------------------------------------------------------------
// states

actor_state &avatar_state()
{
    static actor_state s;
    return s;
}

namespace
{
std::map<int, actor_state> &remote_states()
{
    static std::map<int, actor_state> m;
    return m;
}
} // namespace

actor_state &state_of( int character_id )
{
    return remote_states()[character_id];
}

void forget_state( int character_id )
{
    remote_states().erase( character_id );
}

// ---------------------------------------------------------------------------------------
// the driver

void drive( Character &who, actor_state &st, const input_frame &in, ms_t now,
            std::vector<event> *out )
{
    if( who.is_dead_state() ) {
        return;
    }
    // B6: a sleeping remote player wakes up by moving
    if( !who.is_avatar() && who.in_sleep_state() &&
        ( in.move_x != 0 || in.move_y != 0 || in.primary || in.cancel ) ) {
        who.wake_up();
    }
    // B7: moving or the cancel key stops a timed action (crafting, eating ...) at once
    if( who.activity && who.activity.is_interruptible_with_kb() &&
        ( in.move_x != 0 || in.move_y != 0 || in.cancel ) ) {
        who.add_msg_if_player( m_alert, _( "You stop %s." ), who.activity.get_verb().translated() );
        who.cancel_activity();
    }
    const tuning &tn = tune();
    if( !st.initialised ) {
        st.initialised = true;
        st.seen_primary_presses = in.primary_presses;
        st.seen_secondary_presses = in.secondary_presses;
        st.prev_primary = in.primary;
        st.prev_secondary = in.secondary;
        st.aim_last = now;
    }
    const bool primary_pressed = in.primary_presses != st.seen_primary_presses ||
                                 ( in.primary && !st.prev_primary );
    const bool secondary_pressed = in.secondary_presses != st.seen_secondary_presses ||
                                   ( in.secondary && !st.prev_secondary );
    st.seen_primary_presses = in.primary_presses;
    st.seen_secondary_presses = in.secondary_presses;
    st.prev_primary = in.primary;
    st.prev_secondary = in.secondary;

    // ---- hold to sprint (B4); back to walking on release or when out of stamina
    if( in.sprint && !st.prev_sprint && who.can_run() ) {
        who.set_movement_mode( move_mode_run );
    } else if( who.is_running() && ( ( !in.sprint && st.prev_sprint ) || !who.can_run() ) ) {
        who.set_movement_mode( move_mode_walk );
    }
    st.prev_sprint = in.sprint;

    const tripoint_abs_ms pos = who.pos_abs();
    const bool have_cursor = in.cursor_valid && in.cursor_z == pos.z();
    const double cdx = have_cursor ? in.cursor_x - ( pos.x() + 0.5 ) : 0.0;
    const double cdy = have_cursor ? in.cursor_y - ( pos.y() + 0.5 ) : 0.0;
    const bool have_dir = have_cursor && std::abs( cdx ) + std::abs( cdy ) > 0.25;
    const bool acting = can_act_now( who );
    const bool armed_with_gun = wielded_gun( who ) != nullptr;

    // ---- melee phases
    if( st.melee == phase::windup ) {
        const bool cancel = in.cancel || ( secondary_pressed && !armed_with_gun );
        if( !acting ) {
            cancel_swing( who, st, now, out );
        } else if( cancel ) {
            cancel_swing( who, st, now, out );
        } else if( now >= st.phase_end ) {
            do_strike( who, st, in, now, out );
        }
    }
    if( st.melee == phase::recovery && now >= st.phase_end ) {
        st.melee = phase::idle;
    }
    const bool idle = acting && st.melee == phase::idle && now >= st.ready_at;
    if( !idle && primary_pressed ) {
        // too early (walking, recovering from a blow ...): the click is remembered and used
        // the moment the actor is ready
        st.buffered_until = now + tn.buffer_ms;
        st.buffered_is_attack = true;
    }

    // ---- aim and recoil (every frame, also when the character can not act)
    update_aim( who, st, in, now, idle );

    // ---- new actions
    if( idle ) {
        // chained actions start when the previous one ended, not when this frame happened to run
        const ms_t t0 = ( now - st.ready_at < 50 && st.ready_at > 0 ) ? st.ready_at : now;
        bool started = false;

        if( !armed_with_gun ) {
            const bool want_attack = primary_pressed || ( st.buffered_until > now && st.buffered_is_attack );
            const bool hold_attack = in.primary && !primary_pressed;
            if( want_attack || hold_attack ) {
                std::pair<int, int> dir = have_dir ? snap8( cdx, cdy ) : std::make_pair( st.face_dx, st.face_dy );
                if( dir.first == 0 && dir.second == 0 ) {
                    dir = { st.face_dx, st.face_dy };
                }
                std::pair<int, int> assisted = dir;
                Creature *tgt = target_at( who, pos + tripoint_rel_ms( dir.first, dir.second, 0 ) );
                if( tgt == nullptr ) {
                    tgt = assist_target( who, have_dir ? cdx : dir.first, have_dir ? cdy : dir.second,
                                         assisted );
                    if( tgt != nullptr ) {
                        dir = assisted;
                    }
                }
                // a held button only keeps swinging while there is something to hit;
                // a click always swings (the enemy may step in during the wind-up)
                if( want_attack || tgt != nullptr ) {
                    start_swing( who, st, t0, dir.first, dir.second, out );
                    started = true;
                }
            }
            st.buffered_until = 0;
        } else {
            const gun_mode mode = wielded_gun( who )->gun_current_mode();
            const bool automatic = mode && mode.qty > 1;
            const bool buffered = st.buffered_until > now && st.buffered_is_attack;
            st.buffered_until = 0;
            if( primary_pressed || buffered || ( in.primary && automatic ) ) {
                const int dur = fire_at_cursor( who, st, now, out );
                if( dur > 0 ) {
                    st.ready_at = t0 + dur;
                    started = true;
                } else if( primary_pressed ) {
                    st.ready_at = now + 120;
                }
            }
        }

        if( !started && ( in.move_x != 0 || in.move_y != 0 ) ) {
            started = start_step( who, st, in.move_x, in.move_y, now, t0, out );
        }
        ( void )started;
    }

    // ---- what the overlays show (local avatar only; set by host_frame)
}

// ---------------------------------------------------------------------------------------
// the turn clock

void reset_clock()
{
    clk().valid = false;
}

bool turn_due()
{
    if( !enabled() ) {
        return true;
    }
    turn_clock &c = clk();
    const ms_t now = now_ms();
    if( !c.valid ) {
        // e.g. a long activity was interrupted: the player gets a whole window to decide
        c.valid = true;
        c.turn_end = now + turn_ms();
        return false;
    }
    return now >= c.turn_end;
}

void begin_turn()
{
    if( !enabled() ) {
        return;
    }
    avatar &player = get_avatar();
    // Single player: long activities and sleep run as fast as the machine allows.  Not in
    // multiplayer: that would fast-forward the world for everybody (sleep: see B6).
    if( ( !net::active() && ( player.activity || player.in_sleep_state() ) ) ||
        net::everyone_asleep() ) {
        clk().valid = false;
        return;
    }
    const ms_t now = now_ms();
    turn_clock &c = clk();
    if( !c.valid || now > c.turn_end + turn_ms() ) {
        // first turn, or we were away (menu): do not run a burst of turns to catch up
        c.turn_end = now;
        c.valid = true;
    }
    c.turn_end += turn_ms();
}

namespace
{
// Pumps the screen and the network while the world waits for the end of the turn.
void idle_frame()
{
    host_frame();
    ui_manager::redraw_invalidated();
    std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
}
} // namespace

void finish_turn_window()
{
    if( !enabled() ) {
        return;
    }
    if( !clk().valid ) {
        // a long activity or sleep: the world runs as fast as the machine allows
        return;
    }
    // Guard: never stall the game for more than a few turns, whatever the clock says.
    const ms_t give_up = now_ms() + 4 * turn_ms();
    while( !turn_due() && now_ms() < give_up && g->uquit == QUIT_NO ) {
        idle_frame();
    }
}

// ---------------------------------------------------------------------------------------
// monsters and NPCs

namespace
{
std::map<const Creature *, tripoint_abs_ms> &before_positions()
{
    static std::map<const Creature *, tripoint_abs_ms> m;
    return m;
}
} // namespace

void snapshot_before_monsters()
{
    if( !enabled() ) {
        return;
    }
    auto &m = before_positions();
    m.clear();
    for( monster &critter : g->all_monsters() ) {
        m[&critter] = critter.pos_abs();
    }
    for( npc &guy : g->all_npcs() ) {
        m[&guy] = guy.pos_abs();
    }
}

void snapshot_after_monsters()
{
    if( !enabled() ) {
        return;
    }
    auto &m = before_positions();
    const ms_t now = now_ms();
    const int dur = std::min( turn_ms(), tune().slide_max_ms );
    std::vector<event> events;
    const auto handle = [&]( Creature & c ) {
        const auto iter = m.find( &c );
        if( iter == m.end() ) {
            return;
        }
        const tripoint_abs_ms from = iter->second;
        const tripoint_abs_ms to = c.pos_abs();
        if( from == to || from.z() != to.z() ) {
            return;
        }
        const int dist = std::max( std::abs( from.x() - to.x() ), std::abs( from.y() - to.y() ) );
        if( dist > 3 ) {
            return;
        }
        slides().erase( from );
        begin_slide( to, from.x() + 0.5, from.y() + 0.5, now, dur );
        event ev;
        ev.type = fx_type::step;
        ev.actor = 0;
        ev.from = from;
        ev.to = to;
        ev.dur_ms = dur;
        events.push_back( ev );
    };
    for( monster &critter : g->all_monsters() ) {
        handle( critter );
    }
    for( npc &guy : g->all_npcs() ) {
        handle( guy );
    }
    m.clear();
    if( !events.empty() ) {
        net::host_send_events( events );
    }
}

// ---------------------------------------------------------------------------------------
// the host frame

void host_frame()
{
    if( !enabled() ) {
        return;
    }
    static ms_t last_frame = -1000;
    const ms_t now = now_ms();
    if( now - last_frame < 4 ) {
        return;
    }
    last_frame = now;

    std::vector<event> events;
    avatar &u = get_avatar();

    input_frame in;
    const bool device = poll_device( in );
    if( device ) {
        // The classic input of vehicles, menus ... is left alone while they are in use.
        const bool fluid_owns_input = !u.controlling_vehicle && !u.is_dead_state() &&
                                      g->uquit != QUIT_WATCH;
        if( fluid_owns_input ) {
            drive( u, avatar_state(), in, now, &events );
        }
        // the overlays of the local avatar
        const actor_state &st = avatar_state();
        aim_view &av = aim_view_rw();
        av.active = st.holds_gun && st.can_fire && !u.controlling_vehicle;
        av.x = st.aim_x;
        av.y = st.aim_y;
        av.origin = u.pos_abs();
        av.spread_tiles = st.spread_tiles;
        av.steadiness = st.steadiness;
        av.can_fire = st.can_fire;
        melee_view &mv = melee_view_rw();
        mv.active = st.melee != phase::idle && !st.struck;
        if( mv.active ) {
            mv.tile = u.pos_abs() + tripoint_rel_ms( st.strike_dx, st.strike_dy, 0 );
            mv.phase = static_cast<int>( st.melee );
            mv.progress = std::clamp( static_cast<double>( now - st.melee_started ) /
                                      std::max( 1, st.melee_windup_ms ), 0.0, 1.0 );
        }
    }

    if( net::is_host() ) {
        net::host_drive_remote_players( now, &events );
    }
    if( !events.empty() ) {
        net::host_send_events( events );
    }
    net::poll();

    prune_animations( now );
    if( animating() ) {
        g->invalidate_main_ui_adaptor();
    }
}

// ---------------------------------------------------------------------------------------
// classic input that fluid mode owns

bool swallows( const std::string &action )
{
    if( !enabled() ) {
        return false;
    }
    avatar &u = get_avatar();
    if( u.controlling_vehicle || u.is_dead_state() || g->uquit == QUIT_WATCH ) {
        return false;
    }
    const action_id act = look_up_action( action );
    switch( act ) {
        case ACTION_MOVE_FORTH:
        case ACTION_MOVE_FORTH_RIGHT:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_BACK_RIGHT:
        case ACTION_MOVE_BACK:
        case ACTION_MOVE_BACK_LEFT:
        case ACTION_MOVE_LEFT:
        case ACTION_MOVE_FORTH_LEFT:
        case ACTION_SELECT:
        case ACTION_SEC_SELECT:
            note_classic_action( act );
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------
// network events

void apply_visual_event( const event &ev, ms_t arrival )
{
    switch( ev.type ) {
        case fx_type::step:
            begin_slide( ev.from, ev.to, arrival, ev.dur_ms );
            break;
        case fx_type::swing:
            begin_lunge( ev.from, ev.dx, ev.dy, arrival, ev.windup_ms, ev.dur_ms );
            break;
        case fx_type::cancel:
            end_lunge( ev.from );
            break;
        case fx_type::strike:
        case fx_type::shot:
        case fx_type::aim:
            break;
    }
}

void write_event( JsonOut &jo, const event &ev )
{
    jo.start_object();
    jo.member( "k", static_cast<int>( ev.type ) );
    jo.member( "a", ev.actor );
    jo.member( "f" );
    jo.start_array();
    jo.write( ev.from.x() );
    jo.write( ev.from.y() );
    jo.write( ev.from.z() );
    jo.end_array();
    jo.member( "o" );
    jo.start_array();
    jo.write( ev.to.x() );
    jo.write( ev.to.y() );
    jo.write( ev.to.z() );
    jo.end_array();
    jo.member( "d", ev.dur_ms );
    jo.member( "w", ev.windup_ms );
    jo.member( "x", ev.dx );
    jo.member( "y", ev.dy );
    jo.member( "h", ev.hit );
    jo.member( "s", ev.steadiness );
    jo.member( "r", ev.spread );
    jo.end_object();
}

event read_event( const JsonObject &jo )
{
    event ev;
    ev.type = static_cast<fx_type>( jo.get_int( "k", 0 ) );
    ev.actor = jo.get_int( "a", 0 );
    JsonArray f = jo.get_array( "f" );
    const int fx = f.next_int();
    const int fy = f.next_int();
    const int fz = f.next_int();
    ev.from = tripoint_abs_ms( fx, fy, fz );
    JsonArray o = jo.get_array( "o" );
    const int ox = o.next_int();
    const int oy = o.next_int();
    const int oz = o.next_int();
    ev.to = tripoint_abs_ms( ox, oy, oz );
    ev.dur_ms = jo.get_int( "d", 0 );
    ev.windup_ms = jo.get_int( "w", 0 );
    ev.dx = jo.get_int( "x", 0 );
    ev.dy = jo.get_int( "y", 0 );
    ev.hit = jo.get_bool( "h", false );
    ev.steadiness = jo.get_int( "s", 0 );
    ev.spread = jo.get_int( "r", 0 );
    return ev;
}


// ---------------------------------------------------------------------------------------
// the client

void client_set_mode( bool on, int my_character_id )
{
    client_info &ci = cinfo();
    ci = client_info();
    ci.on = on;
    ci.my_id = my_character_id;
    pred().active = false;
}

bool client_active()
{
    return cinfo().on && enabled() && net::is_client();
}

void client_aim_status( double steadiness, double spread_tiles, bool can_fire, bool aiming )
{
    client_info &ci = cinfo();
    ci.steadiness = steadiness;
    ci.spread_tiles = spread_tiles;
    ci.can_fire = can_fire;
    ci.aiming = aiming;
}

void client_event( const event &ev, ms_t arrival, bool mine )
{
    client_info &ci = cinfo();
    if( !mine ) {
        apply_visual_event( ev, arrival );
        return;
    }
    switch( ev.type ) {
        case fx_type::step: {
            prediction &pr = pred();
            ms_t t0 = arrival;
            if( pr.active && pr.from == ev.from && pr.to == ev.to ) {
                // the prediction was right: continue it, the sprite does not jump
                t0 = pr.t0;
            }
            pr.active = false;
            begin_slide( ev.from, ev.to, t0, ev.dur_ms );
            ci.last_step_ms = ev.dur_ms;
            ci.confirmed_end = t0 + ev.dur_ms;
            break;
        }
        case fx_type::swing:
            pred().active = false;
            ci.busy_until = arrival + ev.dur_ms;
            apply_visual_event( ev, arrival );
            break;
        case fx_type::cancel:
            ci.busy_until = arrival;
            apply_visual_event( ev, arrival );
            break;
        case fx_type::shot:
            ci.busy_until = arrival + ev.dur_ms;
            break;
        case fx_type::strike:
        case fx_type::aim:
            break;
    }
}

void client_frame()
{
    if( !client_active() ) {
        return;
    }
    client_info &ci = cinfo();
    const ms_t now = now_ms();
    avatar &u = get_avatar();
    map &here = get_map();

    input_frame in;
    if( !poll_device( in ) ) {
        return;
    }
    net::client_send_input( in );

    // ---- prediction of the own step
    prediction &pr = pred();
    if( pr.active && now >= pr.t0 + pr.dur + 250 ) {
        // never confirmed: the host did not let us go there
        pr.active = false;
    }
    const bool wants_move = in.move_x != 0 || in.move_y != 0;
    if( wants_move && !pr.active && now >= ci.confirmed_end && now >= ci.busy_until &&
        !u.controlling_vehicle && !in.primary && !in.secondary ) {
        const tripoint_abs_ms from = u.pos_abs();
        const tripoint_abs_ms to = from + tripoint_rel_ms( in.move_x, in.move_y, 0 );
        if( here.inbounds( to ) ) {
            const tripoint_bub_ms bub = here.get_bub( to );
            if( here.passable( bub ) && get_creature_tracker().creature_at( bub ) == nullptr ) {
                pr.active = true;
                pr.from = from;
                pr.to = to;
                pr.t0 = ( ci.confirmed_end > 0 && now - ci.confirmed_end < 50 ) ? ci.confirmed_end : now;
                pr.dur = ci.last_step_ms > 0 ? ci.last_step_ms : turn_ms();
            }
        }
    }
    // a prediction that is over while the avatar is still on its old tile stays at the
    // target until the confirmation arrives (or times out above)

    // ---- the overlays
    aim_view &av = aim_view_rw();
    av.active = ci.aiming && in.cursor_valid;
    av.x = in.cursor_x;
    av.y = in.cursor_y;
    av.origin = u.pos_abs();
    av.spread_tiles = ci.spread_tiles;
    av.steadiness = ci.steadiness;
    av.can_fire = ci.can_fire;

    prune_animations( now );
    if( animating() ) {
        g->invalidate_main_ui_adaptor();
    }
}

} // namespace fluid
