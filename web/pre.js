/*
 * web/pre.js
 * ---------------------------------------------------------------------------
 * Se inyecta con `--pre-js`, asi que se ejecuta DENTRO del modulo generado por
 * Emscripten: aqui SI tenemos acceso a FS, IDBFS, Module...
 *
 * Mision: que el juego guarde en el navegador (IndexedDB) sus partidas, mods y
 * ajustes, que sobrevivan a recargar la pagina, y que el ROM del jugador se
 * pueda colocar desde la pantalla de carga (web/shell.html) sin subirlo a
 * ningun servidor.
 *
 * Como: la carpeta de escritura del juego (SDL_GetPrefPath("", "sm64coopdx") ->
 * "/libsdl/sm64coopdx") se monta como IDBFS. El juego sigue escribiendo ahi con
 * sus fopen() de siempre y nosotros volcamos esos cambios a IndexedDB.
 *
 * IMPORTANTE: el juego hace `fs_init()` al arrancar y comprueba que la carpeta
 * de escritura exista, ademas de buscar el ROM dentro de ella. Por eso montamos
 * IndexedDB y esperamos a que sus datos esten LEIDOS (syncfs es asincrono) antes
 * de llamar a main(); de ahi el -sINVOKE_RUN=0 del Makefile.
 * ---------------------------------------------------------------------------
 */
(function () {
  if (typeof Module === 'undefined') { return; }

  var CARPETA_DATOS = '/libsdl/sm64coopdx';
  var NOTA = CARPETA_DATOS + '/guardado-en-este-navegador.txt';
  var TEXTO_NOTA =
    'SM64 Coop DX (Web)\n' +
    '==================\n\n' +
    'Aqui se guardan tus partidas, tus mods y tus ajustes.\n' +
    'Estos archivos viven en el almacenamiento del navegador (IndexedDB),\n' +
    'o sea, en tu propio PC: no se suben a ningun servidor y siguen ahi\n' +
    'aunque cierres la pestana.\n';

  // El ROM incrustado (si el build lo lleva) vive en un paquete de solo lectura.
  // Antes de montar IndexedDB lo leemos, porque despues queda tapado.
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

  // Devuelve la ruta del .z64 que el juego encontrara en su carpeta de
  // escritura, o NULL si aun no hay ninguno.
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
    } catch (e) { console.log('[web-guardado] no pude mirar la carpeta: ' + e); }
    return null;
  }

  function leerRomIncrustado(FS) {
    for (var i = 0; i < RUTAS_ROM.length; i++) {
      if (existe(FS, RUTAS_ROM[i])) {
        try {
          var datos = FS.readFile(RUTAS_ROM[i]);
          console.log('[web-guardado] rom incrustado leido de ' + RUTAS_ROM[i] + ' (' + datos.length + ' bytes)');
          return datos;
        } catch (e) {
          console.log('[web-guardado] no pude leer ' + RUTAS_ROM[i] + ': ' + e);
        }
      }
    }
    return null;
  }

  function volcar(cb) {
    var FS = getFS();
    if (!FS) { if (cb) { cb(); } return; }
    try { FS.syncfs(false, function () { if (cb) { cb(); } }); }
    catch (e) { console.log('[web-guardado] aviso al guardar: ' + e); if (cb) { cb(); } }
  }

  // El shell (shell.html) usa esto para dejar el ROM que elige el jugador.
  globalThis.__sm64Rom = {
    existe: function () { return romEnCarpeta(getFS()) !== null; },
    escribir: function (bytes, cb) {
      var FS = getFS();
      if (!FS) { if (cb) { cb('sin sistema de ficheros'); } return; }
      try {
        FS.writeFile(RUTA_ROM_DESTINO, bytes);
        console.log('[web-guardado] rom del jugador guardado (' + bytes.length + ' bytes)');
      } catch (e) {
        if (cb) { cb(String(e)); }
        return;
      }
      volcar(function () { if (cb) { cb(null); } });   // que quede ya en IndexedDB
    }
  };

  function terminar(FS, romBytes) {
    // ROM: si no esta en lo guardado, lo dejamos ahi (el juego lo busca en su
    // carpeta de escritura y asi no hay que volver a copiarlo nunca).
    try {
      if (romBytes && romEnCarpeta(FS) === null) {
        FS.writeFile(RUTA_ROM_DESTINO, romBytes);
        console.log('[web-guardado] rom incrustado copiado al guardado del navegador');
      }
    } catch (e) {
      console.log('[web-guardado] no pude dejar el rom en el guardado: ' + e);
    }

    // La carpeta de escritura NO puede quedar vacia al arrancar: el juego decide
    // cual es su carpeta de guardado segun si esta vacia (sys_user_path()).
    try {
      var hay = FS.readdir(CARPETA_DATOS).filter(function (n) { return n !== '.' && n !== '..'; }).length > 0;
      if (!hay) { FS.writeFile(NOTA, TEXTO_NOTA); }
    } catch (e) { console.log('[web-guardado] aviso al comprobar la carpeta: ' + e); }

    // Volcamos a IndexedDB (la primera vez crea toda la estructura).
    volcar(function () {
      var ruta = romEnCarpeta(FS);
      if (ruta) {
        console.log('[web-guardado] rom disponible en ' + ruta);
        romInfo({ hay: true, fuente: ruta === RUTA_ROM_DESTINO ? 'guardado en este navegador' : ruta });
      } else {
        console.log('[web-guardado] no hay rom: hay que pedirlo en la pantalla de carga');
        romInfo({ hay: false });
      }
      listo();
    });
  }

  function preparar() {
    var FS = getFS();
    if (!FS) {
      aviso('Aviso: sin sistema de ficheros; esta partida no se podra guardar.');
      listo();
      return;
    }

    var IDB = getIdbfs();
    if (!IDB) {
      aviso('Aviso: este navegador no soporta el guardado (IndexedDB).');
      listo();
      return;
    }

    var romBytes = leerRomIncrustado(FS);

    try {
      if (!existe(FS, CARPETA_DATOS)) { FS.mkdirTree(CARPETA_DATOS); }
      FS.mount(IDB, {}, CARPETA_DATOS);
      console.log('[web-guardado] IndexedDB montado en ' + CARPETA_DATOS);
    } catch (e) {
      console.log('[web-guardado] no pude montar IndexedDB: ' + e);
      aviso('Aviso: no se pudo activar el guardado (' + e + ').');
      listo();
      return;
    }

    estado('Leyendo tus partidas, mods y ajustes guardados…', 0.98);

    // syncfs(true) = traer de IndexedDB al sistema de ficheros del juego.
    FS.syncfs(true, function (err) {
      if (err) {
        console.log('[web-guardado] error leyendo IndexedDB: ' + err);
        aviso('Aviso: no pude leer tus datos guardados (' + err + ').');
      } else {
        console.log('[web-guardado] datos guardados listos');
      }
      terminar(FS, romBytes);
    });
  }

  Module['onRuntimeInitialized'] = function () {
    if (anteriorOnRuntimeInitialized) { anteriorOnRuntimeInitialized(); }
    preparar();
  };
})();
