#pragma once
#ifndef CATA_SRC_MOD_ENGINE_H
#define CATA_SRC_MOD_ENGINE_H

#include <optional>
#include <string>

class JsonObject;
class input_context;

/**
 * "Moddable engine" extras: things that let JSON mods do more than the vanilla game allows.
 *
 *  - realtime_eoc : effect_on_conditions that run every N real-time seconds while the game
 *                   waits for the player's input.
 *  - mod_window   : small floating windows with live text and clickable buttons that the
 *                   player can drag around.
 *  - mod_option   : options of a mod (on/off, number, list) that the player can change in the
 *                   in-game "Mod options" menu; saved in the config folder.
 *  - text templates ("{music_track}", "{var:name}", ...) used by windows and sidebar widgets.
 *  - a tiny sandboxed file store (config/mod_data) that mods can write to and read from.
 *
 * See doc/JSON/MODDABLE_ENGINE.md.
 */
namespace mod_engine
{
/** Forget everything loaded from mods and close all windows (called when data is unloaded). */
void reset();

/** Close all windows right now (the game is going back to the main menu). Which windows were
 *  open stays remembered, so they come back in the next game. */
void shutdown_windows();

// ---- JSON loaders ("realtime_eoc", "mod_window", "mod_option") ----
void load_realtime_eoc( const JsonObject &jo, const std::string &src );
void load_window( const JsonObject &jo, const std::string &src );
void load_option( const JsonObject &jo, const std::string &src );

/** Called repeatedly (about 8 times a second) while the game waits for input. */
void tick();

/**
 * Called for mouse actions ("CLICK_AND_DRAG" = button pressed, "SELECT" = left button released,
 * "SEC_SELECT" = right click, "MOUSE_MOVE").  Returns true if a mod window used the event, in
 * which case the game must not handle it any more.
 */
bool handle_mouse( input_context &ctxt, const std::string &action );

// ---- windows ----
/** commands: "open", "close", "toggle", "close_all", "reset_position". The id is ignored for close_all. */
void window_command( const std::string &command, const std::string &id );
bool window_is_open( const std::string &id );
/** The mp3 player was switched on / off: windows with "show_while": "mp3" open / close. */
void mp3_state_changed( bool on );

// ---- albums of the mp3 player ----
/** Menu of the albums the player carries; sets the one picked.  True if an album was picked. */
bool choose_album();
/** Switch to the next (direction 1) or previous (-1) album the player carries. */
void cycle_album( int direction );

// ---- text templates ----
/** Replace the {placeholders} of a text by their current values. */
std::string expand_text( const std::string &text );

// ---- options ----
bool has_options();
bool option_exists( const std::string &id );
/** Current value as text ("true"/"false" for switches).  Empty if the option does not exist. */
std::string option_get( const std::string &id );
/** "bool", "int", "list" or "text"; empty if the option does not exist. */
std::string option_kind( const std::string &id );
/** Current value as number (switches: 1 or 0; text and lists: 0). */
double option_get_number( const std::string &id );
/** Returns false if the option does not exist or the value is not allowed. */
bool option_set( const std::string &id, const std::string &value );
/** Menu that lists all mod options and lets the player change them. */
void options_menu();

// ---- files (always inside config/mod_data, nothing else can be reached) ----
bool file_write( const std::string &name, const std::string &text, bool append );
std::optional<std::string> file_read( const std::string &name );
} // namespace mod_engine

#endif // CATA_SRC_MOD_ENGINE_H
