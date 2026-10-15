// Host = server + client (A2, Noah 2026-10-15): a multiplayer host starts a dedicated server
// for the world in the background and joins it as a client, so the host's menus never stop the
// world for the other players.  The client runs from its own user folder (mp_local/) with a copy
// of the configuration and the small files of the world, so the two processes never write the
// same files.  The server hands the world's character over to the host's player (once) and keeps
// the avatar as its invisible anchor: such a world stays a multiplayer world.
#include "net_session.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include "platform_win.h"
#else
#include <spawn.h>
#include <unistd.h>
extern char **environ;
#endif

#include "debug.h"
#include "path_info.h"

namespace net
{

namespace
{
struct listen_state {
    bool requested = false;
    int port = default_port;
    bool local = false;
    bool child = false;
    bool retry_pending = false;
    std::chrono::steady_clock::time_point retry_at;
    int retries = 0;
};

listen_state &lstate()
{
    static listen_state s;
    return s;
}

std::string executable_path( const char *argv0 )
{
#if defined(_WIN32)
    ( void ) argv0;
    std::vector<char> buf( 4096 );
    const unsigned long n = GetModuleFileNameA( nullptr, buf.data(), static_cast<unsigned long>( buf.size() ) );
    return n > 0 ? std::string( buf.data(), n ) : std::string( argv0 );
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink( "/proc/self/exe", ec );
    return ec ? std::string( argv0 ) : self.string();
#endif
}

#if defined(_WIN32)
// _spawnv hands the arguments to the command line as they are: quote the ones with spaces
std::string quoted( const std::string &arg )
{
    if( arg.find_first_of( " \t\"" ) == std::string::npos && !arg.empty() ) {
        return arg;
    }
    std::string out = "\"";
    for( const char c : arg ) {
        if( c == '"' ) {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}
#endif

bool spawn( const std::string &exe, const std::vector<std::string> &args )
{
#if defined(_WIN32)
    std::vector<std::string> q;
    q.reserve( args.size() );
    for( const std::string &a : args ) {
        q.push_back( quoted( a ) );
    }
    std::vector<const char *> argv;
    for( const std::string &a : q ) {
        argv.push_back( a.c_str() );
    }
    argv.push_back( nullptr );
    return _spawnv( _P_NOWAIT, exe.c_str(), argv.data() ) != -1;
#else
    std::vector<char *> argv;
    for( const std::string &a : args ) {
        argv.push_back( const_cast<char *>( a.c_str() ) );
    }
    argv.push_back( nullptr );
    pid_t pid = 0;
    return posix_spawn( &pid, exe.c_str(), nullptr, nullptr, argv.data(), environ ) == 0;
#endif
}

bool has_character( const std::filesystem::path &world )
{
    std::error_code ec;
    for( const std::filesystem::directory_entry &e : std::filesystem::directory_iterator( world, ec ) ) {
        if( e.is_regular_file() && e.path().extension() == ".sav" ) {
            return true;
        }
    }
    return false;
}

// The client's copy: the configuration and the small files of the world (options, mods, the
// character), never the maps (they come from the server).
bool make_local_copy( const std::filesystem::path &user, const std::filesystem::path &local,
                      const std::string &world )
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories( local / "config", ec );
    fs::copy( user / "config", local / "config",
              fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec );
    if( ec ) {
        DebugLog( D_WARNING, D_MAIN ) << "Multiplayer: could not copy the configuration: " << ec.message();
        return false;
    }
    const fs::path from = user / "save" / world;
    const fs::path to = local / "save" / world;
    fs::create_directories( to, ec );
    for( const fs::directory_entry &e : fs::directory_iterator( from, ec ) ) {
        const std::string name = e.path().filename().string();
        if( !e.is_regular_file() || ( e.path().extension() != ".json" && name.rfind( '#', 0 ) != 0 ) ) {
            continue;
        }
        std::error_code cec;
        fs::copy_file( e.path(), to / name, fs::copy_options::overwrite_existing, cec );
    }
    return !ec;
}
} // namespace

void request_listen( int port )
{
    lstate().requested = true;
    lstate().port = port;
}

bool listen_requested()
{
    return lstate().requested;
}

void set_listen_child()
{
    lstate().child = true;
}

bool listen_child()
{
    return lstate().child;
}

bool listen_local()
{
    return lstate().local;
}

bool prepare_listen( const std::string &world, const char *argv0 )
{
    namespace fs = std::filesystem;
    listen_state &ls = lstate();
    if( world.empty() ) {
        printf( "Multiplayer: --mp-host needs --world <name>; hosting the old way.\n" );
        return false;
    }
    const fs::path user = fs::absolute( fs::path( PATH_INFO::user_dir() ) );
    const fs::path world_dir = user / "save" / world;
    if( !has_character( world_dir ) ) {
        printf( "Multiplayer: \"%s\" has no character yet.  Start the world once on its own and make a character, then host it.\n",
                world.c_str() );
        return false;
    }
    // the identity of this player: the server hands the world's character to it
    client_key();
    const fs::path local = user / "mp_local";
    if( !make_local_copy( user, local, world ) ) {
        return false;
    }
    const std::string exe = executable_path( argv0 );
    const std::vector<std::string> args = {
        exe, "--userdir", user.string() + "/", "--world", world,
        "--mp-server", std::to_string( ls.port ), "--mp-listen-child"
    };
    if( !spawn( exe, args ) ) {
        printf( "Multiplayer: could not start the server process; hosting the old way.\n" );
        return false;
    }
    PATH_INFO::init_user_dir( local.string() + "/" );
    PATH_INFO::set_standard_filenames();
    ls.local = true;
    request_connect( "127.0.0.1", ls.port );
    return true;
}

void listen_connect_failed()
{
    listen_state &ls = lstate();
    if( !ls.local || ls.retries >= 60 ) {
        return;
    }
    // the server is still loading the world: try again in a moment
    ls.retries++;
    ls.retry_pending = true;
    ls.retry_at = std::chrono::steady_clock::now() + std::chrono::seconds( 3 );
}

void listen_tick()
{
    listen_state &ls = lstate();
    if( ls.retry_pending && std::chrono::steady_clock::now() >= ls.retry_at ) {
        ls.retry_pending = false;
        shutdown();
        request_reconnect();
    }
}

} // namespace net
