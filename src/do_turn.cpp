#include "do_turn.h"

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <ratio>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <unordered_set>
#include <vector>

#include "action.h"
#include "alerts.h"
#include "activity_type.h"
#include "avatar.h"
#include "bionics.h"
#include "cached_options.h"
#include "bubbles.h"
#include "current_map.h"
#include "calendar.h"
#include "cata_variant.h"
#include "clzones.h"
#include "coordinates.h"
#include "debug.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "explosion.h"
#include "field.h"
#include "fluid.h"
#include "focus_minigame.h"
#include "game.h"
#include "game_constants.h"
#include "gamemode.h"
#include "help.h"
#include "input.h"
#include "input_context.h"
#include "make_static.h"
#include "magic_enchantment.h"
#include "map.h"
#include "map_iterator.h"
#include "map_scale_constants.h"
#include "mapbuffer.h"
#include "mapdata.h"
#include "memorial_logger.h"
#include "messages.h"
#include "mission.h"
#include "item_marks.h"
#include "mod_engine.h"
#include "monster.h"
#include "mtype.h"
#include "music.h"
#include "net_session.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmap_ui.h"
#include "overmapbuffer.h"
#include "pimpl.h"
#include "player_activity.h"
#include "point.h"
#include "popup.h"
#include "rng.h"
#include "scent_map.h"
#include "sdlsound.h"
#include "simple_pathfinding.h"
#include "sounds.h"
#include "stations.h"
#include "stats_tracker.h"
#include "string_formatter.h"
#include "timed_event.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "ui_manager.h"
#include "units.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"
#include "weather_type.h"
#include "worldfactory.h"

static const activity_id ACT_AUTODRIVE( "ACT_AUTODRIVE" );
static const activity_id ACT_FIRSTAID( "ACT_FIRSTAID" );
static const activity_id ACT_OPERATION( "ACT_OPERATION" );

static const bionic_id bio_alarm( "bio_alarm" );

static const itype_id itype_afs_atomic_smartphone_music( "afs_atomic_smartphone_music" );
static const itype_id itype_afs_atomic_wraitheon_music( "afs_atomic_wraitheon_music" );
static const itype_id itype_atomic_mp3_on( "atomic_mp3_on" );
static const itype_id itype_atomic_smartphone_music( "atomic_smartphone_music" );
static const itype_id itype_mp3_on( "mp3_on" );
static const itype_id itype_smartphone_music( "smartphone_music" );

static const efftype_id effect_controlled( "controlled" );
static const efftype_id effect_npc_suspend( "npc_suspend" );
static const efftype_id effect_ridden( "ridden" );
static const efftype_id effect_sleep( "sleep" );

static const event_statistic_id event_statistic_last_words( "last_words" );

static const trait_id trait_HAS_NEMESIS( "HAS_NEMESIS" );

#if defined(__ANDROID__)
extern std::map<std::string, std::list<input_event>> quick_shortcuts_map;
extern bool add_best_key_for_action_to_quick_shortcuts( action_id action,
        const std::string &category, bool back );
#endif

#define dbg(x) DebugLog((x),D_GAME) << __FILE__ << ":" << __LINE__ << ": "

namespace turn_handler
{
bool cleanup_at_end()
{
    mod_engine::shutdown_windows();
    avatar &u = get_avatar();
    if( g->uquit == QUIT_DIED || g->uquit == QUIT_SUICIDE ) {
        // Put (non-hallucinations) into the overmap so they are not lost.
        for( monster &critter : g->all_monsters() ) {
            g->despawn_monster( critter );
        }
        // if player has "hunted" trait, remove their nemesis monster on death
        if( u.has_trait( trait_HAS_NEMESIS ) ) {
            overmap_buffer.remove_nemesis();
        }
        // Reset NPC factions and disposition
        g->reset_npc_dispositions();
        // Save the factions', missions and set the NPC's overmap coordinates
        // Npcs are saved in the overmap.
        g->save_factions_missions_npcs(); //missions need to be saved as they are global for all saves.

        // and the overmap, and the local map.
        g->save_maps(); //Omap also contains the npcs who need to be saved.

        //save achievements entry
        g->save_achievements();

        g->death_screen();
        std::chrono::seconds time_since_load =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - g->time_of_last_load );
        std::chrono::seconds total_time_played = g->time_played_at_last_load + time_since_load;
        get_event_bus().send<event_type::game_over>( total_time_played );
        // Struck the save_player_data here to forestall Weirdness
        g->move_save_to_graveyard();
        g->write_memorial_file( g->stats().value_of( event_statistic_last_words )
                                .get<cata_variant_type::string>() );
        get_memorial().clear();
        std::vector<std::string> characters = g->list_active_saves();
        // remove current player from the active characters list, as they are dead
        std::vector<std::string>::iterator curchar = std::find( characters.begin(),
                characters.end(), u.get_save_id() );
        if( curchar != characters.end() ) {
            characters.erase( curchar );
        }

        if( characters.empty() ) {
            bool queryDelete = false;
            bool queryReset = false;

            if( get_option<std::string>( "WORLD_END" ) == "query" ) {
                bool decided = false;
                std::string buffer = _( "Warning: NPC interactions and some other global flags "
                                        "will not all reset when starting a new character in an "
                                        "already-played world.  This can lead to some strange "
                                        "behavior.\n\n"
                                        "Are you sure you wish to keep this world?"
                                      );

                while( !decided ) {
                    uilist smenu;
                    smenu.allow_cancel = false;
                    smenu.addentry( 0, true, 'r', "%s", _( "Reset world" ) );
                    smenu.addentry( 1, true, 'd', "%s", _( "Delete world" ) );
                    smenu.addentry( 2, true, 'k', "%s", _( "Keep world" ) );
                    smenu.query();

                    switch( smenu.ret ) {
                        case 0:
                            queryReset = true;
                            decided = true;
                            break;
                        case 1:
                            queryDelete = true;
                            decided = true;
                            break;
                        case 2:
                            decided = query_yn( buffer );
                            break;
                    }
                }
            }

            if( queryDelete || get_option<std::string>( "WORLD_END" ) == "delete" ) {
                world_generator->delete_world( world_generator->active_world->world_name, true );

            } else if( queryReset || get_option<std::string>( "WORLD_END" ) == "reset" ) {
                world_generator->delete_world( world_generator->active_world->world_name, false );
            }
        } else if( get_option<std::string>( "WORLD_END" ) != "keep" ) {
            std::string tmpmessage;
            for( auto &character : characters ) {
                tmpmessage += "\n  ";
                tmpmessage += character;
            }
            popup( _( "World retained.  Characters remaining:%s" ), tmpmessage );
        }
        if( g->gamemode ) {
            g->gamemode = std::make_unique<special_game>(); // null gamemode or something..
        }
    }

    //Reset any offset due to driving
    g->set_driving_view_offset( point_rel_ms::zero );

    //clear all sound channels
    sfx::fade_audio_channel( sfx::channel::any, 300 );
    sfx::fade_audio_group( sfx::group::weather, 300 );
    sfx::fade_audio_group( sfx::group::time_of_day, 300 );
    sfx::fade_audio_group( sfx::group::context_themes, 300 );
    sfx::fade_audio_group( sfx::group::low_stamina, 300 );

    zone_manager::get_manager().clear();

    MAPBUFFER.clear();
    overmap_buffer.clear();

#if defined(__ANDROID__)
    quick_shortcuts_map.clear();
#endif
    return true;
}

} // namespace turn_handler

void handle_key_blocking_activity()
{
    if( test_mode ) {
        return;
    }
    avatar &u = get_avatar();
    const bool has_unfinished_activity = u.activity && (
            u.activity.id()->based_on() == based_on_type::NEITHER
            || u.activity.moves_left > 0 );
    if( has_unfinished_activity || u.has_destination() ) {
        input_context ctxt = get_default_mode_input_context();
        const std::string action = ctxt.handle_input( 0 );
        bool refresh = true;
        // Realtime / multiplayer (B7): one key stops an action at once, no question asked.
        if( alerts::nonblocking() ) {
            if( ctxt.get_raw_input().get_first_input() == ' ' ) {
                // SPACE answers the focus prompt (B3/B5) instead of stopping the action
                focus_minigame::press( u );
                return;
            }
            const action_id act = look_up_action( action );
            const bool movement = act >= ACTION_MOVE_FORTH && act <= ACTION_MOVE_UP;
            if( action == "pause" || action == "QUIT" || act == ACTION_MAIN_MENU || movement ) {
                if( u.has_destination() ) {
                    u.abort_automove();
                }
                if( u.activity && u.activity.is_interruptible_with_kb() ) {
                    add_msg( m_alert, _( "You stop %s." ), u.activity.get_verb().translated() );
                    u.cancel_activity();
                }
                ui_manager::redraw();
                refresh_display();
                return;
            }
        }
        if( action == "pause" ) {
            if( u.activity.is_interruptible_with_kb() ) {
                g->cancel_activity_query( _( "Confirm:" ) );
            }
        } else if( action == "player_data" ) {
            u.disp_info( true );
        } else if( action == "messages" ) {
            Messages::display_messages();
        } else if( action == "help" ) {
            get_help().display_help();
        } else if( action != "HELP_KEYBINDINGS" ) {
            refresh = false;
        }
        if( refresh ) {
            ui_manager::redraw();
            refresh_display();
        }
    } else {
        refresh_display();
        inp_mngr.pump_events();
    }
}

namespace
{
void monmove()
{
    g->cleanup_dead();
    avatar &u = get_avatar();

    // Experimental multiplayer: the main bubble and every extra bubble run their own monsters.
    std::unordered_set<const monster *> simulated;
    std::vector<map *> bubble_maps = { &get_map() };
    bubble_maps.insert( bubble_maps.end(), bubbles::extras().begin(), bubbles::extras().end() );
    for( map *bubble_map : bubble_maps ) {
    swap_map swap_to_bubble( *bubble_map );
    map &m = *bubble_map;
    for( monster &critter : g->all_monsters() ) {
        // each creature is simulated once: by the extra bubble it is in, else by the main one
        if( !m.inbounds( critter.pos_abs() ) || &bubbles::owner( critter.pos_abs() ) != &m ) {
            continue;
        }
        // a monster that walked from one bubble into the next is not simulated twice
        if( !bubbles::extras().empty() && !simulated.insert( &critter ).second ) {
            continue;
        }
        const tripoint_bub_ms critter_pos = critter.pos_bub( m );

        // Critters in impassable tiles get pushed away, unless it's not impassable for them
        if( !critter.is_dead() && ( m.impassable( critter_pos ) &&
                                    !m.get_impassable_field_at( critter_pos ).has_value() ) &&
            !critter.can_move_to( critter_pos ) ) {
            DebugLog( D_INFO, D_MAIN ) << "Multiplayer: stuck monster at " << critter.pos_abs().to_string() <<
                                       " in bubble at " << m.get_abs_sub().to_string() << " main at " <<
                                       reality_bubble().get_abs_sub().to_string() << " bubbles " << bubble_maps.size() <<
                                       " passable " << m.passable( critter_pos ) << " main passable " <<
                                       reality_bubble().passable( reality_bubble().get_bub( critter.pos_abs() ) );
            dbg( D_ERROR ) << "game:monmove: " << critter.name()
                           << " can't move to its location!  (" << critter_pos.x()
                           << ":" << critter_pos.y() << ":" << critter_pos.z() << "), "
                           << m.tername( critter_pos );
            add_msg_debug( debugmode::DF_MONSTER, "%s can't move to its location!  (%d,%d,%d), %s",
                           critter.name(),
                           critter_pos.x(), critter_pos.y(), critter_pos.z(), m.tername( critter_pos ) );
            bool okay = false;
            for( const tripoint_bub_ms &dest : m.points_in_radius( critter_pos, 3 ) ) {
                if( critter.can_move_to( dest ) && g->is_empty( dest ) ) {
                    critter.setpos( m, dest );
                    okay = true;
                    break;
                }
            }
            if( !okay ) {
                // die of "natural" cause (overpopulation is natural)
                critter.die( &m, nullptr );
            }
        }

        if( !critter.is_dead() ) {
            critter.process_turn();
        }

        m.creature_in_field( critter );
        if( calendar::once_every( 1_days ) ) {
            if( critter.has_flag( mon_flag_MILKABLE ) ) {
                critter.refill_udders();
            }
            critter.try_biosignature();
            critter.try_reproduce();
        }
        while( critter.get_moves() > 0 && !critter.is_dead() && !critter.has_effect( effect_ridden ) ) {
            critter.made_footstep = false;
            // Controlled critters don't make their own plans
            if( !critter.has_effect( effect_controlled ) ) {
                // Formulate a path to follow
                critter.plan();
            } else {
                critter.set_moves( 0 );
                break;
            }
            critter.move(); // Move one square, possibly hit u
            critter.process_triggers();
            m.creature_in_field( critter );
        }

        if( !critter.is_dead() && !critter.is_hallucination() &&
            rl_dist( u.pos_abs(), critter.pos_abs() ) < u.enchantment_cache->modify_value(
                enchant_vals::mod::MOTION_ALARM, 0 ) ) {
            if( u.has_active_bionic( bio_alarm ) ) {
                u.mod_power_level( -bio_alarm->power_trigger );
                add_msg( m_warning, _( "Your motion alarm goes off!" ) );
                g->cancel_activity_or_ignore_query( distraction_type::motion_alarm,
                                                    _( "Your motion alarm goes off!" ) );
            } else {
                add_msg( m_warning, _( "You suddenly feel alerted!" ) );
                g->cancel_activity_or_ignore_query( distraction_type::motion_alarm,
                                                    _( "Your instincts warn you for danger!" ) );
            }
            if( u.has_effect( effect_sleep ) ) {
                u.wake_up();
            }
        }
    }

    }

    g->cleanup_dead();

    // The remaining monsters are all alive, but may be outside of the reality bubble.
    // If so, despawn them. This is not the same as dying, they will be stored for later and the
    // monster::die function is not called.
    g->despawn_nonlocal_monsters();

    // Now, do active NPCs.
    for( npc &guy : g->all_npcs() ) {
        // a player's body in an extra bubble is simulated there
        std::optional<swap_map> swap_guy;
        if( !bubbles::extras().empty() ) {
            swap_guy.emplace( bubbles::map_for( guy ) );
        }
        int turns = 0;
        int real_count = 0;
        const int count_limit = std::max( 10, guy.get_moves() / 64 );
        if( guy.is_mounted() ) {
            guy.check_mount_is_spooked();
        }
        get_map().creature_in_field( guy );
        if( !guy.has_effect( effect_npc_suspend ) ) {
            guy.process_turn();
        }
        // Experimental multiplayer: an NPC played by a client does what the client sends.
        if( net::host_drive_npc( guy ) ) {
            if( !guy.is_dead() ) {
                guy.npc_update_body();
            }
            continue;
        }
        while( !guy.is_dead() && ( !guy.in_sleep_state() || guy.activity.id() == ACT_OPERATION ) &&
               guy.get_moves() > 0 && turns < 10 ) {
            const int moves = guy.get_moves();
            const bool has_destination = guy.has_destination_activity();
            guy.move();
            if( moves == guy.get_moves() ) {
                // Count every time we exit npc::move() without spending any moves.
                real_count++;
                if( has_destination == guy.has_destination_activity() || real_count > count_limit ) {
                    turns++;
                }
            }
            // Turn on debug mode when in infinite loop
            // It has to be done before the last turn, otherwise
            // there will be no meaningful debug output.
            if( turns == 9 ) {
                debugmsg( "NPC '%s' entered infinite loop, npc activity id: '%s'",
                          guy.get_name(), guy.activity.id().str() );
            }
        }

        // If we spun too long trying to decide what to do (without spending moves),
        // Invoke cognitive suspension to prevent an infinite loop.
        if( turns == 10 ) {
            add_msg( _( "%s faints!" ), guy.get_name() );
            guy.reboot();
        }

        if( !guy.is_dead() ) {
            guy.npc_update_body();
        }
    }
    g->cleanup_dead();
}

// Sounds are processed for NPCs near any player (the NPC of a client always has a player at
// distance 0).  Sounds are stored relative to the bubble they were made in, so an NPC in an
// extra bubble is processed from that bubble.
void process_npc_sounds()
{
    for( npc &guy : g->all_npcs() ) {
        const tripoint_abs_ms where = guy.pos_abs();
        if( rl_dist( where, net::nearest_player( where ).pos_abs() ) >= MAX_VIEW_DISTANCE ) {
            continue;
        }
        if( bubbles::extras().empty() ) {
            sounds::process_sound_markers( &guy );
        } else {
            swap_map swap( bubbles::map_for( guy ) );
            sounds::process_sound_markers( &guy );
        }
    }
}

void overmap_npc_move()
{
    std::vector<npc *> travelling_npcs;
    static constexpr int move_search_radius = 600;
    for( auto &elem : overmap_buffer.get_npcs_near_players( move_search_radius ) ) {
        if( !elem ) {
            continue;
        }
        npc *npc_to_add = elem.get();
        const tripoint_abs_ms npc_pos = npc_to_add->pos_abs();
        if( ( !npc_to_add->is_active() ||
              rl_dist( net::nearest_player( npc_pos ).pos_abs(), npc_pos ) > SEEX * 2 ) &&
            npc_to_add->mission == NPC_MISSION_TRAVELLING ) {
            travelling_npcs.push_back( npc_to_add );
        }
    }
    bool npcs_need_reload = false;
    for( npc *&elem : travelling_npcs ) {
        if( elem->has_omt_destination() ) {
            if( !elem->omt_path.empty() ) {
                if( rl_dist( elem->omt_path.back(), elem->pos_abs_omt() ) > 2 ) {
                    // recalculate path, we got distracted doing something else probably
                    elem->omt_path.clear();
                } else if( elem->omt_path.back() == elem->pos_abs_omt() ) {
                    elem->omt_path.pop_back();
                }
            }
            if( elem->omt_path.empty() ) {
                elem->omt_path = overmap_buffer.get_travel_path( elem->pos_abs_omt(), elem->goal,
                                 overmap_path_params::for_npc() ).points;
                if( elem->omt_path.empty() ) { // goal is unreachable, or already reached goal, reset it
                    elem->goal = npc::no_goal_point;
                }
            } else {
                elem->travel_overmap( elem->omt_path.back() );
                npcs_need_reload = true;
            }
        }
        if( !elem->has_omt_destination() && calendar::once_every( 1_hours ) && one_in( 3 ) ) {
            // travelling destination is reached/not set, try different one
            elem->set_omt_destination();
        }
    }
    if( npcs_need_reload ) {
        g->reload_npcs();
    }
}

} // namespace

// MAIN GAME LOOP
// Returns true if game is over (death, saved, quit, etc)
bool do_turn()
{
    if( g->is_game_over() ) {
        return turn_handler::cleanup_at_end();
    }

    // From here on, popups and questions must not block while time runs (see alerts.h).
    alerts::set_in_game( true );
    // Experimental multiplayer: a client only shows the host's world, it simulates nothing.
    net::start_pending();
    if( net::is_client() ) {
        return net::client_do_turn();
    }
    net::poll();
    net::dedicated_turn();
    // Experimental fluid mode: a new turn window opens (see fluid.h).
    fluid::begin_turn();

    weather_manager &weather = get_weather();
    // Actual stuff
    if( g->new_game ) {
        g->new_game = false;
        if( get_option<std::string>( "ETERNAL_WEATHER" ) != "normal" ) {
            weather.weather_override = static_cast<weather_type_id>
                                       ( get_option<std::string>( "ETERNAL_WEATHER" ) );
            weather.set_nextweather( calendar::turn );
        } else {
            weather.weather_override = WEATHER_NULL;
            weather.set_nextweather( calendar::turn );
        }
    } else {
        g->gamemode->per_turn();
        // Short days in realtime/multiplayer play (doc/MULTIPLAYER_ROADMAP.md A1).
        double world_scale = 1.0;
        const int day_minutes = get_option<int>( "REALTIME_DAY_MINUTES" );
        if( alerts::nonblocking() && day_minutes > 0 ) {
            // real seconds one turn takes
            const double turn_seconds = fluid::enabled() ? fluid::turn_ms() / 1000.0 :
                                        net::turn_duration( get_option<float>( "TURN_DURATION" ) );
            world_scale = 86400.0 / ( day_minutes * 60.0 ) * std::max( 0.05, turn_seconds );
        }
        calendar::set_world_time_scale( world_scale );
        calendar::advance_turn();
    }

    play_music( music::get_music_id_string() );

    // starting a new turn, clear out temperature cache
    weather.temperature_cache.clear();

    if( g->npcs_dirty ) {
        g->load_npcs();
    }

    timed_event_manager &timed_events = get_timed_events();
    timed_events.process();
    mission::process_all();
    avatar &u = get_avatar();
    map &m = get_map();
    // If controlling a vehicle that is owned by someone else
    if( u.in_vehicle && u.controlling_vehicle ) {
        vehicle *veh = veh_pointer_or_null( m.veh_at( u.pos_bub() ) );
        if( veh && !veh->handle_potential_theft( u, true ) ) {
            veh->handle_potential_theft( u, false, false );
        }
    }

    // If you're inside a wall or something and haven't been telefragged, let's get you out.
    if( ( m.impassable( u.pos_bub() ) && !m.impassable_field_at( u.pos_bub() ) ) &&
        !m.has_flag( ter_furn_flag::TFLAG_CLIMBABLE, u.pos_bub() ) ) {
        u.stagger();
    }

    // If riding a horse - chance to spook
    if( u.is_mounted() ) {
        u.check_mount_is_spooked();
    }
    if( calendar::once_every( 1_days ) ) {
        overmap_buffer.process_mongroups();
    }

    // Move hordes every 2.5 min
    if( calendar::once_every( time_duration::from_minutes( 2.5 ) ) ) {

        if( get_option<bool>( "WANDER_SPAWNS" ) ) {
            overmap_buffer.move_hordes();
        }
        if( u.has_trait( trait_HAS_NEMESIS ) ) {
            overmap_buffer.move_nemesis();
        }
        // Hordes that reached the reality bubble need to spawn,
        // make them spawn in invisible areas only.
        m.spawn_monsters( false );
        // ... also where they reached the bubble of another player
        for( map *bubble : bubbles::extras() ) {
            swap_map swap( *bubble );
            bubble->spawn_monsters( false );
        }
    }

    g->debug_hour_timer.print_time();

    u.update_body();

    // Auto-save if autosave is enabled
    if( get_option<bool>( "AUTOSAVE" ) &&
        calendar::once_every( 1_turns * get_option<int>( "AUTOSAVE_TURNS" ) ) &&
        !u.is_dead_state() ) {
        g->autosave();
    }

    weather.update_weather();
    g->reset_light_level();

    g->perhaps_add_random_npc( /* ignore_spawn_timers_and_rates = */ false );
    // Short days (A1/B7): activities (crafting, reading, butchering ...) progress in world
    // time like everything else, so a 30 minute job takes 30 game minutes, not 30 turns.
    const int world_step_turns = to_turns<int>( calendar::last_step() );
    // Focus minigame (B3/B5): hitting SPACE on the prompt doubles the work done for a moment.
    focus_minigame::tick( u );
    const double focus = focus_minigame::multiplier( u );
    int bonus_turns = 0;
    if( u.activity && alerts::nonblocking() ) {
        bonus_turns = static_cast<int>( std::max( world_step_turns, 1 ) * focus ) - 1;
    }
    // Most activities spend all moves in one do_turn call (and rooted ones pause), so the
    // extra world turns are handed out one turn of moves at a time: one activity turn per
    // world turn.  Otherwise pulping etc. advance only one second per real turn.
    for( int extra = 0; ; ++extra ) {
        while( u.get_moves() > 0 && u.activity ) {
            u.activity.do_turn( u );
        }
        if( extra >= bonus_turns || !u.activity ) {
            break;
        }
        u.mod_moves( u.get_speed() );
    }
    if( ( world_step_turns > 1 || focus > 1.0 ) && !u.activity && u.get_moves() > u.get_speed() ) {
        // no burst of free actions after the activity ended
        u.set_moves( u.get_speed() );
    }

    // Process NPC sound events before they move or they hear themselves talking
    process_npc_sounds();

    music::deactivate_music_id( music::music_id::sound );

    // Moddable engine: automatic item marks (junk rules), every few seconds
    if( calendar::once_every( 5_seconds ) ) {
        item_marks::auto_mark_scan( u );
    }

    // Moddable engine: a player that ran out of battery turns itself off without telling the
    // music system, so the music (and the player window) would go on.  No running player = no music.
    if( music::music_id_list[music::music_id::mp3].second &&
        !u.has_active_item( itype_mp3_on ) && !u.has_active_item( itype_smartphone_music ) &&
        !u.has_active_item( itype_afs_atomic_smartphone_music ) &&
        !u.has_active_item( itype_afs_atomic_wraitheon_music ) &&
        !u.has_active_item( itype_atomic_mp3_on ) &&
        !u.has_active_item( itype_atomic_smartphone_music ) ) {
        music::deactivate_music_id( music::music_id::mp3 );
        mod_engine::mp3_state_changed( false );
    }

    // Process sound events into sound markers for display to the player.
    sounds::process_sound_markers( &u );

    if( u.is_deaf() ) {
        sfx::do_hearing_loss();
    }

    if( !u.has_effect( effect_sleep ) || g->uquit == QUIT_WATCH ) {
        if( u.get_moves() > 0 || g->uquit == QUIT_WATCH ) {
            while( u.get_moves() > 0 || g->uquit == QUIT_WATCH ) {
                m.process_falling();
                g->cleanup_dead();
                g->mon_info_update();
                // Process any new sounds the player caused during their turn.
                process_npc_sounds();
                explosion_handler::process_explosions();
                sounds::process_sound_markers( &u );
                if( !u.activity && g->uquit != QUIT_WATCH
                    && ( !u.has_distant_destination() || calendar::once_every( 10_seconds ) ) ) {
                    g->wait_popup_reset();
                    ui_manager::redraw();
                }

                if( g->queue_screenshot ) {
                    g->take_screenshot();
                    g->queue_screenshot = false;
                }

                if( g->handle_action() ) {
                    ++g->moves_since_last_save;
                    u.action_taken();
                }

                if( g->is_game_over() ) {
                    return turn_handler::cleanup_at_end();
                }

                if( g->uquit == QUIT_WATCH ) {
                    break;
                }
                while( u.get_moves() > 0 && u.activity ) {
                    u.activity.do_turn( u );
                }
            }
            // Reset displayed sound markers now that the turn is over.
            // We only want this to happen if the player had a chance to examine the sounds.
            sounds::reset_markers();
        } else {
            // Rate limit key polling to 10 times a second.
            static auto start = std::chrono::time_point_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() );
            const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() );
            if( ( now - start ).count() > 100 ) {
                handle_key_blocking_activity();
                start = now;
            }

            g->mon_info_update();

            // If player is performing a task, a monster is dangerously close,
            // and monster can reach to the player or it has some sort of a ranged attack,
            // warn them regardless of previous safemode warnings
            if( u.activity ) {
                for( std::pair<const distraction_type, std::string> &dist : u.activity.get_distractions() ) {
                    if( g->cancel_activity_or_ignore_query( dist.first, dist.second ) ) {
                        break;
                    }
                }
            }
        }
    }

    // Experimental fluid mode: the world waits for the end of the turn window.
    fluid::finish_turn_window();

    if( g->driving_view_offset.x() != 0 || g->driving_view_offset.y() != 0 ) {
        // Still have a view offset, but might not be driving anymore,
        // or the option has been deactivated,
        // might also happen when someone dives from a moving car.
        // or when using the handbrake.
        vehicle *veh = veh_pointer_or_null( m.veh_at( u.pos_bub() ) );
        g->calc_driving_offset( veh );
    }

    // Multiplayer: every player leaves a trail that monsters can follow, not only the host.
    // Each reality bubble has its own scent grid; a player marks the one that simulates him.
    std::vector<Character *> scent_bodies = { &u };
    for( npc *other : net::remote_player_npcs() ) {
        scent_bodies.push_back( other );
    }
    // No-scent debug mutation has to be processed here or else it takes time to start working
    for( Character *body : scent_bodies ) {
        if( !body->has_flag( STATIC( json_character_flag( "NO_SCENT" ) ) ) ) {
            overmap_buffer.set_scent( body->pos_abs_omt(), body->scent );
        }
    }
    for( map *bm : bubbles::all() ) {
        swap_map sw( *bm );
        scent_map &scent = get_scent();
        const bool is_main = bm == &reality_bubble();
        std::optional<tripoint_bub_ms> centre;
        for( Character *body : scent_bodies ) {
            const tripoint_abs_ms body_abs = body->pos_abs();
            if( !bm->inbounds( body_abs ) || !bubbles::owns( *bm, body_abs ) ) {
                continue;
            }
            const tripoint_bub_ms body_pos = bm->get_bub( body_abs );
            if( !centre ) {
                centre = body_pos;
            }
            if( !body->has_flag( STATIC( json_character_flag( "NO_SCENT" ) ) ) ) {
                scent.set( body_pos, body->scent, body->get_type_of_scent() );
            }
        }
        if( is_main ) {
            centre = u.pos_bub();
        } else if( !centre ) {
            centre = tripoint_bub_ms( MAPSIZE_X / 2, MAPSIZE_Y / 2, bm->get_abs_sub().z() );
        }
        scent.update( *centre, *bm );
    }

    bubbles::follow_all();
    bubbles::sync_caches();
    // We need floor cache before checking falling 'n stuff
    m.build_floor_caches();

    m.process_falling();
    m.vehmove();
    bubbles::sync_vehicle_caches();
    m.process_fields();
    m.process_items();
    explosion_handler::process_explosions();
    m.creature_in_field( u );

    // Apply sounds from previous turn to monster and NPC AI.
    sounds::process_sounds();
    const int levz = m.get_abs_sub().z();
    // Update vision caches for monsters. If this turns out to be expensive,
    // consider a stripped down cache just for monsters.
    m.build_map_cache( levz, true );
    bubbles::tick();
    fluid::snapshot_before_monsters();
    monmove();
    fluid::snapshot_after_monsters();
    if( calendar::once_every( time_between_npc_OM_moves ) ) {
        overmap_npc_move();
    }
    if( calendar::once_every( 10_seconds ) ) {
        for( const tripoint_bub_ms &elem : m.get_furn_field_locations() ) {
            const furn_t &furn = *m.furn( elem );
            for( const emit_id &e : furn.emissions ) {
                m.emit_field( elem, e );
            }
        }
        for( const tripoint_bub_ms &elem : m.get_ter_field_locations() ) {
            const ter_t &ter = *m.ter( elem );
            for( const emit_id &e : ter.emissions ) {
                m.emit_field( elem, e );
            }
        }
    }
    g->mon_info_update();
    u.process_turn();
    if( u.get_moves() < 0 && get_option<bool>( "FORCE_REDRAW" ) ) {
        ui_manager::redraw();
        refresh_display();
    }

    if( levz >= 0 && !u.is_underwater() ) {
        handle_weather_effects( weather.weather_id );
    }

    const bool player_is_sleeping = u.has_effect( effect_sleep );
    bool wait_redraw = false;
    std::string wait_message;
    time_duration wait_refresh_rate;
    if( player_is_sleeping ) {
        wait_redraw = true;
        wait_message = _( "Wait till you wake up…" );
        wait_refresh_rate = 30_minutes;
    } else if( const std::optional<std::string> progress = u.activity.get_progress_message( u ) ) {
        wait_redraw = true;
        wait_message = *progress;
        if( u.activity.is_interruptible() && u.activity.interruptable_with_kb ) {
            wait_message += string_format( _( "\n%s to interrupt" ), press_x( ACTION_PAUSE ) );
        }
        if( u.activity.id() == ACT_AUTODRIVE ) {
            wait_refresh_rate = 1_turns;
        } else if( u.activity.id() == ACT_FIRSTAID ) {
            wait_refresh_rate = 5_turns;
        } else {
            wait_refresh_rate = 5_minutes;
        }
    }
    if( wait_redraw ) {
        if( g->first_redraw_since_waiting_started ||
            calendar::once_every( std::min( 1_minutes, wait_refresh_rate ) ) ) {
            if( g->first_redraw_since_waiting_started || calendar::once_every( wait_refresh_rate ) ) {
                ui_manager::redraw();
            }

            // Avoid redrawing the main UI every time due to invalidation
            ui_adaptor dummy( ui_adaptor::disable_uis_below {} );
            if( !g->wait_popup ) {
                g->wait_popup = std::make_unique<static_popup>();
            }
            g->wait_popup->on_top( true ).wait_message( "%s", wait_message );
            ui_manager::redraw();
            refresh_display();
            g->first_redraw_since_waiting_started = false;
        }
    } else {
        // Nothing to wait for now
        g->wait_popup_reset();
        g->first_redraw_since_waiting_started = true;
    }

    m.invalidate_visibility_cache();

    u.update_bodytemp();
    u.update_body_wetness( *weather.weather_precise );
    u.apply_wetness_morale( weather.temperature );

    if( calendar::once_every( 1_minutes ) ) {
        u.update_morale();
        for( npc &guy : g->all_npcs() ) {
            guy.update_morale();
            guy.check_and_recover_morale();
        }
    }

    if( calendar::once_every( 9_turns ) ) {
        u.check_and_recover_morale();
    }

    if( !u.is_deaf() ) {
        sfx::remove_hearing_loss();
    }
    sfx::do_danger_music();
    sfx::do_vehicle_engine_sfx();
    sfx::do_vehicle_exterior_engine_sfx();
    sfx::do_low_stamina_sfx();

    // reset player noise
    u.volume = 0;

    // Calculate bionic power balance
    u.power_balance = u.get_power_level() - u.power_prev_turn;
    u.power_prev_turn = u.get_power_level();

    // B3: crafts left on workbenches keep working
    stations::process();
    net::host_end_of_turn();
    net::pace_turn();

#if defined(EMSCRIPTEN)
    // This will cause a prompt to be shown if the window is closed, until the
    // game is saved.
    EM_ASM( window.game_unsaved = true; );
#endif

    return false;
}
