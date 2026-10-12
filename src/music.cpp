#include "music.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

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

static std::map<std::string, std::vector<unsigned char>> cover_cache;
static bool pool_exists = false;

void reset_albums()
{
    albums.clear();
    cover_cache.clear();
    pool_exists = false;
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

// ---------------------------------------------------------------------------------------------
// album covers
// ---------------------------------------------------------------------------------------------
static bool looks_like_picture( const std::vector<unsigned char> &d )
{
    if( d.size() < 8 ) {
        return false;
    }
    const bool jpeg = d[0] == 0xFF && d[1] == 0xD8;
    const bool png = d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
    return jpeg || png;
}

static std::vector<unsigned char> read_file_bytes( const std::filesystem::path &path,
        std::uintmax_t max_size )
{
    std::vector<unsigned char> out;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size( path, ec );
    if( ec || size == 0 || size > max_size ) {
        return out;
    }
    std::ifstream in( path, std::ios::binary );
    if( !in ) {
        return out;
    }
    out.resize( static_cast<size_t>( size ) );
    in.read( reinterpret_cast<char *>( out.data() ), static_cast<std::streamsize>( size ) );
    if( static_cast<std::uintmax_t>( in.gcount() ) != size ) {
        out.clear();
    }
    return out;
}

static unsigned int be_number( const unsigned char *p, int bytes )
{
    unsigned int v = 0;
    for( int i = 0; i < bytes; i++ ) {
        v = ( v << 8 ) | p[i];
    }
    return v;
}

static unsigned int syncsafe_number( const unsigned char *p )
{
    return ( ( p[0] & 0x7Fu ) << 21 ) | ( ( p[1] & 0x7Fu ) << 14 ) | ( ( p[2] & 0x7Fu ) << 7 ) |
           ( p[3] & 0x7Fu );
}

static void remove_unsynchronisation( std::vector<unsigned char> &d )
{
    size_t w = 0;
    for( size_t r = 0; r < d.size(); r++ ) {
        d[w++] = d[r];
        if( d[r] == 0xFF && r + 1 < d.size() && d[r + 1] == 0x00 ) {
            r++;
        }
    }
    d.resize( w );
}

/** The text of an APIC / PIC frame: finds the picture bytes.  Returns the picture type, -1 if none. */
static int parse_picture_frame( const std::vector<unsigned char> &f, bool old_format,
                                std::vector<unsigned char> &picture )
{
    if( f.size() < 8 ) {
        return -1;
    }
    const unsigned char encoding = f[0];
    size_t i = 1;
    if( old_format ) {
        i += 3; // image format, three letters
    } else {
        while( i < f.size() && f[i] != 0 ) {
            i++; // mime type
        }
        i++;
    }
    if( i >= f.size() ) {
        return -1;
    }
    const int type = f[i++];
    // description: ends with one zero byte, or two for UTF-16
    if( encoding == 1 || encoding == 2 ) {
        while( i + 1 < f.size() && !( f[i] == 0 && f[i + 1] == 0 ) ) {
            i += 2;
        }
        i += 2;
    } else {
        while( i < f.size() && f[i] != 0 ) {
            i++;
        }
        i++;
    }
    if( i >= f.size() ) {
        return -1;
    }
    picture.assign( f.begin() + static_cast<std::ptrdiff_t>( i ), f.end() );
    return looks_like_picture( picture ) ? type : -1;
}

/** The cover stored in the ID3v2 tag at the start of an mp3 file (front cover preferred). */
static std::vector<unsigned char> read_id3_cover( const std::filesystem::path &file )
{
    std::vector<unsigned char> none;
    std::ifstream in( file, std::ios::binary );
    if( !in ) {
        return none;
    }
    unsigned char head[10];
    in.read( reinterpret_cast<char *>( head ), 10 );
    if( in.gcount() < 10 || head[0] != 'I' || head[1] != 'D' || head[2] != '3' ) {
        return none;
    }
    const int version = head[3];
    const unsigned char flags = head[5];
    if( version < 2 || version > 4 || ( head[6] & 0x80 ) || ( head[7] & 0x80 ) || ( head[8] & 0x80 ) ||
        ( head[9] & 0x80 ) ) {
        return none;
    }
    const unsigned int tag_size = syncsafe_number( head + 6 );
    if( tag_size < 10 || tag_size > 40u * 1024u * 1024u ) {
        return none;
    }
    std::vector<unsigned char> tag( tag_size );
    in.read( reinterpret_cast<char *>( tag.data() ), static_cast<std::streamsize>( tag_size ) );
    if( static_cast<unsigned int>( in.gcount() ) != tag_size ) {
        tag.resize( static_cast<size_t>( std::max<std::streamsize>( in.gcount(), 0 ) ) );
    }
    if( version < 4 && ( flags & 0x80 ) ) {
        remove_unsynchronisation( tag ); // whole tag
    }
    size_t pos = 0;
    if( version >= 3 && ( flags & 0x40 ) && tag.size() >= 6 ) { // extended header
        pos = version == 3 ? be_number( tag.data(), 4 ) + 4u : syncsafe_number( tag.data() );
    }
    const size_t header_size = version == 2 ? 6 : 10;
    std::vector<unsigned char> best;
    int best_type = 1000;
    while( pos + header_size <= tag.size() ) {
        const unsigned char *h = tag.data() + pos;
        if( h[0] == 0 ) {
            break; // padding
        }
        size_t frame_size = 0;
        unsigned char frame_flags_2 = 0;
        std::string id;
        if( version == 2 ) {
            id.assign( reinterpret_cast<const char *>( h ), 3 );
            frame_size = be_number( h + 3, 3 );
        } else {
            id.assign( reinterpret_cast<const char *>( h ), 4 );
            frame_size = version == 3 ? be_number( h + 4, 4 ) : syncsafe_number( h + 4 );
            frame_flags_2 = h[9];
        }
        pos += header_size;
        if( frame_size == 0 || pos + frame_size > tag.size() ) {
            break;
        }
        if( id == "APIC" || id == "PIC" ) {
            std::vector<unsigned char> frame( tag.begin() + static_cast<std::ptrdiff_t>( pos ),
                                              tag.begin() + static_cast<std::ptrdiff_t>( pos + frame_size ) );
            bool usable = true;
            if( version == 4 ) {
                if( frame_flags_2 & 0x01 ) { // data length indicator
                    if( frame.size() > 4 ) {
                        frame.erase( frame.begin(), frame.begin() + 4 );
                    } else {
                        usable = false;
                    }
                }
                if( frame_flags_2 & 0x02 ) {
                    remove_unsynchronisation( frame );
                }
                if( frame_flags_2 & 0x0C ) { // compressed / encrypted
                    usable = false;
                }
            } else if( version == 3 && ( h[9] & 0xE0 ) ) { // compressed / encrypted / grouping bits
                usable = false;
            }
            std::vector<unsigned char> picture;
            const int type = usable ? parse_picture_frame( frame, version == 2, picture ) : -1;
            if( type >= 0 ) {
                const int rank = type == 3 ? 0 : ( type == 0 ? 2 : 1 );
                if( rank < best_type ) {
                    best_type = rank;
                    best = std::move( picture );
                    if( rank == 0 ) {
                        break;
                    }
                }
            }
        }
        pos += frame_size;
    }
    return best;
}

static std::vector<unsigned char> find_album_cover( const std::string &folder_utf8 )
{
    const std::filesystem::path folder = std::filesystem::u8path( folder_utf8 );
    // 1. a picture file lying in the folder
    static const char *const names[] = { "cover", "folder", "front", "albumart", "album" };
    static const char *const exts[] = { ".jpg", ".jpeg", ".png" };
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for( std::filesystem::directory_iterator it( folder, ec ), end; !ec && it != end;
         it.increment( ec ) ) {
        std::error_code ec2;
        if( it->is_regular_file( ec2 ) ) {
            files.push_back( it->path() );
        }
    }
    const auto lower = []( std::string s ) {
        std::transform( s.begin(), s.end(), s.begin(), []( unsigned char c ) {
            return static_cast<char>( std::tolower( c ) );
        } );
        return s;
    };
    for( const char *name : names ) {
        for( const char *ext : exts ) {
            for( const std::filesystem::path &f : files ) {
                if( lower( f.filename().u8string() ) == std::string( name ) + ext ) {
                    std::vector<unsigned char> bytes = read_file_bytes( f, 20u * 1024u * 1024u );
                    if( looks_like_picture( bytes ) ) {
                        return bytes;
                    }
                }
            }
        }
    }
    // 2. the picture inside the songs (first song that has one)
    for( const std::string &song : album_files( folder ) ) {
        const std::filesystem::path p = std::filesystem::u8path( song );
        if( lower( p.extension().u8string() ) != ".mp3" ) {
            continue;
        }
        std::vector<unsigned char> bytes = read_id3_cover( p );
        if( !bytes.empty() ) {
            return bytes;
        }
    }
    return std::vector<unsigned char>();
}

const std::vector<unsigned char> &album_cover( const std::string &playlist )
{
    const auto cached = cover_cache.find( playlist );
    if( cached != cover_cache.end() ) {
        return cached->second;
    }
    std::vector<unsigned char> bytes;
    for( const album_info &album : albums ) {
        if( album.playlist == playlist ) {
            bytes = find_album_cover( album.folder );
            break;
        }
    }
    return cover_cache.emplace( playlist, std::move( bytes ) ).first->second;
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

// Without any albums the pool still has to exist: mods refer to it from their loot groups.
static void define_empty_album_pool( const std::string &src, const cata_path &base_path,
                                     const cata_path &full_path )
{
    if( pool_exists ) {
        return;
    }
    std::ostringstream pool_json;
    JsonOut pool_out( pool_json );
    pool_out.start_object();
    pool_out.member( "type", "item_group" );
    pool_out.member( "id", "music_albums_pool" );
    pool_out.member( "subtype", "distribution" );
    pool_out.member( "entries" );
    pool_out.start_array();
    pool_out.end_array();
    pool_out.end_object();
    DynamicDataLoader::get_instance().load_generated_json( pool_json.str(), src, base_path, full_path );
    pool_exists = true;
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
        define_empty_album_pool( src, base_path, full_path );
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
        albums.push_back( { id, id, name, folder.generic_u8string() } );
        new_ids.push_back( id );

        // The item the player carries around.  It has no weight and takes almost no room (1 ml, so an mp3 player holds only a few).
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
                         " songs.  Carry it, or store it inside an mp3 player, then choose it from the "
                         "menu of the mp3 player (use the mp3 player, then \"Choose album\")." );
        item_out.member( "category", "other" );
        item_out.member( "weight", "0 g" );
        item_out.member( "volume", "1 ml" );
        item_out.member( "price", "1 USD" );
        item_out.member( "price_postapoc", "10 cent" );
        item_out.member( "symbol", "?" );
        item_out.member( "color", "light_cyan" );
        item_out.member( "material", std::vector<std::string> { "plastic" } );
        item_out.member( "flags", std::vector<std::string> { "ZERO_WEIGHT", "OWN_MUSIC_ALBUM" } );
        item_out.end_object();
        loader.load_generated_json( item_json.str(), src, base_path, full_path );
    }

    if( new_ids.empty() ) {
        define_empty_album_pool( src, base_path, full_path );
        return;
    }
    // One group with every album (equal chance each).  Mods put it into loot piles with
    //   { "type": "item_group", "id": "<pile>", "copy-from": "<pile>", ... "extend": { ... { "group": "music_albums_pool", "prob": N } } }
    {
        std::ostringstream pool_json;
        JsonOut pool_out( pool_json );
        pool_out.start_object();
        pool_out.member( "type", "item_group" );
        pool_out.member( "id", "music_albums_pool" );
        pool_out.member( "subtype", "distribution" );
        if( pool_exists ) {
            pool_out.member( "copy-from", "music_albums_pool" );
            pool_out.member( "extend" );
            pool_out.start_object();
        }
        pool_out.member( "entries" );
        pool_out.start_array();
        for( const std::string &id : new_ids ) {
            pool_out.start_object();
            pool_out.member( "item", id );
            pool_out.end_object();
        }
        pool_out.end_array();
        if( pool_exists ) {
            pool_out.end_object();
        }
        pool_out.end_object();
        loader.load_generated_json( pool_json.str(), src, base_path, full_path );
        pool_exists = true;
    }
    if( drop_weight <= 0 ) {
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
