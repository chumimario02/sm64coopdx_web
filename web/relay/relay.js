#!/usr/bin/env node
/*
 * relay.js - WebSocket relay for sm64coopdx-web (multiplayer between browsers only).
 *
 * A browser cannot open UDP sockets, so the game carries a "virtual socket" on
 * top of WebSocket. This relay is dumb on purpose: it does not understand the
 * game, it only forwards bytes in a star shape.
 *
 * Protocol of every binary frame:
 *     [dst][src][payload...]     (from a client to the relay; src is 0)
 *     [src][payload...]          (from the relay to a client)
 *   dst: 0 = all clients, 1 = host, N = client N
 *   src: always set by the relay (1 = host, 2+ = clients of that room)
 *
 * Topology:
 *     client -> relay -> host        (a room has ONE host)
 *     host   -> relay -> clients     (broadcast, or to a particular one)
 *
 * No dependencies: it implements the WebSocket handshake and framing by hand
 * (small binary frames only) so it can be deployed anywhere (Render included)
 * with "node relay.js".
 */

const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const PORT = process.env.PORT || 4321;
const MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

// Folder with the game page (build/us_pc). If it exists, the relay also serves
// the files: that way the game and the relay use the SAME port and a single
// tunnel (cloudflared, for instance) is enough to play online with friends.
//   node relay.js [static_folder]
const STATIC_DIR = process.env.STATIC_DIR || process.argv[2] || '';

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.wasm': 'application/wasm',
    '.data': 'application/octet-stream',
    '.png': 'image/png',
    '.ico': 'image/x-icon',
    '.json': 'application/json; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.txt': 'text/plain; charset=utf-8',
};

function serveStatic(req, res) {
    let rel = decodeURIComponent(req.url.split('?')[0]);
    if (rel === '/' || rel === '') { rel = '/index.html'; }
    const full = path.join(STATIC_DIR, path.normalize(rel).replace(/^([/\\])+/, ''));
    if (!full.startsWith(path.resolve(STATIC_DIR))) { res.writeHead(403); res.end(); return; }
    fs.stat(full, (err, st) => {
        if (err || !st.isFile()) {
            // it is a directory: look for the game html
            const alt = path.join(full, 'sm64coopdx.html');
            fs.stat(alt, (e2, s2) => {
                if (e2 || !s2.isFile()) { res.writeHead(404, { 'Content-Type': 'text/plain' }); res.end('not found: ' + rel); return; }
                sendFile(alt, res);
            });
            return;
        }
        sendFile(full, res);
    });
}

function sendFile(file, res) {
    const type = MIME[path.extname(file).toLowerCase()] || 'application/octet-stream';
    res.writeHead(200, { 'Content-Type': type, 'Cache-Control': 'no-cache' });
    fs.createReadStream(file).pipe(res);
}

/** room -> { room, host, clients: Map<id, peer>, nextId } */
const rooms = new Map();

// Control frame for "the room already has another host". It travels as a normal
// binary frame because some proxies (Render's, for instance) do not deliver the
// WebSocket close frame the relay sends right after the handshake. 0xFF is never
// a valid source id, so the game can tell it apart from a real datagram.
const TAKEN_FRAME = Buffer.concat([Buffer.from([0xff]), Buffer.from('TAKEN', 'utf8')]);

function log(...args) {
    console.log(new Date().toISOString().slice(11, 19), ...args);
}

// ---------------------------- WebSocket framing ----------------------------

function writeFrame(peer, opcode, payload) {
    const len = payload.length;
    let header;
    if (len < 126) {
        header = Buffer.allocUnsafe(2);
        header[0] = 0x80 | opcode;
        header[1] = len;
    } else if (len < 65536) {
        header = Buffer.allocUnsafe(4);
        header[0] = 0x80 | opcode;
        header[1] = 126;
        header.writeUInt16BE(len, 2);
    } else {
        header = Buffer.allocUnsafe(10);
        header[0] = 0x80 | opcode;
        header[1] = 127;
        header.writeBigUInt64BE(BigInt(len), 2);
    }
    try {
        peer.socket.write(Buffer.concat([header, payload]));
    } catch (e) {
        log('write error:', e.message);
    }
}

function onData(peer, chunk) {
    peer.buf = peer.buf && peer.buf.length ? Buffer.concat([peer.buf, chunk]) : chunk;
    for (;;) {
        const b = peer.buf;
        if (b.length < 2) return;

        const opcode = b[0] & 0x0f;
        const masked = (b[1] & 0x80) !== 0;
        let len = b[1] & 0x7f;
        let off = 2;

        if (len === 126) {
            if (b.length < 4) return;
            len = b.readUInt16BE(2);
            off = 4;
        } else if (len === 127) {
            if (b.length < 10) return;
            len = Number(b.readBigUInt64BE(2));
            off = 10;
        }

        let maskKey = null;
        if (masked) {
            if (b.length < off + 4) return;
            maskKey = b.subarray(off, off + 4);
            off += 4;
        }

        if (b.length < off + len) return; // incomplete frame: wait for more data

        const payload = Buffer.from(b.subarray(off, off + len));
        if (maskKey) {
            for (let i = 0; i < len; i++) { payload[i] ^= maskKey[i % 4]; }
        }
        peer.buf = b.subarray(off + len);

        if (opcode === 0x2) {            // binary = game datagram
            route(peer, payload);
        } else if (opcode === 0x8) {     // close
            peer.socket.end();
            return;
        } else if (opcode === 0x9) {     // ping
            writeFrame(peer, 0xa, payload);
        }
        // 0x1 (text) and 0xa (pong) are ignored
    }
}

// ------------------------------- routing ----------------------------------

function route(peer, payload) {
    const r = peer.room;
    if (!r || payload.length < 2) return;

    const dst = payload[0];
    const body = payload.subarray(2);

    if (peer.isHost) {
        // from the host to the clients (broadcast or to a single one)
        if (dst === 0) {
            for (const c of r.clients.values()) { sendToPeer(c, 1, body); }
        } else {
            const c = r.clients.get(dst);
            if (c) { sendToPeer(c, 1, body); }
        }
    } else {
        // from the client to the host
        if (r.host) { sendToPeer(r.host, peer.id, body); }
    }
}

function sendToPeer(peer, src, payload) {
    if (!peer) return;
    const out = Buffer.allocUnsafe(1 + payload.length);
    out[0] = src & 0xff;
    payload.copy(out, 1);
    writeFrame(peer, 0x2, out);
}

// ------------------------------ HTTP + upgrade ----------------------------

const server = http.createServer((req, res) => {
    if (req.url === '/' && !STATIC_DIR) {
        res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('relay sm64coopdx-web OK\nrooms: ' + rooms.size + '\n');
        return;
    }
    if (req.url.startsWith('/healthz')) {
        res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('relay sm64coopdx-web OK\nrooms: ' + rooms.size + '\n');
        return;
    }
    if (req.url.startsWith('/rooms')) {
        const list = [];
        for (const r of rooms.values()) {
            if (!r.host) continue; // only playable rooms
            list.push({ room: r.room, clients: r.clients.size, max: 32 });
        }
        res.writeHead(200, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
        res.end(JSON.stringify(list));
        return;
    }
    if (STATIC_DIR) { serveStatic(req, res); return; }
    res.writeHead(404);
    res.end();
});

server.on('upgrade', (req, socket) => {
    const key = req.headers['sec-websocket-key'];
    if (!key) { socket.destroy(); return; }

    const url = new URL(req.url, 'http://relay');
    if (url.pathname !== '/ws') { socket.destroy(); return; }

    const roomName = (url.searchParams.get('room') || '').slice(0, 40);
    const wantsHost = url.searchParams.get('role') === 'host';
    if (!roomName) { socket.destroy(); return; }

    const accept = crypto.createHash('sha1').update(key + MAGIC).digest('base64');
    socket.write(
        'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n'
    );
    socket.setNoDelay(true);

    let r = rooms.get(roomName);
    if (!r) { r = { room: roomName, host: null, clients: new Map(), nextId: 2 }; rooms.set(roomName, r); }

    const peer = { socket, buf: null, room: r, isHost: wantsHost, id: 0 };

    if (wantsHost) {
        const old = r.host;
        const oldAlive = old && old.socket && !old.socket.destroyed && !old.socket.writableEnded && old.socket.writable;
        if (oldAlive) {
            // The room already has a live host: we reject the second one so two
            // people do not "steal" the room in a reconnection loop.
            log(`[${roomName}] room taken: rejecting the second host`);
            // First the control frame (this is what the game looks at, it always
            // arrives), and a moment later the proper WebSocket close.
            try { writeFrame(peer, 0x2, TAKEN_FRAME); } catch (e) {}
            setTimeout(() => {
                const reason = Buffer.from('room taken', 'utf8');
                const payload = Buffer.alloc(2 + reason.length);
                payload.writeUInt16BE(4001, 0);
                reason.copy(payload, 2);
                try { writeFrame(peer, 0x8, payload); } catch (e) {}
                try { socket.end(); } catch (e) {}
            }, 250);
            return;
        }
        if (old) { log(`[${roomName}] the previous host was down: replacing it`); }
        r.host = peer;
        peer.id = 1;
        log(`[${roomName}] HOST connected (active rooms: ${rooms.size})`);
    } else {
        peer.id = r.nextId++;
        r.clients.set(peer.id, peer);
        log(`[${roomName}] client ${peer.id} connected (${r.clients.size} in the room${r.host ? '' : ', NO host'})`);
    }

    socket.on('data', (chunk) => onData(peer, chunk));
    socket.on('error', () => {});
    socket.on('close', () => {
        if (peer.isHost) {
            if (r.host === peer) { r.host = null; }
        } else {
            r.clients.delete(peer.id);
        }
        if (!r.host && r.clients.size === 0) { rooms.delete(roomName); }
        log(`[${roomName}] ${peer.isHost ? 'HOST' : 'client ' + peer.id} disconnected`);
    });
});

server.listen(PORT, () => log('relay listening on port ' + PORT));
