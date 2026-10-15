// Host = server + client (A2, Noah 2026-10-15): a multiplayer host starts a dedicated server
// for the world in the background and joins it as a client, so the host's menus never stop the
// world for the other players.  Both run in the same game folder (Noah: no copy of the game): the
// server owns and saves the world, the host's game only shows it and never writes into it (its
// map memory has a name of its own, see main_menu).  A world without characters: the host makes a
// character first, the server makes the world, then the host joins with that character.
#include "net_session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include "platform_win.h"
#else
#include <spawn.h>
#include <sys/wait.h>
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
    // joining a server that was just started (it may still be loading): keep trying
    bool retry_join = false;
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
    // UTF-8 like the game's other paths (a folder name with umlauts must work)
    std::vector<wchar_t> buf( 32768 );
    const DWORD n = GetModuleFileNameW( nullptr, buf.data(), static_cast<DWORD>( buf.size() ) );
    if( n == 0 ) {
        return std::string( argv0 );
    }
    const int len = WideCharToMultiByte( CP_UTF8, 0, buf.data(), static_cast<int>( n ), nullptr, 0, nullptr,
                                         nullptr );
    std::string out( len, '\0' );
    WideCharToMultiByte( CP_UTF8, 0, buf.data(), static_cast<int>( n ), out.data(), len, nullptr, nullptr );
    return out;
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

// the server process this game started (host and play), to wait for it when the host leaves
#if defined(_WIN32)
intptr_t server_process = -1;
#else
pid_t server_process = 0;
#endif

bool spawn( const std::string &exe, const std::vector<std::string> &args, bool keep = false )
{
#if defined(_WIN32)
    std::vector<std::string> q;
    q.reserve( args.size() );
    for( const std::string &a : args ) {
        q.push_back( quoted( a ) );
    }
    const auto wide = []( const std::string & s ) {
        const int len = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), static_cast<int>( s.size() ), nullptr, 0 );
        std::wstring w( len, L'\0' );
        MultiByteToWideChar( CP_UTF8, 0, s.c_str(), static_cast<int>( s.size() ), w.data(), len );
        return w;
    };
    std::vector<std::wstring> wq;
    wq.reserve( q.size() );
    for( const std::string &a : q ) {
        wq.push_back( wide( a ) );
    }
    std::vector<const wchar_t *> argv;
    for( const std::wstring &a : wq ) {
        argv.push_back( a.c_str() );
    }
    argv.push_back( nullptr );
    const std::wstring wexe = wide( exe );
    const intptr_t h = _wspawnv( _P_NOWAIT, wexe.c_str(), argv.data() );
    if( h != -1 && keep ) {
        server_process = h;
    } else if( h != -1 ) {
        CloseHandle( reinterpret_cast<HANDLE>( h ) );
    }
    return h != -1;
#else
    std::vector<char *> argv;
    for( const std::string &a : args ) {
        argv.push_back( const_cast<char *>( a.c_str() ) );
    }
    argv.push_back( nullptr );
    pid_t pid = 0;
    if( posix_spawn( &pid, exe.c_str(), nullptr, nullptr, argv.data(), environ ) != 0 ) {
        return false;
    }
    if( keep ) {
        server_process = pid;
    }
    return true;
#endif
}

bool has_character( const std::filesystem::path &world )
{
    std::error_code ec;
    for( const std::filesystem::directory_entry &e : std::filesystem::directory_iterator( world, ec ) ) {
        const std::string name = e.path().filename().u8string();
        if( e.is_regular_file() && ( e.path().extension() == ".sav" ||
                                     ( name.size() > 9 && name.compare( name.size() - 9, 9, ".sav.zzip" ) == 0 ) ) ) {
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
    const fs::path user = fs::absolute( fs::u8path( PATH_INFO::user_dir() ) );
    std::vector<std::string> all = { exe, "--userdir", user.u8string() + "/" };
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
std::atomic<bool> console_stop{ false };
std::atomic<bool> console_saved{ false };
#if defined(_WIN32)
BOOL WINAPI console_ctrl( DWORD )
{
    // closing the server's terminal window: the game loop saves and stops (Windows gives a few
    // seconds before it ends the process)
    console_stop = true;
    // wait for the save (Windows ends the process after about 5 seconds anyway)
    for( int i = 0; i < 48 && !console_saved; ++i ) {
        Sleep( 100 );
    }
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

void console_stop_done()
{
    console_saved = true;
}

bool listen_local()
{
    return lstate().local;
}

namespace
{
std::filesystem::path ready_marker( const std::string &world )
{
    return std::filesystem::absolute( std::filesystem::u8path( PATH_INFO::user_dir() ) ) / "save" /
           std::filesystem::u8path( world ) /
           "mp_server_ready";
}
} // namespace

void mark_world_ready( const std::string &world )
{
    std::ofstream f( ready_marker( world ) );
    f << "ready\n";
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
    const fs::path user = fs::absolute( fs::u8path( PATH_INFO::user_dir() ) );
    const fs::path world_dir = user / "save" / fs::u8path( world );
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
        exe, "--userdir", user.u8string() + "/", "--world", world,
        "--mp-max-players", std::to_string( max_players() ),
        "--mp-server", std::to_string( ls.port ), "--mp-listen-child"
    };
    // the server marks the world when it is saved and can be shown (a new world)
    std::error_code ec;
    std::filesystem::remove( ready_marker( world ), ec );
    if( !spawn( exe, args, true ) ) {
        printf( "Multiplayer: could not start the server process; hosting the old way.\n" );
        return false;
    }
    ls.local = true;
    request_connect( "127.0.0.1", ls.port );
    return true;
}

void retry_join_while_loading()
{
    lstate().retry_join = true;
}

bool listen_server_running()
{
#if defined(_WIN32)
    if( server_process == -1 ) {
        return false;
    }
    if( WaitForSingleObject( reinterpret_cast<HANDLE>( server_process ), 0 ) == WAIT_TIMEOUT ) {
        return true;
    }
    CloseHandle( reinterpret_cast<HANDLE>( server_process ) );
    server_process = -1;
    return false;
#else
    if( server_process == 0 ) {
        return false;
    }
    if( waitpid( server_process, nullptr, WNOHANG ) == 0 ) {
        return true;
    }
    server_process = 0;
    return false;
#endif
}

void listen_end()
{
    // ready for another session from the main menu (the server stopped, see listen_server_running)
    listen_state &ls = lstate();
    ls = listen_state();
}

void listen_connect_failed()
{
    listen_state &ls = lstate();
    if( ( !ls.local && !ls.retry_join ) || ls.retries >= 60 ) {
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
