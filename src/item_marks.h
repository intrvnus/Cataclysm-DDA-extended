#pragma once
#ifndef CATA_SRC_ITEM_MARKS_H
#define CATA_SRC_ITEM_MARKS_H

#include <string>
#include <vector>

#include "item_location.h"
#include "type_id.h"

class avatar;
class Character;
class JsonObject;
class item;

/**
 * Moddable engine: item marks (json_flag with "user_mark") - toggling, automatic marking by rules,
 * and dropping everything that carries a mark.  See doc/JSON/MODDABLE_ENGINE.md.
 */
namespace item_marks
{
/** JSON type "item_mark_rule": marks items automatically while the player carries them. */
void load_rule( const JsonObject &jo, const std::string &src );
/** Forget all loaded rules (called when the loaded mods change). */
void reset();

/** Look at everything the avatar carries and apply the automatic rules.  Cheap; called often. */
void auto_mark_scan( Character &who );

/** Switch a mark on or off on every item of a stack (remembers that the player did it by hand). */
void toggle_mark( std::vector<item_location> &stack, const flag_id &mark );

/** Drop every carried item (not worn, not wielded) that has the mark, after asking the player. */
void drop_marked( avatar &you, const flag_id &mark );
} // namespace item_marks

#endif // CATA_SRC_ITEM_MARKS_H
