// Multiplayer host: owns the world, drives one NPC per connected client and sends
// the world state to the clients. See net_session.h.

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <enet/enet.h>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "character_id.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "faction.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "game_constants.h"
#include "item.h"
#include "json.h"
#include "map.h"
#include "map_iterator.h"
#include "mapbuffer.h"
#include "memory_fast.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "net_protocol.h"
#include "net_session.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "point.h"
#include "translations.h"
#include "type_id.h"

static const faction_id faction_your_followers( "your_followers" );

namespace net
{
namespace
{

struct command {
    std::string action;
    point_rel_ms delta;
};

struct remote_player {
    ENetPeer *peer = nullptr;
    std::string name;
    bool greeted = false;
    character_id npc_id;
    std::deque<command> commands;
    // hash of the last version of every submap sent to this client
    std::map<tripoint_abs_sm, size_t> sent_submaps;
};

std::vector<remote_player> &clients()
{
    static std::vector<remote_player> list;
    return list;
}

// NPCs that belonged to a client that left. They stay where they are and wait.
std::set<character_id> &abandoned_npcs()
{
    static std::set<character_id> ids;
    return ids;
}

remote_player *find_client( const ENetPeer *peer )
{
    for( remote_player &rp : clients() ) {
        if( rp.peer == peer ) {
            return &rp;
        }
    }
    return nullptr;
}

remote_player *find_client( const character_id &id )
{
    for( remote_player &rp : clients() ) {
        if( rp.npc_id == id ) {
            return &rp;
        }
    }
    return nullptr;
}

void write_pos( JsonOut &jo, const tripoint_abs_ms &p )
{
    jo.start_array();
    jo.write( p.x() );
    jo.write( p.y() );
    jo.write( p.z() );
    jo.end_array();
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

void send_text( remote_player &rp, const std::string &text )
{
    impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "msg" );
        jo.member( "text", text );
    } ) );
}

// ---- spawning the NPC of a new client ----

npc *spawn_remote_npc( remote_player &rp )
{
    map &here = get_map();
    avatar &u = get_avatar();
    creature_tracker &creatures = get_creature_tracker();

    tripoint_bub_ms spot = u.pos_bub( here );
    for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub( here ), 2 ) ) {
        if( p != u.pos_bub( here ) && here.passable( p ) && creatures.creature_at( p ) == nullptr ) {
            spot = p;
            break;
        }
    }
    if( spot == u.pos_bub( here ) ) {
        return nullptr;
    }

    shared_ptr_fast<npc> guy = make_shared_fast<npc>();
    guy->normalize();
    guy->randomize();
    if( !rp.name.empty() ) {
        guy->name = rp.name;
    }
    guy->spawn_at_precise( here.get_abs( spot ) );
    overmap_buffer.insert_npc( guy );
    guy->set_fac( faction_your_followers );
    guy->set_attitude( NPCATT_NULL );
    guy->mission = NPC_MISSION_NULL;
    guy->set_value( "mp_remote_player", "1" );
    const character_id id = guy->getID();
    g->load_npcs();
    return g->find_npc( id );
}

// ---- submaps ----

std::vector<tripoint_abs_sm> bubble_submaps( const std::set<int> &z_levels )
{
    const tripoint_abs_sm origin = get_map().get_abs_sub();
    std::vector<tripoint_abs_sm> result;
    for( int z : z_levels ) {
        for( int x = 0; x < MAPSIZE; ++x ) {
            for( int y = 0; y < MAPSIZE; ++y ) {
                result.emplace_back( origin.x() + x, origin.y() + y, z );
            }
        }
    }
    return result;
}

std::vector<tripoint_abs_sm> submaps_around( const tripoint_abs_ms &p, int radius )
{
    const tripoint_abs_sm center = project_to<coords::sm>( p );
    std::vector<tripoint_abs_sm> result;
    for( int x = -radius; x <= radius; ++x ) {
        for( int y = -radius; y <= radius; ++y ) {
            result.emplace_back( center + tripoint_rel_sm( x, y, 0 ) );
        }
    }
    return result;
}

// Sends the submaps that changed since they were last sent to this client.
void send_submaps( remote_player &rp, const std::vector<tripoint_abs_sm> &list )
{
    std::string payload;
    for( const tripoint_abs_sm &p : list ) {
        std::ostringstream os;
        JsonOut jo( os );
        if( !MAPBUFFER.net_store_submap( p, jo ) ) {
            continue;
        }
        std::string json = os.str();
        const size_t hash = std::hash<std::string> {}( json );
        auto iter = rp.sent_submaps.find( p );
        if( iter != rp.sent_submaps.end() && iter->second == hash ) {
            continue;
        }
        rp.sent_submaps[p] = hash;
        if( !payload.empty() ) {
            payload += ',';
        }
        payload += json;
    }
    if( !payload.empty() ) {
        impl::send_json( rp.peer, R"({"t":"submaps","list":[)" + payload + "]}" );
    }
}

// ---- world state ----

void send_state( remote_player &rp, const npc &guy )
{
    map &here = get_map();
    const avatar &u = get_avatar();
    const std::string json = message( [&]( JsonOut & jo ) {
        jo.member( "t", "state" );
        jo.member( "turn", to_turn<int>( calendar::turn ) );

        jo.member( "you" );
        jo.start_object();
        jo.member( "pos" );
        write_pos( jo, guy.pos_abs() );
        jo.member( "dead", guy.is_dead() );
        jo.member( "hp" );
        jo.start_object();
        for( const bodypart_id &bp : guy.get_all_body_parts() ) {
            jo.member( bp.id().str(), guy.get_part_hp_cur( bp ) );
        }
        jo.end_object();
        jo.end_object();

        // Everybody else who is playing: the host and the other clients.
        jo.member( "others" );
        jo.start_array();
        jo.start_object();
        jo.member( "id", u.getID().get_value() );
        jo.member( "name", u.get_name() );
        jo.member( "pos" );
        write_pos( jo, u.pos_abs() );
        jo.end_object();
        for( const remote_player &other : clients() ) {
            if( &other == &rp || !other.npc_id.is_valid() ) {
                continue;
            }
            const npc *np = g->find_npc( other.npc_id );
            if( np == nullptr ) {
                continue;
            }
            jo.start_object();
            jo.member( "id", np->getID().get_value() );
            jo.member( "name", np->get_name() );
            jo.member( "pos" );
            write_pos( jo, np->pos_abs() );
            jo.end_object();
        }
        jo.end_array();

        jo.member( "monsters" );
        jo.start_array();
        for( const monster &critter : g->all_monsters() ) {
            if( critter.is_dead() || !here.inbounds( critter.pos_abs() ) ) {
                continue;
            }
            jo.start_object();
            jo.member( "id", critter.type->id.str() );
            jo.member( "hp", critter.get_hp() );
            jo.member( "pos" );
            write_pos( jo, critter.pos_abs() );
            jo.end_object();
        }
        jo.end_array();
    } );
    impl::send_json( rp.peer, json );
}

// ---- executing client commands ----

void pick_up_all( npc &guy )
{
    map &here = get_map();
    const tripoint_bub_ms pos = guy.pos_bub( here );
    std::vector<item *> to_take;
    for( item &it : here.i_at( pos ) ) {
        to_take.push_back( &it );
    }
    int taken = 0;
    for( item *it : to_take ) {
        if( !guy.can_pickVolume( *it ) || !guy.can_pickWeight( *it, false ) ) {
            continue;
        }
        guy.i_add( *it );
        here.i_rem( pos, it );
        ++taken;
    }
    remote_player *rp = find_client( guy.getID() );
    if( rp != nullptr ) {
        if( taken > 0 ) {
            send_text( *rp, string_format( _( "You pick up %d item(s)." ), taken ) );
        } else if( !to_take.empty() ) {
            send_text( *rp, _( "You can't carry that." ) );
        } else {
            send_text( *rp, _( "There is nothing to pick up here." ) );
        }
    }
    // same cost as one pick up action for a player
    guy.mod_moves( -std::max( 100, 25 * taken ) );
}

void execute( npc &guy, const command &cmd )
{
    map &here = get_map();
    if( cmd.action == "move" ) {
        const tripoint_bub_ms dest = guy.pos_bub( here ) + cmd.delta;
        Creature *critter = get_creature_tracker().creature_at( dest );
        if( critter != nullptr && critter->is_monster() ) {
            // Bumping into a monster attacks it, like the player does.
            guy.melee_attack( *critter, true );
        } else if( critter != nullptr ) {
            // Never attack other players by walking into them.
            guy.pause();
        } else {
            guy.move_to( dest );
        }
    } else if( cmd.action == "pickup" ) {
        pick_up_all( guy );
    } else {
        guy.pause();
    }
}

} // namespace

// ---- connection events ----

namespace impl
{

void server_on_connect( ENetPeer *peer )
{
    if( clients().size() >= static_cast<size_t>( max_clients ) ) {
        enet_peer_disconnect_later( peer, 0 );
        return;
    }
    remote_player rp;
    rp.peer = peer;
    clients().push_back( std::move( rp ) );
}

void server_on_disconnect( ENetPeer *peer )
{
    std::vector<remote_player> &list = clients();
    for( auto iter = list.begin(); iter != list.end(); ++iter ) {
        if( iter->peer == peer ) {
            if( iter->npc_id.is_valid() ) {
                abandoned_npcs().insert( iter->npc_id );
            }
            add_msg( m_info, _( "Multiplayer: %s left." ),
                     iter->name.empty() ? _( "a player" ) : iter->name );
            list.erase( iter );
            return;
        }
    }
}

void server_on_message( ENetPeer *peer, const JsonObject &msg )
{
    remote_player *rp = find_client( peer );
    if( rp == nullptr ) {
        return;
    }
    const std::string type = msg.get_string( "t", "" );
    if( type == "hello" ) {
        if( msg.get_int( "v", 0 ) != protocol_version ) {
            send_json( peer, message( [&]( JsonOut & jo ) {
                jo.member( "t", "reject" );
                jo.member( "reason", _( "different game version" ) );
            } ) );
            enet_peer_disconnect_later( peer, 0 );
            return;
        }
        rp->name = msg.get_string( "name", "" );
        // The NPC is spawned at the end of the turn, see host_end_of_turn().
        rp->greeted = true;
    } else if( type == "act" && rp->npc_id.is_valid() ) {
        command cmd;
        cmd.action = msg.get_string( "a", "wait" );
        cmd.delta = point_rel_ms( std::clamp( msg.get_int( "dx", 0 ), -1, 1 ),
                                  std::clamp( msg.get_int( "dy", 0 ), -1, 1 ) );
        // Holding a key down must not build up a long queue of old commands.
        if( rp->commands.size() < 2 ) {
            rp->commands.push_back( cmd );
        }
    }
}

} // namespace impl

// ---- hooks called by the game ----

bool host_drive_npc( npc &guy )
{
    if( !is_host() && abandoned_npcs().empty() ) {
        return false;
    }
    remote_player *rp = find_client( guy.getID() );
    if( rp == nullptr ) {
        if( abandoned_npcs().count( guy.getID() ) == 0 ) {
            return false;
        }
        // The player left: just stand there.
        if( guy.get_moves() > 0 ) {
            guy.pause();
        }
        return true;
    }
    // A few tries: actions that did not cost anything (e.g. walking into a wall) must not
    // let a command queue run forever.
    for( int tries = 0; tries < 4 && guy.get_moves() > 0 && !guy.is_dead(); ++tries ) {
        if( rp->commands.empty() ) {
            // No input this turn: time goes on, the player waits.
            guy.pause();
            break;
        }
        const command cmd = rp->commands.front();
        rp->commands.pop_front();
        execute( guy, cmd );
    }
    if( guy.get_moves() > 0 && rp->commands.empty() ) {
        guy.pause();
    }
    return true;
}

void host_end_of_turn()
{
    if( !is_host() ) {
        return;
    }
    poll();
    for( remote_player &rp : clients() ) {
        if( !rp.greeted ) {
            continue;
        }
        npc *guy = rp.npc_id.is_valid() ? g->find_npc( rp.npc_id ) : nullptr;
        if( !rp.npc_id.is_valid() ) {
            guy = spawn_remote_npc( rp );
            if( guy == nullptr ) {
                // no free spot next to the host, try again next turn
                continue;
            }
            rp.npc_id = guy->getID();
            add_msg( m_good, _( "Multiplayer: %s joined." ), guy->get_name() );
            impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
                jo.member( "t", "welcome" );
                jo.member( "v", protocol_version );
                jo.member( "id", guy->getID().get_value() );
            } ) );
        }
        if( guy == nullptr ) {
            // Not in the reality bubble (or gone).  Milestone 1 keeps everybody in one bubble.
            continue;
        }

        const std::set<int> z_levels{ guy->posz(), get_avatar().posz() };
        if( rp.sent_submaps.empty() || calendar::once_every( 10_turns ) ) {
            send_submaps( rp, bubble_submaps( z_levels ) );
        } else {
            std::vector<tripoint_abs_sm> near = submaps_around( guy->pos_abs(), 1 );
            const std::vector<tripoint_abs_sm> near_host = submaps_around( get_avatar().pos_abs(), 1 );
            near.insert( near.end(), near_host.begin(), near_host.end() );
            send_submaps( rp, near );
        }
        send_state( rp, *guy );
    }
    poll();
}

} // namespace net
