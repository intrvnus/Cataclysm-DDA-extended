// Experimental fluid movement and real time combat: the driver, the clocks and the animations.
// See fluid.h and doc/MULTIPLAYER_FLUID.md.

#include "fluid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "action.h"
#include "avatar.h"
#include "alerts.h"
#include "avatar_action.h"
#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "creature_tracker.h"
#include "dispersion.h"
#include "flag.h"
#include "flexbuffer_json.h"
#include "focus_minigame.h"
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
#include "mp_groups.h"
#include "mtype.h"
#include "net_session.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmap_ui.h"
#include "player_activity.h"
#include "projectile.h"
#include "ranged.h"
#include "translations.h"
#include "ui_manager.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "units.h"

static const efftype_id effect_incorporeal( "incorporeal" );
static const efftype_id effect_stunned_fluid( "stunned" );
static const json_character_flag json_flag_CANNOT_ATTACK( "CANNOT_ATTACK" );
static const move_mode_id move_mode_run( "run" );
static const move_mode_id move_mode_walk( "walk" );

namespace fluid
{

// ---------------------------------------------------------------------------------------
// configuration and clocks

bool enabled()
{
#if defined(TILES)
    // multiplayer and fluid mode are one thing: every multiplayer session is fluid
    return net::active() || get_option<bool>( "FLUID_MODE" );
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

bool free_movement()
{
    return enabled() && get_option<bool>( "FLUID_FREE_MOVEMENT" );
}

namespace
{
bool soft_wall_push = false;
} // namespace

bool soft_wall_confirmed()
{
    return soft_wall_push;
}

const tuning &tune()
{
    // compile-time values, except the knobs that are options (read at most once a millisecond)
    static tuning t;
    static ms_t refreshed = -1;
    const ms_t now = now_ms();
    if( now != refreshed && has_option( "FLUID_SOFT_WALL_MS" ) && has_option( "FLUID_BODY_WIDTH" ) ) {
        refreshed = now;
        t.soft_wall_ms = get_option<int>( "FLUID_SOFT_WALL_MS" );
        t.body_half = get_option<int>( "FLUID_BODY_WIDTH" ) / 200.0;
    }
    return t;
}

// a monster loses its pending blow and may not wind up a new one for @p ms (shove, a strong hit)
void stagger_monster( monster &z, int ms, ms_t now );
// the monster is winding up a melee blow right now
bool monster_winding_up( const monster &z );

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
    // ease-out instead of linear: used by the ease-back of a rejected step
    bool ease = false;
};

struct lunge {
    int dx = 0;
    int dy = 0;
    ms_t t0 = 0;
    int windup = 0;
    int total = 0;
    // hit-stop: the lunge holds still for stop_ms from stop_at (0 = none)
    ms_t stop_at = 0;
    int stop_ms = 0;
};

// a creature that was hit: it flashes, shakes during the hit-stop and is knocked back a little
// (drawn only), from the direction (dx, dy)
struct hit_fx {
    int dx = 0;
    int dy = 0;
    ms_t t0 = 0;
};
constexpr int hit_stop_ms = 60;
constexpr int hit_recoil_ms = 180;
constexpr int hit_flash_ms = 120;
constexpr double hit_recoil_tiles = 0.22;

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

std::map<tripoint_abs_ms, hit_fx> &hits()
{
    static std::map<tripoint_abs_ms, hit_fx> m;
    return m;
}

struct tracer_fx {
    tripoint_abs_ms from;
    tripoint_abs_ms to;
    ms_t t0 = 0;
};
constexpr int tracer_ms = 160;

struct boom_fx {
    tripoint_abs_ms at;
    int radius = 0;
    ms_t t0 = 0;
};
constexpr int boom_ms = 350;

std::vector<boom_fx> &boom_list()
{
    static std::vector<boom_fx> v;
    return v;
}

struct flourish_fx {
    flourish_kind kind = flourish_kind::dash;
    double x = 0.0;
    double y = 0.0;
    int z = 0;
    double dx = 0.0;
    double dy = 0.0;
    tripoint_abs_ms on;
    ms_t t0 = 0;
};

int flourish_ms( flourish_kind k )
{
    switch( k ) {
        case flourish_kind::dash:
            return 260;
        case flourish_kind::evade:
            return 320;
        case flourish_kind::stagger:
            return 420;
        case flourish_kind::winded:
            return 380;
    }
    return 300;
}

std::vector<flourish_fx> &flourish_list()
{
    static std::vector<flourish_fx> v;
    return v;
}

void add_flourish( flourish_kind kind, double x, double y, int z, double dx, double dy,
                   const tripoint_abs_ms &on, ms_t t0 )
{
    flourish_fx f;
    f.kind = kind;
    f.x = x;
    f.y = y;
    f.z = z;
    f.dx = dx;
    f.dy = dy;
    f.on = on;
    f.t0 = t0;
    flourish_list().push_back( f );
}

// the same as an event for the clients (positions in 1/1000 tile)
event flourish_event( flourish_kind kind, double x, double y, int z, double dx, double dy,
                      const tripoint_abs_ms &on, int actor )
{
    event ev;
    ev.type = fx_type::flourish;
    ev.actor = actor;
    ev.windup_ms = static_cast<int>( kind );
    const int tx = static_cast<int>( std::floor( x ) );
    const int ty = static_cast<int>( std::floor( y ) );
    ev.from = tripoint_abs_ms( tx, ty, z );
    ev.steadiness = static_cast<int>( std::lround( ( x - tx ) * 1000.0 ) );
    ev.spread = static_cast<int>( std::lround( ( y - ty ) * 1000.0 ) );
    ev.dx = static_cast<int>( std::lround( dx * 1000.0 ) );
    ev.dy = static_cast<int>( std::lround( dy * 1000.0 ) );
    ev.to = on;
    return ev;
}

struct aim_line_fx {
    tripoint_abs_ms from;
    tripoint_abs_ms to;
    ms_t t0 = 0;
    int dur = 1;
};

std::vector<aim_line_fx> &aim_line_list()
{
    static std::vector<aim_line_fx> v;
    return v;
}


std::vector<tracer_fx> &tracer_list()
{
    static std::vector<tracer_fx> v;
    return v;
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

// ---- camera lead -------------------------------------------------------------------------

// The cursor of the last input frame of the local player, and the low-passed lead built from it.
struct camera_lead_state {
    bool cursor_valid = false;
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    int cursor_z = 0;
    ms_t cursor_stamp = 0;
    // the lead now, in tiles, and when it was updated
    double x = 0.0;
    double y = 0.0;
    ms_t updated = 0;
};

camera_lead_state &lead_state()
{
    static camera_lead_state c;
    return c;
}

void note_cursor( const input_frame &in, ms_t now )
{
    camera_lead_state &cs = lead_state();
    cs.cursor_valid = in.cursor_valid;
    cs.cursor_x = in.cursor_x;
    cs.cursor_y = in.cursor_y;
    cs.cursor_z = in.cursor_z;
    cs.cursor_stamp = now;
    // the cursor turns into the red crosshair over a hostile creature the player can see
    bool hostile = false;
    if( in.cursor_valid ) {
        map &here = get_map();
        const avatar &u = get_avatar();
        const tripoint_abs_ms tile( static_cast<int>( std::floor( in.cursor_x ) ), static_cast<int>( std::floor( in.cursor_y ) ), in.cursor_z );
        if( here.inbounds( tile ) ) {
            const Creature *c = get_creature_tracker().creature_at( here.get_bub( tile ) );
            hostile = c != nullptr && c != &u && u.sees( here, *c ) &&
                      u.attitude_to( *c ) == Creature::Attitude::HOSTILE;
        }
    }
    set_hostile_cursor( hostile );
}

// Where the camera wants to be, in tiles from the avatar: toward the cursor, by an amount that
// grows with the distance of the cursor (so that the cursor, which is read through the shifted
// camera, cannot run away from it) up to camera_lead_tiles.
offset camera_lead_target( ms_t now )
{
    const camera_lead_state &cs = lead_state();
    const tuning &tn = tune();
    constexpr double full_lead_distance = 4.0;
    // the frames are not polled while a menu is open: an old cursor is not a cursor
    constexpr ms_t stale_ms = 150;
    const avatar &u = get_avatar();
    offset o;
    if( tn.camera_lead_tiles > 0.0 && get_option<bool>( "FLUID_CAMERA_LEAD" ) && cs.cursor_valid &&
        now - cs.cursor_stamp <= stale_ms && cs.cursor_z == u.posz() && !u.is_dead_state() ) {
        const tripoint_abs_ms at = u.pos_abs();
        const double vx = cs.cursor_x - ( at.x() + 0.5 );
        const double vy = cs.cursor_y - ( at.y() + 0.5 );
        const double dist = std::sqrt( vx * vx + vy * vy );
        if( dist > 1e-6 ) {
            const double mag = tn.camera_lead_tiles * std::min( 1.0, dist / full_lead_distance );
            o.x = vx / dist * mag;
            o.y = vy / dist * mag;
        }
    }
    return o;
}

// The lead now: it follows the target by an exponential approach.
offset camera_lead_at( ms_t now )
{
    camera_lead_state &cs = lead_state();
    constexpr double approach_ms = 75.0;
    const offset target = camera_lead_target( now );
    const double dt = static_cast<double>( std::clamp<ms_t>( now - cs.updated, 0, 100 ) );
    cs.updated = now;
    const double k = 1.0 - std::exp( -dt / approach_ms );
    cs.x += ( target.x - cs.x ) * k;
    cs.y += ( target.y - cs.y ) * k;
    offset o;
    o.x = cs.x;
    o.y = cs.y;
    return o;
}

double ease_out( double t )
{
    t = std::clamp( t, 0.0, 1.0 );
    return 1.0 - ( 1.0 - t ) * ( 1.0 - t );
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
    // linear on purpose: chained steps then look like one constant speed movement; only the
    // ease-back of a rejected step is eased
    const double left = 1.0 - ( s.ease ? ease_out( progress ) : progress );
    offset o;
    o.x = ( s.from_x - ( p.x() + 0.5 ) ) * left;
    o.y = ( s.from_y - ( p.y() + 0.5 ) ) * left;
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

// The own step was not allowed by the host: the sprite, drawn at (from_x, from_y) (absolute tile
// coordinates), eases back to the tile it really stands on instead of snapping.
void ease_back( const tripoint_abs_ms &real, double from_x, double from_y, ms_t now )
{
    const double ex = from_x - ( real.x() + 0.5 );
    const double ey = from_y - ( real.y() + 0.5 );
    const double d2 = ex * ex + ey * ey;
    // nothing to ease, or too far for an animation to make sense
    if( tune().ease_back_ms <= 0 || d2 < 0.0004 || d2 > 9.0 ) {
        return;
    }
    slide s;
    s.from_x = from_x;
    s.from_y = from_y;
    s.t0 = now;
    s.dur = std::clamp( tune().ease_back_ms, 1, tune().slide_max_ms );
    s.ease = true;
    slides()[real] = s;
}

// true while an ease-back is running at @p
bool easing_back_at( const tripoint_abs_ms &p, ms_t now )
{
    const auto iter = slides().find( p );
    return iter != slides().end() && iter->second.ease && now < iter->second.t0 + iter->second.dur;
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
    double t = static_cast<double>( now - l.t0 );
    if( l.stop_ms > 0 && now >= l.stop_at ) {
        // hit-stop: hold the pose of the moment the blow connected
        t = now < l.stop_at + l.stop_ms ? static_cast<double>( l.stop_at - l.t0 ) : t - l.stop_ms;
    }
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

offset hit_offset_at( const tripoint_abs_ms &p, ms_t now )
{
    const auto iter = hits().find( p );
    if( iter == hits().end() || now < iter->second.t0 ) {
        return offset();
    }
    const hit_fx &h = iter->second;
    const double len = std::hypot( h.dx, h.dy );
    if( len < 0.5 ) {
        return offset();
    }
    const double ux = h.dx / len;
    const double uy = h.dy / len;
    const double t = static_cast<double>( now - h.t0 );
    double amount = 0.0;
    double shake = 0.0;
    if( t < hit_stop_ms ) {
        amount = hit_recoil_tiles * ease_out( t / hit_stop_ms );
        // a short shake across the direction of the blow
        shake = ( static_cast<int>( t / 20 ) % 2 == 0 ? 0.05 : -0.05 );
    } else if( t < hit_stop_ms + hit_recoil_ms ) {
        amount = hit_recoil_tiles * ( 1.0 - ease_out( ( t - hit_stop_ms ) / hit_recoil_ms ) );
    }
    offset o;
    o.x = ux * amount - uy * shake;
    o.y = uy * amount + ux * shake;
    return o;
}

void prune_animations( ms_t now )
{
    std::vector<aim_line_fx> &al = aim_line_list();
    al.erase( std::remove_if( al.begin(), al.end(), [now]( const aim_line_fx & a ) {
        return now >= a.t0 + a.dur;
    } ), al.end() );
    std::vector<boom_fx> &bl = boom_list();
    bl.erase( std::remove_if( bl.begin(), bl.end(), [now]( const boom_fx & b ) {
        return now >= b.t0 + boom_ms;
    } ), bl.end() );
    std::vector<flourish_fx> &fl = flourish_list();
    fl.erase( std::remove_if( fl.begin(), fl.end(), [now]( const flourish_fx & f ) {
        return now >= f.t0 + flourish_ms( f.kind );
    } ), fl.end() );
    std::vector<tracer_fx> &tl = tracer_list();
    tl.erase( std::remove_if( tl.begin(), tl.end(), [now]( const tracer_fx & t ) {
        return now >= t.t0 + tracer_ms;
    } ), tl.end() );
    for( auto iter = hits().begin(); iter != hits().end(); ) {
        if( now >= iter->second.t0 + hit_stop_ms + hit_recoil_ms ) {
            iter = hits().erase( iter );
        } else {
            ++iter;
        }
    }
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

// compass direction of 45 degree sector @p octant (0 = east, counted toward +y = south)
std::pair<int, int> compass_dir( int octant )
{
    static const int table_x[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int table_y[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    const int idx = ( ( octant % 8 ) + 8 ) % 8;
    return { table_x[idx], table_y[idx] };
}

// The compass direction whose 45 degree sector (centred on it, e.g. east = -22.5..+22.5 degrees,
// south-east = 22.5..67.5, y grows downward) holds the vector (dx, dy); (0,0) for a null vector.
std::pair<int, int> sector_dir( double dx, double dy )
{
    if( std::abs( dx ) < 1e-6 && std::abs( dy ) < 1e-6 ) {
        return { 0, 0 };
    }
    const double angle = std::atan2( dy, dx );
    return compass_dir( static_cast<int>( std::lround( angle / ( M_PI / 4.0 ) ) ) );
}

// 8 direction from a vector, (0,0) if the vector is too short
std::pair<int, int> snap8( double dx, double dy )
{
    return sector_dir( dx, dy );
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
    if( mp_groups::pvp_target( who, c ) || mp_groups::sparring( who, c ) ) {
        return true;
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

// M-C: the adjacent tile in the 45 degree sector the cursor (vector cdx, cdy from the player's tile
// centre) is in.  If that tile holds no valid target but the neighbouring sector on the side of the
// cursor does and the cursor is within 12 degrees of the boundary to it, the neighbour is used.
// Returns false (dir untouched) if the cursor is inside the player's own tile (< 0.3 tile).
bool cursor_sector_dir( const Character &who, double cdx, double cdy, std::pair<int, int> &dir )
{
    if( std::sqrt( cdx * cdx + cdy * cdy ) < 0.3 ) {
        return false;
    }
    const double step = M_PI / 4.0;
    const double angle = std::atan2( cdy, cdx );
    const int octant = static_cast<int>( std::lround( angle / step ) );
    dir = compass_dir( octant );
    const tripoint_abs_ms pos = who.pos_abs();
    if( target_at( who, pos + tripoint_rel_ms( dir.first, dir.second, 0 ) ) != nullptr ) {
        return true;
    }
    const double off = angle - octant * step; // -22.5 .. +22.5 degrees
    const double tolerance = 12.0 * M_PI / 180.0;
    if( std::abs( off ) >= step / 2.0 - tolerance ) {
        const auto side = compass_dir( octant + ( off > 0.0 ? 1 : -1 ) );
        if( target_at( who, pos + tripoint_rel_ms( side.first, side.second, 0 ) ) != nullptr ) {
            dir = side;
        }
    }
    return true;
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
            // moving spoils the aim (top-down shooter bloom, Noah 2026-10-15): walking keeps it
            // at 35% of the worst, running at 70%; it only settles while standing
            const bool moving = now - st.moved_at < 150;
            if( gun->has_flag( flag_id( "ALWAYS_AIMED" ) ) ) {
                who.recoil = 0.0;
            } else if( moving ) {
                const double floor = MAX_RECOIL * ( who.is_running() ? 0.7 : 0.35 );
                if( who.recoil < floor ) {
                    who.recoil = std::min( floor, who.recoil + dt * MAX_RECOIL / 400.0 );
                }
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

// Throws the wielded item at the cursor.  Returns the time it takes, 0 if nothing happened.
int throw_at_cursor( Character &who, const input_frame &in, ms_t now, std::vector<event> *out )
{
    map &here = get_map();
    const tripoint_abs_ms pos = who.pos_abs();
    if( !in.cursor_valid || in.cursor_z != pos.z() ) {
        return 0;
    }
    const tripoint_abs_ms aim = tile_of( in.cursor_x, in.cursor_y, pos.z() );
    if( !here.inbounds( aim ) || aim == pos ) {
        return 0;
    }
    item_location weapon = who.get_wielded_item();
    if( !weapon ) {
        who.add_msg_if_player( m_info, _( "Wield something to throw it." ) );
        return 250;
    }
    if( weapon->has_flag( flag_NO_UNWIELD ) ) {
        who.add_msg_if_player( m_info, _( "That's part of your body, you can't throw that!" ) );
        return 250;
    }
    if( who.has_flag( json_flag_CANNOT_ATTACK ) || who.has_effect( effect_incorporeal ) ) {
        who.add_msg_if_player( m_info, _( "You are incapable of throwing anything!" ) );
        return 250;
    }
    item thrown = *weapon;
    const int range = who.throw_range( thrown );
    if( range <= 0 ) {
        who.add_msg_if_player( m_info, _( "That is too heavy to throw." ) );
        return 250;
    }
    // like avatar_action::plthrow: one of a stack, else the whole item leaves the hands
    if( weapon->count_by_charges() && weapon->charges > 1 ) {
        weapon->mod_charges( -1 );
        thrown.charges = 1;
    } else {
        who.remove_weapon();
    }
    int charged = 0;
    {
        native_scope scope( who );
        who.throw_item( here.get_bub( aim ), thrown, std::nullopt );
        charged = scope.finish();
    }
    const int duration = std::max( tune().fire_min_ms, moves_to_ms( who, charged ) );

    event ev;
    ev.type = fx_type::shot;
    ev.actor = who.getID().get_value();
    ev.from = pos;
    ev.to = aim;
    ev.dur_ms = duration;
    const auto dir = snap8( in.cursor_x - ( pos.x() + 0.5 ), in.cursor_y - ( pos.y() + 0.5 ) );
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
    st.bump_swing = false;
    begin_lunge( who.pos_abs(), dx, dy, t0, windup, total );

    static const bool swing_log = std::getenv( "CATA_MP_TEST_TRACE" ) != nullptr;
    if( swing_log ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace swing id=" << who.getID().get_value() <<
                                   " t=" << now_ms() << " dx=" << dx << " dy=" << dy << " hit=" <<
                                   ( target_at( who, who.pos_abs() + tripoint_rel_ms( dx, dy, 0 ) ) != nullptr ? 1 : 0 );
    }

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

// Pushes @p target one tile in direction (dx, dy) if that tile is free and can be walked on
// (never into walls, other creatures or ledges: no extra damage).  True if it moved.
bool push_creature( Creature &target, int dx, int dy, ms_t now, std::vector<event> *out )
{
    map &here = get_map();
    const tripoint_abs_ms from = target.pos_abs();
    const tripoint_abs_ms to = from + tripoint_rel_ms( dx, dy, 0 );
    if( ( dx == 0 && dy == 0 ) || !here.inbounds( to ) ) {
        return false;
    }
    const tripoint_bub_ms to_bub = here.get_bub( to );
    if( !here.passable( to_bub ) || here.has_flag( ter_furn_flag::TFLAG_NO_FLOOR, to_bub ) ||
        get_creature_tracker().creature_at( to ) != nullptr ) {
        return false;
    }
    target.knock_back_to( to_bub );
    if( target.pos_abs() != to ) {
        return false;
    }
    constexpr int push_ms = 140;
    begin_slide( from, to, now, push_ms );
    event ev;
    ev.type = fx_type::step;
    ev.from = from;
    ev.to = to;
    ev.dur_ms = push_ms;
    emit( out, ev );
    return true;
}

// Knockback of a melee hit (Noah 2026-10-15): heavy blunt weapons can push what they hit one tile
// back.  Chance: 10% per kg of the weapon plus 2% per strength above 8, at most 50%; only
// bash weapons of at least 1 kg, only targets up to human size.  World option
// FLUID_MELEE_KNOCKBACK.
void melee_knockback( Character &who, Creature &target, int dx, int dy, ms_t now,
                      std::vector<event> *out )
{
    if( !get_option<bool>( "FLUID_MELEE_KNOCKBACK" ) || target.is_dead_state() ||
        target.get_size() > creature_size::medium ) {
        return;
    }
    item_location weapon = who.used_weapon();
    if( !weapon ) {
        return;
    }
    static const damage_type_id dt_bash( "bash" );
    static const damage_type_id dt_cut( "cut" );
    static const damage_type_id dt_stab( "stab" );
    const int bash = weapon->damage_melee( dt_bash );
    const double kg = units::to_kilogram( weapon->weight() );
    if( kg < 1.0 || bash < std::max( weapon->damage_melee( dt_cut ), weapon->damage_melee( dt_stab ) ) ) {
        return;
    }
    const int chance = std::clamp( static_cast<int>( kg * 10 ) + 2 * ( who.get_str() - 8 ), 0, 50 );
    if( x_in_y( chance, 100 ) ) {
        push_creature( target, dx, dy, now, out );
    }
}

// Shove (Noah 2026-10-15, after Project Zomboid): a quick push of the creature in front, no
// damage.  Creatures up to human size are pushed one tile back (onto a free tile) and may fall:
// 20% + 4% per strength above 8 (5..60%), small ones 20% more; bigger creatures only stagger.
// Costs 300 stamina and 450 ms.  Returns the time it takes, 0 if nothing happened.
int shove( Character &who, actor_state &st, double dx, double dy, ms_t t0, std::vector<event> *out )
{
    static const efftype_id effect_downed_shove( "downed" );
    constexpr int shove_ms = 450;
    constexpr int shove_stamina = 300;
    if( std::abs( dx ) + std::abs( dy ) < 1e-6 ) {
        return 0;
    }
    if( who.get_stamina() < shove_stamina ) {
        who.add_msg_if_player( m_bad, _( "You are too winded to shove." ) );
        return 250;
    }
    std::pair<int, int> dir = snap8( dx, dy );
    Creature *target = assist_target( who, dx, dy, dir );
    const tripoint_abs_ms pos = who.pos_abs();
    who.mod_stamina( -shove_stamina );
    begin_lunge( pos, dir.first, dir.second, t0, 0, 220 );
    event sw;
    sw.type = fx_type::swing;
    sw.actor = who.getID().get_value();
    sw.from = pos;
    sw.dx = dir.first;
    sw.dy = dir.second;
    sw.dur_ms = 220;
    emit( out, sw );
    st.face_dx = dir.first;
    st.face_dy = dir.second;
    if( target == nullptr ) {
        return shove_ms;
    }
    event ev;
    ev.type = fx_type::strike;
    ev.actor = who.getID().get_value();
    ev.from = pos;
    ev.to = target->pos_abs();
    ev.dx = dir.first;
    ev.dy = dir.second;
    ev.hit = true;
    register_hit( pos, ev.to, dir.first, dir.second, t0 );
    emit( out, ev );
    if( target->get_size() > creature_size::medium ) {
        who.add_msg_if_player( m_info, _( "You shove the %s, but it hardly moves." ), target->disp_name() );
        return shove_ms;
    }
    // the shove distance is a world option (1-3 tiles); each tile is one push
    bool pushed = push_creature( *target, dir.first, dir.second, t0, out );
    for( int extra = 1; pushed && extra < get_option<int>( "MP_SHOVE_TILES" ); ++extra ) {
        pushed = push_creature( *target, dir.first, dir.second, t0, out );
    }
    if( monster *const z = target->as_monster() ) {
        // a shoved monster loses its blow and needs a moment
        stagger_monster( *z, 500, t0 );
    }
    int fall = std::clamp( 20 + 4 * ( who.get_str() - 8 ), 5, 60 );
    if( target->get_size() < creature_size::medium ) {
        fall += 20;
    }
    if( x_in_y( fall, 100 ) ) {
        target->add_effect( effect_downed_shove, 1_turns, true );
        who.add_msg_if_player( m_good, _( "You shove the %s to the ground!" ), target->disp_name() );
    } else if( pushed ) {
        who.add_msg_if_player( m_info, _( "You shove the %s back." ), target->disp_name() );
    } else {
        who.add_msg_if_player( m_info, _( "You shove the %s." ), target->disp_name() );
    }
    // a pushed creature loses a moment
    target->mod_moves( -50 );
    return shove_ms;
}

// The blow lands: whoever is there now is hit, the direction can still be steered a little.
void do_strike( Character &who, actor_state &st, const input_frame &in, ms_t now,
                std::vector<event> *out )
{
    const tripoint_abs_ms pos = who.pos_abs();
    std::pair<int, int> dir = { st.strike_dx, st.strike_dy };
    Creature *target = nullptr;

    // phase 6 (Noah 2026-10-15): the blow hits what is within the weapon's reach of the body
    // (centre to centre, plus the bodies' width) inside a 90 degree arc toward the cursor;
    // creatures count where they are drawn (gliding monsters included)
    double bx = pos.x() + 0.5;
    double by = pos.y() + 0.5;
    if( free_movement() && st.body_valid && st.body_tile == pos ) {
        bx = st.body_x;
        by = st.body_y;
    }
    double ax = st.strike_dx;
    double ay = st.strike_dy;
    if( !st.bump_swing && in.cursor_valid && in.cursor_z == pos.z() ) {
        const double cdx = in.cursor_x - bx;
        const double cdy = in.cursor_y - by;
        if( std::hypot( cdx, cdy ) > 0.3 ) {
            ax = cdx;
            ay = cdy;
        }
    }
    const double alen = std::max( 1e-6, std::hypot( ax, ay ) );
    item_location weapon = who.used_weapon();
    const int reach = std::max( 1, weapon ? weapon->reach_range( who ) : 1 );
    const double max_dist = reach + 2.0 * tune().body_half;
    const ms_t when = now;
    double best_score = -2.0;
    int best_cheb = 0;
    for( int ox = -reach; ox <= reach; ++ox ) {
        for( int oy = -reach; oy <= reach; ++oy ) {
            if( ox == 0 && oy == 0 ) {
                continue;
            }
            const tripoint_abs_ms tile = pos + tripoint_rel_ms( ox, oy, 0 );
            Creature *c = target_at( who, tile );
            if( c == nullptr ) {
                continue;
            }
            const offset drawn = travel_offset_at( tile, when );
            const double cx = tile.x() + 0.5 + drawn.x - bx;
            const double cy = tile.y() + 0.5 + drawn.y - by;
            const double clen = std::hypot( cx, cy );
            if( clen > max_dist || clen < 1e-6 ) {
                continue;
            }
            const double cosine = ( ax * cx + ay * cy ) / ( alen * clen );
            if( cosine < 0.7071 ) {
                continue;
            }
            // closest to the aim line first, then the nearer one
            const double score = cosine - clen * 0.01;
            if( score > best_score ) {
                best_score = score;
                target = c;
                dir = { std::clamp( ox, -1, 1 ), std::clamp( oy, -1, 1 ) };
                best_cheb = std::max( std::abs( ox ), std::abs( oy ) );
            }
        }
    }

    bool hit = false;
    if( target != nullptr ) {
        native_scope scope( who );
        if( mp_groups::sparring( who, *target ) ) {
            // B5: a sparring blow trains, it does not hurt
            mp_groups::spar_hit( who, *target->as_character() );
            hit = true;
        } else if( best_cheb > 1 ) {
            // a reach weapon past the neighbouring tiles
            who.reach_attack( get_map().get_bub( target->pos_abs() ) );
            hit = true;
        } else {
            const int hp_before = target->get_hp();
            hit = who.melee_attack( *target, true );
            // a strong hit on a monster winding up a blow staggers it and cancels the blow
            const int stagger_damage = get_option<int>( "MP_STAGGER_DAMAGE" );
            monster *const z = target->as_monster();
            if( z != nullptr && stagger_damage > 0 && !z->is_dead() &&
                hp_before - z->get_hp() >= stagger_damage && monster_winding_up( *z ) ) {
                stagger_monster( *z, 600, now );
                who.add_msg_if_player( m_good, _( "You stagger the %s!" ), z->get_name() );
            }
        }
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
    ev.to = target != nullptr ? target->pos_abs() : pos;
    ev.dx = dir.first;
    ev.dy = dir.second;
    ev.hit = hit;
    if( hit && target != nullptr ) {
        register_hit( pos, ev.to, dir.first, dir.second, now );
    }
    emit( out, ev );
    if( hit && target != nullptr && best_cheb <= 1 && !mp_groups::sparring( who, *target ) ) {
        melee_knockback( who, *target, dir.first, dir.second, now, out );
    }
}

// ---- steps -------------------------------------------------------------------------------

// Starts a step in direction (dx, dy). Returns true if time was spent.
// ---- dragging furniture (grab) for the body of a multiplayer player ----

// A step while holding furniture: pulling or pushing moves it along.  Returns true if the body
// moved (or tried to and failed: then @p blocked is set).
bool body_drag_step( npc &guy, const tripoint_abs_ms &dest, bool &blocked )
{
    blocked = false;
    if( guy.grab_point == tripoint_rel_ms::zero ) {
        return false;
    }
    map &here = get_map();
    const tripoint_bub_ms pos = guy.pos_bub( here );
    const tripoint_bub_ms fpos = pos + guy.grab_point;
    if( here.veh_at( fpos ) ) {
        // dragging a vehicle: the same code the avatar uses
        const tripoint_bub_ms step_to = here.get_bub( dest );
        // the body has to be able to take its step, else the vehicle must not move either
        if( step_to != fpos && ( !here.passable( step_to ) ||
                                 get_creature_tracker().creature_at( step_to ) != nullptr ) ) {
            blocked = true;
            return true;
        }
        if( g->grabbed_veh_move( tripoint_rel_ms( step_to.x() - pos.x(), step_to.y() - pos.y(), 0 ), guy ) ) {
            blocked = true;
            return true;
        }
        return false;
    }
    if( !here.has_furn( fpos ) ) {
        guy.grab_point = tripoint_rel_ms::zero;
        return false;
    }
    const tripoint_bub_ms to = here.get_bub( dest );
    const point_rel_ms dp = to.xy() - pos.xy();
    const point_rel_ms grab = guy.grab_point.xy();
    const bool pulling = dp == -grab;
    const bool pushing = dp == grab;
    if( !pulling && !pushing ) {
        // stepping sideways lets go of it
        guy.add_msg_if_player( _( "You let go of the %s." ), here.furnname( fpos ) );
        guy.grab_point = tripoint_rel_ms::zero;
        return false;
    }
    const tripoint_bub_ms fdest = pulling ? pos : fpos + tripoint_rel_ms( dp, 0 );
    const furn_t &furn = here.furn( fpos ).obj();
    const Creature *in_the_way = get_creature_tracker().creature_at( fdest );
    const bool can_move = here.can_move_furniture( fpos, &guy );
    const bool no_items = here.has_flag( ter_furn_flag::TFLAG_NOITEM, fdest );
    if( !can_move || no_items || ( in_the_way != nullptr && in_the_way != &guy ) ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: drag blocked: can_move " << can_move << " noitem " <<
                                   no_items << " creature " << ( in_the_way != nullptr && in_the_way != &guy );
        guy.add_msg_if_player( m_bad, _( "The %s won't move that way." ), here.furnname( fpos ) );
        blocked = true;
        return true;
    }
    if( furn.move_str_req > guy.get_arm_str() + 2 ) {
        guy.add_msg_if_player( m_bad, _( "The %s is too heavy for you." ), here.furnname( fpos ) );
        blocked = true;
        return true;
    }
    if( pulling && ( !here.passable( to ) || get_creature_tracker().creature_at( to ) != nullptr ) ) {
        blocked = true;
        return true;
    }
    // the furniture and what lies on it go first, then the body follows
    const furn_id fid = here.furn( fpos ).id();
    std::vector<item> things;
    for( const item &it : here.i_at( fpos ) ) {
        things.push_back( it );
    }
    here.i_clear( fpos );
    here.furn_set( fpos, furn_str_id::NULL_ID() );
    here.furn_set( fdest, fid );
    for( const item &it : things ) {
        here.add_item_or_charges( fdest, it );
    }
    guy.mod_moves( -std::max( 100, furn.move_str_req * 10 ) );
    guy.move_to( to );
    return true;
}

// true if a plain step onto @p is possible: walkable, nobody there, nothing harmful
bool step_is_free( const tripoint_abs_ms &p )
{
    map &here = get_map();
    if( !here.inbounds( p ) ) {
        return false;
    }
    const tripoint_bub_ms bub = here.get_bub( p );
    return here.passable( bub ) && get_creature_tracker().creature_at( bub ) == nullptr &&
           !g->is_dangerous_tile( bub );
}

/**
 * The direction a step toward (@p dx, @p dy) really takes (corner sliding): a blocked diagonal
 * slides along the cardinal component that is free, if exactly one is. Anything else (not a
 * diagonal, an attack, a door to open, both or neither component free) is left as it is.
 */
std::pair<int, int> corner_slide_dir( const Character &who, int dx, int dy )
{
    if( !tune().corner_slide || dx == 0 || dy == 0 ) {
        return { dx, dy };
    }
    map &here = get_map();
    const tripoint_abs_ms from = who.pos_abs();
    const tripoint_abs_ms dest = from + tripoint_rel_ms( dx, dy, 0 );
    if( !here.inbounds( dest ) || target_at( who, dest ) != nullptr ) {
        return { dx, dy };
    }
    const tripoint_bub_ms bub = here.get_bub( dest );
    // doors and the like are opened by walking into them, not slid around
    const bool openable = !here.ter( bub ).obj().open.is_null() || !here.furn( bub ).obj().open.is_null();
    if( step_is_free( dest ) || openable ) {
        return { dx, dy };
    }
    const bool free_x = step_is_free( from + tripoint_rel_ms( dx, 0, 0 ) );
    const bool free_y = step_is_free( from + tripoint_rel_ms( 0, dy, 0 ) );
    if( free_x == free_y ) {
        return { dx, dy };
    }
    return free_x ? std::make_pair( dx, 0 ) : std::make_pair( 0, dy );
}

bool trace_enabled()
{
    static const bool on = std::getenv( "CATA_MP_TEST_TRACE" ) != nullptr;
    return on;
}

bool start_step( Character &who, actor_state &st, int dx, int dy, ms_t now, ms_t t0,
                 std::vector<event> *out )
{
    map &here = get_map();
    const tuning &tn = tune();
    const tripoint_abs_ms from = who.pos_abs();
    tripoint_abs_ms dest = from + tripoint_rel_ms( dx, dy, 0 );

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
        st.bump_swing = true;
        return true;
    }

    // a blocked diagonal slides along the free cardinal component
    const int wanted_dx = dx;
    const int wanted_dy = dy;
    std::tie( dx, dy ) = corner_slide_dir( who, dx, dy );
    if( dx != wanted_dx || dy != wanted_dy ) {
        dest = from + tripoint_rel_ms( dx, dy, 0 );
        st.face_dx = dx;
        st.face_dy = dy;
        if( trace_enabled() ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace slide id=" << who.getID().get_value() <<
                                       " t=" << now << " wanted=" << wanted_dx << "," << wanted_dy <<
                                       " took=" << dx << "," << dy;
        }
    }

    // a player's body walking into somebody who is not an enemy: the NPC menu (talk, trade ...)
    if( !who.is_avatar() ) {
        npc *walker = dynamic_cast<npc *>( &who );
        Creature *there = get_creature_tracker().creature_at( here.get_bub( dest ) );
        if( walker != nullptr && there != nullptr && there->is_npc() &&
            net::bump_npc( *walker, *there->as_npc() ) ) {
            st.ready_at = t0 + 150;
            return true;
        }
    }

    const double recoil_before = who.recoil;
    int charged = 0;
    bool moved = false;
    {
        native_scope scope( who );
        if( who.is_avatar() ) {
            moved = avatar_action::move( get_avatar(), here, tripoint_rel_ms( dx, dy, 0 ) );
        } else if( npc *guy = dynamic_cast<npc *>( &who ) ) {
            bool blocked = false;
            if( !body_drag_step( *guy, dest, blocked ) ) {
                guy->move_to( here.get_bub( dest ) );
            }
            moved = !blocked;
        }
        charged = scope.finish();
    }
    const tripoint_abs_ms now_pos = who.pos_abs();
    const bool changed = now_pos != from;

    if( changed ) {
        if( who.is_avatar() ) {
            note_own_step_started();
        }
        if( !who.is_avatar() ) {
            // a player's body spends stamina like the host's avatar (running costs, walking is free)
            const int stam_before = who.get_stamina();
            who.burn_move_stamina( charged );
            static const bool burn_log = std::getenv( "CATA_MP_TEST_STAMINA" ) != nullptr;
            if( burn_log && who.is_running() ) {
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: burn charged " << charged << " stamina " << stam_before << " -> " << who.get_stamina();
            }
        }
        // a step keeps part of the steadiness unless the character runs
        const double loss = who.is_running() ? tn.aim_run_loss : tn.aim_step_loss;
        who.recoil = std::min( MAX_RECOIL, recoil_before + ( MAX_RECOIL - recoil_before ) * loss );
        const int dur = std::clamp( std::max( 40, moves_to_ms( who, charged ) ), 40, tn.slide_max_ms );
        st.ready_at = t0 + dur;
        st.aim_move_carry = 0.0;
        if( trace_enabled() ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace step id=" << who.getID().get_value() <<
                                       " t=" << now << " from=" << from.x() << "," << from.y() <<
                                       " to=" << now_pos.x() << "," << now_pos.y() << " dx=" << dx <<
                                       " dy=" << dy << " dur=" << dur;
        }
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
    if( trace_enabled() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace blocked id=" << who.getID().get_value() <<
                                   " t=" << now << " dx=" << wanted_dx << " dy=" << wanted_dy;
    }
    st.ready_at = now + 150;
    ( void )moved;
    return false;
}

// ---- free movement -----------------------------------------------------------------------
// (doc/MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md) A player's body is a square of 2 * body_half tiles
// that moves continuously.  The game keeps the character on the tile under the body's centre;
// crossing into another tile runs the native move once (traps, fields, items, dragging ...),
// whose charged moves are refunded: the time was already spent walking.

// What cata_tiles draws for a body: the tile it stands on and where the sprite is from there.
struct body_draw {
    tripoint_abs_ms tile;
    offset off;
    bool moving = false;
    ms_t stamp = 0;
    // the client's own body: its predicted speed in tiles per ms in the last frame (the sprite is
    // drawn where the body is at the moment of drawing, which comes a varying few ms later)
    double vx = 0.0;
    double vy = 0.0;
};

std::map<int, body_draw> &body_draws()
{
    static std::map<int, body_draw> m;
    return m;
}

// The native cost (moves) of moving from @p from toward its neighbour @p to, as the game charges
// a step: terrain and furniture of both tiles, run/crouch/prone, encumbrance, load, mount,
// swimming.  Always a straight step: free movement is equally fast in every direction.
int body_step_cost( const Character &who, const tripoint_abs_ms &from, const tripoint_abs_ms &to )
{
    map &here = get_map();
    if( !here.inbounds( from ) ) {
        return 100;
    }
    const tripoint_bub_ms f = here.get_bub( from );
    tripoint_bub_ms t = f;
    if( here.inbounds( to ) && here.passable( here.get_bub( to ) ) ) {
        t = here.get_bub( to );
    }
    if( !who.in_vehicle && here.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, t ) &&
        here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, t ) && !here.veh_at( t ) ) {
        return std::max( 1, who.swim_speed() );
    }
    int mc = here.combined_movecost( f, t );
    if( f.x() != t.x() && f.y() != t.y() && trigdist ) {
        // the same speed in every direction (Noah, decision 2): the native cost of a diagonal
        // step is 1.41x that of a straight one
        mc = mc * 50 / 71;
    }
    if( mc <= 0 ) {
        // standing somewhere odd (the own tile is not passable): plain ground
        mc = 100;
    }
    int cost = who.run_cost( mc, false );
    if( who.is_mounted() && who.mounted_creature ) {
        cost = cost * 100 / std::max( 1, who.mounted_creature->get_speed() );
    }
    return std::max( 1, cost );
}

// True if the body may not overlap tile @p: walls, closed doors, solid furniture and vehicle
// parts, other creatures, harmful tiles and ledges.  Those are entered (or opened, attacked,
// talked to) by pushing into them, which goes through the native move like a tile step.
bool body_blocked( const Character &who, const tripoint_abs_ms &p )
{
    map &here = get_map();
    if( !here.inbounds( p ) ) {
        return true;
    }
    const tripoint_bub_ms b = here.get_bub( p );
    if( !here.passable( b ) ) {
        return true;
    }
    const Creature *c = get_creature_tracker().creature_at( b );
    if( c != nullptr && c != &who && c != who.mounted_creature.get() ) {
        return true;
    }
    return g->is_dangerous_tile( b ) || here.has_flag( ter_furn_flag::TFLAG_NO_FLOOR, b );
}

int floor_int( double v )
{
    return static_cast<int>( std::floor( v ) );
}

// Moves the body by @p d tiles along @p axis (0 = x, 1 = y).  Stops it before a blocked tile;
// then @p blocked_cells holds the coordinates (on the other axis) of the blocked tiles and the
// result is false.
bool body_move_axis( const Character &who, actor_state &st, int axis, double d,
                     std::vector<int> &blocked_cells )
{
    constexpr double eps = 1e-7;
    const double h = tune().body_half;
    double &a = axis == 0 ? st.body_x : st.body_y;
    const double b = axis == 0 ? st.body_y : st.body_x;
    const int z = st.body_tile.z();
    blocked_cells.clear();
    if( std::abs( d ) < 1e-12 ) {
        return true;
    }
    const double na = a + d;
    // the tile column (row) the leading edge is in, before and after
    const int cur_edge = d > 0 ? floor_int( a + h ) : floor_int( a - h );
    const int new_edge = d > 0 ? floor_int( na + h ) : floor_int( na - h );
    if( new_edge != cur_edge ) {
        const int lo = floor_int( b - h + eps );
        const int hi = floor_int( b + h - eps );
        for( int o = lo; o <= hi; ++o ) {
            const tripoint_abs_ms t = axis == 0 ? tripoint_abs_ms( new_edge, o, z ) :
                                      tripoint_abs_ms( o, new_edge, z );
            if( t != st.body_tile && body_blocked( who, t ) ) {
                blocked_cells.push_back( o );
            }
        }
    }
    if( blocked_cells.empty() ) {
        a = na;
        return true;
    }
    // stop with the leading edge on the boundary of the blocked tiles
    a = d > 0 ? new_edge - h - eps : new_edge + 1 + h + eps;
    return false;
}

// Walking straight into a corner that only overlaps the edge of the body: slide around it.
// Returns true if the body was nudged.
bool body_corner_nudge( const Character &who, actor_state &st, int axis,
                        const std::vector<int> &blocked_cells, double amount )
{
    const tuning &tn = tune();
    if( blocked_cells.size() != 1 ) {
        return false;
    }
    const double h = tn.body_half;
    const double b = axis == 0 ? st.body_y : st.body_x;
    const int cell = blocked_cells.front();
    // how far the body reaches into the blocked tile's row (column), and which way is free
    double overlap = 0.0;
    double dir = 0.0;
    if( cell == floor_int( b - h + 1e-7 ) && cell != floor_int( b ) ) {
        overlap = ( cell + 1 ) - ( b - h );
        dir = 1.0;
    } else if( cell == floor_int( b + h - 1e-7 ) && cell != floor_int( b ) ) {
        overlap = ( b + h ) - cell;
        dir = -1.0;
    }
    if( dir == 0.0 || overlap <= 0.0 || overlap > tn.corner_nudge ) {
        return false;
    }
    std::vector<int> unused;
    const double before = b;
    body_move_axis( who, st, 1 - axis, dir * std::min( amount, overlap + 1e-6 ), unused );
    const double after = axis == 0 ? st.body_y : st.body_x;
    return std::abs( after - before ) > 1e-9;
}

void body_set_correction( actor_state &st, double from_x, double from_y, ms_t now, int dur )
{
    st.corr_x = from_x - st.body_x;
    st.corr_y = from_y - st.body_y;
    st.corr_t0 = now;
    st.corr_ms = std::max( 1, dur );
}

offset body_correction_at( const actor_state &st, ms_t now )
{
    offset o;
    if( st.corr_ms <= 0 || now >= st.corr_t0 + st.corr_ms ) {
        return o;
    }
    const double left = 1.0 - static_cast<double>( now - st.corr_t0 ) / st.corr_ms;
    o.x = st.corr_x * left;
    o.y = st.corr_y * left;
    return o;
}

// Keeps the body on the character's tile: a move the game made itself (stairs, knockback,
// vehicles, teleports, loading) puts the body on the centre of the new tile.  Returns the time
// since the last frame (ms) the body may move for.
int body_sync( Character &who, actor_state &st, ms_t now )
{
    const tripoint_abs_ms pos = who.pos_abs();
    if( !st.body_valid ) {
        st.body_valid = true;
        st.body_x = pos.x() + 0.5;
        st.body_y = pos.y() + 0.5;
        st.body_tile = pos;
        st.body_last = now;
        st.corr_ms = 0;
        return 0;
    }
    if( pos != st.body_tile ) {
        const offset drawn = body_correction_at( st, now );
        const double old_x = st.body_x + drawn.x;
        const double old_y = st.body_y + drawn.y;
        const bool next_door = pos.z() == st.body_tile.z() &&
                               std::max( std::abs( pos.x() - st.body_tile.x() ), std::abs( pos.y() - st.body_tile.y() ) ) == 1;
        st.body_x = pos.x() + 0.5;
        st.body_y = pos.y() + 0.5;
        st.body_tile = pos;
        st.corr_ms = 0;
        if( next_door ) {
            // pushed / pulled one tile: a short slide instead of a jump
            body_set_correction( st, old_x, old_y, now, 120 );
        }
    }
    const int dt = static_cast<int>( std::clamp<ms_t>( now - st.body_last, 0,
                                     tune().max_move_frame_ms ) );
    st.body_last = now;
    return dt;
}

// What the renderer gets for this body.
void body_publish( const Character &who, const actor_state &st, ms_t now )
{
    body_draw &d = body_draws()[who.getID().get_value()];
    d.tile = st.body_tile;
    const offset c = body_correction_at( st, now );
    d.off.x = st.body_x - ( st.body_tile.x() + 0.5 ) + c.x;
    d.off.y = st.body_y - ( st.body_tile.y() + 0.5 ) + c.y;
    d.moving = st.body_moving || c.x != 0.0 || c.y != 0.0;
    d.stamp = now;
}

// The body's centre entered @p dest: the native move takes the character there.  False if the
// game refused (the body is then kept on its tile).
bool body_cross( Character &who, actor_state &st, const tripoint_abs_ms &dest, ms_t now,
                 std::vector<event> *out )
{
    map &here = get_map();
    const tuning &tn = tune();
    const tripoint_abs_ms from = who.pos_abs();
    const int dx = std::clamp( dest.x() - from.x(), -1, 1 );
    const int dy = std::clamp( dest.y() - from.y(), -1, 1 );
    const double recoil_before = who.recoil;
    int charged = 0;
    {
        native_scope scope( who );
        if( who.is_avatar() ) {
            avatar_action::move( get_avatar(), here, tripoint_rel_ms( dx, dy, 0 ) );
        } else if( npc *guy = dynamic_cast<npc *>( &who ) ) {
            bool blocked = false;
            if( !body_drag_step( *guy, dest, blocked ) ) {
                guy->move_to( here.get_bub( dest ) );
            }
        }
        charged = scope.finish();
    }
    const tripoint_abs_ms now_pos = who.pos_abs();
    if( now_pos == from ) {
        who.recoil = recoil_before;
        constexpr double margin = 1e-4;
        st.body_x = std::clamp( st.body_x, from.x() + margin, from.x() + 1.0 - margin );
        st.body_y = std::clamp( st.body_y, from.y() + margin, from.y() + 1.0 - margin );
        // do not ask the game again every frame
        st.ready_at = now + ( charged > 0 ? std::max( 40, moves_to_ms( who, charged ) ) : 150 );
        if( trace_enabled() ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace free refused id=" << who.getID().get_value() <<
                                       " t=" << now << " dx=" << dx << " dy=" << dy;
        }
        return false;
    }
    if( !who.is_avatar() ) {
        // a player's body spends stamina like the host's avatar (running costs, walking is free)
        who.burn_move_stamina( charged );
    }
    // a step keeps part of the steadiness unless the character runs
    const double loss = who.is_running() ? tn.aim_run_loss : tn.aim_step_loss;
    who.recoil = std::min( MAX_RECOIL, recoil_before + ( MAX_RECOIL - recoil_before ) * loss );
    st.aim_move_carry = 0.0;
    st.face_dx = dx;
    st.face_dy = dy;
    st.body_tile = now_pos;
    if( now_pos != dest ) {
        // the game put the character elsewhere (a ramp, a fall ...): the body follows
        st.body_x = now_pos.x() + 0.5;
        st.body_y = now_pos.y() + 0.5;
        st.corr_ms = 0;
    }
    // straight step time: what the others see as this player's tile slide
    const bool diag = dx != 0 && dy != 0;
    const int straight = diag && trigdist ? charged * 50 / 71 : charged;
    const int dur = std::clamp( moves_to_ms( who, straight ), 40, tn.slide_max_ms );
    if( trace_enabled() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace free cross id=" << who.getID().get_value() <<
                                   " t=" << now << " from=" << from.x() << "," << from.y() << " to=" << now_pos.x() << "," <<
                                   now_pos.y() << " charged=" << charged << " tile_ms=" << dur;
    }
    const int dist = std::max( std::abs( now_pos.x() - from.x() ), std::abs( now_pos.y() - from.y() ) );
    if( dist == 1 && now_pos.z() == from.z() ) {
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

// True if pushing into the tile in direction (dx, dy) can do something: a creature to attack or
// talk to, a door (also of a vehicle), a harmful tile or a ledge to enter on purpose, a diagonal
// gap to squeeze through.  A plain wall is not worth a try (that would only stall the body).
bool body_bump_worthy( const Character &who, const actor_state &st, int dx, int dy )
{
    map &here = get_map();
    const tripoint_abs_ms ahead = st.body_tile + tripoint_rel_ms( dx, dy, 0 );
    if( !here.inbounds( ahead ) ) {
        return false;
    }
    const tripoint_bub_ms b = here.get_bub( ahead );
    const Creature *c = get_creature_tracker().creature_at( b );
    if( c != nullptr && c != &who ) {
        return true;
    }
    if( here.passable( b ) ) {
        // harmful / no floor, or the diagonal squeeze between two blocked tiles
        return true;
    }
    return !here.ter( b ).obj().open.is_null() || !here.furn( b ).obj().open.is_null() ||
           static_cast<bool>( here.veh_at( b ) );
}

// Realtime combat (Noah 2026-10-16): a dodge = a 150 ms dash at three times the speed and
// 250 ms in which monster blows miss; 15% stamina, at most one per 1.2 s.  @p predict: the
// client's prediction (the host spends the stamina).  False if it is not possible now.
bool start_dodge( Character &who, actor_state &st, ms_t now, bool predict, int dir_x, int dir_y,
                  std::vector<event> *out )
{
    if( trace_enabled() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace dodge try predict=" << predict << " option=" <<
                                   get_option<bool>( "MP_DODGE" ) << " dir=" << dir_x << "," << dir_y << " ready_in=" <<
                                   st.dodge_ready_at - now << " stamina=" << who.get_stamina() << "/" << who.get_stamina_max();
    }
    if( !get_option<bool>( "MP_DODGE" ) || ( dir_x == 0 && dir_y == 0 ) ) {
        st.dodge_buffered_until = 0;
        return false;
    }
    if( now < st.dodge_ready_at ) {
        // a press a moment too early is kept and goes when the dodge is ready (as in Hades)
        if( st.dodge_ready_at - now <= 200 ) {
            st.dodge_buffered_until = st.dodge_ready_at + 50;
        }
        return false;
    }
    st.dodge_buffered_until = 0;
    if( who.get_stamina() < who.get_stamina_max() * 15 / 100 ) {
        if( !predict ) {
            // once per attempt: a grey puff and a word, no spam
            emit( out, flourish_event( flourish_kind::winded, st.body_x, st.body_y, who.posz(), 0, 0,
                                       who.pos_abs(), who.getID().get_value() ) );
            who.add_msg_if_player( m_bad, _( "You are too winded to dodge." ) );
        }
        st.dodge_ready_at = now + 400;
        return false;
    }
    if( !predict ) {
        who.mod_stamina( -who.get_stamina_max() * 15 / 100 );
    }
    st.dash_left_ms = 150;
    st.invuln_until = now + 250;
    st.dodge_ready_at = now + 1200;
    const double len = std::sqrt( static_cast<double>( dir_x * dir_x + dir_y * dir_y ) );
    const double ux = dir_x / len;
    const double uy = dir_y / len;
    if( predict ) {
        // the own dash is drawn at once, from where the body is
        add_flourish( flourish_kind::dash, st.body_x, st.body_y, who.posz(), ux, uy, who.pos_abs(), now );
    } else {
        emit( out, flourish_event( flourish_kind::dash, st.body_x, st.body_y, who.posz(), ux, uy,
                                   who.pos_abs(), who.getID().get_value() ) );
    }
    return true;
}

// The tile ahead in the frame's direction can now be entered (a door was just opened there).
bool body_door_opened( const actor_state &st, const input_frame &in )
{
    map &here = get_map();
    const tripoint_abs_ms ahead = st.body_tile + tripoint_rel_ms( in.move_x, in.move_y, 0 );
    return here.inbounds( ahead ) && here.passable( here.get_bub( ahead ) ) && !here.veh_at( here.get_bub( ahead ) );
}

// One frame of free movement in the held direction for @p dt ms.  Returns true if the body
// moved; if it could not move at all, pushing into the tile ahead does what a tile step into it
// does (attack, open a door, talk, squeeze through a diagonal gap, enter a harmful tile).
bool free_move( Character &who, actor_state &st, const input_frame &in, int dt, ms_t now,
                std::vector<event> *out )
{
    const tuning &tn = tune();
    const double len = std::sqrt( static_cast<double>( in.move_x * in.move_x + in.move_y * in.move_y ) );
    if( len < 0.5 ) {
        return false;
    }
    // holding furniture or a vehicle: tile steps along the grab line (free movement would cross
    // the tile edges one axis at a time, which counts as a step sideways and lets go)
    if( who.grab_point != tripoint_rel_ms::zero ) {
        if( now < st.ready_at ) {
            return false;
        }
        const tripoint_abs_ms before = who.pos_abs();
        start_step( who, st, in.move_x, in.move_y, now, now, out );
        const tripoint_abs_ms after = who.pos_abs();
        if( after != before ) {
            const double old_x = st.body_x;
            const double old_y = st.body_y;
            slides().erase( after );
            st.body_x = after.x() + 0.5;
            st.body_y = after.y() + 0.5;
            st.body_tile = after;
            body_set_correction( st, old_x, old_y, now, std::max( 40, static_cast<int>( st.ready_at - now ) ) );
            return true;
        }
        return false;
    }
    const double ux = in.move_x / len;
    const double uy = in.move_y / len;
    st.face_dx = in.move_x;
    st.face_dy = in.move_y;
    const tripoint_abs_ms ahead = st.body_tile + tripoint_rel_ms( in.move_x, in.move_y, 0 );
    const int ms_per_tile = std::max( 1, moves_to_ms( who, body_step_cost( who, st.body_tile, ahead ) ) );
    // a dodge dashes: three times the distance while its dash time lasts (frame time)
    const double dash = std::min<double>( dt, std::max( 0.0, st.dash_left_ms ) );
    st.dash_left_ms = std::max( 0.0, st.dash_left_ms - dt );
    double left = ( static_cast<double>( dt ) + 2.0 * dash ) / ms_per_tile;
    const double start_x = st.body_x;
    const double start_y = st.body_y;
    std::vector<int> cells;
    while( left > 1e-9 && who.pos_abs() == st.body_tile ) {
        const double step = std::min( left, tn.max_substep );
        left -= step;
        const double bx = st.body_x;
        const double by = st.body_y;
        if( ux != 0.0 && !body_move_axis( who, st, 0, ux * step, cells ) && uy == 0.0 ) {
            body_corner_nudge( who, st, 0, cells, step );
        }
        if( uy != 0.0 && !body_move_axis( who, st, 1, uy * step, cells ) && ux == 0.0 ) {
            body_corner_nudge( who, st, 1, cells, step );
        }
        const tripoint_abs_ms under( floor_int( st.body_x ), floor_int( st.body_y ), st.body_tile.z() );
        if( under != st.body_tile && !body_cross( who, st, under, now, out ) ) {
            break;
        }
        if( std::abs( st.body_x - bx ) + std::abs( st.body_y - by ) < 1e-9 ) {
            break;
        }
    }
    const bool moved = std::abs( st.body_x - start_x ) + std::abs( st.body_y - start_y ) > 1e-9;
    if( moved || dt <= 0 || now < st.ready_at ) {
        return moved;
    }
    if( !body_bump_worthy( who, st, in.move_x, in.move_y ) ) {
        return false;
    }
    if( trace_enabled() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace free bump id=" << who.getID().get_value() <<
                                   " t=" << now << " dx=" << in.move_x << " dy=" << in.move_y;
    }
    // a dangerous tile ahead (fire, a known trap, a ledge) is a soft wall: the body goes in only
    // after pushing on for tuning::soft_wall_ms; the push replaces the "Really step into" query
    map &here = get_map();
    const tripoint_bub_ms ahead_b = here.get_bub( st.body_tile + tripoint_rel_ms( in.move_x,
                                    in.move_y, 0 ) );
    const bool ledge = here.has_flag( ter_furn_flag::TFLAG_NO_FLOOR, ahead_b ) &&
                       !here.veh_at( ahead_b );
    const bool soft = here.passable( ahead_b ) && ( ledge || g->is_dangerous_tile( ahead_b ) ) &&
                      get_creature_tracker().creature_at( ahead_b ) == nullptr;
    if( soft ) {
        if( st.soft_since == 0 || now - st.soft_last > 150 || st.soft_dx != in.move_x ||
            st.soft_dy != in.move_y ) {
            st.soft_since = now;
            st.soft_dx = in.move_x;
            st.soft_dy = in.move_y;
            if( trace_enabled() ) {
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace free soft id=" << who.getID().get_value() <<
                                           " t=" << now << " dx=" << in.move_x << " dy=" << in.move_y << " ledge=" << ledge;
            }
            // warn instead of asking: what is there, and that pushing on goes in
            if( ledge ) {
                who.add_msg_if_player( m_warning, _( "A ledge!  Keep pushing to climb down." ) );
            } else {
                const std::vector<std::string> harm = g->get_dangerous_tile( ahead_b );
                who.add_msg_if_player( m_warning, string_format( _( "%s ahead!  Keep pushing to step in." ),
                                       harm.empty() ? _( "Danger" ) : uppercase_first_letter( enumerate_as_string( harm ) ) ) );
            }
        }
        st.soft_last = now;
        if( now - st.soft_since < tn.soft_wall_ms ) {
            return false;
        }
        st.soft_since = 0;
        if( ledge && !who.is_avatar() ) {
            // the host's avatar gets the native climb-down menu; a player's body gets the same
            // menu on the player's own screen (the client examines the ledge)
            net::request_climb( who, in.move_x, in.move_y );
            st.ready_at = now + 1500;
            return false;
        }
    } else {
        st.soft_since = 0;
    }
    // stuck: push into the tile ahead like a tile step
    const double old_x = st.body_x;
    const double old_y = st.body_y;
    const tripoint_abs_ms before = who.pos_abs();
    npc *body = who.is_avatar() ? nullptr : dynamic_cast<npc *>( &who );
    if( soft && body != nullptr ) {
        // npc::move_to never walks into danger on purpose: the body steps in like a player
        const tripoint_bub_ms from_b = body->pos_bub( here );
        const int cost = body->run_cost( here.combined_movecost( from_b, ahead_b ), false );
        body->setpos( here, ahead_b );
        body->burn_move_stamina( cost );
        here.creature_in_field( *body );
        here.creature_on_trap( *body );
        st.ready_at = now + std::max( 40, moves_to_ms( who, cost ) );
    } else {
        soft_wall_push = soft;
        start_step( who, st, in.move_x, in.move_y, now, now, out );
        soft_wall_push = false;
    }
    const tripoint_abs_ms after = who.pos_abs();
    if( after != before ) {
        // e.g. squeezed through a diagonal gap or stepped into a harmful tile: the body slides
        // there for the time the native step took
        slides().erase( after );
        st.body_x = after.x() + 0.5;
        st.body_y = after.y() + 0.5;
        st.body_tile = after;
        body_set_correction( st, old_x, old_y, now, std::max( 40, static_cast<int>( st.ready_at - now ) ) );
        return true;
    }
    return false;
}

// Autoattack in fluid mode: a swing at the weakest hostile creature on a neighbouring tile.
bool autoattack( Character &who, actor_state &st, ms_t t0, std::vector<event> *out )
{
    if( who.has_flag( json_flag_CANNOT_ATTACK ) ) {
        who.add_msg_if_player( m_info, _( "You are incapable of attacking!" ) );
        return false;
    }
    const tripoint_abs_ms pos = who.pos_abs();
    Creature *best = nullptr;
    int best_dx = 0;
    int best_dy = 0;
    for( int dy = -1; dy <= 1; ++dy ) {
        for( int dx = -1; dx <= 1; ++dx ) {
            if( dx == 0 && dy == 0 ) {
                continue;
            }
            Creature *c = target_at( who, pos + tripoint_rel_ms( dx, dy, 0 ) );
            if( c == nullptr || who.attitude_to( *c ) != Creature::Attitude::HOSTILE ) {
                continue;
            }
            if( best == nullptr || c->get_hp() < best->get_hp() ) {
                best = c;
                best_dx = dx;
                best_dy = dy;
            }
        }
    }
    if( best == nullptr ) {
        who.add_msg_if_player( m_info, _( "No hostile creature in reach." ) );
        return false;
    }
    start_swing( who, st, t0, best_dx, best_dy, out );
    // aimed at the chosen creature, not at the cursor
    st.bump_swing = true;
    return true;
}

bool can_act_now( const Character &who )
{
    return !who.is_dead_state() && who.get_moves() > 0 && !who.activity && !who.in_sleep_state();
}

} // namespace

bool body_step( npc &guy, const tripoint_bub_ms &dest )
{
    map &here = get_map();
    bool blocked = false;
    if( !body_drag_step( guy, here.get_abs( dest ), blocked ) ) {
        const int moves_before = guy.get_moves();
        guy.move_to( dest );
        // a player's body spends stamina like the host's avatar (running costs, walking is free)
        guy.burn_move_stamina( std::max( 0, moves_before - guy.get_moves() ) );
    }
    return !blocked;
}

bool body_grab( Character &who, const tripoint_rel_ms &dir )
{
    map &here = get_map();
    if( dir.xy() == point_rel_ms::zero ) {
        const bool had = who.grab_point != tripoint_rel_ms::zero;
        who.grab_point = tripoint_rel_ms::zero;
        who.add_msg_if_player( had ? _( "You let go." ) : _( "You get a hold of yourself." ) );
        return had;
    }
    const tripoint_bub_ms there = who.pos_bub( here ) + dir;
    if( const optional_vpart_position vp = here.veh_at( there ) ) {
        who.grab_point = tripoint_rel_ms( dir.xy(), 0 );
        who.add_msg_if_player( _( "You grab the %s." ), vp->vehicle().name );
        return true;
    }
    if( !here.has_furn( there ) ) {
        who.add_msg_if_player( m_info, _( "There's nothing to grab there!" ) );
        return false;
    }
    if( !here.furn( there ).obj().is_movable() ) {
        who.add_msg_if_player( _( "You can not grab the %s." ), here.furnname( there ) );
        return false;
    }
    who.grab_point = tripoint_rel_ms( dir.xy(), 0 );
    who.add_msg_if_player( _( "You grab the %s." ), here.furnname( there ) );
    return true;
}


// ---------------------------------------------------------------------------------------
// visuals API

void begin_slide( const tripoint_abs_ms &to, double from_x, double from_y, ms_t t0, int dur )
{
    slide s;
    s.from_x = from_x;
    s.from_y = from_y;
    s.t0 = t0;
    // (a safety limit; callers pass their own: a slow creature on a busy host may glide for seconds)
    s.dur = std::clamp( dur, 1, 6000 );
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

void register_hit( const tripoint_abs_ms &attacker, const tripoint_abs_ms &target, int dx, int dy,
                   ms_t now )
{
    const auto iter = lunges().find( attacker );
    if( iter != lunges().end() && iter->second.stop_ms == 0 ) {
        iter->second.stop_at = now;
        iter->second.stop_ms = hit_stop_ms;
        iter->second.total += hit_stop_ms;
    }
    hit_fx h;
    h.dx = dx;
    h.dy = dy;
    h.t0 = now;
    hits()[target] = h;
}

std::vector<incoming_blow> incoming_blows()
{
    std::vector<incoming_blow> out;
    const ms_t now = now_ms();
    creature_tracker &creatures = get_creature_tracker();
    for( const std::pair<const tripoint_abs_ms, lunge> &e : lunges() ) {
        const lunge &l = e.second;
        if( now < l.t0 || now >= l.t0 + l.windup || ( l.dx == 0 && l.dy == 0 ) ) {
            continue;
        }
        if( creatures.creature_at<monster>( e.first ) == nullptr ) {
            continue;
        }
        incoming_blow b;
        b.from = e.first;
        b.tile = e.first + tripoint_rel_ms( l.dx, l.dy, 0 );
        b.progress = std::clamp( static_cast<double>( now - l.t0 ) / std::max( 1, l.windup ), 0.0, 1.0 );
        out.push_back( b );
    }
    return out;
}

namespace
{
// shots of this frame that still have to go to the clients
std::vector<event> &pending_tracers()
{
    static std::vector<event> v;
    return v;
}

void add_tracer( const tripoint_abs_ms &from, const tripoint_abs_ms &to, ms_t now )
{
    if( from == to ) {
        return;
    }
    tracer_fx t;
    t.from = from;
    t.to = to;
    t.t0 = now;
    tracer_list().push_back( t );
}
} // namespace

void note_explosion( const tripoint_abs_ms &at, int radius )
{
    boom_fx b;
    b.at = at;
    b.radius = std::max( 1, radius );
    b.t0 = now_ms();
    boom_list().push_back( b );
    if( net::is_host() ) {
        event ev;
        ev.type = fx_type::boom;
        ev.from = at;
        ev.dx = b.radius;
        pending_tracers().push_back( ev );
    }
}

std::vector<flourish_view> flourishes()
{
    std::vector<flourish_view> out;
    const ms_t now = now_ms();
    for( const flourish_fx &f : flourish_list() ) {
        const int dur = flourish_ms( f.kind );
        if( now >= f.t0 && now < f.t0 + dur ) {
            flourish_view v;
            v.kind = f.kind;
            v.x = f.x;
            v.y = f.y;
            v.z = f.z;
            v.dx = f.dx;
            v.dy = f.dy;
            v.on = f.on;
            v.progress = static_cast<double>( now - f.t0 ) / dur;
            out.push_back( v );
        }
    }
    return out;
}

std::vector<boom_view> booms()
{
    std::vector<boom_view> out;
    const ms_t now = now_ms();
    for( const boom_fx &b : boom_list() ) {
        if( now >= b.t0 && now < b.t0 + boom_ms ) {
            boom_view v;
            v.at = b.at;
            v.radius = b.radius;
            v.progress = static_cast<double>( now - b.t0 ) / boom_ms;
            out.push_back( v );
        }
    }
    return out;
}

void note_projectile( const tripoint_abs_ms &from, const tripoint_abs_ms &to )
{
    add_tracer( from, to, now_ms() );
    if( net::is_host() ) {
        event ev;
        ev.type = fx_type::tracer;
        ev.from = from;
        ev.to = to;
        pending_tracers().push_back( ev );
    }
}

std::vector<tracer_view> tracers()
{
    std::vector<tracer_view> out;
    const ms_t now = now_ms();
    for( const tracer_fx &t : tracer_list() ) {
        const ms_t age = now - t.t0;
        if( age >= 0 && age < tracer_ms ) {
            tracer_view v;
            v.from = t.from;
            v.to = t.to;
            v.strength = 1.0 - static_cast<double>( age ) / tracer_ms;
            out.push_back( v );
        }
    }
    return out;
}

namespace
{
void add_aim_line( const tripoint_abs_ms &from, const tripoint_abs_ms &to, ms_t t0, int dur )
{
    std::vector<aim_line_fx> &v = aim_line_list();
    // one line per shooter
    v.erase( std::remove_if( v.begin(), v.end(), [&from]( const aim_line_fx & a ) {
        return a.from == from;
    } ), v.end() );
    aim_line_fx a;
    a.from = from;
    a.to = to;
    a.t0 = t0;
    a.dur = std::max( 1, dur );
    v.push_back( a );
}

struct ranged_windup {
    weak_ptr_fast<monster> attacker;
    std::string attack;
    ms_t ready_at = 0;
};

std::vector<ranged_windup> &ranged_windups()
{
    static std::vector<ranged_windup> v;
    return v;
}
constexpr int ranged_windup_ms = 800;
} // namespace

void monster_aims( const tripoint_abs_ms &from, const tripoint_abs_ms &to, int dur_ms )
{
    const ms_t now = now_ms();
    add_aim_line( from, to, now, dur_ms );
    if( net::is_host() ) {
        event ev;
        ev.type = fx_type::aimline;
        ev.from = from;
        ev.to = to;
        ev.dur_ms = dur_ms;
        pending_tracers().push_back( ev );
    }
}

std::vector<aim_line_view> monster_aim_lines()
{
    std::vector<aim_line_view> out;
    const ms_t now = now_ms();
    for( const aim_line_fx &a : aim_line_list() ) {
        if( now >= a.t0 && now < a.t0 + a.dur ) {
            aim_line_view v;
            v.from = a.from;
            v.to = a.to;
            v.progress = static_cast<double>( now - a.t0 ) / a.dur;
            out.push_back( v );
        }
    }
    return out;
}

bool ranged_windup_ready( monster &z, const std::string &attack )
{
    if( !enabled() ) {
        return true;
    }
    Creature *target = z.attack_target();
    const Character *ch = target != nullptr ? target->as_character() : nullptr;
    if( ch == nullptr || !( ch->is_avatar() || net::is_remote_player( *ch ) ) ||
        !z.sees( get_map(), *target ) ) {
        return true;
    }
    const ms_t now = now_ms();
    std::vector<ranged_windup> &v = ranged_windups();
    for( auto iter = v.begin(); iter != v.end(); ) {
        const shared_ptr_fast<monster> m = iter->attacker.lock();
        if( !m || now > iter->ready_at + 3000 ) {
            // gone, or the chance passed long ago: start over
            iter = v.erase( iter );
            continue;
        }
        if( m.get() == &z && iter->attack == attack ) {
            if( now < iter->ready_at ) {
                return false;
            }
            v.erase( iter );
            return true;
        }
        ++iter;
    }
    ranged_windup w;
    w.attacker = g->shared_from( z );
    w.attack = attack;
    w.ready_at = now + ranged_windup_ms;
    v.push_back( w );
    monster_aims( z.pos_abs(), target->pos_abs(), ranged_windup_ms );
    if( ch->is_avatar() || net::is_remote_player( *ch ) ) {
        net::alert_player( *ch, string_format( _( "The %s takes aim at you!" ), z.name() ),
                           static_cast<int>( alerts::level::danger ), z.pos_abs() );
    }
    return false;
}

std::vector<std::pair<tripoint_abs_ms, double>> hit_flashes()
{
    std::vector<std::pair<tripoint_abs_ms, double>> out;
    const ms_t now = now_ms();
    for( const std::pair<const tripoint_abs_ms, hit_fx> &e : hits() ) {
        const ms_t t = now - e.second.t0;
        if( t >= 0 && t < hit_flash_ms ) {
            out.emplace_back( e.first, 1.0 - static_cast<double>( t ) / hit_flash_ms );
        }
    }
    return out;
}

namespace
{
// The draw offset of a player's body standing on @p p, if there is a fresh one.  The client's
// own body is matched by the avatar's current tile and its offset taken from the body's world
// position: the host's update can move the avatar to the next tile between two movement frames,
// and an offset kept for the old tile would put the sprite (and the camera) a tile off for that
// frame - a shake at every tile edge (Noah's test, 2026-10-16).
std::optional<offset> body_draw_at( const tripoint_abs_ms &p, ms_t now )
{
    const bool client = client_active();
    const int own = client ? cinfo().my_id : -1;
    const bool own_here = client && get_avatar().pos_abs() == p;
    for( const std::pair<const int, body_draw> &e : body_draws() ) {
        if( now - e.second.stamp >= 250 ) {
            continue;
        }
        if( e.first == own && own_here && e.second.tile.z() == p.z() ) {
            const double ahead_ms = static_cast<double>( std::clamp<ms_t>( now - e.second.stamp, 0, 40 ) );
            offset o;
            o.x = std::clamp( e.second.tile.x() + e.second.off.x + e.second.vx * ahead_ms - p.x(), -1.5, 1.5 );
            o.y = std::clamp( e.second.tile.y() + e.second.off.y + e.second.vy * ahead_ms - p.y(), -1.5, 1.5 );
            return o;
        }
        if( e.second.tile == p && e.first != own ) {
            return e.second.off;
        }
    }
    return std::nullopt;
}
} // namespace

offset sprite_offset( const tripoint_abs_ms &p )
{
    if( slides().empty() && lunges().empty() && hits().empty() && !pred().active &&
        body_draws().empty() ) {
        return offset();
    }
    const ms_t now = now_ms();
    offset a;
    if( const std::optional<offset> b = body_draw_at( p, now ) ) {
        a = *b;
    } else {
        a = travel_offset_at( p, now );
    }
    const offset b = lunge_offset_at( p, now );
    const offset h = hit_offset_at( p, now );
    a.x += b.x + h.x;
    a.y += b.y + h.y;
    return a;
}

namespace
{
// how far the smoothed camera still is from the character (tiles): it is animating until it
// has caught up
double camera_follow_gap = 0.0;
} // namespace

offset camera_offset()
{
    if( !smooth_camera() ) {
        return offset();
    }
    const ms_t now = now_ms();
    offset o;
    if( tune().camera_follows_slide ) {
        const tripoint_abs_ms at = get_avatar().pos_abs();
        offset target;
        if( const std::optional<offset> b = body_draw_at( at, now ) ) {
            target = *b;
        } else {
            target = travel_offset_at( at, now );
        }
        // the camera follows a smoothed position in world coordinates: a tile change and the
        // offset that goes with it may reach the renderer a frame apart, and the game snaps the
        // character when an action ends; neither may jerk the view (Noah's test, 2026-10-15)
        struct follow {
            bool valid = false;
            double x = 0.0;
            double y = 0.0;
            int z = 0;
            ms_t last = 0;
        };
        static follow f;
        const double wx = at.x() + 0.5 + target.x;
        const double wy = at.y() + 0.5 + target.y;
        const double dt = static_cast<double>( std::clamp<ms_t>( now - f.last, 0, 100 ) );
        f.last = now;
        if( !f.valid || f.z != at.z() || std::abs( wx - f.x ) > 3.0 || std::abs( wy - f.y ) > 3.0 ) {
            // a level change, stairs or a teleport: no glide over the whole map
            f.valid = true;
            f.x = wx;
            f.y = wy;
            f.z = at.z();
        } else {
            constexpr double time_constant_ms = 60.0;
            const double k = 1.0 - std::exp( -dt / time_constant_ms );
            f.x += ( wx - f.x ) * k;
            f.y += ( wy - f.y ) * k;
        }
        o.x = f.x - ( at.x() + 0.5 );
        o.y = f.y - ( at.y() + 0.5 );
        camera_follow_gap = std::abs( wx - f.x ) + std::abs( wy - f.y );
    }
    const offset lead = camera_lead_at( now );
    o.x += lead.x;
    o.y += lead.y;
    return o;
}

bool animating()
{
    if( !slides().empty() || !lunges().empty() || !hits().empty() || !tracer_list().empty() || !aim_line_list().empty() ||
        !boom_list().empty() || !flourish_list().empty() || pred().active ||
        aim_view_rw().active ) {
        return true;
    }
    const ms_t now = now_ms();
    for( const std::pair<const int, body_draw> &e : body_draws() ) {
        if( e.second.moving && now - e.second.stamp < 250 ) {
            return true;
        }
    }
    if( camera_follow_gap > 0.003 ) {
        return true;
    }
    // the camera is still catching up with the cursor
    if( smooth_camera() ) {
        const camera_lead_state &cs = lead_state();
        const offset target = camera_lead_target( now_ms() );
        return std::abs( target.x - cs.x ) > 0.003 || std::abs( target.y - cs.y ) > 0.003;
    }
    return false;
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

void request_autoattack( actor_state &st )
{
    st.autoattack_until = now_ms() + tune().buffer_ms;
}

void forget_state( int character_id )
{
    remote_states().erase( character_id );
    body_draws().erase( character_id );
}

// ---------------------------------------------------------------------------------------
// the driver

void drive( Character &who, actor_state &st, const input_frame &in, ms_t now,
            std::vector<event> *out, const std::vector<move_frame> *moves )
{
    if( who.is_dead_state() ) {
        return;
    }
    // B6: a sleeping remote player wakes up by moving
    if( !who.is_avatar() && who.in_sleep_state() &&
        ( in.move_x != 0 || in.move_y != 0 || in.primary || in.cancel ) ) {
        who.wake_up();
    }
    // B7: moving stops a timed action (crafting, eating ...) at once; SPACE during one
    // answers the focus prompt (B3/B5) instead
    const bool cancel_pressed = in.cancel && !st.prev_cancel;
    st.prev_cancel = in.cancel;
    if( who.activity && cancel_pressed ) {
        focus_minigame::press( who );
    }
    // reloading goes on while walking freely (Noah 2026-10-15); the steps use the moves the
    // reload would have used, so it takes longer
    static const activity_id act_reload_free( "ACT_RELOAD" );
    const bool reloading = who.activity && who.activity.id() == act_reload_free &&
                           free_movement() && !who.is_running();
    if( who.activity && who.activity.is_interruptible_with_kb() && !reloading &&
        ( in.move_x != 0 || in.move_y != 0 ) ) {
        who.add_msg_if_player( m_alert, _( "You stop %s." ), who.activity.get_verb().translated() );
        who.cancel_activity();
    }
    const tuning &tn = tune();
    const bool free = free_movement();
    const int body_dt = free ? body_sync( who, st, now ) : 0;
    st.body_moving = false;
    // the body's free movement this frame: a remote player's frames one by one (each with its
    // own direction and duration, like the client predicted them), else the frame time
    const bool wants_move = moves != nullptr ? std::any_of( moves->begin(), moves->end(),
    []( const move_frame & f ) {
        return f.move_x != 0 || f.move_y != 0;
    } ) : ( in.move_x != 0 || in.move_y != 0 );
    const auto move_body = [&]() {
        if( moves == nullptr ) {
            return free_move( who, st, in, body_dt, now, out );
        }
        bool moved = false;
        for( const move_frame &f : *moves ) {
            if( f.move_x == 0 && f.move_y == 0 ) {
                continue;
            }
            if( st.frame_wait_ms > 0 ) {
                // opening a door: these frames pass standing (as on the client)
                st.frame_wait_ms -= f.dt;
                continue;
            }
            input_frame step = in;
            step.move_x = f.move_x;
            step.move_y = f.move_y;
            step.sprint = f.sprint;
            // walk or run as this frame says (the client predicted it so); not the newest
            // input's sprint key, which may belong to a later frame
            if( f.sprint && !who.is_running() && who.can_run() ) {
                who.set_movement_mode( move_mode_run );
            } else if( who.is_running() && ( !f.sprint || !who.can_run() ) ) {
                who.set_movement_mode( move_mode_walk );
            }
            st.prev_sprint = f.sprint;
            if( f.dodge || now <= st.dodge_buffered_until ) {
                start_dodge( who, st, now, false, f.move_x, f.move_y, out );
            }
            const ms_t ready_before = st.ready_at;
            const tripoint_abs_ms tile_before = who.pos_abs();
            const bool frame_moved = free_move( who, st, step, f.dt, now, out );
            moved = frame_moved || moved;
            if( !frame_moved && who.pos_abs() == tile_before && st.ready_at > now &&
                st.ready_at != ready_before && body_door_opened( st, step ) ) {
                // the wait of the opened door, in frame time instead of the host's clock
                st.frame_wait_ms = static_cast<double>( st.ready_at - now );
                st.ready_at = now;
            }
            if( trace_enabled() ) {
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace srvframe seq=" << f.seq << " dt=" << f.dt <<
                                           " x=" << st.body_x << " y=" << st.body_y << " tile=" << who.pos_abs().y();
            }
        }
        return moved;
    };
    if( !st.initialised ) {
        st.initialised = true;
        st.seen_primary_presses = in.primary_presses;
        st.seen_secondary_presses = in.secondary_presses;
        st.seen_throw_presses = in.throw_presses;
        st.seen_shove_presses = in.shove_presses;
        st.prev_primary = in.primary;
        st.prev_secondary = in.secondary;
        st.aim_last = now;
    }
    const bool throw_pressed = in.throw_presses != st.seen_throw_presses;
    st.seen_throw_presses = in.throw_presses;
    const bool shove_pressed = in.shove_presses != st.seen_shove_presses;
    st.seen_shove_presses = in.shove_presses;
    const bool primary_pressed = in.primary_presses != st.seen_primary_presses ||
                                 ( in.primary && !st.prev_primary );
    const bool secondary_pressed = in.secondary_presses != st.seen_secondary_presses ||
                                   ( in.secondary && !st.prev_secondary );
    st.seen_primary_presses = in.primary_presses;
    st.seen_secondary_presses = in.secondary_presses;
    st.prev_primary = in.primary;
    st.prev_secondary = in.secondary;

    // ---- hold to sprint (B4); back to walking on release or when out of stamina
    // level based: holding the key while winded starts the sprint once stamina allows it
    if( in.sprint && !who.is_running() && who.can_run() ) {
        who.set_movement_mode( move_mode_run );
    } else if( who.is_running() && ( ( !in.sprint && st.prev_sprint ) || !who.can_run() ) ) {
        who.set_movement_mode( move_mode_walk );
    }
    st.prev_sprint = in.sprint;

    const tripoint_abs_ms pos = who.pos_abs();
    const bool have_cursor = in.cursor_valid && in.cursor_z == pos.z();
    // aim from where the body really is
    const double origin_x = free ? st.body_x : pos.x() + 0.5;
    const double origin_y = free ? st.body_y : pos.y() + 0.5;
    const double cdx = have_cursor ? in.cursor_x - origin_x : 0.0;
    const double cdy = have_cursor ? in.cursor_y - origin_y : 0.0;
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
    if( trace_enabled() && moves != nullptr && !moves->empty() && !idle ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace frames skipped n=" << moves->size() << " moves=" <<
                                   who.get_moves() << " activity=" << static_cast<bool>( who.activity ) << " melee=" <<
                                   static_cast<int>( st.melee ) << " ready_in=" << st.ready_at - now;
    }
    if( !idle && primary_pressed ) {
        // too early (walking, recovering from a blow ...): the click is remembered and used
        // the moment the actor is ready
        st.buffered_until = now + tn.buffer_ms;
        st.buffered_is_attack = true;
    }

    if( in.move_x != 0 || in.move_y != 0 ) {
        st.moved_at = now;
    }
    // ---- aim and recoil (every frame, also when the character can not act)
    update_aim( who, st, in, now, idle );

    // ---- new actions
    if( idle ) {
        // chained actions start when the previous one ended, not when this frame happened to run
        const ms_t t0 = ( now - st.ready_at < 50 && st.ready_at > 0 ) ? st.ready_at : now;
        bool started = false;

        if( shove_pressed ) {
            const std::pair<double, double> sd = have_dir ? std::make_pair( cdx, cdy ) :
                                                 std::make_pair( static_cast<double>( st.face_dx ), static_cast<double>( st.face_dy ) );
            const int dur = shove( who, st, sd.first, sd.second, t0, out );
            if( dur > 0 ) {
                st.ready_at = t0 + dur;
                started = true;
            }
        }

        if( !started && throw_pressed ) {
            const int dur = throw_at_cursor( who, in, now, out );
            if( dur > 0 ) {
                st.ready_at = t0 + dur;
                started = true;
            }
        }

        if( !started && st.autoattack_until > now ) {
            st.autoattack_until = 0;
            started = autoattack( who, st, t0, out );
        }

        if( started ) {
            // the throw / the autoattack is the action of this frame
        } else if( !armed_with_gun ) {
            const bool want_attack = primary_pressed || ( st.buffered_until > now && st.buffered_is_attack );
            const bool hold_attack = in.primary && !primary_pressed;
            if( want_attack || hold_attack ) {
                std::pair<int, int> dir = have_dir ? snap8( cdx, cdy ) : std::make_pair( st.face_dx, st.face_dy );
                if( dir.first == 0 && dir.second == 0 ) {
                    dir = { st.face_dx, st.face_dy };
                }
                // M-C: the click hits the tile of the cursor's 45 degree sector (no assist cone)
                const bool sectors = tn.melee_sectors && have_cursor &&
                                     cursor_sector_dir( who, cdx, cdy, dir );
                if( want_attack && have_cursor && trace_enabled() ) {
                    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace cursor id=" << who.getID().get_value() <<
                                               " t=" << now << " cdx=" << cdx << " cdy=" << cdy <<
                                               " sector=" << dir.first << "," << dir.second <<
                                               " sectors=" << sectors;
                }
                std::pair<int, int> assisted = dir;
                Creature *tgt = target_at( who, pos + tripoint_rel_ms( dir.first, dir.second, 0 ) );
                if( tgt == nullptr && !sectors ) {
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

        if( !started && free && wants_move ) {
            st.body_moving = move_body();
            if( st.body_moving && who.is_avatar() ) {
                // a tap has been used: the input resolver lets go of it
                note_own_step_started();
            }
        } else if( !started && !free && ( in.move_x != 0 || in.move_y != 0 ) ) {
            // (tile steps only without free movement: a frame without movement frames of a
            // remote player is a frame without movement)
            // While walking continuously the next step starts exactly when the last one ended,
            // even if the world turn in between took longer than a frame: no lost time (and
            // no pause on the tile) after a hitch.
            const ms_t step_t0 = ( st.stepping && st.ready_at > 0 && now - st.ready_at < tn.chain_ms ) ?
                                 st.ready_at : t0;
            const tripoint_abs_ms before = who.pos_abs();
            started = start_step( who, st, in.move_x, in.move_y, now, step_t0, out );
            st.stepping = started && who.pos_abs() != before;
        } else {
            st.stepping = false;
        }
    } else if( reloading && free && wants_move &&
               !who.is_dead_state() && now >= st.ready_at ) {
        st.body_moving = move_body();
        if( st.body_moving && who.is_avatar() ) {
            note_own_step_started();
        }
    } else if( in.move_x == 0 && in.move_y == 0 ) {
        st.stepping = false;
    }

    if( free ) {
        body_publish( who, st, now );
    }
    // ---- what the overlays show (local avatar only; set by host_frame)
}

// ---------------------------------------------------------------------------------------
// the turn clock

void reset_clock()
{
    clk().valid = false;
}

void reset_session()
{
    // a new game or multiplayer session in the same run: nothing of the last one carries over
    reset_clock();
    slides().clear();
    lunges().clear();
    hits().clear();
    boom_list().clear();
    aim_line_list().clear();
    tracer_list().clear();
    aim_view_rw() = aim_view();
    melee_view_rw() = melee_view();
    pred() = prediction();
    cinfo() = client_info();
    lead_state() = camera_lead_state();
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
    // the real time between world turns (smoothed): a turn can take longer than the turn
    // length option (a busy host); glides that assume the option end early and the creature
    // stands until its next step - stop and go instead of walking (Noah's test, 2026-10-16)
    static ms_t last_turn_at = 0;
    static double turn_interval = 0.0;
    if( last_turn_at > 0 ) {
        const double gap = static_cast<double>( std::clamp<ms_t>( now - last_turn_at, 50, 5000 ) );
        turn_interval = turn_interval <= 0.0 ? gap : turn_interval * 0.8 + gap * 0.2;
    }
    last_turn_at = now;
    const int real_turn_ms = std::max( turn_ms(), static_cast<int>( turn_interval ) );
    static ms_t last_turn_log = 0;
    if( trace_enabled() && now - last_turn_log > 5000 ) {
        last_turn_log = now;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace world turn every " << turn_interval <<
                                   " ms (option " << turn_ms() << ")";
    }
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
        // phase 6: the creature glides for as long as the steps take at its own speed (a slow
        // zombie does not dash and then wait), continuing from where its sprite is now
        const int speed = std::max( 1, c.get_speed() );
        // steps come on whole turns: a creature slower than one tile per turn steps on some
        // turns and not on others (speed 70: 7 of 10), so a glide of exactly the average step
        // time ends before a late step and the creature stands still for a moment.  Half a turn
        // more lets the next step arrive while the glide still runs: the glides join into a walk
        const int dur = std::clamp( dist * 100 * real_turn_ms / speed + real_turn_ms / 2, real_turn_ms / 2,
                                    std::max( tune().slide_max_ms, 3 * real_turn_ms ) );
        begin_slide( from, to, now, dur );
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
// monster melee with a wind-up (phase 6, Noah 2026-10-15): the blow lands a moment after the
// monster commits to it, and only if the player is still in reach then: stepping back dodges.

namespace
{
struct monster_windup {
    weak_ptr_fast<monster> attacker;
    int target_id = 0;
    bool target_is_avatar = false;
    ms_t land_at = 0;
};

// realtime combat rebalance (Noah 2026-10-16): until when a monster may not wind up its next
// blow (recovery after a blow, a stagger)
std::map<const monster *, ms_t> &monster_recover_until()
{
    static std::map<const monster *, ms_t> m;
    return m;
}

std::vector<monster_windup> &monster_windups()
{
    static std::vector<monster_windup> v;
    return v;
}

// where a player really is: the free body, else the tile centre
std::pair<double, double> player_body_pos( const Character &who )
{
    const actor_state &st = who.is_avatar() ? avatar_state() : state_of( who.getID().get_value() );
    if( free_movement() && st.body_valid && st.body_tile == who.pos_abs() ) {
        return { st.body_x, st.body_y };
    }
    return { who.pos_abs().x() + 0.5, who.pos_abs().y() + 0.5 };
}

Character *find_player( const monster_windup &w )
{
    if( w.target_is_avatar ) {
        return &get_avatar();
    }
    return g->find_npc( character_id( w.target_id ) );
}
} // namespace

bool monster_winding_up( const monster &z )
{
    for( const monster_windup &w : monster_windups() ) {
        if( const shared_ptr_fast<monster> m = w.attacker.lock() ) {
            if( m.get() == &z ) {
                return true;
            }
        }
    }
    return false;
}

void stagger_monster( monster &z, int ms, ms_t now )
{
    std::vector<monster_windup> &v = monster_windups();
    for( auto iter = v.begin(); iter != v.end(); ) {
        const shared_ptr_fast<monster> m = iter->attacker.lock();
        if( !m || m.get() == &z ) {
            iter = v.erase( iter );
        } else {
            ++iter;
        }
    }
    end_lunge( z.pos_abs() );
    ms_t &until = monster_recover_until()[&z];
    until = std::max( until, now + ms );
    // everybody sees it: the red warning goes at once, a yellow burst on the monster
    event cancel;
    cancel.type = fx_type::cancel;
    cancel.from = z.pos_abs();
    net::host_send_events( { cancel, flourish_event( flourish_kind::stagger, z.pos_abs().x() + 0.5,
                                    z.pos_abs().y() + 0.5, z.posz(), 0, 0, z.pos_abs(), 0 )
                           } );
}

bool monster_melee_windup( monster &z, Character &target )
{
    if( !enabled() ) {
        return false;
    }
    std::vector<monster_windup> &v = monster_windups();
    for( const monster_windup &w : v ) {
        if( const shared_ptr_fast<monster> m = w.attacker.lock() ) {
            if( m.get() == &z ) {
                // still winding up: this turn's attack is that blow
                z.mod_moves( -z.type->attack_cost );
                return true;
            }
        }
    }
    const ms_t now = now_ms();
    // recovering from its last blow or staggered: this turn passes without a blow
    const auto rec = monster_recover_until().find( &z );
    if( rec != monster_recover_until().end() && now < rec->second ) {
        z.mod_moves( -z.type->attack_cost );
        return true;
    }
    // attack slots: only so many monsters wind up on the same player at once, the others wait
    const int slots = get_option<int>( "MP_ATTACK_SLOTS" );
    if( slots > 0 ) {
        int busy = 0;
        for( const monster_windup &w : v ) {
            if( w.target_id == target.getID().get_value() && !w.attacker.expired() ) {
                ++busy;
            }
        }
        if( busy >= slots ) {
            if( trace_enabled() ) {
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace combat slot wait t=" << now;
            }
            z.mod_moves( -z.type->attack_cost );
            return true;
        }
    }
    const int attack_ms = z.type->attack_cost * turn_ms() / std::max( 1, z.get_speed() );
    // a readable warning: at least the option's time, slow attacks up to 350 ms longer
    const int windup_min = get_option<int>( "MP_MONSTER_WINDUP_MS" );
    const int windup = std::clamp( attack_ms * 7 / 6, windup_min, windup_min + 350 );
    if( trace_enabled() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace combat windup t=" << now << " ms=" << windup;
    }
    // the first blow coming at a player in this session: one line on how to get out of it
    static std::set<int> told;
    if( get_option<bool>( "MP_DODGE" ) && told.insert( target.getID().get_value() ).second ) {
        net::tell_player( target, _( "Tip: press Space or tap a direction twice to dodge; blows miss you while you dash." ),
                          true );
    }
    monster_windup w;
    w.attacker = g->shared_from( z );
    w.target_is_avatar = target.is_avatar();
    w.target_id = target.getID().get_value();
    w.land_at = now + windup;
    v.push_back( w );
    z.mod_moves( -z.type->attack_cost );
    // the wind-up is seen: a lunge toward the target
    const tripoint_abs_ms from = z.pos_abs();
    const int dx = std::clamp( target.pos_abs().x() - from.x(), -1, 1 );
    const int dy = std::clamp( target.pos_abs().y() - from.y(), -1, 1 );
    begin_lunge( from, dx, dy, now, windup, windup + 150 );
    event ev;
    ev.type = fx_type::swing;
    ev.actor = 0;
    ev.from = from;
    ev.dx = dx;
    ev.dy = dy;
    ev.dur_ms = windup + 150;
    ev.windup_ms = windup;
    net::host_send_events( { ev } );
    return true;
}

void process_monster_windups( ms_t now )
{
    std::vector<monster_windup> &v = monster_windups();
    // recovery times that are over are forgotten (a dead monster's address may be reused)
    for( auto iter = monster_recover_until().begin(); iter != monster_recover_until().end(); ) {
        iter = now >= iter->second ? monster_recover_until().erase( iter ) : std::next( iter );
    }
    for( auto iter = v.begin(); iter != v.end(); ) {
        if( now < iter->land_at ) {
            ++iter;
            continue;
        }
        const monster_windup w = *iter;
        iter = v.erase( iter );
        const shared_ptr_fast<monster> z = w.attacker.lock();
        if( z ) {
            // blow or not, the monster recovers before the next one
            ms_t &until = monster_recover_until()[z.get()];
            until = std::max( until, now + get_option<int>( "MP_MONSTER_RECOVERY_MS" ) );
        }
        Character *target = find_player( w );
        if( !z || z->is_dead() || target == nullptr || target->is_dead_state() ||
            z->has_effect( effect_stunned_fluid ) ) {
            continue;
        }
        // dodging: the blow misses
        const actor_state &tst = w.target_is_avatar ? avatar_state() : state_of( w.target_id );
        if( now < tst.invuln_until ) {
            // the blow passes through the dash: a white ring on the player, no words
            net::host_send_events( { flourish_event( flourish_kind::evade, target->pos_abs().x() + 0.5,
                                     target->pos_abs().y() + 0.5, target->posz(), 0, 0, target->pos_abs(),
                                     target->getID().get_value() )
                                   } );
            continue;
        }
        // in reach when the blow lands: body to body, one tile plus the bodies' width
        const std::pair<double, double> body = player_body_pos( *target );
        const double mx = z->pos_abs().x() + 0.5;
        const double my = z->pos_abs().y() + 0.5;
        const double dist = std::hypot( body.first - mx, body.second - my );
        if( dist > 1.0 + 2.0 * tune().body_half || !z->is_adjacent( target, true ) ) {
            target->add_msg_if_player( m_good, _( "You step out of the %s's reach." ), z->get_name() );
            continue;
        }
        // the moves were spent when it committed to the blow
        z->mod_moves( z->type->attack_cost );
        const int hp_before = target->get_hp();
        z->melee_attack( *target );
        if( trace_enabled() ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace combat blow t=" << now << " damage=" <<
                                       hp_before - target->get_hp();
        }
        if( target->get_hp() < hp_before ) {
            event ev;
            ev.type = fx_type::strike;
            ev.from = z->pos_abs();
            ev.to = target->pos_abs();
            ev.dx = std::clamp( ev.to.x() - ev.from.x(), -1, 1 );
            ev.dy = std::clamp( ev.to.y() - ev.from.y(), -1, 1 );
            ev.hit = true;
            register_hit( ev.from, ev.to, ev.dx, ev.dy, now );
            net::host_send_events( { ev } );
        }
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
    process_monster_windups( now );

    input_frame in;
    const bool device = poll_device( in );
    if( device ) {
        note_cursor( in, now );
        // The classic input of vehicles, menus ... is left alone while they are in use.
        const bool fluid_owns_input = !u.controlling_vehicle && !u.is_dead_state() &&
                                      g->uquit != QUIT_WATCH && !net::host_avatar_downed();
        if( fluid_owns_input && !net::dedicated() ) {
            drive( u, avatar_state(), in, now, &events );
        } else {
            // driving a vehicle, dead ...: drawn on its tile; the body starts afresh later
            body_draws().erase( u.getID().get_value() );
            avatar_state().body_valid = false;
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
        events.insert( events.end(), pending_tracers().begin(), pending_tracers().end() );
    }
    pending_tracers().clear();
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
        case ACTION_THROW:
        case ACTION_THROW_WIELDED:
        case ACTION_FIRE:
            note_classic_action( act );
            return true;
        case ACTION_CLICK_AND_DRAG:
            // a held mouse button: the device state already has it (aim / attack)
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
            if( ev.hit && ev.to != ev.from ) {
                register_hit( ev.from, ev.to, ev.dx, ev.dy, arrival );
            }
            break;
        case fx_type::tracer:
            add_tracer( ev.from, ev.to, arrival );
            break;
        case fx_type::aimline:
            add_aim_line( ev.from, ev.to, arrival, ev.dur_ms );
            break;
        case fx_type::boom: {
            boom_fx b;
            b.at = ev.from;
            b.radius = std::max( 1, ev.dx );
            b.t0 = arrival;
            boom_list().push_back( b );
            break;
        }
        case fx_type::flourish:
            add_flourish( static_cast<flourish_kind>( ev.windup_ms ), ev.from.x() + ev.steadiness / 1000.0,
                          ev.from.y() + ev.spread / 1000.0, ev.from.z(), ev.dx / 1000.0, ev.dy / 1000.0, ev.to,
                          arrival );
            break;
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

// ---- free movement over the network (phase 4 of doc/MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md)
// The host sends every body's exact position ~20 times a second.  The own body is predicted
// with the same movement maths on the client's copy of the map and corrected against the
// prediction made for the input frame the host had applied; other players are drawn
// interp_delay_ms in the past, between two positions from the host.

namespace
{
const efftype_id effect_downed_free( "downed" );
int interp_delay_ms()
{
    return has_option( "FLUID_INTERP_MS" ) ? get_option<int>( "FLUID_INTERP_MS" ) : 100;
}

struct remote_body {
    // arrival time, x, y, z
    std::deque<std::tuple<ms_t, double, double, int>> samples;
    tripoint_abs_ms placed = tripoint_abs_ms::invalid;
};

struct free_client {
    bool on = false;
    actor_state st;
    ms_t last = 0;
    // movement frame number, the predicted body position right after the frame, and right
    // before it
    std::deque<std::tuple<int, double, double, double, double>> history;
    std::map<int, remote_body> others;
};

free_client &fclient()
{
    static free_client f;
    return f;
}

void body_place( actor_state &st, double x, double y, int z )
{
    st.body_valid = true;
    st.body_x = x;
    st.body_y = y;
    st.body_tile = tripoint_abs_ms( floor_int( x ), floor_int( y ), z );
    st.corr_ms = 0;
}

// The host's free_move without the native moves: the body slides and collides on the
// client's copy of the map; crossing into another tile is only noted (the host moves the
// character, and its step event places the avatar).
void predict_free_move( const Character &who, actor_state &st, const input_frame &in, int dt )
{
    const tuning &tn = tune();
    const double len = std::sqrt( static_cast<double>( in.move_x * in.move_x + in.move_y * in.move_y ) );
    if( len < 0.5 || dt <= 0 ) {
        return;
    }
    const double ux = in.move_x / len;
    const double uy = in.move_y / len;
    const tripoint_abs_ms ahead = st.body_tile + tripoint_rel_ms( in.move_x, in.move_y, 0 );
    int cost = body_step_cost( who, st.body_tile, ahead );
    // this frame runs if its sprint key is held and there is stamina, as on the host (which
    // sets the mode per movement frame); the local copy's mode follows the host's state later:
    // running doubles the speed (move_modes.json)
    const bool want_run = in.sprint && who.can_run();
    if( want_run && !who.is_running() ) {
        cost = std::max( 1, cost / 2 );
    } else if( !want_run && who.is_running() ) {
        cost *= 2;
    }
    const int ms_per_tile = std::max( 1, moves_to_ms( who, cost ) );
    // a dodge dashes: three times the distance while its dash time lasts (frame time)
    const double dash = std::min<double>( dt, std::max( 0.0, st.dash_left_ms ) );
    st.dash_left_ms = std::max( 0.0, st.dash_left_ms - dt );
    double left = ( static_cast<double>( dt ) + 2.0 * dash ) / ms_per_tile;
    std::vector<int> cells;
    while( left > 1e-9 ) {
        const double step = std::min( left, tn.max_substep );
        left -= step;
        const double bx = st.body_x;
        const double by = st.body_y;
        if( ux != 0.0 && !body_move_axis( who, st, 0, ux * step, cells ) && uy == 0.0 ) {
            body_corner_nudge( who, st, 0, cells, step );
        }
        if( uy != 0.0 && !body_move_axis( who, st, 1, uy * step, cells ) && ux == 0.0 ) {
            body_corner_nudge( who, st, 1, cells, step );
        }
        st.body_tile = tripoint_abs_ms( floor_int( st.body_x ), floor_int( st.body_y ), st.body_tile.z() );
        if( std::abs( st.body_x - bx ) + std::abs( st.body_y - by ) < 1e-9 ) {
            break;
        }
    }
}

// The predicted body is blocked by a closed door the host opens when the body pushes into it
// (free_move -> start_step, as a vanilla step: the door opens, the step costs a turn's moves,
// nobody moves): the same here, so that the prediction does not stop at the door until the
// host's map arrives and then rush ahead of the body.  The host's map update confirms the door.
void predict_door_open( avatar &u, actor_state &st, const input_frame &in )
{
    if( in.move_x == 0 && in.move_y == 0 ) {
        return;
    }
    map &here = get_map();
    const tripoint_abs_ms ahead = st.body_tile + tripoint_rel_ms( in.move_x, in.move_y, 0 );
    if( !here.inbounds( ahead ) ) {
        return;
    }
    const tripoint_bub_ms b = here.get_bub( ahead );
    if( here.passable( b ) || here.veh_at( b ) ||
        get_creature_tracker().creature_at( b ) != nullptr ) {
        return;
    }
    const bool inside = !here.is_outside( u.pos_bub( here ) );
    // (check only: a locked door or one that opens from the other side stays shut)
    if( !here.open_door( u, b, inside, true ) ) {
        return;
    }
    // only the map: the sound and the message come from the host
    if( !here.ter( b ).obj().open.is_null() ) {
        here.ter_set( b, here.ter( b ).obj().open );
    } else if( !here.furn( b ).obj().open.is_null() ) {
        here.furn_set( b, here.furn( b ).obj().open );
    }
    // vanilla charges a turn's moves for opening (avatar_action::move); the host's step the same
    st.frame_wait_ms = std::max( 40, moves_to_ms( u, u.get_speed() ) );
    if( trace_enabled() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace predicted door wait=" << st.frame_wait_ms;
    }
}

void client_reconcile( free_client &fc, const body_sample &b, int seq, ms_t now )
{
    actor_state &st = fc.st;
    if( !st.body_valid || b.z != st.body_tile.z() ) {
        body_place( st, b.x, b.y, b.z );
        fc.history.clear();
        return;
    }
    while( !fc.history.empty() && std::get<0>( fc.history.front() ) < seq ) {
        fc.history.pop_front();
    }
    double px = st.body_x;
    double py = st.body_y;
    if( !fc.history.empty() && std::get<0>( fc.history.front() ) == seq ) {
        px = std::get<1>( fc.history.front() );
        py = std::get<2>( fc.history.front() );
    } else if( !fc.history.empty() ) {
        // the host has not applied any of the unconfirmed frames yet: it is where the body was
        // before the first of them
        px = std::get<3>( fc.history.front() );
        py = std::get<4>( fc.history.front() );
    }
    const double ex = b.x - px;
    const double ey = b.y - py;
    if( std::abs( ex ) + std::abs( ey ) < 0.02 ) {
        return;
    }
    // for play tests: big corrections are worth knowing about (rubber banding)
    static ms_t last_log = 0;
    const double err = std::sqrt( ex * ex + ey * ey );
    if( trace_enabled() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace correction err=" << err << " ex=" << ex << " ey=" << ey <<
                                   " seq=" << seq << " unconfirmed=" << fc.history.size();
    }
    if( err > 0.3 && now - last_log > 1000 ) {
        last_log = now;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: free movement correction " << err <<
                                   " tiles (input " << seq << ", " << fc.history.size() << " frames unconfirmed)";
    }
    if( std::abs( ex ) > 2.0 || std::abs( ey ) > 2.0 ) {
        // far off (stairs, a fall, a teleport): jump there
        body_place( st, st.body_x + ex, st.body_y + ey, b.z );
        fc.history.clear();
        return;
    }
    // shift the prediction by the error; the sprite eases over from where it was drawn
    const double old_x = st.body_x;
    const double old_y = st.body_y;
    st.body_x += ex;
    st.body_y += ey;
    st.body_tile = tripoint_abs_ms( floor_int( st.body_x ), floor_int( st.body_y ), b.z );
    for( std::tuple<int, double, double, double, double> &h : fc.history ) {
        std::get<1>( h ) += ex;
        std::get<2>( h ) += ey;
        std::get<3>( h ) += ex;
        std::get<4>( h ) += ey;
    }
    body_set_correction( st, old_x, old_y, now, 100 );
}

// Client, every frame: the own body moves on, other bodies are interpolated; both go to the
// renderer through body_draws().
void client_free_frame( const input_frame &in, int seq, int dt, ms_t now )
{
    free_client &fc = fclient();
    client_info &ci = cinfo();
    avatar &u = get_avatar();
    const tripoint_abs_ms tile = u.pos_abs();
    actor_state &st = fc.st;
    if( !st.body_valid || st.body_tile.z() != tile.z() ||
        std::abs( st.body_x - ( tile.x() + 0.5 ) ) > 2.5 || std::abs( st.body_y - ( tile.y() + 0.5 ) ) > 2.5 ) {
        body_place( st, tile.x() + 0.5, tile.y() + 0.5, tile.z() );
        fc.history.clear();
    }
    // where the body was before this frame (the host's answer may come before it applied it)
    const double before_x = st.body_x;
    const double before_y = st.body_y;
    if( u.controlling_vehicle || u.in_vehicle ) {
        // riding: the vehicle moves the character, the body stays on its tile
        body_place( st, tile.x() + 0.5, tile.y() + 0.5, tile.z() );
    } else if( u.grab_point != tripoint_rel_ms::zero ) {
        // dragging moves in tile steps on the host: follow them, do not predict
        body_place( st, tile.x() + 0.5, tile.y() + 0.5, tile.z() );
    } else if( now >= ci.busy_until && !u.has_effect( effect_downed_free ) && !u.in_sleep_state() ) {
        // (a downed or sleeping body does not move on the host: predicting it would only be
        // corrected back again and again)
        if( ( in.dodge || now <= st.dodge_buffered_until ) && seq >= 0 ) {
            // the same dodge the host starts with this frame
            start_dodge( u, st, now, true, in.move_x, in.move_y, nullptr );
        }
        if( st.frame_wait_ms > 0 && ( in.move_x != 0 || in.move_y != 0 ) && seq >= 0 ) {
            // opening a door: this movement frame passes standing, as on the host
            st.frame_wait_ms -= dt;
        } else {
            const double px = st.body_x;
            const double py = st.body_y;
            predict_free_move( u, st, in, dt );
            if( std::abs( st.body_x - px ) + std::abs( st.body_y - py ) < 1e-9 && seq >= 0 ) {
                predict_door_open( u, st, in );
            }
        }
    }
    if( seq >= 0 ) {
        if( trace_enabled() ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace cliframe seq=" << seq << " dt=" << dt <<
                                       " x=" << st.body_x << " y=" << st.body_y;
        }
        fc.history.emplace_back( seq, st.body_x, st.body_y, before_x, before_y );
        while( fc.history.size() > 120 ) {
            fc.history.pop_front();
        }
    }
    {
        body_draw &d = body_draws()[ci.my_id];
        d.vx = dt > 0 ? ( st.body_x - before_x ) / dt : 0.0;
        d.vy = dt > 0 ? ( st.body_y - before_y ) / dt : 0.0;
        d.tile = tile;
        const offset c = body_correction_at( st, now );
        d.off.x = std::clamp( st.body_x - ( tile.x() + 0.5 ) + c.x, -1.5, 1.5 );
        d.off.y = std::clamp( st.body_y - ( tile.y() + 0.5 ) + c.y, -1.5, 1.5 );
        d.moving = true;
        d.stamp = now;
    }
    const ms_t render_t = now - interp_delay_ms();
    for( std::pair<const int, remote_body> &e : fc.others ) {
        std::deque<std::tuple<ms_t, double, double, int>> &q = e.second.samples;
        if( q.empty() || now - std::get<0>( q.back() ) > 1000 ) {
            continue;
        }
        while( q.size() >= 2 && std::get<0>( q[1] ) <= render_t ) {
            q.pop_front();
        }
        double x = std::get<1>( q.front() );
        double y = std::get<2>( q.front() );
        const int z = std::get<3>( q.front() );
        if( q.size() >= 2 && std::get<0>( q.front() ) <= render_t && std::get<3>( q[1] ) == z ) {
            const ms_t t0 = std::get<0>( q.front() );
            const ms_t t1 = std::get<0>( q[1] );
            const double f = t1 > t0 ? std::clamp( static_cast<double>( render_t - t0 ) / ( t1 - t0 ), 0.0,
                                                   1.0 ) : 1.0;
            x += ( std::get<1>( q[1] ) - x ) * f;
            y += ( std::get<2>( q[1] ) - y ) * f;
        }
        const tripoint_abs_ms at( floor_int( x ), floor_int( y ), z );
        if( at != e.second.placed ) {
            e.second.placed = at;
            net::client_place_shadow( e.first, at );
        }
        body_draw &d = body_draws()[e.first];
        d.tile = at;
        d.off.x = x - ( at.x() + 0.5 );
        d.off.y = y - ( at.y() + 0.5 );
        d.moving = true;
        d.stamp = now;
    }
}
} // namespace

std::vector<body_sample> host_body_samples()
{
    std::vector<body_sample> out;
    const ms_t now = now_ms();
    for( const std::pair<const int, body_draw> &e : body_draws() ) {
        if( now - e.second.stamp > 250 ) {
            continue;
        }
        body_sample b;
        b.actor = e.first;
        b.x = e.second.tile.x() + 0.5 + e.second.off.x;
        b.y = e.second.tile.y() + 0.5 + e.second.off.y;
        b.z = e.second.tile.z();
        out.push_back( b );
    }
    return out;
}

void client_bodies( const std::vector<body_sample> &list, int acked_seq, ms_t arrival )
{
    free_client &fc = fclient();
    const int me = cinfo().my_id;
    fc.on = true;
    for( const body_sample &b : list ) {
        if( b.actor == me ) {
            client_reconcile( fc, b, acked_seq, arrival );
        } else {
            std::deque<std::tuple<ms_t, double, double, int>> &q = fc.others[b.actor].samples;
            q.emplace_back( arrival, b.x, b.y, b.z );
            while( q.size() > 20 ) {
                q.pop_front();
            }
        }
    }
}

bool client_free_movement()
{
    return fclient().on;
}

void client_set_mode( bool on, int my_character_id )
{
    client_info &ci = cinfo();
    ci = client_info();
    ci.on = on;
    ci.my_id = my_character_id;
    pred().active = false;
    fclient() = free_client();
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
            if( client_free_movement() ) {
                // the predicted body is drawn; the step only moves the avatar to its tile
                note_own_step_started();
                ci.last_step_ms = ev.dur_ms;
                break;
            }
            prediction &pr = pred();
            ms_t t0 = arrival;
            if( pr.active && pr.from == ev.from && pr.to == ev.to ) {
                // the prediction was right: continue it, the sprite does not jump
                t0 = pr.t0;
                pr.active = false;
                begin_slide( ev.from, ev.to, t0, ev.dur_ms );
            } else if( pr.active ) {
                // the host let us go elsewhere: the sprite goes on from where the prediction
                // had put it, so it does not jump
                const offset o = travel_offset_at( pr.from, arrival );
                pr.active = false;
                begin_slide( ev.to, pr.from.x() + 0.5 + o.x, pr.from.y() + 0.5 + o.y, t0, ev.dur_ms );
            } else {
                begin_slide( ev.from, ev.to, t0, ev.dur_ms );
            }
            note_own_step_started();
            ci.last_step_ms = ev.dur_ms;
            ci.confirmed_end = t0 + ev.dur_ms;
            break;
        }
        case fx_type::swing:
            if( pred().active ) {
                // the host did not let us go: the sprite eases back to where the actor really is
                const prediction &pr = pred();
                const offset o = travel_offset_at( pr.from, arrival );
                ease_back( ev.from, pr.from.x() + 0.5 + o.x, pr.from.y() + 0.5 + o.y, arrival );
                pred().active = false;
            }
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
        case fx_type::tracer:
        case fx_type::aimline:
        case fx_type::boom:
            apply_visual_event( ev, arrival );
            break;
        case fx_type::flourish:
            // the own dash is drawn from the prediction, at once; the rest as it comes
            if( static_cast<flourish_kind>( ev.windup_ms ) != flourish_kind::dash ) {
                apply_visual_event( ev, arrival );
            }
            break;
        case fx_type::aim:
            break;
    }
}

namespace
{
// C11: auto-travel of a client (overmap "travel", auto-move): the route the overmap gave the
// avatar is kept in absolute tiles (the client's map shifts under it) and walked by pressing
// the movement keys for the player; any key of its own or a danger warning stops it
std::vector<tripoint_abs_ms> &travel_route()
{
    static std::vector<tripoint_abs_ms> r;
    return r;
}

// overmap travel: the overmap tiles still to go (the destination first, as Character::omt_path)
std::vector<tripoint_abs_omt> &travel_omts()
{
    static std::vector<tripoint_abs_omt> r;
    return r;
}

void stop_travel( avatar &u, const std::string &why, game_message_type type )
{
    travel_route().clear();
    travel_omts().clear();
    u.omt_path.clear();
    ui::omap::force_quit();
    if( !why.empty() ) {
        u.add_msg_if_player( type, why );
    }
}

void client_auto_travel( avatar &u, input_frame &in )
{
    map &here = get_map();
    std::vector<tripoint_abs_ms> &route = travel_route();
    std::vector<tripoint_abs_omt> &omts = travel_omts();
    if( u.has_destination() ) {
        route.clear();
        omts.clear();
        for( const tripoint_bub_ms &p : u.get_auto_move_route() ) {
            route.push_back( here.get_abs( p ) );
        }
        u.clear_destination();
    }
    if( route.empty() && omts.empty() ) {
        return;
    }
    static const bool trace = std::getenv( "CATA_MP_TEST_TRACE" ) != nullptr;
    if( trace ) {
        DebugLog( D_INFO, D_MAIN ) << "trace travel route " << route.size() << " omt " << omts.size();
    }
    if( in.move_x != 0 || in.move_y != 0 || in.primary || in.cancel ) {
        stop_travel( u, _( "You stop travelling." ), m_info );
        return;
    }
    if( alerts::recent_threat( 1000 ) ) {
        stop_travel( u, _( "Danger!  You stop travelling." ), m_warning );
        return;
    }
    const tripoint_abs_ms at = u.pos_abs();
    // drop the points already reached (a ramp can change the level)
    while( !route.empty() && route.front().xy() == at.xy() ) {
        route.erase( route.begin() );
    }
    if( route.empty() && !omts.empty() ) {
        // overmap travel: the next leg (as activity_handlers::travel_do_turn)
        omts.pop_back();
        if( omts.empty() ) {
            stop_travel( u, _( "You have reached your destination." ), m_info );
            return;
        }
        const tripoint_abs_omt next_omt = omts.back();
        tripoint_abs_ms waypoint;
        if( omts.size() == 1 ) {
            waypoint = midpoint( project_bounds<coords::ms>( next_omt ) );
        } else {
            const tripoint_abs_ms cur_mid = midpoint( project_bounds<coords::ms>( u.pos_abs_omt() ) );
            waypoint = clamp( cur_mid, project_bounds<coords::ms>( next_omt ) );
        }
        const std::vector<tripoint_bub_ms> leg =
            here.route( u, pathfinding_target::radius( here.get_bub( waypoint ), 2 ) );
        if( leg.empty() ) {
            stop_travel( u, _( "You cannot reach that destination." ), m_warning );
            return;
        }
        for( const tripoint_bub_ms &p : leg ) {
            route.push_back( here.get_abs( p ) );
        }
    }
    if( route.empty() ) {
        return;
    }
    const tripoint_abs_ms next = route.front();
    if( std::max( std::abs( next.x() - at.x() ), std::abs( next.y() - at.y() ) ) > 2 ) {
        // pushed off the way: stop rather than walk somewhere else
        stop_travel( u, _( "You lost the way and stop travelling." ), m_info );
        return;
    }
    in.move_x = std::clamp( next.x() - at.x(), -1, 1 );
    in.move_y = std::clamp( next.y() - at.y(), -1, 1 );
}
} // namespace

void client_start_travel( const std::vector<tripoint_abs_omt> &omt_path )
{
    travel_route().clear();
    travel_omts() = omt_path;
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
    client_auto_travel( u, in );
    // a dodge asked for since the last frame goes with this movement frame (standing: none)
    static int seen_dodges = -1;
    if( seen_dodges < 0 ) {
        seen_dodges = in.dodge_presses;
    }
    if( in.dodge_presses != seen_dodges ) {
        seen_dodges = in.dodge_presses;
        in.dodge = in.move_x != 0 || in.move_y != 0;
        if( trace_enabled() ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace dodge pressed moving=" << in.dodge;
        }
    }
    // the frame's duration: the host moves the body by exactly this (with the same direction),
    // so its positions match the prediction below
    free_client &fc = fclient();
    const int dt = fc.last == 0 ? 0 : static_cast<int>( std::clamp<ms_t>( now - fc.last, 0,
                   tune().max_move_frame_ms ) );
    fc.last = now;
    const int seq = net::client_send_input( in, client_free_movement() ? dt : 0 );
    note_cursor( in, now );
    if( client_free_movement() ) {
        client_free_frame( in, seq, dt, now );
    }

    // ---- prediction of the own step
    prediction &pr = pred();
    if( pr.active && now >= pr.t0 + pr.dur + 250 ) {
        // never confirmed: the host did not let us go there, the sprite eases back
        const offset o = travel_offset_at( pr.from, now );
        pr.active = false;
        ease_back( u.pos_abs(), pr.from.x() + 0.5 + o.x, pr.from.y() + 0.5 + o.y, now );
    }
    const bool wants_move = in.move_x != 0 || in.move_y != 0;
    if( wants_move && !client_free_movement() && !pr.active && now >= ci.confirmed_end && now >= ci.busy_until &&
        !u.controlling_vehicle && !in.primary && !in.secondary && !easing_back_at( u.pos_abs(), now ) ) {
        const tripoint_abs_ms from = u.pos_abs();
        // the host slides a blocked diagonal along a wall: predict the same
        const std::pair<int, int> dir = corner_slide_dir( u, in.move_x, in.move_y );
        const tripoint_abs_ms to = from + tripoint_rel_ms( dir.first, dir.second, 0 );
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
