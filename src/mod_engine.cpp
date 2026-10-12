#include "mod_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "dialogue.h"
#include "effect_on_condition.h"
#include "game.h"
#include "global_vars.h"
#include "input_context.h"
#include "json.h"
#include "json_loader.h"
#include "math_parser_diag_value.h"
#include "music.h"
#include "output.h"
#include "path_info.h"
#include "point.h"
#include "sdlsound.h"
#include "string_input_popup.h"
#include "talker.h"
#include "translations.h"
#include "popup.h"
#include "uilist.h"
#include "ui_manager.h"
#include "widget.h"

namespace mod_engine
{
namespace
{
using clock_type = std::chrono::steady_clock;

std::string config_file( const std::string &name )
{
    return PATH_INFO::config_dir() + name;
}

// ---------------------------------------------------------------------------------------------
// saved state: option values and window positions (small JSON files in the config folder)
// ---------------------------------------------------------------------------------------------
struct option_def {
    std::string id;
    std::string source; // id of the mod that defines it
    std::string name;
    std::string description;
    std::string kind = "bool"; // bool, int, list, text
    std::string default_value = "false";
    int min_value = 0;
    int max_value = 100;
    int step = 1;
    std::vector<std::pair<std::string, std::string>> choices; // value, display name
    effect_on_condition_id on_change;
    bool has_on_change = false;
};

struct window_state {
    point pos;
    bool has_pos = false;
    bool open = false;
};

std::map<std::string, option_def> &option_registry()
{
    static std::map<std::string, option_def> reg;
    return reg;
}

std::map<std::string, std::string> &option_values()
{
    static std::map<std::string, std::string> values;
    return values;
}

bool &option_values_loaded()
{
    static bool loaded = false;
    return loaded;
}

std::map<std::string, window_state> &window_states()
{
    static std::map<std::string, window_state> states;
    return states;
}

bool &window_states_loaded()
{
    static bool loaded = false;
    return loaded;
}

void ensure_option_values()
{
    if( option_values_loaded() ) {
        return;
    }
    option_values_loaded() = true;
    const std::optional<std::string> text = read_whole_file( config_file( "mod_options.json" ) );
    if( !text || text->empty() ) {
        return;
    }
    try {
        JsonValue jv = json_loader::from_string( *text );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        for( const JsonMember member : jo ) {
            option_values()[member.name()] = member.get_string();
        }
    } catch( const std::exception &err ) {
        debugmsg( "Could not read mod_options.json: %s", err.what() );
    }
}

void save_option_values()
{
    write_to_file( config_file( "mod_options.json" ), []( std::ostream & fout ) {
        JsonOut jsout( fout, true );
        jsout.start_object();
        for( const std::pair<const std::string, std::string> &entry : option_values() ) {
            jsout.member( entry.first, entry.second );
        }
        jsout.end_object();
    }, _( "mod options" ) );
}

void ensure_window_states()
{
    if( window_states_loaded() ) {
        return;
    }
    window_states_loaded() = true;
    const std::optional<std::string> text = read_whole_file( config_file( "mod_windows.json" ) );
    if( !text || text->empty() ) {
        return;
    }
    try {
        JsonValue jv = json_loader::from_string( *text );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        for( const JsonMember member : jo ) {
            JsonObject entry = member.get_object();
            entry.allow_omitted_members();
            window_state st;
            if( entry.has_int( "x" ) && entry.has_int( "y" ) ) {
                st.pos = point( entry.get_int( "x" ), entry.get_int( "y" ) );
                st.has_pos = true;
            }
            st.open = entry.get_bool( "open", false );
            window_states()[member.name()] = st;
        }
    } catch( const std::exception &err ) {
        debugmsg( "Could not read mod_windows.json: %s", err.what() );
    }
}

void save_window_states()
{
    write_to_file( config_file( "mod_windows.json" ), []( std::ostream & fout ) {
        JsonOut jsout( fout, true );
        jsout.start_object();
        for( const std::pair<const std::string, window_state> &entry : window_states() ) {
            jsout.member( entry.first );
            jsout.start_object();
            if( entry.second.has_pos ) {
                jsout.member( "x", entry.second.pos.x );
                jsout.member( "y", entry.second.pos.y );
            }
            jsout.member( "open", entry.second.open );
            jsout.end_object();
        }
        jsout.end_object();
    }, _( "mod windows" ) );
}

// ---------------------------------------------------------------------------------------------
// realtime effect_on_conditions
// ---------------------------------------------------------------------------------------------
struct realtime_entry {
    std::string id;
    effect_on_condition_id eoc;
    double interval = 1.0; // seconds
    bool broken = false;
    clock_type::time_point next_run = clock_type::now();
};

std::vector<realtime_entry> &realtime_registry()
{
    static std::vector<realtime_entry> reg;
    return reg;
}

// ---------------------------------------------------------------------------------------------
// windows
// ---------------------------------------------------------------------------------------------
enum class align_type {
    left,
    center,
    right
};

struct button_def {
    std::string text;
    effect_on_condition_id eoc;
};

struct row_def {
    std::string text;
    std::vector<button_def> buttons;
    align_type align = align_type::left;
};

struct window_def {
    std::string id;
    std::string title;
    int width = 32;
    point default_pos = point( 2, 2 );
    bool remember_open = false;
    std::vector<row_def> rows;
};

std::map<std::string, window_def> &window_registry()
{
    static std::map<std::string, window_def> reg;
    return reg;
}

struct button_layout {
    int x = 0; // relative to the window
    int width = 0;
    std::string text;
};

struct row_layout {
    std::string text;
    int text_x = 0;
    std::vector<button_layout> buttons;
};

struct window_instance {
    std::string id;
    point pos;
    int width = 0;
    int height = 0;
    catacurses::window win;
    std::unique_ptr<ui_adaptor> ui;
    std::vector<row_layout> layout;
    std::string signature;
    int hover_row = -1;
    int hover_button = -1;
};

std::vector<std::unique_ptr<window_instance>> &open_windows()
{
    static std::vector<std::unique_ptr<window_instance>> wins;
    return wins;
}

std::vector<std::pair<std::string, std::string>> &pending_commands()
{
    static std::vector<std::pair<std::string, std::string>> cmds;
    return cmds;
}

bool &restore_needed()
{
    static bool need = true;
    return need;
}

struct drag_state {
    enum class kind_t {
        none,
        window,
        button
    };
    kind_t kind = kind_t::none;
    std::string id;
    point grab; // cell inside the window where the player grabbed it
    int row = -1;
    int button = -1;
};

drag_state &drag()
{
    static drag_state d;
    return d;
}

window_instance *find_instance( const std::string &id )
{
    for( std::unique_ptr<window_instance> &w : open_windows() ) {
        if( w->id == id ) {
            return w.get();
        }
    }
    return nullptr;
}

point clamp_position( const window_instance &w, point p )
{
    const int max_x = std::max( 0, TERMX - w.width );
    const int max_y = std::max( 0, TERMY - w.height );
    return point( std::clamp( p.x, 0, max_x ), std::clamp( p.y, 0, max_y ) );
}

/** Work out the texts and button places of a window.  Returns true if something changed. */
bool refresh_layout( window_instance &w )
{
    const auto def_it = window_registry().find( w.id );
    if( def_it == window_registry().end() ) {
        return false;
    }
    const window_def &def = def_it->second;
    std::vector<row_layout> layout;
    std::string sig;
    const int inner = w.width - 2;
    for( const row_def &row : def.rows ) {
        row_layout rl;
        if( row.buttons.empty() ) {
            rl.text = expand_text( row.text );
            const int len = utf8_width( rl.text, true );
            switch( row.align ) {
                case align_type::left:
                    rl.text_x = 0;
                    break;
                case align_type::center:
                    rl.text_x = std::max( 0, ( inner - len ) / 2 );
                    break;
                case align_type::right:
                    rl.text_x = std::max( 0, inner - len );
                    break;
            }
            sig += rl.text;
        } else {
            int total = 0;
            for( const button_def &b : row.buttons ) {
                button_layout bl;
                bl.text = expand_text( b.text );
                bl.width = utf8_width( bl.text, true );
                total += bl.width + ( rl.buttons.empty() ? 0 : 1 );
                rl.buttons.push_back( bl );
                sig += bl.text + "\x1f";
            }
            int x = 0;
            if( row.align == align_type::center ) {
                x = std::max( 0, ( inner - total ) / 2 );
            } else if( row.align == align_type::right ) {
                x = std::max( 0, inner - total );
            }
            for( button_layout &bl : rl.buttons ) {
                bl.x = x;
                x += bl.width + 1;
            }
        }
        sig += "\x1e";
        layout.push_back( std::move( rl ) );
    }
    sig += string_format( "%d,%d", w.hover_row, w.hover_button );
    if( sig == w.signature ) {
        return false;
    }
    w.signature = sig;
    w.layout = std::move( layout );
    return true;
}

void draw_window( window_instance &w )
{
    const auto def_it = window_registry().find( w.id );
    if( def_it == window_registry().end() || !w.win ) {
        return;
    }
    const window_def &def = def_it->second;
    werase( w.win );
    draw_border( w.win, BORDER_COLOR, def.title.empty() ? std::string() : " " + def.title + " ",
                 c_light_cyan );
    const int inner = w.width - 2;
    for( size_t r = 0; r < w.layout.size(); r++ ) {
        const row_layout &rl = w.layout[r];
        const int y = static_cast<int>( r ) + 1;
        if( def.rows[r].buttons.empty() ) {
            trim_and_print( w.win, point( 1 + rl.text_x, y ), std::max( 1, inner - rl.text_x ), c_light_gray,
                            rl.text );
        } else {
            for( size_t b = 0; b < rl.buttons.size(); b++ ) {
                const bool hot = static_cast<int>( r ) == w.hover_row && static_cast<int>( b ) == w.hover_button;
                const nc_color col = hot ? hilite( c_light_green ) : c_light_green;
                mvwprintz( w.win, point( 1 + rl.buttons[b].x, y ), col, "%s", rl.buttons[b].text );
            }
        }
    }
    wnoutrefresh( w.win );
}

void place_window( window_instance &w )
{
    w.win = catacurses::newwin( w.height, w.width, w.pos );
    if( w.ui ) {
        w.ui->position_from_window( w.win );
    }
}

void open_window( const std::string &id )
{
    if( find_instance( id ) != nullptr ) {
        return;
    }
    const auto def_it = window_registry().find( id );
    if( def_it == window_registry().end() ) {
        debugmsg( "window_control: there is no mod_window with the id \"%s\"", id );
        return;
    }
    ensure_window_states();
    const window_def &def = def_it->second;
    std::unique_ptr<window_instance> w = std::make_unique<window_instance>();
    w->id = id;
    w->width = std::clamp( def.width, 10, std::max( 10, TERMX ) );
    w->height = static_cast<int>( def.rows.size() ) + 2;
    const window_state &saved = window_states()[id];
    w->pos = clamp_position( *w, saved.has_pos ? saved.pos : def.default_pos );
    refresh_layout( *w );
    w->ui = std::make_unique<ui_adaptor>();
    window_instance *raw = w.get();
    w->ui->on_screen_resize( [raw]( ui_adaptor & ) {
        raw->pos = clamp_position( *raw, raw->pos );
        place_window( *raw );
    } );
    w->ui->on_redraw( [raw]( ui_adaptor & ) {
        draw_window( *raw );
    } );
    place_window( *w );
    open_windows().push_back( std::move( w ) );
    window_states()[id].open = true;
    save_window_states();
}

void close_window( const std::string &id, bool remember )
{
    std::vector<std::unique_ptr<window_instance>> &wins = open_windows();
    for( auto it = wins.begin(); it != wins.end(); ++it ) {
        if( ( *it )->id == id ) {
            if( drag().id == id ) {
                drag() = drag_state();
            }
            wins.erase( it );
            if( remember ) {
                window_states()[id].open = false;
                save_window_states();
            }
            return;
        }
    }
}

void apply_command( const std::string &cmd, const std::string &id )
{
    if( cmd == "open" ) {
        open_window( id );
    } else if( cmd == "close" ) {
        close_window( id, true );
    } else if( cmd == "toggle" ) {
        if( find_instance( id ) != nullptr ) {
            close_window( id, true );
        } else {
            open_window( id );
        }
    } else if( cmd == "close_all" ) {
        std::vector<std::string> ids;
        for( const std::unique_ptr<window_instance> &w : open_windows() ) {
            ids.push_back( w->id );
        }
        for( const std::string &i : ids ) {
            close_window( i, true );
        }
    } else if( cmd == "reset_position" ) {
        const auto def_it = window_registry().find( id );
        if( def_it == window_registry().end() ) {
            return;
        }
        ensure_window_states();
        window_state &st = window_states()[id];
        st.has_pos = false;
        if( window_instance *w = find_instance( id ) ) {
            w->pos = clamp_position( *w, def_it->second.default_pos );
            place_window( *w );
        }
        save_window_states();
    } else {
        debugmsg( "window_control: unknown command \"%s\" (use open, close, toggle, close_all or reset_position)",
                  cmd );
    }
}

void run_eoc( const effect_on_condition_id &eoc, const std::string &what )
{
    if( !eoc.is_valid() ) {
        debugmsg( "%s refers to an unknown effect_on_condition (%s)", what, eoc.str() );
        return;
    }
    dialogue d( get_talker_for( get_avatar() ), nullptr );
    eoc->activate( d );
}

/** Which window and which button (if any) is at this screen cell? */
struct hit_result {
    window_instance *win = nullptr;
    int row = -1;
    int button = -1;
};

hit_result hit_test( const point &cell )
{
    hit_result res;
    std::vector<std::unique_ptr<window_instance>> &wins = open_windows();
    for( auto it = wins.rbegin(); it != wins.rend(); ++it ) {
        window_instance &w = **it;
        if( cell.x < w.pos.x || cell.y < w.pos.y || cell.x >= w.pos.x + w.width ||
            cell.y >= w.pos.y + w.height ) {
            continue;
        }
        res.win = &w;
        const int rx = cell.x - w.pos.x - 1;
        const int ry = cell.y - w.pos.y - 1;
        if( ry >= 0 && ry < static_cast<int>( w.layout.size() ) ) {
            const row_layout &rl = w.layout[ry];
            for( size_t b = 0; b < rl.buttons.size(); b++ ) {
                if( rx >= rl.buttons[b].x && rx < rl.buttons[b].x + rl.buttons[b].width ) {
                    res.row = ry;
                    res.button = static_cast<int>( b );
                    break;
                }
            }
        }
        return res;
    }
    return res;
}

void bring_to_front( window_instance *w )
{
    std::vector<std::unique_ptr<window_instance>> &wins = open_windows();
    if( wins.empty() || wins.back().get() == w ) {
        return;
    }
    // Moving the instance to the end of the list is not enough, the ui stack order is fixed when
    // the ui_adaptor is created, so the window is re-created on top.
    const std::string id = w->id;
    const point pos = w->pos;
    close_window( id, false );
    open_window( id );
    if( window_instance *nw = find_instance( id ) ) {
        nw->pos = clamp_position( *nw, pos );
        place_window( *nw );
    }
}

void set_hover( window_instance *w, int row, int button )
{
    for( std::unique_ptr<window_instance> &i : open_windows() ) {
        const bool is_target = i.get() == w;
        const int new_row = is_target ? row : -1;
        const int new_button = is_target ? button : -1;
        if( i->hover_row != new_row || i->hover_button != new_button ) {
            i->hover_row = new_row;
            i->hover_button = new_button;
            if( refresh_layout( *i ) && i->ui ) {
                i->ui->invalidate_ui();
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// text templates
// ---------------------------------------------------------------------------------------------
std::string format_seconds( double sec )
{
    if( sec < 0 ) {
        return "--:--";
    }
    const int total = static_cast<int>( sec );
    return string_format( "%d:%02d", total / 60, total % 60 );
}

std::string album_display_name()
{
    const std::string current = music_get_mp3_album();
    if( current.empty() ) {
        return _( "normal playlist" );
    }
    for( const music::album_info &album : music::album_list() ) {
        if( album.playlist == current ) {
            return album.name;
        }
    }
    return current;
}

std::optional<std::string> resolve_placeholder( const std::string &key )
{
    std::string name = key;
    std::string arg;
    const size_t colon = key.find( ':' );
    if( colon != std::string::npos ) {
        name = key.substr( 0, colon );
        arg = key.substr( colon + 1 );
    }
    if( name == "music_track" ) {
        const std::string track = music_current_track();
        return track.empty() ? _( "(no music)" ) : track;
    } else if( name == "music_playlist" ) {
        return music_current_playlist();
    } else if( name == "music_album" ) {
        return album_display_name();
    } else if( name == "music_state" ) {
        if( music_current_track().empty() ) {
            return std::string( _( "Stopped" ) );
        }
        return music_is_paused() ? std::string( _( "Paused" ) ) : std::string( _( "Playing" ) );
    } else if( name == "music_playpause" ) {
        return music_is_paused() ? std::string( _( "Play" ) ) : std::string( _( "Pause" ) );
    } else if( name == "music_time" ) {
        return format_seconds( music_position_seconds() ) + " / " + format_seconds( music_duration_seconds() );
    } else if( name == "music_position" ) {
        return format_seconds( music_position_seconds() );
    } else if( name == "music_length" ) {
        return format_seconds( music_duration_seconds() );
    } else if( name == "music_bar" ) {
        int width = 20;
        if( !arg.empty() ) {
            width = std::clamp( atoi( arg.c_str() ), 3, 80 );
        }
        const double pos = music_position_seconds();
        const double len = music_duration_seconds();
        int filled = 0;
        if( pos >= 0 && len > 0 ) {
            filled = std::clamp( static_cast<int>( std::round( ( width - 2 ) * pos / len ) ), 0, width - 2 );
        }
        return "[" + std::string( filled, '#' ) + std::string( ( width - 2 ) - filled, '-' ) + "]";
    } else if( name == "music_volume" ) {
        return string_format( "%d", music_volume() );
    } else if( name == "time" ) {
        return to_string_time_of_day( calendar::turn );
    } else if( name == "day" ) {
        return string_format( "%d", to_days<int>( calendar::turn - calendar::turn_zero ) + 1 );
    } else if( name == "player_name" ) {
        return get_avatar().get_name();
    } else if( name == "var" ) {
        const diag_value *val = get_avatar().maybe_get_value( arg );
        return val == nullptr ? std::string() : val->to_string();
    } else if( name == "gvar" ) {
        const diag_value *val = get_globals().maybe_get_global_value( arg );
        return val == nullptr ? std::string() : val->to_string();
    } else if( name == "option" ) {
        return option_get( arg );
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------------------------
std::optional<std::filesystem::path> sandbox_path( const std::string &name_in )
{
    std::string name = name_in;
    std::replace( name.begin(), name.end(), '\\', '/' );
    if( name.empty() || name.size() > 100 || name.front() == '/' || name.back() == '/' ) {
        return std::nullopt;
    }
    if( name.find( ".." ) != std::string::npos || name.find( "//" ) != std::string::npos ) {
        return std::nullopt;
    }
    for( const char c : name ) {
        const bool ok = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ||
                        c == '_' || c == '-' || c == '.' || c == '/' || c == ' ';
        if( !ok ) {
            return std::nullopt;
        }
    }
    std::filesystem::path root = std::filesystem::u8path( PATH_INFO::config_dir() ) / "mod_data";
    return root / std::filesystem::u8path( name );
}

} // namespace

// =============================================================================================
// public interface
// =============================================================================================
void shutdown_windows()
{
    drag() = drag_state();
    pending_commands().clear();
    open_windows().clear();
    restore_needed() = true;
}

void reset()
{
    shutdown_windows();
    realtime_registry().clear();
    window_registry().clear();
    option_registry().clear();
}

void load_realtime_eoc( const JsonObject &jo, const std::string &src )
{
    realtime_entry entry;
    entry.id = jo.get_string( "id" );
    if( entry.id.empty() ) {
        jo.throw_error_at( "id", "realtime_eoc needs a non-empty id" );
    }
    entry.interval = std::max( 0.1, jo.get_float( "interval", 1.0 ) );
    if( !jo.has_member( "eoc" ) ) {
        jo.throw_error( "realtime_eoc needs an \"eoc\" (an effect_on_condition id or an inline effect_on_condition)" );
    }
    entry.eoc = effect_on_conditions::load_inline_eoc( jo.get_member( "eoc" ), src );
    entry.next_run = clock_type::now() + std::chrono::milliseconds( static_cast<int>
                     ( entry.interval * 1000 ) );
    std::vector<realtime_entry> &all = realtime_registry();
    auto it = std::find_if( all.begin(), all.end(), [&entry]( const realtime_entry & e ) {
        return e.id == entry.id;
    } );
    if( it != all.end() ) {
        *it = entry;
    } else {
        all.push_back( entry );
    }
}

void load_window( const JsonObject &jo, const std::string &src )
{
    window_def def;
    def.id = jo.get_string( "id" );
    if( def.id.empty() ) {
        jo.throw_error_at( "id", "mod_window needs a non-empty id" );
    }
    def.title = jo.get_string( "title", "" );
    def.width = jo.get_int( "width", 32 );
    if( jo.has_member( "x" ) || jo.has_member( "y" ) ) {
        def.default_pos = point( jo.get_int( "x", 2 ), jo.get_int( "y", 2 ) );
    }
    def.remember_open = jo.get_bool( "remember_open", false );
    const auto read_align = []( const JsonObject & obj ) {
        const std::string a = obj.get_string( "align", "left" );
        if( a == "center" ) {
            return align_type::center;
        } else if( a == "right" ) {
            return align_type::right;
        } else if( a == "left" ) {
            return align_type::left;
        }
        obj.throw_error_at( "align", "align must be \"left\", \"center\" or \"right\"" );
    };
    for( JsonValue row_val : jo.get_array( "rows" ) ) {
        row_def row;
        if( row_val.test_string() ) {
            row.text = row_val.get_string();
        } else {
            JsonObject ro = row_val.get_object();
            row.align = read_align( ro );
            if( ro.has_array( "buttons" ) ) {
                for( JsonObject bo : ro.get_array( "buttons" ) ) {
                    button_def b;
                    b.text = bo.get_string( "text" );
                    if( !bo.has_member( "eoc" ) ) {
                        bo.throw_error( "a button needs an \"eoc\" (an effect_on_condition id or an inline effect_on_condition)" );
                    }
                    b.eoc = effect_on_conditions::load_inline_eoc( bo.get_member( "eoc" ), src );
                    row.buttons.push_back( b );
                }
                if( row.buttons.empty() ) {
                    ro.throw_error_at( "buttons", "a button row needs at least one button" );
                }
            } else {
                row.text = ro.get_string( "text" );
            }
        }
        def.rows.push_back( std::move( row ) );
    }
    if( def.rows.empty() ) {
        jo.throw_error_at( "rows", "mod_window needs at least one row" );
    }
    window_registry()[def.id] = std::move( def );
}

void load_option( const JsonObject &jo, const std::string &src )
{
    option_def opt;
    opt.id = jo.get_string( "id" );
    if( opt.id.empty() ) {
        jo.throw_error_at( "id", "mod_option needs a non-empty id" );
    }
    opt.source = src;
    opt.name = jo.get_string( "name", opt.id );
    opt.description = jo.get_string( "description", "" );
    opt.kind = jo.get_string( "kind", "bool" );
    if( opt.kind == "bool" ) {
        opt.default_value = jo.get_bool( "default", false ) ? "true" : "false";
    } else if( opt.kind == "int" ) {
        opt.min_value = jo.get_int( "min", 0 );
        opt.max_value = jo.get_int( "max", 100 );
        if( opt.max_value < opt.min_value ) {
            jo.throw_error_at( "max", "max must not be smaller than min" );
        }
        opt.step = std::max( 1, jo.get_int( "step", 1 ) );
        opt.default_value = std::to_string( std::clamp( jo.get_int( "default", opt.min_value ), opt.min_value,
                                            opt.max_value ) );
    } else if( opt.kind == "list" ) {
        for( JsonObject co : jo.get_array( "choices" ) ) {
            opt.choices.emplace_back( co.get_string( "value" ), co.get_string( "name" ) );
        }
        if( opt.choices.empty() ) {
            jo.throw_error_at( "choices", "a list option needs at least one choice" );
        }
        opt.default_value = jo.get_string( "default", opt.choices.front().first );
        const bool known = std::any_of( opt.choices.begin(), opt.choices.end(),
        [&opt]( const std::pair<std::string, std::string> &c ) {
            return c.first == opt.default_value;
        } );
        if( !known ) {
            jo.throw_error_at( "default", "the default must be one of the choices" );
        }
    } else if( opt.kind == "text" ) {
        opt.default_value = jo.get_string( "default", "" );
    } else {
        jo.throw_error_at( "kind", "kind must be \"bool\", \"int\", \"list\" or \"text\"" );
    }
    if( jo.has_member( "on_change" ) ) {
        opt.on_change = effect_on_conditions::load_inline_eoc( jo.get_member( "on_change" ), src );
        opt.has_on_change = true;
    }
    option_registry()[opt.id] = std::move( opt );
}

void tick()
{
    static bool busy = false;
    if( busy || g == nullptr ) {
        return;
    }
    busy = true;
    try {
        // 1. commands that mods gave to windows since the last tick
        std::vector<std::pair<std::string, std::string>> cmds;
        cmds.swap( pending_commands() );
        for( const std::pair<std::string, std::string> &cmd : cmds ) {
            apply_command( cmd.first, cmd.second );
        }

        // 2. windows that were open when the player last left the game
        if( restore_needed() ) {
            restore_needed() = false;
            ensure_window_states();
            for( const std::pair<const std::string, window_def> &def : window_registry() ) {
                const auto st = window_states().find( def.first );
                if( def.second.remember_open && st != window_states().end() && st->second.open ) {
                    open_window( def.first );
                }
            }
        }

        // 3. timers
        const clock_type::time_point now = clock_type::now();
        std::vector<realtime_entry> &timers = realtime_registry();
        for( size_t i = 0; i < timers.size(); i++ ) {
            realtime_entry &t = timers[i];
            if( t.broken || now < t.next_run ) {
                continue;
            }
            t.next_run = now + std::chrono::milliseconds( static_cast<int>( t.interval * 1000 ) );
            if( !t.eoc.is_valid() ) {
                debugmsg( "realtime_eoc %s refers to an unknown effect_on_condition (%s).  It is switched off.",
                          t.id, t.eoc.str() );
                t.broken = true;
                continue;
            }
            run_eoc( t.eoc, "realtime_eoc " + t.id );
        }

        // 4. sidebar widgets with a "mod_text" need a redraw now and then to stay current
        static clock_type::time_point next_sidebar = clock_type::now();
        static bool sidebar_uses_mod_text = false;
        if( now >= next_sidebar ) {
            next_sidebar = now + std::chrono::milliseconds( 1000 );
            sidebar_uses_mod_text = widget::any_uses_mod_text();
            if( sidebar_uses_mod_text ) {
                g->invalidate_main_ui_adaptor();
            }
        }

        // 5. live content of the windows
        for( std::unique_ptr<window_instance> &w : open_windows() ) {
            if( refresh_layout( *w ) && w->ui ) {
                w->ui->invalidate_ui();
            }
        }
    } catch( const std::exception &err ) {
        debugmsg( "mod_engine tick failed: %s", err.what() );
    }
    busy = false;
}

bool handle_mouse( input_context &ctxt, const std::string &action )
{
    if( open_windows().empty() ) {
        return false;
    }
    const std::optional<point> cell = ctxt.get_coordinates_text( catacurses::stdscr );
    drag_state &ds = drag();

    if( action == "MOUSE_MOVE" ) {
        if( ds.kind == drag_state::kind_t::window ) {
            window_instance *w = find_instance( ds.id );
            if( w == nullptr ) {
                ds = drag_state();
                return false;
            }
            if( cell ) {
                w->pos = clamp_position( *w, *cell - ds.grab );
                place_window( *w );
            }
            return true;
        }
        if( ds.kind == drag_state::kind_t::button ) {
            return true;
        }
        if( !cell ) {
            set_hover( nullptr, -1, -1 );
            return false;
        }
        const hit_result hit = hit_test( *cell );
        set_hover( hit.win, hit.row, hit.button );
        return false;
    }

    if( !cell ) {
        if( action == "SELECT" && ds.kind != drag_state::kind_t::none ) {
            ds = drag_state();
            return true;
        }
        return false;
    }

    if( action == "CLICK_AND_DRAG" ) {
        const hit_result hit = hit_test( *cell );
        if( hit.win == nullptr ) {
            return false;
        }
        if( hit.button >= 0 ) {
            ds.kind = drag_state::kind_t::button;
            ds.id = hit.win->id;
            ds.row = hit.row;
            ds.button = hit.button;
            set_hover( hit.win, hit.row, hit.button );
        } else {
            const std::string win_id = hit.win->id;
            const point grab = *cell - hit.win->pos;
            bring_to_front( hit.win ); // hit.win is not valid after this
            ds.kind = drag_state::kind_t::window;
            ds.id = win_id;
            ds.grab = grab;
        }
        return true;
    }

    if( action == "SELECT" ) {
        if( ds.kind == drag_state::kind_t::none ) {
            // A click on a window that did not start on it (e.g. drag from the map): ignore it.
            return hit_test( *cell ).win != nullptr;
        }
        const drag_state finished = ds;
        ds = drag_state();
        if( finished.kind == drag_state::kind_t::window ) {
            if( window_instance *w = find_instance( finished.id ) ) {
                ensure_window_states();
                window_state &st = window_states()[finished.id];
                st.pos = w->pos;
                st.has_pos = true;
                save_window_states();
            }
            return true;
        }
        const hit_result hit = hit_test( *cell );
        if( hit.win != nullptr && hit.win->id == finished.id && hit.row == finished.row &&
            hit.button == finished.button ) {
            const auto def_it = window_registry().find( finished.id );
            if( def_it != window_registry().end() && finished.row < static_cast<int>( def_it->second.rows.size() ) ) {
                const row_def &row = def_it->second.rows[finished.row];
                if( finished.button < static_cast<int>( row.buttons.size() ) ) {
                    // copy: the effect may close the window
                    const effect_on_condition_id eoc = row.buttons[finished.button].eoc;
                    run_eoc( eoc, "a button of the window " + finished.id );
                }
            }
        }
        return true;
    }

    if( action == "SEC_SELECT" ) {
        return hit_test( *cell ).win != nullptr;
    }
    return false;
}

void window_command( const std::string &command, const std::string &id )
{
    pending_commands().emplace_back( command, id );
}

bool window_is_open( const std::string &id )
{
    return find_instance( id ) != nullptr;
}

std::string expand_text( const std::string &text )
{
    if( text.find( '{' ) == std::string::npos ) {
        return text;
    }
    std::string out;
    out.reserve( text.size() + 16 );
    size_t i = 0;
    while( i < text.size() ) {
        if( text[i] == '{' ) {
            const size_t close = text.find( '}', i );
            if( close != std::string::npos ) {
                const std::optional<std::string> value = resolve_placeholder( text.substr( i + 1,
                        close - i - 1 ) );
                if( value ) {
                    out += *value;
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

bool has_options()
{
    return !option_registry().empty();
}

bool option_exists( const std::string &id )
{
    return option_registry().count( id ) > 0;
}

std::string option_get( const std::string &id )
{
    const auto it = option_registry().find( id );
    if( it == option_registry().end() ) {
        return std::string();
    }
    ensure_option_values();
    const auto val = option_values().find( id );
    return val == option_values().end() ? it->second.default_value : val->second;
}

std::string option_kind( const std::string &id )
{
    const auto it = option_registry().find( id );
    return it == option_registry().end() ? std::string() : it->second.kind;
}

double option_get_number( const std::string &id )
{
    const auto it = option_registry().find( id );
    if( it == option_registry().end() ) {
        return 0.0;
    }
    const std::string value = option_get( id );
    if( it->second.kind == "bool" ) {
        return value == "true" ? 1.0 : 0.0;
    } else if( it->second.kind == "int" ) {
        return atof( value.c_str() );
    }
    return 0.0;
}

bool option_set( const std::string &id, const std::string &value_in )
{
    const auto it = option_registry().find( id );
    if( it == option_registry().end() ) {
        return false;
    }
    const option_def &opt = it->second;
    std::string value = value_in;
    if( opt.kind == "bool" ) {
        if( value == "1" || value == "true" ) {
            value = "true";
        } else if( value == "0" || value == "false" ) {
            value = "false";
        } else {
            return false;
        }
    } else if( opt.kind == "int" ) {
        char *end = nullptr;
        const double parsed = strtod( value.c_str(), &end );
        if( end == value.c_str() ) {
            return false;
        }
        value = std::to_string( std::clamp( static_cast<int>( std::lround( parsed ) ), opt.min_value,
                                            opt.max_value ) );
    } else if( opt.kind == "list" ) {
        const bool known = std::any_of( opt.choices.begin(), opt.choices.end(),
        [&value]( const std::pair<std::string, std::string> &c ) {
            return c.first == value;
        } );
        if( !known ) {
            return false;
        }
    } else if( value.size() > 200 ) {
        value.resize( 200 );
    }
    ensure_option_values();
    if( option_get( id ) == value ) {
        return true;
    }
    option_values()[id] = value;
    save_option_values();
    if( opt.has_on_change && g != nullptr ) {
        const effect_on_condition_id eoc = opt.on_change;
        run_eoc( eoc, "the on_change of the mod option " + id );
    }
    return true;
}

void options_menu()
{
    if( option_registry().empty() ) {
        popup( _( "No mod has options." ) );
        return;
    }
    int last = 0;
    while( true ) {
        std::vector<const option_def *> list;
        for( const std::pair<const std::string, option_def> &o : option_registry() ) {
            list.push_back( &o.second );
        }
        std::sort( list.begin(), list.end(), []( const option_def * a, const option_def * b ) {
            return a->source == b->source ? a->name < b->name : a->source < b->source;
        } );
        uilist menu;
        menu.title = _( "Mod options" );
        menu.desc_enabled = true;
        for( size_t i = 0; i < list.size(); i++ ) {
            const option_def &o = *list[i];
            std::string shown = option_get( o.id );
            if( o.kind == "bool" ) {
                shown = shown == "true" ? _( "On" ) : _( "Off" );
            } else if( o.kind == "list" ) {
                for( const std::pair<std::string, std::string> &c : o.choices ) {
                    if( c.first == shown ) {
                        shown = c.second;
                    }
                }
            }
            const std::string desc = string_format( _( "From the mod \"%s\".\n%s" ), o.source, o.description );
            menu.addentry_desc( static_cast<int>( i ), true, MENU_AUTOASSIGN,
                                string_format( "%s: %s", o.name, shown ), desc );
        }
        menu.selected = std::min( last, static_cast<int>( list.size() ) - 1 );
        menu.query();
        if( menu.ret < 0 || menu.ret >= static_cast<int>( list.size() ) ) {
            return;
        }
        last = menu.ret;
        const option_def &o = *list[menu.ret];
        if( o.kind == "bool" ) {
            option_set( o.id, option_get( o.id ) == "true" ? "false" : "true" );
        } else if( o.kind == "int" ) {
            string_input_popup popup_in;
            popup_in.title( string_format( _( "%s (%d to %d):" ), o.name, o.min_value, o.max_value ) )
            .width( 12 ).text( option_get( o.id ) );
            const std::optional<int> val = popup_in.query_int();
            if( !popup_in.canceled() && val ) {
                option_set( o.id, std::to_string( *val ) );
            }
        } else if( o.kind == "list" ) {
            uilist choice;
            choice.title = o.name;
            const std::string current = option_get( o.id );
            for( size_t c = 0; c < o.choices.size(); c++ ) {
                choice.addentry( static_cast<int>( c ), true, MENU_AUTOASSIGN, o.choices[c].second );
                if( o.choices[c].first == current ) {
                    choice.selected = static_cast<int>( c );
                }
            }
            choice.query();
            if( choice.ret >= 0 && choice.ret < static_cast<int>( o.choices.size() ) ) {
                option_set( o.id, o.choices[choice.ret].first );
            }
        } else {
            string_input_popup popup_in;
            popup_in.title( o.name + ":" ).width( 40 ).text( option_get( o.id ) );
            const std::string val = popup_in.query_string();
            if( !popup_in.canceled() ) {
                option_set( o.id, val );
            }
        }
    }
}

bool file_write( const std::string &name, const std::string &text, bool append )
{
    const std::optional<std::filesystem::path> path = sandbox_path( name );
    if( !path || text.size() > 1000000 ) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories( path->parent_path(), ec );
    if( ec ) {
        return false;
    }
    if( append ) {
        std::error_code size_ec;
        const auto size = std::filesystem::file_size( *path, size_ec );
        if( !size_ec && size + text.size() > 5000000 ) {
            return false;
        }
    }
    std::ofstream fout( *path, std::ios::binary | ( append ? std::ios::app : std::ios::trunc ) );
    if( !fout ) {
        return false;
    }
    fout << text;
    return static_cast<bool>( fout );
}

std::optional<std::string> file_read( const std::string &name )
{
    const std::optional<std::filesystem::path> path = sandbox_path( name );
    if( !path ) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size( *path, ec );
    if( ec || size > 5000000 ) {
        return std::nullopt;
    }
    std::ifstream fin( *path, std::ios::binary );
    if( !fin ) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << fin.rdbuf();
    return buffer.str();
}
} // namespace mod_engine
