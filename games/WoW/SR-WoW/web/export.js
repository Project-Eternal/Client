var UserExport = { busy: false, formatVersion: 1, timeLimit: 20000, startLimit: 180000 };

var UserExport_CrcTable = (function () {
  var table = new Uint32Array(256);

  for (var n = 0; n < 256; n++)
  {
    var c = n;
    for (var k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    table[n] = c >>> 0;
  }
  return table;
})();

function UserExport_Crc32(bytes)
{
  var crc = 0xffffffff;

  for (var i = 0; i < bytes.length; i++) crc = UserExport_CrcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function UserExport_U16(v) { return new Uint8Array([v & 0xff, (v >> 8) & 0xff]); }
function UserExport_U32(v) { return new Uint8Array([v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]); }

function UserExport_DosDate(d)
{
  return {
    time: ((d.getHours() & 0x1f) << 11) | ((d.getMinutes() & 0x3f) << 5) | ((d.getSeconds() >> 1) & 0x1f),
    date: (((Math.max(d.getFullYear(), 1980) - 1980) & 0x7f) << 9) | (((d.getMonth() + 1) & 0xf) << 5) | (d.getDate() & 0x1f)
  };
}

function UserExport_DateFromDos(date, time)
{
  return new Date(((date >> 9) & 0x7f) + 1980, ((date >> 5) & 0xf) - 1, date & 0x1f, (time >> 11) & 0x1f, (time >> 5) & 0x3f, (time & 0x1f) * 2);
}

async function UserExport_Deflate(bytes)
{
  var stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));

  return new Uint8Array(await new Response(stream).arrayBuffer());
}

async function UserExport_Inflate(bytes)
{
  var stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));

  return new Uint8Array(await new Response(stream).arrayBuffer());
}

async function UserExport_BuildZip(entries)
{
  var local = [], central = [], offset = 0, canDeflate = typeof CompressionStream !== 'undefined';

  for (var i = 0; i < entries.length; i++)
  {
    var entry = entries[i], dos = UserExport_DosDate(entry.mtime || new Date()), name = new TextEncoder().encode(entry.name);
    var data = entry.data || new Uint8Array(0), crc = UserExport_Crc32(data), stored = data, method = 0;

    if (canDeflate && data.length > 0)
    {
      try
      {
        stored = await UserExport_Deflate(data);
        method = 8;
      }
      catch (e)
      {
        stored = data;
        Shell_Print(entry.name + ' could not be compressed and is stored as it is (' + e + ').');
      }
    }
    var at = offset;
    [UserExport_U32(0x04034b50), UserExport_U16(20), UserExport_U16(0x0800), UserExport_U16(method), UserExport_U16(dos.time), UserExport_U16(dos.date),
     UserExport_U32(crc), UserExport_U32(stored.length), UserExport_U32(data.length), UserExport_U16(name.length), UserExport_U16(0), name, stored
    ].forEach(function (part) { local.push(part); offset += part.length; });
    central.push([UserExport_U32(0x02014b50), UserExport_U16(20), UserExport_U16(20), UserExport_U16(0x0800), UserExport_U16(method), UserExport_U16(dos.time),
                  UserExport_U16(dos.date), UserExport_U32(crc), UserExport_U32(stored.length), UserExport_U32(data.length), UserExport_U16(name.length),
                  UserExport_U16(0), UserExport_U16(0), UserExport_U16(0), UserExport_U16(0), UserExport_U32(entry.data ? 0 : 0x10), UserExport_U32(at), name]);
  }
  var centralAt = offset, parts = local;

  central.forEach(function (entry) { entry.forEach(function (part) { parts.push(part); offset += part.length; }); });
  parts.push(UserExport_U32(0x06054b50), UserExport_U16(0), UserExport_U16(0), UserExport_U16(entries.length), UserExport_U16(entries.length),
             UserExport_U32(offset - centralAt), UserExport_U32(centralAt), UserExport_U16(0));
  return new Blob(parts, { type: 'application/zip' });
}

async function UserExport_ParseZip(buffer)
{
  var bytes = new Uint8Array(buffer), view = new DataView(buffer);
  var u16 = function (o) { return view.getUint16(o, true); }, u32 = function (o) { return view.getUint32(o, true); };
  var eocd = -1, found = {}, result = [];

  for (var i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 65535); i--)
  {
    if (u32(i) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw UserExport_Problem('it is not a ZIP file');
  var count = u16(eocd + 10), at = u32(eocd + 16);

  if (count === 0xffff || at === 0xffffffff) throw UserExport_Problem('ZIP64 files are not supported');
  for (var e = 0; e < count; e++)
  {
    if (at + 46 > bytes.length || u32(at) !== 0x02014b50) throw UserExport_Problem('the ZIP directory is damaged');
    var flags = u16(at + 8), method = u16(at + 10), crc = u32(at + 16), packed = u32(at + 20), size = u32(at + 24), nameLength = u16(at + 28);
    var extraLength = u16(at + 30), commentLength = u16(at + 32), local = u32(at + 42);
    var name = new TextDecoder().decode(bytes.subarray(at + 46, at + 46 + nameLength)), mtime = UserExport_DateFromDos(u16(at + 14), u16(at + 12));

    at += 46 + nameLength + extraLength + commentLength;
    if (flags & 1) throw UserExport_Problem(name + ' is encrypted');
    if (found[name]) throw UserExport_Problem(name + ' is in the file twice');
    found[name] = true;
    if (local + 30 > bytes.length || u32(local) !== 0x04034b50) throw UserExport_Problem('the header of ' + name + ' is damaged');
    var dataAt = local + 30 + u16(local + 26) + u16(local + 28);

    if (dataAt + packed > bytes.length) throw UserExport_Problem(name + ' is cut short');
    var packedData = bytes.subarray(dataAt, dataAt + packed), data;

    if (method === 0) data = packedData;
    else if (method === 8)
    {
      if (typeof DecompressionStream === 'undefined') throw UserExport_Problem('this browser cannot decompress the file');
      try { data = await UserExport_Inflate(packedData); }
      catch (err) { Shell_Print(name + ' could not be decompressed (' + err + ').'); throw UserExport_Problem(name + ' is damaged'); }
    }
    else throw UserExport_Problem(name + ' uses an unsupported compression method (' + method + ')');
    if (data.length !== size || UserExport_Crc32(data) !== crc) throw UserExport_Problem(name + ' is damaged (its checksum does not match)');
    result.push({ name: name, directory: name.charAt(name.length - 1) === '/', data: data, mtime: mtime });
  }
  return result;
}

function UserExport_Queued(fn)
{
  return new Promise(function (resolve, reject) {
    Module.SyncQueue(function (done) {
      var finish = function (fn2, value) { try { done(); } finally { fn2(value); } };

      Promise.resolve().then(fn).then(function (value) { finish(resolve, value); }, function (error) { finish(reject, error); });
    });
  });
}

function UserExport_Sync(FS, populate)
{
  return new Promise(function (resolve, reject) {
    FS.syncfs(populate, function (err) { if (err) reject(err); else resolve(); });
  });
}

function UserExport_ServerMounts(load)
{
  var paths = Kept_Paths(Kept_Owner('acore'));

  return {
    mount: async function ()
    {
      paths.forEach(function (path) { FS.mkdirTree(path); FS.mount(IDBFS, {}, path); });
      if (load) await UserExport_Sync(FS, true);
    },
    unmount: function ()
    {
      paths.forEach(function (path) { try { FS.unmount(path); } catch (e) { Shell_Print(path + ' could not be unmounted (' + e + ').'); } });
    }
  };
}

function UserExport_Collect(fs, dir, entries)
{
  var stat = fs.stat(dir);

  entries.push({ name: dir.substring(1) + '/', mtime: stat.mtime });
  fs.readdir(dir).forEach(function (name) {
    if (name === '.' || name === '..') return;
    var path = dir + '/' + name, st = fs.stat(path);

    if (fs.isDir(st.mode)) UserExport_Collect(fs, path, entries);
    else if (fs.isFile(st.mode)) entries.push({ name: path.substring(1), data: fs.readFile(path), mtime: st.mtime });
  });
}

function UserExport_Empty(fs, dir)
{
  fs.readdir(dir).forEach(function (name) {
    if (name === '.' || name === '..') return;
    var path = dir + '/' + name, st = fs.stat(path);

    if (fs.isDir(st.mode)) { UserExport_Empty(fs, path); fs.rmdir(path); }
    else fs.unlink(path);
  });
}

function UserExport_Limit(promise, what, milliseconds)
{
  var timer, limit = milliseconds || UserExport.timeLimit;

  return Promise.race([promise, new Promise(function (resolve, reject) {
    timer = setTimeout(function () { reject(UserExport_Problem(what + ' did not finish in ' + limit / 1000 + ' seconds. Try again.')); }, limit);
  })]).finally(function () { clearTimeout(timer); });
}

function UserExport_Update()
{
  var exportButton = document.getElementById('btn-export'), importButton = document.getElementById('btn-import'), startButton = document.getElementById('btn-start');
  var hasStorage = typeof indexedDB !== 'undefined' && typeof IDBFS !== 'undefined';

  if (!exportButton || !importButton) return;
  var checking = typeof DataSource !== 'undefined' && DataSource.busy;

  exportButton.disabled = !Game_RuntimeReady || UserExport.busy || checking || !hasStorage;
  importButton.disabled = !Game_RuntimeReady || UserExport.busy || checking || !hasStorage || Game_StartRequested;
  importButton.title = !hasStorage ? 'The browser keeps no data (IndexedDB is not available).' : Game_StartRequested ? 'Reload the page to import.' : 'Replace the local data with an exported file.';
  exportButton.title = hasStorage ? 'Save the settings, characters and screenshots in a ZIP file.' : 'The browser keeps no data (IndexedDB is not available).';
  if (startButton) startButton.disabled = UserExport.busy || checking;
}

function UserExport_Problem(text)
{
  var problem = new Error(text);

  problem.ours = true;
  return problem;
}

function UserExport_Fail(what, error)
{
  Shell_Print('The ' + what + ' failed: ' + (error && error.message || error));
  window.alert(error && error.ours ? error.message : 'The ' + what + ' did not work. Try again; if it happens again, send the log.');
}

async function UserExport_Run(fn)
{
  UserExport.busy = true;
  UserExport_Update();
  try { return await fn(); }
  finally { UserExport.busy = false; UserExport_Update(); }
}

async function UserExport_ReadParts()
{
  var parts = [], concerned = Kept_Owner('acore').enabled(), game = Kept_Owner('game');

  if (!Game_StartRequested)
  {
    parts = await UserExport_Queued(async function () {
      var mounts = UserExport_ServerMounts(true), result = [];

      if (concerned) await mounts.mount();
      try
      {
        result.push({ owner: game, entries: UserExport_ReadOwner(FS, game) });
        if (concerned) result.push({ owner: Kept_Owner('acore'), entries: UserExport_ReadOwner(FS, Kept_Owner('acore')) });
      }
      finally { if (concerned) mounts.unmount(); }
      return result;
    });
    return parts;
  }
  var work = [UserExport_Queued(async function () {
    await UserExport_Sync(FS, false);
    return { owner: game, entries: UserExport_ReadOwner(FS, game) };
  })];

  if (concerned)
  {
    if (!Acore.module) Shell_Print('Waiting for the embedded server to start before the export.');
    work.push((async function () {
      if (!(await UserExport_Limit(Acore.filesReady, 'The start of the embedded server', UserExport.startLimit)) || !Acore.module) return null;
      var server = Acore.module;

      try
      {
        await Acore_SaveNow(server);
        await Acore_Sync(server.FS, false);
      }
      catch (e) { Shell_Print('The embedded server could not save now (' + e + '). The export holds its last saved data.'); }
      return { owner: Kept_Owner('acore'), entries: UserExport_ReadOwner(server.FS, Kept_Owner('acore')) };
    })());
  }
  return (await UserExport_Limit(Promise.all(work), 'Saving', UserExport.startLimit + UserExport.timeLimit)).filter(function (part) { return part; });
}

function UserExport_ReadOwner(fs, owner)
{
  var entries = [];

  Kept_Paths(owner).forEach(function (dir) { UserExport_Collect(fs, dir, entries); });
  return entries;
}

async function UserExport_Export()
{
  var parts = await UserExport_ReadParts(), now = new Date(), entries = [];
  var description = {
    format: UserExport.formatVersion,
    game: { build: Game_Version.build, name: Game_Version.name },
    date: now.toISOString(),
    parts: parts.map(function (part) { return { name: part.owner.name, dirs: part.owner.dirs }; })
  };

  entries.push({ name: 'export.json', data: new TextEncoder().encode(JSON.stringify(description, null, 1) + '\n'), mtime: now });
  parts.forEach(function (part) { part.entries.forEach(function (entry) { entries.push(entry); }); });
  var blob = await UserExport_BuildZip(entries), url = URL.createObjectURL(blob), link = document.createElement('a');
  var files = entries.filter(function (entry) { return entry.data; }).length - 1;

  link.href = url;
  link.download = 'wow-' + Game_Version.name + '-' + now.toISOString().substring(0, 10) + '.zip';
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  setTimeout(function () { URL.revokeObjectURL(url); }, 10000);
  Shell_Print('Exported ' + files + ' files (' + Math.round(blob.size / 1024) + ' KB): ' + parts.map(function (part) { return part.owner.label; }).join(' and ') + '.');
  window.alert('The data was saved in ' + link.download + '.\n\nKeep the file private' +
               (parts.length > 1 ? ': it contains the key of your account in the shared world, and whoever has it can act as your characters.' : '.'));
}

var UserExport_Areas = [['game settings, key bindings, macros and add-on data', 'game', 'WTF'], ['the game\'s caches', 'game', 'Cache'],
                        ['screenshots', 'game', 'Screenshots'], ['the embedded server\'s settings', 'acore', 'etc'],
                        ['the embedded server\'s characters', 'acore', 'realm/' + Kept.realm + '/db']];

function UserExport_Check(entries)
{
  var description = null, owners = {}, roots = [];

  Kept.owners.forEach(function (owner) { Kept_Paths(owner).forEach(function (dir) { roots.push({ owner: owner, dir: dir.substring(1) }); }); });
  entries.forEach(function (entry) {
    var path = entry.directory ? entry.name.substring(0, entry.name.length - 1) : entry.name, segments = path.split('/');

    if (!path || path.charAt(0) === '/' || path.indexOf('\\') >= 0 || segments.some(function (s) { return s === '' || s === '.' || s === '..'; }))
      throw UserExport_Problem('the path "' + entry.name + '" is not valid');
    if (entry.name === 'export.json') { description = entry; return; }
    var root = roots.filter(function (r) { return path === r.dir || path.indexOf(r.dir + '/') === 0; })[0];

    if (!root)
    {
      if (entry.directory && roots.some(function (r) { return r.dir.indexOf(path + '/') === 0; })) return;
      throw UserExport_Problem('"' + entry.name + '" is not in a directory this page keeps');
    }
    (owners[root.owner.name] = owners[root.owner.name] || []).push(entry);
  });
  if (!description) throw UserExport_Problem('it is not an export of this page');
  try { description = JSON.parse(new TextDecoder().decode(description.data)); }
  catch (e) { throw UserExport_Problem('its description cannot be read'); }
  if (!description || description.format !== UserExport.formatVersion) throw UserExport_Problem('it was made by another version of the export (format ' + (description && description.format) + ')');
  if (!description.game || description.game.build !== Game_Version.build)
    throw UserExport_Problem('it is an export of the game ' + (description.game && description.game.name || 'of another version') + ', this page is ' + Game_Version.name);
  var listed = (description.parts || []).map(function (part) { return part.name; });

  Object.keys(owners).forEach(function (name) { if (listed.indexOf(name) < 0) throw UserExport_Problem('it holds files of "' + name + '" that its description does not list'); });
  listed.forEach(function (name) { if (!Kept_Owner(name)) throw UserExport_Problem('it holds a part ("' + name + '") this page does not know'); });
  return { description: description, listed: listed, owners: owners };
}

function UserExport_Describe(checked, enabledServer, fileName)
{
  var lines = ['Import ' + fileName + ' (exported ' + new Date(checked.description.date).toLocaleString() + ')?', '', 'This replaces the local data; nothing is merged:'];
  var hasServer = checked.listed.indexOf('acore') >= 0;

  UserExport_Areas.forEach(function (area) {
    var owner = Kept_Owner(area[1]);

    if (!owner.enabled()) return;
    var prefix = owner.root.substring(1) + '/' + area[2] + '/', files = (checked.owners[owner.name] || []).filter(function (entry) { return !entry.directory && entry.name.indexOf(prefix) === 0; });
    var size = files.reduce(function (sum, entry) { return sum + entry.data.length; }, 0);

    lines.push('  - ' + area[0] + ': ' + (owner.name === 'game' || hasServer ? files.length + ' files, ' + Math.round(size / 1024) + ' KB' : 'nothing (emptied)'));
  });
  if (enabledServer && !hasServer) lines.push('', 'The file has no data of the embedded server (it was exported with the server off): the server\'s characters will be deleted.');
  if (!enabledServer && hasServer) lines.push('', 'The embedded server is off: the server\'s part of the file is ignored and its data stays as it is.');
  lines.push('', 'Everything the browser keeps for the game now is replaced. Continue?');
  return lines.join('\n');
}

async function UserExport_Replace(checked, enabledServer)
{
  await UserExport_Queued(async function () {
    var mounts = UserExport_ServerMounts(false), owners = [Kept_Owner('game')].concat(enabledServer ? [Kept_Owner('acore')] : []), total = 0;

    if (enabledServer) await mounts.mount();
    try
    {
      owners.forEach(function (owner) {
        Kept_Paths(owner).forEach(function (dir) { UserExport_Empty(FS, dir); });
        (checked.owners[owner.name] || []).forEach(function (entry) {
          var path = '/' + (entry.directory ? entry.name.substring(0, entry.name.length - 1) : entry.name);

          if (entry.directory) { FS.mkdirTree(path); return; }
          FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
          FS.writeFile(path, entry.data);
          FS.utime(path, entry.mtime.getTime(), entry.mtime.getTime());
          total++;
        });
      });
      await UserExport_Sync(FS, false);
      Shell_Print('Imported ' + total + ' files.');
    }
    finally { if (enabledServer) mounts.unmount(); }
  });
}

async function UserExport_Import(file)
{
  if (Game_StartRequested) throw UserExport_Problem('The game has started. Reload the page to import.');
  var checked, enabledServer = Kept_Owner('acore').enabled();

  try { checked = UserExport_Check(await UserExport_ParseZip(await file.arrayBuffer())); }
  catch (e) { Shell_Print(file.name + ' cannot be imported: ' + e); throw UserExport_Problem(file.name + ' cannot be imported' + (e.ours ? ': ' + e.message + '.' : '. The file could not be read.')); }
  if (Game_StartRequested) throw UserExport_Problem('The game has started. Reload the page to import.');
  if (!window.confirm(UserExport_Describe(checked, enabledServer, file.name))) return;
  await UserExport_Replace(checked, enabledServer);
  window.alert('The data was imported. Press Start to play with it.');
}

(function () {
  var input = document.getElementById('import-input');

  if (!input) return;
  document.getElementById('btn-export').addEventListener('click', function () {
    UserExport_Run(UserExport_Export).catch(function (e) { UserExport_Fail('export', e); });
  });
  document.getElementById('btn-import').addEventListener('click', function () { input.value = ''; input.click(); });
  input.addEventListener('change', function () {
    if (input.files.length) UserExport_Run(function () { return UserExport_Import(input.files[0]); }).catch(function (e) { UserExport_Fail('import', e); });
  });
  UserExport_Update();
})();
