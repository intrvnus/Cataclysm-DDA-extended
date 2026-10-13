// Multiplayer client: shows the world the host sends and sends key presses to the host.
// It does not simulate anything itself. See net_session.h.

#include <algorithm>
#include <random>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "action.h"
#include "alerts.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "character_id.h"
#include "coordinates.h"
#include "debug.h"
#include "faction.h"
#include "filesystem.h"
#include "flexbuffer_json.h"
#include "fluid.h"
#include "game.h"
#include "game_inventory.h"
#include "input_context.h"
#include "item_location.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "mapbuffer.h"
#include "memory_fast.h"
#include "messages.h"
#include "mod_engine.h"
#include "monster.h"
#include "net_protocol.h"
#include "net_session.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "point.h"
#include "sounds.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"

namespace net
{
namespace
{

struct client_state {
    // B6: the own character sleeps on the host (any key wakes it up)
    bool asleep = false;
    bool connected = false;
    bool welcomed = false;
    bool lost = false;
    // fluid mode (see fluid.h): negotiated with the host in "welcome"
    bool fluid_on = false;
    // character id of the NPC that stands for this client on the host
    int my_id = 0;
    // host character id -> NPC on this client that shows that player
    std::map<int, character_id> shadows;
};

client_state &cstate()
{
    static client_state s;
    return s;
}

tripoint_abs_ms read_pos( JsonArray ja )
{
    const int x = ja.next_int();
    const int y = ja.next_int();
    const int z = ja.next_int();
    return tripoint_abs_ms( x, y, z );
}

std::string message( const std::function<void( JsonOut & )> &body )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    body( jo );
    jo.end_object();
    return os.str();
}

void send_action( const std::string &action, const point_rel_ms &delta = point_rel_ms::zero )
{
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "act" );
        jo.member( "a", action );
        if( delta != point_rel_ms::zero ) {
            jo.member( "dx", delta.x() );
            jo.member( "dy", delta.y() );
        }
    } ) );
}

// Moves the avatar of this client to where its NPC is on the host.
void place_avatar( const tripoint_abs_ms &p )
{
    avatar &u = get_avatar();
    if( u.pos_abs() == p ) {
        return;
    }
    map &here = get_map();
    if( p.z() != u.posz() ) {
        g->vertical_shift( p.z() );
    }
    if( !here.inbounds( p ) ) {
        g->place_player_overmap( project_to<coords::omt>( p ) );
    }
    map &now = get_map();
    u.setpos( now, now.get_bub( p ), false );
    g->update_map( u );
}

void reload_map()
{
    map &here = get_map();
    here.load( here.get_abs_sub(), true );
    g->load_npcs();
    g->invalidate_main_ui_adaptor();
}

void apply_submaps( const JsonArray &list )
{
    // The replaced submaps own their vehicles: the map must not keep pointers to them.
    get_map().net_forget_vehicles();
    try {
        MAPBUFFER.net_replace_submaps( list );
    } catch( ... ) {
        // Some submaps may already be replaced: never keep pointers to the old ones.
        reload_map();
        throw;
    }
    // The map still points at the replaced submaps: load it again.
    reload_map();
}

npc *shadow_for( int host_id, const std::string &name, const tripoint_abs_ms &p )
{
    client_state &cs = cstate();
    auto iter = cs.shadows.find( host_id );
    if( iter != cs.shadows.end() ) {
        if( npc *existing = g->find_npc( iter->second ) ) {
            return existing;
        }
    }
    shared_ptr_fast<npc> guy = make_shared_fast<npc>();
    guy->normalize();
    guy->randomize();
    guy->name = name;
    guy->spawn_at_precise( p );
    overmap_buffer.insert_npc( guy );
    guy->set_attitude( NPCATT_NULL );
    guy->mission = NPC_MISSION_NULL;
    guy->set_value( "mp_shadow_player", "1" );
    cs.shadows[host_id] = guy->getID();
    g->load_npcs();
    return g->find_npc( guy->getID() );
}

void apply_state( const JsonObject &msg )
{
    map &here = get_map();
    avatar &u = get_avatar();

    calendar::turn = calendar::turn_zero + time_duration::from_turns( msg.get_int( "turn" ) );
    // C14: weather (and with it the light) of the host
    const weather_type_id host_weather( msg.get_string( "weather", "" ) );
    if( host_weather.is_valid() && host_weather != WEATHER_NULL ) {
        weather_manager &weather = get_weather();
        if( weather.weather_id != host_weather ) {
            weather.weather_override = host_weather;
            weather.weather_id = host_weather;
            weather.set_nextweather( calendar::turn );
            g->reset_light_level();
        }
    }

    JsonObject you = msg.get_object( "you" );
    you.allow_omitted_members();
    place_avatar( read_pos( you.get_array( "pos" ) ) );
    JsonObject hp = you.get_object( "hp" );
    hp.allow_omitted_members();
    for( const bodypart_id &bp : u.get_all_body_parts() ) {
        if( hp.has_int( bp.id().str() ) ) {
            // Death is decided by the host; never let the client die by itself.
            u.set_part_hp_cur( bp, std::max( 1, hp.get_int( bp.id().str() ) ) );
        }
    }
    const bool asleep = you.get_bool( "asleep", false );
    if( asleep && !cstate().asleep ) {
        alerts::warn( _( "You are asleep.  The night passes faster when everybody sleeps.  Press a key to wake up." ),
                      alerts::level::notice );
    } else if( !asleep && cstate().asleep ) {
        alerts::warn( _( "You wake up." ), alerts::level::notice );
    }
    cstate().asleep = asleep;
    if( you.get_bool( "dead", false ) && !cstate().lost ) {
        cstate().lost = true;
        add_msg( m_bad, _( "Multiplayer: you died on the host." ) );
    }

    map &now = get_map();
    std::set<int> seen_ids;
    for( JsonObject other : msg.get_array( "others" ) ) {
        other.allow_omitted_members();
        const tripoint_abs_ms p = read_pos( other.get_array( "pos" ) );
        const int host_id = other.get_int( "id" );
        seen_ids.insert( host_id );
        npc *guy = shadow_for( host_id, other.get_string( "name", "" ), p );
        if( guy != nullptr && now.inbounds( p ) ) {
            guy->setpos( now, now.get_bub( p ), false );
        }
    }
    // People the host no longer has here leave the view; NPCs of the client's own world
    // (which is only a view) are hidden.
    client_state &cs = cstate();
    std::set<character_id> keep;
    for( auto iter = cs.shadows.begin(); iter != cs.shadows.end(); ) {
        if( seen_ids.count( iter->first ) == 0 ) {
            g->remove_npc( iter->second );
            overmap_buffer.remove_npc( iter->second );
            iter = cs.shadows.erase( iter );
        } else {
            keep.insert( iter->second );
            ++iter;
        }
    }
    std::vector<character_id> foreign;
    for( const npc &np : g->all_npcs() ) {
        if( keep.count( np.getID() ) == 0 ) {
            foreign.push_back( np.getID() );
        }
    }
    for( const character_id &id : foreign ) {
        g->remove_npc( id );
        overmap_buffer.remove_npc( id );
    }

    // Monsters: replaced completely with what the host has.
    g->clear_zombies();
    for( JsonObject mon : msg.get_array( "monsters" ) ) {
        mon.allow_omitted_members();
        const tripoint_abs_ms p = read_pos( mon.get_array( "pos" ) );
        const mtype_id type( mon.get_string( "id" ) );
        if( !type.is_valid() || !now.inbounds( p ) ) {
            continue;
        }
        if( monster *critter = g->place_critter_at( type, now.get_bub( p ) ) ) {
            critter->set_hp( mon.get_int( "hp", critter->get_hp() ) );
        }
    }
    ( void )here;
    // The client does not run do_turn(), so update what the player can see here, otherwise
    // monsters stay invisible.
    now.invalidate_visibility_cache();
    now.build_map_cache( u.posz() );
    g->mon_info_update();
    g->invalidate_main_ui_adaptor();
}

// Fluid mode: steps and blows of the players and monsters, as they happen on the host.
void apply_fx( const JsonArray &list )
{
    client_state &cs = cstate();
    const fluid::ms_t arrival = fluid::now_ms();
    for( JsonObject o : list ) {
        o.allow_omitted_members();
        const fluid::event ev = fluid::read_event( o );
        const bool mine = ev.actor != 0 && ev.actor == cs.my_id;
        if( ev.type == fluid::fx_type::step && ev.actor != 0 ) {
            if( mine ) {
                // The position is known now, no need to wait for the end of the turn.
                place_avatar( ev.to );
            } else {
                const auto iter = cs.shadows.find( ev.actor );
                map &now = get_map();
                if( iter != cs.shadows.end() && now.inbounds( ev.to ) ) {
                    if( npc *guy = g->find_npc( iter->second ) ) {
                        guy->setpos( now, now.get_bub( ev.to ), false );
                    }
                }
            }
        }
        if( cs.fluid_on ) {
            fluid::client_event( ev, arrival, mine );
        }
    }
    g->invalidate_main_ui_adaptor();
}

// A6: sends an item action chosen in a menu on the own (synced) character to the host.
void send_use( const std::string &action, const item_location &loc )
{
    if( !loc ) {
        return;
    }
    std::ostringstream os;
    JsonOut lj( os );
    loc.serialize( lj );
    const std::string loc_json = os.str();
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "use" );
        jo.member( "a", action );
        jo.member( "cid", get_avatar().getID().get_value() );
        jo.member( "loc", loc_json );
    } ) );
}

void send_drop( const drop_locations &what )
{
    if( what.empty() ) {
        return;
    }
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "use" );
        jo.member( "a", "drop" );
        jo.member( "cid", get_avatar().getID().get_value() );
        jo.member( "locs" );
        jo.start_array();
        for( const drop_location &d : what ) {
            std::ostringstream os;
            JsonOut lj( os );
            d.first.serialize( lj );
            jo.start_object();
            jo.member( "loc", os.str() );
            jo.member( "n", d.second );
            jo.end_object();
        }
        jo.end_array();
    } ) );
}

// Reads one key (or times out) and sends the matching command to the host.
// Returns true if the player wants to leave the game.
bool handle_input()
{
    input_context ctxt = get_default_mode_input_context();
    const bool fluid_on = cstate().fluid_on && fluid::client_active();
    // fluid mode: one frame is 16 ms, the screen moves smoothly
    ctxt.set_timeout( fluid_on ? fluid::tune().frame_ms : 50 );
    const std::string &action = ctxt.handle_input();
    if( action == "TIMEOUT" || action == "MOUSE_MOVE" ) {
        return false;
    }
    if( fluid_on && fluid::swallows( action ) ) {
        // movement and the mouse buttons are read as held state, see fluid::client_frame
        return false;
    }
    if( cstate().asleep ) {
        // B6: any key wakes the own character
        send_action( "wake" );
        return false;
    }
    if( ctxt.get_raw_input().get_first_input() == ' ' ) {
        // the focus minigame key (B3/B5); the host ignores it when no prompt is open
        send_action( "focus" );
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
            send_action( "move", get_delta_from_movement_action( act, iso_rotate::yes ) );
            break;
        case ACTION_PAUSE:
            // also stops a timed action (B7)
            send_action( "stop" );
            send_action( "wait" );
            break;
        case ACTION_PICKUP:
        case ACTION_PICKUP_ALL:
            send_action( "pickup" );
            break;
        case ACTION_SLEEP:
            send_action( "sleep" );
            break;
        // A6: menus run on the own character (a copy the host keeps up to date); the choice
        // is carried out by the host
        case ACTION_WIELD:
            send_use( "wield", game_menus::inv::wield() );
            break;
        case ACTION_WEAR:
            send_use( "wear", game_menus::inv::wear( get_avatar() ) );
            break;
        case ACTION_TAKE_OFF:
            send_use( "takeoff", game_menus::inv::take_off() );
            break;
        case ACTION_EAT:
            send_use( "consume", game_menus::inv::consume() );
            break;
        case ACTION_READ:
            send_use( "read", game_menus::inv::read( get_avatar() ) );
            break;
        case ACTION_DROP:
            send_drop( game_menus::inv::multidrop( get_avatar() ) );
            break;
        case ACTION_PL_INFO:
            get_avatar().disp_info();
            break;
        case ACTION_TOGGLE_RUN:
            send_action( "run" );
            break;
        case ACTION_TOGGLE_CROUCH:
            send_action( "crouch" );
            break;
        case ACTION_TOGGLE_PRONE:
            send_action( "prone" );
            break;
        case ACTION_LOOK:
            g->look_around();
            break;
        case ACTION_MAIN_MENU:
        case ACTION_SAVE:
            if( query_yn( _( "Leave the multiplayer game?  This world is only a view of the host's "
                             "world and will not be saved." ) ) ) {
                return true;
            }
            break;
        default:
            add_msg( m_info, _( "Not available in multiplayer yet." ) );
            break;
    }
    return false;
}

// A4: a secret that identifies this player to hosts, kept in the user folder.
std::string client_key()
{
    static std::string key;
    if( !key.empty() ) {
        return key;
    }
    const cata_path file = PATH_INFO::config_dir_path() / "mp_identity.txt";
    if( file_exist( file ) ) {
        read_from_file( file, [&]( std::istream & fin ) {
            fin >> key;
        } );
    }
    const bool ok = key.size() >= 8 && std::all_of( key.begin(), key.end(), []( char c ) {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0;
    } );
    if( !ok ) {
        std::random_device rd;
        static const char hex[] = "0123456789abcdef";
        key.clear();
        for( int i = 0; i < 32; ++i ) {
            key += hex[rd() % 16];
        }
        write_to_file( file, [&]( std::ostream & fout ) {
            fout << key;
        }, _( "multiplayer identity" ) );
    }
    return key;
}

// A4: the host's copy of the own character (inventory, skills, needs ...).
void apply_full_character( std::string json )
{
    // fields the avatar needs that an NPC save does not have
    const size_t open = json.find( '{' );
    if( open == std::string::npos ) {
        return;
    }
    if( json.find( "\"assigned_invlet\"" ) == std::string::npos ) {
        json.insert( open + 1, "\"assigned_invlet\":[]," );
    }
    avatar &u = get_avatar();
    const tripoint_abs_ms pos = u.pos_abs();
    const character_id own_id = u.getID();
    try {
        JsonValue jv = json_loader::from_string( std::move( json ) );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        u.deserialize( jo );
    } catch( const std::exception &err ) {
        DebugLog( D_WARNING, D_GAME ) << "Multiplayer: bad character data: " << err.what();
    }
    // the position is the client's business (see place_avatar); keep the local id
    u.setID( own_id, true );
    u.set_pos_abs_only( pos );
    g->invalidate_main_ui_adaptor();
}

} // namespace

namespace impl
{

void client_on_connect()
{
    cstate().connected = true;
    send_json( state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "hello" );
        jo.member( "v", protocol_version );
        jo.member( "name", get_avatar().get_name() );
        // A4: who we are (kept between sessions) and the character made for this game
        jo.member( "key", client_key() );
        std::ostringstream os;
        JsonOut cj( os );
        get_avatar().serialize( cj );
        jo.member( "char", os.str() );
    } ) );
    add_msg( m_good, _( "Multiplayer: connected, joining…" ) );
}

void client_on_disconnect()
{
    client_state &cs = cstate();
    if( cs.connected ) {
        add_msg( m_bad, _( "Multiplayer: the connection to the host was lost." ) );
    } else {
        add_msg( m_bad, _( "Multiplayer: could not reach the host." ) );
    }
    cs.connected = false;
    cs.lost = true;
    state().server_peer = nullptr;
}

void client_on_message( const JsonObject &msg )
{
    const std::string type = msg.get_string( "t", "" );
    if( type == "welcome" ) {
        const bool respawn = cstate().welcomed;
        cstate().welcomed = true;
        // a new character after dying (C1)
        cstate().lost = false;
        cstate().my_id = msg.get_int( "id", 0 );
        cstate().fluid_on = msg.get_bool( "fluid", false ) && fluid::enabled();
        fluid::client_set_mode( cstate().fluid_on, cstate().my_id );
        add_msg( m_good, respawn ? _( "Multiplayer: you are back with a new character." ) :
                 _( "Multiplayer: you joined the game." ) );
    } else if( type == "reject" ) {
        add_msg( m_bad, _( "Multiplayer: the host refused the connection (%s)." ),
                 msg.get_string( "reason", "" ) );
    } else if( type == "submaps" ) {
        apply_submaps( msg.get_array( "list" ) );
    } else if( type == "fx" ) {
        apply_fx( msg.get_array( "list" ) );
    } else if( type == "aim" ) {
        fluid::client_aim_status( msg.get_int( "s", 0 ) / 1000.0, msg.get_int( "r", 0 ) / 1000.0,
                                  msg.get_bool( "f", false ), msg.get_bool( "a", false ) );
    } else if( type == "state" ) {
        apply_state( msg );
    } else if( type == "you_full" ) {
        apply_full_character( msg.get_string( "char", "" ) );
    } else if( type == "alert" ) {
        std::optional<tripoint_abs_ms> source;
        if( msg.has_array( "src" ) ) {
            source = read_pos( msg.get_array( "src" ) );
        }
        alerts::warn( msg.get_string( "text", "" ),
                      static_cast<alerts::level>( std::clamp( msg.get_int( "lvl", 1 ), 0, 2 ) ), source );
    } else if( type == "msg" ) {
        add_msg( m_info, msg.get_string( "text", "" ) );
    }
}

} // namespace impl

void client_send_input( const fluid::input_frame &in )
{
    client_state &cs = cstate();
    if( !cs.welcomed || !cs.fluid_on || impl::state().server_peer == nullptr ) {
        return;
    }
    static fluid::input_frame last;
    static fluid::ms_t last_at = -100000;
    const fluid::ms_t now = fluid::now_ms();
    const bool buttons = in.move_x != last.move_x || in.move_y != last.move_y ||
                         in.primary != last.primary || in.secondary != last.secondary ||
                         in.cancel != last.cancel || in.sprint != last.sprint ||
                         in.primary_presses != last.primary_presses ||
                         in.secondary_presses != last.secondary_presses ||
                         in.cursor_valid != last.cursor_valid;
    const double moved = std::abs( in.cursor_x - last.cursor_x ) + std::abs( in.cursor_y - last.cursor_y );
    const bool send = ( now - last_at >= 100 ) || ( buttons && now - last_at >= 12 ) ||
                      ( in.cursor_valid && moved > 0.12 && now - last_at >= 40 );
    if( !send ) {
        return;
    }
    last = in;
    last_at = now;
    impl::send_json_fast( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "in" );
        jo.member( "mx", in.move_x );
        jo.member( "my", in.move_y );
        jo.member( "p", in.primary );
        jo.member( "s", in.secondary );
        jo.member( "pc", in.primary_presses );
        jo.member( "sc", in.secondary_presses );
        jo.member( "c", in.cancel );
        jo.member( "sp", in.sprint );
        jo.member( "cv", in.cursor_valid );
        jo.member( "cx", in.cursor_x );
        jo.member( "cy", in.cursor_y );
        jo.member( "cz", in.cursor_z );
    } ) );
}

bool client_do_turn()
{
    poll();
    mod_engine::tick();
    fluid::client_frame();
    ui_manager::redraw_invalidated();
    if( handle_input() ) {
        shutdown();
        // The next do_turn() ends the game without saving.
        g->uquit = QUIT_NOSAVED;
    }
    poll();
    return false;
}

} // namespace net
