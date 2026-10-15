#include "mp_menu.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "cata_path.h"
#include "color.h"
#include "filesystem.h"
#include "net_protocol.h"
#include "net_session.h"
#include "options.h"
#include "output.h"
#include "path_info.h"
#include "popup.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "uilist.h"
#include "worldfactory.h"

namespace mp_menu
{

namespace
{
// ---- remembered choices (config/mp_menu.txt, "key=value" lines)

using settings = std::map<std::string, std::string>;

cata_path settings_file()
{
    return PATH_INFO::config_dir_path() / "mp_menu.txt";
}

settings load_settings()
{
    settings s;
    std::ifstream in( settings_file().get_unrelative_path() );
    std::string line;
    while( std::getline( in, line ) ) {
        const size_t eq = line.find( '=' );
        if( eq != std::string::npos ) {
            s[line.substr( 0, eq )] = line.substr( eq + 1 );
        }
    }
    return s;
}

void save_settings( const settings &s )
{
    assure_dir_exist( PATH_INFO::config_dir_path() );
    std::ofstream out( settings_file().get_unrelative_path() );
    for( const auto &e : s ) {
        out << e.first << "=" << e.second << "\n";
    }
}

std::string get( const settings &s, const std::string &key, const std::string &fallback )
{
    const auto iter = s.find( key );
    return iter == s.end() || iter->second.empty() ? fallback : iter->second;
}

int get_int( const settings &s, const std::string &key, int fallback )
{
    const std::string v = get( s, key, "" );
    return v.empty() ? fallback : std::atoi( v.c_str() );
}

// ---- the world's multiplayer rules (world options, saved with the world)

struct rule {
    const char *option;
    // values to cycle through and how they are shown
    std::vector<std::pair<std::string, std::string>> values;
    const char *label;
};

const std::vector<rule> &rules()
{
    static const std::vector<rule> r = {
        {
            "MP_GROUPING", {
                { "shared", _( "everybody together" ) },
                { "solo", _( "everyone on their own (groups made in the chat)" ) }
            }, _( "Players start" )
        },
        {
            "MP_LIVES_DIFFICULTY", {
                { "tourist", _( "tourist (unlimited)" ) }, { "easy", _( "easy (3)" ) },
                { "normal", _( "normal (2)" ) }, { "hard", _( "hard (2, items stay)" ) },
                { "very_hard", _( "very hard (1, items stay)" ) }
            }, _( "Extra lives" )
        },
        { "MP_PVP", { { "false", _( "off" ) }, { "true", _( "on" ) } }, _( "Players can hurt other groups (PvP)" ) },
        { "MP_FRIENDLY_FIRE", { { "true", _( "on" ) }, { "false", _( "off" ) } }, _( "Friendly fire" ) },
        { "MP_CLIENT_DEBUG", { { "true", _( "on" ) }, { "false", _( "off" ) } }, _( "Debug menu for clients" ) },
    };
    return r;
}

std::string option_value( WORLD *world, const std::string &key )
{
    if( world != nullptr ) {
        const auto iter = world->WORLD_OPTIONS.find( key );
        if( iter != world->WORLD_OPTIONS.end() ) {
            return iter->second.getValue();
        }
    }
    return get_options().get_option( key ).getValue();
}

void set_option_value( WORLD *world, const std::string &key, const std::string &value )
{
    if( world == nullptr ) {
        return;
    }
    auto iter = world->WORLD_OPTIONS.find( key );
    if( iter == world->WORLD_OPTIONS.end() ) {
        // a world made before the option existed
        iter = world->WORLD_OPTIONS.emplace( key, get_options().get_option( key ) ).first;
    }
    iter->second.setValue( value );
    world->save();
}

std::string shown_value( const rule &r, const std::string &value )
{
    for( const auto &v : r.values ) {
        if( v.first == value ) {
            return v.second;
        }
    }
    return value;
}

// ---- small helpers

bool ask_number( const std::string &title, int &value, int lo, int hi )
{
    string_input_popup pop;
    pop.title( title ).width( 10 ).text( std::to_string( value ) ).only_digits( true );
    pop.query();
    if( pop.canceled() || pop.text().empty() ) {
        return false;
    }
    value = std::clamp( std::atoi( pop.text().c_str() ), lo, hi );
    return true;
}

// a world to use: an existing one, or a new one made right now
std::string choose_world( const std::string &current, bool allow_new )
{
    std::vector<std::string> names = world_generator->all_worldnames();
    uilist menu;
    menu.text = _( "Which world?" );
    int i = 0;
    if( allow_new ) {
        menu.addentry( i++, true, 'n', _( "Make a new world…" ) );
    }
    for( const std::string &name : names ) {
        const WORLD *w = world_generator->get_world( name );
        const int saves = w != nullptr ? static_cast<int>( w->world_saves.size() ) : 0;
        menu.addentry( i, true, MENU_AUTOASSIGN, "%s%s", name,
                       saves == 0 ? std::string( _( "  (no character yet)" ) ) : std::string() );
        if( name == current ) {
            menu.selected = i;
        }
        i++;
    }
    menu.query();
    if( menu.ret < 0 ) {
        return current;
    }
    if( allow_new && menu.ret == 0 ) {
        WORLD *made = world_generator->make_new_world( true );
        return made != nullptr ? made->world_name : current;
    }
    const size_t idx = static_cast<size_t>( menu.ret - ( allow_new ? 1 : 0 ) );
    return idx < names.size() ? names[idx] : current;
}

std::string join_info( int port )
{
    const std::string lan = net::local_address();
    std::string text = string_format( _( "How others join (Multiplayer → Join a game):\n"
                                         "• on this computer: <color_light_green>127.0.0.1:%d</color>\n" ), port );
    if( !lan.empty() ) {
        text += string_format( _( "• in your home network: <color_light_green>%s:%d</color>\n" ), lan, port );
    }
    text += string_format( _( "• over the internet: your public address (search \"what is my ip\") with port %d.  "
                              "Your router must forward <color_yellow>UDP port %d</color> to this computer%s, "
                              "and the firewall must let the game through (Windows asks the first time).\n" ),
                           port, port, lan.empty() ? std::string() : string_format( " (%s)", lan ) );
    text += _( "Everybody needs this same build and the same mods." );
    return text;
}

bool join_now( settings &s, const std::string &address, const std::string &server_world = std::string() );

// the world this game loads now (host and play, join)
std::string &chosen_world()
{
    static std::string w;
    return w;
}

// ---- host / dedicated server

// true: hosting is set up (play too: chosen_world() is the world this game loads now)
bool host_menu( settings &s, bool play_too_default )
{
    std::string world = get( s, "world", "" );
    if( world.empty() || !world_generator->has_world( world ) ) {
        const std::vector<std::string> names = world_generator->all_worldnames();
        world = names.empty() ? std::string() : names.front();
    }
    // (the file can be edited by hand: the values are checked like typed ones)
    int port = std::clamp( get_int( s, "port", net::default_port ), 1024, 65535 );
    int players = std::clamp( get_int( s, "players", 4 ), 1, net::max_clients );
    bool play_too = play_too_default;

    enum entry : int { e_world = 0, e_play, e_port, e_players, e_info, e_start, e_rules = 100 };
    while( true ) {
        WORLD *w = world.empty() ? nullptr : world_generator->get_world( world );
        uilist menu;
        menu.text = play_too ?
                    _( "Host a game: the world runs in a server in the background (a terminal window shows its progress), "
                       "you play in this window." ) :
                    _( "Dedicated server: the world runs on its own (a terminal window shows it); nobody plays on this "
                       "computer unless they join it like everybody else." );
        menu.addentry( e_world, true, 'w', string_format( _( "World: <color_white>%s</color>%s" ),
                       world.empty() ? std::string( _( "(make one)" ) ) : world,
                       w != nullptr && w->world_saves.empty() ? std::string( _( "  (new: everybody makes a character when they join)" ) ) :
                       std::string() ) );
        menu.addentry( e_play, true, 'p', string_format( _( "Play on this computer too: <color_white>%s</color>" ),
                       play_too ? _( "yes" ) : _( "no (dedicated server)" ) ) );
        menu.addentry( e_port, true, 'o', string_format( _( "Port: <color_white>%d</color>  (UDP)" ), port ) );
        menu.addentry( e_players, true, 'm', string_format( _( "Players at most: <color_white>%d</color>" ),
                       players ) );
        int key = '1';
        for( size_t i = 0; i < rules().size(); ++i ) {
            const rule &r = rules()[i];
            menu.addentry( e_rules + static_cast<int>( i ), w != nullptr, key++, string_format( "%s: <color_white>%s</color>",
                           r.label, shown_value( r, option_value( w, r.option ) ) ) );
        }
        menu.addentry( e_info, true, 'i', _( "How do the others join?" ) );
        menu.addentry( e_start, !world.empty(), 's', play_too ? _( "<color_light_green>Start hosting</color>" ) :
                       _( "<color_light_green>Start the server</color>" ) );
        menu.desc_enabled = true;
        menu.footer_text = _( "The rules are saved with the world.  More world options: World → the world → options." );
        menu.query();

        if( menu.ret < 0 ) {
            return false;
        }
        if( menu.ret >= e_rules ) {
            const rule &r = rules()[menu.ret - e_rules];
            const std::string cur = option_value( w, r.option );
            size_t idx = 0;
            for( size_t i = 0; i < r.values.size(); ++i ) {
                if( r.values[i].first == cur ) {
                    idx = i;
                }
            }
            set_option_value( w, r.option, r.values[( idx + 1 ) % r.values.size()].first );
            continue;
        }
        switch( menu.ret ) {
            case e_world:
                world = choose_world( world, true );
                break;
            case e_play:
                play_too = !play_too;
                break;
            case e_port:
                ask_number( _( "Port (1024-65535, the others need the same):" ), port, 1024, 65535 );
                break;
            case e_players:
                ask_number( _( "How many players at most (1-32)?" ), players, 1, net::max_clients );
                break;
            case e_info:
                popup( join_info( port ) );
                break;
            case e_start: {
                s["world"] = world;
                s["port"] = std::to_string( port );
                s["players"] = std::to_string( players );
                s["join"] = string_format( "127.0.0.1:%d", port );
                save_settings( s );
                const std::vector<std::string> common = {
                    "--world", world, "--mp-max-players", std::to_string( players )
                };
                if( play_too ) {
                    // this game becomes the host's client right here (no second game): the server
                    // starts in the background with its terminal window, then the world loads
                    net::set_max_players( players );
                    net::request_listen( port );
                    if( !net::prepare_listen( world, "cataclysm-tiles" ) ) {
                        popup( _( "The server could not be started.  Start it with: --world \"%s\" --mp-host %d" ),
                               world, port );
                        break;
                    }
                    chosen_world() = world;
                    return true;
                }
                std::vector<std::string> args = common;
                args.emplace_back( "--mp-server" );
                args.emplace_back( std::to_string( port ) );
                if( !net::launch_game( args ) ) {
                    popup( _( "The server could not be started.  Start it with: --world \"%s\" --mp-server %d" ),
                           world, port );
                    break;
                }
                if( query_yn( _( "The server starts in a terminal window of its own (it saves by itself; closing that window saves and stops it).\n\n%s\n\n"
                                 "Join it now from this computer?" ), join_info( port ) ) ) {
                    net::retry_join_while_loading();
                    return join_now( s, string_format( "127.0.0.1:%d", port ), world );
                }
                return false;
            }
            default:
                break;
        }
    }
}

bool join_menu( settings &s )
{
    std::string address = get( s, "join", string_format( "127.0.0.1:%d", net::default_port ) );
    string_input_popup pop;
    pop.title( _( "Address of the game to join (address:port):" ) ).width( 40 ).text( address );
    pop.description( _( "On this computer: 127.0.0.1:7777.  The host sees the address under Multiplayer → Host → How do the others join?" ) );
    pop.query();
    if( pop.canceled() || pop.text().empty() ) {
        return false;
    }
    return join_now( s, pop.text() );
}

// Starts this game again as a client of @p address.  The client needs a world of its own (it only
// shows the host's world): the last one used, or a new one with the default mods.
bool join_now( settings &s, const std::string &address, const std::string &server_world )
{
    // the client's own world (it only shows the host's world); never the world a server on this
    // computer runs: that folder is the server's
    std::string world = get( s, "client_world", "" );
    if( world.empty() || world == server_world || !world_generator->has_world( world ) ) {
        std::vector<std::string> names = world_generator->all_worldnames();
        names.erase( std::remove( names.begin(), names.end(), server_world ), names.end() );
        if( names.size() == 1 ) {
            world = names.front();
        } else if( !names.empty() ) {
            uilist menu;
            menu.text = _( "Which world shows the game on this computer?  (Only its mods matter; it is not changed.)" );
            for( size_t i = 0; i < names.size(); ++i ) {
                menu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, names[i] );
            }
            menu.query();
            if( menu.ret < 0 ) {
                return false;
            }
            world = names[menu.ret];
        } else {
            WORLD *made = world_generator->make_new_world( false );
            world = made != nullptr ? made->world_name : std::string();
        }
    }
    if( world.empty() ) {
        popup( _( "Joining needs a world on this computer (with the same mods as the host).  Make one first." ) );
        return false;
    }
    s["join"] = address;
    s["client_world"] = world;
    save_settings( s );
    // this game joins right here (no second game): connect once the client world is loaded
    std::string host = address;
    int port = net::default_port;
    const size_t colon = host.rfind( ':' );
    if( colon != std::string::npos ) {
        port = std::atoi( host.substr( colon + 1 ).c_str() );
        host = host.substr( 0, colon );
    }
    if( port <= 0 || port > 65535 ) {
        port = net::default_port;
    }
    net::request_connect( host, port );
    chosen_world() = world;
    return true;
}
} // namespace

std::string open()
{
    chosen_world().clear();
    settings s = load_settings();
    while( true ) {
        uilist menu;
        menu.text = _( "Multiplayer" );
        menu.addentry( 0, true, 'h', _( "Host a game (you play too)" ) );
        menu.addentry( 1, true, 'd', _( "Start a dedicated server (runs the world on its own)" ) );
        menu.addentry( 2, true, 'j', string_format( _( "Join a game  (last: %s)" ), get( s, "join", "-" ) ) );
        menu.addentry( 3, true, 'i', _( "How does this work?" ) );
        menu.query();
        switch( menu.ret ) {
            case 0:
                if( host_menu( s, true ) ) {
                    return chosen_world();
                }
                break;
            case 1:
                if( host_menu( s, false ) ) {
                    return chosen_world();
                }
                break;
            case 2:
                if( join_menu( s ) ) {
                    return chosen_world();
                }
                break;
            case 3:
                popup( _( "<color_white>Host a game</color>: you play in this game; the world runs in a server in the "
                          "background (so your menus never stop it for the others), its progress shows in a small terminal "
                          "window.  Leaving the game saves the world and ends the session.\n\n"
                          "<color_white>Dedicated server</color>: only the world runs, shown in a terminal window; players "
                          "(you too) join it.  It keeps running when everybody leaves and saves by itself; closing its "
                          "window saves and stops it.\n\n"
                          "<color_white>Join</color>: enter the host's address.  Your character is made when you join "
                          "the first time and kept on the host; joining again puts you back into it.\n\n"
                          "A new world is fine: the server waits with the world's time until the first player is in.\n\n"
                          "Everybody needs the same build and the same mods." ) );
                break;
            default:
                return std::string();
        }
    }
}

} // namespace mp_menu
