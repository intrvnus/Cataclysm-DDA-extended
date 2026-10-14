// Multiplayer test scene "armor": a friendly NPC wearing two layers next to the body, for the
// "Sort armor" entry of the NPC menu.  See mp_test_npc.h.

#include "mp_test_npc.h"

#include <string>
#include <vector>

#include "character.h"
#include "character_id.h"
#include "debug.h"
#include "game.h"
#include "item.h"
#include "line.h"
#include "npc.h"
#include "type_id.h"

static const faction_id faction_your_followers( "your_followers" );

namespace net
{

namespace
{
std::string worn_order_text( const npc &who )
{
    std::string order;
    for( const item *it : who.worn_item_order() ) {
        order += it->tname() + " | ";
    }
    return order;
}
} // namespace

void mp_test_armor( npc &guy )
{
    static fluid::ms_t start = 0;
    static fluid::ms_t last_report = 0;
    static character_id npc_id;

    if( !npc_id.is_valid() ) {
        if( !mp_test_due( start, 10000 ) ) {
            return;
        }
        npc *other = mp_test_find_npc( guy, 8 );
        if( other == nullptr ) {
            if( mp_test_every( last_report, 5000 ) ) {
                DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test armor waiting: no NPC near the body "
                                           "(start with CATA_MP_TEST_NPC=1)";
            }
            return;
        }
        if( !mp_test_put_next_to( guy, *other ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test armor failed: no free tile next to the body";
            return;
        }
        // a friend who stays put: a follower of the host, told to wait
        g->add_npc_follower( other->getID() );
        other->set_fac( faction_your_followers );
        other->set_attitude( NPCATT_WAIT );
        // two layers on the torso: a shirt under a hoodie
        other->clear_worn();
        other->wear_item( item( itype_id( "tshirt" ) ), false );
        other->wear_item( item( itype_id( "hoodie" ) ), false );
        npc_id = other->getID();
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test armor ready npc " << other->get_name() <<
                                   " dir=" << mp_test_dir_key( guy.pos_bub(), other->pos_bub() ) <<
                                   " ally=" << other->is_player_ally() <<
                                   " friendly_to_body=" << other->is_friendly( guy ) <<
                                   " worn_order=" << worn_order_text( *other );
        return;
    }
    npc *other = g->find_npc( npc_id );
    if( other == nullptr || other->is_dead() ) {
        if( mp_test_every( last_report, 5000 ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test armor state: the NPC is gone";
        }
        return;
    }
    if( rl_dist( guy.pos_abs(), other->pos_abs() ) > 1 && mp_test_put_next_to( guy, *other ) ) {
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test armor npc put back dir=" <<
                                   mp_test_dir_key( guy.pos_bub(), other->pos_bub() );
    }
    if( !mp_test_every( last_report, 4000 ) ) {
        return;
    }
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: test npc worn order: " << worn_order_text( *other );
}

} // namespace net
