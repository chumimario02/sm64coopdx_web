#ifndef SOCKET_WEB_H
#define SOCKET_WEB_H

// ---------------------------------------------------------------------------
// Network transport for the web build (Emscripten / WebAssembly).
//
// The game speaks its own protocol over UDP datagrams, but a browser CANNOT
// open UDP sockets. The solution is a "virtual socket": every player is given a
// synthetic address (::ffff:10.0.0.<id>, where id=1 is always the host and
// 2..N are the clients) and the datagrams travel over WebSocket to a relay
// (a dumb server, e.g. on Render) that forwards them:
//
//     client -> host        (the relay tags the client's id as the source)
//     host   -> clients     (broadcast or to a particular client)
//
// The relay does not understand the game: it only moves bytes. All the logic
// (packets, reconnections, state) is still the game's.
//
// If no relay is available the game does NOT block: sends are dropped and
// nothing arrives, which is exactly "a game with 0 clients" (single player).
// ---------------------------------------------------------------------------

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define SOCKET unsigned int
#define INVALID_SOCKET (unsigned int)(-1)
#define SOCKET_LAST_ERROR 0
#define NO_ERROR (0)
#define SOCKADDR void
#define SOCKET_ERROR (-1)
#define closesocket(fd) ((void)0)
#define SOCKET_EWOULDBLOCK 100
#define SOCKET_ECONNRESET 101
#define RX_ADDR_SIZE_TYPE unsigned int

SOCKET socket_initialize(void);
void socket_shutdown(SOCKET socket);

// Web backend (used by socket.c)
void webnet_begin(int networkType);
int  webnet_bind(unsigned int port);
int  webnet_send(struct sockaddr_in6* addr, const unsigned char* buffer, unsigned short length);
int  webnet_receive(struct sockaddr_in6* rxAddr, unsigned char* buffer, unsigned short bufferLength, unsigned short* receiveLength);
void webnet_make_host_addr(struct sockaddr_in6* addr);

// Web menu: room name and room list from the relay
extern char gWebRoomName[64];
void webnet_ensure_room_name(void);
void webnet_rooms_fetch(void);                                 // asks the relay for the list (asynchronous)
int  webnet_rooms_state(void);                                 // 0 = loading, 1 = list ready, 2 = error
int  webnet_rooms_count(void);
void webnet_rooms_get(int i, char* room, int roomMax, int* clients);

#endif
