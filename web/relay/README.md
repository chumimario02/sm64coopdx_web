# Room relay for sm64coopdx-web

A dumb server that **forwards packets** between browsers (a browser cannot use UDP)
and, on the side, **serves the game page**, so a single process and a single port
are all you need to play.

```
node relay.js [game_folder]
```

* No folder: it is only a relay (`/rooms` gives the room list, `/healthz` says
  whether it is alive).
* With a folder (e.g. `build/us_pc`): it also serves the page at
  `http://localhost:4321/sm64coopdx.html`.

## Frame protocol

Every binary WebSocket message is:

```
[destination][source][data...]     (from a browser to the relay; source = 0)
[source][data...]                  (from the relay to a browser)
```

* `destination`: `0` = all clients, `1` = host, `N` = client N.
* `source`: always set by the relay (`1` = host, `2+` = clients of that room).

The game knows nothing about any of this: it believes it is talking UDP to
addresses `::ffff:10.0.0.<id>` (the `id` is exactly that `source`). The transport
code is in `src/pc/network/socket/socket_web.c`.

## Two ways to use it

### 1. Locally / on your LAN (the usual one)

```bash
node relay.js C:/path/to/sm64coopdx_web/build/us_pc
```

* You: `http://127.0.0.1:4321/sm64coopdx.html` → **Create** → tell your friends the
  room name.
* Friends (same network): `http://YOUR-LOCAL-IP:4321/sm64coopdx.html` → **Join** →
  your room shows up in the list → click it.
* If they cannot see each other, port 4321 has to be allowed in the Windows
  firewall.

### 2. Over the internet (tunnel, no router ports opened)

```bash
cloudflared tunnel --url http://localhost:4321
```

It gives you a `https://something.trycloudflare.com` address. Your friends open
that address and that is it: the page and the relay travel through the same tunnel
(Cloudflare supports WebSocket). The game finds the relay on its own (it uses the
same host as the page).

### 3. In the cloud (Render / Railway / Fly) — optional

Those services can host **the relay** (not the game with a ROM). With the
**ROM-less** version (every player places their own in the browser) that makes
perfect sense:

* New → Web Service → this repository, with `web/relay/relay.js` + `package.json`
* Build: `npm install` · Start: `npm start` (it uses `process.env.PORT`)
* The page points at the relay with `?relay=your-relay.onrender.com`

## Notes

* Render's free plan puts the service to sleep after a while without visitors: the
  first player to join waits ~30-60 s for it to wake up (the game retries the
  connection on its own for ~30 s).
* One room = one host. If two people use the same room name on the same relay, the
  second one replaces the first.
* Packets larger than `MAX_PAYLOAD` bytes are dropped (the game never sends
  anything that big).
