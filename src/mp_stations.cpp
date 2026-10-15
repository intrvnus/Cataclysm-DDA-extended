#include "mp_stations.h"

#include <algorithm>
#include <set>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_path.h"
#include "character.h"
#include "debug.h"
#include "filesystem.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "mapdata.h"
#include "messages.h"
#include "net_session.h"
#include "npc.h"
#include "options.h"
#include "path_info.h"
#include "player_activity.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "remote_ui.h"
#include "crafting_gui.h"
#include "string_formatter.h"
#include "translations.h"
#include "veh_type.h"
#include "vpart_position.h"

namespace mp_stations
{

namespace
{
// how often the stations work (turns), and their speed as a share of a player's
constexpr int tick_turns = 10;
constexpr double unattended_share = 0.25;
constexpr double queue_share = 0.5;
// unattended crafts stop just before the end: the player finishes them
constexpr int unattended_cap = 9900000;
constexpr int counter_full = 10000000;

std::set<tripoint_abs_ms> &stations()
{
    static std::set<tripoint_abs_ms> s;
    return s;
}

bool &queueing()
{
    static bool q = false;
    return q;
}

cata_path stations_file()
{
    return PATH_INFO::world_base_save_path() / "mp_players" / "stations.json";
}

bool is_station( const map &here, const tripoint_bub_ms &p )
{
    if( here.furn( p ).obj().workbench ) {
        return true;
    }
    const optional_vpart_position vp = here.veh_at( p );
    return vp && vp.part_with_feature( "WORKBENCH", true );
}

// somebody right there is working on something: the station is attended
bool attended( const tripoint_abs_ms &where )
{
    return !g->get_characters_if( [&where]( const Character & guy ) {
        return guy.activity && rl_dist( guy.pos_abs(), where ) <= 1;
    } ).empty();
}

Character *character_by_id( int id )
{
    if( get_avatar().getID().get_value() == id ) {
        return &get_avatar();
    }
    return g->find_npc( character_id( id ) );
}

// one station's work for a tick; false if nothing is left there
bool work_station( map &here, const tripoint_abs_ms &where )
{
    const tripoint_bub_ms p = here.get_bub( where );
    if( !is_station( here, p ) ) {
        return false;
    }
    std::vector<item *> crafts;
    for( item &it : here.i_at( p ) ) {
        if( it.is_craft() ) {
            crafts.push_back( &it );
        }
    }
    if( crafts.empty() ) {
        return false;
    }
    if( attended( where ) ) {
        return true;
    }
    const double speed = std::max( 1, get_option<int>( "FLUID_ACTIVITY_SPEED" ) );
    bool queue_worked = false;
    for( item *craft : crafts ) {
        const int queued_by = static_cast<int>( craft->get_var( "mp_queue", -1.0 ) );
        const bool queued = queued_by >= 0;
        if( queued && queue_worked ) {
            // one queued recipe at a time
            continue;
        }
        Character *crafter = queued ? character_by_id( queued_by ) : nullptr;
        // the speed is the one of who queued it or last worked on it (never the host's own
        // character; a dedicated server's is a placeholder without skills)
        const Character *worker = crafter != nullptr ? crafter :
                                  character_by_id( static_cast<int>( craft->get_var( "mp_crafter", -1.0 ) ) );
        const recipe &rec = craft->get_making();
        double base_moves = craft->get_var( "mp_base_moves", 0.0 );
        if( worker != nullptr ) {
            // remembered on the craft: it keeps that speed while its crafter is offline
            base_moves = std::max<int64_t>( 1, rec.batch_time( *worker, craft->get_making_batch_size(), 1.0f, 0 ) );
            craft->set_var( "mp_base_moves", base_moves );
        } else if( base_moves <= 0 ) {
            base_moves = std::max<int64_t>( 1, rec.batch_time( get_avatar(), craft->get_making_batch_size(), 1.0f,
                                            0 ) );
        }
        const double share = queued ? queue_share : unattended_share;
        const double delta = share * speed * tick_turns * 100.0 / base_moves * counter_full;
        const int cap = queued ? counter_full : unattended_cap;
        if( craft->item_counter < cap ) {
            craft->item_counter = std::min<int>( cap, craft->item_counter + std::max( 1.0, delta ) );
        }
        if( queued ) {
            queue_worked = true;
            if( craft->item_counter >= counter_full && crafter != nullptr ) {
                // done: made for the player who queued it, onto the station
                item done = *craft;
                item_location loc( map_cursor( &here, p ), craft );
                loc.remove_item();
                crafter->complete_craft( done, p );
                crafter->add_msg_if_player( m_good, _( "The station finished your %s." ),
                                            done.get_making().result_name() );
                // the list of items changed: the next tick goes on
                return true;
            }
        }
    }
    return true;
}
} // namespace

void note_craft_at( const tripoint_abs_ms &where )
{
    if( net::is_host() ) {
        stations().insert( where );
    }
}

bool queue_mode()
{
    return queueing();
}

void tick()
{
    if( !net::is_host() || stations().empty() || !calendar::once_every( time_duration::from_turns(
                tick_turns ) ) ) {
        return;
    }
    map &here = get_map();
    for( auto iter = stations().begin(); iter != stations().end(); ) {
        if( !here.inbounds( *iter ) ) {
            // not loaded: it waits (B3 decision: it keeps working when somebody is near)
            ++iter;
            continue;
        }
        if( work_station( here, *iter ) ) {
            ++iter;
        } else {
            iter = stations().erase( iter );
        }
    }
}

bool mp_craft_at( Character &who, const tripoint_abs_ms &where, bool is_long, bool queue )
{
    map &here = get_map();
    if( remote_ui::recording() ) {
        // the client: the usual window, the answer goes to the host
        int batch = 0;
        const std::pair<Character *, const recipe *> chosen =
            select_crafter_and_crafting_recipe( batch, recipe_id(), &who );
        remote_ui::record( "rcp", chosen.second != nullptr ? chosen.second->ident().str() : std::string() );
        remote_ui::record( "bat", std::to_string( std::max( 1, batch ) ) );
        return true;
    }
    if( !remote_ui::replaying() ) {
        return false;
    }
    const recipe_id id( remote_ui::answer( "rcp" ).value_or( "" ) );
    const int batch = std::clamp( std::atoi( remote_ui::answer( "bat" ).value_or( "1" ).c_str() ), 1, 1000 );
    if( id.str().empty() || !id.is_valid() || !here.inbounds( where ) ) {
        return true;
    }
    const recipe &rec = id.obj();
    if( !who.knows_recipe( &rec ) && !who.get_group_available_recipes().contains( &rec ) ) {
        who.add_msg_if_player( m_info, _( "You don't know how to make that." ) );
        return true;
    }
    if( !who.can_make( &rec, batch ) ) {
        who.add_msg_if_player( m_info, _( "You can't make that right now." ) );
        return true;
    }
    const tripoint_bub_ms loc = here.get_bub( where );
    queueing() = queue;
    if( is_long && !queue ) {
        who.make_all_craft( id, batch, loc );
    } else {
        who.make_craft( id, batch, loc );
    }
    queueing() = false;
    return true;
}

void queue_menu( Character &who, const tripoint_abs_ms &where )
{
    if( mp_craft_at( who, where, false, true ) ) {
        return;
    }
    map &here = get_map();
    if( !here.inbounds( where ) ) {
        return;
    }
    queueing() = true;
    who.craft( here.get_bub( where ) );
    queueing() = false;
}

void load()
{
    stations().clear();
    if( !file_exist( stations_file() ) ) {
        return;
    }
    read_from_file_optional_json( stations_file(), []( const JsonValue & jv ) {
        for( JsonArray a : jv.get_array() ) {
            const int x = a.next_int();
            const int y = a.next_int();
            const int z = a.next_int();
            stations().emplace( x, y, z );
        }
    } );
}

void save()
{
    assure_dir_exist( PATH_INFO::world_base_save_path() / "mp_players" );
    write_to_file( stations_file(), []( std::ostream & fout ) {
        JsonOut jo( fout );
        jo.start_array();
        for( const tripoint_abs_ms &p : stations() ) {
            jo.start_array();
            jo.write( p.x() );
            jo.write( p.y() );
            jo.write( p.z() );
            jo.end_array();
        }
        jo.end_array();
    }, _( "multiplayer stations" ) );
}

} // namespace mp_stations
