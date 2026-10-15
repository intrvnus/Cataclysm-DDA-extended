#include "mp_groups.h"

#include "mp_scores.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "avatar.h"
#include "cata_path.h"
#include "filesystem.h"
#include "json.h"
#include "json_loader.h"
#include "map_scale_constants.h"
#include "path_info.h"
#include "character.h"
#include "messages.h"
#include "skill.h"
#include "item_location.h"
#include "item.h"
#include "calendar.h"
#include "faction.h"
#include "game.h"
#include "net_session.h"
#include "npc.h"
#include "trap.h"
#include "map.h"
#include "omdata.h"
#include "options.h"
#include "overmapbuffer.h"
#include "string_formatter.h"
#include "translations.h"

namespace mp_groups
{

namespace
{
// open invitations: character id -> group name
std::map<int, std::string> &invites()
{
    static std::map<int, std::string> m;
    return m;
}

const faction_id faction_shared( "your_followers" );

// overmap tiles known to each group other than the shared one
std::map<std::string, std::set<tripoint_abs_omt>> &group_maps()
{
    static std::map<std::string, std::set<tripoint_abs_omt>> m;
    return m;
}

// overmap notes of each group other than the shared one
std::map<std::string, std::map<tripoint_abs_omt, std::string>> &group_notes()
{
    static std::map<std::string, std::map<tripoint_abs_omt, std::string>> m;
    return m;
}

// the knowledge of group @p from is added to group @p to
void copy_knowledge( const std::string &from, const std::string &to )
{
    if( from == to ) {
        return;
    }
    if( !from.empty() ) {
        const auto notes = group_notes().find( from );
        if( notes != group_notes().end() ) {
            for( const auto &n : notes->second ) {
                if( to.empty() ) {
                    if( !overmap_buffer.has_note( n.first ) ) {
                        overmap_buffer.add_note( n.first, n.second );
                    }
                } else {
                    group_notes()[to].emplace( n.first, n.second );
                }
            }
        }
    }
    if( from.empty() ) {
        // the shared group's map is the world's overmap: too big to copy; the new group starts
        // with what the player saw since (the world's map stays readable in the shared group)
        return;
    }
    const auto iter = group_maps().find( from );
    if( iter == group_maps().end() ) {
        return;
    }
    if( to.empty() ) {
        for( const tripoint_abs_omt &p : iter->second ) {
            if( overmap_buffer.seen( p ) < om_vision_level::details ) {
                overmap_buffer.set_seen( p, om_vision_level::details );
            }
        }
    } else {
        std::set<tripoint_abs_omt> &dest = group_maps()[to];
        dest.insert( iter->second.begin(), iter->second.end() );
    }
}

// The faction of a group: the shared group is the player faction; every other group gets a copy
// of it (made once, kept with the world), so monsters and NPCs see its members as players, while
// ownership (and with it theft) follows the group.
faction_id faction_for( const std::string &group )
{
    if( group.empty() ) {
        return faction_shared;
    }
    std::string id = "mp_grp_";
    for( const char c : group ) {
        id += std::isalnum( static_cast<unsigned char>( c ) ) ? static_cast<char>( std::tolower(
                    static_cast<unsigned char>( c ) ) ) : '_';
    }
    const faction_id fid( id );
    if( g->faction_manager_ptr->get( fid, false ) == nullptr ) {
        g->faction_manager_ptr->add_new_faction( group, fid, faction_shared );
    }
    return fid;
}

// the faction a group got when it was founded: kept on its members ("mp_group_fac"), so a
// rename keeps what the group owns
faction_id group_faction( const Character &who )
{
    const std::string group = group_of( who );
    if( group.empty() ) {
        return faction_shared;
    }
    const std::string stored = who.get_value( "mp_group_fac" ).str();
    if( !stored.empty() ) {
        return faction_id( stored );
    }
    return faction_for( group );
}

// a client's body takes the faction of its group (the host's own character keeps the player
// faction: the engine ties the avatar to it)
void apply_faction( Character &who )
{
    const faction_id fid = group_faction( who );
    if( group_of( who ).empty() ) {
        who.remove_value( "mp_group_fac" );
    } else {
        who.set_value( "mp_group_fac", fid.str() );
    }
    if( npc *guy = dynamic_cast<npc *>( &who ) ) {
        guy->set_fac( fid );
    }
}

bool is_leader( const Character &who )
{
    return who.get_value( "mp_group_leader" ).str() == "1";
}

std::vector<Character *> players();

void set_group( Character &who, const std::string &group, bool leader )
{
    copy_knowledge( who.get_value( "mp_group" ).str(), group );
    who.set_value( "mp_group", group );
    who.remove_value( "mp_group_fac" );
    for( const Character *c : players() ) {
        // joining a group that exists: its faction (founding a new one: made from the name)
        if( c != &who && !group.empty() && group_of( *c ) == group &&
            !c->get_value( "mp_group_fac" ).str().empty() ) {
            who.set_value( "mp_group_fac", c->get_value( "mp_group_fac" ).str() );
            break;
        }
    }
    who.set_value( "mp_group_leader", leader ? "1" : "0" );
    apply_faction( who );
}

// every player in the world: the host's character (not the dedicated server's anchor) and the
// bodies of the clients
std::vector<Character *> players()
{
    std::vector<Character *> out;
    if( !net::dedicated() ) {
        out.push_back( &get_avatar() );
    }
    for( npc *guy : net::remote_player_npcs() ) {
        out.push_back( guy );
    }
    return out;
}

Character *player_named( const std::string &name )
{
    for( Character *c : players() ) {
        std::string a = c->get_name();
        std::string b = name;
        std::transform( a.begin(), a.end(), a.begin(), []( unsigned char ch ) {
            return std::tolower( ch );
        } );
        std::transform( b.begin(), b.end(), b.begin(), []( unsigned char ch ) {
            return std::tolower( ch );
        } );
        if( a == b ) {
            return c;
        }
    }
    return nullptr;
}

std::string shown_name( const std::string &group )
{
    return group.empty() ? std::string( _( "everybody" ) ) : group;
}

bool group_exists( const std::string &group )
{
    for( const Character *c : players() ) {
        if( group_of( *c ) == group ) {
            return true;
        }
    }
    return false;
}

std::string list_groups()
{
    std::map<std::string, std::vector<std::string>> groups;
    for( const Character *c : players() ) {
        groups[group_of( *c )].push_back( c->get_name() + ( is_leader( *c ) ? "*" : "" ) );
    }
    std::string text = _( "Groups (* = leader):" );
    for( const auto &g : groups ) {
        text += "\n" + shown_name( g.first ) + ": ";
        for( size_t i = 0; i < g.second.size(); ++i ) {
            text += ( i > 0 ? ", " : "" ) + g.second[i];
        }
    }
    return text;
}
} // namespace

namespace
{
std::map<int, std::string> &mission_groups()
{
    static std::map<int, std::string> m;
    return m;
}

cata_path mission_file()
{
    return PATH_INFO::world_base_save_path() / "mp_players" / "mission_groups.json";
}

cata_path map_file()
{
    return PATH_INFO::world_base_save_path() / "mp_players" / "group_maps.json";
}

cata_path notes_file()
{
    return PATH_INFO::world_base_save_path() / "mp_players" / "group_notes.json";
}
} // namespace

std::vector<std::pair<tripoint_abs_omt, std::string>> notes_of( const Character &who )
{
    std::vector<std::pair<tripoint_abs_omt, std::string>> out;
    const std::string group = group_of( who );
    if( group.empty() ) {
        for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
            for( const auto &n : overmap_buffer.get_all_notes( z ) ) {
                out.emplace_back( tripoint_abs_omt( n.first, z ), n.second );
            }
        }
        return out;
    }
    const auto iter = group_notes().find( group );
    if( iter != group_notes().end() ) {
        out.assign( iter->second.begin(), iter->second.end() );
    }
    return out;
}

void set_note( const Character &who, const tripoint_abs_omt &p, const std::string &text )
{
    const std::string group = group_of( who );
    if( group.empty() ) {
        overmap_buffer.add_note( p, text );
    } else {
        group_notes()[group][p] = text;
    }
}

void delete_note( const Character &who, const tripoint_abs_omt &p )
{
    const std::string group = group_of( who );
    if( group.empty() ) {
        overmap_buffer.delete_note( p );
    } else {
        group_notes()[group].erase( p );
    }
}

void reveal( const Character &who, const tripoint_abs_omt &p )
{
    const std::string group = group_of( who );
    if( group.empty() ) {
        if( overmap_buffer.seen( p ) < om_vision_level::details ) {
            overmap_buffer.set_seen( p, om_vision_level::details );
        }
    } else {
        group_maps()[group].insert( p );
    }
}

opinion_scope::opinion_scope( const Character &you, npc &other ) : other( other ),
    group( net::is_host() ? group_of( you ) : std::string() )
{
    if( group.empty() ) {
        return;
    }
    stash = other.op_of_u;
    npc_opinion op;
    std::istringstream is( other.get_value( "mp_op_" + group ).str() );
    is >> op.trust >> op.fear >> op.value >> op.anger >> op.owed >> op.sold;
    other.op_of_u = is.fail() ? npc_opinion() : op;
}

opinion_scope::~opinion_scope()
{
    if( group.empty() ) {
        return;
    }
    const npc_opinion &op = other.op_of_u;
    other.set_value( "mp_op_" + group, string_format( "%d %d %d %d %d %d", op.trust, op.fear,
                     op.value, op.anger, op.owed, op.sold ) );
    other.op_of_u = stash;
}

om_vision_level seen_by( const Character &who, const tripoint_abs_omt &p )
{
    const std::string group = group_of( who );
    if( group.empty() ) {
        return overmap_buffer.seen( p );
    }
    const auto iter = group_maps().find( group );
    return iter != group_maps().end() && iter->second.count( p ) > 0 ? om_vision_level::details :
           om_vision_level::unseen;
}

namespace
{
std::map<int, std::string> &mission_takers()
{
    static std::map<int, std::string> m;
    return m;
}
} // namespace

void note_mission_taker( int mission_id, const std::string &name )
{
    mission_takers()[mission_id] = name;
}

std::string mission_taker( int mission_id )
{
    const auto iter = mission_takers().find( mission_id );
    return iter == mission_takers().end() ? std::string() : iter->second;
}

void note_mission( int mission_id, const std::string &group )
{
    mission_groups()[mission_id] = group;
}

std::string mission_group( int mission_id )
{
    const auto iter = mission_groups().find( mission_id );
    return iter == mission_groups().end() ? std::string() : iter->second;
}

void load()
{
    group_maps().clear();
    if( file_exist( map_file() ) ) {
        read_from_file_optional_json( map_file(), []( const JsonValue & jv ) {
            for( const JsonMember m : jv.get_object() ) {
                std::set<tripoint_abs_omt> &known = group_maps()[m.name()];
                for( JsonArray a : m.get_array() ) {
                    const int x = a.next_int();
                    const int y = a.next_int();
                    const int z = a.next_int();
                    known.emplace( x, y, z );
                }
            }
        } );
    }
    group_notes().clear();
    if( file_exist( notes_file() ) ) {
        read_from_file_optional_json( notes_file(), []( const JsonValue & jv ) {
            for( const JsonMember m : jv.get_object() ) {
                std::map<tripoint_abs_omt, std::string> &notes = group_notes()[m.name()];
                for( JsonArray a : m.get_array() ) {
                    const int x = a.next_int();
                    const int y = a.next_int();
                    const int z = a.next_int();
                    notes[tripoint_abs_omt( x, y, z )] = a.next_string();
                }
            }
        } );
    }
    mission_groups().clear();
    mission_takers().clear();
    if( !file_exist( mission_file() ) ) {
        return;
    }
    read_from_file_optional_json( mission_file(), []( const JsonValue & jv ) {
        for( const JsonMember m : jv.get_object() ) {
            const int id = std::atoi( m.name().c_str() );
            if( m.test_string() ) {
                mission_groups()[id] = m.get_string();
            } else {
                // {"g": group, "by": who accepted it}
                JsonObject o = m.get_object();
                mission_groups()[id] = o.get_string( "g", "" );
                mission_takers()[id] = o.get_string( "by", "" );
            }
        }
    } );
}

void save()
{
    assure_dir_exist( PATH_INFO::world_base_save_path() / "mp_players" );
    write_to_file( mission_file(), []( std::ostream & fout ) {
        JsonOut jo( fout );
        jo.start_object();
        for( const auto &e : mission_groups() ) {
            jo.member( std::to_string( e.first ) );
            jo.start_object();
            jo.member( "g", e.second );
            jo.member( "by", mission_taker( e.first ) );
            jo.end_object();
        }
        jo.end_object();
    }, _( "multiplayer missions" ) );
    write_to_file( map_file(), []( std::ostream & fout ) {
        JsonOut jo( fout );
        jo.start_object();
        for( const auto &e : group_maps() ) {
            jo.member( e.first );
            jo.start_array();
            for( const tripoint_abs_omt &p : e.second ) {
                jo.start_array();
                jo.write( p.x() );
                jo.write( p.y() );
                jo.write( p.z() );
                jo.end_array();
            }
            jo.end_array();
        }
        jo.end_object();
    }, _( "multiplayer group maps" ) );
    write_to_file( notes_file(), []( std::ostream & fout ) {
        JsonOut jo( fout );
        jo.start_object();
        for( const auto &e : group_notes() ) {
            jo.member( e.first );
            jo.start_array();
            for( const auto &n : e.second ) {
                jo.start_array();
                jo.write( n.first.x() );
                jo.write( n.first.y() );
                jo.write( n.first.z() );
                jo.write( n.second );
                jo.end_array();
            }
            jo.end_array();
        }
        jo.end_object();
    }, _( "multiplayer group notes" ) );
}

void share_trap( const Character &who, const tripoint_bub_ms &pos )
{
    if( !net::is_host() ) {
        return;
    }
    const trap &tr = get_map().tr_at( pos );
    if( tr.is_null() ) {
        return;
    }
    for( Character *c : players() ) {
        if( c != &who && same_group( *c, who ) ) {
            c->add_known_trap( pos, tr );
        }
    }
}

namespace
{
struct spar_request {
    int partner = 0;
    time_point until;
};

std::map<int, spar_request> &spar_requests()
{
    static std::map<int, spar_request> m;
    return m;
}

bool wants_to_spar( int who, int partner )
{
    const auto iter = spar_requests().find( who );
    return iter != spar_requests().end() && iter->second.partner == partner &&
           calendar::turn <= iter->second.until;
}
} // namespace

bool sparring( const Creature &a, const Creature &b )
{
    if( &a == &b || !is_player( a ) || !is_player( b ) ) {
        return false;
    }
    const int ia = a.as_character()->getID().get_value();
    const int ib = b.as_character()->getID().get_value();
    return wants_to_spar( ia, ib ) && wants_to_spar( ib, ia ) &&
           same_group( *a.as_character(), *b.as_character() );
}

void spar_hit( Character &attacker, Character &partner )
{
    static const skill_id skill_dodge( "dodge" );
    item_location weapon = attacker.used_weapon();
    const skill_id used = weapon ? weapon->melee_skill() : skill_id( "unarmed" );
    attacker.practice( used, 4 );
    attacker.practice( skill_id( "melee" ), 2 );
    partner.practice( skill_dodge, 3 );
    attacker.mod_stamina( -100 );
    attacker.add_msg_if_player( m_good, _( "You spar with %s." ), partner.get_name() );
    partner.add_msg_if_player( m_info, _( "%s lands a sparring blow." ), attacker.get_name() );
}

bool is_player( const Creature &who )
{
    const Character *ch = who.as_character();
    if( ch == nullptr || !net::is_host() ) {
        return false;
    }
    return ( ch->is_avatar() && !net::dedicated() ) || net::is_remote_player( *ch );
}

bool protected_from( const Creature &attacker, const Creature &victim )
{
    if( &attacker == &victim || !is_player( attacker ) || !is_player( victim ) ) {
        return false;
    }
    const bool same = same_group( *attacker.as_character(), *victim.as_character() );
    return same ? !get_option<bool>( "MP_FRIENDLY_FIRE" ) : !get_option<bool>( "MP_PVP" );
}

bool pvp_target( const Character &attacker, const Creature &victim )
{
    return &attacker != &victim && is_player( attacker ) && is_player( victim ) &&
           !victim.is_dead_state() && get_option<bool>( "MP_PVP" ) &&
           !same_group( attacker, *victim.as_character() );
}

std::string group_of( const Character &who )
{
    return who.get_value( "mp_group" ).str();
}

bool same_group( const Character &a, const Character &b )
{
    return group_of( a ) == group_of( b );
}

void on_join( Character &who )
{
    if( who.get_value( "mp_group_set" ).str() != "1" && who.get_value( "mp_group" ).str().empty() ) {
        who.set_value( "mp_group_set", "1" );
        if( get_option<std::string>( "MP_GROUPING" ) == "solo" ) {
            set_group( who, who.get_name(), true );
        }
    }
    // a saved body comes back in the faction of its group
    apply_faction( who );
}

std::string command( Character &who, const std::string &args )
{
    std::istringstream is( args );
    std::string op;
    is >> op;
    std::string rest;
    std::getline( is, rest );
    rest.erase( 0, rest.find_first_not_of( ' ' ) );
    const std::string mine = group_of( who );
    if( op.empty() || op == "list" ) {
        return list_groups();
    }
    if( op == "stats" ) {
        return mp_scores::scoreboard();
    }
    if( op == "spar" ) {
        Character *other = player_named( rest );
        if( other == nullptr || other == &who ) {
            return _( "Usage: /spar <player> (both of you, same group): melee trains instead of hurting for 10 minutes." );
        }
        if( !same_group( who, *other ) ) {
            return string_format( _( "%s is not in your group." ), other->get_name() );
        }
        spar_request &req = spar_requests()[who.getID().get_value()];
        req.partner = other->getID().get_value();
        req.until = calendar::turn + 10_minutes;
        if( wants_to_spar( other->getID().get_value(), who.getID().get_value() ) ) {
            net::tell_player( *other, string_format( _( "You spar with %s now (10 minutes)." ), who.get_name() ),
                              true );
            return string_format( _( "You spar with %s now (10 minutes)." ), other->get_name() );
        }
        net::tell_player( *other, string_format( _( "%s wants to spar.  Type /spar %s to agree." ),
                          who.get_name(), who.get_name() ), true );
        return string_format( _( "You asked %s to spar." ), other->get_name() );
    }
    if( op == "create" ) {
        if( rest.empty() || rest.size() > 30 ) {
            return _( "Usage: /group create <name> (up to 30 letters)" );
        }
        if( group_exists( rest ) ) {
            return string_format( _( "There is already a group called %s." ), rest );
        }
        set_group( who, rest, true );
        return string_format( _( "You founded the group %s." ), rest );
    }
    if( op == "invite" ) {
        if( !is_leader( who ) ) {
            return _( "Only the leader of a group can invite." );
        }
        Character *other = player_named( rest );
        if( other == nullptr ) {
            return string_format( _( "Nobody called %s is here." ), rest );
        }
        invites()[other->getID().get_value()] = mine;
        net::tell_player( *other, string_format( _( "%1$s invites you to the group %2$s.  Type /group accept to join." ),
                          who.get_name(), shown_name( mine ) ), true );
        return string_format( _( "You invited %s." ), other->get_name() );
    }
    if( op == "accept" ) {
        const auto iter = invites().find( who.getID().get_value() );
        if( iter == invites().end() ) {
            return _( "Nobody invited you." );
        }
        const std::string group = iter->second;
        invites().erase( iter );
        set_group( who, group, false );
        return string_format( _( "You joined the group %s." ), shown_name( group ) );
    }
    if( op == "leave" ) {
        if( mine.empty() ) {
            return _( "You are in the group everybody starts in.  Found your own with /group create <name>." );
        }
        const bool was_leader = is_leader( who );
        set_group( who, std::string(), false );
        if( was_leader ) {
            // the next member leads (the group is archived, not destroyed, when nobody is left)
            for( Character *c : players() ) {
                if( group_of( *c ) == mine ) {
                    c->set_value( "mp_group_leader", "1" );
                    net::tell_player( *c, string_format( _( "You lead the group %s now." ), mine ), true );
                    break;
                }
            }
        }
        return string_format( _( "You left the group %s." ), mine );
    }
    if( op == "kick" ) {
        if( !is_leader( who ) ) {
            return _( "Only the leader of a group can send somebody away." );
        }
        Character *other = player_named( rest );
        if( other == nullptr || other == &who || group_of( *other ) != mine ) {
            return string_format( _( "%s is not in your group." ), rest );
        }
        set_group( *other, std::string(), false );
        net::tell_player( *other, string_format( _( "You were sent away from the group %s." ), mine ), false );
        return string_format( _( "%1$s is no longer in the group %2$s." ), other->get_name(), mine );
    }
    if( op == "rename" ) {
        if( !is_leader( who ) || mine.empty() ) {
            return _( "Only the leader of a group can rename it." );
        }
        if( rest.empty() || rest.size() > 30 || group_exists( rest ) ) {
            return _( "Usage: /group rename <new name> (unique, up to 30 letters)" );
        }
        for( Character *c : players() ) {
            if( group_of( *c ) == mine ) {
                c->set_value( "mp_group", rest );
                apply_faction( *c );
            }
        }
        const auto map_iter = group_maps().find( mine );
        if( map_iter != group_maps().end() ) {
            group_maps()[rest] = std::move( map_iter->second );
            group_maps().erase( mine );
        }
        const auto notes_iter = group_notes().find( mine );
        if( notes_iter != group_notes().end() ) {
            group_notes()[rest] = std::move( notes_iter->second );
            group_notes().erase( mine );
        }
        return string_format( _( "The group is called %s now." ), rest );
    }
    return _( "Group commands: /group, /group create <name>, /group invite <player>, /group accept, /group leave, /group kick <player>, /group rename <name>" );
}

} // namespace mp_groups
