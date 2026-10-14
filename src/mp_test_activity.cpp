#include "mp_test_menus.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "creature_tracker.h"
#include "debug.h"
#include "fluid.h"
#include "item.h"
#include "item_location.h"
#include "map.h"
#include "map_selector.h"
#include "npc.h"
#include "player_activity.h"
#include "type_id.h"

static const activity_id ACT_WAIT( "ACT_WAIT" );

namespace net
{

namespace
{

// A zombie corpse on a free tile next to @p who, or null.
item_location corpse_next_to( Character &who )
{
    map &here = get_map();
    const tripoint_bub_ms at = who.pos_bub( here );
    for( const tripoint_bub_ms &p : here.points_in_radius( at, 1 ) ) {
        if( p == at || !here.passable( p ) || !here.i_at( p ).empty() ||
            get_creature_tracker().creature_at( p ) != nullptr ) {
            continue;
        }
        item &corpse = here.add_item_or_charges( p, item::make_corpse( mtype_id( "mon_zombie" ),
                       calendar::turn ) );
        return item_location( map_cursor( p ), &corpse );
    }
    return item_location();
}

void start_on( Character &who, const std::string &kind )
{
    if( kind == "wait" ) {
        // TIME based (legacy activity)
        who.assign_activity( player_activity( ACT_WAIT, to_moves<int>( 60_minutes ) ) );
    } else if( kind == "meditate" ) {
        // SPEED based
        who.assign_activity( meditate_activity_actor() );
    } else if( kind == "pulp" ) {
        // SPEED based, but one blow per do_turn call
        const item_location corpse = corpse_next_to( who );
        if( !corpse ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test activity no place for a corpse";
            return;
        }
        who.assign_activity( pulp_activity_actor( corpse ) );
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test activity started " << kind << " on " <<
                               who.get_name() << ( who.activity ? " ok" : " FAILED" );
}

struct watch {
    fluid::ms_t started = 0;
    time_point game_start;
    fluid::ms_t last_log = 0;
    bool ended = false;
};

void report( Character &who, watch &w, fluid::ms_t now )
{
    if( w.ended || now - w.last_log < 1000 ) {
        return;
    }
    w.last_log = now;
    const double real_s = ( now - w.started ) / 1000.0;
    const int game_s = to_seconds<int>( calendar::turn - w.game_start );
    if( !who.activity ) {
        w.ended = true;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test activity " << who.get_name() <<
                                   " ended after real " << real_s << " s, game " << game_s << " s";
        return;
    }
    std::string extra;
    if( who.activity.id() == activity_id( "ACT_PULP" ) ) {
        for( const tripoint_bub_ms &p : get_map().points_in_radius( who.pos_bub(), 1 ) ) {
            for( const item &it : get_map().i_at( p ) ) {
                if( it.is_corpse() ) {
                    extra = " corpse damage " + std::to_string( it.damage() ) + "/" +
                            std::to_string( it.max_damage() );
                }
            }
        }
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test activity " << who.get_name() << " " <<
                               who.activity.id().str() << " left " << who.activity.moves_left << "/" <<
                               who.activity.moves_total << extra << " real " << real_s << " s game " << game_s << " s";
}

} // namespace

void mp_test_activity( npc &guy )
{
    static const char *kind = std::getenv( "CATA_MP_TEST_ACTIVITY" );
    if( kind == nullptr ) {
        return;
    }
    static std::map<int, watch> watches;
    const fluid::ms_t now = fluid::now_ms();
    avatar &u = get_avatar();
    for( Character *who : std::initializer_list<Character *> { &u, &guy } ) {
        const int id = who->getID().get_value();
        auto iter = watches.find( id );
        if( iter == watches.end() ) {
            start_on( *who, kind );
            watch w;
            w.started = now;
            w.game_start = calendar::turn;
            iter = watches.emplace( id, w ).first;
        }
        report( *who, iter->second, now );
    }
}

} // namespace net
