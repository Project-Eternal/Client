var Acore = { started: false, ready: false, module: null, log: [] };
Acore.filesReady = new Promise(function (resolve) { Acore.settleFiles = resolve; });

function Acore_Log(text)
{
  var bound = /^>> Realm (\d+) \(.*\) bound in /.exec(text);
  if (bound) { try { localStorage.setItem('wow-acore-realm', bound[1]); } catch (e) { } }

  if (text === Acore.lastText) { Acore.repeats = (Acore.repeats || 0) + 1; return; }
  if (Acore.repeats) { var count = Acore.repeats; Acore.repeats = 0; Acore.lastText = null; Acore_Log('The line above was repeated ' + count + ' times.'); }
  Acore.lastText = text;
  Acore.log.push(text);
  if (Acore.log.length > 400) Acore.log.shift();
  if (typeof Module !== 'undefined' && Module.print) Module.print(Shell_LogLine('Server', text));
  else console.log(Shell_LogLine('Server', text));
}

function Acore_LoadScript(src)
{
  return new Promise(function (resolve, reject) {
    var script = document.createElement('script');

    script.src = src;
    script.onload = resolve;
    script.onerror = function () { reject(new Error('cannot load ' + src)); };
    document.head.appendChild(script);
  });
}

function Acore_UnpackDbc(FS, pack, dir)
{
  var view = new DataView(pack.buffer, pack.byteOffset, pack.byteLength), at = 0, count = view.getUint32(at, true);

  at += 4;
  FS.mkdirTree(dir);
  for (var i = 0; i < count; i++)
  {
    var nameLength = view.getUint16(at, true), name = new TextDecoder().decode(pack.subarray(at + 2, at + 2 + nameLength)), size;

    at += 2 + nameLength;
    size = view.getUint32(at, true);
    at += 4;
    FS.writeFile(dir + '/' + name, pack.slice(at, at + size));
    at += size;
  }
  return count;
}

function Acore_Sync(FS, populate)
{
  return new Promise(function (resolve) {
    if (Acore.syncing)
    {
      (Acore.syncWaiters = Acore.syncWaiters || []).push(resolve);
      return;
    }
    Acore.syncing = true;
    FS.syncfs(populate, function (err) {
      if (err) Acore_Log('The kept files could not be ' + (populate ? 'loaded' : 'saved') + ': ' + err + '.');
      Acore.syncing = false;
      var waiters = Acore.syncWaiters;
      Acore.syncWaiters = null;
      resolve();
      if (waiters) Acore_Sync(FS, false).then(function () { waiters.forEach(function (w) { w(); }); });
    });
  });
}

function Acore_SaveNow(server)
{
  return new Promise(function (resolve) {
    if (!server || !server._mysql_sqlite_save_now) { resolve(); return; }
    var before = server._mysql_sqlite_saves(), waited = 0;
    server._mysql_sqlite_save_now();
    (function poll() {
      if (server._mysql_sqlite_saves() !== before || waited >= 1000) { resolve(); return; }
      waited += 20;
      setTimeout(poll, 20);
    })();
  });
}

function Acore_ControllerRequest(request, heap)
{
  var words = new Int32Array(heap()), w = request >> 2, bytes = new Uint8Array(heap());
  var kind = words[w + 1], start = words[w + 2] >>> 0, end = start;

  var content = kind === 3 ? bytes.slice(words[w + 3] >>> 0, (words[w + 3] >>> 0) + (words[w + 4] >>> 0)) : null;
  while (!content && bytes[end]) end++;
  var text = content ? 'world queries' : new TextDecoder().decode(bytes.slice(start, end));
  var finish = function (answer) {
    var server = Acore.module, words = new Int32Array(heap()), ok = answer && server, pointer = 0;
    if (ok)
    {
      pointer = server._malloc(answer.length || 1);
      new Uint8Array(heap()).set(answer, pointer);
      words[w + 3] = pointer;
      words[w + 4] = answer.length;
    }
    if (Atomics.compareExchange(words, w, 0, ok ? 1 : -1) !== 0)
    {
      if (pointer) server._free(pointer);
      if (server) server._free(request);
    }
    else Atomics.notify(words, w);
  };
  var game = Module.wowGame;

  if (kind === 5) { if (Acore.saveAndSync) Acore.saveAndSync(); finish(new Uint8Array([1])); return; }
  if (!game) { finish(null); return; }
  (kind === 1 ? game.readFile(text) : kind === 3 ? game.worldQueries(content) :
   game.command(text).then(function (t) { return t === null ? null : new TextEncoder().encode(t); }))
    .then(finish, function (e) { Acore_Log('The game could not answer the question "' + text + '": ' + e + '.'); finish(null); });
}

function Acore_OnClientExit()
{
  var server = Acore.module;

  if (server && server._acore_stop_server && !Acore.stopRequested)
  {
    var waited = 0;

    Acore.stopRequested = true;
    server._acore_stop_server();
    (function poll() {
      if (server._acore_server_stopped() || waited >= 8000)
      {
        Acore_Log('The server stopped' + (waited >= 8000 ? ' (it did not finish in time).' : ' after ' + waited + ' ms.'));
        if (Acore.saveAndSync) Acore.saveAndSync();
        return;
      }
      waited += 100;
      setTimeout(poll, 100);
    })();
    return;
  }
  if (Acore.saveAndSync) Acore.saveAndSync();
}

function Acore_P2PObject()
{
  var settings = Module['p2pSettings'] || {}, rings = null, host = null;

  var urls = String(settings.p2pNode || '').split(/\r?\n/).map(function (u) { return u.trim(); }).filter(function (u) { return u; });

  if (settings.p2p === '0' || !urls.length || typeof P2PHost === 'undefined' || typeof mqtt === 'undefined') return null;
  return {
    shard: String(settings.p2pShard || '').trim(),
    chatEvents: settings.p2pChat === '0' ? 0 : 1,
    attach: function (toHostPtr, toModulePtr, getBuffer)
    {
      var ice = [];

      if (settings.p2pStun) ice.push({ urls: settings.p2pStun });
      if (settings.p2pTurn) ice.push({ urls: settings.p2pTurn, username: settings.p2pTurnUser || undefined, credential: settings.p2pTurnPassword || undefined });
      host = new P2PHost({ mqtt: mqtt, urls: urls, relayOnly: settings.p2pDirect === '0', iceServers: ice.length ? ice : undefined, log: function (t) { Acore_Log('[P2P] ' + t); },
        onQuality: function (q) { if (typeof Shell_Quality === 'function') Shell_Quality(q); } });
      rings = P2P_AttachRings(host, getBuffer, toHostPtr, toModulePtr);
      Acore_Log('[P2P] The relay list has ' + urls.length + (urls.length === 1 ? ' relay node.' : ' relay nodes.'));
      return 1;
    },
    wake: function () { if (rings) rings.pump(); },
    host: function () { return host; }
  };
}

async function Acore_Start()
{
  if (Acore.started) return;
  Acore.started = true;

  try
  {
    var base = new URL('server/acore/', location.href).href;
    var owner = Kept_Owner('acore'), root = owner.root, realm = Module['acoreRealm'] || Kept.realm, realmDir = root + '/realm/' + realm, t0 = Date.now();
    var streamed = ['data/db/world.sqlite'];

    var p2pSettings = Module['p2pSettings'] || {};

    if (p2pSettings.p2p !== '0' && String(p2pSettings.p2pNode || '').trim())
    {
      try
      {
        await Acore_LoadScript(new URL('mqtt.min.js', location.href).href);
        await Acore_LoadScript(new URL('p2p.js', location.href).href);
      }
      catch (e) { Acore_Log('[P2P] ' + e); }
    }
    Acore_Log('Loading the server.');
    var scriptLoaded = Acore_LoadScript(base + 'worldserver.js');
    var chosenLayer = typeof DataSource !== 'undefined' && DataSource.sources.server ? DataSource.sources.server.layer : null;
    var chosenMod = typeof DataSource !== 'undefined' && DataSource.sources.servermod ? DataSource.sources.servermod.layer : null;
    var remote = Module.srwData.loadRemote({ data: base + 'data/', dataLayer: chosenLayer || undefined, modLayer: chosenMod || undefined, patch: base + 'patch/', init: base + 'init/' });

    await scriptLoaded;
    var server = await createAcoreServer({
      noInitialRun: true,
      locateFile: function (path) { return base + path; },
      mainScriptUrlOrBlob: base + 'worldserver.js',
      print: Acore_Log,
      printErr: Acore_Log,
      stdin: function () { return null; },
      wowData: Module.srwData,
      wowController: { request: Acore_ControllerRequest },
      p2p: Acore_P2PObject(),
      preRun: [function (mod) {
        mod.ENV.AC_REALM_ID = String(realm);
        mod.ENV.AC_WORLD_SERVER_PORT = '8085';
        mod.ENV.AC_WORLD_REALM_SWITCH_ENABLE = '1';
        mod.ENV.AC_WORLD_REALM_SWITCH_CHARACTER_DATABASE_INFO = root + '/realm/{}/db-live;0;u;p;characters';
        mod.ENV.AC_PLAYER_SAVE_INTERVAL = '120000';
        mod.ENV.AC_CHARACTER_DATABASE_INFO = realmDir + '/db-live;0;u;p;characters';
        mod.ENV.MYSQL_SQLITE_CONF = root + '/etc/mysql-sqlite.conf';
      }]
    });
    Acore.module = server;
    Acore_Log('The server is loaded after ' + (Date.now() - t0) + ' ms.');

    var FS = server.FS, kept = owner.dirs;

    ['etc', 'data/db', 'logs'].concat(Kept.realms.map(function (r) { return 'realm/' + r + '/db'; }), Kept.realms.map(function (r) { return 'realm/' + r + '/db-live'; }))
      .forEach(function (d) { FS.mkdirTree(root + '/' + d); });
    Acore_Log('Realm ' + realm + (realm === 2 ? ' (Sandbox).' : ' (Normal).'));
    FS.mkdirTree('/Port');
    if (typeof indexedDB !== 'undefined' && FS.filesystems.IDBFS)
    {
      kept.forEach(function (d) { FS.mount(FS.filesystems.IDBFS, {}, root + '/' + d); });
      await Acore_Sync(FS, true);
    }
    else Acore_Log('IndexedDB is not available, so the characters and the settings of the server are not kept.');

    var files = await remote, list = [];
    streamed.forEach(function (path) {
      var entry = files.files[path.toLowerCase()];
      if (!entry) return;
      list.push(root + '/' + entry.path + '\t' + entry.size + '\t' + entry.url);
      delete files.files[path.toLowerCase()];
    });
    FS.writeFile('/Port/files.txt', list.join('\n') + '\n');
    var written = await Module.srwData.materialize(FS, root, files, Module.srwData.SERVER_CACHE, true, "Preparing the server's files");

    Acore_Log('Files of the server: ' + written + ' written, ' + list.length + ' streamed (data ' + files.counts[0] + ', patch ' + files.counts[1] + ', init ' + files.counts[2] + ').');
    if (FS.analyzePath(root + '/data/dbc.pack').exists)
    {
      Acore_Log('Database files unpacked: ' + Acore_UnpackDbc(FS, FS.readFile(root + '/data/dbc.pack'), root + '/data/dbc') + '.');
      FS.unlink(root + '/data/dbc.pack');
    }

    if (FS.filesystems.IDBFS && typeof indexedDB !== 'undefined')
    {
      await Acore_Sync(FS, false);
      Acore.settleFiles(true);
      setInterval(function () { Acore_Sync(FS, false); }, 5000);
      var saveAndSync = function () {
        Acore_Sync(FS, false);
        Acore_SaveNow(server).then(function () { Acore_Sync(FS, false); });
      };
      document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'hidden') saveAndSync(); });
      window.addEventListener('pagehide', function () {
        var p2p = server.p2p && server.p2p.host ? server.p2p.host() : null;
        if (p2p && p2p.publishWill) p2p.publishWill();
        Acore_Sync(FS, false);
      });
      Acore.saveAndSync = saveAndSync;
    }

    Acore.settleFiles(false);
    Acore.ready = true;
    Acore_Log('Starting the server after ' + (Date.now() - t0) + ' ms.');
    server.callMain(['-c', root + '/etc/worldserver.conf']);
  }
  catch (e)
  {
    Acore.settleFiles(false);
    Acore_Log('The embedded server could not be started: ' + e + '. Reload the page; if it happens again, send the log.' + (e && e.stack ? '\n' + e.stack : ''));
  }
}
