// Host = server + client (A2, Noah 2026-10-15): a multiplayer host starts a dedicated server
// for the world in the background and joins it as a client, so the host's menus never stop the
// world for the other players.  Both run in the same game folder (Noah: no copy of the game): the
// server owns and saves the world, the host's game only shows it and never writes into it (its
// map memory has a name of its own, see main_menu).  A world without characters: the host makes a
// character first, the server makes the world, then the host joins with that character.
#include "net_session.h"

#include <chrono>
#include <cstdio>
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

} // namespace

bool launch_game( const std::vector<std::string> &args )
{
    namespace fs = std::filesystem;
    const std::string exe = executable_path( "cataclysm-tiles" );
    const fs::path user = fs::absolute( fs::path( PATH_INFO::user_dir() ) );
    std::vector<std::string> all = { exe, "--userdir", user.string() + "/" };
    all.insert( all.end(), args.begin(), args.end() );
    return spawn( exe, all );
}

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

namespace
{
volatile bool console_stop = false;
#if defined(_WIN32)
BOOL WINAPI console_ctrl( DWORD )
{
    // closing the server's terminal window: the game loop saves and stops (Windows gives a few
    // seconds before it ends the process)
    console_stop = true;
    Sleep( 5000 );
    return TRUE;
}
#endif
} // namespace

void open_server_console()
{
#if defined(_WIN32)
    if( GetConsoleWindow() == nullptr && AllocConsole() ) {
        FILE *f = nullptr;
        freopen_s( &f, "CONOUT$", "w", stdout );
        freopen_s( &f, "CONOUT$", "w", stderr );
        // no QuickEdit: a click into the window would pause the server until Escape
        HANDLE in = CreateFileA( "CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr );
        DWORD mode = 0;
        if( in != INVALID_HANDLE_VALUE && GetConsoleMode( in, &mode ) ) {
            SetConsoleMode( in, ( mode | ENABLE_EXTENDED_FLAGS ) & ~ENABLE_QUICK_EDIT_MODE );
        }
        SetConsoleTitleA( "Cataclysm multiplayer server (close this window to save and stop it)" );
    }
    SetConsoleCtrlHandler( console_ctrl, TRUE );
#endif
    std::printf( "Cataclysm multiplayer server: loading the world...\n"
                 "The log is in config/debug_server.log.\n" );
    std::fflush( stdout );
}

bool console_stop_requested()
{
    return console_stop;
}

bool listen_local()
{
    return lstate().local;
}

namespace
{
std::filesystem::path ready_marker( const std::string &world )
{
    return std::filesystem::absolute( std::filesystem::path( PATH_INFO::user_dir() ) ) / "save" / world /
           "mp_server_ready";
}
} // namespace

void mark_world_ready( const std::string &world )
{
    std::FILE *f = std::fopen( ready_marker( world ).string().c_str(), "w" );
    if( f != nullptr ) {
        std::fputs( "ready\n", f );
        std::fclose( f );
    }
}

bool world_ready( const std::string &world )
{
    std::error_code ec;
    return std::filesystem::exists( ready_marker( world ), ec );
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
        // a new world: the server makes its anchor, this player makes a character on its copy
        // when the game starts (and the world waits for it)
        printf( "Multiplayer: \"%s\" has no character yet: you make one, the world waits.\n",
                world.c_str() );
    }
    // the identity of this player: the server hands the world's character to it
    client_key();
    const std::string exe = executable_path( argv0 );
    const std::vector<std::string> args = {
        exe, "--userdir", user.string() + "/", "--world", world,
        "--mp-max-players", std::to_string( max_players() ),
        "--mp-server", std::to_string( ls.port ), "--mp-listen-child"
    };
    // the server marks the world when it is saved and can be shown (a new world)
    std::error_code ec;
    std::filesystem::remove( ready_marker( world ), ec );
    if( !spawn( exe, args ) ) {
        printf( "Multiplayer: could not start the server process; hosting the old way.\n" );
        return false;
    }
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
