#include "bubbles.h"

#include <memory>

#include "current_map.h"
#include "debug.h"
#include "game.h"
#include "map.h"

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

map &owner( const tripoint_abs_ms &p )
{
    if( map *m = extra_containing( p ) ) {
        return *m;
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
    list().clear();
    storage().clear();
}
} // namespace bubbles
