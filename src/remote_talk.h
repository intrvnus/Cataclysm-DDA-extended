#pragma once
#ifndef CATA_SRC_REMOTE_TALK_H
#define CATA_SRC_REMOTE_TALK_H

#include <memory>
#include <string>
#include <vector>

class npc;

/**
 * A conversation of a multiplayer player with an NPC, run turn by turn on the host.
 *
 * The normal conversation (avatar::talk_to) waits for keys inside a window.  The host cannot
 * wait: this class does the same work in two steps.  turn() says what the NPC says and which
 * answers there are (that is sent to the player's computer, which shows the usual dialogue
 * window); choose() carries out the answer the player picked.  Nothing is shown on the host.
 * Windows that an answer would open (trading, training ...) are not opened; see
 * remote_ui.h and request_trade().
 */
class remote_conversation
{
    public:
        struct response {
            int color = 0;
            std::string hotkey_desc;
            std::string text;
            int key = 0;
            bool enabled = true;
            // 0 nothing, 1 "You may be attacked!", 2 "You'll be helpless!"
            int consequence = 0;
        };
        struct turn_data {
            /** {"add", text, color} and {"clr"}, see dialogue_window::journal */
            std::vector<std::vector<std::string>> history;
            std::vector<response> responses;
            std::string npc_name;
        };

        /** @p you is the body of the player.  started() is false if the NPC does not want to talk. */
        remote_conversation( npc &you, npc &other );
        ~remote_conversation();
        remote_conversation( const remote_conversation & ) = delete;
        remote_conversation &operator=( const remote_conversation & ) = delete;

        bool started() const;
        bool finished() const;
        /** The next thing the NPC says; false if the conversation is over. */
        bool turn( turn_data &out );
        /** The player picked answer @p index (-1: wants to leave the conversation). */
        void choose( int index );
        /** One of the side-column commands (look at, size up, assess, check opinion, yell). */
        void choose_special( const std::string &action );
        int other_id() const;

    private:
        struct impl;
        std::unique_ptr<impl> p;
};

#endif // CATA_SRC_REMOTE_TALK_H
