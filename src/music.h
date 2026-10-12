#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

class JsonObject;
class cata_path;

template <typename E> struct enum_traits;

namespace music
{
enum music_id {
    mp3,
    instrument,
    sound,
    title,
    LAST
};

extern std::map<music_id, std::pair<bool, bool>> music_id_list;

bool is_active_music_id( music_id data );
music_id get_music_id();
std::string get_music_id_string();
void activate_music_id( music_id data );
void activate_music_id( const std::string &data );
void deactivate_music_id( music_id data );
void deactivate_music_id( const std::string &data );
void deactivate_music_id_all();
void update_music_id_is_empty_flag( music_id data, bool update );
void update_music_id_is_empty_flag( const std::string &data, bool update );
/** True if the string is one of the built-in playlist ids (mp3, instrument, sound, title). */
bool is_known_music_id( const std::string &data );

/** An album: a folder of songs that became an item the player can pick up and play. */
struct album_info {
    std::string item_id;   // item type id, also the id of the playlist
    std::string playlist;  // playlist id (same as item_id)
    std::string name;      // folder name, shown to the player
    std::string folder;    // full path of the album folder
};
/** All albums found by "music_albums" JSON objects of the loaded mods. */
const std::vector<album_info> &album_list();
/**
 * The cover picture of an album (bytes of a jpg / png file), or an empty vector if it has none.
 * Looked for automatically: a picture file in the album folder (cover.jpg, folder.png, ...), else
 * the picture stored inside the songs (the "attached picture" of the mp3 tags).  Nothing has to be
 * prepared when new albums are added.
 */
const std::vector<unsigned char> &album_cover( const std::string &playlist );
/** JSON type "music_albums": makes an item type and a playlist for every sub folder. */
void load_albums( const JsonObject &jo, const std::string &src, const cata_path &base_path,
                  const cata_path &full_path );
void reset_albums();
} // namespace music

// Use enum_traits for generic iteration over music_id, and string (de-)serialization.
// Use io::string_to_enum<music_id>( music_id ) to convert a string to music_id.
template<>
struct enum_traits<music::music_id> {
    static constexpr music::music_id last = music::music_id::LAST;
};
