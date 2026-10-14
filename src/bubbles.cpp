#include "bubbles.h"

#include <map>
#include <memory>

#include "current_map.h"
#include "debug.h"
#include "game.h"
#include "map.h"
#include "scent_map.h"

namespace
{
std::vector<std::unique_ptr<map>> &storage()
{
    static std::vector<std::unique_ptr<map>> s;
    return s;
}

// the scent grid of each extra bubble
std::map<const map *, std::unique_ptr<scent_map>> &scents()
{
    static std::map<const map *, std::unique_ptr<scent_map>> s;
    return s;
}

std::vector<map *> &list()
{
    static std::vector<map *> l;
    return l;
}
} // namespace

namespace bubbles
{
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
    scents()[&ref] = std::make_unique<scent_map>( *g );
    scents()[&ref]->reset();
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

scent_map &scent_of( const map &m )
{
    if( &m == &reality_bubble() ) {
        return g->scent;
    }
    const auto it = scents().find( &m );
    if( it == scents().end() ) {
        // not a bubble we know (e.g. a map used for mapgen): fall back to the main grid
        return g->scent;
    }
    return *it->second;
}

void shift_scent( const map &m, const point_rel_ms &shift )
{
    scent_of( m ).shift( shift );
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
    list().clear();
    scents().clear();
    storage().clear();
}
} // namespace bubbles
