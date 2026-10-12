// Multiplayer client: shows the world the host sends and sends key presses to the host.
// It does not simulate anything itself. See net_session.h.

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "action.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "character_id.h"
#include "coordinates.h"
#include "faction.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "input_context.h"
#include "json.h"
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
    bool connected = false;
    bool welcomed = false;
    bool lost = false;
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
    detail::send_json( detail::state().server_peer, message( [&]( JsonOut & jo ) {
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
    if( you.get_bool( "dead", false ) && !cstate().lost ) {
        cstate().lost = true;
        add_msg( m_bad, _( "Multiplayer: you died on the host." ) );
    }

    map &now = get_map();
    for( JsonObject other : msg.get_array( "others" ) ) {
        other.allow_omitted_members();
        const tripoint_abs_ms p = read_pos( other.get_array( "pos" ) );
        npc *guy = shadow_for( other.get_int( "id" ), other.get_string( "name", "" ), p );
        if( guy != nullptr && now.inbounds( p ) ) {
            guy->setpos( now, now.get_bub( p ), false );
        }
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
    g->invalidate_main_ui_adaptor();
}

// Reads one key (or times out) and sends the matching command to the host.
// Returns true if the player wants to leave the game.
bool handle_input()
{
    input_context ctxt = get_default_mode_input_context();
    ctxt.set_timeout( 50 );
    const std::string &action = ctxt.handle_input();
    if( action == "TIMEOUT" ) {
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
            send_action( "wait" );
            break;
        case ACTION_PICKUP:
        case ACTION_PICKUP_ALL:
            send_action( "pickup" );
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

} // namespace

namespace detail
{

void client_on_connect()
{
    cstate().connected = true;
    send_json( state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "hello" );
        jo.member( "v", protocol_version );
        jo.member( "name", get_avatar().get_name() );
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
        cstate().welcomed = true;
        add_msg( m_good, _( "Multiplayer: you joined the game." ) );
    } else if( type == "reject" ) {
        add_msg( m_bad, _( "Multiplayer: the host refused the connection (%s)." ),
                 msg.get_string( "reason", "" ) );
    } else if( type == "submaps" ) {
        apply_submaps( msg.get_array( "list" ) );
    } else if( type == "state" ) {
        apply_state( msg );
    } else if( type == "msg" ) {
        add_msg( m_info, msg.get_string( "text", "" ) );
    }
}

} // namespace detail

bool client_do_turn()
{
    poll();
    mod_engine::tick();
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
