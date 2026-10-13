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

#include "alerts.h"
#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "character_id.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "faction.h"
#include "flexbuffer_json.h"
#include "fluid.h"
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

static const efftype_id effect_downed( "downed" );
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
    // fluid mode: the newest input state of this client
    fluid::input_frame input;
    bool has_input = false;
    fluid::ms_t input_at = 0;
    fluid::ms_t aim_sent = 0;
    // hash of the last version of every submap sent to this client
    std::map<tripoint_abs_sm, size_t> sent_submaps;
    // down instead of dead (C1): helped up by a friend, or dies when the time runs out
    bool downed = false;
    bool allow_death = false;
    fluid::ms_t bleed_out_at = 0;
    int revive_progress = 0;
    fluid::ms_t last_revive_step = 0;
};

// How long a downed player can wait for help, and how many steps of help bring them up.
constexpr fluid::ms_t bleed_out_ms = 60000;
constexpr int revive_steps = 3;

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
    } else if( cmd.action == "run" || cmd.action == "crouch" || cmd.action == "prone" ) {
        // movement mode toggles (B4): pressing the same one again goes back to walking
        const move_mode_id wanted( cmd.action );
        if( guy.current_movement_mode() == wanted ) {
            guy.set_movement_mode( move_mode_id( "walk" ) );
        } else if( cmd.action != "run" || guy.can_run() ) {
            guy.set_movement_mode( wanted );
        }
    } else {
        guy.pause();
    }
}

// Keeps a downed player on the ground; lets them die when the time is up.
// Returns true while the player is down (they cannot act).
bool update_downed( remote_player &rp, npc &guy )
{
    if( !rp.downed ) {
        return false;
    }
    guy.add_effect( effect_downed, 2_turns, false, 0, true );
    guy.set_moves( 0 );
    if( fluid::now_ms() >= rp.bleed_out_at ) {
        rp.downed = false;
        rp.allow_death = true;
        send_text( rp, _( "Nobody came to help.  You died." ) );
        alerts::warn( string_format( _( "%s died." ), guy.get_name() ), alerts::level::danger,
                      guy.pos_abs() );
        guy.die( &get_map(), nullptr );
    }
    return true;
}

} // namespace

bool host_prevent_death( npc &guy )
{
    if( !is_host() ) {
        return false;
    }
    remote_player *rp = find_client( guy.getID() );
    if( rp == nullptr || rp->allow_death ) {
        return false;
    }
    // Keep the body alive: every broken vital part gets 1 hit point back.
    for( const bodypart_id &bp : guy.get_all_body_parts() ) {
        if( guy.get_part_hp_cur( bp ) <= 0 ) {
            guy.set_part_hp_cur( bp, 1 );
        }
    }
    const fluid::ms_t now = fluid::now_ms();
    if( !rp->downed ) {
        rp->downed = true;
        rp->bleed_out_at = now + bleed_out_ms;
        rp->revive_progress = 0;
        send_text( *rp, string_format(
                       _( "You are down!  A friend can help you up by walking into you.  You have %d seconds." ),
                       static_cast<int>( bleed_out_ms / 1000 ) ) );
        alerts::warn( string_format( _( "%s is down!  Walk into them to help them up." ),
                                     guy.get_name() ), alerts::level::danger, guy.pos_abs() );
    } else {
        // hit again while down: less time left
        rp->bleed_out_at -= 10000;
    }
    guy.add_effect( effect_downed, 2_turns, false, 0, true );
    return true;
}

bool host_try_revive( Character &helper, const tripoint_abs_ms &p )
{
    if( !is_host() ) {
        return false;
    }
    map &here = get_map();
    if( !here.inbounds( p ) ) {
        return false;
    }
    npc *patient = get_creature_tracker().creature_at<npc>( here.get_bub( p ) );
    if( patient == nullptr ) {
        return false;
    }
    remote_player *rp = find_client( patient->getID() );
    if( rp == nullptr || !rp->downed ) {
        return false;
    }
    const fluid::ms_t now = fluid::now_ms();
    // help has to be continuous: a long pause starts over
    if( now - rp->last_revive_step > 3000 ) {
        rp->revive_progress = 0;
    }
    rp->last_revive_step = now;
    ++rp->revive_progress;
    if( rp->revive_progress < revive_steps ) {
        helper.add_msg_if_player( m_info, _( "You help %s up…" ), patient->get_name() );
        send_text( *rp, string_format( _( "%s is helping you up…" ), helper.get_name() ) );
        return true;
    }
    rp->downed = false;
    rp->revive_progress = 0;
    patient->remove_effect( effect_downed );
    // back on their feet with a fifth of their health on every part
    for( const bodypart_id &bp : patient->get_all_body_parts() ) {
        const int min_hp = std::max( 1, patient->get_part_hp_max( bp ) / 5 );
        if( patient->get_part_hp_cur( bp ) < min_hp ) {
            patient->set_part_hp_cur( bp, min_hp );
        }
    }
    helper.add_msg_if_player( m_good, _( "You help %s back on their feet." ), patient->get_name() );
    send_text( *rp, string_format( _( "%s helped you back on your feet." ), helper.get_name() ) );
    return true;
}

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
                fluid::forget_state( iter->npc_id.get_value() );
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
    } else if( type == "in" && rp->npc_id.is_valid() ) {
        fluid::input_frame f;
        f.move_x = std::clamp( msg.get_int( "mx", 0 ), -1, 1 );
        f.move_y = std::clamp( msg.get_int( "my", 0 ), -1, 1 );
        f.primary = msg.get_bool( "p", false );
        f.secondary = msg.get_bool( "s", false );
        f.primary_presses = msg.get_int( "pc", 0 );
        f.secondary_presses = msg.get_int( "sc", 0 );
        f.cancel = msg.get_bool( "c", false );
        f.sprint = msg.get_bool( "sp", false );
        f.cursor_valid = msg.get_bool( "cv", false );
        f.cursor_x = msg.get_float( "cx", 0.0 );
        f.cursor_y = msg.get_float( "cy", 0.0 );
        f.cursor_z = msg.get_int( "cz", 0 );
        if( !rp->has_input ) {
            // the counters of presses start wherever the client is: that is not a click
            fluid::actor_state &st = fluid::state_of( rp->npc_id.get_value() );
            st.seen_primary_presses = f.primary_presses;
            st.seen_secondary_presses = f.secondary_presses;
            st.prev_primary = f.primary;
            st.prev_secondary = f.secondary;
            st.initialised = true;
        }
        rp->input = f;
        rp->has_input = true;
        rp->input_at = fluid::now_ms();
    } else if( type == "act" && rp->npc_id.is_valid() ) {
        command cmd;
        cmd.action = msg.get_string( "a", "wait" );
        if( fluid::enabled() && cmd.action == "move" ) {
            // fluid mode: movement comes in as "in" frames
            return;
        }
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
    if( rp != nullptr && update_downed( *rp, guy ) ) {
        return true;
    }
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
    if( fluid::enabled() ) {
        // Fluid mode: the player of this NPC is driven between the turns
        // (host_drive_remote_players); the turn itself does nothing.
        // One-off commands of the classic protocol (pick up) still work.
        while( !rp->commands.empty() ) {
            const command cmd = rp->commands.front();
            rp->commands.pop_front();
            if( cmd.action != "move" && cmd.action != "wait" ) {
                execute( guy, cmd );
            }
        }
        // The time of the NPC is kept by the fluid driver; the move points only say whether it
        // is busy with something slow (a pick up, a long action).  Unspent points are not
        // saved up.
        const int speed = guy.get_speed();
        if( guy.get_moves() > speed ) {
            guy.set_moves( speed );
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
        // Forward what the host's message log says about this action (e.g. combat).
        const size_t messages_before = Messages::size();
        execute( guy, cmd );
        const size_t messages_after = Messages::size();
        if( messages_after > messages_before ) {
            for( const std::pair<std::string, std::string> &m :
                 Messages::recent_messages( messages_after - messages_before ) ) {
                send_text( *rp, m.second );
            }
        }
    }
    if( guy.get_moves() > 0 && rp->commands.empty() ) {
        guy.pause();
    }
    return true;
}

void host_drive_remote_players( fluid::ms_t now, std::vector<fluid::event> *events )
{
    if( !is_host() ) {
        return;
    }
    for( remote_player &rp : clients() ) {
        if( !rp.npc_id.is_valid() ) {
            continue;
        }
        npc *guy = g->find_npc( rp.npc_id );
        if( guy == nullptr || guy->is_dead() || rp.downed ) {
            continue;
        }
        fluid::input_frame frame = rp.input;
        if( !rp.has_input || now - rp.input_at > 1500 ) {
            // nothing heard for a while (lag, host menu ...): the player stands still
            frame.move_x = 0;
            frame.move_y = 0;
            frame.primary = false;
            frame.secondary = false;
            frame.cancel = false;
        }
        fluid::actor_state &st = fluid::state_of( guy->getID().get_value() );
        // the host's message log is forwarded to the owner, like in the classic protocol
        const size_t messages_before = Messages::size();
        fluid::drive( *guy, st, frame, now, events );
        const size_t messages_after = Messages::size();
        if( messages_after > messages_before ) {
            for( const std::pair<std::string, std::string> &m :
                 Messages::recent_messages( messages_after - messages_before ) ) {
                send_text( rp, m.second );
            }
        }
        // the owner draws the crosshair, the host knows how steady it is
        if( now - rp.aim_sent >= 80 ) {
            rp.aim_sent = now;
            impl::send_json_fast( rp.peer, message( [&]( JsonOut & jo ) {
                jo.member( "t", "aim" );
                jo.member( "s", static_cast<int>( st.steadiness * 1000.0 ) );
                jo.member( "r", static_cast<int>( st.spread_tiles * 1000.0 ) );
                jo.member( "f", st.can_fire );
                jo.member( "a", st.aiming );
            } ) );
        }
    }
}

void host_send_events( const std::vector<fluid::event> &events )
{
    if( !is_host() || events.empty() ) {
        return;
    }
    std::string payload;
    for( const fluid::event &ev : events ) {
        std::ostringstream os;
        JsonOut jo( os );
        fluid::write_event( jo, ev );
        if( !payload.empty() ) {
            payload += ',';
        }
        payload += os.str();
    }
    const std::string json = R"({"t":"fx","list":[)" + payload + "]}";
    for( remote_player &rp : clients() ) {
        if( rp.greeted && rp.npc_id.is_valid() ) {
            impl::send_json( rp.peer, json );
        }
    }
}

std::vector<npc *> remote_player_npcs()
{
    std::vector<npc *> result;
    if( !is_host() ) {
        return result;
    }
    for( const remote_player &rp : clients() ) {
        if( rp.npc_id.is_valid() ) {
            if( npc *guy = g->find_npc( rp.npc_id ) ) {
                result.push_back( guy );
            }
        }
    }
    return result;
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
        // Died for good (C1): respawn with a new character next to the others.
        if( rp.allow_death && ( guy == nullptr || guy->is_dead() ) ) {
            fluid::forget_state( rp.npc_id.get_value() );
            rp.npc_id = character_id();
            rp.allow_death = false;
            rp.commands.clear();
            guy = nullptr;
            send_text( rp, _( "Respawning…" ) );
        }
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
                jo.member( "fluid", fluid::enabled() );
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
            std::vector<tripoint_abs_sm> nearby = submaps_around( guy->pos_abs(), 1 );
            const std::vector<tripoint_abs_sm> near_host = submaps_around( get_avatar().pos_abs(), 1 );
            nearby.insert( nearby.end(), near_host.begin(), near_host.end() );
            send_submaps( rp, nearby );
        }
        send_state( rp, *guy );
    }
    poll();
}

} // namespace net
