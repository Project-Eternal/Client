var Game_SettingsFields = [
  {
    key: 'gxResolution', label: 'Resolution', group: 'display', type: 'select', default: '1280x720',
    options: [['800x600', '800x600'], ['1024x768', '1024x768'], ['1280x720', '1280x720'], ['1280x1024', '1280x1024'], ['1920x1080', '1920x1080']]
  },
  { key: 'gxWindow', label: 'Windowed', group: 'display', type: 'yesno', default: '1', values: ['1', '0'] },
  { key: 'gxMaximize', label: 'Maximized window (no border)', group: 'display', type: 'yesno', default: '1', values: ['1', '0'] },
  { key: 'widescreen', label: 'Widescreen resolutions', group: 'display', type: 'yesno', default: '1', values: ['1', '0'] },
  { key: 'movie', label: 'Startup movies', group: 'display', type: 'yesno', default: '0', values: ['1', '0'] },
  { key: 'gxVSync', label: 'Vertical sync', group: 'display', type: 'yesno', default: '1', values: ['1', '0'] },

  { key: 'Sound_EnableAllSound', label: 'Sound', group: 'audio', type: 'onoff', default: '1', values: ['1', '0'] },
  { key: 'Sound_MasterVolume', label: 'Master volume (0-1)', group: 'audio', type: 'number', default: '1', min: 0, max: 1, step: 0.05 },

  { key: 'embeddedServer', label: 'Embedded EternalCore server', group: 'connection', type: 'yesno', default: '1', values: ['1', '0'], file: 'embeddedserver' },
  { key: 'realmlist', label: 'Realm list', group: 'connection', type: 'text', default: '127.0.0.1', file: 'realmlist' },
  { key: 'realm', label: 'Realm the embedded server starts on', group: 'connection', type: 'select', default: 'last',
    options: [['last', 'The realm used last'], ['1', 'Normal'], ['2', 'Sandbox (game master)']], file: 'acorerealm' },

  { key: 'netProxy', label: 'Network proxy', group: 'connection', type: 'text', default: '', file: 'netproxy' },

  { key: 'dataUrl', start: true, label: 'Client files location', group: 'locations', type: 'text', default: 'https://cdn.is4.site/WoW/3.3.5.12340/', file: 'dataroot', source: 'data' },
  { key: 'modUrl', start: true, label: 'Client mod location', group: 'locations', type: 'text', default: '', file: 'modroot', source: 'mod' },
  { key: 'locale', start: true, label: 'Locale', group: 'locations', type: 'text', default: 'enGB', file: 'locale', locale: true, inputId: 'locale-input' },
  { key: 'serverUrl', start: true, label: 'Server files location', group: 'locations', type: 'text', default: 'https://cdn.is4.site/AzerothCore/3.3.5.12340/', file: 'serverroot', source: 'server' },
  { key: 'serverModUrl', start: true, label: 'Server mod location', group: 'locations', type: 'text', default: '', file: 'servermodroot', source: 'servermod' },

  { key: 'p2p', start: true, label: 'Shared world with other players', group: 'multiplayer', type: 'yesno', default: '1', values: ['1', '0'], file: 'p2p' },
  { key: 'p2pNode', start: true, label: 'Relay nodes (MQTT over WebSocket, one address per line, e.g. wss://broker.example/mqtt)', group: 'multiplayer', type: 'lines', default: 'wss://relay1.is4.site/mqtt\nwss://relay2.is4.site/mqtt\nwss://broker.hivemq.com:8884/mqtt\nwss://broker.emqx.io:8084/mqtt', file: 'p2p' },
  { key: 'p2pShard', start: true, label: 'Private shard identifier (empty: the public shard)', group: 'multiplayer', type: 'text', default: '', file: 'p2p', shard: true },
  { key: 'p2pDirect', label: 'Direct links to other players', group: 'multiplayer', type: 'yesno', default: '0', values: ['1', '0'], file: 'p2p' },
  { key: 'p2pChat', label: 'Connection events in the chat (relay nodes reached or lost, direct links)', group: 'multiplayer', type: 'yesno', default: '1', values: ['1', '0'], file: 'p2p' },
  { key: 'p2pStun', label: 'STUN server for direct links (empty: no STUN server is used)', group: 'multiplayer', type: 'text', default: 'stun:stun.l.google.com:19302', file: 'p2p' },
  { key: 'p2pTurn', label: 'TURN server for players behind strict routers (optional)', group: 'multiplayer', type: 'text', default: '', file: 'p2p' },
  { key: 'p2pTurnUser', label: 'TURN user name', group: 'multiplayer', type: 'text', default: '', file: 'p2p' },
  { key: 'p2pTurnPassword', label: 'TURN password', group: 'multiplayer', type: 'text', default: '', file: 'p2p' },

  { key: 'dataCache', label: 'Keep the data in the browser', group: 'data', type: 'yesno', default: '1', values: ['1', '0'], file: 'datacache' },

  { key: 'd3dCull', label: 'Developer setting: Direct3D culling mode (0-2)', group: 'data', type: 'text', default: '0', file: 'd3dcull' },
  { key: 'present', label: 'How frames are handed to the page', group: 'data', type: 'select', default: 'pixels', options: [['pixels', 'As pixels'], ['bitmap', 'As image bitmaps']], file: 'present' },
  { key: 'd3dShaders', label: 'Pixel shaders', group: 'data', type: 'yesno', default: '1', values: ['1', '0'], file: 'd3dshaders' },
  { key: 'dynStream', label: 'Stream the data of changing buffers', group: 'data', type: 'yesno', default: '1', values: ['1', '0'], file: 'dynstream' },
  { key: 'd3dAlphaCoverage', label: 'Antialiased alpha test with multisampling', group: 'data', type: 'yesno', default: '1', phoneDefault: '0', values: ['1', '0'], file: 'd3dalphacoverage' },
  { key: 'checkAddonVersion', label: 'Check the version of add-ons', group: 'data', type: 'yesno', default: '0', values: ['1', '0'] },
  { key: 'd3dDebug', label: 'Developer setting: Direct3D debug mode (0-4)', group: 'data', type: 'text', default: '0', file: 'd3ddebug' },
  { key: 'bigLock', label: 'Big lock (only one thread runs the game code at a time; off: real parallel threads)', group: 'data', type: 'yesno', default: '0', values: ['1', '0'], file: 'biglock' },
  { key: 'soundReader', label: 'Who reads the sound files', group: 'data', type: 'select', default: 'client', options: [['client', 'The game'], ['native', 'The port (native reader)']], file: 'soundreader' },
  { key: 'sse', label: 'Report MMX, SSE and SSE2 to the game', group: 'data', type: 'yesno', default: '1', values: ['1', '0'], file: 'sse' },
  { key: 'debugLog', label: 'Developer setting: debug log categories at the start', group: 'data', type: 'text', default: '', file: 'debuglog' },
];

var Game_SettingsGroupLabels = { display: 'Display', audio: 'Audio', locations: 'Data locations', connection: 'Connection', multiplayer: 'Shared world', data: 'Data' };
var Game_SettingsGroupOrder = ['display', 'audio', 'locations', 'connection', 'multiplayer', 'data'];

var Game_PhoneFirstStart = {
  gxMultisample: '0',
  farclip: '300',
  groundEffectDensity: '16',
  groundEffectDist: '40',
  particleDensity: '0.3',
  spellEffectLevel: '4',
  projectedTextures: '0'
};

var Game_CookiePrefix = 'wow_cfg_';

function Game_OnPhone()
{
  return typeof Module !== 'undefined' && !!Module.srwKeyboard && Module.srwKeyboard.isPhone;
}

function Game_GetCookie(name)
{
  var re = new RegExp('(?:^|; )' + name.replace(/([.$?*|{}()\[\]\\\/+^])/g, '\\$1') + '=([^;]*)');
  var m = document.cookie.match(re);
  return m ? decodeURIComponent(m[1]) : null;
}

function Game_SetCookie(name, value)
{
  var maxAge = 10 * 365 * 24 * 60 * 60;
  document.cookie = name + '=' + encodeURIComponent(value) + '; path=/; max-age=' + maxAge + '; samesite=lax';
}

function Game_FieldCookieName(field)
{
  return field.cookieName || (Game_CookiePrefix + field.key);
}

function Game_ParseFragmentParams()
{
  var hash = location.hash;
  if (hash.length <= 1)
  {
    return new URLSearchParams();
  }
  var query = (hash.charAt(1) === '?') ? hash.substring(2) : hash.substring(1);
  return new URLSearchParams(query);
}

function Game_FieldUriValue(field, params)
{
  return params.has(field.key) ? params.get(field.key) : null;
}

function Game_FieldValue(field, params)
{
  var uriValue = Game_FieldUriValue(field, params);
  if (uriValue != null)
  {
    return uriValue;
  }
  var cookieValue = Game_GetCookie(Game_FieldCookieName(field));
  if (cookieValue != null) return cookieValue;
  return (field.phoneDefault !== undefined && Game_OnPhone()) ? field.phoneDefault : field.default;
}

function Game_EmbeddedServerWanted()
{
  var field = Game_SettingsFields.filter(function (f) { return f.key === 'embeddedServer'; })[0];

  return String(Game_FieldValue(field, Game_ParseFragmentParams())) === '1';
}

function Game_ApplyBootOverrides(firstStart)
{
  var params = Game_ParseFragmentParams();
  var portFiles = { netproxy: 1, d3dcull: 1, d3dalphacoverage: 1, d3dshaders: 1, present: 1, sse: 1, soundreader: 1, biglock: 1, debuglog: 1, d3ddebug: 1, dynstream: 1 };

  Game_SettingsFields.forEach(function (field) {
    var value = Game_FieldValue(field, params);

    if (!field.file)
    {
      if (Game_FieldUriValue(field, params) != null || Game_GetCookie(Game_FieldCookieName(field)) != null) Module.wowSetCvar(field.key, value);
    }
    else if (field.file === 'realmlist')
    {
      FS.writeFile('/Port/realmlist.wtf', 'set realmlist ' + value + '\n');
      Module.wowSetCvar('realmList', value);
    }
    else if (field.file === 'locale')
    {
      FS.writeFile('/Port/locale.txt', String(value));
      Module.wowSetCvar('locale', value);
    }
    else if (field.file === 'netproxy')
    {
      FS.writeFile('/Port/netproxy.txt', value ? (value.indexOf('acore:') === 0 ? value : new URL(value, location.href).href.replace(/^http/, 'ws')) : '');
    }
    else if (field.file === 'acorerealm')
    {
      var last = 1;
      try { last = Number(localStorage.getItem('wow-acore-realm')) === 2 ? 2 : 1; } catch (e) { }
      Module['acoreRealm'] = String(value) === 'last' ? last : String(value) === '2' ? 2 : 1;
    }
    else if (field.file === 'embeddedserver')
    {
      Module['acoreEnabled'] = String(value) === '1';
    }
    else if (field.file === 'p2p')
    {
      (Module['p2pSettings'] = Module['p2pSettings'] || {})[field.key] = String(value);
    }
    else if (field.file === 'datacache')
    {
      if (String(value) === '0' && Module['srwData']) Module['srwData'].disable();
      else Module['srwDataCache'] = true;
    }
    else if (portFiles[field.file])
    {
      FS.writeFile('/Port/' + field.file + '.txt', String(value));
    }
  });

  if (firstStart && Game_OnPhone())
  {
    Object.keys(Game_PhoneFirstStart).forEach(function (key) { Module.wowSetCvar(key, Game_PhoneFirstStart[key]); });
  }

  if (Module['acoreEnabled'])
  {
    FS.writeFile('/Port/realmlist.wtf', 'set realmlist acore.local\n');
    Module.wowSetCvar('realmList', 'acore.local');
    FS.writeFile('/Port/netproxy.txt', 'acore:');
  }
}

function Game_ShardRandom()
{
  var bytes = new Uint8Array(16), alphabet = 'abcdefghijklmnopqrstuvwxyz234567', value = 0, bits = 0, out = '', i;

  if (typeof crypto !== 'undefined' && crypto.getRandomValues) crypto.getRandomValues(bytes);
  else for (i = 0; i < 16; i++) bytes[i] = Math.floor(Math.random() * 256);
  for (i = 0; i < 16; i++)
  {
    value = ((value << 8) | bytes[i]) & 0xFFFF;
    bits += 8;
    while (bits >= 5)
    {
      out += alphabet[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += alphabet[(value << (5 - bits)) & 31];
  return out;
}

function Game_ShardWarning(value)
{
  var text = String(value || '').trim();

  return text && text.length < 16 ? 'A short identifier can be guessed from the name of its shard on the relay node. Make a long one with the button.' : '';
}

function Game_DecorateShard(row, control, forced)
{
  var button = document.createElement('button'), warning = document.createElement('span');

  button.type = 'button';
  button.textContent = 'Generate';
  button.className = 'settings-shard-generate';
  button.disabled = forced;
  warning.className = 'settings-shard-warning';
  warning.setAttribute('role', 'status');
  function refresh() { warning.textContent = Game_ShardWarning(control.value); }
  button.addEventListener('click', function (event)
  {
    event.preventDefault();
    control.value = Game_ShardRandom();
    Game_SetCookie(Game_FieldCookieName({ key: 'p2pShard' }), control.value);
    refresh();
  });
  control.addEventListener('input', refresh);
  row.appendChild(button);
  row.appendChild(warning);
  refresh();
}

function Game_BuildFieldRow(field, params)
{
  var row = document.createElement('tr');
  row.className = 'settings-row';

  var label = document.createElement('label');
  label.className = 'settings-label';
  label.textContent = field.label;

  var cell = document.createElement('td'), box = document.createElement('div');
  box.className = 'settings-control';
  cell.appendChild(box);

  var forced = Game_FieldUriValue(field, params) != null;
  var effective = Game_FieldValue(field, params);

  var control;
  if (field.type === 'lines')
  {
    control = document.createElement('textarea');
    control.rows = 3;
    control.spellcheck = false;
    control.value = effective;
    control.addEventListener('input', function () { Game_SetCookie(Game_FieldCookieName(field), control.value); });
  }
  else if (field.type === 'number' || field.type === 'text')
  {
    control = document.createElement('input');
    control.type = field.type;
    if (field.min != null) control.min = field.min;
    if (field.max != null) control.max = field.max;
    if (field.step != null) control.step = field.step;
    control.value = effective;
    if (!field.source) control.addEventListener('input', function () {
      if (control.value === '' && !field.shard)
      {
        return;
      }
      Game_SetCookie(Game_FieldCookieName(field), control.value);
    });
  }
  else
  {
    control = document.createElement('select');

    var values = field.values || ['1', '0'];
    var opts = field.options;
    if (!opts)
    {
      opts = (field.type === 'onoff') ? [[values[0], 'On'], [values[1], 'Off']] : [[values[0], 'Yes'], [values[1], 'No']];
    }
    opts.forEach(function (pair) {
      var opt = document.createElement('option');
      opt.value = pair[0];
      opt.textContent = pair[1];
      control.appendChild(opt);
    });

    if (!opts.some(function (pair) { return String(pair[0]) === String(effective); }))
    {
      var other = document.createElement('option');
      other.value = effective;
      other.textContent = effective;
      control.appendChild(other);
    }
    control.value = effective;
    control.addEventListener('change', function () {
      Game_SetCookie(Game_FieldCookieName(field), control.value);
    });
  }

  control.disabled = forced;
  control.dataset.key = field.key;
  box.appendChild(control);
  if (field.source) DataSource_DecorateRow(box, control, forced, field);
  else if (field.locale) DataSource_DecorateLocale(box, control, field, forced);
  else if (field.shard) Game_DecorateShard(box, control, forced);

  if (!control.id) control.id = 'field-' + field.key;
  label.htmlFor = control.id;
  if (control.tagName === 'TEXTAREA')
  {
    cell.colSpan = 2;
    box.insertBefore(label, control);
    row.appendChild(cell);
  }
  else
  {
    var labelCell = document.createElement('td');
    labelCell.appendChild(label);
    row.appendChild(labelCell);
    row.appendChild(cell);
  }

  return row;
}

function Game_BuildFieldTable()
{
  var table = document.createElement('table'), body = document.createElement('tbody');

  table.className = 'settings-table';
  table.appendChild(body);
  return table;
}

function Game_BuildSettingsPanel()
{
  var panel = document.getElementById('settings-panel');
  if (!panel)
  {
    return;
  }
  if (typeof Shell_ClearStartRows === 'function') Shell_ClearStartRows();
  panel.innerHTML = '';

  var params = Game_ParseFragmentParams();
  var byGroup = {};
  Game_SettingsFields.forEach(function (f) {
    (byGroup[f.group] = byGroup[f.group] || []).push(f);
  });

  Game_SettingsGroupOrder.forEach(function (g) {
    var fields = byGroup[g];
    if (!fields || !fields.length)
    {
      return;
    }

    var fieldset = document.createElement('fieldset');
    var legend = document.createElement('legend');
    legend.textContent = Game_SettingsGroupLabels[g];
    fieldset.appendChild(legend);

    var table = Game_BuildFieldTable();
    fields.forEach(function (field) {
      table.tBodies[0].appendChild(Game_BuildFieldRow(field, params));
    });
    fieldset.appendChild(table);

    if (g === 'connection')
    {
      var about = document.createElement('p');
      about.className = 'settings-about';
      about.appendChild(document.createTextNode('EternalCore, based on AzerothCore, licensed under the GNU GPL. Source code: '));
      var link = document.createElement('a');
      link.href = 'https://github.com/Project-Eternal/EternalCore';
      link.target = '_blank';
      link.rel = 'noopener';
      link.textContent = 'github.com/Project-Eternal/EternalCore';
      about.appendChild(link);
      fieldset.appendChild(about);
    }

    panel.appendChild(fieldset);
  });
}
