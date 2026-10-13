// Multiplayer host: owns the world, drives one NPC per connected client and sends
// the world state to the clients. See net_session.h.

#include <algorithm>
#include <optional>
#include <iterator>
#include <cctype>
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

#include "action.h"
#include "activity_actor_definitions.h"
#include "alerts.h"
#include "avatar_action.h"
#include "avatar.h"
#include "calendar.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "character.h"
#include "character_id.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "debug.h"
#include "faction.h"
#include "filesystem.h"
#include "flexbuffer_json.h"
#include "fluid.h"
#include "focus_minigame.h"
#include "game.h"
#include "game_constants.h"
#include "gates.h"
#include "item.h"
#include "item_location.h"
#include "iuse.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_iterator.h"
#include "mapbuffer.h"
#include "memory_fast.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "mutation.h"
#include "net_protocol.h"
#include "net_session.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "player_activity.h"
#include "point.h"
#include "recipe.h"
#include "remote_talk.h"
#include "npctrade.h"
#include "trade_ui.h"
#include "remote_ui.h"
#include "recipe_dictionary.h"
#include "requirements.h"
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
    // A4: who this is (a secret key the client keeps) and the character it made
    std::string key;
    std::string start_character;
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
    // A4: last full character sent to the owner (hash) and when
    size_t sent_character = 0;
    fluid::ms_t character_sent_at = 0;
    // down instead of dead (C1): helped up by a friend, or dies when the time runs out
    bool downed = false;
    bool allow_death = false;
    fluid::ms_t bleed_out_at = 0;
    int revive_progress = 0;
    fluid::ms_t last_revive_step = 0;
    // a conversation with an NPC, run turn by turn (remote_talk.h); the player can not be
    // seen or hurt while it lasts
    std::unique_ptr<remote_conversation> talk;
    // the NPC whose trading window is open on the player's computer, else -1
    int trade_npc = -1;
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

// ---- conversations with NPCs (C4) ----

void resync_player( remote_player &rp, const npc &guy );

void send_talk_turn( remote_player &rp, npc &guy )
{
    if( !rp.talk ) {
        return;
    }
    remote_conversation::turn_data td;
    // what the NPC says may open windows: they are not shown on the host
    bool more = false;
    {
        remote_ui::replay_scope replay( std::vector<remote_ui::entry>(), guy,
                                        guy.getID().get_value() );
        more = rp.talk->turn( td );
    }
    if( !more ) {
        rp.talk.reset();
        impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
            jo.member( "t", "talk" );
            jo.member( "end", true );
        } ) );
        return;
    }
    impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "talk" );
        jo.member( "end", false );
        jo.member( "name", td.npc_name );
        jo.member( "ops" );
        jo.start_array();
        for( const std::vector<std::string> &op : td.history ) {
            jo.write( op );
        }
        jo.end_array();
        jo.member( "resp" );
        jo.start_array();
        for( const remote_conversation::response &r : td.responses ) {
            jo.start_object();
            jo.member( "c", r.color );
            jo.member( "h", r.hotkey_desc );
            jo.member( "x", r.text );
            jo.member( "k", r.key );
            jo.member( "ok", r.enabled );
            jo.member( "q", r.consequence );
            jo.end_object();
        }
        jo.end_array();
    } ) );
}

void end_talk( remote_player &rp )
{
    if( rp.talk ) {
        rp.talk.reset();
        impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
            jo.member( "t", "talk" );
            jo.member( "end", true );
        } ) );
    }
}

// The player talks to the nearest NPC (that is not another player).
void start_talk( remote_player &rp, npc &guy )
{
    if( rp.talk ) {
        return;
    }
    npc *best = nullptr;
    int best_dist = 1000;
    for( npc *other : g->get_npcs_if( [&]( const npc & n ) {
    return &n != &guy && !n.is_dead() && find_client( n.getID() ) == nullptr &&
           n.posz() == guy.posz() && guy.sees( get_map(), n );
    } ) ) {
        const int dist = rl_dist( guy.pos_abs(), other->pos_abs() );
        if( dist <= SEEX * 2 && dist < best_dist ) {
            best = other;
            best_dist = dist;
        }
    }
    if( best == nullptr ) {
        send_text( rp, _( "There is nobody to talk to." ) );
        return;
    }
    rp.talk = std::make_unique<remote_conversation>( guy, *best );
    if( !rp.talk->started() ) {
        rp.talk.reset();
        send_text( rp, string_format( _( "%s doesn't want to talk to you." ), best->get_name() ) );
        return;
    }
    send_talk_turn( rp, guy );
}

// The player answered: the NPC reacts.
void pick_talk( remote_player &rp, npc &guy, int index )
{
    if( !rp.talk ) {
        return;
    }
    // windows that an answer would open are not shown on the host: they are answered
    // "never mind"; what they would have said goes to the player
    std::vector<std::string> texts;
    {
        remote_ui::replay_scope replay( std::vector<remote_ui::entry>(), guy,
                                        guy.getID().get_value() );
        rp.talk->choose( index );
        texts = replay.texts();
    }
    for( const std::string &text : texts ) {
        send_text( rp, text );
    }
    // an answer that starts trading: the window opens on the player's computer, the
    // conversation goes on when it is closed
    if( const std::optional<remote_ui::trade_request> tr = remote_ui::take_trade_request() ) {
        npc *trader = g->find_npc( character_id( tr->npc_id ) );
        if( trader != nullptr ) {
            trader->shop_restock();
            trader->drop_invalid_inventory();
            std::ostringstream os;
            JsonOut tjo( os );
            trader->serialize( tjo );
            const std::string trader_json = os.str();
            rp.trade_npc = tr->npc_id;
            impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
                jo.member( "t", "trade" );
                jo.member( "npc", trader_json );
                jo.member( "cost", tr->cost );
                jo.member( "deal", tr->deal );
            } ) );
            return;
        }
    }
    send_talk_turn( rp, guy );
}

// The player closed the trading window.
void finish_trade( remote_player &rp, npc &guy, const JsonObject &msg )
{
    npc *trader = g->find_npc( character_id( rp.trade_npc ) );
    rp.trade_npc = -1;
    if( trader != nullptr && !trader->is_dead() && msg.get_bool( "ok", false ) &&
        rl_dist( guy.pos_abs(), trader->pos_abs() ) <= SEEX * 2 ) {
        trade_ui::trade_result_t res;
        res.traded = true;
        res.balance = msg.get_int( "balance", 0 );
        res.delta_bank = msg.get_int( "bank", 0 );
        res.value_you = msg.get_int( "vyou", 0 );
        res.value_trader = msg.get_int( "vthem", 0 );
        {
            // what the player carries on the computer is what the body carries
            remote_ui::replay_scope scope( std::vector<remote_ui::entry>(), guy,
                                           msg.get_int( "cid", -1 ) );
            for( const std::pair<item_location, int> &e :
                 remote_ui::locations_from_string( msg.get_string( "you", "" ) ) ) {
                res.items_you.push_back( e );
            }
            for( const std::pair<item_location, int> &e :
                 remote_ui::locations_from_string( msg.get_string( "them", "" ) ) ) {
                res.items_trader.push_back( e );
            }
        }
        if( npc_trading::npc_will_accept_trade( *trader, res.balance ) ) {
            npc_trading::perform_agreed_trade( guy, *trader, res );
            resync_player( rp, guy );
        } else {
            send_text( rp, _( "The trade is no longer acceptable." ) );
        }
    }
    send_talk_turn( rp, guy );
}

// ---- spawning the NPC of a new client ----

// ---- player characters (A4) ----

bool valid_key( const std::string &key )
{
    if( key.size() < 8 || key.size() > 64 ) {
        return false;
    }
    return std::all_of( key.begin(), key.end(), []( char c ) {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0;
    } );
}

cata_path player_file( const std::string &key )
{
    return PATH_INFO::world_base_save_path() / "mp_players" / ( key + ".json" );
}

// The body of a player is kept in mp_players/<key>.json while they are away, not in the world.
void save_body( const remote_player &rp, const npc &guy )
{
    if( !valid_key( rp.key ) ) {
        return;
    }
    assure_dir_exist( PATH_INFO::world_base_save_path() / "mp_players" );
    write_to_file( player_file( rp.key ), [&]( std::ostream & fout ) {
        JsonOut jo( fout );
        guy.serialize( jo );
    }, _( "multiplayer player" ) );
}

std::optional<std::string> saved_body( const std::string &key )
{
    if( !valid_key( key ) || !file_exist( player_file( key ) ) ) {
        return std::nullopt;
    }
    std::string data;
    read_from_file( player_file( key ), [&]( std::istream & fin ) {
        data.assign( std::istreambuf_iterator<char>( fin ), std::istreambuf_iterator<char>() );
    } );
    if( data.empty() ) {
        return std::nullopt;
    }
    return data;
}

// Loads a character (an NPC save, or the avatar a client made) into @p guy.
bool load_body( npc &guy, std::string json )
{
    // fields an NPC needs that an avatar save does not have
    const size_t open = json.find( '{' );
    if( open == std::string::npos ) {
        return false;
    }
    if( json.find( "\"complaints\"" ) == std::string::npos ) {
        json.insert( open + 1, "\"complaints\":{}," );
    }
    try {
        JsonValue jv = json_loader::from_string( std::move( json ) );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        guy.deserialize( jo );
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: loaded body of " << guy.get_name() << " with " <<
                                   guy.get_mutations().size() << " traits";
    } catch( const std::exception &err ) {
        DebugLog( D_WARNING, D_GAME ) << "Multiplayer: could not load a player character: " << err.what();
        return false;
    }
    return true;
}

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
    // A4: the saved body of this player, else the character the client made, else random
    bool loaded = false;
    bool returning = false;
    if( std::optional<std::string> saved = saved_body( rp.key ) ) {
        loaded = returning = load_body( *guy, *saved );
    }
    if( !loaded && !rp.start_character.empty() ) {
        loaded = load_body( *guy, rp.start_character );
    }
    if( !loaded ) {
        guy->normalize();
        guy->randomize();
        if( !rp.name.empty() ) {
            guy->name = rp.name;
        }
    }
    if( returning ) {
        // back where they left, if that place is loaded and free (else next to the host)
        const tripoint_abs_ms left_at = guy->pos_abs();
        if( here.inbounds( left_at ) ) {
            const tripoint_bub_ms p = here.get_bub( left_at );
            if( here.passable( p ) && creatures.creature_at( p ) == nullptr ) {
                spot = p;
            }
        }
    }
    // a fresh id in this world, whatever the save said
    guy->setID( g->assign_npc_id(), true );
    guy->spawn_at_precise( here.get_abs( spot ) );
    overmap_buffer.insert_npc( guy );
    guy->set_fac( faction_your_followers );
    guy->set_attitude( NPCATT_NULL );
    guy->mission = NPC_MISSION_NULL;
    guy->set_value( "mp_remote_player", "1" );
    const character_id id = guy->getID();
    g->load_npcs();
    if( returning ) {
        send_text( rp, _( "Welcome back.  Your character was waiting for you." ) );
    }
    return g->find_npc( id );
}

// Takes the body of a player out of the world (it is saved in mp_players/).
void store_and_remove_body( remote_player &rp )
{
    if( !rp.npc_id.is_valid() ) {
        return;
    }
    if( npc *guy = g->find_npc( rp.npc_id ) ) {
        if( !guy->is_dead() ) {
            save_body( rp, *guy );
        }
    }
    fluid::forget_state( rp.npc_id.get_value() );
    g->remove_npc( rp.npc_id );
    overmap_buffer.remove_npc( rp.npc_id );
    rp.npc_id = character_id();
    // the sidebar (compass) keeps pointers to the people the host saw
    g->mon_info_update();
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

// Sends the player's character and surroundings again, even if the host's copy did not change
// (the client may have changed its own copy).
void resync_player( remote_player &rp, const npc &guy )
{
    for( const tripoint_abs_sm &p : submaps_around( guy.pos_abs(), 1 ) ) {
        rp.sent_submaps.erase( p );
    }
    rp.character_sent_at = 0;
    rp.sent_character = 0;
}

// ---- world state ----

void send_state( remote_player &rp, const npc &guy )
{
    map &here = get_map();
    const avatar &u = get_avatar();
    const std::string json = message( [&]( JsonOut & jo ) {
        jo.member( "t", "state" );
        jo.member( "turn", to_turn<int>( calendar::turn ) );
        // C14: the host's weather (the client shows it instead of its own world's)
        jo.member( "weather", get_weather().weather_id.str() );

        jo.member( "you" );
        jo.start_object();
        jo.member( "pos" );
        write_pos( jo, guy.pos_abs() );
        jo.member( "dead", guy.is_dead() );
        jo.member( "asleep", guy.in_sleep_state() );
        // what changes quickly while moving (the full character only comes every few seconds)
        jo.member( "mm", guy.current_movement_mode().str() );
        jo.member( "stam", guy.get_stamina() );
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
        if( !dedicated() ) {
            jo.start_object();
            jo.member( "id", u.getID().get_value() );
            jo.member( "name", u.get_name() );
            jo.member( "pos" );
            write_pos( jo, u.pos_abs() );
            jo.end_object();
        }
        // the other players and every ordinary NPC in the reality bubble
        for( const npc &np : g->all_npcs() ) {
            if( np.getID() == guy.getID() || np.is_dead() || !here.inbounds( np.pos_abs() ) ) {
                continue;
            }
            jo.start_object();
            jo.member( "id", np.getID().get_value() );
            jo.member( "name", np.get_name() );
            jo.member( "pos" );
            write_pos( jo, np.pos_abs() );
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
    } else if( cmd.action == "open" ) {
        const tripoint_bub_ms where = guy.pos_bub( here ) + cmd.delta;
        if( here.open_door( guy, where, !here.is_outside( guy.pos_bub( here ) ) ) ) {
            guy.mod_moves( -100 );
        } else if( remote_player *rp = find_client( guy.getID() ) ) {
            send_text( *rp, _( "You can't open that." ) );
        }
    } else if( cmd.action == "close" ) {
        // closing costs moves and says why it fails (to the body: see send_text below)
        const int moves_before = guy.get_moves();
        doors::close_door( here, guy, guy.pos_bub( here ) + cmd.delta );
        if( guy.get_moves() == moves_before ) {
            if( remote_player *rp = find_client( guy.getID() ) ) {
                send_text( *rp, _( "You can't close that." ) );
            }
        }
    } else if( cmd.action == "sleep" ) {
        // B6: sleep until morning (the time skips when everybody sleeps)
        if( !guy.in_sleep_state() ) {
            const time_point now = calendar::turn;
            time_point wake = now - time_past_midnight( now ) + 7_hours;
            if( wake <= now + 1_hours ) {
                wake += 1_days;
            }
            guy.fall_asleep( std::min( wake - now, 10_hours ) );
            alerts::warn( string_format( _( "%s went to sleep." ), guy.get_name() ), alerts::level::notice );
        }
    } else if( cmd.action == "stop" ) {
        // handled before the action runs, see host_drive_npc()
    } else if( cmd.action == "wake" ) {
        if( guy.in_sleep_state() ) {
            guy.wake_up();
        }
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

// ---- client menus (A6) ----

// An item the client picked in a menu of its own copy of the character.  The client's copy
// has its own character id; on the host the same item belongs to the player's body.
item_location client_item( const npc &guy, int client_id, std::string loc_json )
{
    const std::string from = "\"character\":" + std::to_string( client_id );
    const std::string to = "\"character\":" + std::to_string( guy.getID().get_value() );
    for( size_t at = loc_json.find( from ); at != std::string::npos;
         at = loc_json.find( from, at + to.size() ) ) {
        loc_json.replace( at, from.size(), to );
    }
    item_location loc;
    try {
        JsonValue jv = json_loader::from_string( std::move( loc_json ) );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        loc.deserialize( jo );
    } catch( const std::exception & ) {
        return item_location();
    }
    if( !loc || loc.get_item() == nullptr ) {
        return item_location();
    }
    // only own things, or things right next to the player
    if( loc.carrier() != nullptr ) {
        if( loc.carrier()->getID() != guy.getID() ) {
            return item_location();
        }
    } else if( rl_dist( loc.pos_abs(), guy.pos_abs() ) > 1 ) {
        return item_location();
    }
    return loc;
}

// A6: an action a client did with menus on its copy of the character (see remote_ui.h).  The
// host does it again on the body with the answers the client gave.
void run_ui_action( remote_player &rp, npc &guy, const JsonObject &msg )
{
    const std::string what = msg.get_string( "a", "" );
    const int client_id = msg.get_int( "cid", -1 );
    if( what == "aim" || what == "capture" ) {
        // advanced inventory, butchering, reloading ...: the client chose on its copy what to
        // do, here the activity it started starts on the body
        static const std::set<std::string> allowed = { "ACT_PICKUP", "ACT_DROP", "ACT_MOVE_ITEMS",
                                                       "ACT_INSERT_ITEM", "ACT_WEAR", "ACT_WIELD", "ACT_STASH",
                                                       "ACT_RELOAD", "ACT_UNLOAD", "ACT_DISASSEMBLE",
                                                       "ACT_BUTCHER", "ACT_BUTCHER_FULL", "ACT_FIELD_DRESS",
                                                       "ACT_SKIN", "ACT_QUARTER", "ACT_DISMEMBER", "ACT_DISSECT",
                                                       "ACT_MEND_ITEM", "ACT_PULP", "ACT_BASH", "ACT_CONSUME"
                                                     };
        for( JsonValue entry : msg.get_array( "acts" ) ) {
            try {
                const std::string act_json = entry.get_string();
                JsonValue jv = json_loader::from_string( remote_ui::remap_character_id( act_json,
                                client_id, guy.getID().get_value() ) );
                JsonObject jo = jv.get_object();
                jo.allow_omitted_members();
                player_activity act;
                act.deserialize( jo );
                if( act.id().is_null() || !allowed.count( act.id().str() ) ) {
                    send_text( rp, _( "That is not available in multiplayer yet." ) );
                    continue;
                }
                guy.assign_activity( act );
            } catch( const std::exception & ) {
                send_text( rp, _( "Things changed while you were doing that: you stop." ) );
            }
        }
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: " << what << " of " << rp.name << ": " <<
                                   ( guy.activity ? guy.activity.id().str() : std::string( "no activity" ) );
        resync_player( rp, guy );
        return;
    }
    std::vector<std::string> texts;
    bool failed = false;
    {
        remote_ui::replay_scope replay( remote_ui::from_json( msg.get_string( "ans", "[]" ) ), guy,
                                        client_id );
        if( what == "apply" ) {
            item_location loc;
            avatar_action::use_item( guy, loc );
        } else if( what == "item_change_side" || what == "item_favorite" ) {
            item_location loc = remote_ui::location_from_string( remote_ui::answer( "i" ).value_or( "" ) );
            if( loc ) {
                if( what == "item_favorite" ) {
                    loc->is_favorite = !loc->is_favorite;
                } else {
                    guy.change_side( loc );
                }
            }
        } else if( what == "smash" ) {
            smash_menu();
        } else if( what == "mutations" ) {
            guy.replay_mutations_menu();
        } else if( what == "bionics" ) {
            guy.replay_bionics_menu();
        } else if( what == "examine" || what == "examine_pickup" ) {
            g->examine( what == "examine_pickup" );
        } else if( what == "pickup" ) {
            g->pickup();
        } else if( what == "pickup_all" ) {
            g->pickup_all();
        } else {
            send_text( rp, _( "That is not available in multiplayer yet." ) );
        }
        texts = replay.texts();
        failed = replay.failed();
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: ui action '" << what << "' of " << rp.name <<
                               ( failed ? " failed (ran out of answers)" : " replayed" );
    for( const std::string &text : texts ) {
        send_text( rp, text );
    }
    if( failed ) {
        send_text( rp, _( "Things changed while you were doing that: you stop." ) );
    }
    // the client did the action on its copy too: show it what really happened
    resync_player( rp, guy );
}

// Crafting for a player (A6).  The checks that would otherwise open a window on the host's
// screen are made here first, and the player hears why it does not work.
void start_craft( remote_player &rp, npc &guy, const JsonObject &msg )
{
    const recipe_id id( msg.get_string( "r", "" ) );
    if( !id.is_valid() || !id.obj() ) {
        send_text( rp, _( "You don't know how to make that." ) );
        return;
    }
    const recipe &rec = id.obj();
    const int batch = std::clamp( msg.get_int( "b", 1 ), 1, 1000 );
    if( guy.activity ) {
        send_text( rp, _( "You are busy." ) );
        return;
    }
    if( !guy.knows_recipe( &rec ) && !guy.get_group_available_recipes().contains( &rec ) ) {
        send_text( rp, _( "You don't know how to make that." ) );
        return;
    }
    if( guy.lighting_craft_speed_multiplier( rec ) <= 0.0f ) {
        send_text( rp, _( "You can't see to craft!" ) );
        return;
    }
    if( guy.morale_crafting_speed_multiplier( rec ) <= 0.0f ) {
        send_text( rp, _( "Your morale is too low to craft such a difficult thing…" ) );
        return;
    }
    if( !guy.can_make( &rec, batch ) ) {
        send_text( rp, string_format( _( "You can't make that right now.  %s" ),
                                      rec.simple_requirements().list_missing() ) );
        return;
    }
    if( msg.get_bool( "long", false ) ) {
        guy.make_all_craft( rec.ident(), batch, std::nullopt );
    } else {
        guy.make_craft( rec.ident(), batch, std::nullopt );
    }
    if( guy.activity ) {
        send_text( rp, string_format( _( "You start working on the %s." ), rec.result_name() ) );
    } else {
        send_text( rp, string_format( _( "You could not start the %s." ), rec.result_name() ) );
    }
    rp.character_sent_at = 0;
    rp.sent_character = 0;
}

void use_item( remote_player &rp, npc &guy, const JsonObject &msg )
{
    const std::string what = msg.get_string( "a", "" );
    const int client_id = msg.get_int( "cid", -1 );
    // the host's message log about this action goes to the player
    const size_t messages_before = Messages::size();
    if( what == "craft" ) {
        start_craft( rp, guy, msg );
        return;
    }
    if( what == "drop" ) {
        drop_locations to_drop;
        for( JsonObject entry : msg.get_array( "locs" ) ) {
            entry.allow_omitted_members();
            item_location loc = client_item( guy, client_id, entry.get_string( "loc", "" ) );
            if( loc && loc.carrier() != nullptr ) {
                to_drop.emplace_back( loc, std::max( 1, entry.get_int( "n", 1 ) ) );
            }
        }
        if( !to_drop.empty() ) {
            const std::string what_dropped = to_drop.size() == 1 ? to_drop.front().first->tname() :
                                             string_format( _( "%d things" ), to_drop.size() );
            static_cast<Character &>( guy ).drop( to_drop, guy.pos_bub() );
            send_text( rp, string_format( _( "You drop the %s." ), what_dropped ) );
        }
    } else {
        item_location loc = client_item( guy, client_id, msg.get_string( "loc", "" ) );
        if( !loc ) {
            send_text( rp, _( "That is not there any more." ) );
            return;
        }
        // the host's log speaks of the body in the third person (if the host sees it at all):
        // the player gets a message of their own
        const std::string name = loc->tname();
        const auto done = [&]( bool ok, const std::string &yes, const std::string &no ) {
            send_text( rp, string_format( ok ? yes : no, name ) );
            // show the result at once
            rp.character_sent_at = 0;
            rp.sent_character = 0;
        };
        if( what == "wield" ) {
            done( guy.wield( loc ), _( "You wield your %s." ), _( "You can't wield your %s." ) );
            return;
        } else if( what == "wear" ) {
            done( guy.wear( loc, false ).has_value(), _( "You put on your %s." ),
                  _( "You can't wear your %s." ) );
            return;
        } else if( what == "takeoff" ) {
            done( guy.takeoff( loc ), _( "You take off your %s." ), _( "You can't take off your %s." ) );
            return;
        } else if( what == "consume" ) {
            done( guy.consume( loc ) != trinary::NONE, _( "You consume the %s." ),
                  _( "You can't consume the %s." ) );
            return;
        } else if( what == "read" ) {
            // B5: reading as a timed action (progress in world time, cancel with any key)
            std::vector<std::string> reasons;
            const Character *reader = guy.get_book_reader( *loc, reasons );
            if( reader == nullptr ) {
                for( const std::string &r : reasons ) {
                    send_text( rp, r );
                }
            } else {
                item_location no_ereader;
                const time_duration t = guy.time_to_read( *loc, *reader );
                guy.assign_activity( read_activity_actor( t, loc, no_ereader, false,
                                     guy.getID().get_value() ) );
            }
        }
    }
    const size_t messages_after = Messages::size();
    if( messages_after > messages_before ) {
        for( const std::pair<std::string, std::string> &m :
             Messages::recent_messages( messages_after - messages_before ) ) {
            send_text( rp, m.second );
        }
    }
    // show the result at once
    rp.character_sent_at = 0;
    rp.sent_character = 0;
}

// Runs the timed action (reading, crafting ...) of a player's body.  Returns true while one
// is going on.  Like the host avatar's, it progresses in world time with short days.
bool run_activity( remote_player &rp, npc &guy )
{
    if( !guy.activity ) {
        return false;
    }
    const int world_step_turns = to_turns<int>( calendar::last_step() );
    if( alerts::nonblocking() ) {
        const double focus = focus_minigame::multiplier( guy );
        const int bonus_turns = static_cast<int>( std::max( world_step_turns, 1 ) * focus ) - 1;
        if( bonus_turns > 0 ) {
            guy.mod_moves( guy.get_speed() * bonus_turns );
        }
    }
    const size_t messages_before = Messages::size();
    for( int guard = 0; guard < 1000 && guy.get_moves() > 0 && guy.activity; ++guard ) {
        guy.activity.do_turn( guy );
    }
    const size_t messages_after = Messages::size();
    if( messages_after > messages_before ) {
        for( const std::pair<std::string, std::string> &m :
             Messages::recent_messages( messages_after - messages_before ) ) {
            send_text( rp, m.second );
        }
    }
    if( !guy.activity && guy.get_moves() > guy.get_speed() ) {
        guy.set_moves( guy.get_speed() );
    }
    return true;
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
        // dead for good: the next character starts fresh
        if( valid_key( rp.key ) && file_exist( player_file( rp.key ) ) ) {
            remove_file( player_file( rp.key ).get_unrelative_path() );
        }
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

void server_on_start()
{
    // Bodies of players that were in the world when it was saved: their real copy is in
    // mp_players/, they come back when their player connects (A4).
    std::vector<character_id> stale;
    for( const shared_ptr_fast<npc> &guy : overmap_buffer.get_overmap_npcs() ) {
        if( guy && guy->get_value( "mp_remote_player" ).str() == "1" ) {
            stale.push_back( guy->getID() );
        }
    }
    for( const character_id &id : stale ) {
        g->remove_npc( id );
        overmap_buffer.remove_npc( id );
    }
    if( !stale.empty() ) {
        g->mon_info_update();
    }
}

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
            // A4: the body is saved and leaves the world with its player
            store_and_remove_body( *iter );
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
        rp->key = msg.get_string( "key", "" );
        if( !valid_key( rp->key ) ) {
            rp->key.clear();
        }
        for( const remote_player &other : clients() ) {
            if( &other != rp && !rp->key.empty() && other.key == rp->key ) {
                send_json( peer, message( [&]( JsonOut & jo ) {
                    jo.member( "t", "reject" );
                    jo.member( "reason", _( "this character is already playing" ) );
                } ) );
                enet_peer_disconnect_later( peer, 0 );
                return;
            }
        }
        rp->start_character = msg.get_string( "char", "" );
        // The NPC is spawned at the end of the turn, see host_end_of_turn().
        rp->greeted = true;
    } else if( type == "chat" && rp->npc_id.is_valid() ) {
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() && !rp->downed ) {
            start_talk( *rp, *guy );
        }
    } else if( type == "trade_done" && rp->npc_id.is_valid() && rp->trade_npc >= 0 ) {
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() && rp->talk ) {
            finish_trade( *rp, *guy, msg );
        }
    } else if( type == "talk_pick" && rp->npc_id.is_valid() ) {
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() && rp->talk ) {
            pick_talk( *rp, *guy, msg.get_int( "i", -1 ) );
        }
    } else if( type == "ui" && rp->npc_id.is_valid() ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: ui message from " << rp->name;
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() && !rp->downed ) {
            run_ui_action( *rp, *guy, msg );
        }
    } else if( type == "use" && rp->npc_id.is_valid() ) {
        // A6: an item action chosen in a menu on the client
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() && !rp->downed ) {
            use_item( *rp, *guy, msg );
        } else {
            DebugLog( D_WARNING, D_GAME ) << "Multiplayer: item action of " << rp->name <<
                                          " ignored (body " << ( guy == nullptr ? "missing" : "dead" ) <<
                                          ", downed " << rp->downed << ")";
        }
    } else if( type == "in" && rp->npc_id.is_valid() ) {
        fluid::input_frame f;
        f.move_x = std::clamp( msg.get_int( "mx", 0 ), -1, 1 );
        f.move_y = std::clamp( msg.get_int( "my", 0 ), -1, 1 );
        f.primary = msg.get_bool( "p", false );
        f.secondary = msg.get_bool( "s", false );
        f.primary_presses = msg.get_int( "pc", 0 );
        f.secondary_presses = msg.get_int( "sc", 0 );
        f.throw_presses = msg.get_int( "tc", 0 );
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
            st.seen_throw_presses = f.throw_presses;
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
    if( rp != nullptr && rp->talk ) {
        // the conversation ends when the NPC is gone or the player is down
        const npc *other = g->find_npc( character_id( rp->talk->other_id() ) );
        if( other == nullptr || other->is_dead() || rp->downed || guy.is_dead() ) {
            end_talk( *rp );
        }
    }
    if( rp != nullptr && update_downed( *rp, guy ) ) {
        return true;
    }
    if( rp != nullptr ) {
        // a one-key cancel from the client stops a timed action at once
        for( auto it = rp->commands.begin(); it != rp->commands.end(); ) {
            if( it->action == "stop" ) {
                if( guy.activity ) {
                    guy.cancel_activity();
                    send_text( *rp, _( "You stop." ) );
                }
                it = rp->commands.erase( it );
            } else if( it->action == "focus" ) {
                // the focus minigame key (B3/B5)
                focus_minigame::press( guy );
                it = rp->commands.erase( it );
            } else {
                ++it;
            }
        }
        focus_minigame::tick( guy );
        run_activity( *rp, guy );
        if( guy.activity ) {
            return true;
        }
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

void dedicated_turn()
{
    if( !dedicated() ) {
        return;
    }
    avatar &u = get_avatar();
    map &here = get_map();
    // the ghost: nobody can see, hurt, smell or hear it, and it needs nothing
    static const std::vector<trait_id> ghost_traits = {
        trait_id( "DEBUG_CLOAK" ), trait_id( "DEBUG_NODMG" ), trait_id( "DEBUG_NOSCENT" ),
        trait_id( "DEBUG_SILENT" ), trait_id( "DEBUG_PREVENT_DEATH" ), trait_id( "DEBUG_LS" ),
        trait_id( "DEBUG_NOTEMP" )
    };
    for( const trait_id &t : ghost_traits ) {
        if( t.is_valid() && !u.has_trait( t ) ) {
            u.set_mutation( t );
        }
    }
    // stay with the players, so the reality bubble is where they are
    std::vector<npc *> bodies = remote_player_npcs();
    if( !bodies.empty() ) {
        int64_t sx = 0;
        int64_t sy = 0;
        for( const npc *guy : bodies ) {
            sx += guy->pos_abs().x();
            sy += guy->pos_abs().y();
        }
        const tripoint_abs_ms centre( static_cast<int>( sx / static_cast<int64_t>( bodies.size() ) ),
                                      static_cast<int>( sy / static_cast<int64_t>( bodies.size() ) ),
                                      bodies.front()->pos_abs().z() );
        if( rl_dist( u.pos_abs(), centre ) > 8 && here.inbounds( centre ) ) {
            for( const tripoint_bub_ms &p : here.points_in_radius( here.get_bub( centre ), 4 ) ) {
                if( here.passable( p ) && get_creature_tracker().creature_at( p ) == nullptr ) {
                    if( p.z() != u.posz() ) {
                        g->vertical_shift( p.z() );
                    }
                    u.setpos( here, p, false );
                    g->update_map( u );
                    break;
                }
            }
        }
    }
    // save now and then (a dedicated server has nobody to press "save")
    static fluid::ms_t last_save = fluid::now_ms();
    if( fluid::now_ms() - last_save > 10 * 60 * 1000 ) {
        last_save = fluid::now_ms();
        g->save();
        add_msg( m_info, _( "Multiplayer: the server saved the world." ) );
    }
}

void host_save_players()
{
    if( !is_host() ) {
        return;
    }
    for( const remote_player &rp : clients() ) {
        if( rp.npc_id.is_valid() ) {
            if( const npc *guy = g->find_npc( rp.npc_id ) ) {
                if( !guy->is_dead() ) {
                    save_body( rp, *guy );
                }
            }
        }
    }
}

bool everyone_asleep()
{
    if( !is_host() ) {
        return false;
    }
    if( !dedicated() && !get_avatar().in_sleep_state() ) {
        return false;
    }
    bool anybody = !dedicated();
    for( const remote_player &rp : clients() ) {
        if( !rp.npc_id.is_valid() ) {
            continue;
        }
        const npc *guy = g->find_npc( rp.npc_id );
        if( guy != nullptr ) {
            anybody = true;
            if( !guy->in_sleep_state() ) {
                return false;
            }
        }
    }
    return anybody;
}

bool is_remote_player( const Character &who )
{
    return is_host() && find_client( who.getID() ) != nullptr;
}

void alert_player( const Character &who, const std::string &text, int level,
                   const std::optional<tripoint_abs_ms> &source )
{
    if( who.is_avatar() ) {
        alerts::warn( text, static_cast<alerts::level>( level ), source );
        return;
    }
    remote_player *rp = is_host() ? find_client( who.getID() ) : nullptr;
    if( rp == nullptr ) {
        return;
    }
    impl::send_json( rp->peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "alert" );
        jo.member( "text", text );
        jo.member( "lvl", level );
        if( source ) {
            jo.member( "src" );
            write_pos( jo, *source );
        }
    } ) );
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
        // A4: the owner gets the whole character (inventory, skills, needs) every few seconds
        const fluid::ms_t now = fluid::now_ms();
        if( now - rp.character_sent_at >= 3000 ) {
            rp.character_sent_at = now;
            std::ostringstream os;
            JsonOut jo( os );
            guy->serialize( jo );
            const std::string json = os.str();
            const size_t hash = std::hash<std::string> {}( json );
            if( hash != rp.sent_character ) {
                rp.sent_character = hash;
                impl::send_json( rp.peer, message( [&]( JsonOut & out ) {
                    out.member( "t", "you_full" );
                    out.member( "char", json );
                } ) );
            }
        }
    }
    poll();
}

bool dialogue_protected( const Creature &who )
{
    if( !is_host() ) {
        return false;
    }
    const Character *ch = who.as_character();
    if( ch == nullptr ) {
        return false;
    }
    const remote_player *rp = find_client( ch->getID() );
    return rp != nullptr && rp->talk != nullptr;
}

} // namespace net
