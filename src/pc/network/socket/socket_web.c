#ifdef TARGET_WEB

#include "socket.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <emscripten.h>
#include "pc/configfile.h"
#include "pc/debuglog.h"

// ---------------------------------------------------------------------------
// Virtual sockets on top of WebSocket (see socket_web.h)
//
// Synthetic address of a player:  ::ffff:10.0.0.<id>
//   id 1  = host (the one hosting the game)
//   id 2+ = clients (assigned by the relay)
// The last byte of the address IS the routing information: that way the game
// can keep comparing addresses with memcmp() as it always did.
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
    // If it is not one of our synthetic addresses (the player typed a real IP,
    // for instance), we assume it goes to the host: on the web there is only
    // "the host of the room".
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

// Opens (or retries opening) the WebSocket against the relay.
//   isHost: 1 if we are hosting, 0 if we are joining
//   room:   room code (host: "p<port>", client: text from the IP field)
//   relay:  "host:port" or "ws://host:port"; empty = automatic
EM_JS(int, webnet_js_begin, (int isHost, const char* roomC, const char* relayC), {
    var P = function (x) { return (typeof x === 'bigint') ? Number(x) : x; };
    var N = globalThis.__webnet;
    if (!N) { N = globalThis.__webnet = { ws: null, role: 0, inbox: [], outbox: [], status: "idle", url: "", tries: 0 }; }
    if (!N.outbox) { N.outbox = []; }
    N.role = isHost ? 1 : 2;
    N.inbox.length = 0;
    N.outbox.length = 0;
    N.tries = 0;

    var room = UTF8ToString(P(roomC));
    N.room = room;
    var relay = UTF8ToString(P(relayC));
    var cfg = relay || Module['WEB_RELAY'] || "";
    if (!cfg && typeof location !== 'undefined') {
        try { cfg = new URLSearchParams(location.search).get('relay') || ""; } catch (e) {}
    }
    if (!cfg && typeof location !== 'undefined') {
        // - http (local/LAN): the relay is usually on port 4321
        // - https (Cloudflare tunnel or a server in the cloud): the page and
        //   the relay live on the SAME host and port (443), so that is enough.
        cfg = (location.protocol === 'https:') ? location.host : (location.hostname + ':4321');
    }
    // we keep just "host:port" (we accept ws://, wss://, http:// or https:// being typed)
    cfg = String(cfg);
    var forcedScheme = null;
    var schemeAt = cfg.indexOf("://");
    if (schemeAt >= 0) {
        var scheme = cfg.substring(0, schemeAt).toLowerCase();
        forcedScheme = (scheme === "wss" || scheme === "https");
        cfg = cfg.substring(schemeAt + 3);
    }

    // TLS is used for anything that is not this machine or the LAN: a relay in the
    // cloud (Render, for instance) only accepts wss://. If the player typed a
    // scheme, that one wins.
    var relayHost = cfg.split("/")[0];
    if (relayHost.charAt(0) === "[") { relayHost = relayHost.substring(0, relayHost.indexOf("]") + 1); }
    else { relayHost = relayHost.split(":")[0]; }
    relayHost = relayHost.toLowerCase();
    var isLocal = relayHost === "localhost" || relayHost === "127.0.0.1" ||
                  relayHost === "::1" || relayHost === "[::1]" || relayHost === "0.0.0.0" ||
                  /^10\./.test(relayHost) || /^192\.168\./.test(relayHost) ||
                  /^172\.(1[6-9]|2[0-9]|3[01])\./.test(relayHost);
    var secure = (forcedScheme !== null)
        ? forcedScheme
        : ((typeof location !== 'undefined' && location.protocol === 'https:') || !isLocal);
    N.httpBase = (secure ? 'https://' : 'http://') + cfg;
    N.url = (secure ? 'wss://' : 'ws://') + cfg + '/ws?room=' + encodeURIComponent(room) + '&role=' + (isHost ? 'host' : 'client');

    function connect() {
        // If an old connection is still open, close it before opening another.
        // (Otherwise the relay would see two hosts of the same room and they
        //  would fight each other forever.)
        if (N.ws) {
            try { N.ws.onclose = null; N.ws.onmessage = null; N.ws.close(); } catch (e) {}
            N.ws = null;
        }
        var ws;
        try { ws = new WebSocket(N.url); } catch (e) { return retry('exception: ' + e); }
        ws.binaryType = 'arraybuffer';
        ws.onopen = function () {
            N.status = 'open';
            console.log('[webnet] relay connected: ' + N.url);
            // The game sends its first datagram (the mod list request) at the
            // very moment we connect, without waiting for the WebSocket to
            // finish opening: if we dropped it, the host would never answer and
            // the client would never manage to join. So we keep it and send it
            // now, in order.
            if (N.outbox && N.outbox.length > 0) {
                var pending = N.outbox.length;
                for (var i = 0; i < N.outbox.length; i++) {
                    try { ws.send(N.outbox[i]); } catch (e) { break; }
                }
                N.outbox.length = 0;
                console.log('[webnet] flushed ' + pending + ' queued datagrams');
            }
            // We only count the attempt as good if the connection lasts a
            // while. That way, if the relay closes it right away (the room is
            // taken, for instance), the attempt counter is not reset and we do
            // not keep trying forever.
            setTimeout(function () {
                if (N.ws === ws && ws.readyState === 1) { N.tries = 0; }
            }, 10000);
        };
        ws.onerror = function () { N.status = 'error'; };
        ws.onmessage = function (ev) {
            var b = new Uint8Array(ev.data);
            if (b.length < 1) { return; }
            // Control frame from the relay: the room we tried to host already has
            // a host. It comes as a normal binary frame (0xFF + "TAKEN") because
            // some proxies do not deliver the WebSocket close frame. 0xFF is
            // never a valid source id, so it cannot be confused with a datagram.
            if (b[0] === 0xff && b.length >= 6 && b[1] === 84 && b[2] === 65 && b[3] === 75 && b[4] === 69 && b[5] === 78) {
                N.roomTaken = 1;
                N.role = 0;
                console.log('[webnet] room "' + N.room + '" already has another host: press Join to enter it');
                try { ws.close(); } catch (e) {}
                return;
            }
            if (N.inbox.length > 8192) { return; } // flood protection
            N.inbox.push({ src: b[0], data: b.subarray(1) });
        };
        ws.onclose = function (ev) {
            if (N.ws === ws) { N.ws = null; }
            N.status = 'closed';
            if (ev && ev.code === 4001) {
                // The relay is telling us that this room already has another live
                // host: we stop insisting instead of fighting over the room in a
                // reconnection loop, and we raise a flag so the game can tell the
                // player to press Join (see webnet_room_was_taken).
                N.role = 0;
                N.roomTaken = 1;
                console.log('[webnet] room "' + N.room + '" already has another host: press Join to enter it');
                return;
            }
            retry('closed');
        };
        N.ws = ws;
    }

    function retry(why) {
        if (N.role === 0) { return; }
        if (N.tries >= 12) {
            console.log('[webnet] relay unreachable (' + why + '): ' + N.url + ' -> continuing WITHOUT network (single player mode)');
            return;
        }
        N.tries++;
        setTimeout(function () { if (N.role !== 0) { connect(); } }, 2500);
    }

    connect();
    console.log('[webnet] ' + (isHost ? 'HOST' : 'CLIENT') + ' room="' + room + '" url=' + N.url);
    return 1;
});

// Sends a datagram.  frame = [dst][0] + payload   (the relay fills in the source)
// Returns 0 if it was sent, -1 if there is no relay (the game ignores it: single player).
EM_JS(int, webnet_js_send, (int dst, double ptr, int len), {
    var N = globalThis.__webnet;
    if (!N) { return -1; }
    // NOTE: Emscripten 4 no longer publishes Module.HEAPxx. We use the local
    // view and, if it did not exist, the Module one (so it works on any version).
    var H8 = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module['HEAPU8'];
    if (!H8) { return -1; }
    var p = (typeof ptr === 'bigint') ? Number(ptr) : ptr;
    var frame = new Uint8Array(2 + len);
    frame[0] = dst & 0xff;  // 0 = all clients, 1 = host, N = client N
    frame[1] = 0;           // source: the relay fills it in
    frame.set(H8.subarray(p, p + len), 2);
    if (!N.ws || N.ws.readyState !== 1) {
        // The WebSocket is still opening (or it just dropped). We keep the
        // datagram to send it as soon as it connects: if we dropped it, the
        // client's first packet (the mod list request) would be lost EVERY time
        // and joining the host's room would never finish.
        if (!N.outbox) { N.outbox = []; }
        if (N.role !== 0 && N.outbox.length < 512) { N.outbox.push(frame); }
        return 0;
    }
    try { N.ws.send(frame); } catch (e) { return -1; }
    return 0;
});

// Takes a datagram out of the incoming queue.
// Returns 1 if there was one (and writes payload, src and length), 0 if not.
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

// Closes the connection (when leaving the game).
EM_JS(void, webnet_js_close, (int unused), {
    var N = globalThis.__webnet;
    if (!N) { return; }
    N.role = 0;
    if (N.ws) {
        try { N.ws.onclose = null; N.ws.close(); } catch (e) {}
        N.ws = null;
    }
    console.log('[webnet] relay closed');
});

// ------------------------ room list (for the join menu) --------------------

// Asks the relay for the list of open rooms (GET /rooms). It is asynchronous:
// afterwards webnet_rooms_state() has to be polled until it leaves 0.
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
            console.log("[webnet] " + N.rooms.length + " open rooms at " + N.httpBase);
        })
        .catch(function (e) {
            N.rooms = [];
            N.roomsState = 2;
            console.log("[webnet] could not read the room list (" + N.httpBase + "): " + e);
        });
});

// state: 0 = loading, 1 = list ready, 2 = error
EM_JS(int, webnet_js_rooms_state, (int unused), {
    var N = globalThis.__webnet;
    return (N && N.roomsState) ? N.roomsState : 0;
});

EM_JS(int, webnet_js_rooms_count, (int unused), {
    var N = globalThis.__webnet;
    return (N && N.rooms) ? N.rooms.length : 0;
});

// Copies the name of room i into a C buffer (and how many players are in it).
EM_JS(int, webnet_js_rooms_get, (int i, double ptr, int maxLen, double clientsPtr), {
    var N = globalThis.__webnet;
    var P = function (x) { return (typeof x === 'bigint') ? Number(x) : x; };
    // Emscripten 4 does not publish Module.HEAPxx: we use the module view.
    var H8 = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module['HEAPU8'];
    var cp = clientsPtr ? P(clientsPtr) : 0;
    if (H8 && cp) { new DataView(H8.buffer).setInt32(cp, 0, true); }
    if (!N || !N.rooms || i < 0 || i >= N.rooms.length) { return 0; }
    if (!H8 || maxLen <= 0) { return 0; }
    // We copy by hand (we do not depend on stringToUTF8, which may not be exported)
    var s = String(N.rooms[i].room);
    var q = P(ptr);
    var lim = Math.min(s.length, maxLen - 1);
    for (var k = 0; k < lim; k++) { H8[q + k] = s.charCodeAt(k) & 0xff; }
    H8[q + lim] = 0;
    if (cp) { new DataView(H8.buffer).setInt32(cp, (N.rooms[i].clients | 0) + 1, true); } // +1 = the host
    return 1;
});

// ---------------------------------- API C ----------------------------------

// Room name chosen in the "Create" menu (web build).
char gWebRoomName[64] = { 0 };

// PUBLIC room: the page is meant to be shared (a Discord link, for instance), so
// the default room has a fixed, easy name and everybody can get in: the host
// presses Create, the rest press Join (the field already suggests this name) or
// pick the room from the relay's list.
#define WEB_PUBLIC_ROOM "coop01"

const char* webnet_public_room(void) {
    return WEB_PUBLIC_ROOM;
}

// Set from JavaScript when the relay rejected us as host because the room already
// had one (reading it clears the flag).
EM_JS(int, webnet_js_room_taken, (), {
    var N = globalThis.__webnet;
    if (N && N.roomTaken) { N.roomTaken = 0; return 1; }
    return 0;
});

int webnet_room_was_taken(void) {
    return webnet_js_room_taken();
}

void webnet_ensure_room_name(void) {
    if (gWebRoomName[0] != '\0') { return; }
    // Public name by default. It can still be changed in the "Room" field of the
    // create game menu (handy if another host is already using the name).
    snprintf(gWebRoomName, sizeof(gWebRoomName), "%s", WEB_PUBLIC_ROOM);
}

void webnet_begin(int networkType) {
    // There are no real sockets: just one connection to the relay.
    char room[64] = { 0 };
    int isHost = (networkType == NT_SERVER) ? 1 : 0;
    if (isHost) {
        webnet_ensure_room_name();
        snprintf(room, sizeof(room), "%s", gWebRoomName);
    } else {
        snprintf(room, sizeof(room), "%s", configJoinIp);
    }
    if (room[0] == '\0') {
        // Without a room name there is nobody to talk to: we stay in single
        // player instead of opening a useless connection to the relay.
        LOG_INFO("webnet: no room name, going without network (single player mode)");
        return;
    }
    webnet_js_begin(isHost, room, "");
    LOG_INFO("webnet: %s (room '%s')", isHost ? "hosting" : "joining", room);
}

void webnet_rooms_fetch(void) { webnet_js_rooms_fetch(0); }
int  webnet_rooms_state(void) { return webnet_js_rooms_state(0); }
int  webnet_rooms_count(void) { return webnet_js_rooms_count(0); }

void webnet_rooms_get(int i, char* room, int roomMax, int* clients) {
    if (clients) { *clients = 0; }
    webnet_js_rooms_get(i, (double)(uintptr_t)room, roomMax, (double)(uintptr_t)clients);
}

SOCKET socket_initialize(void) {
    return 1; // fake handle: the WebSocket is opened in webnet_begin()
}

void socket_shutdown(SOCKET socket) {
    (void)socket;
    webnet_js_close(0);
}

int webnet_bind(unsigned int port) {
    LOG_INFO("webnet: virtual bind to port %u (relay room: '%s')", port, gWebRoomName);
    return NO_ERROR;
}

int webnet_send(struct sockaddr_in6* addr, const unsigned char* buffer, unsigned short length) {
    unsigned char dst = webnet_id_from_addr(addr);
    if (webnet_js_send(dst, (double)(uintptr_t)buffer, (int)length) != 0) {
        // No relay: there is nowhere to send it to. It is dropped silently.
        return NO_ERROR;
    }
    return NO_ERROR;
}

int webnet_receive(struct sockaddr_in6* rxAddr, unsigned char* buffer, unsigned short bufferLength, unsigned short* receiveLength) {
    int srcId = 0;
    int length = 0;
    *receiveLength = 0;
    if (!webnet_js_recv((double)(uintptr_t)buffer, (int)bufferLength, (double)(uintptr_t)&srcId, (double)(uintptr_t)&length)) {
        return SOCKET_ERROR; // nothing new (same as EWOULDBLOCK)
    }
    webnet_addr_from_id((unsigned char)(srcId & 0xff), rxAddr);
    *receiveLength = (unsigned short)length;
    return NO_ERROR;
}

#endif // TARGET_WEB
