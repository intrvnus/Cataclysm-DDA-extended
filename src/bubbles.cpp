#include "bubbles.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>

#include "current_map.h"
#include "debug.h"
#include "character.h"
#include "game.h"
#include "map.h"
#include "net_session.h"
#include "npc.h"

namespace
{
std::vector<std::unique_ptr<map>> &storage()
{
    static std::vector<std::unique_ptr<map>> s;
    return s;
}

std::vector<map *> &list()
{
    static std::vector<map *> l;
    return l;
}
} // namespace

namespace
{
std::map<int, map *> &by_body()
{
    static std::map<int, map *> b;
    return b;
}

void forget( map *m )
{
    list().erase( std::remove( list().begin(), list().end(), m ), list().end() );
    auto &v = storage();
    v.erase( std::remove_if( v.begin(), v.end(), [m]( const std::unique_ptr<map> &p ) {
        return p.get() == m;
    } ), v.end() );
}
} // namespace

namespace bubbles
{
bool enabled()
{
    static const bool off = std::getenv( "CATA_MP_NO_BUBBLES" ) != nullptr;
    return !off && net::is_host();
}

map &open_for( const tripoint_abs_ms &centre, int body_id )
{
    release( body_id );
    const tripoint_abs_sm c = project_to<coords::sm>( centre );
    const tripoint_abs_sm origin( c.x() - MAPSIZE / 2, c.y() - MAPSIZE / 2, centre.z() );
    auto m = std::make_unique<map>();
    map &ref = *m;
    {
        swap_map swap( ref );
        ref.load( origin, true );
    }
    storage().push_back( std::move( m ) );
    list().push_back( &ref );
    by_body()[body_id] = &ref;
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: bubble for body " << body_id << " at " << origin.to_string();
    return ref;
}

map *bubble_of( const Character &who )
{
    if( list().empty() ) {
        return nullptr;
    }
    const auto it = by_body().find( who.getID().get_value() );
    return it == by_body().end() ? nullptr : it->second;
}

map &map_for( const Character &who )
{
    if( map *m = bubble_of( who ) ) {
        return *m;
    }
    return owner( who.pos_abs() );
}

void release( int body_id )
{
    const auto it = by_body().find( body_id );
    if( it == by_body().end() ) {
        return;
    }
    map *m = it->second;
    by_body().erase( it );
    forget( m );
}

bool in_any( const tripoint_abs_ms &p )
{
    return reality_bubble().inbounds( p ) || in_extra( p );
}

void follow_all()
{
    // copy: following can open and close bubbles
    const std::map<int, map *> bodies = by_body();
    for( const auto &entry : bodies ) {
        npc *guy = g->find_npc( character_id( entry.first ) );
        if( guy == nullptr || guy->is_dead() ) {
            continue;
        }
        g->update_extra_bubble( *entry.second, guy->pos_abs() );
    }
}

view::view( const Character &who )
{
    if( map *m = bubble_of( who ) ) {
        swapped.emplace( *m );
    }
}

map &open( const tripoint_abs_sm &origin )
{
    for( map *m : list() ) {
        if( m->get_abs_sub() == origin ) {
            return *m;
        }
    }
    auto m = std::make_unique<map>();
    map &ref = *m;
    {
        // everything the loading generates or looks up must see this map as the current one
        swap_map swap( ref );
        ref.load( origin, true );
    }
    storage().push_back( std::move( m ) );
    list().push_back( &ref );
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: opened an extra bubble at " << origin.to_string();
    return ref;
}

map *extra_containing( const tripoint_abs_ms &p )
{
    for( map *m : list() ) {
        if( m->inbounds( p ) ) {
            return m;
        }
    }
    return nullptr;
}

bool in_extra( const tripoint_abs_ms &p )
{
    return !list().empty() && extra_containing( p ) != nullptr;
}

bool keeps( const tripoint_abs_sm &p )
{
    for( map *m : list() ) {
        if( m->inbounds( p ) ) {
            return true;
        }
    }
    return false;
}

bool keeps( const tripoint_abs_omt &p )
{
    for( map *m : list() ) {
        if( m->inbounds( p ) ) {
            return true;
        }
    }
    return false;
}

const std::vector<map *> &extras()
{
    return list();
}

std::vector<map *> all()
{
    std::vector<map *> v = { &reality_bubble() };
    v.insert( v.end(), list().begin(), list().end() );
    return v;
}

namespace
{
// squared distance (in submaps) from sm to the centre of m
int centre_distance( const map &m, const tripoint_abs_sm &sm )
{
    const tripoint_abs_sm o = m.get_abs_sub();
    const int dx = 2 * ( sm.x() - o.x() ) - ( m.getmapsize() - 1 );
    const int dy = 2 * ( sm.y() - o.y() ) - ( m.getmapsize() - 1 );
    return dx * dx + dy * dy;
}
} // namespace

bool owns_submap( const map &m, const tripoint_abs_sm &sm )
{
    if( list().empty() ) {
        return true;
    }
    const map *best = nullptr;
    int best_d = 0;
    for( const map *c : all() ) {
        if( !c->inbounds( sm ) ) {
            continue;
        }
        const int d = centre_distance( *c, sm );
        if( best == nullptr || d < best_d ) {
            best = c;
            best_d = d;
        }
    }
    return best == &m;
}

bool owns( const map &m, const tripoint_abs_ms &p )
{
    return owns_submap( m, project_to<coords::sm>( p ) );
}

map &owner( const tripoint_abs_ms &p )
{
    for( map *c : all() ) {
        if( owns( *c, p ) ) {
            return *c;
        }
    }
    return reality_bubble();
}

void tick()
{
    for( map *m : list() ) {
        swap_map swap( *m );
        m->build_floor_caches();
        m->process_falling();
        m->vehmove();
        m->process_fields();
        m->process_items();
        m->build_map_cache( m->get_abs_sub().z(), true );
    }
}

void close_all()
{
    by_body().clear();
    list().clear();
    storage().clear();
}
} // namespace bubbles
