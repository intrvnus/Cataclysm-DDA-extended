// Multiplayer test scenes: the dispatcher and the NPC actions (disarm, steal, use item).
// Only active with CATA_MP_TEST_SCENE set; see mp_test_npc.h.

#include "mp_test_npc.h"

#include <climits>
#include <cstdlib>
#include <functional>
#include <string>

#include "calendar.h"
#include "character.h"
#include "creature_tracker.h"
#include "debug.h"
#include "game.h"
#include "item.h"
#include "line.h"
#include "map.h"
#include "net_session.h"
#include "npc.h"
#include "type_id.h"
#include "visitable.h"

namespace net
{

void mp_test_scenes( npc &guy )
{
    static const char *const env = std::getenv( "CATA_MP_TEST_SCENE" );
    if( env == nullptr ) {
        return;
    }
    // the first player to join is the one the scene is made for
    static character_id owner;
    if( !owner.is_valid() ) {
        owner = guy.getID();
    }
    if( guy.getID() != owner ) {
        return;
    }
    const std::string scene( env );
    if( scene == "disarm" || scene == "steal" || scene == "useitem" ) {
        mp_test_npc_scene( guy, scene );
    } else if( scene == "armor" ) {
        mp_test_armor( guy );
    } else if( scene == "console" ) {
        mp_test_console( guy );
    } else if( scene == "recraft" ) {
        mp_test_recraft( guy );
    } else if( scene == "workout" ) {
        mp_test_workout( guy );
    } else {
        static bool said = false;
        if( !said ) {
            said = true;
            DebugLog( D_WARNING, D_MAIN ) << "Multiplayer: unknown CATA_MP_TEST_SCENE '" << scene <<
                                          "' (disarm, steal, useitem, armor, console, recraft, workout)";
        }
    }
}

npc *mp_test_find_npc( const npc &guy, int radius )
{
    npc *best = nullptr;
    int best_dist = INT_MAX;
    for( npc &n : g->all_npcs() ) {
        if( &n == &guy || is_remote_player( n ) || n.is_dead() || n.posz() != guy.posz() ) {
            continue;
        }
        const int dist = rl_dist( n.pos_abs(), guy.pos_abs() );
        if( dist <= radius && dist < best_dist ) {
            best = &n;
            best_dist = dist;
        }
    }
    return best;
}

bool mp_test_put_next_to( npc &guy, npc &other )
{
    map &here = get_map();
    const tripoint_bub_ms at = guy.pos_bub( here );
    if( rl_dist( at, other.pos_bub( here ) ) == 1 ) {
        return true;
    }
    static const tripoint_rel_ms order[] = {
        tripoint_rel_ms( 0, -1, 0 ), tripoint_rel_ms( 1, 0, 0 ), tripoint_rel_ms( -1, 0, 0 ),
        tripoint_rel_ms( 0, 1, 0 ), tripoint_rel_ms( 1, -1, 0 ), tripoint_rel_ms( -1, -1, 0 ),
        tripoint_rel_ms( 1, 1, 0 ), tripoint_rel_ms( -1, 1, 0 )
    };
    for( const tripoint_rel_ms &d : order ) {
        const tripoint_bub_ms p = at + d;
        if( here.passable( p ) && !here.veh_at( p ) &&
            get_creature_tracker().creature_at( p ) == nullptr ) {
            other.setpos( here, p );
            return true;
        }
    }
    return false;
}

std::string mp_test_dir_key( const tripoint_bub_ms &from, const tripoint_bub_ms &to )
{
    const int dx = to.x() - from.x();
    const int dy = to.y() - from.y();
    if( dx < -1 || dx > 1 || dy < -1 || dy > 1 || ( dx == 0 && dy == 0 ) ) {
        return "none";
    }
    static const char *const keys[3][3] = {
        // dx = -1, 0, 1 in a row, dy = -1, 0, 1 from the top
        { "7", "Up", "9" },
        { "Left", "none", "Right" },
        { "1", "Down", "3" }
    };
    return keys[dy + 1][dx + 1];
}

int mp_test_count( const Character &who, const itype_id &id )
{
    int total = 0;
    who.visit_items( [&]( item * it, item * ) {
        if( it->typeId() == id ) {
            total += it->count();
        }
        return VisitResponse::NEXT;
    } );
    return total;
}

std::string mp_test_weapon( const Character &who )
{
    const item_location weapon = who.get_wielded_item();
    return weapon ? weapon->tname() : std::string( "none" );
}

bool mp_test_due( fluid::ms_t &start, int delay_ms )
{
    const fluid::ms_t now = fluid::now_ms();
    if( start == 0 ) {
        start = now;
        return false;
    }
    return now - start >= delay_ms;
}

bool mp_test_every( fluid::ms_t &last, int period_ms )
{
    const fluid::ms_t now = fluid::now_ms();
    if( last != 0 && now - last < period_ms ) {
        return false;
    }
    last = now;
    return true;
}

namespace
{
// how many of @p id lie on the floor within 3 tiles of @p centre
int on_floor( const tripoint_bub_ms &centre, const itype_id &id )
{
    map &here = get_map();
    int total = 0;
    for( const tripoint_bub_ms &p : here.points_in_radius( centre, 3 ) ) {
        for( const item &it : here.i_at( p ) ) {
            if( it.typeId() == id ) {
                total += it.count();
            }
        }
    }
    return total;
}
} // namespace

// disarm / steal / useitem: a neutral NPC next to the body, set up with what that action needs.
void mp_test_npc_scene( npc &guy, const std::string &scene )
{
    static fluid::ms_t start = 0;
    static fluid::ms_t last_report = 0;
    static character_id npc_id;
    static const itype_id weapon_id( "bat" );
    static const itype_id loot_id( "hammer" );
    static const itype_id heal_id( "bandages" );

    if( !npc_id.is_valid() ) {
        // give the client time to join and see the world, then set the scene up
        if( !mp_test_due( start, 10000 ) ) {
            return;
        }
        npc *other = mp_test_find_npc( guy, 8 );
        if( other == nullptr ) {
            if( mp_test_every( last_report, 5000 ) ) {
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test " << scene <<
                                           " waiting: no NPC near the body (start with CATA_MP_TEST_NPC=1)";
            }
            return;
        }
        if( !mp_test_put_next_to( guy, *other ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test " << scene << " failed: no free tile next to the body";
            return;
        }
        // a neutral stranger: the menu offers disarm and steal, not trade
        if( other->is_enemy() || other->is_walking_with() ) {
            other->set_attitude( NPCATT_NULL );
        }
        if( scene == "disarm" ) {
            other->set_wielded_item( item( weapon_id ) );
        } else if( scene == "steal" ) {
            // in the pack, not wielded and not worn (those can not be stolen)
            other->set_wielded_item( item() );
            other->i_add( item( loot_id ) );
            other->i_add( item( heal_id ) );
        } else {
            // a wound to heal; the body carries what heals it
            other->mod_part_hp_cur( bodypart_id( "torso" ), -20 );
            guy.i_add( item( heal_id ) );
            guy.i_add( item( heal_id ) );
        }
        npc_id = other->getID();
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test " << scene << " ready npc " << other->get_name() <<
                                   " dir=" << mp_test_dir_key( guy.pos_bub(), other->pos_bub() ) <<
                                   " npc_weapon=" << mp_test_weapon( *other ) <<
                                   " npc_armed=" << other->is_armed() <<
                                   " npc_enemy=" << other->is_enemy() <<
                                   " npc_ally=" << other->is_player_ally() <<
                                   " body_weapon=" << mp_test_weapon( guy );
        return;
    }
    npc *other = g->find_npc( npc_id );
    if( other == nullptr || other->is_dead() ) {
        if( mp_test_every( last_report, 5000 ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test " << scene << " state: the NPC is gone";
        }
        return;
    }
    // it stays within reach (it wanders, and the host's NPC AI knows nothing of this test)
    if( !other->is_enemy() && rl_dist( guy.pos_abs(), other->pos_abs() ) > 1 &&
        mp_test_put_next_to( guy, *other ) ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test " << scene << " npc put back dir=" <<
                                   mp_test_dir_key( guy.pos_bub(), other->pos_bub() );
    }
    if( !mp_test_every( last_report, 4000 ) ) {
        return;
    }
    std::string line = "Multiplayer: test " + scene + " state: ";
    line += "dir=" + mp_test_dir_key( guy.pos_bub(), other->pos_bub() );
    line += " npc_weapon=" + mp_test_weapon( *other );
    line += " body_weapon=" + mp_test_weapon( guy );
    line += " npc_enemy=" + std::to_string( other->is_enemy() );
    if( scene == "disarm" ) {
        line += " bats npc=" + std::to_string( mp_test_count( *other, weapon_id ) ) +
                " body=" + std::to_string( mp_test_count( guy, weapon_id ) ) +
                " floor=" + std::to_string( on_floor( other->pos_bub(), weapon_id ) );
    } else if( scene == "steal" ) {
        line += " hammers npc=" + std::to_string( mp_test_count( *other, loot_id ) ) +
                " body=" + std::to_string( mp_test_count( guy, loot_id ) ) +
                " bandages npc=" + std::to_string( mp_test_count( *other, heal_id ) ) +
                " body=" + std::to_string( mp_test_count( guy, heal_id ) );
    } else {
        const bodypart_id torso( "torso" );
        line += " bandages body=" + std::to_string( mp_test_count( guy, heal_id ) ) +
                " npc torso hp=" + std::to_string( other->get_part_hp_cur( torso ) ) + "/" +
                std::to_string( other->get_part_hp_max( torso ) );
    }
    DebugLog( D_INFO, D_MAIN ) << line;
}

} // namespace net
