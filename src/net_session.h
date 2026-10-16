#pragma once
#ifndef CATA_SRC_NET_SESSION_H
#define CATA_SRC_NET_SESSION_H

#include <optional>
#include <string>

#include "coordinates.h"
#include <vector>

#include "fluid.h"

class Character;
class monster;
class npc;

/**
 * Experimental multiplayer (milestone 1).
 *
 * One game instance is the authoritative host: it runs the simulation as usual and
 * every connected client is represented on the host by an NPC that is driven by the
 * commands that client sends (instead of the NPC AI).
 * A client runs a normal game (any world with the same mods) only as a display:
 * it does not simulate anything, it shows the submaps, creatures and time it gets
 * from the host and sends its key presses (move/attack, wait, pick up) to the host.
 *
 * Time flows continuously while a session is active (TURN_DURATION is forced on),
 * so nobody waits for anyone else's input.
 *
 * Started from the command line:
 *   --mp-host [port]            host a session once the game is loaded
 *   --mp-connect <addr[:port]>  join a session once the game is loaded
 */
class Creature;
class item_location;

namespace net
{

constexpr int default_port = 7777;

enum class role : int {
    none,
    host,
    client
};

/** Command line: remember what to start once a game is running. */
void request_host( int port );
/** Command line: dedicated server (A2): host without playing; the avatar becomes an
 * invisible, invulnerable ghost that follows the players and never waits for input. */
void request_dedicated( int port );
/** Server: how many players may be connected at once (--mp-max-players, 1..32, default 4). */
void set_max_players( int n );
int max_players();
/** Starts another copy of this game (same user folder) with @p args; false if it failed. */
bool launch_game( const std::vector<std::string> &args );
/** The address of this computer in the local network (empty if unknown). */
std::string local_address();
bool dedicated();
/** A dedicated server was asked for on the command line (before the game is loaded). */
bool dedicated_requested();
/** Dedicated server: once per turn, keeps the ghost avatar with the players. */
void dedicated_turn();
void request_connect( const std::string &address, int port );

/** Command line --mp-host: host = server + client (A2).  See net_listen.cpp. */
void request_listen( int port );
bool listen_requested();
/** Starts the server for @p world in the background and makes this process its client (same
 * game folder; this game only shows the world, the server owns it).  False if that is not
 * possible (then host the old way). */
bool prepare_listen( const std::string &world, const char *argv0 );
/** The server: @p world is saved and complete, the host's own game can show it. */
void mark_world_ready( const std::string &world );
bool world_ready( const std::string &world );
/** This process is the server started by prepare_listen (--mp-listen-child). */
void set_listen_child();
bool listen_child();
/** The server process (dedicated or a host's background server): a terminal window of its own
 * that shows the log (Windows; elsewhere the log goes to the terminal that started it). */
void open_server_console();
/** The server's terminal window was closed: save and stop. */
bool console_stop_requested();
/** The server saved after its window was closed: the window may close now. */
void console_stop_done();
/** This process is the host's own client (started with --mp-host). */
bool listen_local();
/** Why the last session ended by itself (the host left, the connection failed), once; else empty. */
std::string take_session_end_notice();
/** Forgets the client side of the last session (leaving, before hosting or joining again). */
void client_reset();
/** The server process this game started is still running (saving after the host left). */
bool listen_server_running();
/** The host's game gave up before joining: its server stops, the hosting state is cleared. */
void listen_cancel();
/** The server: its host's game gave up (listen_cancel). */
bool listen_stop_requested();
/** The host's session is over (its server ended): back to the main menu. */
void listen_end();
/** The server joined was just started from this computer: retry while it loads its world. */
void retry_join_while_loading();
/** The client could not reach the server: true if it tries again soon (the host's own server or
 * one just started on this computer may still be loading). */
bool listen_connect_failed();
/** Once per frame: a planned new connection attempt of the host's own client. */
void listen_tick();
/** This player's identity key (config/mp_identity.txt, made if missing). */
std::string client_key();

/** Starts the requested session (once). Called from the main game loop. */
void start_pending();
/** Closes all connections. */
void shutdown();
/** Handles network traffic. Cheap, call often (game loop, input wait loops). */
void poll();

/** Bytes of JSON handed to the network so far (before compression), for the traffic log. */
extern uint64_t raw_bytes_sent;
/** Bytes the host has put on the wire so far (after compression), 0 when not hosting. */
uint64_t wire_bytes_sent();

role current_role();
inline bool is_host()
{
    return current_role() == role::host;
}
inline bool is_client()
{
    return current_role() == role::client;
}
inline bool active()
{
    return current_role() != role::none;
}

/** Host: makes a turn last at least the realtime turn length (activities would otherwise
 * run the world as fast as the machine allows).  Keeps the network and the screen alive. */
void pace_turn();
/** Seconds per turn while waiting for input; @p option is the TURN_DURATION setting. */
float turn_duration( float option );

// ---- host ----
/** Called for every NPC in monmove(). If it is a remote player, executes its queued
 * commands (or lets it wait) and returns true; the NPC AI must then be skipped.
 * In fluid mode (see fluid.h) the remote players are driven by host_drive_remote_players()
 * between the turns, here they only pass the turn. */
bool host_drive_npc( npc &guy );
/** Fluid mode: runs one frame of input for every remote player. Called by fluid::host_frame(). */
void host_drive_remote_players( fluid::ms_t now, std::vector<fluid::event> *events );
/** Fluid mode: tells the clients what happened (steps, blows ...). */
void host_send_events( const std::vector<fluid::event> &events );
/** Called by npc::die(): a player of a client goes down instead of dying (C1).  Returns true
 * if the death was prevented. */
bool host_prevent_death( npc &guy );
/** Walking into a downed player helps them up.  Returns true if @p p held a downed player
 * (the step is used up by helping). */
bool host_try_revive( Character &helper, const tripoint_abs_ms &p );
/** Client: connects again to the host it was connected to (after making a new character). */
void request_reconnect();
/** Called by game::is_game_over(): the host's own character goes down instead of dying, like a
 * client's (C1).  Returns true if the death was prevented. */
bool host_prevent_avatar_death();
/** True while the host's own character is down (it cannot act). */
bool host_avatar_downed();
/** A4: saves the characters of the connected players (called when the host saves). */
void host_save_players();
/** B6: true while every player (host avatar and all clients) sleeps: time may then run as
 * fast as the machine allows until somebody wakes up. */
bool everyone_asleep();
/** Is @p who played by a connected client? */
bool is_remote_player( const Character &who );
/** A plain message for the player who controls @p who (host avatar: the message log; remote
 * player: their client's log). */
void tell_player( const Character &who, const std::string &text, bool good );
/** Warns the player who controls @p who (host avatar: locally; remote player: on their
 * client) with the alerts system (sound, coloured message, arrow to @p source). */
void alert_player( const Character &who, const std::string &text, int level,
                   const std::optional<tripoint_abs_ms> &source );
/** B7: characters doing a timed action and how far along they are (0..1), for the progress
 * bars above them.  Clients: from the host's state; otherwise from the characters here. */
std::vector<std::pair<tripoint_abs_ms, double>> activity_bars();
/** 0..1000 progress of @p who's timed action, -1 if none (or no known length). */
int activity_progress( const Character &who );
/** B6: the player of this computer is asleep (client: its body on the host). */
bool own_asleep();
/** C13 music: the body of a client has music going (its mp3 player, its instrument, music it
 * hears): the client plays that track (music::music_id) while these keep coming. */
void music_for( const Character &who, int music_id );
/** C13: a sound effect the body of a client hears is played on that client's computer, from
 * where it came (volume and direction from the client's own position). */
void play_sound_for( const Character &who, const std::string &id, const std::string &variant,
                     const std::string &season, bool indoors, bool night,
                     const tripoint_abs_ms &source );
/** The player whose character is closest to @p from (the host's avatar or the body of a
 * client).  Monsters use it where the single player used to be "the" player.  Without a
 * session this is the avatar. */
Character &nearest_player( const tripoint_abs_ms &from );
/**
 * A player's body walks into another person: for an NPC that is not an enemy the player gets
 * the NPC menu (talk, trade, examine ...), like walking into one in the single-player game.
 * Returns true if the step was used for that.  Throttled so that a held key opens one menu.
 */
bool bump_npc( npc &guy, npc &other );
/** Host: @p who (a client's body) starts a conversation with @p other on its own screen.
 * False if @p who is not a client's body (then the caller talks the normal way). */
bool talk_to_npc( Character &who, npc &other );
/** Host: @p who (a client's body) goes to @p dest far away (translocators): it is stored there
 * at the end of the turn and comes back in a reality bubble of its own.  False if @p who is
 * not a client's body. */
bool move_body_far( Character &who, const tripoint_abs_ms &dest );
/** The same with a monster that can talk. */
bool talk_to_monster( Character &who, monster &other );
/** Where the players are: the host's avatar first, then the body of every connected client.
 * World simulation that used to be anchored on the avatar (hordes, overmap NPCs, spawns)
 * runs around each of these.  Without a session this is just the avatar. */
std::vector<tripoint_abs_ms> all_player_positions();
/** Host: says something to everybody (chat). */
void host_chat( const std::string &text );
/** The list of the players with health and distance, for the chat/player-list key. */
std::string player_list_text( const Character &viewer );
/** The NPCs on the host that are played by connected clients (in the reality bubble). */
std::vector<npc *> remote_player_npcs();
/** Host: a server started on a world without characters keeps the world's time still until
 * the first player's character has joined (remembered with the world). */
bool waiting_for_first_player();
/** Called once at the end of every turn: handles joins and sends the world state. */
void host_end_of_turn();

// ---- client ----
/** Replaces the whole turn on a client. Returns true if the game is over. */
bool client_do_turn();

/** Host: true while @p who is a player who is talking to an NPC: such a player cannot be
 * seen, targeted or hurt (a conversation must not get anyone killed). */
bool dialogue_protected( const Creature &who );

/** Client: the item menu of the inventory screen chose @p key (the menu's hotkey) for
 * @p loc: the action is sent to the host.  False if the choice is only a view or a setting
 * that is done locally. */
bool client_item_action( int key, const item_location &loc );
/** Fluid mode: sends the input of this frame (only if something changed, or now and then).
 * Returns the sequence number of the frame sent, or -1 if nothing was sent. */
int client_send_input( const fluid::input_frame &in );
/** Called while the game waits for a key.  Multiplayer client inside a menu: keeps hearing the
 * host (messages are queued until the menu closes) and returns true when danger appeared (own
 * hit points went down, a hostile monster next to the own character): the menu backs out. */
bool input_idle();
/** Host: a client's body pushed into a ledge: that client opens the climb-down menu (examines
 * the tile in direction @p dx, @p dy) on its own screen. */
void request_climb( const Character &body, int dx, int dy );
/** Client, free movement: puts the shadow of another player on @p tile (from the
 * interpolated body position). */
void client_place_shadow( int actor, const tripoint_abs_ms &tile );

} // namespace net

#endif // CATA_SRC_NET_SESSION_H
