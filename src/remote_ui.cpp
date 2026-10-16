#include "debug.h"
#include "remote_ui.h"

#include <cstddef>
#include <exception>
#include <sstream>

#include "flexbuffer_json.h"
#include "json.h"
#include "avatar.h"
#include "character.h"
#include "item_location.h"
#include "json_loader.h"

namespace remote_ui
{
namespace
{

struct state {
    bool recording = false;
    bool replaying = false;
    bool failed = false;
    bool capture = false;
    std::vector<std::string> activities;
    int muted = 0;
    std::vector<entry> answers;
    size_t next = 0;
    std::vector<std::string> texts;
    Character *actor = nullptr;
    int client_id = -1;
};

state &st()
{
    static state s;
    return s;
}

} // namespace

bool recording()
{
    return st().recording;
}

bool replaying()
{
    return st().replaying;
}

record_scope::record_scope( bool capture_activities )
{
    st() = state();
    st().recording = true;
    st().capture = capture_activities;
}

record_scope::~record_scope()
{
    st().recording = false;
    st().capture = false;
}

std::vector<std::string> record_scope::activities() const
{
    return st().activities;
}

bool capturing()
{
    return st().recording && st().capture;
}

void capture_activity( const std::string &json )
{
    if( capturing() ) {
        st().activities.push_back( json );
    }
}

std::string remap_character_id( std::string json, int from_id, int to_id )
{
    const std::string from = "\"character\":" + std::to_string( from_id );
    const std::string to = "\"character\":" + std::to_string( to_id );
    for( size_t at = json.find( from ); at != std::string::npos; ) {
        const size_t end = at + from.size();
        // "character":12 is not "character":1
        if( end < json.size() && json[end] >= '0' && json[end] <= '9' ) {
            at = json.find( from, end );
            continue;
        }
        json.replace( at, from.size(), to );
        at = json.find( from, at + to.size() );
    }
    return json;
}

std::vector<entry> record_scope::answers() const
{
    return st().answers;
}

mute_scope::mute_scope()
{
    ++st().muted;
}

mute_scope::~mute_scope()
{
    --st().muted;
}

replay_scope::replay_scope( std::vector<entry> answers, Character &actor, int client_id )
{
    st() = state();
    st().replaying = true;
    st().answers = std::move( answers );
    st().actor = &actor;
    st().client_id = client_id;
}

replay_scope::~replay_scope()
{
    st().replaying = false;
    st().actor = nullptr;
}

Character *actor()
{
    return st().replaying ? st().actor : nullptr;
}

Character &acting_player()
{
    Character *who = actor();
    return who != nullptr ? *who : get_player_character();
}

bool acts_as_player( const Character &who )
{
    return who.is_avatar() || ( st().replaying && &who == st().actor );
}

bool replay_scope::failed() const
{
    return st().failed;
}

std::vector<std::string> replay_scope::texts() const
{
    return st().texts;
}

void record( const std::string &kind, const std::string &value )
{
    if( st().recording && st().muted == 0 ) {
        st().answers.emplace_back( kind, value );
    }
}

bool next_is( const std::string &kind )
{
    const state &s = st();
    return s.replaying && !s.failed && s.next < s.answers.size() && s.answers[s.next].first == kind;
}

std::optional<std::string> answer( const std::string &kind )
{
    state &s = st();
    if( !s.replaying || s.failed ) {
        return std::nullopt;
    }
    // a popup the client had to close but the host does not show is skipped
    while( kind != "q" && s.next < s.answers.size() && s.answers[s.next].first == "q" ) {
        ++s.next;
    }
    if( s.next >= s.answers.size() || s.answers[s.next].first != kind ) {
        // asked something the player was not asked: give up here
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer: replay wanted answer '" << kind << "', next is '" <<
                                   ( s.next < s.answers.size() ? s.answers[s.next].first : std::string( "(none)" ) ) <<
                                   "' (" << s.next << " of " << s.answers.size() << ")";
        s.failed = true;
        return std::nullopt;
    }
    return s.answers[s.next++].second;
}

void tell( const std::string &text )
{
    if( st().replaying && !text.empty() ) {
        st().texts.push_back( text );
    }
}

std::string location_to_string( const item_location &loc )
{
    if( !loc ) {
        return std::string();
    }
    std::ostringstream os;
    JsonOut jo( os );
    loc.serialize( jo );
    return os.str();
}

item_location location_from_string( const std::string &text )
{
    item_location loc;
    if( text.empty() ) {
        return loc;
    }
    std::string json = text;
    const state &s = st();
    if( s.replaying && s.actor != nullptr && s.client_id >= 0 ) {
        // what the client carries, the body carries
        json = remap_character_id( json, s.client_id, s.actor->getID().get_value() );
    }
    try {
        JsonValue jv = json_loader::from_string( json );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        loc.deserialize( jo );
    } catch( const std::exception & ) {
        return item_location();
    }
    if( loc.get_item() == nullptr ) {
        return item_location();
    }
    return loc;
}

std::string locations_to_string( const std::list<std::pair<item_location, int>> &locs )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_array();
    for( const std::pair<item_location, int> &l : locs ) {
        jo.start_array();
        jo.write( l.second );
        jo.write( location_to_string( l.first ) );
        jo.end_array();
    }
    jo.end_array();
    return os.str();
}

std::list<std::pair<item_location, int>> locations_from_string( const std::string &text )
{
    std::list<std::pair<item_location, int>> result;
    if( text.empty() ) {
        return result;
    }
    try {
        JsonValue jv = json_loader::from_string( text );
        for( JsonArray e : jv.get_array() ) {
            const int count = e.next_int();
            item_location loc = location_from_string( e.next_string() );
            if( loc ) {
                result.emplace_back( loc, count );
            }
        }
    } catch( const std::exception & ) {
        result.clear();
    }
    return result;
}

namespace
{
std::optional<trade_request> &pending_trade()
{
    static std::optional<trade_request> request;
    return request;
}
} // namespace

void request_trade( int npc_id, int cost, const std::string &deal )
{
    trade_request r;
    r.npc_id = npc_id;
    r.cost = cost;
    r.deal = deal;
    pending_trade() = r;
}

std::optional<trade_request> take_trade_request()
{
    std::optional<trade_request> r = pending_trade();
    pending_trade().reset();
    return r;
}

std::string to_json( const std::vector<entry> &answers )
{
    std::ostringstream os;
    JsonOut jo( os );
    jo.start_array();
    for( const entry &e : answers ) {
        jo.start_array();
        jo.write( e.first );
        jo.write( e.second );
        jo.end_array();
    }
    jo.end_array();
    return os.str();
}

std::vector<entry> from_json( const std::string &json )
{
    std::vector<entry> result;
    try {
        JsonValue jv = json_loader::from_string( json );
        for( JsonArray e : jv.get_array() ) {
            const std::string kind = e.next_string();
            const std::string value = e.next_string();
            result.emplace_back( kind, value );
        }
    } catch( const std::exception & ) {
        result.clear();
    }
    return result;
}

} // namespace remote_ui
