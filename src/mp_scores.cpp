#include "mp_scores.h"

#include <fstream>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_path.h"
#include "character.h"
#include "debug.h"
#include "event.h"
#include "event_bus.h"
#include "event_subscriber.h"
#include "filesystem.h"
#include "game.h"
#include "mp_groups.h"
#include "net_session.h"
#include "npc.h"
#include "path_info.h"
#include "string_formatter.h"
#include "translations.h"

namespace mp_scores
{

namespace
{
int get_int( const Character &who, const std::string &key )
{
    const std::string v = who.get_value( key ).str();
    return v.empty() ? 0 : std::atoi( v.c_str() );
}

void add( Character &who, const std::string &key, int amount )
{
    who.set_value( key, std::to_string( get_int( who, key ) + amount ) );
}

Character *player_by_id( const character_id &id )
{
    Character *who = nullptr;
    if( get_avatar().getID() == id ) {
        who = &get_avatar();
    } else {
        who = g->find_npc( id );
    }
    return who != nullptr && mp_groups::is_player( *who ) ? who : nullptr;
}

class score_listener : public event_subscriber
{
    public:
        using event_subscriber::notify;
        void notify( const cata::event &e ) override {
            switch( e.type() ) {
                case event_type::character_kills_monster:
                    if( Character *who = player_by_id( e.get<character_id>( "killer" ) ) ) {
                        add( *who, "mp_kills", 1 );
                    }
                    break;
                case event_type::character_kills_character:
                    if( Character *who = player_by_id( e.get<character_id>( "killer" ) ) ) {
                        add( *who, "mp_npc_kills", 1 );
                    }
                    break;
                case event_type::gains_skill_level:
                    if( Character *who = player_by_id( e.get<character_id>( "character" ) ) ) {
                        add( *who, "mp_levels", 1 );
                    }
                    break;
                default:
                    break;
            }
        }
};

score_listener &listener()
{
    static score_listener l;
    return l;
}

std::string line_of( const Character &who )
{
    return string_format( _( "%1$s: %2$d monsters, %3$d people, %4$d skill levels, %5$d lives lost" ),
                          who.get_name(), get_int( who, "mp_kills" ), get_int( who, "mp_npc_kills" ),
                          get_int( who, "mp_levels" ), get_int( who, "mp_deaths" ) );
}
} // namespace

void start()
{
    static bool started = false;
    if( started || !net::is_host() ) {
        return;
    }
    started = true;
    get_event_bus().subscribe( &listener() );
}

std::string scoreboard()
{
    std::string text = _( "Scores:" );
    if( !net::dedicated() ) {
        text += "\n" + line_of( get_avatar() );
    }
    for( const npc *guy : net::remote_player_npcs() ) {
        text += "\n" + line_of( *guy );
    }
    return text;
}

void memorial( const Character &who )
{
    const cata_path dir = PATH_INFO::world_base_save_path() / "mp_players";
    assure_dir_exist( dir );
    std::ofstream out( ( dir / "memorial.txt" ).get_unrelative_path(), std::ios::app );
    if( !out ) {
        DebugLog( D_WARNING, D_MAIN ) << "Multiplayer: could not write the memorial";
        return;
    }
    out << to_string( calendar::turn ) << "  " << line_of( who ) << "\n";
}

} // namespace mp_scores
