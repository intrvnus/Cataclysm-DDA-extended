#pragma once
#ifndef CATA_SRC_NET_SESSION_H
#define CATA_SRC_NET_SESSION_H

#include <optional>
#include <string>

#include "coordinates.h"
#include <vector>

#include "fluid.h"

class Character;
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
bool dedicated();
/** Dedicated server: once per turn, keeps the ghost avatar with the players. */
void dedicated_turn();
void request_connect( const std::string &address, int port );

/** Starts the requested session (once). Called from the main game loop. */
void start_pending();
/** Closes all connections. */
void shutdown();
/** Handles network traffic. Cheap, call often (game loop, input wait loops). */
void poll();

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
/** A4: saves the characters of the connected players (called when the host saves). */
void host_save_players();
/** B6: true while every player (host avatar and all clients) sleeps: time may then run as
 * fast as the machine allows until somebody wakes up. */
bool everyone_asleep();
/** Is @p who played by a connected client? */
bool is_remote_player( const Character &who );
/** Warns the player who controls @p who (host avatar: locally; remote player: on their
 * client) with the alerts system (sound, coloured message, arrow to @p source). */
void alert_player( const Character &who, const std::string &text, int level,
                   const std::optional<tripoint_abs_ms> &source );
/** The NPCs on the host that are played by connected clients (in the reality bubble). */
std::vector<npc *> remote_player_npcs();
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
/** Fluid mode: sends the input of this frame (only if something changed, or now and then). */
void client_send_input( const fluid::input_frame &in );

} // namespace net

#endif // CATA_SRC_NET_SESSION_H
