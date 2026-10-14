#include "mp_test_free.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "avatar.h"
#include "bubbles.h"
#include "calendar.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "current_map.h"
#include "debug.h"
#include "fluid.h"
#include "game.h"
#include "line.h"
#include "map.h"
#include "mapdata.h"
#include "monster.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "point.h"
#include "type_id.h"
#include "units.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace net
{

namespace
{
const efftype_id effect_immobilization_free( "immobilization" );

struct free_state {
    fluid::ms_t first_seen = 0;
    fluid::ms_t ready_at = 0;
    bool built = false;
    bool finished = false;
    tripoint_abs_ms start;
    tripoint_abs_ms target;
    std::string last;
};
} // namespace

void test_free( npc &guy )
{
    static const char *env = std::getenv( "CATA_MP_TEST_FREE" );
    if( env == nullptr ) {
        return;
    }
    const std::string mode = env;
    static std::map<int, free_state> states;
    free_state &fs = states[guy.getID().get_value()];
    if( fs.finished ) {
        return;
    }
    const fluid::ms_t now = fluid::now_ms();
    if( fs.first_seen == 0 ) {
        fs.first_seen = now;
        return;
    }
    if( !fs.built ) {
        if( now - fs.first_seen < 30000 ) {
            return;
        }
        if( std::getenv( "CATA_MP_TEST_BUBBLE" ) != nullptr &&
            rl_dist( guy.pos_abs(), get_avatar().pos_abs() ) < 100 ) {
            if( now - fs.first_seen > 150000 ) {
                fs.finished = true;
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test free failed: body never left the host's bubble";
            }
            return;
        }
        fs.built = true;
        fs.ready_at = now;
        map &bm = bubbles::map_for( guy );
        swap_map swap( bm );
        const tripoint_bub_ms p = guy.pos_bub( bm );
        const ter_id wall = ter_str_id( "t_wall" ).id();
        const ter_id dirt = ter_str_id( "t_dirt" ).id();
        // corridor: walls north and south, open dirt from x-1 to x+12
        for( int dx = -2; dx <= 13; dx++ ) {
            for( int dy = -2; dy <= 2; dy++ ) {
                const tripoint_bub_ms t = p + tripoint_rel_ms( dx, dy, 0 );
                if( !bm.inbounds( t ) || ( dx == 0 && dy == 0 ) ) {
                    continue;
                }
                bm.furn_set( t, furn_str_id( "f_null" ) );
                bm.i_clear( t );
                const bool inside = dy == 0 && dx >= -1 && dx <= 12;
                bm.ter_set( t, inside ? dirt : wall );
            }
        }
        const tripoint_bub_ms obstacle = p + tripoint_rel_ms( 3, 0, 0 );
        fs.start = guy.pos_abs();
        fs.target = bm.get_abs( obstacle );
        std::string what = "none";
        if( mode == "door" ) {
            bm.ter_set( obstacle, ter_str_id( "t_door_c" ).id() );
            what = "t_door_c";
        } else if( mode == "attack" ) {
            if( monster *z = g->place_critter_at( mtype_id( "mon_zombie" ), obstacle ) ) {
                z->unique_name = "TESTFREE";
                z->add_effect( effect_immobilization_free, 600_minutes );
                z->set_hp( 100000 );
                what = "mon_zombie";
            }
        } else if( mode == "talk" ) {
            shared_ptr_fast<npc> other = make_shared_fast<npc>();
            other->normalize();
            other->randomize();
            other->spawn_at_precise( fs.target );
            overmap_buffer.insert_npc( other );
            other->set_attitude( NPCATT_NULL );
            other->mission = NPC_MISSION_GUARD;
            other->add_effect( effect_immobilization_free, 600_minutes );
            g->load_npcs();
            what = "npc";
        } else if( mode == "squeeze" ) {
            // the corridor is closed east of the body; the only way on is the diagonal gap north-east
            bm.ter_set( p + tripoint_rel_ms( 1, 0, 0 ), wall );
            bm.ter_set( p + tripoint_rel_ms( 1, -1, 0 ), dirt );
            fs.target = bm.get_abs( p + tripoint_rel_ms( 1, -1, 0 ) );
            what = "gap";
        } else if( mode == "swim" ) {
            for( int dx = 3; dx <= 6; dx++ ) {
                bm.ter_set( p + tripoint_rel_ms( dx, 0, 0 ), ter_str_id( "t_water_dp" ).id() );
            }
            what = "t_water_dp";
        } else if( mode == "ledge" ) {
            bm.ter_set( obstacle, ter_str_id( "t_open_air" ).id() );
            what = "t_open_air";
        } else if( mode == "vehicle" ) {
            if( bm.add_vehicle( vproto_id( "bicycle" ), obstacle, 0_degrees, 0, 0 ) != nullptr ) {
                what = "bicycle";
            }
        }
        bm.build_map_cache( p.z(), true );
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test free ready case=" << mode << " body=" <<
                                   guy.getID().get_value() << " at " << fs.start.to_string() << " obstacle=" << what <<
                                   " target=" << fs.target.to_string();
        return;
    }
    if( now - fs.ready_at > 60000 ) {
        fs.finished = true;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test free end case=" << mode;
        return;
    }
    map &bm = bubbles::map_for( guy );
    swap_map swap( bm );
    const tripoint_abs_ms at = guy.pos_abs();
    std::string state = "off=" + std::to_string( at.x() - fs.start.x() ) + "," +
                        std::to_string( at.y() - fs.start.y() ) + "," + std::to_string( at.z() - fs.start.z() );
    const tripoint_bub_ms tb = bm.get_bub( fs.target );
    if( bm.inbounds( tb ) ) {
        state += " target_ter=" + bm.ter( tb ).id().str();
        if( const Creature *c = get_creature_tracker().creature_at( tb ) ) {
            if( c != &guy ) {
                state += " target_hp=" + std::to_string( c->get_hp() );
            }
        }
    }
    state += std::string( " in_vehicle=" ) + ( guy.in_vehicle ? "1" : "0" );
    state += std::string( " underwater=" ) + ( guy.is_underwater() ? "1" : "0" );
    if( state != fs.last ) {
        fs.last = state;
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test free state case=" << mode << " t=" <<
                                   ( now - fs.ready_at ) << " " << state;
    }
}

} // namespace net
