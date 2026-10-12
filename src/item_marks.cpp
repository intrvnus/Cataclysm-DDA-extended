#include "item_marks.h"

#include <algorithm>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "character.h"
#include "flag.h"
#include "item.h"
#include "json.h"
#include "mod_engine.h"
#include "output.h"
#include "popup.h"
#include "translations.h"
#include "visitable.h"

namespace
{
struct mark_rule {
    std::string id;
    flag_id mark;
    std::string option;            // mod option (switch) that turns the rule on / off; empty = always on
    bool broken = false;           // item is broken or completely worn out
    bool armor_only = false;       // item is clothing / armor
    bool empty_container = false;  // item is a container without contents
    std::vector<flag_id> any_flag; // item has at least one of these flags (if not empty)
    std::vector<itype_id> any_id;  // item is one of these (if not empty)
};

std::vector<mark_rule> &rules()
{
    static std::vector<mark_rule> all;
    return all;
}

bool rule_enabled( const mark_rule &r )
{
    if( r.option.empty() || !mod_engine::option_exists( r.option ) ) {
        return true;
    }
    return mod_engine::option_get_number( r.option ) != 0.0;
}

bool rule_matches( const mark_rule &r, const item &it )
{
    if( r.broken && !( it.is_broken() || it.damage() >= it.max_damage() ) ) {
        return false;
    }
    if( r.armor_only && !it.is_armor() ) {
        return false;
    }
    if( r.empty_container && !( it.is_container() && it.empty_container() ) ) {
        return false;
    }
    if( !r.any_flag.empty() ) {
        const bool found = std::any_of( r.any_flag.begin(), r.any_flag.end(), [&it]( const flag_id & f ) {
            return it.has_flag( f );
        } );
        if( !found ) {
            return false;
        }
    }
    if( !r.any_id.empty() ) {
        const bool found = std::find( r.any_id.begin(), r.any_id.end(), it.typeId() ) != r.any_id.end();
        if( !found ) {
            return false;
        }
    }
    // a rule without any condition would mark everything
    return r.broken || r.armor_only || r.empty_container || !r.any_flag.empty() || !r.any_id.empty();
}

std::string auto_var( const mark_rule &r )
{
    return "auto_mark:" + r.id;
}

std::string off_var( const flag_id &mark )
{
    return "mark_off:" + mark.str();
}

/** The player changed a mark by hand: automatic rules must not undo that. */
void note_toggled( item &it, const flag_id &mark, bool turned_on )
{
    if( turned_on ) {
        it.erase_var( off_var( mark ) );
    } else {
        it.set_var( off_var( mark ), "1" );
    }
    for( const mark_rule &r : rules() ) {
        if( r.mark == mark ) {
            it.erase_var( auto_var( r ) );
        }
    }
}
} // namespace

namespace item_marks
{
void load_rule( const JsonObject &jo, const std::string & )
{
    mark_rule r;
    r.id = jo.get_string( "id" );
    if( r.id.empty() ) {
        jo.throw_error_at( "id", "item_mark_rule needs a non-empty id" );
    }
    r.mark = flag_id( jo.get_string( "mark" ) );
    r.option = jo.get_string( "option", "" );
    r.broken = jo.get_bool( "broken", false );
    r.armor_only = jo.get_bool( "armor_only", false );
    r.empty_container = jo.get_bool( "empty_container", false );
    for( const std::string &f : jo.get_string_array( "has_flag" ) ) {
        r.any_flag.emplace_back( f );
    }
    for( const std::string &i : jo.get_string_array( "item_ids" ) ) {
        r.any_id.emplace_back( i );
    }
    std::vector<mark_rule> &all = rules();
    auto it = std::find_if( all.begin(), all.end(), [&r]( const mark_rule & m ) {
        return m.id == r.id;
    } );
    if( it != all.end() ) {
        *it = r;
    } else {
        all.push_back( r );
    }
}

void reset()
{
    rules().clear();
}

void auto_mark_scan( Character &who )
{
    if( rules().empty() ) {
        return;
    }
    // the marks must exist (a mod may have been removed)
    std::vector<const mark_rule *> active;
    for( const mark_rule &r : rules() ) {
        if( r.mark.is_valid() && rule_enabled( r ) ) {
            active.push_back( &r );
        }
    }
    bool changed = false;
    who.visit_items( [&]( item * it, item * ) {
        if( who.is_worn( *it ) || who.is_wielding( *it ) ) {
            return VisitResponse::NEXT;
        }
        for( const mark_rule &r : rules() ) {
            if( !r.mark.is_valid() ) {
                continue;
            }
            const bool on = std::find( active.begin(), active.end(), &r ) != active.end();
            const bool auto_set = it->has_var( auto_var( r ) );
            if( on && rule_matches( r, *it ) ) {
                if( !auto_set && !it->has_own_flag( r.mark ) && !it->has_var( off_var( r.mark ) ) &&
                    !it->is_mark_protected() ) {
                    it->set_flag( r.mark );
                    it->set_var( auto_var( r ), "1" );
                    changed = true;
                }
            } else if( auto_set ) {
                // the item no longer matches (dried, refilled, rule switched off): take the mark back
                it->unset_flag( r.mark );
                it->erase_var( auto_var( r ) );
                changed = true;
            }
        }
        return VisitResponse::NEXT;
    } );
    if( changed ) {
        who.invalidate_inventory_validity_cache();
    }
}

void toggle_mark( std::vector<item_location> &stack, const flag_id &mark )
{
    if( stack.empty() || !stack.front() ) {
        return;
    }
    const bool turn_on = !stack.front()->has_own_flag( mark );
    for( item_location &loc : stack ) {
        if( !loc ) {
            continue;
        }
        if( turn_on ) {
            loc->set_flag( mark );
        } else {
            loc->unset_flag( mark );
        }
        note_toggled( *loc, mark, turn_on );
    }
}

void drop_marked( avatar &you, const flag_id &mark )
{
    if( !mark.is_valid() ) {
        return;
    }
    std::vector<item_location> found;
    std::set<const item *> picked;
    for( item_location &loc : you.all_items_loc() ) {
        if( !loc || !loc->has_own_flag( mark ) || loc->is_guarded() || you.is_worn( *loc ) ||
            you.is_wielding( *loc ) ) {
            continue;
        }
        // contents of a marked container leave with the container
        const item_location parent = loc.parent_item();
        if( parent && picked.count( parent.get_item() ) > 0 ) {
            continue;
        }
        picked.insert( loc.get_item() );
        found.push_back( loc );
    }
    // a second pass: a container found after its contents must keep only itself
    std::vector<item_location> top;
    for( const item_location &loc : found ) {
        item_location up = loc.parent_item();
        bool inside_found = false;
        while( up ) {
            if( picked.count( up.get_item() ) > 0 ) {
                inside_found = true;
                break;
            }
            up = up.parent_item();
        }
        if( !inside_found ) {
            top.push_back( loc );
        }
    }
    const std::string mark_name = mark->name().empty() ? mark.str() : mark->name();
    if( top.empty() ) {
        popup( _( "You carry nothing marked \"%s\"." ), mark_name );
        return;
    }
    std::string list;
    int shown = 0;
    for( const item_location &loc : top ) {
        if( shown++ >= 12 ) {
            list += string_format( _( "\n  ...and %d more" ), static_cast<int>( top.size() ) - 12 );
            break;
        }
        list += "\n  " + remove_color_tags( loc->tname( loc->count(), false ) );
    }
    if( !query_yn( _( "Drop everything marked \"%s\"?%s" ), mark_name, list ) ) {
        return;
    }
    drop_locations to_drop;
    for( const item_location &loc : top ) {
        to_drop.emplace_back( loc, loc->count() );
    }
    you.drop( to_drop, you.pos_bub() );
}
} // namespace item_marks
