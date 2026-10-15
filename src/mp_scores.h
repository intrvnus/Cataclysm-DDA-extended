#pragma once
#ifndef CATA_SRC_MP_SCORES_H
#define CATA_SRC_MP_SCORES_H

#include <string>

class Character;

/**
 * C21 scores per player (Noah 2026-10-15).  The game's own kill list and achievements count
 * for the host's character only; this counts for every player, on the player's own character
 * (values, saved with it): monsters killed, people killed, skill levels gained, lives lost.
 * "/stats" in the chat shows everybody present; a player who runs out of lives gets a line in
 * the world's memorial (mp_players/memorial.txt).
 */
namespace mp_scores
{
/** Host: listen to the game's events (once). */
void start();
/** The scoreboard of everybody present. */
std::string scoreboard();
/** @p who is gone for good (no life left): their line in the memorial. */
void memorial( const Character &who );
} // namespace mp_scores

#endif // CATA_SRC_MP_SCORES_H
