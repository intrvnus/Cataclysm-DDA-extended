#pragma once
#ifndef CATA_SRC_SDLSOUND_H
#define CATA_SRC_SDLSOUND_H

#include <string>
#include <vector>
#if defined(SDL_SOUND)

/**
 * Attempt to initialize an audio device.  Returns false if initialization fails.
 */
bool init_sound();
void initSDLAudioOnly();
void shutdown_sound();
void play_music( const std::string &playlist );
void stop_music();
void update_music_volume();
void load_soundset();
/** Skip to the next (direction > 0) or previous (direction < 0) track of the playing playlist.
 *  Returns the name of the new track, or an empty string if no music was playing. */
std::string music_skip( int direction );
/** Name of the track that is playing right now, or an empty string. */
std::string music_current_track();
/** Force a playlist to play regardless of the automatic choice.  Empty string = automatic again. */
void music_force_playlist( const std::string &playlist );
/** Re-read the "music" folder in the user directory (drop-in playlists). */
void music_rescan_user_folder();
/** Id of the playlist that is playing right now, or an empty string. */
std::string music_current_playlist();
/** Add a playlist (full file paths) that can be played by id.  Used for the album folders. */
void music_register_playlist( const std::string &id, const std::vector<std::string> &files,
                              bool shuffle );
/** Choose the album (playlist) the mp3 player plays; empty = no album, so no music. */
void music_set_mp3_album( const std::string &playlist );
std::string music_get_mp3_album();
bool music_playlist_exists( const std::string &id );
bool music_is_paused();
void music_set_paused( bool paused );
/** Position / length of the song that plays, in seconds; -1 if unknown. */
/** The songs of the playlist that plays, in the order they are played; "current" is the index of the
 *  song that plays right now (-1 if none). */
std::vector<std::string> music_queue( int &current );
/** Play the song at this place of music_queue.  Returns its name. */
std::string music_play_index( int index );
bool music_get_shuffle();
void music_set_shuffle( bool shuffle );
/** 0 = stop after the last song, 1 = repeat the album, 2 = repeat the song. */
int music_get_repeat();
void music_set_repeat( int mode );
double music_position_seconds();
double music_duration_seconds();
/** The "Music volume" option, 0 - 128 (100 = normal).  Setting it saves the options. */
int music_volume();
void music_set_volume( int volume );
extern bool sound_init_success;

#else

inline bool init_sound()
{
    return false;
}
inline void shutdown_sound() { }
inline void play_music( const std::string &/*playlist*/ )
{
}
inline void update_music_volume() { }
inline void load_soundset() { }
inline std::string music_skip( int )
{
    return std::string();
}
inline std::string music_current_track()
{
    return std::string();
}
inline void music_force_playlist( const std::string & ) { }
inline void music_rescan_user_folder() { }
inline std::string music_current_playlist()
{
    return std::string();
}
inline void music_register_playlist( const std::string &, const std::vector<std::string> &, bool ) { }
inline void music_set_mp3_album( const std::string & ) { }
inline std::string music_get_mp3_album()
{
    return std::string();
}
inline bool music_playlist_exists( const std::string & )
{
    return false;
}
inline bool music_is_paused()
{
    return false;
}
inline void music_set_paused( bool ) { }
inline std::vector<std::string> music_queue( int &current )
{
    current = -1;
    return std::vector<std::string>();
}
inline std::string music_play_index( int )
{
    return std::string();
}
inline bool music_get_shuffle()
{
    return false;
}
inline void music_set_shuffle( bool ) { }
inline int music_get_repeat()
{
    return 1;
}
inline void music_set_repeat( int ) { }
inline double music_position_seconds()
{
    return -1.0;
}
inline double music_duration_seconds()
{
    return -1.0;
}
inline int music_volume()
{
    return 100;
}
inline void music_set_volume( int ) { }

#endif

#endif // CATA_SRC_SDLSOUND_H
