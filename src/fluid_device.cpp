// Fluid mode: the keyboard / mouse as seen by the fluid driver, and the key layout switch.
// See fluid.h and doc/MULTIPLAYER_FLUID.md.

#include "fluid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "action.h"
#include "avatar.h"
#include "debug.h"
#include "game.h"
#include "input.h"
#include "map.h"
#include "options.h"
#include "path_info.h"

#if defined(TILES)
#include "cata_tiles.h"
#include "sdl_wrappers.h"
#include "sdltiles.h"
#endif

namespace fluid
{

// ---------------------------------------------------------------------------------------
// key layout

namespace
{
key_layout &layout_ref()
{
    static key_layout l = key_layout::classic;
    return l;
}
} // namespace

key_layout current_key_layout()
{
    return layout_ref();
}

void apply_key_layout( key_layout layout )
{
    // stock files, then (optionally) the fluid overlay, then the player's own changes
    std::optional<cata_path> overlay;
    if( layout == key_layout::fluid_wasd ) {
        overlay = PATH_INFO::keybindings_fluid_layout();
    }
    inp_mngr.reload_keybindings( overlay );
    layout_ref() = layout;
}

void sync_key_layout()
{
    const key_layout wanted = get_option<bool>( "FLUID_WASD_LAYOUT" ) ? key_layout::fluid_wasd :
                              key_layout::classic;
    if( wanted != layout_ref() ) {
        apply_key_layout( wanted );
    }
}

#if defined(TILES)

namespace
{

enum bit : unsigned {
    bit_primary = 1u << 8,
    bit_secondary = 1u << 9,
    bit_cancel = 1u << 10,
    bit_primary_alt = 1u << 11,
    bit_secondary_alt = 1u << 12,
    bit_shove = 1u << 13,
    bit_dodge = 1u << 14
};

struct device_state {
    int primary_presses = 0;
    // when the mouse buttons were last seen going down here (a click event of the game counts
    // only if it was too short to be seen: else it is the same click twice)
    ms_t primary_down_at = -100000;
    ms_t secondary_down_at = -100000;
    int secondary_presses = 0;
    int throw_presses = 0;
    int shove_presses = 0;
    int dodge_presses = 0;
    // when each movement key was last pressed (a second press soon after is a dodge)
    std::array<ms_t, 8> move_pressed_at = {};
    // raw bits (8 movement keys + buttons) of the last poll
    unsigned last_raw = 0;
    // bits that were pressed while a classic action ran: ignored until released
    unsigned suppressed = 0;
    unsigned before_action = 0;
    std::array<std::vector<int>, 8> move_keys;
    std::vector<int> cancel_keys;
    std::vector<int> primary_keys;
    std::vector<int> secondary_keys;
    std::vector<int> shove_keys;
    std::vector<int> dodge_keys;
    ms_t refreshed = -100000;
};

device_state &dev()
{
    static device_state d;
    return d;
}

// A classic action may open a menu or a direction prompt: fluid input is completely parked until it
// returns.  Nested classic actions (a direction prompt inside an action) keep the outermost start.
int &classic_action_depth()
{
    static int depth = 0;
    return depth;
}

bool classic_action_active()
{
    return classic_action_depth() > 0;
}

const action_id move_actions[8] = {
    ACTION_MOVE_FORTH, ACTION_MOVE_FORTH_RIGHT, ACTION_MOVE_RIGHT, ACTION_MOVE_BACK_RIGHT,
    ACTION_MOVE_BACK, ACTION_MOVE_BACK_LEFT, ACTION_MOVE_LEFT, ACTION_MOVE_FORTH_LEFT
};

// Turns the held movement keys into the direction the driver gets (the WASD feel, see the
// movement rework spec in doc/MULTIPLAYER_ROADMAP.md):
// - opposing keys: the one pressed last wins, never zero;
// - from standstill a short chord window lets a two-key diagonal form before the first step;
// - when one key of a held diagonal is released the diagonal stays latched for a moment, so
//   releasing both keys almost together leaves no stray cardinal step;
// - a tap is sent for a minimum time and kept until the game has started the step, so a short
//   press is never lost.
struct move_resolver {
    ms_t press_seq_at[8] = {};
    unsigned seq[8] = {};
    unsigned counter = 0;
    unsigned prev_bits = 0;
    int out_x = 0;
    int out_y = 0;
    ms_t out_since = 0;
    ms_t grace_until = 0;
    ms_t chord_until = 0;
    int chord_x = 0;
    int chord_y = 0;
    ms_t tap_until = 0;
    bool tap_pending = false;
    ms_t tap_pending_until = 0;
    int tap_x = 0;
    int tap_y = 0;
    unsigned steps_started = 0;
    unsigned steps_seen = 0;

    void start_tap( int x, int y, ms_t now, const tuning &tn ) {
        tap_x = x;
        tap_y = y;
        tap_until = now + tn.tap_hold_ms;
        tap_pending = true;
        tap_pending_until = now + 400;
    }

    // A menu or direction prompt has owned the keyboard: forget the direction that was held
    // before it, so that releasing that key afterwards is not replayed as a phantom tap.
    void reset() {
        prev_bits = 0;
        out_x = 0;
        out_y = 0;
        out_since = 0;
        grace_until = 0;
        chord_until = 0;
        chord_x = 0;
        chord_y = 0;
        tap_until = 0;
        tap_pending = false;
        tap_pending_until = 0;
        tap_x = 0;
        tap_y = 0;
    }

    std::pair<int, int> resolve( unsigned bits, ms_t now ) {
        const tuning &tn = tune();
        bits &= 0xFFu;
        for( size_t i = 0; i < 8; ++i ) {
            if( ( bits & ~prev_bits ) & ( 1u << i ) ) {
                seq[i] = ++counter;
            }
        }
        prev_bits = bits;
        // the held keys, opposing ones resolved by who was pressed last
        unsigned best_seq[4] = {};  // x+, x-, y+, y-
        for( size_t i = 0; i < 8; ++i ) {
            if( !( bits & ( 1u << i ) ) ) {
                continue;
            }
            const point_rel_ms delta = get_delta_from_movement_action( move_actions[i], iso_rotate::no );
            if( delta.x() > 0 ) {
                best_seq[0] = std::max( best_seq[0], seq[i] );
            } else if( delta.x() < 0 ) {
                best_seq[1] = std::max( best_seq[1], seq[i] );
            }
            if( delta.y() > 0 ) {
                best_seq[2] = std::max( best_seq[2], seq[i] );
            } else if( delta.y() < 0 ) {
                best_seq[3] = std::max( best_seq[3], seq[i] );
            }
        }
        const auto axis = [&]( unsigned plus, unsigned minus ) {
            if( plus == 0 && minus == 0 ) {
                return 0;
            }
            return plus > minus ? 1 : -1;
        };
        const int rx = axis( best_seq[0], best_seq[1] );
        const int ry = axis( best_seq[2], best_seq[3] );

        if( steps_started != steps_seen ) {
            steps_seen = steps_started;
            tap_pending = false;
        }
        if( tap_pending && now > tap_pending_until ) {
            tap_pending = false;
        }

        int nx = out_x;
        int ny = out_y;
        const bool was_diag = out_x != 0 && out_y != 0;
        if( rx == 0 && ry == 0 ) {
            // keys released: a clean stop (also inside the release grace)
            if( chord_until != 0 && ( chord_x != 0 || chord_y != 0 ) ) {
                // released again before the chord window ended: that was a tap
                start_tap( chord_x, chord_y, now, tn );
            }
            nx = 0;
            ny = 0;
            grace_until = 0;
            chord_until = 0;
        } else if( was_diag && ( rx != out_x || ry != out_y ) &&
                   ( ( rx == out_x && ry == 0 ) || ( ry == out_y && rx == 0 ) ) ) {
            // one key of the diagonal was released
            if( grace_until == 0 ) {
                grace_until = now + tn.release_grace_ms;
            }
            if( now >= grace_until ) {
                nx = rx;
                ny = ry;
                grace_until = 0;
            }
        } else if( out_x == 0 && out_y == 0 && ( rx == 0 || ry == 0 ) && tn.chord_ms > 0 &&
                   !free_movement() ) {
            // (free movement needs no chord window: a diagonal can start at any moment)
            // from standstill: give a second key the chance to join
            if( chord_until == 0 ) {
                chord_until = now + tn.chord_ms;
            }
            chord_x = rx;
            chord_y = ry;
            if( now >= chord_until ) {
                nx = rx;
                ny = ry;
                chord_until = 0;
            }
        } else {
            nx = rx;
            ny = ry;
            grace_until = 0;
            chord_until = 0;
        }
        if( nx != out_x || ny != out_y ) {
            if( ( nx == 0 && ny == 0 ) && ( out_x != 0 || out_y != 0 ) && now - out_since < tn.tap_hold_ms ) {
                start_tap( out_x, out_y, now - ( now - out_since ), tn );
                tap_until = out_since + tn.tap_hold_ms;
            }
            out_x = nx;
            out_y = ny;
            out_since = now;
        }
        if( out_x != 0 || out_y != 0 ) {
            tap_pending = false;
            return { out_x, out_y };
        }
        if( ( tap_pending || now < tap_until ) && ( tap_x != 0 || tap_y != 0 ) ) {
            return { tap_x, tap_y };
        }
        return { 0, 0 };
    }
};

move_resolver &resolver()
{
    static move_resolver r;
    return r;
}

std::vector<int> scancodes_for( const input_event &ev )
{
    std::vector<int> result;
    if( ev.sequence.size() != 1 || !ev.modifiers.empty() ) {
        return result;
    }
    const int key = ev.sequence.front();
    if( ev.type == input_event_t::keyboard_char ) {
        switch( key ) {
            case KEY_UP:
                result.push_back( SDL_SCANCODE_UP );
                break;
            case KEY_DOWN:
                result.push_back( SDL_SCANCODE_DOWN );
                break;
            case KEY_LEFT:
                result.push_back( SDL_SCANCODE_LEFT );
                break;
            case KEY_RIGHT:
                result.push_back( SDL_SCANCODE_RIGHT );
                break;
            case KEY_HOME:
                result.push_back( SDL_SCANCODE_HOME );
                break;
            case KEY_END:
                result.push_back( SDL_SCANCODE_END );
                break;
            case KEY_PPAGE:
                result.push_back( SDL_SCANCODE_PAGEUP );
                break;
            case KEY_NPAGE:
                result.push_back( SDL_SCANCODE_PAGEDOWN );
                break;
            case '\t':
                result.push_back( SDL_SCANCODE_TAB );
                break;
            default:
                // printable ascii; upper case letters would need shift, which is not tracked
                if( key > ' ' && key < 127 && !( key >= 'A' && key <= 'Z' ) ) {
                    const SDL_Scancode sc = SDL_GetScancodeFromKey( key );
                    if( sc != SDL_SCANCODE_UNKNOWN ) {
                        result.push_back( sc );
                    }
                    if( key >= '1' && key <= '9' ) {
                        result.push_back( SDL_SCANCODE_KP_1 + ( key - '1' ) );
                    } else if( key == '0' ) {
                        result.push_back( SDL_SCANCODE_KP_0 );
                    }
                } else if( key == ' ' ) {
                    result.push_back( SDL_SCANCODE_SPACE );
                }
                break;
        }
    } else if( ev.type == input_event_t::keyboard_code ) {
        const SDL_Scancode sc = SDL_GetScancodeFromKey( key );
        if( sc != SDL_SCANCODE_UNKNOWN ) {
            result.push_back( sc );
        }
    }
    return result;
}

void collect( const std::string &action, const std::string &context, std::vector<int> &dest )
{
    dest.clear();
    for( const input_event &ev : inp_mngr.get_input_for_action( action, context ) ) {
        for( const int sc : scancodes_for( ev ) ) {
            if( std::find( dest.begin(), dest.end(), sc ) == dest.end() ) {
                dest.push_back( sc );
            }
        }
    }
}

void refresh_bindings( device_state &d, ms_t now )
{
    if( now - d.refreshed < 500 ) {
        return;
    }
    d.refreshed = now;
    for( size_t i = 0; i < d.move_keys.size(); ++i ) {
        collect( action_ident( move_actions[i] ), "DEFAULTMODE", d.move_keys[i] );
    }
    collect( "FLUID_CANCEL", "FLUID", d.cancel_keys );
    collect( "FLUID_PRIMARY", "FLUID", d.primary_keys );
    collect( "FLUID_SECONDARY", "FLUID", d.secondary_keys );
    collect( "FLUID_SHOVE", "FLUID", d.shove_keys );
    collect( "FLUID_DODGE", "FLUID", d.dodge_keys );
}

bool any_down( const Uint8 *keys, const std::vector<int> &list )
{
    return std::any_of( list.begin(), list.end(), [&]( int sc ) {
        return keys[sc] != 0;
    } );
}

// what is held down right now: bits 0-7 movement keys, then the buttons
unsigned read_raw( device_state &d )
{
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState( nullptr );
    unsigned raw = 0;
    for( size_t i = 0; i < d.move_keys.size(); ++i ) {
        if( any_down( keys, d.move_keys[i] ) ) {
            raw |= 1u << i;
        }
    }
    const Uint32 buttons = SDL_GetMouseState( nullptr, nullptr );
    if( buttons & SDL_BUTTON( SDL_BUTTON_LEFT ) ) {
        raw |= bit_primary;
    }
    if( buttons & SDL_BUTTON( SDL_BUTTON_RIGHT ) ) {
        raw |= bit_secondary;
    }
    if( ( buttons & SDL_BUTTON( SDL_BUTTON_MIDDLE ) ) || any_down( keys, d.shove_keys ) ) {
        raw |= bit_shove;
    }
    if( any_down( keys, d.cancel_keys ) ) {
        raw |= bit_cancel;
    }
    if( ( buttons & ( SDL_BUTTON( SDL_BUTTON_X1 ) | SDL_BUTTON( SDL_BUTTON_X2 ) ) ) ||
        any_down( keys, d.dodge_keys ) ) {
        raw |= bit_dodge;
    }
    if( any_down( keys, d.primary_keys ) ) {
        raw |= bit_primary_alt;
    }
    if( any_down( keys, d.secondary_keys ) ) {
        raw |= bit_secondary_alt;
    }
    return raw;
}

// the cursor in world tiles
bool read_cursor( input_frame &out )
{
    if( SDL_GetMouseFocus() == nullptr ) {
        return false;
    }
    if( g->is_tileset_isometric() ) {
        return false;
    }
    int mx = 0;
    int my = 0;
    SDL_GetMouseState( &mx, &my );
    const window_dimensions dim = get_window_dimensions( g->w_terrain );
    const point win_min = dim.window_pos_pixel;
    const point win_size = dim.window_size_pixel;
    const point tile = dim.scaled_font_size;
    if( tile.x <= 0 || tile.y <= 0 ) {
        return false;
    }
    point screen( mx - win_min.x, my - win_min.y );
    if( screen.x < 0 || screen.y < 0 || screen.x >= win_size.x || screen.y >= win_size.y ) {
        return false;
    }
    // the camera is shifted by a fraction of a tile, see cata_tiles::draw
    const offset cam = camera_offset();
    const double sx = screen.x + cam.x * tile.x;
    const double sy = screen.y + cam.y * tile.y;
    const point snapped( static_cast<int>( std::floor( sx ) ), static_cast<int>( std::floor( sy ) ) );
    const point_bub_ms base = cata_tiles::screen_to_player( snapped, tile, win_size,
                              point_bub_ms( g->ter_view_p.xy().raw() ), false );
    const double fx = sx / tile.x - std::floor( sx / tile.x );
    const double fy = sy / tile.y - std::floor( sy / tile.y );
    const tripoint_abs_ms abs = get_map().get_abs( tripoint_bub_ms( base, g->ter_view_p.z() ) );
    out.cursor_valid = true;
    out.cursor_x = abs.x() + fx;
    out.cursor_y = abs.y() + fy;
    out.cursor_z = abs.z();
    // test trace: where the cursor is relative to the view centre (the player's tile), logged
    // when it moved by more than 0.2 tile
    static const bool trace = std::getenv( "CATA_MP_TEST_TRACE" ) != nullptr;
    if( trace ) {
        static double last_x = 0.0;
        static double last_y = 0.0;
        if( std::abs( out.cursor_x - last_x ) + std::abs( out.cursor_y - last_y ) > 0.2 ) {
            last_x = out.cursor_x;
            last_y = out.cursor_y;
            const tripoint_abs_ms centre = get_map().get_abs( g->ter_view_p );
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace cursor x=" << out.cursor_x << " y=" <<
                                       out.cursor_y << " dx=" << out.cursor_x - ( centre.x() + 0.5 ) <<
                                       " dy=" << out.cursor_y - ( centre.y() + 0.5 ) << " mouse=" << mx <<
                                       "," << my << " tile=" << tile.x << "," << tile.y;
        }
    }
    return true;
}

} // namespace

void note_own_step_started()
{
    ++resolver().steps_started;
}

void note_classic_action( int action )
{
    device_state &d = dev();
    // (a click the device saw held is counted by the held state already)
    if( action == ACTION_SELECT ) {
        if( now_ms() - d.primary_down_at > 250 ) {
            ++d.primary_presses;
        }
    } else if( action == ACTION_SEC_SELECT ) {
        if( now_ms() - d.secondary_down_at > 250 ) {
            ++d.secondary_presses;
        }
    } else if( action == ACTION_THROW || action == ACTION_THROW_WIELDED ) {
        ++d.throw_presses;
    } else if( action == ACTION_FIRE ) {
        // the fire key is the left mouse button for the keyboard, unless it is the shove key
        // (f by default)
        refresh_bindings( d, now_ms() );
        if( !any_down( SDL_GetKeyboardState( nullptr ), d.shove_keys ) ) {
            ++d.primary_presses;
        }
    }
}

void classic_action_begin()
{
    if( classic_action_depth()++ == 0 ) {
        device_state &d = dev();
        refresh_bindings( d, now_ms() );
        d.before_action = read_raw( d );
        resolver().reset();
    }
}

void classic_action_end()
{
    if( classic_action_depth() == 0 ) {
        return;
    }
    if( --classic_action_depth() == 0 ) {
        device_state &d = dev();
        const unsigned after = read_raw( d );
        // A key held when the menu/action ends (including the direction that answered a prompt)
        // must be released before it counts as movement again.  This also stops the character
        // from jerking into the direction that confirmed an action.
        d.suppressed |= after;
        resolver().reset();
    }
}

bool device_key( int key )
{
    if( key == ' ' ) {
        key = SDLK_SPACE;
    } else if( key >= 'A' && key <= 'Z' ) {
        // the same key held with Shift (sprinting)
        key = key - 'A' + 'a';
    } else if( key <= ' ' || key >= 127 ) {
        return false;
    }
    const SDL_Scancode sc = SDL_GetScancodeFromKey( key );
    if( sc == SDL_SCANCODE_UNKNOWN ) {
        return false;
    }
    device_state &d = dev();
    refresh_bindings( d, now_ms() );
    const auto has = [sc]( const std::vector<int> &list ) {
        return std::find( list.begin(), list.end(), static_cast<int>( sc ) ) != list.end();
    };
    return has( d.cancel_keys ) || has( d.primary_keys ) || has( d.secondary_keys ) ||
           has( d.shove_keys ) ||
           std::any_of( d.move_keys.begin(), d.move_keys.end(), has );
}

bool poll_device( input_frame &out )
{
    device_state &d = dev();
    refresh_bindings( d, now_ms() );
    unsigned raw = read_raw( d );
    // a suppressed bit stays suppressed until it was seen released
    d.suppressed &= raw;
    raw &= ~d.suppressed;
    if( classic_action_active() ) {
        // A menu or direction prompt owns the keyboard and mouse now.  Parking the whole fluid
        // frame also stops a held WASD key from moving the character behind the prompt or
        // inventory.
        d.last_raw = raw;
        return false;
    }
    if( ( raw & bit_shove ) && !( d.last_raw & bit_shove ) ) {
        ++d.shove_presses;
    }
    if( ( raw & bit_dodge ) && !( d.last_raw & bit_dodge ) ) {
        ++d.dodge_presses;
    }
    static const bool dtrace = std::getenv( "CATA_MP_TEST_TRACE" ) != nullptr;
    if( dtrace && ( raw & ~d.last_raw ) != 0 ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: trace device new bits " << ( raw & ~d.last_raw ) <<
                                   " dodge keys " << d.dodge_keys.size() << " presses " << d.dodge_presses;
    }
    // a movement key pressed twice within 250 ms: a dodge in that direction
    for( size_t i = 0; i < d.move_pressed_at.size(); ++i ) {
        const unsigned b = 1u << i;
        if( ( raw & b ) && !( d.last_raw & b ) ) {
            const ms_t t = now_ms();
            if( t - d.move_pressed_at[i] < 250 ) {
                ++d.dodge_presses;
                d.move_pressed_at[i] = 0;
            } else {
                d.move_pressed_at[i] = t;
            }
        }
    }
    if( ( raw & bit_primary ) && !( d.last_raw & bit_primary ) ) {
        d.primary_down_at = now_ms();
    }
    if( ( raw & bit_secondary ) && !( d.last_raw & bit_secondary ) ) {
        d.secondary_down_at = now_ms();
    }
    d.last_raw = raw;

    const std::pair<int, int> dir = resolver().resolve( raw, now_ms() );
    out.move_x = dir.first;
    out.move_y = dir.second;
    out.primary = ( raw & ( bit_primary | bit_primary_alt ) ) != 0;
    out.secondary = ( raw & ( bit_secondary | bit_secondary_alt ) ) != 0;
    out.cancel = ( raw & bit_cancel ) != 0;
    // hold Shift to sprint
    out.sprint = ( SDL_GetModState() & KMOD_SHIFT ) != 0;
    out.primary_presses = d.primary_presses;
    out.secondary_presses = d.secondary_presses;
    out.throw_presses = d.throw_presses;
    out.shove_presses = d.shove_presses;
    out.dodge_presses = d.dodge_presses;
    out.cursor_valid = false;
    read_cursor( out );
    return true;
}

#else // TILES

void note_own_step_started()
{
}

void note_classic_action( int )
{
}

void classic_action_begin()
{
}

void classic_action_end()
{
}

bool device_key( int )
{
    return false;
}

bool poll_device( input_frame & )
{
    return false;
}

#endif // TILES

} // namespace fluid
