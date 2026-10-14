// Multiplayer client: shows the world the host sends and sends key presses to the host.
// It does not simulate anything itself. See net_session.h.

#include <list>
#include <deque>
#include <thread>
#include <chrono>
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
#include "advanced_inv.h"
#include "help.h"
#include "dialogue_win.h"
#include "remote_talk.h"
#include "color.h"
#include "trade_ui.h"
#include "auto_pickup.h"
#include "auto_note.h"
#include "distraction_manager.h"
#include "martialarts.h"
#include "overmap_ui.h"
#include "uistate.h"
#include "ui_manager.h"
#include "panels.h"
#include "safemode_ui.h"
#include "worldfactory.h"
#include "activity_actor_definitions.h"
#include "avatar.h"
#include "string_input_popup.h"
#include "input.h"
#include "clzones.h"
#include "construction.h"
#include "diary.h"
#include "character_martial_arts.h"
#include "bodygraph.h"
#include "avatar_action.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "character_id.h"
#include "coordinates.h"
#include "crafting_gui.h"
#include "debug.h"
#include "faction.h"
#include "filesystem.h"
#include "flexbuffer_json.h"
#include "fluid.h"
#include "game.h"
#include "game_inventory.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "iuse.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "mapbuffer.h"
#include "memory_fast.h"
#include "messages.h"
#include "mp_lives.h"
#include "mod_engine.h"
#include "monster.h"
#include "mp_test_menus.h"
#include "net_protocol.h"
#include "net_session.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "point.h"
#include "recipe.h"
#include "remote_ui.h"
#include "sounds.h"
#include "scenario.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "uilist.h"

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
    const move_mode_id mode( you.get_string( "mm", u.current_movement_mode().str() ) );
    if( mode.is_valid() && mode != u.current_movement_mode() ) {
        u.set_movement_mode( mode );
    }
    u.set_stamina( you.get_int( "stam", u.get_stamina() ) );
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
            critter->friendly = mon.get_int( "fr", 0 );
            if( mon.get_bool( "pet", false ) ) {
                static const efftype_id effect_pet( "pet" );
                critter->add_effect( effect_pet, 1_turns, true );
            }
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
            } else if( !fluid::client_free_movement() ) {
                // (with free movement the shadows follow the body positions instead)
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

// A6: does an action with menus on the own copy of the character, recording the answers, and
// sends them; the host does the same on the real body (see remote_ui.h).  What the action did
// to the local copy is replaced by the host's version soon after.
void send_ui( const std::string &action, const std::function<void()> &run,
              bool capture_activities = false )
{
    std::vector<remote_ui::entry> answers;
    std::vector<std::string> activities;
    {
        remote_ui::record_scope rec( capture_activities );
        run();
        answers = rec.answers();
        activities = rec.activities();
    }
    avatar &u = get_avatar();
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: sending ui action '" << action << "' with " <<
                               answers.size() << " answers and " << activities.size() << " activities";
    // timed actions run on the host
    if( u.activity ) {
        u.activity.set_to_null();
    }
    u.backlog.clear();
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "ui" );
        jo.member( "a", action );
        jo.member( "cid", u.getID().get_value() );
        jo.member( "ans", remote_ui::to_json( answers ) );
        if( !activities.empty() ) {
            jo.member( "acts", activities );
        }
    } ) );
}

// Crafting: the recipe is chosen in the usual window (on the own character, whose
// inventory the host keeps up to date); the host starts the craft.
void send_craft( bool is_long )
{
    int batch = 0;
    const std::pair<Character *, const recipe *> chosen =
        select_crafter_and_crafting_recipe( batch, recipe_id(), &get_avatar() );
    if( chosen.second == nullptr ) {
        return;
    }
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "use" );
        jo.member( "a", "craft" );
        jo.member( "r", chosen.second->ident().str() );
        jo.member( "b", std::max( 1, batch ) );
        jo.member( "long", is_long );
    } ) );
}

// Open / close: the direction is chosen here.
void send_door( const std::string &what )
{
    const std::optional<tripoint_bub_ms> where = choose_adjacent( what == "open" ?
            _( "Open where?" ) : _( "Close where?" ) );
    if( !where ) {
        return;
    }
    const tripoint_rel_ms d = *where - get_avatar().pos_bub();
    send_action( what, d.xy() );
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

// ---- conversations with NPCs (C4): the host runs them, this shows the usual window ----

struct talk_msg {
    bool end = false;
    std::string name;
    std::vector<std::vector<std::string>> ops;
    std::vector<remote_conversation::response> resp;
};

std::deque<talk_msg> &talk_queue()
{
    static std::deque<talk_msg> q;
    return q;
}

struct trade_msg {
    std::string npc_json;
    int cost = 0;
    std::string deal;
};

std::deque<trade_msg> &trade_queue();
void run_trade_ui();

// Shows one conversation until the host ends it.
void run_talk_ui()
{
    dialogue_window win;
    ui_adaptor ui;
    std::string npc_name;
    ui.on_screen_resize( [&]( ui_adaptor & ui ) {
        win.resize( ui );
    } );
    ui.mark_resize();
    ui.on_redraw( [&]( const ui_adaptor & ) {
        win.draw( npc_name );
    } );
    bool first = true;
    for( ;; ) {
        // the next turn comes from the host
        std::chrono::steady_clock::time_point waited = std::chrono::steady_clock::now();
        while( talk_queue().empty() ) {
            poll();
            if( !trade_queue().empty() ) {
                // an answer opened the trading window; the conversation goes on after it
                run_trade_ui();
                waited = std::chrono::steady_clock::now();
            }
            if( talk_queue().empty() ) {
                if( std::chrono::steady_clock::now() - waited > std::chrono::seconds( 20 ) ||
                    !cstate().connected ) {
                    return;
                }
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            }
        }
        const talk_msg m = talk_queue().front();
        talk_queue().pop_front();
        if( m.end ) {
            return;
        }
        npc_name = m.name;
        if( first ) {
            first = false;
            win.add_history_separator();
        }
        for( const std::vector<std::string> &op : m.ops ) {
            if( op.size() >= 3 && op[0] == "add" ) {
                win.add_to_history( op[1], nc_color::from_color_pair_index( std::atoi( op[2].c_str() ) ) );
            } else if( !op.empty() && op[0] == "clr" ) {
                win.clear_history_highlights();
            }
        }
        std::vector<talk_data> lines;
        for( const remote_conversation::response &r : m.resp ) {
            talk_data td;
            td.color = nc_color::from_color_pair_index( r.color );
            td.hotkey_desc = r.hotkey_desc;
            td.text = r.text;
            lines.push_back( td );
        }
        win.set_responses( lines );

        input_context ctxt( "DIALOGUE_CHOOSE_RESPONSE" );
        win.set_up_scrolling( ctxt );
        ctxt.register_action( "HELP_KEYBINDINGS" );
        ctxt.register_action( "CONFIRM" );
        ctxt.register_action( "ANY_INPUT" );
        ctxt.register_action( "QUIT" );
        int chosen = -2;
        std::string special;
        while( chosen == -2 ) {
            ui_manager::redraw();
            std::string action = ctxt.handle_input();
            const input_event evt = ctxt.get_raw_input();
            win.handle_scrolling( action, ctxt );
            int index = -2;
            if( action == "LOOK_AT" || action == "SIZE_UP_STATS" || action == "ASSESS_PERSONALITY" ||
                action == "CHECK_OPINION" || action == "YELL" ) {
                special = action;
                chosen = -3;
                break;
            }
            if( action == "QUIT" ) {
                index = -1;
            } else if( action == "CONFIRM" ) {
                index = win.sel_response;
            } else if( action == "ANY_INPUT" ) {
                for( size_t i = 0; i < m.resp.size(); ++i ) {
                    if( m.resp[i].key != 0 && m.resp[i].key == static_cast<int>( evt.get_first_input() ) ) {
                        index = static_cast<int>( i );
                        break;
                    }
                }
            }
            if( index == -2 ) {
                continue;
            }
            if( index >= static_cast<int>( m.resp.size() ) ) {
                continue;
            }
            if( index >= 0 && !m.resp[index].enabled ) {
                continue;
            }
            if( index >= 0 && m.resp[index].consequence == 1 &&
                !query_yn( _( "You may be attacked!  Proceed?" ) ) ) {
                continue;
            }
            if( index >= 0 && m.resp[index].consequence == 2 &&
                !query_yn( _( "You'll be helpless!  Proceed?" ) ) ) {
                continue;
            }
            chosen = index;
        }
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: answering " << chosen;
        impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
            jo.member( "t", "talk_pick" );
            jo.member( "i", chosen );
            if( !special.empty() ) {
                jo.member( "s", special );
            }
        } ) );
    }
}

// The trading window of a conversation: the host sent the trader, the deal goes back.
std::deque<trade_msg> &trade_queue()
{
    static std::deque<trade_msg> q;
    return q;
}

void run_trade_ui()
{
    const trade_msg m = trade_queue().front();
    trade_queue().pop_front();
    bool ok = false;
    trade_ui::trade_result_t res;
    try {
        std::unique_ptr<npc> trader = std::make_unique<npc>();
        JsonValue jv = json_loader::from_string( std::string( m.npc_json ) );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        trader->deserialize( jo );
        trade_ui window( get_avatar(), *trader, m.cost, m.deal );
        res = window.perform_trade();
        ok = res.traded;
        // the items the player offers are located before the window goes away
        const std::list<std::pair<item_location, int>> you( res.items_you.begin(),
                res.items_you.end() );
        const std::list<std::pair<item_location, int>> them( res.items_trader.begin(),
                res.items_trader.end() );
        impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo2 ) {
            jo2.member( "t", "trade_done" );
            jo2.member( "ok", ok );
            jo2.member( "cid", get_avatar().getID().get_value() );
            jo2.member( "balance", res.balance );
            jo2.member( "bank", res.delta_bank );
            jo2.member( "vyou", res.value_you );
            jo2.member( "vthem", res.value_trader );
            jo2.member( "you", remote_ui::locations_to_string( you ) );
            jo2.member( "them", remote_ui::locations_to_string( them ) );
        } ) );
        return;
    } catch( const std::exception &err ) {
        DebugLog( D_WARNING, D_GAME ) << "Multiplayer: trading failed: " << err.what();
    }
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "trade_done" );
        jo.member( "ok", false );
    } ) );
}

// ---- walking into an NPC: the menu of the normal game ----

struct npc_menu_msg {
    struct entry {
        int option = 0;
        char key = ' ';
        std::string text;
        bool enabled = true;
        bool risky = false;
    };
    int id = -1;
    std::string title;
    std::vector<entry> entries;
};

struct npc_view_msg {
    std::string kind;
    bool precise = false;
    std::string npc_json;
};

std::deque<npc_menu_msg> &npc_menu_queue()
{
    static std::deque<npc_menu_msg> q;
    return q;
}

bool &omap_arrived()
{
    static bool b = false;
    return b;
}

std::string &missions_text()
{
    static std::string t;
    return t;
}

std::string &zones_json()
{
    static std::string t;
    return t;
}

// Zones belong to the group: the host's are fetched, edited here and sent back.
void edit_zones( const std::function<void()> &menu )
{
    impl::send_json( impl::state().server_peer, message( []( JsonOut & jo ) {
        jo.member( "t", "zones_req" );
    } ) );
    zones_json().clear();
    for( int wait = 0; wait < 100 && zones_json().empty(); wait++ ) {
        poll();
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }
    if( zones_json().empty() ) {
        add_msg( m_info, _( "The host did not answer." ) );
        return;
    }
    try {
        JsonValue jv = json_loader::from_string( zones_json() );
        zone_manager::get_manager().deserialize( jv );
        zone_manager::get_manager().cache_data();
    } catch( const std::exception & ) {
        add_msg( m_bad, _( "Could not read the zones of the group." ) );
        return;
    }
    menu();
    std::ostringstream os;
    JsonOut zjo( os );
    zone_manager::get_manager().serialize( zjo );
    const std::string mine = os.str();
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "zones_set" );
        jo.member( "zones", mine );
    } ) );
}

std::deque<npc_view_msg> &npc_view_queue()
{
    static std::deque<npc_view_msg> q;
    return q;
}

void run_npc_menu()
{
    const npc_menu_msg m = npc_menu_queue().front();
    npc_menu_queue().pop_front();
    uilist menu;
    menu.text = m.title;
    for( const npc_menu_msg::entry &e : m.entries ) {
        menu.addentry( e.option, e.enabled, e.key, e.text );
    }
    menu.query();
    if( menu.ret < 0 ) {
        return;
    }
    for( const npc_menu_msg::entry &e : m.entries ) {
        if( e.option == menu.ret && e.risky && !query_yn( _( "You may be attacked!  Proceed?" ) ) ) {
            return;
        }
    }
    if( menu.ret == game::npc_opt_sort_armor ) {
        // sorted here on the copy of that NPC; the new order is sent to the host
        if( npc *other = g->find_npc( character_id( m.id ) ) ) {
            send_ui( "npc_sort_armor", [other]() {
                const std::vector<const item *> before = other->worn_item_order();
                other->worn.sort_armor( *other );
                std::string order;
                for( const item *it : other->worn_item_order() ) {
                    const auto found = std::find( before.begin(), before.end(), it );
                    if( found != before.end() ) {
                        order += std::to_string( found - before.begin() ) + " ";
                    }
                }
                remote_ui::record( "n", std::to_string( other->getID().get_value() ) );
                remote_ui::record( "s", order );
            } );
            return;
        }
    }
    if( menu.ret == game::npc_opt_use_item || menu.ret == game::npc_opt_disarm ||
        menu.ret == game::npc_opt_steal ) {
        // what is asked on the way (the item, "really?") is answered here on the copy of that NPC,
        // the host does the same with these answers
        if( npc *other = g->find_npc( character_id( m.id ) ) ) {
            const int option = menu.ret;
            send_ui( "npc_act", [other, option]() {
                remote_ui::record( "n", std::to_string( other->getID().get_value() ) );
                remote_ui::record( "o", std::to_string( option ) );
                g->npc_menu_choice( get_avatar(), *other, option );
            } );
            return;
        }
    }
    impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "npc_do" );
        jo.member( "id", m.id );
        jo.member( "o", menu.ret );
    } ) );
}

void run_npc_view()
{
    const npc_view_msg m = npc_view_queue().front();
    npc_view_queue().pop_front();
    try {
        std::unique_ptr<npc> other = std::make_unique<npc>();
        JsonValue jv = json_loader::from_string( std::string( m.npc_json ) );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        other->deserialize( jo );
        if( m.kind == "wounds" ) {
            other->body_window( _( "Limbs of: " ) + other->disp_name(), true, m.precise, 0, 0, 0, 0.0f,
                                0.0f, 0.0f, 0.0f, 0.0f );
        } else {
            display_bodygraph( *other );
        }
    } catch( const std::exception &err ) {
        DebugLog( D_WARNING, D_GAME ) << "Multiplayer: could not show an NPC: " << err.what();
    }
}

// Reads one key (or times out) and sends the matching command to the host.
// Returns true if the player wants to leave the game.
bool handle_input()
{
    if( !npc_view_queue().empty() ) {
        run_npc_view();
        return false;
    }
    if( !npc_menu_queue().empty() ) {
        run_npc_menu();
        return false;
    }
    if( !trade_queue().empty() ) {
        run_trade_ui();
        return false;
    }
    if( !talk_queue().empty() ) {
        run_talk_ui();
        return false;
    }
    // the WASD layout option (the host does this in its own input loop)
    fluid::sync_key_layout();
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
    // chat (F2) and the list of players (F4)
    {
        const int raw = ctxt.get_raw_input().get_first_input();
        if( raw == KEY_F( 2 ) ) {
            string_input_popup popup;
            popup.title( _( "Say:" ) ).width( 60 ).max_length( 120 ).identifier( "mp_chat" );
            const std::string text = popup.query_string();
            if( !popup.canceled() && !text.empty() ) {
                impl::send_json( impl::state().server_peer, message( [&]( JsonOut & jo ) {
                    jo.member( "t", "chat" );
                    jo.member( "x", text );
                } ) );
            }
            return false;
        }
        if( raw == KEY_F( 4 ) ) {
            impl::send_json( impl::state().server_peer, message( []( JsonOut & jo ) {
                jo.member( "t", "players_req" );
            } ) );
            missions_text().clear();
            for( int wait = 0; wait < 100 && missions_text().empty(); wait++ ) {
                poll();
                std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            }
            popup( missions_text().empty() ? _( "The host did not answer." ) : missions_text() );
            return false;
        }
    }
    action_id act = look_up_action( action );
    if( act == ACTION_NULL ) {
        // an unbound key (e.g. Shift + a movement key while sprinting): nothing to say
        return false;
    }
    // the action menu picks another action: that one is then done here
    for( ;; ) {
    switch( act ) {
        case ACTION_ACTIONMENU: {
            const action_id chosen = handle_action_menu( get_map() );
            if( chosen != ACTION_NULL && chosen != ACTION_ACTIONMENU ) {
                act = chosen;
                continue;
            }
            break;
        }
        // stairs and ladders
        case ACTION_MOVE_UP:
            send_action( "vertical", point_rel_ms( 1, 0 ) );
            break;
        case ACTION_MOVE_DOWN:
            send_action( "vertical", point_rel_ms( -1, 0 ) );
            break;
        // movement modes
        case ACTION_CYCLE_MOVE:
            send_action( "cycle_move" );
            break;
        case ACTION_CYCLE_MOVE_REVERSE:
            send_action( "cycle_move_back" );
            break;
        case ACTION_RESET_MOVE:
            send_action( "reset_move" );
            break;
        case ACTION_TOGGLE_THIEF_MODE:
            send_action( "thief" );
            break;
        case ACTION_SELECT_FIRE_MODE:
            send_action( "fire_mode" );
            break;
        case ACTION_PICK_STYLE:
            send_ui( "style", [] {
                get_avatar().martial_arts_data->pick_style( get_avatar() );
            } );
            break;
        case ACTION_DIR_DROP:
            send_ui( "capture", [] {
                if( const std::optional<tripoint_bub_ms> pnt = choose_adjacent( _( "Drop where?" ) ) ) {
                    g->mp_drop_in_direction( *pnt );
                }
            }, true );
            break;
        case ACTION_OPEN_CONSUME:
            send_ui( "capture", [] {
                if( !avatar_action::eat_here( get_avatar() ) ) {
                    g->mp_open_consume_menu();
                }
            }, true );
            break;
        case ACTION_WAIT:
            add_msg( m_info, _( "Waiting is not possible in multiplayer: the world does not stop for you." ) );
            break;
        // views and settings that stay on this computer
        case ACTION_ZOOM_IN:
            g->zoom_in();
            g->mark_main_ui_adaptor_resize();
            break;
        case ACTION_ZOOM_OUT:
            g->zoom_out();
            g->mark_main_ui_adaptor_resize();
            break;
        case ACTION_TOGGLE_FULLSCREEN:
            g->toggle_fullscreen();
            break;
        case ACTION_TOGGLE_PIXEL_MINIMAP:
            g->toggle_pixel_minimap();
            break;
        case ACTION_TOGGLE_PANEL_ADM:
            panel_manager::get_manager().show_adm();
            break;
        case ACTION_RELOAD_TILESET:
            g->reload_tileset();
            break;
        case ACTION_TOGGLE_MAP_MEMORY:
            get_avatar().toggle_map_memory();
            break;
        case ACTION_OPTIONS:
            get_options().show( true );
            break;
        case ACTION_AUTOPICKUP:
            get_auto_pickup().show();
            break;
        case ACTION_AUTONOTES:
            get_auto_notes_settings().show_gui();
            break;
        case ACTION_SAFEMODE:
            get_safemode().show();
            break;
        case ACTION_DISTRACTION_MANAGER:
            get_distraction_manager().show();
            break;
        case ACTION_COLOR:
            all_colors.show_gui();
            break;
        case ACTION_WORLD_MODS:
            world_generator->show_active_world_mods( world_generator->active_world->active_mod_order );
            break;
        case ACTION_LIST_ITEMS:
            g->mp_list_items();
            break;
        case ACTION_SKY:
            if( get_map().is_outside( get_avatar().pos_bub() ) ) {
                ui::omap::display_visible_weather();
            } else {
                add_msg( m_info, _( "You can't see the sky from here." ) );
            }
            break;
        case ACTION_MEDICAL:
            get_avatar().disp_medical();
            break;
        case ACTION_BODYSTATUS:
            display_bodygraph( get_avatar() );
            break;
        case ACTION_COMPARE:
            game_menus::inv::compare( std::nullopt );
            break;
        case ACTION_MOVE_FORTH:
        case ACTION_MOVE_FORTH_RIGHT:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_BACK_RIGHT:
        case ACTION_MOVE_BACK:
        case ACTION_MOVE_BACK_LEFT:
        case ACTION_MOVE_LEFT:
        case ACTION_MOVE_FORTH_LEFT:
            if( get_avatar().controlling_vehicle ) {
                // at the wheel: x steers, y speeds up or brakes
                const point_rel_ms d = get_delta_from_movement_action( act, iso_rotate::no );
                send_ui( "drive", [d] {
                    remote_ui::record( "d", string_format( "%d %d 0", d.x(), d.y() ) );
                } );
                break;
            }
            send_action( "move", get_delta_from_movement_action( act, iso_rotate::yes ) );
            break;
        case ACTION_CONTROL_VEHICLE:
            send_ui( "control_vehicle", [] {
                g->mp_control_vehicle();
            } );
            break;
        case ACTION_PAUSE:
            // the world never waits in multiplayer: the key only stops a timed action (B7)
            send_action( "stop" );
            break;
        case ACTION_AUTOATTACK:
            // fluid melee: the host swings at the weakest enemy next to the body
            send_action( "autoattack" );
            break;
        case ACTION_SUICIDE:
        case ACTION_QUICKLOAD:
        case ACTION_QUICKSAVE:
        case ACTION_DEBUG:
        case ACTION_TOGGLE_DEBUG_MODE:
            add_msg( m_info, _( "Not available in multiplayer." ) );
            break;
        case ACTION_TOGGLE_SAFEMODE:
            add_msg( m_info, _( "Danger warnings are always on in multiplayer." ) );
            break;
        case ACTION_PICKUP:
            // choose the tile and what to take on the own character, the host does the same
            send_ui( "pickup", [] {
                g->pickup();
            } );
            break;
        case ACTION_PICKUP_ALL:
            send_ui( "pickup_all", [] {
                g->pickup_all();
            } );
            break;
        case ACTION_EXAMINE:
        case ACTION_EXAMINE_AND_PICKUP:
            send_ui( act == ACTION_EXAMINE ? "examine" : "examine_pickup", [act] {
                g->examine( act == ACTION_EXAMINE_AND_PICKUP );
            } );
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
        case ACTION_USE:
            send_ui( "apply", [] {
                avatar_action::use_item( get_avatar() );
            } );
            break;
        case ACTION_USE_WIELDED:
            send_ui( "apply", [] {
                item_location loc = get_avatar().get_wielded_item();
                // the host picks the item from the answers, like from the menu
                remote_ui::record( "i", remote_ui::location_to_string( loc ) );
                avatar_action::use_item( get_avatar(), loc );
            } );
            break;
        case ACTION_CONSTRUCT:
            if( get_avatar().in_vehicle ) {
                add_msg( m_info, _( "You can't construct while in a vehicle." ) );
            } else {
                send_ui( "construct", [] {
                    construction_menu( false );
                } );
            }
            break;
        case ACTION_HAUL:
            send_ui( "haul", [] {
                haul_menu();
            } );
            break;
        case ACTION_SORT_ARMOR:
            send_ui( "sort_armor", [] {
                avatar &you = get_avatar();
                const std::vector<const item *> before = you.worn_item_order();
                you.worn.sort_armor( you );
                std::string order;
                for( const item *it : you.worn_item_order() ) {
                    const auto found = std::find( before.begin(), before.end(), it );
                    if( found != before.end() ) {
                        order += std::to_string( found - before.begin() ) + " ";
                    }
                }
                remote_ui::record( "s", order );
            }, true );
            break;
        case ACTION_FACTIONS: {
            impl::send_json( impl::state().server_peer, message( []( JsonOut & jo ) {
                jo.member( "t", "factions_req" );
            } ) );
            missions_text().clear();
            for( int wait = 0; wait < 100 && missions_text().empty(); wait++ ) {
                poll();
                std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            }
            popup( missions_text().empty() ? _( "The host did not answer." ) : missions_text() );
            break;
        }
        case ACTION_MISSIONS: {
            impl::send_json( impl::state().server_peer, message( []( JsonOut & jo ) {
                jo.member( "t", "missions_req" );
            } ) );
            missions_text().clear();
            for( int wait = 0; wait < 100 && missions_text().empty(); wait++ ) {
                poll();
                std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            }
            popup( missions_text().empty() ? _( "The host did not answer." ) : missions_text() );
            break;
        }
        case ACTION_ZONES:
            edit_zones( [] {
                g->zones_manager();
            } );
            break;
        case ACTION_LOOT:
            edit_zones( [] {
                loot_menu();
            } );
            break;
        case ACTION_WORKOUT:
            // the intensity and the minutes are asked here, on the copy; the host replays them
            send_ui( "workout", [] {
                avatar &you = get_avatar();
                you.assign_activity( workout_activity_actor( you.pos_bub() ) );
            } );
            break;
        case ACTION_PEEK:
            g->peek();
            break;
        case ACTION_MAP:
            // ask for everything the group knows nearby; it arrives while the map is open
            impl::send_json( impl::state().server_peer, message( []( JsonOut & jo ) {
                jo.member( "t", "omap_req" );
                jo.member( "r", 60 );
            } ) );
            omap_arrived() = false;
            for( int wait = 0; wait < 50 && !omap_arrived(); wait++ ) {
                poll();
                std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            }
            if( !get_map().is_outside( get_avatar().pos_bub() ) ) {
                uistate.overmap_visible_weather = false;
            }
            ui::omap::display();
            break;
        case ACTION_GRAB:
            send_ui( "grab", [] {
                choose_adjacent( _( "Grab where?  (Your own tile lets go.)" ) );
            } );
            break;
        case ACTION_HAUL_TOGGLE:
            send_ui( "haul_toggle", [] {
                get_avatar().toggle_hauling();
            } );
            break;
        case ACTION_DIARY:
            diary::show_diary_ui( get_avatar().get_avatar_diary() );
            break;
        case ACTION_ITEMACTION:
            send_ui( "apply_menu", [] {
                g->mp_item_action_menu();
            } );
            break;
        case ACTION_TOGGLE_AUTO_PICKUP:
        case ACTION_TOGGLE_AUTO_FEATURES:
        case ACTION_TOGGLE_AUTOSAFE:
        case ACTION_TOGGLE_PREVENT_OCCLUSION: {
            // options of this computer only
            const std::string opt = act == ACTION_TOGGLE_AUTO_PICKUP ? "AUTO_PICKUP" :
                                    act == ACTION_TOGGLE_AUTO_FEATURES ? "AUTO_FEATURES" :
                                    act == ACTION_TOGGLE_AUTOSAFE ? "AUTOSAFEMODE" : "PREVENT_OCCLUSION";
            get_options().get_option( opt ).setNext();
            get_options().save();
            add_msg( m_info, _( "Set %s to %s." ), get_options().get_option( opt ).getMenuText(),
                     get_options().get_option( opt ).getValueName() );
            break;
        }
        case ACTION_PANEL_MGMT:
            panel_manager::get_manager().show_adm();
            break;
        case ACTION_KEYBINDINGS:
        case ACTION_NULL:
            break;
        case ACTION_CHAT:
            // talking starts by walking into somebody, like in the normal game
            add_msg( m_info, _( "Walk into somebody to talk to them." ) );
            break;
        case ACTION_INVENTORY:
            // the item menu of the screen sends its choice to the host (client_item_action)
            game_menus::inv::common();
            break;
        case ACTION_THROW:
        case ACTION_THROW_WIELDED:
        case ACTION_FIRE:
        case ACTION_FIRE_BURST:
            add_msg( m_info, _( "Throwing and shooting are aimed with the mouse in fluid mode." ) );
            break;
        case ACTION_SMASH:
            send_ui( "smash", [] {
                smash_menu();
            } );
            break;
        // actions that only start an activity: the menus run here, the host starts it
        case ACTION_BUTCHER:
            send_ui( "capture", [] {
                g->butcher();
            }, true );
            break;
        case ACTION_DISASSEMBLE:
            send_ui( "capture", [] {
                get_avatar().disassemble();
            }, true );
            break;
        case ACTION_MEND:
            send_ui( "capture", [] {
                avatar_action::mend( get_avatar(), item_location() );
            }, true );
            break;
        case ACTION_UNLOAD:
            send_ui( "capture", [] {
                avatar_action::unload( get_avatar() );
            }, true );
            break;
        case ACTION_UNLOAD_CONTAINER:
            send_ui( "capture", [] {
                g->unload_container();
            }, true );
            break;
        case ACTION_INSERT_ITEM:
            send_ui( "capture", [] {
                g->insert_item();
            }, true );
            break;
        case ACTION_RELOAD_ITEM:
            send_ui( "capture", [] {
                g->reload_item();
            }, true );
            break;
        case ACTION_RELOAD_WIELDED:
            send_ui( "capture", [] {
                g->reload_wielded();
            }, true );
            break;
        case ACTION_RELOAD_WEAPON:
            send_ui( "capture", [] {
                g->reload_weapon();
            }, true );
            break;
        // views that only show things
        case ACTION_MESSAGES:
            Messages::display_messages();
            break;
        case ACTION_MORALE:
            get_avatar().disp_morale();
            break;
        case ACTION_HELP:
            get_help().display_help();
            break;
        case ACTION_MUTATIONS:
            send_ui( "mutations", [] {
                get_avatar().power_mutations();
            } );
            break;
        case ACTION_BIONICS:
            send_ui( "bionics", [] {
                get_avatar().power_bionics();
            } );
            break;
        case ACTION_ADVANCEDINV:
            // the item moves it starts are carried out by the host
            send_ui( "aim", [] {
                create_advanced_inv();
            }, true );
            break;
        case ACTION_CRAFT:
            send_craft( false );
            break;
        case ACTION_LONGCRAFT:
            send_craft( true );
            break;
        case ACTION_RECRAFT:
            // the last recipe again: the host knows what this player's body crafted last (the
            // client's copy of the character does not keep that)
            impl::send_json( impl::state().server_peer, message( []( JsonOut & jo ) {
                jo.member( "t", "use" );
                jo.member( "a", "craft" );
                jo.member( "re", true );
                jo.member( "r", "" );
                jo.member( "b", 1 );
                jo.member( "long", false );
            } ) );
            break;
        case ACTION_OPEN:
            send_door( "open" );
            break;
        case ACTION_CLOSE:
            send_door( "close" );
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
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: client action not available: " << action_ident( act );
            add_msg( m_info, _( "Not available in multiplayer yet." ) );
            break;
    }
    break;
    }
    return false;
}

// The inventory screen of a client: what its item menu chose is sent to the host.
bool client_item_action_impl( int key, const item_location &chosen )
{
    item_location loc = chosen;
    switch( key ) {
        case 'a':
            send_ui( "apply", [loc]() mutable {
                // the host picks the item from the answers, like from the menu
                remote_ui::record( "i", remote_ui::location_to_string( loc ) );
                avatar_action::use_item( get_avatar(), loc );
            } );
            return true;
        case 'E':
            if( !loc->is_container() ) {
                send_use( "consume", loc );
            } else {
                send_ui( "capture", [loc]() mutable {
                    avatar_action::eat_or_use( get_avatar(), game_menus::inv::consume( loc ) );
                }, true );
            }
            return true;
        case 'W':
            send_use( "wear", loc );
            return true;
        case 'w':
            send_use( "wield", loc );
            return true;
        case 't':
            // throwing is aimed with the mouse (fluid mode): hold the item, press the throw key
            send_use( "wield", loc );
            add_msg( m_info, _( "Aim with the mouse and press the throw key to throw it." ) );
            return true;
        case 'T':
            send_use( "takeoff", loc );
            return true;
        case 'R':
            send_use( "read", loc );
            return true;
        case 'd':
            send_drop( { { loc, loc->count_by_charges() ? loc->charges : 0 } } );
            return true;
        case 'U':
            send_ui( "capture", [loc]() mutable {
                get_avatar().unload( loc );
            }, true );
            return true;
        case 'r':
        case 'p':
            send_ui( "capture", [loc, key]() mutable {
                g->reload( loc, key == 'p' );
            }, true );
            return true;
        case 'm':
            send_ui( "capture", [loc]() mutable {
                avatar_action::mend( get_avatar(), loc );
            }, true );
            return true;
        case 'D':
            send_ui( "capture", [loc]() mutable {
                get_avatar().disassemble( loc, false );
            }, true );
            return true;
        case 'i':
            if( loc->is_container() ) {
                send_ui( "capture", [loc]() mutable {
                    game_menus::inv::insert_items( loc );
                }, true );
            }
            return true;
        case 'c':
            send_ui( "item_change_side", [loc]() mutable {
                remote_ui::record( "i", remote_ui::location_to_string( loc ) );
                get_avatar().change_side( loc );
            } );
            return true;
        case 'f':
            send_ui( "item_favorite", [loc]() mutable {
                remote_ui::record( "i", remote_ui::location_to_string( loc ) );
                loc->is_favorite = !loc->is_favorite;
            } );
            return true;
        default:
            // views and settings that stay on this computer
            return false;
    }
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
    if( json.find( "\"scenario\"" ) == std::string::npos ) {
        json.insert( open + 1, string_format( "\"scenario\":\"%s\",", get_scenario()->ident().str() ) );
    }
    avatar &u = get_avatar();
    const tripoint_abs_ms pos = u.pos_abs();
    const character_id own_id = u.getID();
    try {
        // Item locations in it (an activity's targets ...) name the host's body; here that
        // is the own character, which has another id.
        JsonValue jv = json_loader::from_string( std::string( json ) );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        const int body_id = jo.get_int( "id", -1 );
        if( body_id >= 0 && body_id != own_id.get_value() ) {
            json = remote_ui::remap_character_id( std::move( json ), body_id, own_id.get_value() );
        }
    } catch( const std::exception & ) {
        // handled below by the real load
    }
    // loading sets the id from the data, which it refuses while one is set
    u.setID( character_id(), true );
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

bool client_item_action( int key, const item_location &loc )
{
    return client_item_action_impl( key, loc );
}

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
    } else if( type == "out_of_lives" ) {
        // C1: no extra life left: a new character, made here, then join again
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: out of lives, making a new character";
        net::shutdown();
        popup( _( "You have no extra life left.  Make a new character to go on playing." ) );
        mp_lives::make_new_character( get_avatar() );
        net::request_reconnect();
    } else if( type == "reject" ) {
        add_msg( m_bad, _( "Multiplayer: the host refused the connection (%s)." ),
                 msg.get_string( "reason", "" ) );
    } else if( type == "submaps" ) {
        apply_submaps( msg.get_array( "list" ) );
    } else if( type == "fx" ) {
        apply_fx( msg.get_array( "list" ) );
    } else if( type == "bp" ) {
        // free movement: where every player's body is (channel 1, ~20 times a second)
        std::vector<fluid::body_sample> list;
        for( JsonArray a : msg.get_array( "l" ) ) {
            if( a.size() < 4 ) {
                continue;
            }
            fluid::body_sample b;
            b.actor = a.get_int( 0 );
            b.x = a.get_int( 1 ) / 1000.0;
            b.y = a.get_int( 2 ) / 1000.0;
            b.z = a.get_int( 3 );
            list.push_back( b );
        }
        if( cstate().fluid_on ) {
            fluid::client_bodies( list, msg.get_int( "q", -1 ), fluid::now_ms() );
        }
    } else if( type == "aim" ) {
        fluid::client_aim_status( msg.get_int( "s", 0 ) / 1000.0, msg.get_int( "r", 0 ) / 1000.0,
                                  msg.get_bool( "f", false ), msg.get_bool( "a", false ) );
    } else if( type == "state" ) {
        apply_state( msg );
    } else if( type == "npc_menu" ) {
        npc_menu_msg m;
        m.id = msg.get_int( "id", -1 );
        m.title = msg.get_string( "title", "" );
        for( JsonObject o : msg.get_array( "entries" ) ) {
            npc_menu_msg::entry e;
            e.option = o.get_int( "o", 0 );
            const std::string key = o.get_string( "k", " " );
            e.key = key.empty() ? ' ' : key[0];
            e.text = o.get_string( "x", "" );
            e.enabled = o.get_bool( "ok", true );
            e.risky = o.get_bool( "r", false );
            m.entries.push_back( e );
        }
        npc_menu_queue().push_back( m );
    } else if( type == "zones" ) {
        zones_json() = msg.get_string( "zones", "" );
    } else if( type == "missions" || type == "factions" ) {
        missions_text() = msg.get_string( "text", "" );
    } else if( type == "omap" ) {
        // terrain of the overmap around this player: what is near is seen
        omap_arrived() = true;
        const int x0 = msg.get_int( "x", 0 );
        const int y0 = msg.get_int( "y", 0 );
        const int z = msg.get_int( "z", 0 );
        const int w = std::max( 1, msg.get_int( "w", 1 ) );
        std::vector<std::string> table;
        for( const std::string s : msg.get_array( "tbl" ) ) {
            table.push_back( s );
        }
        std::vector<int> levels;
        for( const int l : msg.get_array( "l" ) ) {
            levels.push_back( l );
        }
        int i = 0;
        for( const int v : msg.get_array( "v" ) ) {
            const tripoint_abs_omt p( x0 + i % w, y0 + i / w, z );
            const int lv = i < static_cast<int>( levels.size() ) ? levels[i] : 0;
            i++;
            if( v >= 0 && v < static_cast<int>( table.size() ) ) {
                const oter_str_id oid( table[v] );
                if( oid.is_valid() ) {
                    overmap_buffer.ter_set( p, oid.id() );
                    if( overmap_buffer.seen( p ) < static_cast<om_vision_level>( lv ) ) {
                        overmap_buffer.set_seen( p, static_cast<om_vision_level>( lv ) );
                    }
                }
            }
        }
    } else if( type == "npc_view" ) {
        npc_view_msg m;
        m.kind = msg.get_string( "kind", "" );
        m.precise = msg.get_bool( "precise", false );
        m.npc_json = msg.get_string( "npc", "" );
        npc_view_queue().push_back( m );
    } else if( type == "trade" ) {
        trade_msg m;
        m.npc_json = msg.get_string( "npc", "" );
        m.cost = msg.get_int( "cost", 0 );
        m.deal = msg.get_string( "deal", "" );
        trade_queue().push_back( m );
    } else if( type == "talk" ) {
        talk_msg m;
        m.end = msg.get_bool( "end", false );
        m.name = msg.get_string( "name", "" );
        for( JsonArray op : msg.get_array( "ops" ) ) {
            std::vector<std::string> parts;
            for( JsonValue e : op ) {
                parts.push_back( e.get_string() );
            }
            m.ops.push_back( parts );
        }
        for( JsonObject o : msg.get_array( "resp" ) ) {
            remote_conversation::response r;
            r.color = o.get_int( "c", 0 );
            r.hotkey_desc = o.get_string( "h", "" );
            r.text = o.get_string( "x", "" );
            r.key = o.get_int( "k", 0 );
            r.enabled = o.get_bool( "ok", true );
            r.consequence = o.get_int( "q", 0 );
            m.resp.push_back( r );
        }
        talk_queue().push_back( m );
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

int client_send_input( const fluid::input_frame &in )
{
    client_state &cs = cstate();
    if( !cs.welcomed || !cs.fluid_on || impl::state().server_peer == nullptr ) {
        return -1;
    }
    static int seq = 0;
    static fluid::input_frame last;
    static fluid::ms_t last_at = -100000;
    const fluid::ms_t now = fluid::now_ms();
    const bool buttons = in.move_x != last.move_x || in.move_y != last.move_y ||
                         in.primary != last.primary || in.secondary != last.secondary ||
                         in.cancel != last.cancel || in.sprint != last.sprint ||
                         in.primary_presses != last.primary_presses ||
                         in.secondary_presses != last.secondary_presses ||
                         in.throw_presses != last.throw_presses ||
                         in.cursor_valid != last.cursor_valid;
    const double moved = std::abs( in.cursor_x - last.cursor_x ) + std::abs( in.cursor_y - last.cursor_y );
    const bool send = ( now - last_at >= 100 ) || ( buttons && now - last_at >= 12 ) ||
                      ( in.cursor_valid && moved > 0.12 && now - last_at >= 40 );
    if( !send ) {
        return -1;
    }
    last = in;
    last_at = now;
    ++seq;
    impl::send_json_fast( impl::state().server_peer, message( [&]( JsonOut & jo ) {
        jo.member( "t", "in" );
        jo.member( "q", seq );
        jo.member( "mx", in.move_x );
        jo.member( "my", in.move_y );
        jo.member( "p", in.primary );
        jo.member( "s", in.secondary );
        jo.member( "pc", in.primary_presses );
        jo.member( "sc", in.secondary_presses );
        jo.member( "tc", in.throw_presses );
        jo.member( "c", in.cancel );
        jo.member( "sp", in.sprint );
        jo.member( "cv", in.cursor_valid );
        jo.member( "cx", in.cursor_x );
        jo.member( "cy", in.cursor_y );
        jo.member( "cz", in.cursor_z );
    } ) );
    return seq;
}

void client_place_shadow( int actor, const tripoint_abs_ms &tile )
{
    client_state &cs = cstate();
    const auto iter = cs.shadows.find( actor );
    map &here = get_map();
    if( iter == cs.shadows.end() || !here.inbounds( tile ) ) {
        return;
    }
    if( npc *guy = g->find_npc( iter->second ) ) {
        guy->setpos( here, here.get_bub( tile ), false );
    }
}

bool client_do_turn()
{
    poll();
    mod_engine::tick();
    fluid::client_frame();
    mp_test_mapmem_client();
    ui_manager::redraw_invalidated();
    // the overmap copy gets the terrain around the player whenever it moves to another tile
    {
        static tripoint_abs_omt last_omt = tripoint_abs_omt::invalid;
        const tripoint_abs_omt here_omt = get_avatar().pos_abs_omt();
        if( here_omt != last_omt ) {
            last_omt = here_omt;
            impl::send_json( impl::state().server_peer, message( []( JsonOut & jo ) {
                jo.member( "t", "omap_req" );
            } ) );
        }
    }
    if( handle_input() ) {
        // what this player has seen stays with them for the next visit (the world itself is not saved)
        get_avatar().save_map_memory();
        shutdown();
        // The next do_turn() ends the game without saving.
        g->uquit = QUIT_NOSAVED;
    }
    poll();
    return false;
}

} // namespace net
