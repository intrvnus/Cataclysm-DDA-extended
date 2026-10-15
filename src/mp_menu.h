#pragma once
#ifndef CATA_SRC_MP_MENU_H
#define CATA_SRC_MP_MENU_H

#include <string>

/**
 * The "Multiplayer" entry of the main menu (Noah 2026-10-15): host a game (and play), start a
 * dedicated server, or join a game, without typing command lines.  The choices (world, port,
 * player limit, the world's multiplayer rules, the last address joined) are remembered in
 * config/mp_menu.txt.  Hosting and joining happen in this game; a server
 * runs as a process of its own with a terminal window.
 */
namespace mp_menu
{
/** Shows the menu.  The world this game loads now (hosting and playing, or joining: the session
 * is already set up), or empty (nothing to load: back to the main menu). */
std::string open();
} // namespace mp_menu

#endif // CATA_SRC_MP_MENU_H
