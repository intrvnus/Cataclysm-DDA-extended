#pragma once
#ifndef CATA_SRC_SDLSOUND_H
#define CATA_SRC_SDLSOUND_H

#include <string>
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

#endif

#endif // CATA_SRC_SDLSOUND_H
