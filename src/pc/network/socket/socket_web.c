#ifdef TARGET_WEB

#include "socket.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <emscripten.h>
#include "pc/configfile.h"
#include "pc/debuglog.h"

// ---------------------------------------------------------------------------
// Sockets virtuales sobre WebSocket (ver socket_web.h)
//
// Direccion sintetica de un jugador:  ::ffff:10.0.0.<id>
//   id 1  = host (el que hospeda la partida)
//   id 2+ = clientes (los asigna el relay)
// El ultimo byte de la direccion ES la informacion de enrutado: asi el juego
// puede seguir comparando direcciones con memcmp() como siempre.
// ---------------------------------------------------------------------------

#define WEBNET_HOST_ID 1

static void webnet_addr_from_id(unsigned char id, struct sockaddr_in6* addr) {
    memset(addr, 0, sizeof(struct sockaddr_in6));
    addr->sin6_family = AF_INET6;
    addr->sin6_port = htons(1);
    addr->sin6_addr.s6_addr[10] = 0xff;
    addr->sin6_addr.s6_addr[11] = 0xff;
    addr->sin6_addr.s6_addr[15] = (id == 0) ? WEBNET_HOST_ID : id;
}

static unsigned char webnet_id_from_addr(const struct sockaddr_in6* addr) {
    if (addr == NULL) { return WEBNET_HOST_ID; }
    const unsigned char* b = addr->sin6_addr.s6_addr;
    // Si no es una direccion sintetica nuestra (p.ej. el jugador escribio una IP
    // real), asumimos que va al host: en web solo existe "el host de la sala".
    if (b[10] != 0xff || b[11] != 0xff || b[12] != 0 || b[13] != 0 || b[14] != 0) {
        return WEBNET_HOST_ID;
    }
    if (b[15] == 0) { return WEBNET_HOST_ID; }
    return b[15];
}

void webnet_make_host_addr(struct sockaddr_in6* addr) {
    webnet_addr_from_id(WEBNET_HOST_ID, addr);
}

// ---------------------------------- glue JS --------------------------------

// Abre (o reintenta abrir) el WebSocket contra el relay.
//   isHost: 1 si hospedamos, 0 si nos unimos
//   room:   codigo de sala (host: "p<puerto>", cliente: texto del campo IP)
//   relay:  "host:puerto" o "ws://host:puerto"; vacio = automatico
EM_JS(int, webnet_js_begin, (int isHost, const char* roomC, const char* relayC), {
    var P = function (x) { return (typeof x === 'bigint') ? Number(x) : x; };
    var N = globalThis.__webnet;
    if (!N) { N = globalThis.__webnet = { ws: null, role: 0, inbox: [], status: "idle", url: "", tries: 0 }; }
    N.role = isHost ? 1 : 2;
    N.inbox.length = 0;
    N.tries = 0;

    var room = UTF8ToString(P(roomC));
    N.room = room;
    var relay = UTF8ToString(P(relayC));
    var cfg = relay || Module['WEB_RELAY'] || "";
    if (!cfg && typeof location !== 'undefined') {
        try { cfg = new URLSearchParams(location.search).get('relay') || ""; } catch (e) {}
    }
    if (!cfg && typeof location !== 'undefined') {
        // - http (local/LAN): el relay suele estar en el puerto 4321
        // - https (tunel de Cloudflare o servidor en la nube): la pagina y el
        //   relay van en el MISMO host y puerto (443), asi que basta con eso.
        cfg = (location.protocol === 'https:') ? location.host : (location.hostname + ':4321');
    }
    // nos quedamos con "host:puerto" (aceptamos que escriban ws:// o https://)
    cfg = String(cfg);
    if (cfg.indexOf("://") >= 0) { cfg = cfg.substring(cfg.indexOf("://") + 3); }
    var secure = (typeof location !== 'undefined' && location.protocol === 'https:');
    N.httpBase = (secure ? 'https://' : 'http://') + cfg;
    N.url = (secure ? 'wss://' : 'ws://') + cfg + '/ws?room=' + encodeURIComponent(room) + '&role=' + (isHost ? 'host' : 'client');

    function connect() {
        // Si quedo alguna conexion vieja abierta, la cerramos antes de abrir otra.
        // (Si no, el relay veria dos hosts de la misma sala y pelearian para siempre.)
        if (N.ws) {
            try { N.ws.onclose = null; N.ws.onmessage = null; N.ws.close(); } catch (e) {}
            N.ws = null;
        }
        var ws;
        try { ws = new WebSocket(N.url); } catch (e) { return retry('excepcion: ' + e); }
        ws.binaryType = 'arraybuffer';
        ws.onopen = function () {
            N.status = 'open';
            console.log('[webnet] relay conectado: ' + N.url);
            // Solo damos por valido el intento si la conexion aguanta un rato.
            // Asi, si el relay la cierra enseguida (p.ej. sala ocupada), el
            // contador de intentos no se reinicia y no intentamos para siempre.
            setTimeout(function () {
                if (N.ws === ws && ws.readyState === 1) { N.tries = 0; }
            }, 10000);
        };
        ws.onerror = function () { N.status = 'error'; };
        ws.onmessage = function (ev) {
            var b = new Uint8Array(ev.data);
            if (b.length < 1) { return; }
            if (N.inbox.length > 8192) { return; } // proteccion anti-saturacion
            N.inbox.push({ src: b[0], data: b.subarray(1) });
        };
        ws.onclose = function (ev) {
            if (N.ws === ws) { N.ws = null; }
            N.status = 'closed';
            if (ev && ev.code === 4001) {
                // El relay nos dice que esa sala ya tiene otro host vivo: dejamos
                // de insistir (seguimos jugando en solitario) en vez de pelearnos
                // por la sala en un bucle de reconexiones.
                N.role = 0;
                console.log('[webnet] la sala "' + N.room + '" ya tiene otro host: sigo sin relay');
                return;
            }
            retry('cerrado');
        };
        N.ws = ws;
    }

    function retry(why) {
        if (N.role === 0) { return; }
        if (N.tries >= 12) {
            console.log('[webnet] relay inalcanzable (' + why + '): ' + N.url + ' -> seguimos SIN red (modo solitario)');
            return;
        }
        N.tries++;
        setTimeout(function () { if (N.role !== 0) { connect(); } }, 2500);
    }

    connect();
    console.log('[webnet] ' + (isHost ? 'HOST' : 'CLIENTE') + ' sala="' + room + '" url=' + N.url);
    return 1;
});

// Envia un datagrama.  frame = [dst][0] + payload   (el relay rellena el origen)
// Devuelve 0 si se envio, -1 si no hay relay (el juego lo ignora: modo solitario).
EM_JS(int, webnet_js_send, (int dst, double ptr, int len), {
    var N = globalThis.__webnet;
    if (!N || !N.ws || N.ws.readyState !== 1) { return -1; }
    // OJO: Emscripten 4 ya NO publica Module.HEAPxx. Usamos la vista local y, si
    // no existiera, la del Module (asi vale en cualquier version).
    var H8 = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module['HEAPU8'];
    if (!H8) { return -1; }
    var p = (typeof ptr === 'bigint') ? Number(ptr) : ptr;
    var frame = new Uint8Array(2 + len);
    frame[0] = dst & 0xff;  // 0 = todos los clientes, 1 = host, N = cliente N
    frame[1] = 0;           // origen: lo pone el relay
    frame.set(H8.subarray(p, p + len), 2);
    try { N.ws.send(frame); } catch (e) { return -1; }
    return 0;
});

// Saca un datagrama de la cola de entrada.
// Devuelve 1 si habia algo (y escribe payload, src y longitud), 0 si no.
EM_JS(int, webnet_js_recv, (double ptr, int maxLen, double srcPtr, double lenPtr), {
    var N = globalThis.__webnet;
    if (!N || N.inbox.length === 0) { return 0; }
    var H8 = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module['HEAPU8'];
    if (!H8) { return 0; }
    var m = N.inbox.shift();
    var P = function (x) { return (typeof x === 'bigint') ? Number(x) : x; };
    var p  = P(ptr);
    var n = m.data.length;
    if (n > maxLen) { n = maxLen; }
    H8.set(m.data.subarray(0, n), p);
    var dv = new DataView(H8.buffer);
    dv.setInt32(P(srcPtr), m.src | 0, true);
    dv.setInt32(P(lenPtr), n | 0, true);
    return 1;
});

// Cierra la conexion (al salir de la partida).
EM_JS(void, webnet_js_close, (int unused), {
    var N = globalThis.__webnet;
    if (!N) { return; }
    N.role = 0;
    if (N.ws) {
        try { N.ws.onclose = null; N.ws.close(); } catch (e) {}
        N.ws = null;
    }
    console.log('[webnet] relay cerrado');
});

// --------------------- lista de salas (para el menu de unirse) --------------

// Le pide al relay la lista de salas abiertas (GET /rooms). Es asincrono:
// despues hay que mirar webnet_rooms_state() hasta que deje de estar en 0.
EM_JS(void, webnet_js_rooms_fetch, (int unused), {
    var N = globalThis.__webnet;
    if (!N) { N = globalThis.__webnet = { ws: null, role: 0, inbox: [], status: "idle", url: "", tries: 0, rooms: [], roomsState: 0 }; }
    if (!N.httpBase) {
        var cfg = Module['WEB_RELAY'] || "";
        if (!cfg && typeof location !== "undefined") { try { cfg = new URLSearchParams(location.search).get("relay") || ""; } catch (e) {} }
        if (!cfg && typeof location !== "undefined") { cfg = location.hostname + ":4321"; }
        cfg = String(cfg);
        if (cfg.indexOf("://") >= 0) { cfg = cfg.substring(cfg.indexOf("://") + 3); }
        var secure = (typeof location !== "undefined" && location.protocol === "https:");
        N.httpBase = (secure ? "https://" : "http://") + cfg;
    }
    N.roomsState = 0;
    fetch(N.httpBase + "/rooms")
        .then(function (r) { if (!r.ok) { throw new Error("HTTP " + r.status); } return r.json(); })
        .then(function (list) {
            N.rooms = (list || []).map(function (x) { return { room: String(x.room || ""), clients: x.clients | 0 }; });
            N.roomsState = 1;
            console.log("[webnet] " + N.rooms.length + " salas abiertas en " + N.httpBase);
        })
        .catch(function (e) {
            N.rooms = [];
            N.roomsState = 2;
            console.log("[webnet] no pude leer la lista de salas (" + N.httpBase + "): " + e);
        });
});

// estado: 0 = cargando, 1 = lista, 2 = error
EM_JS(int, webnet_js_rooms_state, (int unused), {
    var N = globalThis.__webnet;
    return (N && N.roomsState) ? N.roomsState : 0;
});

EM_JS(int, webnet_js_rooms_count, (int unused), {
    var N = globalThis.__webnet;
    return (N && N.rooms) ? N.rooms.length : 0;
});

// Copia el nombre de la sala i a un buffer de C (y cuantos jugadores hay dentro).
EM_JS(int, webnet_js_rooms_get, (int i, double ptr, int maxLen, double clientsPtr), {
    var N = globalThis.__webnet;
    var P = function (x) { return (typeof x === 'bigint') ? Number(x) : x; };
    // Emscripten 4 no publica Module.HEAPxx: usamos la vista del modulo.
    var H8 = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module['HEAPU8'];
    var cp = clientsPtr ? P(clientsPtr) : 0;
    if (H8 && cp) { new DataView(H8.buffer).setInt32(cp, 0, true); }
    if (!N || !N.rooms || i < 0 || i >= N.rooms.length) { return 0; }
    if (!H8 || maxLen <= 0) { return 0; }
    // Copiamos a mano (no dependemos de stringToUTF8, que puede no estar exportada)
    var s = String(N.rooms[i].room);
    var q = P(ptr);
    var lim = Math.min(s.length, maxLen - 1);
    for (var k = 0; k < lim; k++) { H8[q + k] = s.charCodeAt(k) & 0xff; }
    H8[q + lim] = 0;
    if (cp) { new DataView(H8.buffer).setInt32(cp, (N.rooms[i].clients | 0) + 1, true); } // +1 = el host
    return 1;
});

// ---------------------------------- API C ----------------------------------

// Nombre de sala elegido en el menu "Crear" (build web).
char gWebRoomName[64] = { 0 };

void webnet_ensure_room_name(void) {
    if (gWebRoomName[0] != '\0') { return; }
    // Nombre por defecto: facil de leer y de decir en voz alta. Se puede cambiar
    // en el campo "Sala" del menu de crear partida.
    unsigned int n = (unsigned int)(emscripten_get_now() * 7.0) % 1000u;
    snprintf(gWebRoomName, sizeof(gWebRoomName), "sala%03u", n);
}

void webnet_begin(int networkType) {
    // No hay sockets reales: solo una conexion al relay.
    char room[64] = { 0 };
    int isHost = (networkType == NT_SERVER) ? 1 : 0;
    if (isHost) {
        webnet_ensure_room_name();
        snprintf(room, sizeof(room), "%s", gWebRoomName);
    } else {
        snprintf(room, sizeof(room), "%s", configJoinIp);
    }
    if (room[0] == '\0') {
        // Sin nombre de sala no hay con quien hablar: seguimos en solitario en
        // vez de abrir una conexion inutil al relay.
        LOG_INFO("webnet: sin nombre de sala, sigo sin red (modo solitario)");
        return;
    }
    webnet_js_begin(isHost, room, "");
    LOG_INFO("webnet: %s (sala '%s')", isHost ? "hospedando" : "uniendose", room);
}

void webnet_rooms_fetch(void) { webnet_js_rooms_fetch(0); }
int  webnet_rooms_state(void) { return webnet_js_rooms_state(0); }
int  webnet_rooms_count(void) { return webnet_js_rooms_count(0); }

void webnet_rooms_get(int i, char* room, int roomMax, int* clients) {
    if (clients) { *clients = 0; }
    webnet_js_rooms_get(i, (double)(uintptr_t)room, roomMax, (double)(uintptr_t)clients);
}

SOCKET socket_initialize(void) {
    return 1; // handle ficticio: el WebSocket se abre en webnet_begin()
}

void socket_shutdown(SOCKET socket) {
    (void)socket;
    webnet_js_close(0);
}

int webnet_bind(unsigned int port) {
    LOG_INFO("webnet: bind virtual al puerto %u (sala del relay: '%s')", port, gWebRoomName);
    return NO_ERROR;
}

int webnet_send(struct sockaddr_in6* addr, const unsigned char* buffer, unsigned short length) {
    unsigned char dst = webnet_id_from_addr(addr);
    if (webnet_js_send(dst, (double)(uintptr_t)buffer, (int)length) != 0) {
        // Sin relay: no hay a donde enviar. Se descarta en silencio.
        return NO_ERROR;
    }
    return NO_ERROR;
}

int webnet_receive(struct sockaddr_in6* rxAddr, unsigned char* buffer, unsigned short bufferLength, unsigned short* receiveLength) {
    int srcId = 0;
    int length = 0;
    *receiveLength = 0;
    if (!webnet_js_recv((double)(uintptr_t)buffer, (int)bufferLength, (double)(uintptr_t)&srcId, (double)(uintptr_t)&length)) {
        return SOCKET_ERROR; // nada nuevo (equivale a EWOULDBLOCK)
    }
    webnet_addr_from_id((unsigned char)(srcId & 0xff), rxAddr);
    *receiveLength = (unsigned short)length;
    return NO_ERROR;
}

#endif // TARGET_WEB
