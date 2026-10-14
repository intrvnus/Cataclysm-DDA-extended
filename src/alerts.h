#pragma once
#ifndef CATA_SRC_ALERTS_H
#define CATA_SRC_ALERTS_H

#include <optional>
#include <string>

#include "coordinates.h"

/**
 * Warnings that never stop time (multiplayer / realtime play).
 *
 * In turn-based single player nothing changes: popups and "Stop doing X?" questions work as
 * before.  While the world runs in real time (a multiplayer session or fluid mode), they would
 * freeze the game for everybody, so instead the player gets
 *   - a message in the sidebar log in its own colour (m_alert),
 *   - a short warning sound,
 *   - for threats: an arrow at the edge of the screen pointing at the source.
 * A yes/no question becomes "do it again to confirm": the first time only warns, repeating the
 * same action within a few seconds answers yes.
 */
namespace alerts
{

enum class level : int {
    // something worth noticing (item, noise)
    notice,
    // something that wants an answer or may be dangerous
    warning,
    // a threat right now (hostile spotted, turret targeting you)
    danger
};

/** True while popups and time stops must not block (in game, realtime or multiplayer). */
bool nonblocking();
/** Set by the game loop: true while a game is running, false in menus/character creation. */
void set_in_game( bool in_game );

/** Message + sound (+ direction arrow if @p source is given). Repeats are throttled. */
void warn( const std::string &text, level lvl = level::warning,
           const std::optional<tripoint_abs_ms> &source = std::nullopt );

/**
 * Non-blocking replacement for a yes/no question.  Returns false and warns the first time;
 * returns true when the same question comes again within a few seconds.
 */
bool confirm( const std::string &question );

/**
 * True when a threat is close: a visible hostile creature within a short range, a laser
 * lock or a dangerous field on the player's tile.  Decides whether a warning makes a sound.
 */
bool danger_nearby();

/** Plays the warning sound for @p lvl (no message). */
void play_sound( level lvl );

/** The newest threat direction for the edge-of-screen arrow, if still recent. */
std::optional<tripoint_abs_ms> recent_threat( int max_age_ms = 2500 );

} // namespace alerts

#endif // CATA_SRC_ALERTS_H
