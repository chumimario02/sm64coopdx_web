# sm64coopdx — versión web (Emscripten)

Este repositorio es un fork de [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx)
con un **port a navegador**: el juego se compila a WebAssembly con Emscripten y se
juega desde la propia página, con los menús estilo Minecraft y el multijugador
funcionando entre navegadores.

> **El ROM no va en el repositorio.** Ningún build que se comparta debe llevar el
> ROM dentro. La página pide al jugador su propio `Super Mario 64 (USA)`, lo valida
> y lo guarda **solo en su navegador** (IndexedDB). Ver más abajo.

## Qué funciona

* Juego renderizado con WebGL (shaders GLSL ES 1.00) y menús DJUI completos.
* Bucle a 30 pasos/s exactos (el navegador no marca el ritmo, lo marca el juego).
* **Multijugador**: sockets virtuales sobre WebSocket contra un relay
  (`web/relay/relay.js`), porque el navegador no puede abrir UDP.
* **Guardado persistente en el navegador**: partidas, mods y `sm64config.txt`
  sobreviven a recargar la página.
* **Pantalla de carga propia** con botón JUGAR (entra en pantalla completa y
  activa el sonido, que los navegadores exigen con un clic) y panel para colocar
  el ROM (con arrastrar y soltar, validación MD5 y conversión `.n64`/`.v64`).

## Compilar

Requisitos: [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
activo (`emcc` en el PATH) y el código Lua ya compilado para wasm (`-m64`) en
`lib/lua/web64/liblua53.a`.

```bash
# sin ROM: el jugador la coloca en el navegador (versión para compartir)
make -j8 CC=emcc CXX=em++ AR=emar TARGET_WEB=1 WINDOWS_BUILD=0 ICON=0 WINDOWS_AUTO_BUILDER=1

# con ROM incrustada: SOLO para pruebas locales, NO compartir el build
make -j8 CC=emcc CXX=em++ AR=emar TARGET_WEB=1 WINDOWS_BUILD=0 ICON=0 WINDOWS_AUTO_BUILDER=1 \
     WEB_ROM="/ruta/a/baserom.us.z64"
```

El resultado queda en `build/us_pc/`:

| fichero | qué es |
|---|---|
| `sm64coopdx.html` | la página (usa `web/shell.html`, ya no la de Emscripten) |
| `sm64coopdx.js` | pegamento JS (lleva `web/pre.js`: guardado + ROM) |
| `sm64coopdx.wasm` | el juego |
| `sm64coopdx.data` | recursos (lang, mods, palettes, dynos, y el ROM si se incrustó) |

## Jugar en local

```bash
node web/relay/relay.js build/us_pc
```

y abre `http://127.0.0.1:4321/www1/index.html` (o el nombre que le hayas dado a la
carpeta). El relay sirve la página **y** hace de servidor de salas, así que en la
misma dirección tienes las dos cosas.

* **Crear**: te pide un nombre de sala (p. ej. `sala123`).
* **Unirse**: muestra la lista de salas abiertas del relay.
* Si falta el relay, el juego arranca en solitario sin quejarse.

Detalles de despliegue (LAN, túnel con Cloudflare, Render) en
[`web/relay/README.md`](relay/README.md).

## El ROM y el guardado

* **ROM**: se pide en la pantalla de carga. Se comprueba el MD5 de la versión USA
  (`20b854b239203baf6c961b850a4a51a2`), se acepta `.z64` y también `.n64`/`.v64`
  (se convierten solos) y se guarda en la carpeta de escritura del juego, que está
  montada sobre IndexedDB: **no se sube a ningún servidor**.
* **Partidas y mods**: en esa misma carpeta (`saves/`, `mods/`, `sm64config.txt`).
  El juego escribe con sus `fopen()` de siempre y `web/pre.js` vuelca los cambios a
  IndexedDB (al arrancar y cada 10 s).
* Si quieres empezar de cero: borra los datos del sitio en el navegador.

## Cómo está montado (para quien toque el código)

| fichero | papel |
|---|---|
| `web/shell.html` | pantalla de carga, panel del ROM, botón JUGAR (`--shell-file`) |
| `web/pre.js` | monta IDBFS y expone `__sm64Rom.escribir()` (`--pre-js`) |
| `web/relay/relay.js` | relay WebSocket + servidor de ficheros |
| `src/pc/network/socket/socket_web.c` | sockets virtuales sobre WebSocket |
| `src/pc/fs/fs.c`, `src/pc/platform.c` | carpeta de escritura (la que se persiste) |
| `src/pc/pc_main.c` | copia el ROM incrustado y vuelca el guardado cada 10 s |

El build web usa `-sINVOKE_RUN=0`: `main()` no arranca solo, lo llama el botón
JUGAR cuando ya está leído IndexedDB (montarlo es asíncrono).
