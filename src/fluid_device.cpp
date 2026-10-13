// Fluid mode: the keyboard / mouse as seen by the fluid driver, and the key layout switch.
// See fluid.h and doc/MULTIPLAYER_FLUID.md.

#include "fluid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "action.h"
#include "avatar.h"
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
    bit_secondary_alt = 1u << 12
};

struct device_state {
    int primary_presses = 0;
    int secondary_presses = 0;
    // raw bits (8 movement keys + buttons) of the last poll
    unsigned last_raw = 0;
    // bits that were pressed while a classic action ran: ignored until released
    unsigned suppressed = 0;
    unsigned before_action = 0;
    std::array<std::vector<int>, 8> move_keys;
    std::vector<int> cancel_keys;
    std::vector<int> primary_keys;
    std::vector<int> secondary_keys;
    ms_t refreshed = -100000;
};

device_state &dev()
{
    static device_state d;
    return d;
}

const action_id move_actions[8] = {
    ACTION_MOVE_FORTH, ACTION_MOVE_FORTH_RIGHT, ACTION_MOVE_RIGHT, ACTION_MOVE_BACK_RIGHT,
    ACTION_MOVE_BACK, ACTION_MOVE_BACK_LEFT, ACTION_MOVE_LEFT, ACTION_MOVE_FORTH_LEFT
};

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
    if( any_down( keys, d.cancel_keys ) ) {
        raw |= bit_cancel;
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
    return true;
}

} // namespace

void note_classic_action( int action )
{
    device_state &d = dev();
    if( action == ACTION_SELECT ) {
        ++d.primary_presses;
    } else if( action == ACTION_SEC_SELECT ) {
        ++d.secondary_presses;
    }
}

void classic_action_begin()
{
    device_state &d = dev();
    refresh_bindings( d, now_ms() );
    d.before_action = read_raw( d );
}

void classic_action_end()
{
    device_state &d = dev();
    const unsigned after = read_raw( d );
    // whatever was pressed during the action (e.g. the key that confirmed a menu) must be
    // released before it counts as movement again
    d.suppressed |= after & ~d.before_action;
}

bool poll_device( input_frame &out )
{
    device_state &d = dev();
    refresh_bindings( d, now_ms() );
    unsigned raw = read_raw( d );
    // a suppressed bit stays suppressed until it was seen released
    d.suppressed &= raw;
    raw &= ~d.suppressed;
    d.last_raw = raw;

    int mx = 0;
    int my = 0;
    for( size_t i = 0; i < 8; ++i ) {
        if( raw & ( 1u << i ) ) {
            const point_rel_ms delta = get_delta_from_movement_action( move_actions[i], iso_rotate::no );
            mx += delta.x();
            my += delta.y();
        }
    }
    out.move_x = std::clamp( mx, -1, 1 );
    out.move_y = std::clamp( my, -1, 1 );
    out.primary = ( raw & ( bit_primary | bit_primary_alt ) ) != 0;
    out.secondary = ( raw & ( bit_secondary | bit_secondary_alt ) ) != 0;
    out.cancel = ( raw & bit_cancel ) != 0;
    // hold Shift to sprint
    out.sprint = ( SDL_GetModState() & KMOD_SHIFT ) != 0;
    out.primary_presses = d.primary_presses;
    out.secondary_presses = d.secondary_presses;
    out.cursor_valid = false;
    read_cursor( out );
    return true;
}

#else // TILES

void note_classic_action( int )
{
}

void classic_action_begin()
{
}

void classic_action_end()
{
}

bool poll_device( input_frame & )
{
    return false;
}

#endif // TILES

} // namespace fluid
