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

* You: `http://127.0.0.1:4321/sm64coopdx.html` → **Create** (the room is `coop01`
  by default, and you can rename it).
* Friends (same network): `http://YOUR-LOCAL-IP:4321/sm64coopdx.html` → **Join** →
  your room shows up in the list (with the number of players) → click it.
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

### 3. In the cloud (Render) — the usual setup for playing with friends

Render can host **the relay** for free (never the game with a ROM). The repository
already brings a blueprint for it: [`render.yaml`](../../render.yaml).

1. Push this repository to GitHub.
2. In Render: **New +** → **Blueprint** → pick this repository → **Apply**.
   Render creates the free service `sm64coopdx-relay` (start: `node relay.js`,
   health check: `/healthz`).
   *Manual alternative:* New + → **Web Service** → this repository → Root directory
   `web/relay`, Build command `npm install --omit=dev`, Start command `node relay.js`,
   instance type **Free**.
3. Wait for the first deploy and check that the relay is alive:
   `https://<name>.onrender.com/healthz` has to answer `relay sm64coopdx-web OK`.
4. Open the game page pointing at that relay:
   `http://127.0.0.1:4321/sm64coopdx.html?relay=<name>.onrender.com`
   (the browser remembers the address, so you only type it once; a scheme can be
   given as well: `?relay=wss://<name>.onrender.com`).
5. Your friends open the **same page URL** (with the same `?relay=`) and press **Join**:
   the field already suggests the public room `coop01` and the room list shows your room.

The game switches to `wss://` on its own for any relay that is not on your own
machine or your LAN, which is exactly what Render needs.

## Notes

* The free plan puts the service to sleep after a while without visitors: the first
  player waits ~30-60 s for it to wake up (the game keeps retrying the connection
  for ~30 s, so open the room and wait a moment before giving up).
* One room = one host. If two people try to host the same name on the same relay,
  the second one is rejected (`4001 room taken`) and the game tells the player to
  press *Join* instead of silently leaving them alone in single player.
* The page invites everybody into the public room `coop01`: the *Join* field already
  suggests that name and the room list shows the open rooms of all the players
  (host included), with their player count.
* Packets larger than `MAX_PAYLOAD` bytes are dropped (the game never sends
  anything that big).
* The game page itself has to be reachable by your friends too: put the ROM-less
  build on a static host (Cloudflare Pages, Netlify Drop, GitHub Pages...) and point
  it at the Render relay with `?relay=` (see above). Every player still places their
  own ROM in their own browser, and nothing gets uploaded anywhere.
