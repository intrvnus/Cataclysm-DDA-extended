#include "mp_scores.h"

#include <fstream>
#include <vector>

#include "achievement.h"
#include "messages.h"
#include "mtype.h"
#include "type_id.h"
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

// Per-player achievements: the game's own ones count for the host's character only (its
// statistics are tied to the avatar); these are the matching ones, earned by every player from
// the counters above and kept on the character ("mp_ach_<id>").
struct milestone {
    const char *id;
    const char *counter;
    int target;
};
const std::vector<milestone> &milestones()
{
    static const std::vector<milestone> m = {
        { "achievement_kill_zombie", "mp_zombie_kills", 1 },
        { "achievement_kill_10_monsters", "mp_kills", 10 },
        { "achievement_kill_100_monsters", "mp_kills", 100 },
        { "achievement_kill_1000_monsters", "mp_kills", 1000 },
        { "achievement_kill_10000_monsters", "mp_kills", 10000 },
        { "achievement_kill_100000_monsters", "mp_kills", 100000 },
        { "achievement_survive_one_day", "mp_days", 1 },
        { "achievement_survive_7_days", "mp_days", 7 },
        { "achievement_survive_28_days", "mp_days", 28 },
        { "achievement_survive_91_days", "mp_days", 91 },
        { "achievement_survive_365_days", "mp_days", 365 },
    };
    return m;
}

int achievements_of( const Character &who )
{
    int n = 0;
    for( const milestone &m : milestones() ) {
        n += who.get_value( std::string( "mp_ach_" ) + m.id ).str() == "1" ? 1 : 0;
    }
    return n;
}

void check_milestones( Character &who )
{
    for( const milestone &m : milestones() ) {
        const std::string key = std::string( "mp_ach_" ) + m.id;
        if( who.get_value( key ).str() == "1" || get_int( who, m.counter ) < m.target ) {
            continue;
        }
        const achievement_id aid( m.id );
        if( !aid.is_valid() ) {
            continue;
        }
        who.set_value( key, "1" );
        const std::string name = aid->name().translated();
        net::tell_player( who, string_format( _( "Achievement: %s" ), name ), false );
        add_msg( m_good, _( "%1$s earned the achievement %2$s." ), who.get_name(), name );
    }
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
                        static const species_id species_ZOMBIE( "ZOMBIE" );
                        if( e.get<mtype_id>( "victim_type" ).obj().in_species( species_ZOMBIE ) ) {
                            add( *who, "mp_zombie_kills", 1 );
                        }
                        check_milestones( *who );
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
    return string_format(
               _( "%1$s: %2$d monsters, %3$d people, %4$d skill levels, %5$d lives lost, %6$d days, %7$d achievements" ),
               who.get_name(), get_int( who, "mp_kills" ), get_int( who, "mp_npc_kills" ),
               get_int( who, "mp_levels" ), get_int( who, "mp_deaths" ), get_int( who, "mp_days" ),
               achievements_of( who ) );
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

void tick()
{
    if( !net::is_host() || !calendar::once_every( 1_hours ) ) {
        return;
    }
    std::vector<Character *> players;
    if( !net::dedicated() ) {
        players.push_back( &get_avatar() );
    }
    for( npc *guy : net::remote_player_npcs() ) {
        players.push_back( guy );
    }
    for( Character *who : players ) {
        // days survived since the character came into the world (this character, not the world)
        if( who->get_value( "mp_since" ).str().empty() ) {
            who->set_value( "mp_since", std::to_string( to_turns<int>( calendar::turn - calendar::turn_zero ) ) );
        }
        const int since = get_int( *who, "mp_since" );
        const int now = to_turns<int>( calendar::turn - calendar::turn_zero );
        who->set_value( "mp_days", std::to_string( std::max( 0, now - since ) / to_turns<int>( 1_days ) ) );
        check_milestones( *who );
    }
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
