#pragma once
#ifndef CATA_SRC_DEVICE_UI_H
#define CATA_SRC_DEVICE_UI_H

class Character;
class item_location;

/**
 * Moddable engine: one clear window for phones, laptops and other e-devices.
 *
 * It replaces the chain of menus of the "E_FILE_DEVICE" use action: the devices the player has
 * or can reach are listed on the left, the files of the picked device on the right (with check
 * boxes), and buttons below do the work (browse, read, copy, move, wipe).  The apps of the device
 * (music, camera, ...) are buttons at the top.  The real work is done by the normal
 * efile_activity_actor, so the game rules do not change.
 */
namespace device_ui
{
enum class result {
    done,    // the window did what the player wanted
    classic  // the player asked for the old menus
};

result run( Character &who, item_location &used_edevice );
} // namespace device_ui

#endif // CATA_SRC_DEVICE_UI_H
