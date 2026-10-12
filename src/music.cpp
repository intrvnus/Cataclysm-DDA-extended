#include "music.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>

#include "cata_path.h"
#include "debug.h"
#include "enum_conversions.h"
#include "init.h"
#include "json.h"
#include "sdlsound.h"

namespace io
{
template<>
std::string enum_to_string<music::music_id>( music::music_id data )
{
    switch( data ) {
        case music::music_id::mp3:
            return "mp3";
        case music::music_id::instrument:
            return "instrument";
        case music::music_id::sound:
            return "sound";
        case music::music_id::title:
            return "title";
        case music::music_id::LAST:
            break;
    }

    cata_fatal( "Invalid music_id in enum_to_string" );
}
}  // namespace io

namespace music
{
std::map<music_id, std::pair<bool, bool>> music_id_list {
    { mp3, { false, false } }, { instrument, { false, false } }, { sound, { false, false } }, {title, { true, true } }
};

bool is_active_music_id( music_id data )
{
    return music_id_list[data].first && music_id_list[data].second;
}

music_id get_music_id()
{
    for( std::pair<const music_id, std::pair<bool, bool>> &pair : music_id_list ) {
        if( pair.second.first && pair.second.second ) {
            return pair.first;
        }
    }

    cata_fatal( "At least one music_id must remain activated in music::get_music_id" );
}

std::string get_music_id_string()
{
    return io::enum_to_string<music_id>( get_music_id() );
}

void activate_music_id( music_id data )
{
    music_id_list[data].second = true;
}

void activate_music_id( const std::string &data )
{
    activate_music_id( io::string_to_enum<music_id>( data ) );
}

void deactivate_music_id( music_id data )
{
    music_id_list[data].second = false;
}

void deactivate_music_id( const std::string &data )
{
    deactivate_music_id( io::string_to_enum<music_id>( data ) );
}

void deactivate_music_id_all()
{
    for( std::pair<const music_id, std::pair<bool, bool>> &pair : music_id_list ) {
        pair.second.second = false;
    }

    activate_music_id( music_id::title );
}

void update_music_id_is_empty_flag( music_id data, bool update )
{
    music_id_list[data].first = update;
}

void update_music_id_is_empty_flag( const std::string &data, bool update )
{
    music_id_list[io::string_to_enum<music_id>( data )].first = update;
}

bool is_known_music_id( const std::string &data )
{
    return data == "mp3" || data == "instrument" || data == "sound" || data == "title";
}

static std::vector<album_info> albums;

const std::vector<album_info> &album_list()
{
    return albums;
}

void reset_albums()
{
    albums.clear();
}

static bool is_audio_extension( const std::filesystem::path &path )
{
    std::string ext = path.extension().u8string();
    std::transform( ext.begin(), ext.end(), ext.begin(), []( unsigned char c ) {
        return static_cast<char>( std::tolower( c ) );
    } );
    return ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".wav" || ext == ".opus";
}

/** All audio files in a folder and below it, sorted by name. */
static std::vector<std::string> album_files( const std::filesystem::path &folder )
{
    std::vector<std::string> files;
    std::error_code ec;
    for( std::filesystem::recursive_directory_iterator it( folder, ec ), end; !ec && it != end;
         it.increment( ec ) ) {
        std::error_code ec2;
        if( it->is_regular_file( ec2 ) && is_audio_extension( it->path() ) ) {
            files.push_back( it->path().generic_u8string() );
        }
    }
    std::sort( files.begin(), files.end() );
    return files;
}

/** "album_" + the folder name with everything but letters and digits turned into "_". */
static std::string album_id_for( const std::string &folder_name, std::set<std::string> &used )
{
    std::string id = "album_";
    bool last_underscore = true;
    for( const unsigned char c : folder_name ) {
        if( std::isalnum( c ) && c < 0x80 ) {
            id += static_cast<char>( std::tolower( c ) );
            last_underscore = false;
        } else if( !last_underscore ) {
            id += '_';
            last_underscore = true;
        }
    }
    while( !id.empty() && id.back() == '_' ) {
        id.pop_back();
    }
    if( id == "album" ) {
        id = "album_unnamed";
    }
    // Two folders can only end up with the same id if their names differ in non-ASCII letters.
    std::string unique = id;
    for( int n = 2; used.count( unique ) > 0; n++ ) {
        unique = id + "_" + std::to_string( n );
    }
    used.insert( unique );
    return unique;
}

void load_albums( const JsonObject &jo, const std::string &src, const cata_path &base_path,
                  const cata_path &full_path )
{
    const std::string folder_name = jo.get_string( "folder", "albums" );
    const int drop_weight = jo.get_int( "zombie_drop_weight", 1 );
    // The folder is relative to the JSON file that holds this object.
    const std::filesystem::path dir = full_path.get_unrelative_path().parent_path() /
                                      std::filesystem::u8path( folder_name );
    std::error_code ec;
    if( !std::filesystem::is_directory( dir, ec ) ) {
        return;
    }

    std::vector<std::filesystem::path> folders;
    for( std::filesystem::directory_iterator it( dir, ec ), end; !ec && it != end; it.increment( ec ) ) {
        std::error_code ec2;
        if( it->is_directory( ec2 ) ) {
            folders.push_back( it->path() );
        }
    }
    std::sort( folders.begin(), folders.end() );

    std::set<std::string> used_ids;
    for( const album_info &known : albums ) {
        used_ids.insert( known.item_id );
    }
    std::vector<std::string> new_ids;
    DynamicDataLoader &loader = DynamicDataLoader::get_instance();
    for( const std::filesystem::path &folder : folders ) {
        const std::vector<std::string> files = album_files( folder );
        if( files.empty() ) {
            continue;
        }
        const std::string name = folder.filename().u8string();
        const std::string id = album_id_for( name, used_ids );
        const bool shuffle = !std::filesystem::exists( folder / "noshuffle.txt", ec );
        music_register_playlist( id, files, shuffle );
        albums.push_back( { id, id, name } );
        new_ids.push_back( id );

        // The item the player carries around.  It has no weight and takes (almost) no room.
        std::ostringstream item_json;
        JsonOut item_out( item_json );
        item_out.start_object();
        item_out.member( "type", "ITEM" );
        item_out.member( "id", id );
        item_out.member( "name" );
        item_out.start_object();
        item_out.member( "str", "album: " + name );
        item_out.end_object();
        item_out.member( "description",
                         "A music album with " + std::to_string( files.size() ) +
                         " songs.  Choose it from the menu of an mp3 player (use the mp3 player, "
                         "then \"Choose album\") to listen to it while you carry it." );
        item_out.member( "category", "other" );
        item_out.member( "weight", "0 g" );
        item_out.member( "volume", "0 ml" );
        item_out.member( "price", "1 USD" );
        item_out.member( "price_postapoc", "10 cent" );
        item_out.member( "symbol", "?" );
        item_out.member( "color", "light_cyan" );
        item_out.member( "material", std::vector<std::string> { "plastic" } );
        item_out.end_object();
        loader.load_generated_json( item_json.str(), src, base_path, full_path );
    }

    if( new_ids.empty() || drop_weight <= 0 ) {
        return;
    }
    // Zombies carry them: add one entry (a pick from all the albums) to what zombies drop.
    std::ostringstream group_json;
    JsonOut group_out( group_json );
    group_out.start_object();
    group_out.member( "type", "item_group" );
    group_out.member( "id", "default_zombie_items" );
    group_out.member( "copy-from", "default_zombie_items" );
    group_out.member( "subtype", "distribution" );
    group_out.member( "extend" );
    group_out.start_object();
    group_out.member( "entries" );
    group_out.start_array();
    group_out.start_object();
    group_out.member( "distribution" );
    group_out.start_array();
    for( const std::string &id : new_ids ) {
        group_out.start_object();
        group_out.member( "item", id );
        group_out.end_object();
    }
    group_out.end_array();
    group_out.member( "prob", drop_weight );
    group_out.end_object();
    group_out.end_array();
    group_out.end_object();
    group_out.end_object();
    loader.load_generated_json( group_json.str(), src, base_path, full_path );
}
} // namespace music
