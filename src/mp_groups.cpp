#include "mp_groups.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <vector>

#include "avatar.h"
#include "cata_path.h"
#include "filesystem.h"
#include "json.h"
#include "json_loader.h"
#include "path_info.h"
#include "character.h"
#include "net_session.h"
#include "npc.h"
#include "options.h"
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

bool is_leader( const Character &who )
{
    return who.get_value( "mp_group_leader" ).str() == "1";
}

void set_group( Character &who, const std::string &group, bool leader )
{
    who.set_value( "mp_group", group );
    who.set_value( "mp_group_leader", leader ? "1" : "0" );
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
} // namespace

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
    mission_groups().clear();
    if( !file_exist( mission_file() ) ) {
        return;
    }
    read_from_file_optional_json( mission_file(), []( const JsonValue & jv ) {
        for( const JsonMember m : jv.get_object() ) {
            mission_groups()[std::atoi( m.name().c_str() )] = m.get_string();
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
            jo.member( std::to_string( e.first ), e.second );
        }
        jo.end_object();
    }, _( "multiplayer missions" ) );
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
    if( !who.get_value( "mp_group" ).str().empty() || who.get_value( "mp_group_set" ).str() == "1" ) {
        return;
    }
    who.set_value( "mp_group_set", "1" );
    if( get_option<std::string>( "MP_GROUPING" ) == "solo" ) {
        set_group( who, who.get_name(), true );
    }
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
            }
        }
        return string_format( _( "The group is called %s now." ), rest );
    }
    return _( "Group commands: /group, /group create <name>, /group invite <player>, /group accept, /group leave, /group kick <player>, /group rename <name>" );
}

} // namespace mp_groups
