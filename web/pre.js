/*
 * web/pre.js
 * ---------------------------------------------------------------------------
 * Injected with `--pre-js`, so it runs INSIDE the generated Emscripten module:
 * here we do have access to FS, IDBFS, Module...
 *
 * Mission: make the game store its save files, mods and settings in the browser
 * (IndexedDB), survive a page reload, and let the player drop in their own ROM
 * from the loading screen (web/shell.html) without uploading it anywhere.
 *
 * How: the game's write folder (SDL_GetPrefPath("", "sm64coopdx") ->
 * "/libsdl/sm64coopdx") is mounted as IDBFS. The game keeps writing there with
 * its usual fopen() calls and we flush those changes into IndexedDB.
 *
 * IMPORTANT: the game runs `fs_init()` at startup and checks that the write
 * folder exists, plus it looks for the ROM inside it. That is why we mount
 * IndexedDB and wait for its data to be READ (syncfs is asynchronous) before
 * calling main(); hence the -sINVOKE_RUN=0 in the Makefile.
 * ---------------------------------------------------------------------------
 */
(function () {
  if (typeof Module === 'undefined') { return; }

  var CARPETA_DATOS = '/libsdl/sm64coopdx';
  var NOTA = CARPETA_DATOS + '/saved-in-this-browser.txt';
  var TEXTO_NOTA =
    'SM64 Coop DX (Web)\n' +
    '==================\n\n' +
    'This is where your saves, mods and settings live.\n' +
    'These files are stored in your browser storage (IndexedDB), that is,\n' +
    'on your own PC: nothing is uploaded to any server and they stay here\n' +
    'even if you close the tab.\n';

  // The bundled ROM (if the build has one) lives in a read-only package.
  // We read it before mounting IndexedDB, because afterwards it is shadowed.
  var RUTAS_ROM = ['/rom/baserom.us.z64', CARPETA_DATOS + '/baserom.us.z64'];
  var RUTA_ROM_DESTINO = CARPETA_DATOS + '/baserom.us.z64';

  var anteriorOnRuntimeInitialized = Module['onRuntimeInitialized'];

  function estado(t, f) { if (typeof sm64Estado === 'function') { sm64Estado(t, f); } }
  function listo()        { if (typeof sm64Listo  === 'function') { sm64Listo(); } }
  function aviso(t)       { if (typeof sm64Aviso  === 'function') { sm64Aviso(t); } }
  function romInfo(info)  { if (typeof sm64Rom    === 'function') { sm64Rom(info); } }

  function getFS() { return Module['FS'] || (typeof FS !== 'undefined' ? FS : null); }
  function getIdbfs() { return Module['IDBFS'] || (typeof IDBFS !== 'undefined' ? IDBFS : null); }

  function existe(FS, ruta) {
    try { return FS.analyzePath(ruta).exists; } catch (e) { return false; }
  }

  // Returns the path of the .z64 the game will find in its write folder,
  // or null if there is none yet.
  function romEnCarpeta(FS) {
    try {
      var nombres = FS.readdir(CARPETA_DATOS);
      for (var i = 0; i < nombres.length; i++) {
        var n = nombres[i];
        if (n === '.' || n === '..') { continue; }
        if (/\.z64$/i.test(n)) {
          var ruta = CARPETA_DATOS + '/' + n;
          if (FS.isFile(FS.stat(ruta).mode)) { return ruta; }
        }
      }
    } catch (e) { console.log('[web-storage] could not list the folder: ' + e); }
    return null;
  }

  function leerRomIncrustado(FS) {
    for (var i = 0; i < RUTAS_ROM.length; i++) {
      if (existe(FS, RUTAS_ROM[i])) {
        try {
          var datos = FS.readFile(RUTAS_ROM[i]);
          console.log('[web-storage] bundled rom read from ' + RUTAS_ROM[i] + ' (' + datos.length + ' bytes)');
          return datos;
        } catch (e) {
          console.log('[web-storage] could not read ' + RUTAS_ROM[i] + ': ' + e);
        }
      }
    }
    return null;
  }

  function volcar(cb) {
    var FS = getFS();
    if (!FS) { if (cb) { cb(); } return; }
    try { FS.syncfs(false, function () { if (cb) { cb(); } }); }
    catch (e) { console.log('[web-storage] save warning: ' + e); if (cb) { cb(); } }
  }

  // The shell (shell.html) uses this to store the ROM the player picks.
  globalThis.__sm64Rom = {
    existe: function () { return romEnCarpeta(getFS()) !== null; },
    escribir: function (bytes, cb) {
      var FS = getFS();
      if (!FS) { if (cb) { cb('no filesystem'); } return; }
      try {
        FS.writeFile(RUTA_ROM_DESTINO, bytes);
        console.log('[web-storage] player rom saved (' + bytes.length + ' bytes)');
      } catch (e) {
        if (cb) { cb(String(e)); }
        return;
      }
      volcar(function () { if (cb) { cb(null); } });   // make sure it reaches IndexedDB now
    }
  };

  function terminar(FS, romBytes) {
    // ROM: if it is not in storage yet, leave it there (the game looks for it in
    // its write folder, so it never has to be copied again).
    try {
      if (romBytes && romEnCarpeta(FS) === null) {
        FS.writeFile(RUTA_ROM_DESTINO, romBytes);
        console.log('[web-storage] bundled rom copied into browser storage');
      }
    } catch (e) {
      console.log('[web-storage] could not leave the rom in storage: ' + e);
    }

    // The write folder must NOT be left empty at startup: the game decides which
    // folder is its save folder based on whether it is empty (sys_user_path()).
    try {
      var hay = FS.readdir(CARPETA_DATOS).filter(function (n) { return n !== '.' && n !== '..'; }).length > 0;
      if (!hay) { FS.writeFile(NOTA, TEXTO_NOTA); }
    } catch (e) { console.log('[web-storage] warning while checking the folder: ' + e); }

    // Flush to IndexedDB (the first time this creates the whole structure).
    volcar(function () {
      var ruta = romEnCarpeta(FS);
      if (ruta) {
        console.log('[web-storage] rom available at ' + ruta);
        romInfo({ hay: true, fuente: ruta === RUTA_ROM_DESTINO ? 'saved in this browser' : ruta });
      } else {
        console.log('[web-storage] no rom: the loading screen has to ask for it');
        romInfo({ hay: false });
      }
      listo();
    });
  }

  function preparar() {
    var FS = getFS();
    if (!FS) {
      aviso('Warning: no filesystem; this session cannot be saved.');
      listo();
      return;
    }

    var IDB = getIdbfs();
    if (!IDB) {
      aviso('Warning: this browser does not support saved data (IndexedDB).');
      listo();
      return;
    }

    var romBytes = leerRomIncrustado(FS);

    try {
      if (!existe(FS, CARPETA_DATOS)) { FS.mkdirTree(CARPETA_DATOS); }
      FS.mount(IDB, {}, CARPETA_DATOS);
      console.log('[web-storage] IndexedDB mounted at ' + CARPETA_DATOS);
    } catch (e) {
      console.log('[web-storage] could not mount IndexedDB: ' + e);
      aviso('Warning: saved data could not be enabled (' + e + ').');
      listo();
      return;
    }

    estado('Reading your saved games, mods and settings…', 0.98);

    // syncfs(true) = bring data from IndexedDB into the game's filesystem.
    FS.syncfs(true, function (err) {
      if (err) {
        console.log('[web-storage] error reading IndexedDB: ' + err);
        aviso('Warning: could not read your saved data (' + err + ').');
      } else {
        console.log('[web-storage] saved data ready');
      }
      terminar(FS, romBytes);
    });
  }

  // -------------------------------------------------------------------------
  // "Open mods folder" in a browser = add mods from the player's PC.
  // The game (src/pc/utils/misc.c) calls in here from the button; we show the
  // picker, copy what was chosen into the filesystem (which lives in
  // IndexedDB) and tell the game through Module._web_add_files_done(), which is
  // the one importing the .zip/.lua files with the game's own importer.
  // -------------------------------------------------------------------------
  var elSubida = null;
  var elSubidaEstado = null;
  var stagingSubida = null;
  var inputCarpeta = null;
  var inputArchivos = null;

  function uiCrear() {
    if (elSubida) { return; }

    elSubida = document.createElement('div');
    elSubida.style.cssText = 'position:fixed;inset:0;z-index:60;display:none;align-items:center;' +
      'justify-content:center;background:#000a;font-family:system-ui,"Segoe UI",Roboto,Arial,sans-serif;color:#fff;';

    var caja = document.createElement('div');
    caja.style.cssText = 'width:min(560px,92vw);background:#141a2e;border:2px solid #ffffff26;border-radius:16px;' +
      'padding:20px 22px;box-shadow:0 24px 70px #000b;';
    caja.innerHTML =
      '<div style="font-size:21px;font-weight:800;margin-bottom:8px">Add mods</div>' +
      '<div style="font-size:13.5px;color:#c9d3f2;line-height:1.5">' +
      '  Pick a mod <b>folder</b> (the one with <code>mod.lua</code> inside) or <code>.zip</code> files. ' +
      '  They are stored in <b>this browser</b>, inside the game\'s mods folder, and nothing is uploaded anywhere.' +
      '</div>';

    var botones = document.createElement('div');
    botones.style.cssText = 'display:flex;flex-wrap:wrap;gap:10px;margin-top:16px;';

    function boton(texto) {
      var b = document.createElement('button');
      b.textContent = texto;
      b.style.cssText = 'padding:11px 16px;border:0;border-radius:10px;cursor:pointer;font-size:14px;font-weight:800;' +
        'color:#0b1020;background:linear-gradient(180deg,#dbe6ff,#9fb8ee);box-shadow:0 4px 0 #5f74a8;';
      return b;
    }

    var bCarpeta = boton('📁 Choose a mod folder');
    var bArchivos = boton('🗜️ Choose .zip file(s)');
    var bCerrar = boton('Cancel');
    bCerrar.style.background = 'linear-gradient(180deg,#ffd9d9,#ee9f9f)';
    bCerrar.style.boxShadow = '0 4px 0 #a85f5f';

    elSubidaEstado = document.createElement('div');
    elSubidaEstado.style.cssText = 'margin-top:12px;font-size:12px;color:#9fb0d8;min-height:16px;white-space:pre-wrap;';

    botones.appendChild(bCarpeta);
    botones.appendChild(bArchivos);
    botones.appendChild(bCerrar);
    caja.appendChild(botones);
    caja.appendChild(elSubidaEstado);
    elSubida.appendChild(caja);
    document.body.appendChild(elSubida);

    inputCarpeta = document.createElement('input');
    inputCarpeta.type = 'file';
    inputCarpeta.multiple = true;
    inputCarpeta.webkitdirectory = true;
    inputCarpeta.style.display = 'none';

    inputArchivos = document.createElement('input');
    inputArchivos.type = 'file';
    inputArchivos.multiple = true;
    inputArchivos.accept = '.zip,.lua,.luac,.ini';
    inputArchivos.style.display = 'none';

    document.body.appendChild(inputCarpeta);
    document.body.appendChild(inputArchivos);

    bCarpeta.addEventListener('click', function () { inputCarpeta.click(); });
    bArchivos.addEventListener('click', function () { inputArchivos.click(); });
    bCerrar.addEventListener('click', function () { uiMostrar(false); });

    inputCarpeta.addEventListener('change', function () {
      if (inputCarpeta.files && inputCarpeta.files.length) { copiarElegidos(inputCarpeta.files); }
      inputCarpeta.value = '';
    });
    inputArchivos.addEventListener('change', function () {
      if (inputArchivos.files && inputArchivos.files.length) { copiarElegidos(inputArchivos.files); }
      inputArchivos.value = '';
    });
  }

  function uiMostrar(mostrar) {
    if (elSubida) { elSubida.style.display = mostrar ? 'flex' : 'none'; }
  }

  function uiEstado(t, esError) {
    if (!elSubidaEstado) { return; }
    elSubidaEstado.textContent = t || '';
    elSubidaEstado.style.color = esError ? '#ffb4b4' : '#9fb0d8';
  }

  function copiarElegidos(lista) {
    var FS = getFS();
    if (!FS || !stagingSubida) { uiEstado('Browser storage is not ready yet.', true); return; }

    var archivos = [];
    for (var k = 0; k < lista.length; k++) { archivos.push(lista[k]); }
    if (archivos.length === 0) { return; }

    var i = 0;
    var errores = 0;

    function paso() {
      if (i >= archivos.length) {
        uiEstado('Saving in your browser…', false);
        FS.syncfs(false, function (err) {
          uiEstado(err ? ('Save warning: ' + err) : '', !!err);
          uiMostrar(false);
          if (typeof Module['_web_add_files_done'] === 'function') {
            Module._web_add_files_done();
          } else {
            console.log('[web-mods] web_add_files_done is missing');
          }
        });
        return;
      }

      var f = archivos[i++];
      var rel = f.webkitRelativePath || f.name;
      var ruta = stagingSubida + '/' + rel;
      var dir = ruta.substring(0, ruta.lastIndexOf('/'));
      try { if (!FS.analyzePath(dir).exists) { FS.mkdirTree(dir); } } catch (e) {}

      uiEstado('Copying ' + i + '/' + archivos.length + '…', false);

      f.arrayBuffer().then(function (buf) {
        try { FS.writeFile(ruta, new Uint8Array(buf)); }
        catch (e) { errores++; console.log('[web-mods] could not write ' + ruta + ': ' + e); }
        paso();
      }).catch(function (e) {
        errores++;
        console.log('[web-mods] could not read ' + rel + ': ' + e);
        paso();
      });
    }

    uiEstado('Copying ' + archivos.length + ' file(s)…', false);
    paso();
  }

  // Called by the game (misc.c -> web_add_files_dialog) from the folder button.
  globalThis.__sm64SubirMods = function (staging) {
    stagingSubida = staging;
    uiCrear();
    uiEstado('', false);
    uiMostrar(true);
  };

  Module['onRuntimeInitialized'] = function () {
    if (anteriorOnRuntimeInitialized) { anteriorOnRuntimeInitialized(); }
    preparar();
  };
})();
