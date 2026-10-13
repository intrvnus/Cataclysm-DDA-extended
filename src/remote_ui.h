#pragma once
#ifndef CATA_SRC_REMOTE_UI_H
#define CATA_SRC_REMOTE_UI_H

#include <list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Character;
class item_location;

/**
 * Menus of multiplayer clients (doc/MULTIPLAYER_ROADMAP.md A6).
 *
 * A client does an action (apply an item, examine a door ...) on its own copy of its character,
 * with the usual menus.  Every answer given in a menu is recorded.  The host then does the same
 * action on the player's body with those answers: no menu opens on the host.  If the host's
 * action asks something the client was not asked (the world was different), the action is
 * cancelled at that point.
 *
 * The menus that take part call record() after they were answered, and answer() instead of
 * showing anything while answers are replayed.
 */
namespace remote_ui
{

/** One answer: what kind of question (e.g. "u" for a uilist) and the answer, as text. */
using entry = std::pair<std::string, std::string>;

bool recording();
bool replaying();

/** Client: answers given while this lives are recorded.  With @p capture_activities the
 * activities the action assigns to the own character are not started here but kept (as JSON):
 * the host assigns them to the body (advanced inventory ...). */
class record_scope
{
    public:
        explicit record_scope( bool capture_activities = false );
        ~record_scope();
        record_scope( const record_scope & ) = delete;
        record_scope &operator=( const record_scope & ) = delete;
        std::vector<entry> answers() const;
        std::vector<std::string> activities() const;
};

/** True while activities are captured instead of started (see record_scope). */
bool capturing();
/** Keeps a serialized activity of the own character. */
void capture_activity( const std::string &json );
/** Item locations in @p json that name character @p from_id name @p to_id instead. */
std::string remap_character_id( std::string json, int from_id, int to_id );

/** Client: answers of menus that open while this lives are not recorded (the caller records
 * one answer for the whole thing). */
class mute_scope
{
    public:
        mute_scope();
        ~mute_scope();
        mute_scope( const mute_scope & ) = delete;
        mute_scope &operator=( const mute_scope & ) = delete;
};

/** Host: menus are answered from @p answers while this lives. */
class replay_scope
{
    public:
        /** @p actor is the player's body: menus that ask "where?" measure from it. */
        /** @p client_id is the id of the character on the client (items it carries are the
         * body's). */
        replay_scope( std::vector<entry> answers, Character &actor, int client_id );
        ~replay_scope();
        replay_scope( const replay_scope & ) = delete;
        replay_scope &operator=( const replay_scope & ) = delete;
        /** True if a menu asked something that had no answer (the action was cancelled). */
        bool failed() const;
        /** Texts of popups and messages meant for the player during the replay. */
        std::vector<std::string> texts() const;
};

/** While replaying: the body of the player whose action it is, else nullptr. */
Character *actor();

/** The character doing the action: the player's body while replaying, else the player. */
Character &acting_player();

/** True for the player, and while replaying for the body of the player whose action it is:
 * code that treats NPCs differently (they skip menus, get no messages ...) treats a player's
 * body like the player. */
bool acts_as_player( const Character &who );

/** A menu was answered (while recording). */
void record( const std::string &kind, const std::string &value );
/** The recorded answer to a menu of @p kind, or nothing (then the menu must cancel). */
std::optional<std::string> answer( const std::string &kind );
/** While replaying: a popup or message for the player instead of the host's screen. */
void tell( const std::string &text );

/** Item locations chosen in a menu, as text (the client's own items become the body's). */
std::string location_to_string( const item_location &loc );
item_location location_from_string( const std::string &text );
std::string locations_to_string( const std::list<std::pair<item_location, int>> &locs );
std::list<std::pair<item_location, int>> locations_from_string( const std::string &text );

/** Serialization of the answers for the network. */
std::string to_json( const std::vector<entry> &answers );
std::vector<entry> from_json( const std::string &json );

} // namespace remote_ui

#endif // CATA_SRC_REMOTE_UI_H
