#include "device_ui.h"

#include <imgui/imgui.h>
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
#include "color.h"
#include "game_constants.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
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
        std::vector<item_location> files_of( const item_location &dev, bool only_selected ) const;
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
    if( ImGui::BeginChild( "##devices", size, ImGuiChildFlags_Borders ) ) {
        if( ImGui::BeginTable( "##device_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) ) {
            ImGui::TableSetupColumn( _( "Name" ), ImGuiTableColumnFlags_WidthStretch, 3.0f );
            ImGui::TableSetupColumn( _( "Memory" ), ImGuiTableColumnFlags_WidthStretch, 2.0f );
            ImGui::TableSetupColumn( _( "Files" ), ImGuiTableColumnFlags_WidthStretch, 1.5f );
            ImGui::TableHeadersRow();
            for( int i = 0; i < static_cast<int>( devices.size() ); i++ ) {
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
            const std::vector<item *> files = efiles_of( dev );
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
        plan q = p;
        q.combo = COMBO_MOVE_ONTO_BROWSE;
        if( action_button( _( "Browse all + move every file here" ), reason,
                           _( "Browse all other devices, then move all their files onto the device you are using." ) ) ) {
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
}

void device_window::draw_controls()
{
    if( selected_device < 0 || selected_device >= static_cast<int>( devices.size() ) ) {
        selected_device = 0;
    }
    draw_header();
    draw_apps();
    ImGui::Separator();

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
    if( window.read_file ) {
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
