#pragma once
#ifndef CATA_SRC_NET_PROTOCOL_H
#define CATA_SRC_NET_PROTOCOL_H

// Internal to the net_*.cpp files: message format and shared connection state.
//
// Every ENet packet is one JSON object with a "t" (type) member.  Channel 0 is reliable and
// ordered.  Channel 1 (protocol 2, fluid mode) is unreliable but sequenced: it carries things
// of which only the newest matters (the input of a client, the aim status).
//
// client -> host
//   {"t":"hello","v":<protocol_version>,"name":"<character name>"}
//   {"t":"act","a":"move","dx":<-1..1>,"dy":<-1..1>}   move, or attack what is there
//   {"t":"act","a":"wait"}
//   {"t":"act","a":"pickup"}                           pick up everything on own tile
//   channel 1, fluid mode only (see fluid.h): the whole input state, not a single key press
//   {"t":"in","mx":-1..1,"my":-1..1,"p":<bool>,"s":<bool>,"pc":<int>,"sc":<int>,"c":<bool>,
//    "cv":<bool>,"cx":<float>,"cy":<float>,"cz":<int>}
//     p/s: primary/secondary button held, pc/sc: running counters of presses, c: cancel,
//     cv/cx/cy/cz: the cursor in absolute tile coordinates (integer part = tile)
// host -> client
//   {"t":"welcome","v":<protocol_version>,"id":<npc id>,"fluid":<bool>}
//   {"t":"fx","list":[ {"k":<fluid::fx_type>,"a":<actor>,"f":[x,y,z],"o":[x,y,z],"d":<ms>,
//                        "w":<ms>,"x":<dx>,"y":<dy>,"h":<bool>} ... ]}
//   channel 1: {"t":"aim","s":<steadiness 0..1000>,"r":<spread in 1/1000 tile>,"f":<bool>,
//               "a":<bool aiming>}
//   {"t":"reject","reason":"<text>"}
//   {"t":"submaps","list":[ <submaps in map save file layout> ]}
//   {"t":"state","turn":<int>,"you":{...},"others":[...],"monsters":[...]}
//   {"t":"msg","text":"<text>"}

#include <string>

#include "net_session.h"

struct _ENetHost;
struct _ENetPeer;
class JsonObject;

namespace net
{

constexpr int protocol_version = 2;
constexpr int channel_count = 2;
constexpr int max_clients = 4;

namespace impl
{

struct session_state {
    role current = role::none;
    _ENetHost *host = nullptr;
    // client only: the connection to the host
    _ENetPeer *server_peer = nullptr;

    role pending = role::none;
    std::string pending_address;
    int pending_port = default_port;
};

session_state &state();

void send_json( _ENetPeer *peer, const std::string &json );
/** Channel 1: unreliable, sequenced. Old packets are dropped, never re-sent. */
void send_json_fast( _ENetPeer *peer, const std::string &json );

void server_on_start();
void server_on_connect( _ENetPeer *peer );
void server_on_disconnect( _ENetPeer *peer );
void server_on_message( _ENetPeer *peer, const JsonObject &msg );

void client_on_connect();
void client_on_disconnect();
void client_on_message( const JsonObject &msg );

} // namespace impl
} // namespace net

#endif // CATA_SRC_NET_PROTOCOL_H
