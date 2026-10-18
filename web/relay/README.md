# Relay de salas para sm64coopdx-web

Servidor tonto que **reenvía paquetes** entre navegadores (el navegador no puede
usar UDP) y, de paso, **sirve la página del juego**, así que con un solo proceso
y un solo puerto ya se puede jugar.

```
node relay.js [carpeta_del_juego]
```

* Sin carpeta: solo hace de relay (`/rooms` da la lista de salas, `/healthz` dice
  si está vivo).
* Con carpeta (p. ej. `build/us_pc`): además sirve la página en
  `http://localhost:4321/www35/index.html`.

## Protocolo de los frames

Cada mensaje WebSocket binario es:

```
[destino][origen][datos...]     (de un navegador al relay; origen = 0)
[origen][datos...]              (del relay a un navegador)
```

* `destino`: `0` = todos los clientes, `1` = host, `N` = cliente N.
* `origen`: lo pone siempre el relay (`1` = host, `2+` = clientes de la sala).

El juego no sabe nada de esto: cree que habla UDP con direcciones
`::ffff:10.0.0.<id>` (el `id` es justo ese `origen`). El código del transporte
está en `src/pc/network/socket/socket_web.c`.

## Dos formas de usarlo

### 1. En local / LAN (lo normal)

```bash
node relay.js C:/ruta/a/sm64coopdx_web/build/us_pc
```

* Tú: `http://127.0.0.1:4321/www35/index.html` → **Crear** → dices el nombre de
  sala a tus amigos.
* Amigos (misma red): `http://TU_IP_LOCAL:4321/www35/index.html` → **Unirse** →
  aparece tu sala en la lista → clic.
* Si no se ven, hay que permitir el puerto 4321 en el cortafuegos de Windows.

### 2. Por internet (túnel, sin abrir puertos del router)

```bash
cloudflared tunnel --url http://localhost:4321
```

Te da una dirección `https://algo.trycloudflare.com`. Tus amigos abren
`https://algo.trycloudflare.com/www35/index.html` y ya está: la página y el relay
viajan por el mismo túnel (Cloudflare soporta WebSocket). El juego detecta el
relay solo (usa el mismo host que la página).

### 3. En la nube (Render / Railway / Fly) — opcional

Estos servicios pueden hospedar **el relay** (no el juego con ROM). Con la versión
**sin ROM** (cada jugador coloca la suya en el navegador) tiene todo el sentido:

* New → Web Service → este repositorio, con `web/relay/relay.js` + `package.json`
* Build: `npm install` · Start: `npm start` (usa `process.env.PORT`)
* La página apunta al relay con `?relay=tu-relay.onrender.com`

## Notas

* El plan gratuito de Render duerme el servicio tras un rato sin visitas: el
  primer jugador que entre espera ~30-60 s a que despierte (el juego reintenta
  solo la conexión durante ~30 s).
* Una sala = un host. Si dos personas usan el mismo nombre de sala en el mismo
  relay, la segunda sustituye a la primera.
* Los paquetes con más de `MAX_PAYLOAD` bytes se descartan (el juego no manda
  nada tan grande).
