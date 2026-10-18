#ifndef SOCKET_WEB_H
#define SOCKET_WEB_H

// ---------------------------------------------------------------------------
// Transporte de red para el build web (Emscripten / WebAssembly).
//
// El juego habla su propio protocolo sobre datagramas UDP, pero un navegador
// NO puede abrir sockets UDP. La solucion es un "socket virtual": a cada
// jugador se le asigna una direccion sintetica (::ffff:10.0.0.<id>, donde
// id=1 es siempre el host y 2..N los clientes) y los datagramas viajan por
// WebSocket hasta un relay (servidor tonto, p.ej. en Render) que los reenvia:
//
//     cliente -> host        (el relay marca el id del cliente como origen)
//     host    -> clientes    (broadcast o a un cliente concreto)
//
// El relay no entiende el juego: solo mueve bytes. Toda la logica (paquetes,
// reconexiones, estado) sigue siendo la del juego.
//
// Si no hay relay disponible, el juego NO se bloquea: los envios se descartan
// y no llega nada, que es exactamente "partida con 0 clientes" (modo solitario).
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

// Backend web (lo usa socket.c)
void webnet_begin(int networkType);
int  webnet_bind(unsigned int port);
int  webnet_send(struct sockaddr_in6* addr, const unsigned char* buffer, unsigned short length);
int  webnet_receive(struct sockaddr_in6* rxAddr, unsigned char* buffer, unsigned short bufferLength, unsigned short* receiveLength);
void webnet_make_host_addr(struct sockaddr_in6* addr);

// Menu web: nombre de sala y lista de salas del relay
extern char gWebRoomName[64];
void webnet_ensure_room_name(void);
void webnet_rooms_fetch(void);                                 // pide la lista al relay (asincrono)
int  webnet_rooms_state(void);                                 // 0 = cargando, 1 = lista, 2 = error
int  webnet_rooms_count(void);
void webnet_rooms_get(int i, char* room, int roomMax, int* clients);

#endif
