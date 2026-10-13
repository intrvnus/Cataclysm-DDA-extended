#include "net_session.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include <enet/enet.h>

#include "debug.h"
#include "flexbuffer_json.h"
#include "fluid.h"
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

static void handle_packet( ENetPeer *peer, const ENetPacket *packet )
{
    session_state &s = state();
    try {
        std::string data( reinterpret_cast<const char *>( packet->data ), packet->dataLength );
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

} // namespace impl

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

role current_role()
{
    return state().current;
}

void start_pending()
{
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
            return;
        }
        s.current = role::host;
        server_on_start();
        add_msg( m_good, _( "Multiplayer: hosting on port %d.  Waiting for players…" ),
                 s.pending_port );
    } else {
        s.host = enet_host_create( nullptr, 1, channel_count, 0, 0 );
        if( s.host == nullptr ) {
            add_msg( m_bad, _( "Multiplayer: could not create a network connection." ) );
            return;
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
    session_state &s = state();
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
