# Moddable engine additions (feature level 5)

Everything here only exists in the custom "moddable engine" build.  A mod can declare
`"requires_engine": 5` (or lower) in its `MOD_INFO`; builds with a lower level refuse to load such a mod and say why.

## Item marks (`json_flag`)

A `json_flag` gets four optional fields:

| field | meaning |
|---|---|
| `user_mark` (bool) | the player can toggle this flag on items from inventory menus (key `#`, rebindable: "Toggle item mark") |
| `sort_priority` (int) | items with the flag are sorted higher (bigger number) or lower (negative) in inventory menus |
| `protect_from_drop` (bool) | the "select all non-favorite items" key (`,`) in multi-select menus skips these items |
| `guard` (bool) | level 5: the game asks before you drop, throw, disassemble or sell an item with this flag; crafting and disassembly avoid it (a craft that has to use it asks first); bulk "move all" / "move everything" actions (vehicle cargo, containers, the device window's "Drop" helpers) leave it alone |
| `auto_pickup` (`"pickup"` or `"ignore"`) | level 4: with autopickup on, items carrying the flag are always (`"pickup"`) or never (`"ignore"`) picked up, whatever the autopickup rules say |
| `name` / `item_prefix` / `item_suffix` / `info` | existing fields: menu label, symbol before/after the item name, tooltip |

Example:
```json
{ "type": "json_flag", "id": "MARK_KEEP", "name": "Keep", "item_prefix": "<color_light_green>[K]</color> ",
  "user_mark": true, "protect_from_drop": true, "sort_priority": 10 }
```
Colors: `item_prefix` / `item_suffix` may contain color tags as in the example, so a mark can have its own colored symbol.
Searching by mark: in every item filter (the `/` key) `f:MARK_KEEP` lists the items that carry the flag.
Marks are stored in the item's flags (nothing is added to the save format).  Marks never assign inventory letters.

**Quick keys and bulk actions (level 5)**
* `mod_action` with `"context": "INVENTORY"` and `"toggle_mark": "<flag id>"` (instead of an `eoc`): the key marks / unmarks the highlighted item
  (the whole stack) in any inventory menu, without opening the mark menu.
* `mod_action` with `"context": "DEFAULTMODE"` and `"drop_marked": "<flag id>"`: drops every item carrying the flag (not worn, not wielded,
  not guarded) after one confirmation.
* `item_mark_rule`: marks items automatically (about every 5 seconds, only items that are carried).  A mark that the player removed by hand
  is never set again on that item, and a mark an automatic rule set is taken away again when the item stops matching (refilled, dried, repaired).
```json
{ "type": "item_mark_rule", "id": "MYMOD_RULE", "mark": "MARK_JUNK",
  "option": "mymod.auto_junk",          // optional bool mod_option; the rule only runs while it is on (default true if the option does not exist)
  "broken": true,                       // item is broken / at 0 hp
  "armor_only": false,                  // only clothes and armor
  "empty_container": false,             // only containers that hold nothing
  "has_flag": [ "WET" ],                // any of these item flags
  "item_ids": [ "can_drink", "cup_plastic" ] }   // any of these item types
```
All given conditions must hold for a rule to mark an item; `has_flag` and `item_ids` each match if any listed entry matches.

New option, Interface: **Sort favorites first** (`INV_FAVORITES_FIRST`, default true).  Off = favorites no longer jump to the top.

## `mod_action`: hotkeys that run an effect on condition

```json
{ "type": "mod_action", "id": "MYMOD_HELLO", "name": "Say hello",
  "eoc": "EOC_HELLO",                       // an effect_on_condition id, or an inline EOC
  "context": "DEFAULTMODE",                 // optional. "DEFAULTMODE" = main game view, "INVENTORY" = inventory menus
  "bindings": [ { "input_method": "keyboard_any", "key": "F6" } ] }   // default keys, same format as keybindings.json
```
* The player can rebind it in the keybinding menu; keys the player chose are kept.
* `"context": "INVENTORY"`: the EOC runs with the player as alpha talker and the highlighted item as beta talker.
  The effect should only change the item (flags, variables, ...).  Do not delete or move it: the open menu would still point at it.
* A condition on the EOC decides whether it does anything.

## Music

* **Mod playlists** (`music_playlist`): files are relative to the folder of the JSON file.
  ```json
  { "type": "music_playlist", "id": "mp3", "shuffle": true, "volume": 100,
    "files": [ "songs/a.mp3", { "file": "songs/b.ogg", "volume": 80 } ] }
  ```
  or `"folder": "songs"` to take every audio file (mp3, ogg, flac, wav, opus) below that folder.  `"extend": true` adds to an earlier definition.
  Built-in ids: `mp3` (mp3 players), `instrument`, `sound` (stereos), `title`.  Any other id is a custom playlist.
* **Drop-in folder**: `<game folder>/music/` : files in it (or in `music/mp3/`) replace the mp3 player's playlist; `music/<id>/` makes the playlist `<id>`.
  `noshuffle.txt` inside a folder = play in file-name order.  User folders beat mod playlists, which beat the soundpack.
* **EOC effect `music_control`**:
  `{ "music_control": "next" }`, `"previous"`, `"info"` (shows the current song),
  `{ "music_control": "play", "playlist": "<id>" }` (forces a playlist), `"auto"` (back to the automatic choice), `"rescan"` (re-read `music/`),
  `"choose_album"` (menu of the albums the player carries; the pick is what mp3 players play and is saved with the character),
  `"next_album"` / `"previous_album"` (switch to the next / previous album the player carries),
  `"restore_album"` (re-apply the saved pick; run it from a `game_load` event EOC),
  level 4: `"pause"`, `"resume"`, `"toggle_pause"` (also starts the album again after it ended with repeat off), `"volume_up"` / `"volume_down"` (step = `"value"`, default 10), level 5: `"toggle_shuffle"`, `"cycle_repeat"` (whole album -> one song -> off), `"play_index"` (`"value"` = place in the song list, 0 is the first), `"set_volume"` (`"value"`, 0 - 128, 100 = normal;
  this is the game's "Music volume" option and is saved).  `"value"` can be a number or `{ "math": [ "..." ] }`.
  `"play"` with a playlist that does not exist falls back to the normal music (and writes a warning to the debug log).
* **Albums** (`music_albums`, engine level 3): every sub folder of `folder` (relative to the JSON file, default `albums`) that holds audio files becomes
  * a playlist and an item type `album_<folder name>` (letters/digits only, lower case) named `album: <folder name>`, 0 g and 1 ml, with the item flag `OWN_MUSIC_ALBUM` (the own_music_player mod gives mp3 players a pocket that only takes items with this flag, so albums can be stored in the player; an album stored in a carried player counts as carried),
  * an item group `music_albums_pool` with every album (equal chance each) that mods can put into loot piles, e.g.
    `{ "type": "item_group", "id": "music_shop", "copy-from": "music_shop", "subtype": "old", "extend": { "items": [ { "group": "music_albums_pool", "prob": 10 } ] } }`
    (see `album_loot.json` of the own_music_player mod),
  * an entry in what zombies carry: all albums together get the weight `zombie_drop_weight` (default 1, 0 = no drops) in `default_zombie_items`.
  ```json
  { "type": "music_albums", "folder": "albums", "zombie_drop_weight": 1 }
  ```
  A folder with a file `noshuffle.txt` plays in file-name order.  The albums are generated each time the game loads, so adding a folder needs nothing else.
  Do not delete or rename an album folder while a save holds that album: the item would vanish from the save.

## Floating windows (`mod_window`, level 4)

Small windows on top of the game that show live text and have clickable buttons.  The player can drag them with the mouse
(press anywhere on the window except a button, move, release).  Position and "open" state are saved in `config/mod_windows.json`.

```json
{ "type": "mod_window", "id": "MYMOD_WINDOW", "title": "Music player", "width": 38, "x": 2, "y": 2,
  "remember_open": true,
  "rows": [
    "{music_track}",
    { "text": "{music_time}", "align": "center" },
    { "align": "center", "buttons": [
        { "text": "[Prev]", "eoc": "EOC_PREVIOUS" },
        { "text": "[{music_playpause}]", "eoc": "EOC_TOGGLE_PAUSE" } ] } ] }
```
* `width` is in character cells (10 or more); the height is the number of rows + 2.  `x` / `y` = first position (cells from the top left).
* A row is a text (color tags allowed, clipped to the width), or an object with `text` or `buttons`, and `align` (`left`, `center`, `right`).
* Level 5 row options: `"show_if": "u_somevar"` (the row only exists while that character variable is not 0 / empty; `"!u_somevar"` turns it around;
  the window grows and shrinks with the rows) and a list row `{ "list": "music_queue", "height": 8 }` that shows the songs of the playing album in play
  order (the playing one marked `>`, scrolling along with it); clicking a song plays it.
* `eoc` of a button: an effect_on_condition id or an inline EOC.  It runs with the player as talker when the button is clicked (press and release on the same button).
* A row `{ "image": "album_cover", "height": 11, "image_width": 22, "align": "center" }` shows the cover of the album the mp3 player plays
  (graphical build only; a grey square if the album has no cover).  The cover is found automatically: a `cover.jpg` / `folder.png` / ... in the album folder,
  else the picture stored inside the mp3 files (ID3 "attached picture").  Nothing has to be prepared for new albums.  `height` = rows, `image_width` = cells.
* `show_while`: `"mp3"` = the window opens when an mp3 player is switched on and closes when it is switched off.
* `remember_open`: the window comes back by itself when it was open the last time the game was left.
* Control from effects: `{ "window_control": "open" | "close" | "toggle" | "close_all" | "reset_position", "window": "<id>" }`
  (it takes effect within a fraction of a second; the windows update while the game waits for input, not during long activities).
* Mouse clicks on a window never reach the map, so a window is also a way to get "clickable areas" for mods.

## Text templates

Windows (rows and button texts) and sidebar widgets can contain `{placeholders}`; unknown ones stay as typed.

| placeholder | value |
|---|---|
| `{music_track}` `{music_state}` `{music_playpause}` | song name, Playing / Paused / Stopped, Play / Pause (for the next click) |
| `{music_album}` `{music_playlist}` | chosen album (or "normal playlist"), id of the playing playlist |
| `{music_time}` `{music_position}` `{music_length}` | `1:23 / 3:45`, `1:23`, `3:45` (`--:--` if unknown) |
| `{music_bar:30}` | progress bar of the song, `[####------]`, 30 characters wide |
| `{music_volume}` | volume, 100 = normal |
| `{music_shuffle}` `{music_repeat}` | On / Off, Album / Song / Off (level 5) |
| `{music_battery}` `{music_battery_bar:14}` | battery of the player or phone that plays the music: `73%` / `[#######---]`, `--` if unknown (level 5) |
| `{time}` `{day}` `{player_name}` | time of day, day number, character name |
| `{var:name}` `{gvar:name}` | a character variable / a global variable (`u_name` in EOCs) |
| `{option:id}` | value of a mod option |

**Sidebar**: a widget with `"var": "mod_text"` and `"mod_text": "<template>"` shows such a text (refreshed about once a second):
```json
{ "type": "widget", "id": "mymod_music", "label": "Music", "style": "text", "var": "mod_text",
  "mod_text": "{music_track}", "flags": [ "W_DISABLED_BY_DEFAULT" ] }
```
Add it to the sidebars with `{ "type": "widget", "copy-from": "legacy_labels_sidebar", "id": "legacy_labels_sidebar", "extend": { "widgets": [ "mymod_music" ] } }`.

## Timers (`realtime_eoc`, level 4)

```json
{ "type": "realtime_eoc", "id": "MYMOD_TIMER", "interval": 4, "eoc": "EOC_MY_TICK" }
```
Runs the effect on condition every `interval` real seconds (0.1 or more) while the game waits for the player (not during long activities and not while a menu is open).
The EOC's own `condition` decides whether it does anything.  See the mod `own_music_context` for a complete example.

## Mod options (`mod_option`, level 4)

```json
{ "type": "mod_option", "id": "mymod.step", "name": "Step size", "description": "How big a step is.",
  "kind": "int", "min": 1, "max": 40, "step": 1, "default": 10, "on_change": "EOC_STEP_CHANGED" }
```
`kind`: `bool` (`default` true/false), `int` (`min`, `max`, `default`), `list` (`choices`: `[ { "value": "a", "name": "Option A" } ]`, `default` = a value), `text`.
The player changes them in the Esc menu -> **Mod options** (the entry only exists when a mod has options). Values are saved in `config/mod_options.json`.
* Read in a condition or `math`: `game_option('mod:mymod.step')` (switch: 1/0; lists and texts: 0).
* Read into a variable: `{ "mod_option_get": "mymod.step", "target_var": { "context_val": "x" } }` (numbers for switch / int, text otherwise).
* Set: `{ "mod_option_set": "mymod.step", "value": "20" }`. Open the menu from an effect: `{ "mod_options_menu": true }`.

## Files and sounds (level 4)

* `{ "file_write": "text", "file": "notes/log.txt", "append": true, "newline": true, "expand": true }` writes into `config/mod_data/` (`expand` replaces `{placeholders}`).
* `{ "file_read": "notes/log.txt", "target_var": { "context_val": "x" }, "default": "" }` reads a file from there.
* File names may contain letters, digits, `_ - . /` and spaces, no `..`; nothing outside `config/mod_data/` can be reached; files are limited to a few MB.
* `{ "play_sound_file": "mods/mymod/click.ogg", "volume": 80 }` plays a sound once; the path is relative to the game folder (wav, ogg, mp3, flac).
  The "Sound effect volume" option applies.

## New events for effects on condition

| event | fields | when |
|---|---|---|
| `character_picks_up_item` | `character`, `itype` | an item is picked up |
| `character_drops_item` | `character`, `itype` | items are dropped on purpose |
| `character_performs_action` | `character`, `action` | a game action (move, craft, sleep, ...) is performed; idle ticks are not reported |
| `character_uses_item` | `character`, `itype` | level 4: an item is used (activated), after its use action ran |
| `character_crafts_item` | `character`, `itype` | level 4: a craft is finished; `itype` is the result |

Existing vanilla events that mods can use as well: `character_kills_monster`, `character_takes_damage`, `character_wears_item`, `character_wields_item`,
`avatar_enters_omt` (new overmap tile), `character_gains_effect`, `game_load`, `game_start`, ... (see EFFECT_ON_CONDITION.md).

## Changing the mods of an existing world

Main menu -> **World** -> pick the world -> **Change World Mods** (key `M`): the usual mod selection screen opens for that world.  The change is saved in the world's `mods.json`.
Take a backup of the world first; removing a mod that added items or terrain to a save can break that save.

## Device window (phones, laptops, tablets)

Using a device with the `E_FILE_DEVICE` action (smartphone, laptop, tablet, ...) opens one window instead of the old chain of menus:

* top: the device, its charge and memory; its other apps (music, camera, games, ...) as buttons;
* left: all e-devices the player carries or can reach (the one in use first), with memory and file count; a "Sort" box (name, most / fewest files, most / least free space, carried first) and a "Hide empty" filter;
* right: the files of the picked device with check boxes (after the device was browsed), Select all / none / Invert and a "Sort" box (name, largest, smallest);
* bottom: Browse / Browse all / Browse all + move every file here (also works when everything was browsed already), Read file, Copy and Move (to or from the device in use), Wipe.
* footer: "Drop empty cards" drops every browsed, empty memory card / USB drive / hard drive (any variant) that is carried.  Devices that can read files themselves (phones, laptops, e-readers) are never dropped.
  Hovering a button shows why it is greyed out, or how long the transfer takes.

The work is done by the normal e-file activity, so the rules (time, charges, memory, compatibility) are those of the old menus.
The "Old menu" button opens the old menu chain.  No JSON is needed for this.


## Title screen and loading screen pictures (level 5)

* **Title screen**: every file `data/title/art/<name>.<style>.<size>.title` is one picture (`style` = `braille` or `blocks`, `size` = `L`, `M`, `S`; same format as
  `data/title/en.title`, color tags allowed).  When the main menu opens, one `<name>` is picked at random and the biggest size that fits the window is used
  (L needs 30 free rows, M 21, S 12); the main menu window uses the whole screen height for it.  Option **Title screen picture** (Interface -> ASCII):
  Braille / Blocks / Classic (the old logo).  Seasonal logos on holidays still win.
* **Loading screen** (tiles version): every `.png` in `gfx/loading/` is a possible loading picture (mods can still add their own with `loading_images`).
  Option **Pictures on the loading screen** switches this off.
