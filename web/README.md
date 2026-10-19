# sm64coopdx — Web build (Emscripten)

This repository is a fork of [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx)
with a **browser port**: the game is compiled to WebAssembly with Emscripten and is
played from the page itself, with the Minecraft-style menus and the multiplayer
working between browsers.

> **No ROM is in this repository.** No shared build may carry the ROM inside it. The
> page asks the player for their own `Super Mario 64 (USA)`, validates it and stores
> it **in their browser only** (IndexedDB). See below.

See the [main README](../README.md) for the overview. This file goes into the
details of the build.

## What works

* The game is rendered with WebGL (GLSL ES 1.00 shaders) and the full DJUI menus.
* The loop runs at exactly 30 steps/s (the browser does not set the pace, the game
  does).
* **Multiplayer**: virtual sockets on top of WebSocket against a relay
  (`web/relay/relay.js`), because a browser cannot open UDP sockets.
* **Saves persist in the browser**: games, mods and `sm64config.txt` survive a
  page reload.
* **Custom loading screen** with a PLAY button (goes fullscreen and starts the
  audio, which browsers only allow after a click) and a panel to place the ROM
  (drag and drop, MD5 validation and `.n64`/`.v64` conversion).
* **Mods**: the "open folder" buttons open a picker for a mod folder or `.zip`
  files, and the game's own importer takes it from there (see
  [Mods](#mods)).
* **Mod audio**: songs and sound effects added by mods are heard on the web. The
  browser build cannot give miniaudio a device of its own (no threads, no Web
  Audio worklets), so the engine runs **without a device** and its frames are
  pulled from the game's own audio output instead (`smlua_audio_utils.c` mixes
  them into the queue filled by `audio_sdl.c`), at the same rate: 32000 Hz, stereo.
* **Touch controls**: the page can show an on-screen N64 pad (stick, the D-pad
  above it, plus A, B, Z, L, R, START and the four C buttons, and the **X** and
  **Y** buttons that sm64coopdx added) for phones and tablets. The **Touch
  controls** button sits in the top left corner and the choice is remembered.
  The page only writes the pad state into `globalThis.__sm64touch`
  (`{ buttons, stickX, stickY }`, the usual N64 bit mask) and the game reads it
  in `controller_keyboard.c`: no key presses are faked, so rebinding keys does
  not break it, and the stick stays analog. It is merged with the keyboard, so
  both can be used at the same time.
* **Chat and the phone keyboard**: the pad has a **CHAT** button (it opens and
  closes the chat the same way the chat key does), an **ENTER** button (the same
  as pressing Enter: it sends the message, and it also works as the A button in
  the menus) and a **⌨** button that shows the phone's on-screen keyboard, for
  the chat, the player name and any other text box. SDL/Emscripten never creates
  the editable element Android needs for that, so the page keeps a hidden
  `<input>` of its own and focuses it on demand (inside the touch gesture, as
  Android demands). The game reports whether the chat is open
  (`__sm64touch.chatOpen`) so the keyboard appears and disappears with it. What
  is typed goes through `__sm64touch.text` (UTF-8 bytes, read in
  `controller_keyboard.c`) straight into the game's text boxes, instead of
  through SDL: SDL only accepts text while the game has text input started, and
  phone keyboards work by composition (IME/autocorrect), where the characters
  never reach SDL at all. Enter and Backspace are taken from the keyboard for
  the same reason (phone keyboards use Enter to end the word they are composing
  and Backspace to edit it) and sent as the `SEND` and `DELETE` actions.
* Only the loading screen shows at first; the game takes over once you press PLAY.

## Build

Requirements: an active [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
(`emcc` on the `PATH`) and Lua already compiled for wasm (`-m64`) in
`lib/lua/web64/liblua53.a` (it is in the repository).

```bash
# without a ROM: the player places it in the browser (shareable build)
make -j8 CC=emcc CXX=em++ AR=emar TARGET_WEB=1 WINDOWS_BUILD=0 ICON=0 WINDOWS_AUTO_BUILDER=1

# with an embedded ROM: LOCAL TESTING ONLY, never share that build
make -j8 CC=emcc CXX=em++ AR=emar TARGET_WEB=1 WINDOWS_BUILD=0 ICON=0 WINDOWS_AUTO_BUILDER=1 \
     WEB_ROM="/path/to/baserom.us.z64"
```

The output lands in `build/us_pc/`:

| File | What it is |
|---|---|
| `sm64coopdx.html` | the page (it uses `web/shell.html`, no longer Emscripten's default shell) |
| `sm64coopdx.js` | the JS glue (it includes `web/pre.js`: storage + ROM) |
| `sm64coopdx.wasm` | the game |
| `sm64coopdx.data` | assets (lang, mods, palettes, dynos, and the ROM if one was embedded) |

The whole build is **wasm64** (pointers are 64-bit): the pre-compressed sound data
that ships in the repository was generated with the memory layout of a 64-bit host,
so with 32-bit pointers the game would read garbage out of it.

## Playing locally

```bash
node web/relay/relay.js build/us_pc
```

Then open `http://127.0.0.1:4321/sm64coopdx.html` (the relay maps `/` to
`/index.html`, so you can also copy `sm64coopdx.html` to `index.html`). The relay
serves the page **and** acts as the room server, so a single address gives you both.

* **Create**: it opens the public room `coop01` (pre-filled, and the *Room* field lets you change it).
* **Join**: the field already suggests `coop01`; you can also click any other open room in the list.
* **The list is shared**: the *Join* panel shows **every** open room of the relay
  (name and number of players) and refreshes itself every few seconds, so all the
  players that have the page see each other's rooms. Up to 8 rooms are listed.
* **Room already taken?** If you press *Create* with a name that already has a
  live host, the game warns you and tells you to press *Join* instead of leaving
  you alone in single player.
* If the relay is missing, the game starts in single player without complaining.

Deployment details (LAN, Cloudflare tunnel, the Render blueprint) are in
[`web/relay/README.md`](relay/README.md).

## The ROM and the saved data

* **ROM**: it is requested on the loading screen. The MD5 of the USA version is
  checked (`20b854b239203baf6c961b850a4a51a2`), `.z64` is accepted as well as
  `.n64`/`.v64` (they are converted automatically) and it is stored in the game's
  write directory, which is mounted on IndexedDB: **it is not uploaded to any
  server**.
* **Games and mods**: in that same directory (`saves/`, `mods/`, `sm64config.txt`).
  The game writes with its usual `fopen()` calls and `web/pre.js` flushes the
  changes to IndexedDB (at startup and every 10 s).
* Only one game tab at a time: every tab of the same address shares the same
  browser storage, so the last one to flush wins.
* If you want to start from scratch, clear the site data in your browser.

## Mods

The "open folder" buttons (mods, dynos, save folder) open a file picker: choose a
**mod folder** or **`.zip` file(s)** and the files are copied into the game's
write directory. From there the game's own `mod_import_file()` imports them, the
same code path as dragging and dropping a mod onto the desktop build, so mods,
dynos packs and palettes all work the same way.

That same window lists the mods **added from this browser** and each one has a
**Delete** button (confirm, then reload the page to apply it). The mods that ship
inside the build live in the read-only `/mods` folder and are enabled or disabled
from the game's own mods menu.

## Player name

Pressing **PLAY** asks for the player's name before the game starts (the field is
empty the first time and remembers the last name typed). The loading page leaves
it in `globalThis.__sm64PlayerName` and `web_apply_player_name()` in
`src/pc/utils/misc.c` copies it into `configPlayerName` right after the config
file is read.

## How it is put together (for whoever touches the code)

| File | Role |
|---|---|
| `web/shell.html` | loading screen, ROM panel, name prompt, PLAY button (`--shell-file`) |
| `web/pre.js` | mounts IDBFS, ROM helper and mods list/deletion (`--pre-js`) |
| `web/relay/relay.js` | WebSocket relay + static file server |
| `src/pc/network/socket/socket_web.c` | virtual sockets on top of WebSocket |
| `src/pc/fs/fs.c`, `src/pc/platform.c` | write directory (the one that gets persisted) |
| `src/pc/pc_main.c` | copies an embedded ROM and flushes the saved data every 10 s |
| `src/pc/utils/misc.c` | `open_folder()` on the web asks the page for the files |

The web build uses `-sINVOKE_RUN=0`: `main()` does not start on its own. The PLAY
button calls it once IndexedDB has been read (mounting it is asynchronous).
