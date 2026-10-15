#pragma once
#ifndef CATA_SRC_MP_MENU_H
#define CATA_SRC_MP_MENU_H

/**
 * The "Multiplayer" entry of the main menu (Noah 2026-10-15): host a game (and play), start a
 * dedicated server, or join a game, without typing command lines.  The choices (world, port,
 * player limit, the world's multiplayer rules, the last address joined) are remembered in
 * config/mp_menu.txt.  Starting a session starts the game again with the right options (a server
 * runs in a window of its own).
 */
namespace mp_menu
{
/** Shows the menu.  True if this game must close now (a new copy of it took over). */
bool open();
} // namespace mp_menu

#endif // CATA_SRC_MP_MENU_H
