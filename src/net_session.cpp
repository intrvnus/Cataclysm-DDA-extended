#include "net_session.h"

#include "alerts.h"
#include "bubbles.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <deque>
#include <string>
#include <thread>

#include <enet/enet.h>

#include "debug.h"
#include "flexbuffer_json.h"
#include "fluid.h"
#include "game.h"
#include "json_loader.h"
#include "messages.h"
#include "net_protocol.h"
#include "options.h"
#include "translations.h"
#include "ui_manager.h"

#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#endif

namespace net
{
uint64_t raw_bytes_sent = 0;

namespace impl
{

session_state &state()
{
    static session_state s;
    return s;
}

void send_json( ENetPeer *peer, const std::string &json )
{
    if( peer == nullptr ) {
        return;
    }
    net::raw_bytes_sent += json.size();
    ENetPacket *packet = enet_packet_create( json.data(), json.size(), ENET_PACKET_FLAG_RELIABLE );
    if( enet_peer_send( peer, 0, packet ) != 0 ) {
        enet_packet_destroy( packet );
    }
}

void send_json_fast( ENetPeer *peer, const std::string &json )
{
    if( peer == nullptr ) {
        return;
    }
    net::raw_bytes_sent += json.size();
    ENetPacket *packet = enet_packet_create( json.data(), json.size(), 0 );
    if( enet_peer_send( peer, 1, packet ) != 0 ) {
        enet_packet_destroy( packet );
    }
}

static bool enet_ready()
{
    static bool initialized = false;
    static bool failed = false;
    if( !initialized && !failed ) {
        if( enet_initialize() != 0 ) {
            failed = true;
            debugmsg( "Multiplayer: could not initialize networking (ENet)." );
        } else {
            initialized = true;
            atexit( enet_deinitialize );
        }
    }
    return initialized;
}

static void handle_data( ENetPeer *peer, std::string data )
{
    session_state &s = state();
    try {
        JsonValue jv = json_loader::from_string( std::move( data ) );
        JsonObject msg = jv.get_object();
        msg.allow_omitted_members();
        if( s.current == role::host ) {
            server_on_message( peer, msg );
        } else if( s.current == role::client ) {
            client_on_message( msg );
        }
    } catch( const std::exception &err ) {
        DebugLog( D_WARNING, D_GAME ) << "Multiplayer: bad message: " << err.what();
    }
}

static void handle_packet( ENetPeer *peer, const ENetPacket *packet )
{
    handle_data( peer, std::string( reinterpret_cast<const char *>( packet->data ), packet->dataLength ) );
}

// messages a client received while it was inside a menu, handled at the next poll; an empty
// string stands for "the host went away"
static std::deque<std::string> &menu_queue()
{
    static std::deque<std::string> q;
    return q;
}

void menu_poll()
{
    session_state &s = state();
    if( s.host == nullptr || s.current != role::client ) {
        return;
    }
    ENetEvent event;
    for( int handled = 0; handled < 256 && enet_host_service( s.host, &event, 0 ) > 0; ++handled ) {
        if( event.type == ENET_EVENT_TYPE_RECEIVE ) {
            std::string data( reinterpret_cast<const char *>( event.packet->data ),
                              event.packet->dataLength );
            enet_packet_destroy( event.packet );
            client_peek_message( data );
            menu_queue().push_back( std::move( data ) );
        } else if( event.type == ENET_EVENT_TYPE_DISCONNECT ) {
            menu_queue().emplace_back();
        }
    }
    enet_host_flush( s.host );
}

} // namespace impl

uint64_t wire_bytes_sent()
{
    return impl::state().host != nullptr ? impl::state().host->totalSentData : 0;
}


using namespace impl;

void request_host( int port )
{
    session_state &s = state();
    s.pending = role::host;
    s.pending_port = port;
}

void request_connect( const std::string &address, int port )
{
    session_state &s = state();
    s.pending = role::client;
    s.pending_address = address;
    s.pending_port = port;
}

void request_reconnect()
{
    session_state &s = state();
    s.pending = role::client;
}

namespace
{
int &player_limit()
{
    static int n = 4;
    return n;
}
} // namespace

void set_max_players( int n )
{
    player_limit() = std::clamp( n, 1, max_clients );
}

int max_players()
{
    return player_limit();
}

std::string local_address()
{
    if( enet_initialize() != 0 ) {
        return std::string();
    }
    std::string result;
    // a datagram socket "connected" to a public address sends nothing; it only picks the
    // network card (and so the address) that leads out of this computer
    const ENetSocket sock = enet_socket_create( ENET_SOCKET_TYPE_DATAGRAM );
    if( sock != ENET_SOCKET_NULL ) {
        ENetAddress probe;
        if( enet_address_set_host_ip( &probe, "8.8.8.8" ) == 0 ) {
            probe.port = 53;
            ENetAddress mine;
            char text[64] = {};
            if( enet_socket_connect( sock, &probe ) == 0 && enet_socket_get_address( sock, &mine ) == 0 &&
                enet_address_get_host_ip( &mine, text, sizeof( text ) ) == 0 ) {
                result = text;
            }
        }
        enet_socket_destroy( sock );
    }
    enet_deinitialize();
    return result == "0.0.0.0" ? std::string() : result;
}

void request_dedicated( int port )
{
    request_host( port );
    state().dedicated = true;
    // nobody sits at a server's window: questions and popups must never wait for a key
    alerts::set_unattended( true );
}

bool dedicated()
{
    return state().dedicated && state().current == role::host;
}

bool dedicated_requested()
{
    return state().dedicated;
}

role current_role()
{
    return state().current;
}

void start_pending()
{
    listen_tick();
    session_state &s = state();
    if( s.pending == role::none || s.current != role::none ) {
        return;
    }
    const role wanted = s.pending;
    s.pending = role::none;
    if( !enet_ready() ) {
        return;
    }

    if( wanted == role::host ) {
        ENetAddress address;
        address.host = ENET_HOST_ANY;
        address.port = static_cast<enet_uint16>( s.pending_port );
        s.host = enet_host_create( &address, max_clients, channel_count, 0, 0 );
        if( s.host == nullptr ) {
            add_msg( m_bad, _( "Multiplayer: could not host on port %d." ), s.pending_port );
            if( s.dedicated ) {
                // a server nobody can reach is of no use: it says why and stops
                DebugLog( D_ERROR, D_MAIN ) << "Multiplayer server: could not listen on port " << s.pending_port <<
                                            " (another server running, or the port is in use); the server stops";
                g->uquit = QUIT_NOSAVED;
            }
            return;
        }
        // C17: the messages are JSON text: both ends compress (ENet's range coder)
        if( std::getenv( "CATA_MP_NO_COMPRESS" ) == nullptr ) {
            enet_host_compress_with_range_coder( s.host );
        }
        s.current = role::host;
        server_on_start();
        DebugLog( D_INFO, D_MAIN ) << "Multiplayer server: ready, listening on port " << s.pending_port;
        add_msg( m_good, _( "Multiplayer: hosting on port %d.  Waiting for players…" ),
                 s.pending_port );
    } else {
        s.host = enet_host_create( nullptr, 1, channel_count, 0, 0 );
        if( s.host == nullptr ) {
            add_msg( m_bad, _( "Multiplayer: could not create a network connection." ) );
            return;
        }
        if( std::getenv( "CATA_MP_NO_COMPRESS" ) == nullptr ) {
            enet_host_compress_with_range_coder( s.host );
        }
        ENetAddress address;
        if( enet_address_set_host( &address, s.pending_address.c_str() ) != 0 ) {
            add_msg( m_bad, _( "Multiplayer: unknown address %s." ), s.pending_address );
            enet_host_destroy( s.host );
            s.host = nullptr;
            return;
        }
        address.port = static_cast<enet_uint16>( s.pending_port );
        s.server_peer = enet_host_connect( s.host, &address, channel_count, 0 );
        if( s.server_peer == nullptr ) {
            add_msg( m_bad, _( "Multiplayer: could not connect to %s:%d." ), s.pending_address,
                     s.pending_port );
            enet_host_destroy( s.host );
            s.host = nullptr;
            return;
        }
        s.current = role::client;
        add_msg( m_info, _( "Multiplayer: connecting to %s:%d…" ), s.pending_address, s.pending_port );
    }
}

void shutdown()
{
    bubbles::close_all();
    session_state &s = state();
    // nothing of this connection is handled later (a reconnect asks for its own again)
    s.pending = role::none;
    impl::menu_queue().clear();
    if( s.host == nullptr ) {
        s.current = role::none;
        return;
    }
    for( size_t i = 0; i < s.host->peerCount; ++i ) {
        ENetPeer &peer = s.host->peers[i];
        if( peer.state == ENET_PEER_STATE_CONNECTED ) {
            enet_peer_disconnect( &peer, 0 );
        }
    }
    enet_host_flush( s.host );
    enet_host_destroy( s.host );
    s.host = nullptr;
    s.server_peer = nullptr;
    s.current = role::none;
}

void poll()
{
    session_state &s = state();
    if( s.host == nullptr ) {
        return;
    }
    // first what came in while a menu was open (client)
    while( !impl::menu_queue().empty() && s.host != nullptr ) {
        std::string data = std::move( impl::menu_queue().front() );
        impl::menu_queue().pop_front();
        if( data.empty() ) {
            client_on_disconnect();
        } else {
            impl::handle_data( s.server_peer, std::move( data ) );
        }
    }
    if( s.host == nullptr ) {
        return;
    }
    ENetEvent event;
    // Stop after a while so a flood of packets can not freeze the game.
    for( int handled = 0; handled < 256 && s.host != nullptr &&
         enet_host_service( s.host, &event, 0 ) > 0; ++handled ) {
        switch( event.type ) {
            case ENET_EVENT_TYPE_CONNECT:
                // Generous timeouts: the host may sit in a menu for a while.
                enet_peer_timeout( event.peer, 0, 30000, 120000 );
                if( s.current == role::host ) {
                    server_on_connect( event.peer );
                } else {
                    client_on_connect();
                }
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if( s.current == role::host ) {
                    server_on_disconnect( event.peer );
                } else {
                    client_on_disconnect();
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                handle_packet( event.peer, event.packet );
                enet_packet_destroy( event.packet );
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
    if( s.host != nullptr ) {
        enet_host_flush( s.host );
    }
}

void pace_turn()
{
    static std::chrono::steady_clock::time_point last_end = std::chrono::steady_clock::now();
    if( !is_host() || fluid::enabled() || everyone_asleep() ) {
        last_end = std::chrono::steady_clock::now();
        return;
    }
    const auto turn = std::chrono::milliseconds( static_cast<int>( 1000 * turn_duration(
                          get_option<float>( "TURN_DURATION" ) ) ) );
    while( std::chrono::steady_clock::now() - last_end < turn ) {
        poll();
        ui_manager::redraw_invalidated();
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    last_end = std::chrono::steady_clock::now();
}

float turn_duration( float option )
{
    if( !active() || option > 0.005f ) {
        return option;
    }
    // Continuous time: without the option set, one turn per second of real time.
    return 1.0f;
}

} // namespace net
