#include "device_ui.h"

#include <imgui/imgui.h>
#include <imgui/imgui_stdlib.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "calendar.h"
#include "cata_imgui.h"
#include "character.h"
#include "cata_utility.h"
#include "color.h"
#include "game_constants.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "localized_comparator.h"
#include "math_parser_diag_value.h"
#include "itype.h"
#include "iuse.h"
#include "map.h"
#include "map_selector.h"
#include "output.h"
#include "player_activity.h"
#include "pocket_type.h"
#include "skill.h"
#include "string_formatter.h"
#include "translations.h"
#include "ui_manager.h"
#include "units.h"
#include "vehicle.h"
#include "vehicle_selector.h"
#include "vpart_position.h"

static const skill_id skill_computer( "computer" );

namespace
{
using compat_t = efile_activity_actor::edevice_compatible;

struct plan {
    efile_action action = EF_INVALID;
    efile_combo combo = COMBO_NONE;
    std::vector<item_location> targets;
    std::vector<item_location> files;
    bool valid = false;
};

/** Every e-device (item with a file storage) that is carried or lies within reach. */
/** The files of a device (the non-const list the e-file code wants). */
std::vector<item *> efiles_of( const item_location &dev )
{
    item_location copy = dev;
    return copy.get_item()->efiles();
}

void collect_edevices( const item_location &loc, std::vector<item_location> &out )
{
    if( !loc ) {
        return;
    }
    item_location copy = loc;
    if( copy->is_estorage() ) {
        out.push_back( loc );
        return; // devices inside devices do not matter here
    }
    for( item *child : copy.get_item()->all_items_top() ) {
        collect_edevices( item_location( loc, child ), out );
    }
}

std::vector<item_location> gather_edevices( Character &who )
{
    std::vector<item_location> out;
    map &here = get_map();
    // carried / worn / wielded
    if( item_location weapon = who.get_wielded_item() ) {
        collect_edevices( weapon, out );
    }
    for( item_location &top : who.top_items_loc() ) {
        collect_edevices( top, out );
    }
    // nearby, like the inventory menus do
    for( const tripoint_bub_ms &pos : closest_points_first( who.pos_bub(), PICKUP_RANGE ) ) {
        if( who.pos_bub() != pos &&
            !here.clear_path( who.pos_bub(), pos, rl_dist( who.pos_bub(), pos ), 1, 100 ) ) {
            continue;
        }
        if( here.accessible_items( pos ) ) {
            for( item &it : here.i_at( pos ) ) {
                collect_edevices( item_location( map_cursor( pos ), &it ), out );
            }
        }
        const std::optional<vpart_reference> ovp = here.veh_at( pos ).cargo();
        if( ovp ) {
            const vehicle_cursor cursor( ovp->vehicle(), ovp->part_index() );
            for( item &it : ovp->items() ) {
                collect_edevices( item_location( cursor, &it ), out );
            }
        }
    }
    return out;
}

class device_window : public cataimgui::window
{
    public:
        device_window( Character &who, item_location &used );

        device_ui::result outcome = device_ui::result::done;
        plan chosen;
        std::string app_method;
        std::optional<item_location> read_file;
        /** Empty memory cards / USB drives the player asked to drop. */
        std::vector<item_location> cards_to_drop;

        bool wants_to_close() const {
            return want_close;
        }

    protected:
        cataimgui::bounds get_bounds() override {
            return { -1.f, -1.f, 0.82f, 0.82f };
        }
        void draw_controls() override;

    private:
        Character &who;
        item_location used;
        std::vector<item_location> devices;
        int selected_device = 0;
        int target_device = -1;
        std::set<const item *> selected_files;
        bool want_close = false;
        int device_sort = 0;
        int file_sort = 0;
        bool hide_empty = false;
        std::string search;
        std::string saved_prefs;

        bool is_used( const item_location &loc ) const {
            return loc == used;
        }
        std::string device_name( const item_location &loc ) const;
        std::string where_text( const item_location &loc ) const;
        bool low_skill() const {
            return who.get_skill_level( skill_computer ) < 1;
        }
        /** Same conditions as the old device menus.  Returns an empty text if allowed. */
        std::string why_not( const item_location &loc, efile_action action ) const;
        std::string estimate_text( const plan &p ) const;
        /** Charges the device in use needs for a plan, and a warning text if it does not have them. */
        int charges_needed( const plan &p ) const;
        std::string charge_warning( const plan &p ) const;
        /** Does the search text match this text (case-insensitive)?  An empty search matches all. */
        bool matches_search( const std::string &text ) const;
        bool device_matches_search( const item_location &dev ) const;
        void load_prefs();
        void save_prefs();
        std::vector<item_location> files_of( const item_location &dev, bool only_selected ) const;
        /** Browsed, empty cards / drives (no device that can read them) that the player carries. */
        std::vector<item_location> empty_cards( int *unbrowsed_cards = nullptr ) const;
        /** The device rows in the order the sort mode wants (the used device stays first). */
        std::vector<int> device_order() const;
        void finish_with( const plan &p );
        void draw_header();
        void draw_apps();
        void draw_devices( const ImVec2 &size );
        void draw_files( const ImVec2 &size );
        void draw_actions();
        bool action_button( const char *label, const std::string &disabled_reason,
                            const std::string &tooltip );
};

device_window::device_window( Character &who_, item_location &used_ ) :
    cataimgui::window( remove_color_tags( used_->tname( 1, false ) ),
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove ),
    who( who_ ), used( used_ )
{
    load_prefs();
    devices = gather_edevices( who );
    // the used device first
    auto it = std::find( devices.begin(), devices.end(), used );
    if( it == devices.end() ) {
        devices.insert( devices.begin(), used );
    } else {
        std::rotate( devices.begin(), it, it + 1 );
    }
}

std::string device_window::device_name( const item_location &loc ) const
{
    std::string name = remove_color_tags( loc->tname( 1, false ) );
    if( is_used( loc ) ) {
        name += _( " (this device)" );
    }
    return name;
}

std::string device_window::where_text( const item_location &loc ) const
{
    if( loc.held_by( who ) ) {
        return _( "carried" );
    }
    return _( "nearby" );
}

std::string device_window::why_not( const item_location &loc, efile_action action ) const
{
    const map &here = get_map();
    if( !loc->is_estorage() ) {
        return _( "This is not a storage device." );
    }
    if( !loc->is_owned_by( who, true ) ) {
        return _( "This device belongs to someone else." );
    }
    if( !who.sees( here, loc.pos_bub( here ) ) ) {
        return _( "You can't see this device." );
    }
    if( loc->is_tool() && !loc->ammo_sufficient( &who ) ) {
        return _( "The device needs more charge." );
    }
    for( const pocket_data &pocket : loc->type->pockets ) {
        if( pocket.type == pocket_type::E_FILE_STORAGE && pocket.forbidden ) {
            return _( "This device can't hold files." );
        }
    }
    const compat_t compat = efile_activity_actor::edevices_compatible( used, loc );
    switch( action ) {
        case EF_BROWSE:
            if( loc->is_browsed() ) {
                return _( "Already browsed." );
            }
            if( compat == compat_t::ECOMPAT_NONE ) {
                return _( "This device can't be connected to the one you are using." );
            }
            break;
        case EF_READ:
            if( !loc->is_browsed() ) {
                return _( "Browse the device first." );
            }
            if( !is_used( loc ) && ( compat != compat_t::ECOMPAT_FAST ||
                                     efile_activity_actor::edevice_has_use( loc.get_item() ) ) ) {
                return _( "Copy the file onto this device to read it." );
            }
            break;
        default:
            if( !loc->is_browsed() ) {
                return _( "Browse the device first." );
            }
            if( compat == compat_t::ECOMPAT_NONE ) {
                return _( "This device can't be connected to the one you are using." );
            }
            break;
    }
    return std::string();
}

bool device_window::matches_search( const std::string &text ) const
{
    return search.empty() || lcmatch( text, search );
}

bool device_window::device_matches_search( const item_location &dev ) const
{
    if( search.empty() ) {
        return true;
    }
    if( matches_search( remove_color_tags( dev->tname( 1, false ) ) ) ) {
        return true;
    }
    if( dev->is_browsed() ) {
        for( const item *f : efiles_of( dev ) ) {
            if( matches_search( remove_color_tags( f->tname( 1, false ) ) ) ) {
                return true;
            }
        }
    }
    return false;
}

void device_window::load_prefs()
{
    // sort modes and the "hide empty" switch are remembered with the character
    const diag_value *saved = get_avatar().maybe_get_value( "device_ui_prefs" );
    if( saved != nullptr && saved->is_str() ) {
        int d = 0;
        int f = 0;
        int h = 0;
        if( sscanf( saved->str().c_str(), "%d,%d,%d", &d, &f, &h ) == 3 && d >= 0 && d <= 6 &&
            f >= 0 && f <= 3 ) {
            device_sort = d;
            file_sort = f;
            hide_empty = h != 0;
        }
    }
    saved_prefs = string_format( "%d,%d,%d", device_sort, file_sort, hide_empty ? 1 : 0 );
}

void device_window::save_prefs()
{
    const std::string now = string_format( "%d,%d,%d", device_sort, file_sort, hide_empty ? 1 : 0 );
    if( now != saved_prefs ) {
        saved_prefs = now;
        get_avatar().set_value( "device_ui_prefs", now );
    }
}

int device_window::charges_needed( const plan &p ) const
{
    if( !p.valid || p.files.empty() || p.targets.empty() || p.action == EF_BROWSE ) {
        return 0;
    }
    const bool from_used = efile_activity_actor::efile_action_is_from( p.action );
    const item_location &to_edevice = from_used ? p.targets.front() : used;
    time_duration required = efile_activity_actor::total_processing_time( used, p.targets, p.files,
                             p.action, who, low_skill() );
    for( const item_location &efile : p.files ) {
        const item_location parent = efile.parent_item();
        const efile_transfer transfer( to_edevice, parent );
        required += efile_activity_actor::efile_processing_time( efile, transfer, p.action, who,
                    low_skill() );
    }
    return required / efile_activity_actor::charge_per_transfer_time;
}

std::string device_window::charge_warning( const plan &p ) const
{
    if( !used->is_tool() ) {
        return std::string();
    }
    const int needed = charges_needed( p );
    const int have = used->ammo_remaining();
    if( needed > have ) {
        return string_format( _( "Low charge: this needs about %d charges, %s has %d.  "
                                 "It would stop halfway." ), needed, remove_color_tags( used->tname( 1, false ) ), have );
    }
    return std::string();
}

std::vector<item_location> device_window::empty_cards( int *unbrowsed_cards ) const
{
    std::vector<item_location> out;
    if( unbrowsed_cards != nullptr ) {
        *unbrowsed_cards = 0;
    }
    for( const item_location &loc : devices ) {
        // only plain storage (memory cards, USB drives, hard drives, any variant of them);
        // everything that can read files itself (phones, laptops, e-readers) stays
        if( is_used( loc ) || !loc->is_estorage() || !loc->is_tool() ||
            efile_activity_actor::edevice_has_use( loc.get_item() ) || !loc.held_by( who ) ) {
            continue;
        }
        if( !loc->is_browsed() ) {
            if( unbrowsed_cards != nullptr ) {
                ( *unbrowsed_cards )++;
            }
            continue;
        }
        if( efiles_of( loc ).empty() ) {
            out.push_back( loc );
        }
    }
    return out;
}

std::vector<int> device_window::device_order() const
{
    std::vector<int> order;
    for( int i = 1; i < static_cast<int>( devices.size() ); i++ ) {
        if( hide_empty && devices[i]->is_browsed() && efiles_of( devices[i] ).empty() ) {
            continue;
        }
        if( !device_matches_search( devices[i] ) ) {
            continue;
        }
        order.push_back( i );
    }
    const auto file_count = [this]( int i ) {
        return devices[i]->is_browsed() ? static_cast<int>( efiles_of( devices[i] ).size() ) : -1;
    };
    switch( device_sort ) {
        case 1: // name
            std::stable_sort( order.begin(), order.end(), [this]( int a, int b ) {
                return localized_compare( remove_color_tags( devices[a]->tname( 1, false ) ),
                                          remove_color_tags( devices[b]->tname( 1, false ) ) );
            } );
            break;
        case 2: // most files (not browsed devices last)
            std::stable_sort( order.begin(), order.end(), [&]( int a, int b ) {
                const int fa = file_count( a );
                const int fb = file_count( b );
                if( ( fa < 0 ) != ( fb < 0 ) ) {
                    return fa >= 0;
                }
                return fa > fb;
            } );
            break;
        case 3: // fewest files (not browsed devices last)
            std::stable_sort( order.begin(), order.end(), [&]( int a, int b ) {
                const int fa = file_count( a );
                const int fb = file_count( b );
                if( ( fa < 0 ) != ( fb < 0 ) ) {
                    return fa >= 0;
                }
                return fa < fb;
            } );
            break;
        case 4: // most free space
            std::stable_sort( order.begin(), order.end(), [this]( int a, int b ) {
                return devices[a]->remaining_ememory() > devices[b]->remaining_ememory();
            } );
            break;
        case 5: // least free space
            std::stable_sort( order.begin(), order.end(), [this]( int a, int b ) {
                return devices[a]->remaining_ememory() < devices[b]->remaining_ememory();
            } );
            break;
        case 6: // carried first
            std::stable_sort( order.begin(), order.end(), [this]( int a, int b ) {
                return devices[a].held_by( who ) && !devices[b].held_by( who );
            } );
            break;
        default:
            break;
    }
    order.insert( order.begin(), 0 );
    return order;
}

std::vector<item_location> device_window::files_of( const item_location &dev,
        bool only_selected ) const
{
    std::vector<item_location> out;
    for( item *file : efiles_of( dev ) ) {
        if( !only_selected || selected_files.count( file ) > 0 ) {
            out.emplace_back( dev, file );
        }
    }
    return out;
}

std::string device_window::estimate_text( const plan &p ) const
{
    if( !p.valid || p.files.empty() || p.targets.empty() || p.action == EF_BROWSE ) {
        return std::string();
    }
    const bool from_used = efile_activity_actor::efile_action_is_from( p.action );
    const item_location &to_edevice = from_used ? p.targets.front() : used;
    time_duration required = efile_activity_actor::total_processing_time( used, p.targets, p.files,
                             p.action, who, low_skill() );
    units::ememory total_memory;
    for( const item_location &efile : p.files ) {
        const item_location parent = efile.parent_item();
        const efile_transfer transfer( to_edevice, parent );
        required += efile_activity_actor::efile_processing_time( efile, transfer, p.action, who,
                    low_skill() );
        total_memory += efile->ememory_size();
    }
    const int charges = required / efile_activity_actor::charge_per_transfer_time;
    std::string text = string_format( _( "About %s, %d charges (device has %d)." ),
                                      to_string( required, true ), charges, used->ammo_remaining() );
    if( p.action != EF_WIPE ) {
        const units::ememory room = to_edevice->remaining_ememory();
        text += string_format( _( "\nFiles: %s, free space: %s." ), units::display( total_memory ),
                               units::display( room ) );
        if( total_memory > room ) {
            text += _( "\nNot enough free space!" );
        }
    }
    const std::string warning = charge_warning( p );
    if( !warning.empty() ) {
        text += "\n" + warning;
    }
    return text;
}

void device_window::finish_with( const plan &p )
{
    chosen = p;
    want_close = true;
    is_open = false;
}

bool device_window::action_button( const char *label, const std::string &disabled_reason,
                                   const std::string &tooltip )
{
    const bool disabled = !disabled_reason.empty();
    if( disabled ) {
        ImGui::BeginDisabled();
    }
    const bool clicked = ImGui::Button( label );
    if( disabled ) {
        ImGui::EndDisabled();
    }
    if( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) ) {
        const std::string text = disabled ? disabled_reason : tooltip;
        if( !text.empty() ) {
            ImGui::SetTooltip( "%s", text.c_str() );
        }
    }
    return clicked && !disabled;
}

void device_window::draw_header()
{
    cataimgui::draw_colored_text( used->tname( 1, false ) );
    ImGui::SameLine();
    std::string info;
    if( used->is_tool() ) {
        info += string_format( _( "   Charge: %d" ), used->ammo_remaining() );
    }
    info += string_format( _( "   Memory: %s / %s used" ), units::display( used->occupied_ememory() ),
                           units::display( used->total_ememory() ) );
    ImGui::TextUnformatted( info.c_str() );
}

void device_window::draw_apps()
{
    // the other things the device can do (music, camera, games ...)
    bool any = false;
    for( const std::pair<const std::string, use_function> &method : used->type->use_methods ) {
        if( method.first == "E_FILE_DEVICE" || method.second.get_type() == "E_FILE_DEVICE" ) {
            continue;
        }
        const std::string name = remove_color_tags( method.second.get_name() );
        if( name.empty() ) {
            continue;
        }
        if( !any ) {
            ImGui::TextUnformatted( _( "Apps:" ) );
            any = true;
        }
        ImGui::SameLine();
        ImGui::PushID( method.first.c_str() );
        if( ImGui::Button( name.c_str() ) ) {
            app_method = method.first;
            want_close = true;
            is_open = false;
        }
        ImGui::PopID();
    }
    if( any ) {
        ImGui::Spacing();
    }
}

void device_window::draw_devices( const ImVec2 &size )
{
    ImGui::TextUnformatted( _( "Devices" ) );
    ImGui::SameLine();
    static const char *const sort_names[] = { translate_marker( "Default order" ), translate_marker( "Name" ), translate_marker( "Most files" ),
                                              translate_marker( "Fewest files" ), translate_marker( "Most free space" ),
                                              translate_marker( "Least free space" ), translate_marker( "Carried first" )
                                            };
    ImGui::SetNextItemWidth( ImGui::GetFontSize() * 11 );
    if( ImGui::BeginCombo( "##device_sort", ( std::string( _( "Sort: " ) ) +
                           _( sort_names[device_sort] ) ).c_str() ) ) {
        for( int m = 0; m < static_cast<int>( sizeof( sort_names ) / sizeof( sort_names[0] ) ); m++ ) {
            if( ImGui::Selectable( _( sort_names[m] ), m == device_sort ) ) {
                device_sort = m;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox( _( "Hide empty" ), &hide_empty );
    if( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip( "%s", _( "Hide devices that were browsed and hold no files." ) );
    }
    if( ImGui::BeginChild( "##devices", size, ImGuiChildFlags_Borders ) ) {
        if( ImGui::BeginTable( "##device_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) ) {
            ImGui::TableSetupColumn( _( "Name" ), ImGuiTableColumnFlags_WidthStretch, 3.0f );
            ImGui::TableSetupColumn( _( "Memory" ), ImGuiTableColumnFlags_WidthStretch, 2.0f );
            ImGui::TableSetupColumn( _( "Files" ), ImGuiTableColumnFlags_WidthStretch, 1.5f );
            ImGui::TableHeadersRow();
            const std::vector<int> order = device_order();
            for( const int i : order ) {
                const item_location &dev = devices[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID( i );
                const std::string label = device_name( dev ) + "  [" + where_text( dev ) + "]";
                if( ImGui::Selectable( label.c_str(), i == selected_device,
                                       ImGuiSelectableFlags_SpanAllColumns ) ) {
                    if( selected_device != i ) {
                        selected_files.clear();
                    }
                    selected_device = i;
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( string_format( "%s / %s", units::display( dev->occupied_ememory() ),
                                                       units::display( dev->total_ememory() ) ).c_str() );
                ImGui::TableNextColumn();
                if( dev->is_browsed() ) {
                    ImGui::TextUnformatted( string_format( "%d", static_cast<int>( efiles_of( dev ).size() ) ).c_str() );
                } else {
                    ImGui::TextUnformatted( _( "not browsed" ) );
                }
            }
            if( std::find( order.begin(), order.end(), selected_device ) == order.end() ) {
                // the picked device was hidden by the filter
                selected_device = 0;
                selected_files.clear();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void device_window::draw_files( const ImVec2 &size )
{
    const item_location &dev = devices[selected_device];
    ImGui::TextUnformatted( string_format( _( "Files on %s" ), device_name( dev ) ).c_str() );
    if( ImGui::BeginChild( "##files", size, ImGuiChildFlags_Borders ) ) {
        if( !dev->is_browsed() ) {
            ImGui::TextWrapped( "%s", _( "Nobody has looked through this device yet.  "
                                         "Browse it (button below) to see its files." ) );
        } else {
            std::vector<item *> files = efiles_of( dev );
            if( !search.empty() && !matches_search( remove_color_tags( dev->tname( 1, false ) ) ) ) {
                // the search text names files, not this device: show only the files that match
                files.erase( std::remove_if( files.begin(), files.end(), [this]( const item * f ) {
                    return !matches_search( remove_color_tags( f->tname( 1, false ) ) );
                } ), files.end() );
            }
            if( file_sort != 0 ) {
                std::stable_sort( files.begin(), files.end(), [this]( const item * a, const item * b ) {
                    switch( file_sort ) {
                        case 1:
                            return localized_compare( remove_color_tags( a->tname( 1, false ) ),
                                                      remove_color_tags( b->tname( 1, false ) ) );
                        case 2:
                            return a->ememory_size() > b->ememory_size();
                        default:
                            return a->ememory_size() < b->ememory_size();
                    }
                } );
            }
            if( files.empty() ) {
                ImGui::TextUnformatted( _( "This device has no files." ) );
            } else {
                if( ImGui::SmallButton( _( "Select all" ) ) ) {
                    for( const item *f : files ) {
                        selected_files.insert( f );
                    }
                }
                ImGui::SameLine();
                if( ImGui::SmallButton( _( "Select none" ) ) ) {
                    selected_files.clear();
                }
                ImGui::SameLine();
                if( ImGui::SmallButton( _( "Invert" ) ) ) {
                    for( const item *f : files ) {
                        if( selected_files.count( f ) > 0 ) {
                            selected_files.erase( f );
                        } else {
                            selected_files.insert( f );
                        }
                    }
                }
                ImGui::SameLine();
                static const char *const file_sort_names[] = { translate_marker( "Default order" ), translate_marker( "Name" ),
                                                               translate_marker( "Largest first" ), translate_marker( "Smallest first" )
                                                             };
                ImGui::SetNextItemWidth( ImGui::GetFontSize() * 11 );
                if( ImGui::BeginCombo( "##file_sort", ( std::string( _( "Sort: " ) ) +
                                       _( file_sort_names[file_sort] ) ).c_str() ) ) {
                    for( int m = 0; m < 4; m++ ) {
                        if( ImGui::Selectable( _( file_sort_names[m] ), m == file_sort ) ) {
                            file_sort = m;
                        }
                    }
                    ImGui::EndCombo();
                }
                if( ImGui::BeginTable( "##file_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingStretchProp ) ) {
                    ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed );
                    ImGui::TableSetupColumn( _( "Name" ), ImGuiTableColumnFlags_WidthStretch, 4.0f );
                    ImGui::TableSetupColumn( _( "Size" ), ImGuiTableColumnFlags_WidthStretch, 1.0f );
                    ImGui::TableSetupScrollFreeze( 0, 1 );
                    ImGui::TableHeadersRow();
                    for( size_t i = 0; i < files.size(); i++ ) {
                        const item *f = files[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID( static_cast<int>( i ) );
                        bool checked = selected_files.count( f ) > 0;
                        if( ImGui::Checkbox( "##sel", &checked ) ) {
                            if( checked ) {
                                selected_files.insert( f );
                            } else {
                                selected_files.erase( f );
                            }
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted( remove_color_tags( f->tname( 1, false ) ).c_str() );
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted( units::display( f->ememory_size() ).c_str() );
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
        }
    }
    ImGui::EndChild();
}

void device_window::draw_actions()
{
    const item_location &dev = devices[selected_device];
    const bool on_used = is_used( dev );
    const size_t picked = selected_files.size();

    // ---- browsing
    if( !dev->is_browsed() ) {
        plan p;
        p.action = EF_BROWSE;
        p.targets = { dev };
        p.files = files_of( dev, false );
        p.valid = true;
        if( action_button( _( "Browse this device" ), why_not( dev, EF_BROWSE ),
                           _( "Look through the device to find out what files it holds.  Takes some time." ) ) ) {
            finish_with( p );
            return;
        }
        ImGui::SameLine();
    }
    std::vector<item_location> unbrowsed;
    for( const item_location &other : devices ) {
        if( why_not( other, EF_BROWSE ).empty() ) {
            unbrowsed.push_back( other );
        }
    }
    {
        plan p;
        p.action = EF_BROWSE;
        p.targets = unbrowsed;
        for( const item_location &other : unbrowsed ) {
            for( const item_location &f : files_of( other, false ) ) {
                p.files.push_back( f );
            }
        }
        p.valid = !unbrowsed.empty();
        const std::string reason = p.valid ? std::string() : std::string( _( "Every device has been browsed." ) );
        if( action_button( _( "Browse all devices" ), reason,
                           _( "Browse every device that has not been browsed yet." ) ) ) {
            finish_with( p );
            return;
        }
        ImGui::SameLine();
        plan q;
        std::string q_reason;
        std::string q_tip;
        if( !unbrowsed.empty() ) {
            // some devices were not looked through yet: browse them, then move everything over
            q = p;
            q.combo = COMBO_MOVE_ONTO_BROWSE;
            q_reason = reason;
            q_tip = _( "Browse all other devices, then move all their files onto the device you are using.  "
                       "Devices that were browsed before are not included; press the button again afterwards "
                       "to move their files too." );
        } else {
            // everything was browsed already: simply move all files of the other devices here
            q.action = EF_MOVE_ONTO_THIS;
            for( const item_location &other : devices ) {
                if( is_used( other ) || !why_not( other, EF_MOVE_ONTO_THIS ).empty() ) {
                    continue;
                }
                const std::vector<item_location> found = files_of( other, false );
                if( found.empty() ) {
                    continue;
                }
                q.targets.push_back( other );
                q.files.insert( q.files.end(), found.begin(), found.end() );
            }
            q.valid = !q.files.empty();
            if( !used->is_browsed() ) {
                q_reason = _( "Browse this device first." );
            } else if( !q.valid ) {
                q_reason = _( "No other device has files to move." );
            }
            q_tip = _( "Move every file of all other devices onto the device you are using." ) +
                    std::string( "\n" ) + estimate_text( q );
        }
        if( action_button( _( "Browse all + move every file here" ), q_reason, q_tip ) ) {
            finish_with( q );
            return;
        }
    }
    ImGui::NewLine();

    // ---- reading
    const std::string selection_hint = picked == 0 ? std::string( _( "Tick some files first." ) ) :
                                       std::string();
    {
        std::string reason = why_not( dev, EF_READ );
        if( reason.empty() && picked != 1 ) {
            reason = _( "Tick exactly one file to read it." );
        }
        if( action_button( _( "Read file" ), reason, _( "Read the ticked e-book or document." ) ) ) {
            for( item *f : efiles_of( dev ) ) {
                if( selected_files.count( f ) > 0 ) {
                    read_file = item_location( dev, f );
                    break;
                }
            }
            want_close = true;
            is_open = false;
            return;
        }
    }
    ImGui::SameLine();

    // ---- copy / move
    struct transfer_button {
        const char *label;
        efile_action action;
        const char *tip;
    };
    if( on_used ) {
        // files on this device go to another device
        std::vector<int> candidates;
        for( int i = 0; i < static_cast<int>( devices.size() ); i++ ) {
            if( !is_used( devices[i] ) && why_not( devices[i], EF_COPY_FROM_THIS ).empty() ) {
                candidates.push_back( i );
            }
        }
        if( std::find( candidates.begin(), candidates.end(), target_device ) == candidates.end() ) {
            target_device = candidates.empty() ? -1 : candidates.front();
        }
        ImGui::SetNextItemWidth( ImGui::GetFontSize() * 14 );
        const std::string preview = target_device < 0 ? std::string( _( "(no other device)" ) ) :
                                    device_name( devices[target_device] );
        if( ImGui::BeginCombo( "##target", preview.c_str() ) ) {
            for( int idx : candidates ) {
                if( ImGui::Selectable( device_name( devices[idx] ).c_str(), idx == target_device ) ) {
                    target_device = idx;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const transfer_button buttons[] = {
            { translate_marker( "Copy to selected device" ), EF_COPY_FROM_THIS, translate_marker( "Copy the ticked files onto the device chosen on the left." ) },
            { translate_marker( "Move to selected device" ), EF_MOVE_FROM_THIS, translate_marker( "Move the ticked files onto the device chosen on the left (they leave this one)." ) }
        };
        for( const transfer_button &b : buttons ) {
            plan p;
            p.action = b.action;
            if( target_device >= 0 ) {
                p.targets = { devices[target_device] };
            }
            p.files = files_of( dev, true );
            p.valid = target_device >= 0 && picked > 0;
            std::string reason = target_device < 0 ? std::string( _( "No other device to copy to." ) ) :
                                 selection_hint;
            if( reason.empty() ) {
                reason = why_not( dev, b.action );
            }
            if( action_button( _( b.label ), reason, _( b.tip ) + std::string( "\n" ) + estimate_text( p ) ) ) {
                finish_with( p );
                return;
            }
            ImGui::SameLine();
        }
        const transfer_button all_buttons[] = {
            { translate_marker( "Copy ALL" ), EF_COPY_FROM_THIS, translate_marker( "Copy every file on this device onto the device chosen on the left." ) },
            { translate_marker( "Move ALL" ), EF_MOVE_FROM_THIS, translate_marker( "Move every file on this device onto the device chosen on the left." ) }
        };
        for( const transfer_button &b : all_buttons ) {
            plan p;
            p.action = b.action;
            if( target_device >= 0 ) {
                p.targets = { devices[target_device] };
            }
            p.files = files_of( dev, false );
            p.valid = target_device >= 0 && !p.files.empty();
            std::string reason = target_device < 0 ? std::string( _( "No other device to copy to." ) ) :
                                 ( p.files.empty() ? std::string( _( "There are no files on this device." ) ) :
                                   std::string() );
            if( reason.empty() ) {
                reason = why_not( dev, b.action );
            }
            if( action_button( _( b.label ), reason, _( b.tip ) + std::string( "\n" ) + estimate_text( p ) ) ) {
                finish_with( p );
                return;
            }
            ImGui::SameLine();
        }
    } else {
        // files on another device come onto this one
        const transfer_button buttons[] = {
            { translate_marker( "Copy to this device" ), EF_COPY_ONTO_THIS, translate_marker( "Copy the ticked files onto the device you are using." ) },
            { translate_marker( "Move to this device" ), EF_MOVE_ONTO_THIS, translate_marker( "Move the ticked files onto the device you are using (they leave the other one)." ) }
        };
        for( const transfer_button &b : buttons ) {
            plan p;
            p.action = b.action;
            p.targets = { dev };
            p.files = files_of( dev, true );
            p.valid = picked > 0;
            std::string reason = selection_hint;
            if( reason.empty() ) {
                reason = why_not( dev, b.action );
            }
            if( reason.empty() && ( !used->is_browsed() ) ) {
                reason = _( "Browse this device first." );
            }
            if( action_button( _( b.label ), reason, _( b.tip ) + std::string( "\n" ) + estimate_text( p ) ) ) {
                finish_with( p );
                return;
            }
            ImGui::SameLine();
        }
        const transfer_button all_buttons[] = {
            { translate_marker( "Copy ALL here" ), EF_COPY_ONTO_THIS, translate_marker( "Copy every file on this device onto the device you are using." ) },
            { translate_marker( "Move ALL here" ), EF_MOVE_ONTO_THIS, translate_marker( "Move every file on this device onto the device you are using." ) }
        };
        for( const transfer_button &b : all_buttons ) {
            plan p;
            p.action = b.action;
            p.targets = { dev };
            p.files = files_of( dev, false );
            p.valid = !p.files.empty();
            std::string reason = p.files.empty() ? std::string( _( "There are no files on this device." ) ) :
                                 std::string();
            if( reason.empty() ) {
                reason = why_not( dev, b.action );
            }
            if( reason.empty() && ( !used->is_browsed() ) ) {
                reason = _( "Browse this device first." );
            }
            if( action_button( _( b.label ), reason, _( b.tip ) + std::string( "\n" ) + estimate_text( p ) ) ) {
                finish_with( p );
                return;
            }
            ImGui::SameLine();
        }
    }

    // ---- wipe
    {
        plan p;
        p.action = EF_WIPE;
        p.targets = { dev };
        p.files = files_of( dev, true );
        p.valid = picked > 0;
        std::string reason = selection_hint;
        if( reason.empty() ) {
            reason = why_not( dev, EF_WIPE );
        }
        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.55f, 0.15f, 0.15f, 1.0f ) );
        const bool clicked = action_button( _( "Wipe ticked files" ), reason,
                                            std::string( _( "Delete the ticked files for good." ) ) + "\n" + estimate_text( p ) );
        ImGui::PopStyleColor();
        if( clicked ) {
            finish_with( p );
            return;
        }
    }

    // ---- low charge warning for what is currently selected
    {
        plan p;
        p.action = on_used ? EF_COPY_FROM_THIS : EF_COPY_ONTO_THIS;
        if( on_used ) {
            if( target_device >= 0 ) {
                p.targets = { devices[target_device] };
            }
        } else {
            p.targets = { dev };
        }
        p.files = files_of( dev, picked > 0 );
        p.valid = !p.files.empty() && !p.targets.empty();
        const std::string warning = charge_warning( p );
        if( !warning.empty() ) {
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", warning.c_str() );
        }
    }
}

void device_window::draw_controls()
{
    if( selected_device < 0 || selected_device >= static_cast<int>( devices.size() ) ) {
        selected_device = 0;
    }
    draw_header();
    draw_apps();
    ImGui::Separator();
    ImGui::SetNextItemWidth( ImGui::GetFontSize() * 18 );
    ImGui::InputTextWithHint( "##search", _( "Search devices and files..." ), &search );
    if( !search.empty() ) {
        ImGui::SameLine();
        if( ImGui::SmallButton( _( "Clear" ) ) ) {
            search.clear();
        }
    }
    save_prefs();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 5.5f;
    const float height = std::max( 80.0f, avail.y - footer );
    const float left_width = avail.x * 0.42f;
    ImGui::BeginGroup();
    draw_devices( ImVec2( left_width, height ) );
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    draw_files( ImVec2( 0.0f, height ) );
    ImGui::EndGroup();

    ImGui::Separator();
    if( !want_close ) {
        draw_actions();
    }
    ImGui::NewLine();
    if( ImGui::Button( _( "Close" ) ) ) {
        want_close = true;
        is_open = false;
    }
    ImGui::SameLine();
    {
        int unbrowsed_cards = 0;
        const std::vector<item_location> empties = empty_cards( &unbrowsed_cards );
        std::string reason;
        if( empties.empty() ) {
            reason = _( "You carry no browsed, empty memory cards or USB drives." );
            if( unbrowsed_cards > 0 ) {
                reason += string_format( _( "\n%d not browsed yet are skipped." ), unbrowsed_cards );
            }
        }
        std::string tip = _( "Drop every browsed, empty memory card and USB drive you carry.  "
                             "Devices that can read files (phones, laptops, e-readers) are never dropped." );
        for( const item_location &loc : empties ) {
            tip += "\n  " + remove_color_tags( loc->tname( 1, false ) );
        }
        if( unbrowsed_cards > 0 ) {
            tip += string_format( _( "\n%d not browsed yet are skipped." ), unbrowsed_cards );
        }
        const std::string label = empties.empty() ? std::string( _( "Drop empty cards" ) ) :
                                  string_format( _( "Drop empty cards (%d)" ), static_cast<int>( empties.size() ) );
        if( action_button( label.c_str(), reason, tip ) ) {
            cards_to_drop = empties;
            want_close = true;
            is_open = false;
        }
    }
    ImGui::SameLine();
    if( ImGui::Button( _( "Old menu" ) ) ) {
        outcome = device_ui::result::classic;
        want_close = true;
        is_open = false;
    }
}
} // namespace

namespace device_ui
{
result run( Character &who, item_location &used_edevice )
{
    avatar *you = who.as_avatar();
    if( you == nullptr ) {
        return result::classic;
    }
    input_context ctxt( "DEVICE_WINDOW" );
    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "ANY_INPUT" );
    // This is still bizarrely necessary for imgui
    ctxt.set_timeout( 10 );

    device_window window( who, used_edevice );
    while( true ) {
        ui_manager::redraw_invalidated();
        const std::string action = ctxt.handle_input();
        if( action == "QUIT" || !window.get_is_open() || window.wants_to_close() ) {
            break;
        }
    }
    if( window.outcome == result::classic ) {
        return result::classic;
    }

    // what the player picked happens after the window is gone
    if( !window.cards_to_drop.empty() ) {
        drop_locations to_drop;
        for( const item_location &card : window.cards_to_drop ) {
            to_drop.emplace_back( card, card->count() );
        }
        you->drop( to_drop, you->pos_bub() );
    } else if( window.read_file ) {
        you->use( *window.read_file );
    } else if( !window.app_method.empty() ) {
        you->invoke_item( used_edevice.get_item(), window.app_method );
    } else if( window.chosen.valid ) {
        const plan &p = window.chosen;
        const efile_activity_actor actor( used_edevice, p.targets, p.files, p.action, p.combo );
        who.assign_activity( player_activity( actor ) );
    }
    return result::done;
}
} // namespace device_ui
