var DataSource = {
  busy: false,
  sources: {},
  order: [],
  errors: {},
  layer: null,
  modLayer: null,
  locale: 'enGB',
  database: 'wow-page',
  store: 'folders',
  localePattern: /^[a-z]{2}[A-Z]{2}$/,
  directories: /^(data|interface)$/i,
  batch: 64
};

function DataSource_Define(options)
{
  var source = { typed: null, folder: null, layer: null, urls: {}, optional: false, suffix: '', whole: false, when: null };

  Object.keys(options).forEach(function (k) { source[k] = options[k]; });
  DataSource.sources[source.name] = source;
  DataSource.order.push(source.name);
  return source;
}

DataSource_Define({ name: 'data', key: 'dataUrl', what: 'client files', inputId: 'data-source-input', pickerId: 'wow-game' });
DataSource_Define({ name: 'mod', key: 'modUrl', what: 'client mod', optional: true, suffix: '/mod', inputId: 'mod-source-input', pickerId: 'wow-mod' });
DataSource_Define({ name: 'server', key: 'serverUrl', what: 'server files', whole: true, suffix: '/server', inputId: 'server-source-input', pickerId: 'wow-server',
                    when: function () { return typeof Game_EmbeddedServerWanted !== 'function' || Game_EmbeddedServerWanted(); } });
DataSource_Define({ name: 'servermod', key: 'serverModUrl', what: 'server mod', optional: true, whole: true, suffix: '/servermod', inputId: 'servermod-source-input', pickerId: 'wow-servermod',
                    when: function () { return typeof Game_EmbeddedServerWanted !== 'function' || Game_EmbeddedServerWanted(); } });

function DataSource_Of(field) { return DataSource.sources[field.source]; }

function DataSource_Field(source)
{
  return Game_SettingsFields.filter(function (field) { return field.key === source.key; })[0];
}

function DataSource_LocaleField()
{
  return Game_SettingsFields.filter(function (field) { return field.key === 'locale'; })[0];
}

function DataSource_Forced(source)
{
  return Game_FieldUriValue(DataSource_Field(source), Game_ParseFragmentParams()) != null;
}

function DataSource_Value(source)
{
  return (source.typed !== null && !DataSource_Forced(source)) ? source.typed : String(Game_FieldValue(DataSource_Field(source), Game_ParseFragmentParams()));
}

function DataSource_Hint(source)
{
  return source.folder ? 'file:///…/' + (source.folder.name || 'Local drive').replace(/^.*\//, '') : (source.optional ? 'no ' + source.what.replace(/ files$/, '') : '');
}

function DataSource_Input(source) { return document.getElementById(source.inputId); }

function DataSource_ShowField(source)
{
  var input = DataSource_Input(source);

  if (!input) return;
  input.value = DataSource_Value(source);
  if (input.value === '' && DataSource_Hint(source)) input.setAttribute('placeholder', DataSource_Hint(source));
  else input.removeAttribute('placeholder');
}

function DataSource_Key(source) { return (typeof Game_Version === 'object' ? Game_Version.name : 'game') + (source ? source.suffix : ''); }

function DataSource_Open()
{
  return new Promise(function (resolve, reject)
  {
    if (typeof indexedDB === 'undefined') return reject(new Error('no IndexedDB'));
    var request = indexedDB.open(DataSource.database, 1);

    request.onupgradeneeded = function () { request.result.createObjectStore(DataSource.store); };
    request.onsuccess = function () { resolve(request.result); };
    request.onerror = function () { reject(request.error); };
  });
}

function DataSource_Stored(mode, action)
{
  return DataSource_Open().then(function (db)
  {
    return new Promise(function (resolve, reject)
    {
      var transaction = db.transaction(DataSource.store, mode), result = action(transaction.objectStore(DataSource.store));

      transaction.oncomplete = function () { db.close(); resolve(result.result); };
      transaction.onerror = transaction.onabort = function () { db.close(); reject(transaction.error); };
    });
  });
}

function DataSource_Remember(source, handle)
{
  var write = handle ? DataSource_Stored('readwrite', function (store) { return store.put(handle, DataSource_Key(source)); })
                     : DataSource_Stored('readwrite', function (store) { return store.delete(DataSource_Key(source)); });

  return write.catch(function (e) { console.warn('[Shell] The choice of the ' + source.what + ' folder is not kept: ' + e); });
}

function DataSource_Load()
{
  return Promise.all(DataSource.order.map(function (name)
  {
    var source = DataSource.sources[name];

    return DataSource_Stored('readonly', function (store) { return store.get(DataSource_Key(source)); }).then(function (handle)
    {
      if (handle && !source.folder) source.folder = { name: handle.name, handle: handle, files: null };
      DataSource_ShowField(source);
    }).catch(function () {});
  }));
}

function DataSource_FromInput(list, whole)
{
  var files = {}, name = '';

  Array.prototype.forEach.call(list, function (file)
  {
    var parts = (file.webkitRelativePath || file.name).split('/');

    name = parts[0];
    if (parts.length === 2 || (parts.length > 2 && (whole || DataSource.directories.test(parts[1])))) files[parts.slice(1).join('/')] = file;
  });
  return { name: name, handle: null, files: files };
}

function DataSource_PickWithInput(whole)
{
  return new Promise(function (resolve)
  {
    var input = document.createElement('input');

    input.type = 'file';
    input.webkitdirectory = true;
    input.hidden = true;
    input.addEventListener('change', function ()
    {
      var list = input.files;

      input.remove();
      if (!list || !list.length) return resolve(null);
      resolve(DataSource_FromInput(list, whole));
    });
    input.addEventListener('cancel', function () { input.remove(); resolve(null); });
    document.body.appendChild(input);
    input.click();
  });
}

async function DataSource_DataDirectories(chosen)
{
  var names = [];

  if (chosen.files)
  {
    Object.keys(chosen.files).forEach(function (path)
    {
      var parts = path.split('/');

      if (parts.length > 2 && /^data$/i.test(parts[0]) && names.indexOf(parts[1]) < 0) names.push(parts[1]);
    });
  }
  else if (chosen.handle)
  {
    for await (var item of chosen.handle.entries())
    {
      if (item[1].kind !== 'directory' || !/^data$/i.test(item[0])) continue;
      for await (var inner of item[1].entries()) if (inner[1].kind === 'directory') names.push(inner[0]);
    }
  }
  return names;
}

async function DataSource_TakeLocale(chosen)
{
  var field = DataSource_LocaleField();

  if (!field || Game_FieldUriValue(field, Game_ParseFragmentParams()) != null) return;
  var current = String(Game_FieldValue(field, Game_ParseFragmentParams())), names;

  try { names = await DataSource_DataDirectories(chosen); } catch (e) { return; }
  if (names.some(function (name) { return name.toLowerCase() === current.toLowerCase(); })) return;
  var found = names.filter(function (name) { return DataSource.localePattern.test(name); }).sort();

  if (!found.length) return;
  Game_SetCookie(Game_FieldCookieName(field), found[0]);
  var input = document.getElementById(field.inputId);

  if (input) input.value = found[0];
}

async function DataSource_Browse(source)
{
  var chosen = null;

  if (DataSource.busy) return;
  if (typeof window.showDirectoryPicker === 'function')
  {
    try
    {
      var handle = await window.showDirectoryPicker({ id: source.pickerId, mode: 'read' });

      chosen = { name: handle.name, handle: handle, files: null };
    }
    catch (e)
    {
      if (e && e.name === 'AbortError') return;
      chosen = await DataSource_PickWithInput(source.whole);
    }
  }
  else chosen = await DataSource_PickWithInput(source.whole);
  if (!chosen) return;
  source.folder = chosen;
  source.typed = '';
  DataSource.errors[source.key] = '';
  if (source.name === 'data') await DataSource_TakeLocale(chosen);
  DataSource_ShowField(source);
}

function DataSource_Reset(source)
{
  if (DataSource.busy) return;
  var field = DataSource_Field(source);

  source.folder = null;
  source.typed = String(field.default);
  DataSource.errors[source.key] = '';
  Game_SetCookie(Game_FieldCookieName(field), String(field.default));
  DataSource_Remember(source, null);
  DataSource_ShowField(source);
}

async function DataSource_WalkHandle(root, whole)
{
  var files = {}, found = [];

  async function walk(directory, prefix, deep)
  {
    var subdirectories = [];

    for await (var item of directory.entries())
    {
      if (item[1].kind === 'file') found.push([prefix + item[0], item[1]]);
      else if (deep || DataSource.directories.test(item[0])) subdirectories.push([prefix + item[0] + '/', item[1]]);
    }
    for (var i = 0; i < subdirectories.length; i++) await walk(subdirectories[i][1], subdirectories[i][0], true);
  }

  await walk(root, '', !!whole);
  for (var from = 0; from < found.length; from += DataSource.batch)
  {
    var part = found.slice(from, from + DataSource.batch), got = await Promise.all(part.map(function (item) { return item[1].getFile(); }));

    part.forEach(function (item, index) { files[item[0]] = got[index]; });
  }
  return files;
}

function DataSource_Entries(source, files)
{
  var entries = [], urls = {}, dropped = Module.srwData.droppedDirectories();

  Object.keys(files).forEach(function (path)
  {
    if (/[\t\r\n]/.test(path)) return;
    var lower = path.toLowerCase();

    if (source.name === 'mod' && dropped.some(function (dir) { return lower.indexOf(dir) === 0; })) return;
    var url = 'local:' + source.name + '/' + path;

    entries.push({ path: path, size: files[path].size, url: url, layer: source.name, version: null });
    urls[url] = files[path];
  });
  var joined = Module.srwData.joinParts(entries);

  if (joined !== entries)
  {
    joined.forEach(function (entry)
    {
      if (!entry.parts) return;
      urls[entry.url] = new Blob(entry.parts.map(function (part) { return urls[part.url]; }));
      entry.parts.forEach(function (part) { delete urls[part.url]; });
    });
    entries = joined;
  }
  return { entries: entries, urls: urls };
}

function DataSource_RuntimeReady()
{
  return new Promise(function (resolve)
  {
    (function wait() { if (Game_RuntimeReady) resolve(); else setTimeout(wait, 50); })();
  });
}

function DataSource_Update()
{
  if (typeof UserExport_Update === 'function') UserExport_Update();
}

function DataSource_Fail(key, message, resetField)
{
  return { ok: false, key: key, message: message, resetField: !!resetField };
}

async function DataSource_CheckSource(source, permission, locale)
{
  if (source.when && !source.when()) return { ok: true, layer: null, urls: {} };
  var value = DataSource_Value(source), folder = source.folder, layer, urls = {}, field = DataSource_Field(source);
  var title = 'The ' + source.what + ' location';

  if (value !== '')
  {
    try { layer = await Module.srwData.loadDataLayer(new URL(value, location.href).href, source.name, source.whole); }
    catch (e) { return DataSource_Fail(source.key, title + ' "' + value + '" cannot be used: ' + e.message, false); }
  }
  else if (!folder)
  {
    if (source.optional) layer = null;
    else return DataSource_Fail(source.key, 'There is no ' + source.what + ' location: type an address or choose a folder with Browse.', true);
  }
  else
  {
    if (permission && await permission !== 'granted') return DataSource_Fail(source.key, 'The permission to read the ' + source.what + ' folder was not given.', true);
    try
    {
      if (!folder.files) folder.files = await DataSource_WalkHandle(folder.handle, source.whole);
    }
    catch (e)
    {
      return DataSource_Fail(source.key, 'The ' + source.what + ' folder cannot be read (' + (e && e.message || e) + '): choose it again.', true);
    }
    var made = DataSource_Entries(source, folder.files);

    layer = made.entries;
    urls = made.urls;
  }
  if (source.name === 'data')
  {
    var missing = Module.srwData.missingFiles(Module.srwData.linkLocale(layer, locale), locale);

    if (missing.length)
    {
      return DataSource_Fail(source.key, (value !== '' ? title + ' "' + value + '"' : 'The chosen folder') + ' is not an installation of the game version of this page (' +
                             DataSource_Key() + ', locale ' + locale + '): missing ' + missing.slice(0, 4).join(', ') + (missing.length > 4 ? ' and ' + (missing.length - 4) + ' more' : '') + '.', false);
    }
  }
  Game_SetCookie(Game_FieldCookieName(field), value);
  if (value === '' && folder && folder.handle) await DataSource_Remember(source, folder.handle);
  else await DataSource_Remember(source, null);
  return { ok: true, layer: layer, urls: urls };
}

async function DataSource_Check()
{
  var locale = String(Game_FieldValue(DataSource_LocaleField(), Game_ParseFragmentParams())), permissions = {}, checked = {};

  if (!DataSource.localePattern.test(locale))
  {
    return DataSource_Fail('locale', 'The locale "' + locale + '" is not valid: it is four letters, two lower case and two upper case, such as enGB.', false);
  }
  DataSource.order.forEach(function (name)
  {
    var source = DataSource.sources[name], folder = source.folder;

    if (source.when && !source.when()) return;
    if (DataSource_Value(source) === '' && folder && folder.handle && !folder.files) permissions[name] = folder.handle.requestPermission({ mode: 'read' });
  });
  DataSource.busy = true;
  DataSource_Update();
  Module.setStatus('Checking the files…');
  try
  {
    await DataSource_RuntimeReady();
    for (var i = 0; i < DataSource.order.length; i++)
    {
      var name = DataSource.order[i], result = await DataSource_CheckSource(DataSource.sources[name], permissions[name], locale);

      if (!result.ok) return result;
      checked[name] = result;
    }
    var urls = {};

    DataSource.order.forEach(function (name)
    {
      DataSource.sources[name].layer = checked[name].layer;
      DataSource.sources[name].urls = checked[name].urls;
      Object.keys(checked[name].urls).forEach(function (url) { urls[url] = checked[name].urls[url]; });
    });
    DataSource.layer = checked.data.layer;
    DataSource.modLayer = checked.mod.layer;
    DataSource.locale = locale;
    Module.srwData.useLocalFiles(urls);
    return { ok: true };
  }
  finally
  {
    DataSource.busy = false;
    DataSource_Update();
  }
}

function DataSource_Failed(result)
{
  var field = Game_SettingsFields.filter(function (f) { return f.key === result.key; })[0], source = field && field.source ? DataSource_Of(field) : null;

  DataSource.errors[result.key] = result.message;
  if (result.resetField && source)
  {
    source.typed = String(field.default);
    source.folder = null;
    Game_SetCookie(Game_FieldCookieName(field), String(field.default));
  }
  Shell_Print(result.message);
  Shell_FocusField(result.key);
  Module.setStatus('The ' + (source ? source.what : 'settings') + ' cannot be used.', 'error');
}

function DataSource_DecorateRow(row, control, forced, field)
{
  var source = DataSource_Of(field), browse = document.createElement('button');

  control.id = source.inputId;
  control.value = DataSource_Value(source);
  control.addEventListener('input', function () { source.typed = control.value; DataSource.errors[source.key] = ''; });
  browse.type = 'button';
  browse.textContent = 'Browse…';
  browse.title = 'Choose the folder of the ' + source.what + ' on this computer; its files are read where they are, nothing is uploaded';
  browse.disabled = forced;
  browse.addEventListener('click', function () { DataSource_Browse(source); });
  row.appendChild(browse);
  row.appendChild(DataSource_ResetButton(forced, 'Forget the chosen folder and put the default address back', function () { DataSource_Reset(source); }));
  DataSource_AddError(row, field);
  if (control.value === '' && DataSource_Hint(source)) control.setAttribute('placeholder', DataSource_Hint(source));
}

function DataSource_ResetButton(forced, title, action)
{
  var reset = document.createElement('button');

  reset.type = 'button';
  reset.textContent = 'Reset';
  reset.title = title;
  reset.disabled = forced;
  reset.addEventListener('click', action);
  return reset;
}

function DataSource_AddError(row, field)
{
  if (!DataSource.errors[field.key]) return;
  var message = document.createElement('div');

  message.className = 'settings-error';
  message.textContent = DataSource.errors[field.key];
  row.appendChild(message);
}

function DataSource_DecorateLocale(row, control, field, forced)
{
  control.id = field.inputId;
  control.addEventListener('input', function () { DataSource.errors[field.key] = ''; });
  row.appendChild(DataSource_ResetButton(forced, 'Put the default locale back (' + field.default + ')', function ()
  {
    control.value = String(field.default);
    DataSource.errors[field.key] = '';
    Game_SetCookie(Game_FieldCookieName(field), control.value);
  }));
  DataSource_AddError(row, field);
}

DataSource_Load();
