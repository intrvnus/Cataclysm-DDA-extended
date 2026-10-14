// Multiplayer host: owns the world, drives one NPC per connected client and sends
// the world state to the clients. See net_session.h.

#include <algorithm>
#include <limits>
#include <optional>
#include <iterator>
#include <cctype>
#include <cstddef>
#include <cstdlib>
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
#include "construction.h"
#include "calendar.h"
#include "bubbles.h"
#include "clzones.h"
#include "current_map.h"
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
#include "martialarts.h"
#include "mapdata.h"
#include "move_mode.h"
#include "character_martial_arts.h"
#include "mapbuffer.h"
#include "memory_fast.h"
#include "messages.h"
#include "mission.h"
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
    // the overmap tile this player's body was on when it last revealed the surroundings
    tripoint_abs_omt last_omt = tripoint_abs_omt::invalid;
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
    // test helper (CATA_MP_TEST_DOWN): where we are in the down/revive/bleed-out cycle
    int test_stage = 0;
    fluid::ms_t test_at = 0;
    int build_stage = 0;
    int bubble_stage = 0;
    fluid::ms_t bubble_at = 0;
    fluid::ms_t build_at = 0;
    fluid::ms_t bleed_out_at = 0;
    int revive_progress = 0;
    fluid::ms_t last_revive_step = 0;
    // a conversation with an NPC, run turn by turn (remote_talk.h); the player can not be
    // seen or hurt while it lasts
    std::unique_ptr<remote_conversation> talk;
    // the NPC whose trading window is open on the player's computer, else -1
    int trade_npc = -1;
    // the trading window was opened from the menu of an NPC, not from a conversation
    bool trade_only = false;
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

// The player talks to @p best (an NPC that is not another player).
void start_talk( remote_player &rp, npc &guy, npc *best )
{
    if( rp.talk ) {
        return;
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

// An answer that starts trading: the window opens on the player's computer, the conversation
// goes on when it is closed.  True if a window was opened.
bool send_requested_trade( remote_player &rp )
{
    const std::optional<remote_ui::trade_request> tr = remote_ui::take_trade_request();
    if( !tr ) {
        return false;
    }
    npc *trader = g->find_npc( character_id( tr->npc_id ) );
    if( trader == nullptr ) {
        return false;
    }
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
    return true;
}

// The player answered: the NPC reacts.
void pick_talk( remote_player &rp, npc &guy, int index, const std::string &special = std::string() )
{
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: " << rp.name << " answers " << index;
    if( !rp.talk ) {
        return;
    }
    // windows that an answer would open are not shown on the host: they are answered
    // "never mind"; what they would have said goes to the player
    std::vector<std::string> texts;
    {
        remote_ui::replay_scope replay( std::vector<remote_ui::entry>(), guy,
                                        guy.getID().get_value() );
        if( special.empty() ) {
            rp.talk->choose( index );
        } else {
            rp.talk->choose_special( special );
        }
        texts = replay.texts();
    }
    for( const std::string &text : texts ) {
        send_text( rp, text );
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: answer done, " << texts.size() << " texts";
    if( send_requested_trade( rp ) ) {
        return;
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
    if( rp.trade_only ) {
        rp.trade_only = false;
        end_talk( rp );
        return;
    }
    send_talk_turn( rp, guy );
}

// Walking into a friendly NPC: the menu of the normal game, shown on the player's computer.
void send_npc_menu( remote_player &rp, npc &guy, npc &other )
{
    const std::vector<game::npc_menu_entry> entries = g->npc_menu_entries( guy, other );
    impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "npc_menu" );
        jo.member( "id", other.getID().get_value() );
        jo.member( "title", string_format( _( "What to do with %s?" ), other.disp_name() ) );
        jo.member( "entries" );
        jo.start_array();
        for( const game::npc_menu_entry &e : entries ) {
            jo.start_object();
            jo.member( "o", e.option );
            jo.member( "k", std::string( 1, e.key ) );
            jo.member( "x", e.text );
            jo.member( "ok", e.enabled );
            jo.member( "r", e.risky );
            jo.end_object();
        }
        jo.end_array();
    } ) );
}

// An NPC to look at on the player's computer (wounds, status).
void send_npc_view( remote_player &rp, npc &other, const std::string &kind, bool precise )
{
    std::ostringstream os;
    JsonOut tjo( os );
    other.serialize( tjo );
    const std::string npc_json = os.str();
    impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "npc_view" );
        jo.member( "kind", kind );
        jo.member( "precise", precise );
        jo.member( "npc", npc_json );
    } ) );
}

// The player chose an entry of that menu.
void do_npc_option( remote_player &rp, npc &guy, npc &other, int option )
{
    switch( option ) {
        case game::npc_opt_talk:
            start_talk( rp, guy, &other );
            return;
        case game::npc_opt_examine_wounds: {
            float prof_bonus = guy.get_skill_level( skill_id( "firstaid" ) );
            prof_bonus = guy.has_proficiency( proficiency_id( "prof_wound_care" ) ) ? prof_bonus + 1 :
                         prof_bonus;
            prof_bonus = guy.has_proficiency( proficiency_id( "prof_wound_care_expert" ) ) ? prof_bonus + 2 :
                         prof_bonus;
            send_npc_view( rp, other, "wounds", prof_bonus * 4 + guy.per_cur >= 20 );
            return;
        }
        case game::npc_opt_examine_status:
            if( other.op_of_u.trust >= 5 || other.is_friendly( guy ) || other.in_sleep_state() ) {
                send_npc_view( rp, other, "status", false );
            } else {
                send_text( rp, string_format( _( "%s: \"No.\"" ), other.get_name() ) );
            }
            return;
        case game::npc_opt_use_item:
        case game::npc_opt_sort_armor:
            send_text( rp, _( "That is not available in multiplayer yet." ) );
            return;
        case game::npc_opt_trade:
            if( other.is_hallucination() ) {
                return;
            }
            // the same window as the answer "let's trade" of a conversation
            rp.talk = std::make_unique<remote_conversation>( guy, other );
            if( rp.talk->started() ) {
                rp.trade_only = true;
                remote_ui::request_trade( other.getID().get_value(), 0, _( "Trade" ) );
                if( !send_requested_trade( rp ) ) {
                    end_talk( rp );
                }
            } else {
                rp.talk.reset();
            }
            return;
        default:
            break;
    }
    // everything else happens to the body like it would to the player
    std::vector<std::string> texts;
    {
        remote_ui::replay_scope replay( std::vector<remote_ui::entry>(), guy,
                                        guy.getID().get_value() );
        g->npc_menu_choice( guy, other, option );
        texts = replay.texts();
    }
    for( const std::string &text : texts ) {
        send_text( rp, text );
    }
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

// For testing: with the environment variable CATA_MP_TEST_NPC set, an NPC appears next to
// every player who joins (so that conversations and trading can be tried without the debug menu).
// Test helper: a zombie a few steps from the player (tests whether monsters go for clients).
// Test helper: kills the player's body now and then, helps it up, lets it bleed out, and logs
// what happened (C1: downed players, help, respawn).
void test_downed_cycle( remote_player &rp, npc &guy )
{
    static const bool on = std::getenv( "CATA_MP_TEST_DOWN" ) != nullptr;
    if( !on ) {
        return;
    }
    const fluid::ms_t now = fluid::now_ms();
    switch( rp.test_stage ) {
        case 0:
            rp.test_at = now + 20000;
            rp.test_stage = 1;
            break;
        case 1:
        case 3:
            if( now >= rp.test_at ) {
                guy.die( &get_map(), nullptr );
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test: body killed, downed=" << rp.downed <<
                                           " dead=" << guy.is_dead();
                rp.test_at = now + ( rp.test_stage == 1 ? 8000 : 75000 );
                rp.test_stage++;
            }
            break;
        case 2:
            if( now >= rp.test_at ) {
                bool helped = false;
                for( int i = 0; i < 10; i++ ) {
                    helped = host_try_revive( get_avatar(), guy.pos_abs() ) || helped;
                }
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test: helped=" << helped << " downed=" <<
                                           rp.downed << " dead=" << guy.is_dead();
                rp.test_at = now + 8000;
                rp.test_stage = 3;
            }
            break;
        case 4:
            if( now >= rp.test_at ) {
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test: after bleeding out downed=" << rp.downed <<
                                           " allow_death=" << rp.allow_death << " dead=" << guy.is_dead() <<
                                           " body id " << guy.getID().get_value();
                rp.test_stage = 5;
            }
            break;
        default:
            break;
    }
}

// Test helper: a table next to the player (for the grab/drag check) and its position in the log.
void test_furniture( npc &guy, bool place )
{
    static const bool on = std::getenv( "CATA_MP_TEST_FURN" ) != nullptr;
    if( !on ) {
        return;
    }
    map &here = get_map();
    const tripoint_bub_ms at = guy.pos_bub( here );
    if( place ) {
        const tripoint_bub_ms spot = at + tripoint_rel_ms::east;
        if( here.passable( spot ) && !here.veh_at( spot ) &&
            get_creature_tracker().creature_at( spot ) == nullptr &&
            std::string( std::getenv( "CATA_MP_TEST_FURN" ) ) == "veh" ) {
            here.add_vehicle( vproto_id( "shopping_cart" ), spot, 0_degrees, 0, 0 );
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test cart placed east of the player";
        } else if( here.passable( spot ) && !here.veh_at( spot ) &&
                   get_creature_tracker().creature_at( spot ) == nullptr ) {
            here.furn_set( spot, furn_str_id( "f_table" ) );
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test table placed east of the player";
        } else {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: no room for the test table";
        }
        return;
    }
    if( !calendar::once_every( 5_seconds ) ) {
        return;
    }
    for( const tripoint_bub_ms &p : here.points_in_radius( at, 6 ) ) {
        if( const optional_vpart_position vp = here.veh_at( p ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test table at " << vp->vehicle().pos_abs().to_string() <<
                                       " body at " << guy.pos_abs().to_string() << " grab " <<
                                       guy.grab_point.to_string();
            return;
        }
        if( here.furn( p ) == furn_str_id( "f_table" ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test table at " << here.get_abs( p ).to_string() <<
                                       " body at " << guy.pos_abs().to_string() << " grab " <<
                                       guy.grab_point.to_string();
            return;
        }
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test table not found near the body";
}

// Spike test: the player's body moves into an extra reality bubble far away from the host's.  A
// zombie is placed next to the body there and one next to the host's avatar; both must chase
// their player at the same time.
void test_bubble( remote_player &rp, npc &guy )
{
    static const bool on = std::getenv( "CATA_MP_TEST_BUBBLE" ) != nullptr;
    if( !on ) {
        return;
    }
    const fluid::ms_t now = fluid::now_ms();
    if( rp.bubble_stage == 0 ) {
        rp.bubble_at = now + 20000;
        rp.bubble_stage = 1;
        return;
    }
    if( rp.bubble_stage == 1 && now >= rp.bubble_at ) {
        rp.bubble_stage = 2;
        // overlap test: BUBBLE=near puts the second bubble only 3 submaps (36 tiles) away
        const char *mode = std::getenv( "CATA_MP_TEST_BUBBLE" );
        const int dist_sm = std::string( mode ) == "near" ? 3 : 24;
        const bool zombies = std::string( mode ) != "quiet";
        const tripoint_abs_sm home = guy.pos_abs_sm();
        const tripoint_abs_ms far_centre = guy.pos_abs() + tripoint_rel_ms( dist_sm * SEEX, 0, 0 );
        map &bm = bubbles::open_for( far_centre, guy.getID().get_value() );
        swap_map swap( bm );
        const tripoint_bub_ms centre( MAPSIZE_X / 2, MAPSIZE_Y / 2, home.z() );
        for( const tripoint_bub_ms &p : bm.points_in_radius( centre, 20 ) ) {
            if( !bm.passable( p ) || get_creature_tracker().creature_at( p ) != nullptr ||
                bm.veh_at( p ) ) {
                continue;
            }
            guy.setpos( bm, p );
            bm.build_map_cache( home.z(), true );
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test bubble: body moved to " <<
                                       guy.pos_abs().to_string() << " host avatar at " <<
                                       get_avatar().pos_abs().to_string();
            for( const tripoint_bub_ms &q : bm.points_in_radius( p, 8 ) ) {
                if( !zombies ) {
                    break;
                }
                if( rl_dist( p, q ) >= 2 && rl_dist( p, q ) <= 3 && bm.passable( q ) && bm.sees( q, p, 20 ) &&
                    get_creature_tracker().creature_at( q ) == nullptr ) {
                    if( monster *z = g->place_critter_at( mtype_id( "mon_zombie" ), q ) ) {
                        z->unique_name = "TESTBZ";
                        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test bubble: zombie B at " <<
                                                   z->pos_abs().to_string();
                    }
                    break;
                }
            }
            break;
        }
        // and one in the host's own bubble
        if( !zombies ) {
            return;
        }
        map &here = reality_bubble();
        swap_map swap_main( here );
        const tripoint_bub_ms at = get_avatar().pos_bub( here );
        for( const tripoint_bub_ms &q : here.points_in_radius( at, 8 ) ) {
            if( rl_dist( at, q ) >= 2 && rl_dist( at, q ) <= 3 && here.passable( q ) && here.sees( q, at, 20 ) &&
                get_creature_tracker().creature_at( q ) == nullptr ) {
                if( monster *z = g->place_critter_at( mtype_id( "mon_zombie" ), q ) ) {
                    z->unique_name = "TESTAZ";
                    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test bubble: zombie A at " <<
                                               z->pos_abs().to_string();
                }
                break;
            }
        }
        return;
    }
    if( rp.bubble_stage == 2 && calendar::once_every( 5_seconds ) ) {
        std::string line = "Multiplayer: test bubble: ";
        for( monster &z : g->all_monsters() ) {
            if( z.unique_name == "TESTBZ" ) {
                map &zb = bubbles::owner( z.pos_abs() );
                line += "B dist " + std::to_string( rl_dist( z.pos_abs(), guy.pos_abs() ) ) + " hp " +
                        std::to_string( z.get_hp() ) + " sees " + std::to_string( z.sees( zb, guy ) ) +
                        " attitude " + std::to_string( static_cast<int>( z.attitude( &guy ) ) ) +
                        " anger " + std::to_string( z.anger ) + " aggro " + std::to_string( z.aggro_character ) +
                        " dest " + ( z.get_dest() == guy.pos_abs() ? "body" : "other" ) +
                        " moves " + std::to_string( z.get_moves() ) + " light " +
                        std::to_string( zb.ambient_light_at( z.pos_bub( zb ) ) ) + " ";
            } else if( z.unique_name == "TESTAZ" ) {
                line += "A dist " + std::to_string( rl_dist( z.pos_abs(), get_avatar().pos_abs() ) ) +
                        " hp " + std::to_string( z.get_hp() ) + " ";
            }
        }
        DebugLog( D_INFO, D_MAIN ) << line << "body at " << guy.pos_abs().to_string() << " hp " <<
                                   guy.get_hp();
    }
}

// Test helper: the player builds a battery appliance beside them (checks that the host does a
// construction for a player's body: components used up, building activity, the result).
void test_build( remote_player &rp, npc &guy )
{
    static const bool on = std::getenv( "CATA_MP_TEST_BUILD" ) != nullptr;
    if( !on ) {
        return;
    }
    const fluid::ms_t now = fluid::now_ms();
    map &here = get_map();
    const tripoint_bub_ms target = guy.pos_bub( here ) + tripoint_rel_ms::east;
    if( rp.build_stage == 0 ) {
        rp.build_at = now + 25000;
        rp.build_stage = 1;
    } else if( rp.build_stage == 1 && now >= rp.build_at ) {
        guy.i_add( item( itype_id( "storage_battery" ) ) );
        std::vector<remote_ui::entry> answers = { { "d", "-1 -1 0" } };
        {
            remote_ui::replay_scope scope( answers, guy, guy.getID().get_value() );
            place_construction( { construction_group_str_id( "place_battery" ) } );
            std::string said;
            for( const std::string &t : scope.texts() ) {
                said += t + " / ";
            }
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test build: replay failed=" << scope.failed() <<
                                       " texts: " << said;
        }
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test build: activity " <<
                                   ( guy.activity ? guy.activity.id().str() : std::string( "none" ) ) <<
                                   " partial " << ( here.partial_con_at( target ) != nullptr ) <<
                                   " has battery item " << guy.has_amount( itype_id( "storage_battery" ), 1 );
        rp.build_stage = 2;
        rp.build_at = now;
    } else if( rp.build_stage == 2 && now >= rp.build_at + 5000 ) {
        rp.build_at = now;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test build: activity " <<
                                   ( guy.activity ? guy.activity.id().str() : std::string( "none" ) ) <<
                                   " partial " << ( here.partial_con_at( target ) != nullptr ) <<
                                   " vehicle there " << static_cast<bool>( here.veh_at( target ) );
    }
}

void report_test_armor( const npc &near )
{
    static const bool on = std::getenv( "CATA_MP_TEST_ARMOR" ) != nullptr;
    if( !on || !calendar::once_every( 5_seconds ) ) {
        return;
    }
    std::string order;
    for( const item *it : near.worn_item_order() ) {
        order += it->tname() + " | ";
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test worn order: " << order;
}

void report_traffic()
{
    static const bool on = std::getenv( "CATA_MP_TEST_BEAT" ) != nullptr;
    static fluid::ms_t last = 0;
    static uint64_t last_raw = 0;
    static uint64_t last_wire = 0;
    if( !on ) {
        return;
    }
    const fluid::ms_t now = fluid::now_ms();
    if( last == 0 ) {
        last = now;
        return;
    }
    if( now - last < 10000 ) {
        return;
    }
    const uint64_t raw = raw_bytes_sent;
    const uint64_t wire = wire_bytes_sent();
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: traffic in " << ( now - last ) / 1000 << " s: raw " <<
                               ( raw - last_raw ) / 1024 << " KB, wire " << ( wire - last_wire ) / 1024 << " KB";
    last = now;
    last_raw = raw;
    last_wire = wire;
}

void report_test_vehicle( const npc &near )
{
    static const bool on = std::getenv( "CATA_MP_TEST_VEHICLE" ) != nullptr;
    if( !on || !calendar::once_every( 5_seconds ) ) {
        return;
    }
    map &here = get_map();
    for( const npc *other : remote_player_npcs() ) {
        if( other != &near ) {
            const optional_vpart_position ovp = here.veh_at( other->pos_bub( here ) );
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test other player " << other->getID().get_value() <<
                                       " at " << other->pos_abs().to_string() << " in car " << static_cast<bool>( ovp );
        }
    }
    if( const optional_vpart_position vp = here.veh_at( near.pos_bub( here ) ) ) {
        const vehicle &veh = vp->vehicle();
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test car at " << veh.pos_abs().to_string() <<
                                   " velocity " << veh.velocity << " engine " << veh.engine_on <<
                                   " body at " << near.pos_abs().to_string() << " controlling " <<
                                   near.controlling_vehicle;
    } else {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test body is not in a car, at " <<
                                   near.pos_abs().to_string();
    }
}

void report_test_zombie( const npc &near )
{
    static const bool on = std::getenv( "CATA_MP_TEST_ZOMBIE" ) != nullptr;
    if( !on || !calendar::once_every( 5_seconds ) ) {
        return;
    }
    for( monster &z : g->all_monsters() ) {
        if( z.unique_name == "TESTZED" ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test zombie at distance " <<
                                       rl_dist( z.pos_abs(), near.pos_abs() ) << " hp " << z.get_hp() <<
                                       " body hp " << near.get_hp() << " target " <<
                                       ( z.get_dest() == near.pos_abs() ? "client" : "other" ) <<
                                       " attitude " << static_cast<int>( z.attitude( &near ) ) <<
                                       " aggro " << z.aggro_character << " anger " << z.anger <<
                                       " morale " << z.morale << " sees " <<
                                       z.sees( get_map(), near ) << " fac " << near.get_faction()->id.str();
            return;
        }
    }
}

void spawn_test_zombie( const npc &near )
{
    if( std::getenv( "CATA_MP_TEST_ZOMBIE" ) == nullptr ) {
        return;
    }
    map &here = get_map();
    const tripoint_bub_ms at = near.pos_bub( here );
    // first a spot the player can see, else any free spot close by
    for( const bool need_sight : { true, false } ) {
        for( const tripoint_bub_ms &p : here.points_in_radius( at, 5 ) ) {
            if( rl_dist( p, at ) >= 4 && here.passable( p ) &&
                ( !need_sight || here.sees( at, p, 20 ) ) &&
                get_creature_tracker().creature_at( p ) == nullptr ) {
                if( monster *z = g->place_critter_at( mtype_id( "mon_zombie" ), p ) ) {
                    z->unique_name = "TESTZED";
                    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test zombie spawned, distance " <<
                                               rl_dist( p, at ) << ( need_sight ? " in sight" : " out of sight" );
                    return;
                }
            }
        }
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: no place for a test zombie";
}

void spawn_test_npc( const npc &near )
{
    if( std::getenv( "CATA_MP_TEST_NPC" ) == nullptr ) {
        return;
    }
    map &here = get_map();
    // right above the player when that is free (so that a test can walk into it), else near
    const tripoint_bub_ms north = near.pos_bub( here ) + tripoint_rel_ms::north;
    for( const tripoint_bub_ms &p : here.points_in_radius( near.pos_bub( here ), 3 ) ) {
        const bool is_north = p == north;
        if( ( !is_north && ( rl_dist( p, near.pos_bub( here ) ) < 2 ||
                             here.passable( north ) ) ) || !here.passable( p ) ||
            here.veh_at( p ) || !here.sees( near.pos_bub( here ), p, 10 ) ||
            get_creature_tracker().creature_at( p ) != nullptr ) {
            continue;
        }
        shared_ptr_fast<npc> temp = make_shared_fast<npc>();
        temp->normalize();
        temp->randomize();
        temp->spawn_at_precise( here.get_abs( p ) );
        overmap_buffer.insert_npc( temp );
        temp->form_opinion( get_avatar() );
        temp->mission = NPC_MISSION_NULL;
        temp->add_new_mission( mission::reserve_random( ORIGIN_ANY_NPC, temp->pos_abs_omt(),
                               temp->getID() ) );
        faction *solo = g->faction_manager_ptr->add_new_faction( temp->name,
                        faction_id( "solo_" + temp->name + std::to_string( temp->getID().get_value() ) ),
                        faction_id( "no_faction" ) );
        temp->set_fac( solo ? solo->id : faction_id( "no_faction" ) );
        g->load_npcs();
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test NPC " << temp->get_name() << " spawned";
        return;
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: no free spot in sight for a test NPC";
}

// For testing: with CATA_MP_TEST_STAIRS set, a player who joins stands on the nearest stairs.
void place_on_test_stairs( npc &guy )
{
    if( std::getenv( "CATA_MP_TEST_STAIRS" ) == nullptr ) {
        return;
    }
    map &here = get_map();
    std::optional<tripoint_bub_ms> stairs = find_point_closest_first( guy.pos_bub( here ), 0, 40,
    [&]( const tripoint_bub_ms & p ) {
        return here.has_flag( ter_furn_flag::TFLAG_GOES_UP, p ) &&
               get_creature_tracker().creature_at( p ) == nullptr;
    } );
    if( stairs ) {
        guy.setpos( here, *stairs, false );
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test player put on stairs";
    }
}

// Test helper: a car on open ground and the player in its driver's seat.
void place_test_vehicle( npc &guy )
{
    if( std::getenv( "CATA_MP_TEST_VEHICLE" ) == nullptr ) {
        return;
    }
    map &here = get_map();
    // the second player to join gets a passenger seat of the car that exists already
    static std::optional<tripoint_abs_ms> first_car;
    if( first_car ) {
        if( const optional_vpart_position vp = here.veh_at( here.get_bub( *first_car ) ) ) {
            vehicle &veh = vp->vehicle();
            for( const vpart_reference &seat : veh.get_avail_parts( "BOARDABLE" ) ) {
                const tripoint_bub_ms pos = seat.pos_bub( here );
                if( get_creature_tracker().creature_at( pos ) == nullptr ) {
                    guy.setpos( here, pos, false );
                    here.board_vehicle( pos, &guy );
                    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test passenger seated in the car";
                    return;
                }
            }
        }
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: no free seat for a test passenger";
        return;
    }
    const tripoint_bub_ms at = guy.pos_bub( here );
    for( int r = 3; r <= 60; r += 3 ) {
        for( const tripoint_bub_ms &c : here.points_in_radius( tripoint_bub_ms( at.x(), at.y(), 0 ), r ) ) {
            bool free = true;
            for( int dx = 0; dx < 8 && free; dx++ ) {
                for( int dy = 0; dy < 5 && free; dy++ ) {
                    const tripoint_bub_ms q( c.x() + dx, c.y() + dy, 0 );
                    free = here.inbounds( q ) && here.passable( q ) && !here.has_furn( q ) &&
                           here.is_outside( q ) && !here.veh_at( q ) &&
                           get_creature_tracker().creature_at( q ) == nullptr;
                }
            }
            if( !free ) {
                continue;
            }
            vehicle *veh = here.add_vehicle( vproto_id( "car" ), c + point_rel_ms( 2, 2 ), 0_degrees, 100,
                                             0 );
            if( veh == nullptr ) {
                continue;
            }
            for( const vpart_reference &vpr : veh->get_avail_parts( "CONTROLS" ) ) {
                const tripoint_bub_ms seat = vpr.pos_bub( here );
                veh->is_locked = false;
                guy.setpos( here, seat, false );
                here.board_vehicle( seat, &guy );
                first_car = veh->pos_abs();
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test car placed, player at the controls";
                return;
            }
            return;
        }
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: no open ground for a test car";
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
    // new players start next to somebody who is already playing (in that player's bubble)
    npc *buddy = nullptr;
    if( bubbles::enabled() ) {
        const std::vector<npc *> others = remote_player_npcs();
        if( !others.empty() ) {
            buddy = others.front();
        }
    }
    if( spot == u.pos_bub( here ) && buddy == nullptr ) {
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
    // a fresh id in this world, whatever the save said
    const tripoint_abs_ms left_at = guy->pos_abs();
    guy->setID( g->assign_npc_id(), true );
    tripoint_abs_ms start_at = here.get_abs( spot );
    map *own_bubble = nullptr;
    if( returning && bubbles::enabled() ) {
        // back where they left, in a reality bubble of their own, wherever that is
        own_bubble = &bubbles::open_for( left_at, guy->getID().get_value() );
        swap_map swap( *own_bubble );
        tripoint_bub_ms best = own_bubble->get_bub( left_at );
        if( !own_bubble->passable( best ) || get_creature_tracker().creature_at( best ) != nullptr ) {
            for( const tripoint_bub_ms &p : own_bubble->points_in_radius( best, 4 ) ) {
                if( own_bubble->passable( p ) && get_creature_tracker().creature_at( p ) == nullptr ) {
                    best = p;
                    break;
                }
            }
        }
        start_at = own_bubble->get_abs( best );
    } else if( buddy != nullptr && !returning ) {
        map &bm = bubbles::map_for( *buddy );
        swap_map swap( bm );
        const tripoint_bub_ms at = buddy->pos_bub( bm );
        for( const tripoint_bub_ms &p : bm.points_in_radius( at, 2 ) ) {
            if( p != at && bm.passable( p ) && get_creature_tracker().creature_at( p ) == nullptr ) {
                start_at = bm.get_abs( p );
                break;
            }
        }
    } else if( returning ) {
        // back where they left, if that place is loaded and free (else next to the host)
        if( here.inbounds( left_at ) ) {
            const tripoint_bub_ms p = here.get_bub( left_at );
            if( here.passable( p ) && creatures.creature_at( p ) == nullptr ) {
                start_at = here.get_abs( p );
            }
        }
    }
    guy->spawn_at_precise( start_at );
    overmap_buffer.insert_npc( guy );
    guy->set_fac( faction_your_followers );
    guy->set_attitude( NPCATT_NULL );
    guy->mission = NPC_MISSION_NULL;
    guy->set_value( "mp_remote_player", "1" );
    const character_id id = guy->getID();
    if( own_bubble != nullptr ) {
        swap_map swap( *own_bubble );
        g->load_npcs( own_bubble );
    } else {
        g->load_npcs();
        if( bubbles::enabled() ) {
            // every player gets a bubble of his own (it overlaps the host's while they are close)
            map &own = bubbles::open_for( guy->pos_abs(), id.get_value() );
            swap_map swap( own );
            g->load_npcs( &own );
        }
    }
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
    bubbles::release( rp.npc_id.get_value() );
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
        if( !dedicated() && here.inbounds( u.pos_abs() ) ) {
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
        } else if( critter != nullptr && critter->is_npc() &&
                   find_client( critter->as_npc()->getID() ) == nullptr ) {
            // like the player: an enemy is attacked, anybody else gets the menu
            npc &other = *critter->as_npc();
            if( other.is_enemy() ) {
                guy.melee_attack( other, true );
                other.make_angry();
            } else if( remote_player *rp = find_client( guy.getID() ) ) {
                send_npc_menu( *rp, guy, other );
            }
        } else if( critter != nullptr ) {
            // Never attack other players by walking into them.
            guy.pause();
        } else {
            fluid::body_step( guy, dest );
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
    } else if( cmd.action == "cycle_move" ) {
        const move_mode_id next = guy.current_movement_mode()->cycle();
        guy.set_movement_mode( next );
        if( !guy.movement_mode_is( next ) ) {
            guy.set_movement_mode( next->cycle() );
        }
    } else if( cmd.action == "cycle_move_back" ) {
        const move_mode_id prev = guy.current_movement_mode()->cycle_reverse();
        guy.set_movement_mode( prev );
        if( !guy.movement_mode_is( prev ) ) {
            guy.set_movement_mode( prev->cycle_reverse() );
        }
    } else if( cmd.action == "reset_move" ) {
        if( !guy.is_walking() ) {
            guy.set_movement_mode( move_mode_id( "walk" ) );
        }
    } else if( cmd.action == "thief" ) {
        // honest -> steal -> ask, like the thief mode key of the player
        const std::string mode = guy.get_value( "THIEF_MODE" ).str();
        if( mode == "THIEF_ASK" ) {
            guy.set_value( "THIEF_MODE", "THIEF_HONEST" );
        } else if( mode == "THIEF_HONEST" ) {
            guy.set_value( "THIEF_MODE", "THIEF_STEAL" );
        } else {
            guy.set_value( "THIEF_MODE", "THIEF_ASK" );
        }
        if( remote_player *rp = find_client( guy.getID() ) ) {
            send_text( *rp, string_format( _( "Thief mode: %s" ), guy.get_value( "THIEF_MODE" ).str() ) );
        }
    } else if( cmd.action == "fire_mode" ) {
        item_location weapon = guy.get_wielded_item();
        if( weapon && weapon->is_gun() && !weapon->is_gunmod() && weapon->gun_all_modes().size() > 1 ) {
            weapon->gun_cycle_mode();
        }
    } else if( cmd.action == "vertical" ) {
        // stairs and ladders: like monsters do, the closest matching stairs on the other level
        const int dz = cmd.delta.x() > 0 ? 1 : -1;
        const tripoint_bub_ms pos = guy.pos_bub( here );
        const ter_furn_flag here_flag = dz > 0 ? ter_furn_flag::TFLAG_GOES_UP :
                                        ter_furn_flag::TFLAG_GOES_DOWN;
        const ter_furn_flag there_flag = dz > 0 ? ter_furn_flag::TFLAG_GOES_DOWN :
                                         ter_furn_flag::TFLAG_GOES_UP;
        remote_player *rp = find_client( guy.getID() );
        if( !here.has_flag( here_flag, pos ) ) {
            if( rp != nullptr ) {
                send_text( *rp, dz > 0 ? _( "You can't go up here." ) : _( "You can't go down here." ) );
            }
        } else {
            const tripoint_bub_ms above( pos.xy(), pos.z() + dz );
            std::optional<tripoint_bub_ms> found = find_point_closest_first( above, 0, 10,
            [&]( const tripoint_bub_ms & candidate ) {
                return here.has_flag( there_flag, candidate ) &&
                       get_creature_tracker().creature_at( candidate ) == nullptr;
            } );
            if( found ) {
                guy.setpos( here, *found, false );
                guy.mod_moves( -100 );
                if( rp != nullptr ) {
                    send_text( *rp, dz > 0 ? _( "You climb up." ) : _( "You climb down." ) );
                }
            } else if( rp != nullptr ) {
                send_text( *rp, _( "The way is blocked." ) );
            }
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
    if( what == "aim" || what == "capture" || what == "sort_armor" ) {
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
        if( what == "sort_armor" ) {
            for( const remote_ui::entry &e : remote_ui::from_json( msg.get_string( "ans", "[]" ) ) ) {
                if( e.first == "s" ) {
                    std::istringstream is( e.second );
                    std::vector<int> order;
                    int i = 0;
                    while( is >> i ) {
                        order.push_back( i );
                    }
                    guy.reorder_worn( order );
                }
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
        } else if( what == "apply_menu" ) {
            g->mp_item_action_menu();
        } else if( what == "item_change_side" || what == "item_favorite" ) {
            item_location loc = remote_ui::location_from_string( remote_ui::answer( "i" ).value_or( "" ) );
            if( loc ) {
                if( what == "item_favorite" ) {
                    loc->is_favorite = !loc->is_favorite;
                } else {
                    guy.change_side( loc );
                }
            }
        } else if( what == "construct" ) {
            // nothing was chosen when the client left the menu
            const std::string group = remote_ui::next_is( "s" ) ?
                                      remote_ui::answer( "s" ).value_or( "" ) : std::string();
            if( !group.empty() ) {
                place_construction( { construction_group_str_id( group ) } );
            }
        } else if( what == "npc_act" ) {
            // healing, disarming or robbing somebody: the client chose on its copy
            const int id = std::atoi( remote_ui::answer( "n" ).value_or( "-1" ).c_str() );
            const int option = std::atoi( remote_ui::answer( "o" ).value_or( "-1" ).c_str() );
            npc *other = g->find_npc( character_id( id ) );
            if( other != nullptr && !other->is_dead() && rl_dist( guy.pos_abs(), other->pos_abs() ) <= 2 &&
                ( option == game::npc_opt_use_item || option == game::npc_opt_disarm ||
                  option == game::npc_opt_steal ) ) {
                g->npc_menu_choice( guy, *other, option );
            }
        } else if( what == "npc_sort_armor" ) {
            // the layer order of a friend's clothes, sorted on the client
            const int id = std::atoi( remote_ui::answer( "n" ).value_or( "-1" ).c_str() );
            const std::string order_text = remote_ui::answer( "s" ).value_or( "" );
            npc *other = g->find_npc( character_id( id ) );
            if( other != nullptr && !other->is_dead() && other->is_player_ally() &&
                rl_dist( guy.pos_abs(), other->pos_abs() ) <= 1 ) {
                std::istringstream is( order_text );
                std::vector<int> order;
                int i = 0;
                while( is >> i ) {
                    order.push_back( i );
                }
                other->reorder_worn( order );
            }
        } else if( what == "grab" ) {
            std::istringstream is( remote_ui::answer( "d" ).value_or( "-" ) );
            int x = 0;
            int y = 0;
            int z = 0;
            if( is >> x >> y >> z ) {
                fluid::body_grab( guy, tripoint_rel_ms( x, y, 0 ) );
            }
        } else if( what == "control_vehicle" ) {
            g->mp_control_vehicle();
        } else if( what == "drive" ) {
            std::istringstream is( remote_ui::answer( "d" ).value_or( "0 0 0" ) );
            int x = 0;
            int y = 0;
            int z = 0;
            is >> x >> y >> z;
            if( guy.controlling_vehicle ) {
                drive_vehicle( tripoint_rel_ms( x, y, z ) );
            }
        } else if( what == "haul" ) {
            haul_menu();
        } else if( what == "workout" ) {
            guy.assign_activity( workout_activity_actor( guy.pos_bub() ) );
        } else if( what == "haul_toggle" ) {
            guy.toggle_hauling();
        } else if( what == "style" ) {
            guy.martial_arts_data->pick_style( guy );
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
    // what a client does happens in the reality bubble of its body
    std::optional<bubbles::view> in_bubble;
    if( rp->npc_id.is_valid() ) {
        if( const npc *body = g->find_npc( rp->npc_id ) ) {
            in_bubble.emplace( *body );
        }
    }
    const std::string type = msg.get_string( "t", "" );
    if( type == "ui" ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: ui message arrived, body " << rp->npc_id.get_value() <<
                                   " valid " << rp->npc_id.is_valid() << " downed " << rp->downed;
    }
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
    } else if( type == "npc_do" && rp->npc_id.is_valid() ) {
        npc *guy = g->find_npc( rp->npc_id );
        npc *other = g->find_npc( character_id( msg.get_int( "id", -1 ) ) );
        if( guy != nullptr && other != nullptr && !guy->is_dead() && !rp->downed && !rp->talk &&
            find_client( other->getID() ) == nullptr &&
            rl_dist( guy->pos_abs(), other->pos_abs() ) <= 2 ) {
            do_npc_option( *rp, *guy, *other, msg.get_int( "o", -1 ) );
        }
    } else if( type == "chat" && rp->npc_id.is_valid() ) {
        std::string text = msg.get_string( "x", "" );
        // no colour tags from other people
        std::replace( text.begin(), text.end(), '<', '(' );
        std::replace( text.begin(), text.end(), '>', ')' );
        if( text.size() > 200 ) {
            text.resize( 200 );
        }
        if( !text.empty() ) {
            const std::string line = string_format( "%s: %s", rp->name, text );
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: chat " << line;
            add_msg( m_info, "%s", line );
            for( remote_player &other : clients() ) {
                if( other.greeted ) {
                    send_text( other, line );
                }
            }
        }
    } else if( type == "players_req" && rp->npc_id.is_valid() ) {
        const npc *me = g->find_npc( rp->npc_id );
        const std::string text = player_list_text( me != nullptr ? me->pos_abs() : get_avatar().pos_abs() );
        impl::send_json( rp->peer, message( [&]( JsonOut & jo ) {
            jo.member( "t", "missions" );
            jo.member( "text", text );
        } ) );
    } else if( type == "factions_req" && rp->npc_id.is_valid() ) {
        // who the group knows (the group's standing is kept on the host's avatar)
        std::string text = _( "Factions the group knows" ) + std::string( "\n\n" );
        int shown = 0;
        for( const auto &entry : g->faction_manager_ptr->all() ) {
            const faction &fac = entry.second;
            if( !fac.known_by_u ) {
                continue;
            }
            shown++;
            text += string_format( _( "* %s  (likes you: %d, respects you: %d, trusts you: %d)\n" ),
                                   fac.name, fac.likes_u, fac.respects_u, fac.trusts_u );
            const std::string desc = fac.desc.translated();
            if( !desc.empty() ) {
                text += "  " + desc + "\n";
            }
        }
        if( shown == 0 ) {
            text += _( "You have not met any factions yet." ) + std::string( "\n" );
        }
        impl::send_json( rp->peer, message( [&]( JsonOut & jo ) {
            jo.member( "t", "factions" );
            jo.member( "text", text );
        } ) );
    } else if( type == "missions_req" && rp->npc_id.is_valid() ) {
        // the missions are the group's: they are kept on the host's avatar
        std::string text = _( "Missions of the group" ) + std::string( "\n\n" );
        const std::vector<mission *> active = get_avatar().get_active_missions();
        if( active.empty() ) {
            text += _( "No active missions." ) + std::string( "\n" );
        }
        for( const mission *m : active ) {
            text += "* " + m->name() + "\n  " + m->get_description() + "\n";
            if( m->has_deadline() ) {
                text += string_format( _( "  Deadline: %s\n" ),
                                       to_string( m->get_deadline() - calendar::turn ) );
            }
        }
        text += string_format( _( "\nCompleted: %d   Failed: %d" ),
                               static_cast<int>( get_avatar().get_completed_missions().size() ),
                               static_cast<int>( get_avatar().get_failed_missions().size() ) );
        impl::send_json( rp->peer, message( [&]( JsonOut & jo ) {
            jo.member( "t", "missions" );
            jo.member( "text", text );
        } ) );
    } else if( type == "zones_req" && rp->npc_id.is_valid() ) {
        // zones are shared by the whole group: the client edits a copy and sends it back
        std::ostringstream os;
        JsonOut zjo( os );
        zone_manager::get_manager().serialize( zjo );
        const std::string zones_json = os.str();
        impl::send_json( rp->peer, message( [&]( JsonOut & jo ) {
            jo.member( "t", "zones" );
            jo.member( "zones", zones_json );
        } ) );
    } else if( type == "zones_set" && rp->npc_id.is_valid() ) {
        try {
            JsonValue jv = json_loader::from_string( msg.get_string( "zones", "[]" ) );
            zone_manager::get_manager().deserialize( jv );
            zone_manager::get_manager().cache_data();
        } catch( const std::exception &err ) {
            DebugLog( D_WARNING, D_MAIN ) << "Multiplayer: bad zones from " << rp->name << ": " <<
                                          err.what();
        }
    } else if( type == "omap_req" && rp->npc_id.is_valid() ) {
        // The overmap of a client is only a copy: send what the group knows around the body.
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() ) {
            const tripoint_abs_omt c = guy->pos_abs_omt();
            const int r = std::clamp( msg.get_int( "r", 12 ), 3, 60 );
            std::vector<std::string> table;
            std::map<std::string, int> index;
            std::vector<int> cells;
            std::vector<int> levels;
            for( int dy = -r; dy <= r; dy++ ) {
                for( int dx = -r; dx <= r; dx++ ) {
                    const tripoint_abs_omt q = c + point_rel_omt( dx, dy );
                    const om_vision_level lv = overmap_buffer.seen( q );
                    if( lv == om_vision_level::unseen ) {
                        cells.push_back( -1 );
                        levels.push_back( 0 );
                        continue;
                    }
                    const std::string id = overmap_buffer.ter( q ).id().str();
                    auto it = index.find( id );
                    if( it == index.end() ) {
                        it = index.emplace( id, static_cast<int>( table.size() ) ).first;
                        table.push_back( id );
                    }
                    cells.push_back( it->second );
                    levels.push_back( static_cast<int>( lv ) );
                }
            }
            impl::send_json( rp->peer, message( [&]( JsonOut & jo ) {
                jo.member( "t", "omap" );
                jo.member( "x", c.x() - r );
                jo.member( "y", c.y() - r );
                jo.member( "z", c.z() );
                jo.member( "w", 2 * r + 1 );
                jo.member( "tbl", table );
                jo.member( "v", cells );
                jo.member( "l", levels );
            } ) );
        }
    } else if( type == "trade_done" && rp->npc_id.is_valid() && rp->trade_npc >= 0 ) {
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() && rp->talk ) {
            finish_trade( *rp, *guy, msg );
        }
    } else if( type == "talk_pick" && rp->npc_id.is_valid() ) {
        npc *guy = g->find_npc( rp->npc_id );
        if( guy != nullptr && !guy->is_dead() && rp->talk ) {
            pick_talk( *rp, *guy, msg.get_int( "i", -1 ), msg.get_string( "s", "" ) );
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
        bubbles::view in_bubble( *guy );
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
    if( !bodies.empty() && !bubbles::enabled() ) {
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
    static fluid::ms_t last_beat = fluid::now_ms();
    if( std::getenv( "CATA_MP_TEST_BEAT" ) != nullptr && fluid::now_ms() - last_beat > 5000 ) {
        last_beat = fluid::now_ms();
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: dedicated heartbeat, turn " <<
                                   to_turns<int>( calendar::turn - calendar::turn_zero ) << " players " <<
                                   bodies.size();
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

Character &nearest_player( const tripoint_abs_ms &from )
{
    Character *best = &get_player_character();
    if( !is_host() ) {
        return *best;
    }
    int best_dist = dedicated() ? std::numeric_limits<int>::max() : rl_dist( from, best->pos_abs() );
    for( npc *guy : remote_player_npcs() ) {
        const int d = rl_dist( from, guy->pos_abs() );
        if( d < best_dist ) {
            best = guy;
            best_dist = d;
        }
    }
    return *best;
}

std::vector<tripoint_abs_ms> all_player_positions()
{
    std::vector<tripoint_abs_ms> result = { get_player_character().pos_abs() };
    for( const npc *guy : remote_player_npcs() ) {
        result.push_back( guy->pos_abs() );
    }
    return result;
}

std::string player_list_text( const tripoint_abs_ms &from )
{
    std::string text = _( "Players" ) + std::string( "\n\n" );
    if( !dedicated() ) {
        const avatar &host = get_avatar();
        text += string_format( "%s (host)  hp %d%%\n", host.get_name(),
                               100 * host.get_hp() / std::max( 1, host.get_hp_max() ) );
    }
    for( const remote_player &other : clients() ) {
        if( !other.greeted ) {
            continue;
        }
        const npc *body = other.npc_id.is_valid() ? g->find_npc( other.npc_id ) : nullptr;
        if( body != nullptr && !body->is_dead() ) {
            text += string_format( "%s  hp %d%%  %d tiles away\n", other.name,
                                   100 * body->get_hp() / std::max( 1, body->get_hp_max() ),
                                   rl_dist( body->pos_abs(), from ) );
        } else {
            text += other.name + "\n";
        }
    }
    return text;
}

void host_chat( const std::string &text_in )
{
    if( !is_host() ) {
        return;
    }
    std::string text = text_in;
    std::replace( text.begin(), text.end(), '<', '(' );
    std::replace( text.begin(), text.end(), '>', ')' );
    if( text.size() > 200 || text.empty() ) {
        text.resize( std::min<size_t>( text.size(), 200 ) );
    }
    if( text.empty() ) {
        return;
    }
    const std::string line = string_format( "%s: %s", get_avatar().get_name(), text );
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: chat " << line;
    add_msg( m_info, "%s", line );
    for( remote_player &other : clients() ) {
        if( other.greeted ) {
            send_text( other, line );
        }
    }
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
            place_on_test_stairs( *guy );
            place_test_vehicle( *guy );
            test_furniture( *guy, true );
            spawn_test_npc( *guy );
            spawn_test_zombie( *guy );
            add_msg( m_good, _( "Multiplayer: %s joined." ), guy->get_name() );
            impl::send_json( rp.peer, message( [&]( JsonOut & jo ) {
                jo.member( "t", "welcome" );
                jo.member( "v", protocol_version );
                jo.member( "id", guy->getID().get_value() );
                jo.member( "fluid", fluid::enabled() );
            } ) );
        }
        report_traffic();
        if( guy != nullptr ) {
            report_test_zombie( *guy );
            report_test_vehicle( *guy );
            report_test_armor( *guy );
            test_build( rp, *guy );
            test_bubble( rp, *guy );
            test_furniture( *guy, false );
            test_downed_cycle( rp, *guy );
        }
        if( guy != nullptr && guy->pos_abs_omt() != rp.last_omt ) {
            // overmap knowledge is shared by all players: what anybody sees is known to everybody
            rp.last_omt = guy->pos_abs_omt();
            for( int dy = -2; dy <= 2; dy++ ) {
                for( int dx = -2; dx <= 2; dx++ ) {
                    const tripoint_abs_omt q = rp.last_omt + point_rel_omt( dx, dy );
                    if( overmap_buffer.seen( q ) < om_vision_level::details ) {
                        overmap_buffer.set_seen( q, om_vision_level::details );
                    }
                }
            }
        }
        if( guy == nullptr ) {
            // Not in the reality bubble (or gone).  Milestone 1 keeps everybody in one bubble.
            continue;
        }

        // a player in an extra reality bubble is sent that bubble
        map *own_bubble = bubbles::bubble_of( *guy );
        std::optional<swap_map> view;
        if( own_bubble != nullptr ) {
            view.emplace( *own_bubble );
        }
        std::set<int> z_levels{ guy->posz() };
        if( own_bubble == nullptr ) {
            z_levels.insert( get_avatar().posz() );
        }
        if( rp.sent_submaps.empty() || calendar::once_every( 10_turns ) ) {
            send_submaps( rp, bubble_submaps( z_levels ) );
        } else {
            std::vector<tripoint_abs_sm> nearby = submaps_around( guy->pos_abs(), 1 );
            if( own_bubble == nullptr ) {
                const std::vector<tripoint_abs_sm> near_host = submaps_around( get_avatar().pos_abs(), 1 );
                nearby.insert( nearby.end(), near_host.begin(), near_host.end() );
            }
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
