# Moddable engine additions (feature level 2)

Everything here only exists in the custom "moddable engine" build.  A mod can declare
`"requires_engine": 2` in its `MOD_INFO`; builds with a lower level refuse to load such a mod and say why.

## Item marks (`json_flag`)

A `json_flag` gets four optional fields:

| field | meaning |
|---|---|
| `user_mark` (bool) | the player can toggle this flag on items from inventory menus (key `#`, rebindable: "Toggle item mark") |
| `sort_priority` (int) | items with the flag are sorted higher (bigger number) or lower (negative) in inventory menus |
| `protect_from_drop` (bool) | the "select all non-favorite items" key (`,`) in multi-select menus skips these items |
| `name` / `item_prefix` / `item_suffix` / `info` | existing fields: menu label, symbol before/after the item name, tooltip |

Example:
```json
{ "type": "json_flag", "id": "MARK_KEEP", "name": "Keep", "item_prefix": "<color_light_green>[K]</color> ",
  "user_mark": true, "protect_from_drop": true, "sort_priority": 10 }
```
Marks are stored in the item's flags (nothing is added to the save format).  Marks never assign inventory letters.

New option, Interface: **Sort favorites first** (`INV_FAVORITES_FIRST`, default true).  Off = favorites no longer jump to the top.

## `mod_action`: hotkeys that run an effect on condition

```json
{ "type": "mod_action", "id": "MYMOD_HELLO", "name": "Say hello",
  "eoc": "EOC_HELLO",                       // an effect_on_condition id, or an inline EOC
  "context": "DEFAULTMODE",                 // optional. "DEFAULTMODE" = main game view, "INVENTORY" = inventory menus
  "bindings": [ { "input_method": "keyboard_code", "key": "F6" } ] }   // default keys, same format as keybindings.json
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
  `{ "music_control": "play", "playlist": "<id>" }` (forces a playlist), `"auto"` (back to the automatic choice), `"rescan"` (re-read `music/`).

## New events for effects on condition

| event | fields | when |
|---|---|---|
| `character_picks_up_item` | `character`, `itype` | an item is picked up |
| `character_drops_item` | `character`, `itype` | items are dropped on purpose |
| `character_performs_action` | `character`, `action` | a game action (move, craft, sleep, ...) is performed; idle ticks are not reported |
