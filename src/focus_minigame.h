#pragma once
#ifndef CATA_SRC_FOCUS_MINIGAME_H
#define CATA_SRC_FOCUS_MINIGAME_H

class Character;

/**
 * Focus minigame for long timed actions in realtime / multiplayer play
 * (doc/MULTIPLAYER_ROADMAP.md B3, B5): reading, martial arts books, crafting.
 *
 * Every few seconds the player gets a "Focus!" prompt (sound + message).  Pressing SPACE in
 * time makes the action go twice as fast for a few seconds.  Missing it costs nothing, so
 * the action still finishes on its own; playing along makes it finish sooner.
 */
namespace focus_minigame
{

/** Host: once per turn for every player doing something. */
void tick( Character &who );
/** The player pressed the focus key. */
void press( Character &who );
/** 2.0 while the focus bonus lasts, else 1.0. */
double multiplier( const Character &who );

} // namespace focus_minigame

#endif // CATA_SRC_FOCUS_MINIGAME_H
