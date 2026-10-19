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

  var WRITE_DIR = '/libsdl/sm64coopdx';
  var NOTE_FILE = WRITE_DIR + '/saved-in-this-browser.txt';
  var NOTE_TEXT =
    'SM64 Coop DX (Web)\n' +
    '==================\n\n' +
    'This is where your saves, mods and settings live.\n' +
    'These files are stored in your browser storage (IndexedDB), that is,\n' +
    'on your own PC: nothing is uploaded to any server and they stay here\n' +
    'even if you close the tab.\n';

  // The bundled ROM (if the build has one) lives in a read-only package.
  // We read it before mounting IndexedDB, because afterwards it is shadowed.
  var ROM_PATHS = ['/rom/baserom.us.z64', WRITE_DIR + '/baserom.us.z64'];
  var ROM_DEST_PATH = WRITE_DIR + '/baserom.us.z64';

  var previousOnRuntimeInitialized = Module['onRuntimeInitialized'];

  // Hooks provided by the loading screen (web/shell.html).
  function setStatus(text, progress)  { if (typeof sm64Status  === 'function') { sm64Status(text, progress); } }
  function setReady()                 { if (typeof sm64Ready   === 'function') { sm64Ready(); } }
  function showWarning(text)          { if (typeof sm64Warning === 'function') { sm64Warning(text); } }
  function setRomInfo(info)           { if (typeof sm64RomInfo === 'function') { sm64RomInfo(info); } }

  function getFS() { return Module['FS'] || (typeof FS !== 'undefined' ? FS : null); }
  function getIdbfs() { return Module['IDBFS'] || (typeof IDBFS !== 'undefined' ? IDBFS : null); }

  function pathExists(FS, path) {
    try { return FS.analyzePath(path).exists; } catch (e) { return false; }
  }

  // Returns the path of the .z64 the game will find in its write folder,
  // or null if there is none yet.
  function romInFolder(FS) {
    try {
      var names = FS.readdir(WRITE_DIR);
      for (var i = 0; i < names.length; i++) {
        var name = names[i];
        if (name === '.' || name === '..') { continue; }
        if (/\.z64$/i.test(name)) {
          var path = WRITE_DIR + '/' + name;
          if (FS.isFile(FS.stat(path).mode)) { return path; }
        }
      }
    } catch (e) { console.log('[web-storage] could not list the folder: ' + e); }
    return null;
  }

  function readBundledRom(FS) {
    for (var i = 0; i < ROM_PATHS.length; i++) {
      if (pathExists(FS, ROM_PATHS[i])) {
        try {
          var data = FS.readFile(ROM_PATHS[i]);
          console.log('[web-storage] bundled rom read from ' + ROM_PATHS[i] + ' (' + data.length + ' bytes)');
          return data;
        } catch (e) {
          console.log('[web-storage] could not read ' + ROM_PATHS[i] + ': ' + e);
        }
      }
    }
    return null;
  }

  function flushFs(cb) {
    var FS = getFS();
    if (!FS) { if (cb) { cb(); } return; }
    try { FS.syncfs(false, function () { if (cb) { cb(); } }); }
    catch (e) { console.log('[web-storage] save warning: ' + e); if (cb) { cb(); } }
  }

  // The shell (shell.html) uses this to store the ROM the player picks.
  globalThis.__sm64Rom = {
    exists: function () { return romInFolder(getFS()) !== null; },
    write: function (bytes, cb) {
      var FS = getFS();
      if (!FS) { if (cb) { cb('no filesystem'); } return; }
      try {
        FS.writeFile(ROM_DEST_PATH, bytes);
        console.log('[web-storage] player rom saved (' + bytes.length + ' bytes)');
      } catch (e) {
        if (cb) { cb(String(e)); }
        return;
      }
      flushFs(function () { if (cb) { cb(null); } });   // make sure it reaches IndexedDB now
    }
  };

  function finish(FS, romBytes) {
    // ROM: if it is not in storage yet, leave it there (the game looks for it in
    // its write folder, so it never has to be copied again).
    try {
      if (romBytes && romInFolder(FS) === null) {
        FS.writeFile(ROM_DEST_PATH, romBytes);
        console.log('[web-storage] bundled rom copied into browser storage');
      }
    } catch (e) {
      console.log('[web-storage] could not leave the rom in storage: ' + e);
    }

    // The write folder must NOT be left empty at startup: the game decides which
    // folder is its save folder based on whether it is empty (sys_user_path()).
    try {
      var hasFiles = FS.readdir(WRITE_DIR).filter(function (n) { return n !== '.' && n !== '..'; }).length > 0;
      if (!hasFiles) { FS.writeFile(NOTE_FILE, NOTE_TEXT); }
    } catch (e) { console.log('[web-storage] warning while checking the folder: ' + e); }

    // Flush to IndexedDB (the first time this creates the whole structure).
    flushFs(function () {
      var path = romInFolder(FS);
      if (path) {
        console.log('[web-storage] rom available at ' + path);
        setRomInfo({ hasRom: true, bundled: !!romBytes, source: path === ROM_DEST_PATH ? 'saved in this browser' : path });
      } else {
        console.log('[web-storage] no rom: the loading screen has to ask for it');
        setRomInfo({ hasRom: false });
      }
      setReady();
    });
  }

  function prepare() {
    var FS = getFS();
    if (!FS) {
      showWarning('Warning: no filesystem; this session cannot be saved.');
      setReady();
      return;
    }

    var IDB = getIdbfs();
    if (!IDB) {
      showWarning('Warning: this browser does not support saved data (IndexedDB).');
      setReady();
      return;
    }

    var romBytes = readBundledRom(FS);

    try {
      if (!pathExists(FS, WRITE_DIR)) { FS.mkdirTree(WRITE_DIR); }
      FS.mount(IDB, {}, WRITE_DIR);
      console.log('[web-storage] IndexedDB mounted at ' + WRITE_DIR);
    } catch (e) {
      console.log('[web-storage] could not mount IndexedDB: ' + e);
      showWarning('Warning: saved data could not be enabled (' + e + ').');
      setReady();
      return;
    }

    setStatus('Reading your saved games, mods and settings…', 0.98);

    // syncfs(true) = bring data from IndexedDB into the game's filesystem.
    FS.syncfs(true, function (err) {
      if (err) {
        console.log('[web-storage] error reading IndexedDB: ' + err);
        showWarning('Warning: could not read your saved data (' + err + ').');
      } else {
        console.log('[web-storage] saved data ready');
      }
      finish(FS, romBytes);
    });
  }

  // -------------------------------------------------------------------------
  // "Open mods folder" in a browser = add mods from the player's PC.
  // The game (src/pc/utils/misc.c) calls in here from the button; we show the
  // picker, copy what was chosen into the filesystem (which lives in
  // IndexedDB) and tell the game through Module._web_add_files_done(), which is
  // the one importing the .zip/.lua files with the game's own importer.
  // -------------------------------------------------------------------------
  var uploadOverlay = null;
  var uploadStatus = null;
  var uploadStaging = null;
  var folderInput = null;
  var filesInput = null;

  function createOverlay() {
    if (uploadOverlay) { return; }

    uploadOverlay = document.createElement('div');
    uploadOverlay.style.cssText = 'position:fixed;inset:0;z-index:60;display:none;align-items:center;' +
      'justify-content:center;background:#000a;font-family:system-ui,"Segoe UI",Roboto,Arial,sans-serif;color:#fff;';

    var box = document.createElement('div');
    box.style.cssText = 'width:min(560px,92vw);background:#141a2e;border:2px solid #ffffff26;border-radius:16px;' +
      'padding:20px 22px;box-shadow:0 24px 70px #000b;';
    box.innerHTML =
      '<div style="font-size:21px;font-weight:800;margin-bottom:8px">Add mods</div>' +
      '<div style="font-size:13.5px;color:#c9d3f2;line-height:1.5">' +
      '  Pick a mod <b>folder</b> (the one with <code>mod.lua</code> inside) or <code>.zip</code> files. ' +
      '  They are stored in <b>this browser</b>, inside the game\'s mods folder, and nothing is uploaded anywhere.' +
      '</div>';

    var buttons = document.createElement('div');
    buttons.style.cssText = 'display:flex;flex-wrap:wrap;gap:10px;margin-top:16px;';

    function makeButton(label) {
      var b = document.createElement('button');
      b.textContent = label;
      b.style.cssText = 'padding:11px 16px;border:0;border-radius:10px;cursor:pointer;font-size:14px;font-weight:800;' +
        'color:#0b1020;background:linear-gradient(180deg,#dbe6ff,#9fb8ee);box-shadow:0 4px 0 #5f74a8;';
      return b;
    }

    var folderButton = makeButton('📁 Choose a mod folder');
    var filesButton = makeButton('🗜️ Choose .zip file(s)');
    var cancelButton = makeButton('Cancel');
    cancelButton.style.background = 'linear-gradient(180deg,#ffd9d9,#ee9f9f)';
    cancelButton.style.boxShadow = '0 4px 0 #a85f5f';

    uploadStatus = document.createElement('div');
    uploadStatus.style.cssText = 'margin-top:12px;font-size:12px;color:#9fb0d8;min-height:16px;white-space:pre-wrap;';

    buttons.appendChild(folderButton);
    buttons.appendChild(filesButton);
    buttons.appendChild(cancelButton);
    box.appendChild(buttons);
    box.appendChild(uploadStatus);
    uploadOverlay.appendChild(box);
    document.body.appendChild(uploadOverlay);

    folderInput = document.createElement('input');
    folderInput.type = 'file';
    folderInput.multiple = true;
    folderInput.webkitdirectory = true;
    folderInput.style.display = 'none';

    filesInput = document.createElement('input');
    filesInput.type = 'file';
    filesInput.multiple = true;
    filesInput.accept = '.zip,.lua,.luac,.ini';
    filesInput.style.display = 'none';

    document.body.appendChild(folderInput);
    document.body.appendChild(filesInput);

    folderButton.addEventListener('click', function () { folderInput.click(); });
    filesButton.addEventListener('click', function () { filesInput.click(); });
    cancelButton.addEventListener('click', function () { showOverlay(false); });

    folderInput.addEventListener('change', function () {
      if (folderInput.files && folderInput.files.length) { copySelected(folderInput.files); }
      folderInput.value = '';
    });
    filesInput.addEventListener('change', function () {
      if (filesInput.files && filesInput.files.length) { copySelected(filesInput.files); }
      filesInput.value = '';
    });
  }

  function showOverlay(visible) {
    if (uploadOverlay) { uploadOverlay.style.display = visible ? 'flex' : 'none'; }
  }

  function setOverlayStatus(text, isError) {
    if (!uploadStatus) { return; }
    uploadStatus.textContent = text || '';
    uploadStatus.style.color = isError ? '#ffb4b4' : '#9fb0d8';
  }

  function copySelected(list) {
    var FS = getFS();
    if (!FS || !uploadStaging) { setOverlayStatus('Browser storage is not ready yet.', true); return; }

    var files = [];
    for (var k = 0; k < list.length; k++) { files.push(list[k]); }
    if (files.length === 0) { return; }

    var i = 0;
    var errors = 0;

    function step() {
      if (i >= files.length) {
        setOverlayStatus('Saving in your browser…', false);
        FS.syncfs(false, function (err) {
          setOverlayStatus(err ? ('Save warning: ' + err) : '', !!err);
          showOverlay(false);
          if (typeof Module['_web_add_files_done'] === 'function') {
            Module._web_add_files_done();
          } else {
            console.log('[web-mods] web_add_files_done is missing');
          }
        });
        return;
      }

      var file = files[i++];
      var rel = file.webkitRelativePath || file.name;
      var path = uploadStaging + '/' + rel;
      var dir = path.substring(0, path.lastIndexOf('/'));
      try { if (!FS.analyzePath(dir).exists) { FS.mkdirTree(dir); } } catch (e) {}

      setOverlayStatus('Copying ' + i + '/' + files.length + '…', false);

      file.arrayBuffer().then(function (buf) {
        try { FS.writeFile(path, new Uint8Array(buf)); }
        catch (e) { errors++; console.log('[web-mods] could not write ' + path + ': ' + e); }
        step();
      }).catch(function (e) {
        errors++;
        console.log('[web-mods] could not read ' + rel + ': ' + e);
        step();
      });
    }

    setOverlayStatus('Copying ' + files.length + ' file(s)…', false);
    step();
  }

  // Called by the game (misc.c -> web_add_files_dialog) from the folder button.
  globalThis.__sm64AddFiles = function (staging) {
    uploadStaging = staging;
    createOverlay();
    setOverlayStatus('', false);
    showOverlay(true);
  };

  // The relay address can be given with ?relay=host:port (or ?relay=wss://host) and
  // we remember it, so a hosted page only has to be opened once with the parameter
  // and later visits point at the same relay on their own.
  try {
    var relayFromUrl = new URLSearchParams(location.search).get('relay');
    if (relayFromUrl) { localStorage.setItem('sm64relay', relayFromUrl); }
    if (!Module['WEB_RELAY']) {
      var savedRelay = localStorage.getItem('sm64relay');
      if (savedRelay) { Module['WEB_RELAY'] = savedRelay; }
    }
  } catch (e) { console.log('[webnet] could not read the saved relay: ' + e); }

  Module['onRuntimeInitialized'] = function () {
    if (previousOnRuntimeInitialized) { previousOnRuntimeInitialized(); }
    prepare();
  };
})();
