#include "stations.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "alerts.h"
#include "avatar.h"
#include "calendar.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "coordinates.h"
#include "filesystem.h"
#include "flexbuffer_json.h"
#include "item.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "net_session.h"
#include "npc.h"
#include "path_info.h"
#include "recipe.h"
#include "string_formatter.h"
#include "translations.h"

namespace stations
{
namespace
{

// item_counter of a craft goes from 0 to this (100 %)
constexpr int full_progress = 10000000;
// a station alone works at this fraction of a crafter's speed
constexpr double passive_speed = 0.5;
// how far around each player new station crafts are looked for
constexpr int scan_radius = 12;

struct registry {
    bool loaded = false;
    std::set<tripoint_abs_ms> places;
};

registry &reg()
{
    static registry r;
    return r;
}

cata_path registry_file()
{
    return PATH_INFO::world_base_save_path() / "mp_stations.json";
}

void load_registry()
{
    registry &r = reg();
    if( r.loaded ) {
        return;
    }
    r.loaded = true;
    r.places.clear();
    if( !file_exist( registry_file() ) ) {
        return;
    }
    try {
        JsonValue jv = json_loader::from_path( registry_file() );
        for( JsonArray place : jv.get_array() ) {
            const int x = place.next_int();
            const int y = place.next_int();
            const int z = place.next_int();
            r.places.emplace( x, y, z );
        }
    } catch( const std::exception & ) {
        r.places.clear();
    }
}

bool is_station( const map &here, const tripoint_bub_ms &p )
{
    return static_cast<bool>( here.furn( p ).obj().workbench );
}

std::vector<Character *> players()
{
    std::vector<Character *> result{ &get_avatar() };
    for( npc *guy : net::remote_player_npcs() ) {
        result.push_back( guy );
    }
    return result;
}

void scan_for_new( map &here )
{
    registry &r = reg();
    for( Character *who : players() ) {
        const tripoint_bub_ms center = who->pos_bub( here );
        for( const tripoint_bub_ms &p : here.points_in_radius( center, scan_radius ) ) {
            if( !is_station( here, p ) ) {
                continue;
            }
            for( const item &it : here.i_at( p ) ) {
                if( it.is_craft() && it.item_counter < full_progress ) {
                    r.places.insert( here.get_abs( p ) );
                    break;
                }
            }
        }
    }
}

// Advances one craft by the game time since it was last looked at.
void advance( item &craft, const tripoint_abs_ms &where )
{
    const int now = to_turn<int>( calendar::turn );
    const int last = static_cast<int>( craft.get_var( "station_last", static_cast<double>( now ) ) );
    craft.set_var( "station_last", now );
    if( craft.has_var( "crafter" ) ) {
        // somebody works on it by hand: that progress is counted by the craft activity
        return;
    }
    const int elapsed = now - last;
    if( elapsed <= 0 || craft.item_counter >= full_progress ) {
        return;
    }
    const recipe &rec = craft.get_making();
    const int64_t total_moves = std::max<int64_t>( 1, rec.batch_time( get_avatar(),
                                craft.get_making_batch_size(), 1.0f, 0 ) );
    // moves are 1/100 s of crafting
    const double gained = elapsed * 100.0 * passive_speed / total_moves * full_progress;
    craft.item_counter = static_cast<int>( std::min<double>( full_progress,
                                           craft.item_counter + std::floor( gained ) ) );
    if( craft.item_counter >= full_progress && !craft.has_var( "station_done" ) ) {
        craft.set_var( "station_done", "1" );
        const std::string text = string_format( _( "The %s is finished.  Resume it to collect it." ),
                                                craft.tname() );
        for( Character *who : players() ) {
            if( rl_dist( who->pos_abs(), where ) <= 60 ) {
                net::alert_player( *who, text, static_cast<int>( alerts::level::notice ), where );
            }
        }
    }
}

} // namespace

void process()
{
    if( !alerts::nonblocking() || net::is_client() || !calendar::once_every( 1_minutes ) ) {
        return;
    }
    load_registry();
    map &here = get_map();
    if( calendar::once_every( 5_minutes ) ) {
        scan_for_new( here );
    }
    registry &r = reg();
    for( auto iter = r.places.begin(); iter != r.places.end(); ) {
        if( !here.inbounds( *iter ) ) {
            // not loaded: it catches up the next time it is
            ++iter;
            continue;
        }
        const tripoint_bub_ms p = here.get_bub( *iter );
        bool any = false;
        if( is_station( here, p ) ) {
            for( item &it : here.i_at( p ) ) {
                if( it.is_craft() ) {
                    any = true;
                    advance( it, *iter );
                }
            }
        }
        if( any ) {
            ++iter;
        } else {
            iter = r.places.erase( iter );
        }
    }
}

void reset()
{
    reg() = registry();
}

void save()
{
    const registry &r = reg();
    if( !r.loaded ) {
        return;
    }
    write_to_file( registry_file(), [&]( std::ostream & fout ) {
        JsonOut jo( fout );
        jo.start_array();
        for( const tripoint_abs_ms &p : r.places ) {
            jo.start_array();
            jo.write( p.x() );
            jo.write( p.y() );
            jo.write( p.z() );
            jo.end_array();
        }
        jo.end_array();
    }, _( "crafting stations" ) );
}

} // namespace stations
