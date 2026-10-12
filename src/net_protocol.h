#pragma once
#ifndef CATA_SRC_NET_PROTOCOL_H
#define CATA_SRC_NET_PROTOCOL_H

// Internal to the net_*.cpp files: message format and shared connection state.
//
// Every ENet packet is one JSON object with a "t" (type) member, sent reliably on channel 0.
//
// client -> host
//   {"t":"hello","v":<protocol_version>,"name":"<character name>"}
//   {"t":"act","a":"move","dx":<-1..1>,"dy":<-1..1>}   move, or attack what is there
//   {"t":"act","a":"wait"}
//   {"t":"act","a":"pickup"}                           pick up everything on own tile
// host -> client
//   {"t":"welcome","v":<protocol_version>,"id":<npc id>}
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

constexpr int protocol_version = 1;
constexpr int max_clients = 4;

namespace detail
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

void server_on_connect( _ENetPeer *peer );
void server_on_disconnect( _ENetPeer *peer );
void server_on_message( _ENetPeer *peer, const JsonObject &msg );

void client_on_connect();
void client_on_disconnect();
void client_on_message( const JsonObject &msg );

} // namespace detail
} // namespace net

#endif // CATA_SRC_NET_PROTOCOL_H
