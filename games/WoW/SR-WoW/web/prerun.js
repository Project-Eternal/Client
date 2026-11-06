if (!Module.ENVIRONMENT_IS_PTHREAD)
{
  if (typeof document !== 'undefined' && Module.srwKeyboard) document.body.classList.toggle('is-phone', Module.srwKeyboard.isPhone);

  Module.preRun = Module.preRun || [];

  Module.preRun.push(function () {
    var queue = [];
    var running = false;

    function onResume(err)
    {
      if (err)
      {
        console.error(err);
      }
      if (queue.length === 0)
      {
        running = false;
        return;
      }
      (queue.shift())(onResume);
    }

    Module.SyncQueue = function (op) {
      if (running)
      {
        queue.push(op);
        return;
      }
      running = true;
      op(onResume);
    };
  });

  Module.preRun.push(function () {
    var kept = typeof Kept !== 'undefined' ? Kept_Paths(Kept_Owner('game')) : ['/WoW/WTF', '/WoW/Cache', '/WoW/Screenshots'];

    ['/Port', '/WoW/Logs', '/Sandbox'].concat(kept).forEach(function (dir) { FS.mkdirTree(dir); });
    if (typeof indexedDB === 'undefined')
    {
      console.warn("[Shell] IndexedDB is not available, so the settings will not be kept.");
      return;
    }
    kept.forEach(function (dir) { FS.mount(IDBFS, {}, dir); });

    var dependency = 'fs-sync';
    addRunDependency(dependency);
    FS.syncfs(true, function (err) {
      if (err)
      {
        console.error(err);
      }
      removeRunDependency(dependency);
    });
  });

  Module.onRuntimeInitialized = function () {
    Game_RuntimeReady = true;

    function syncNow()
    {
      if (Module.SyncQueue)
      {
        Module.SyncQueue(function (done) {
          FS.syncfs(false, done);
        });
      }
    }
    if (typeof indexedDB !== 'undefined')
    {
      setInterval(syncNow, 5000);
      document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'hidden') syncNow(); });
      window.addEventListener('pagehide', syncNow);
    }
    Module['srwOnExit'] = function (code)
    {
      Module['srwExited'] = true;
      if (typeof UserExport_Update === 'function') UserExport_Update();
      if (typeof indexedDB !== 'undefined') syncNow();
      if (typeof Acore_OnClientExit === 'function') Acore_OnClientExit();
    };

    if (typeof UserExport_Update === 'function') UserExport_Update();
    if (typeof Game_MaybeStartGame === 'function')
    {
      Game_MaybeStartGame();
    }
  };
}



if (typeof process === 'object' && typeof require === 'function' && process.env)
{
  if (process.env.WOW_NODE_FILES && !Module.ENVIRONMENT_IS_PTHREAD)
  {
    Module.preRun = Module.preRun || [];
    Module.preRun.push(function () {
      var fs = require('fs'), path = require('path');

      ['/Port', '/WoW/WTF', '/WoW/Cache', '/WoW/Screenshots', '/WoW/Logs', '/Sandbox'].forEach(function (dir) { FS.mkdirTree(dir); });
      (function copy(host, target)
      {
        fs.readdirSync(host).forEach(function (name)
        {
          if (target === '' && (name === 'Data' || /\.exe$/i.test(name) || /^manifest\.txt/.test(name))) return;
          var from = path.join(host, name), to;

          if (target === '' && Module.wowPortFile(name)) to = '/Port/' + name;
          else if (target === '' && name === 'Accounts') to = '/Sandbox/Accounts';
          else to = (target === '' ? '/WoW' : target) + '/' + name;
          if (fs.statSync(from).isDirectory()) { FS.mkdirTree(to); copy(from, to); }
          else FS.writeFile(to, fs.readFileSync(from));
        });
      })(process.env.WOW_NODE_FILES, '');
      var dependency = 'remote-files';
      addRunDependency(dependency);
      var locale = process.env.WOW_NODE_LOCALE || 'enGB';
      if (process.env.WOW_NODE_LOCALE) FS.writeFile('/Port/locale.txt', locale);
      Module.srwData.prepareClient(process.env.WOW_NODE_DATA || 'data/', { modUrl: process.env.WOW_NODE_MOD, locale: locale }).then(function () {
        try
        {
          var realmlist = /set\s+realmlist\s+(\S+)/i.exec(FS.readFile('/Port/realmlist.wtf', { encoding: 'utf8' }));
          if (realmlist) Module.wowSetCvar('realmList', realmlist[1]);
        }
        catch (e) { }
        if (process.env.WOW_NODE_LOCALE) Module.wowSetCvar('locale', locale);
        removeRunDependency(dependency);
      });
    });
  }
}

Module['wowGame'] = (function ()
{
  var pending = {};

  function call(kind, content)
  {
    return new Promise(function (resolve) {
      if (!runtimeInitialized) { setTimeout(function () { call(kind, content).then(resolve); }, 50); return; }
      if (!Module['_WoW_ControllerNew']) { resolve({ ok: false, bytes: null }); return; }
      var request;
      if (typeof content === 'string')
      {
        var length = lengthBytesUTF8(content) + 1, string = Module['_malloc'](length);
        stringToUTF8(content, string, length);
        request = Module['_WoW_ControllerNew'](kind, string);
        Module['_free'](string);
      }
      else
      {
        var copy = Module['_malloc'](content.length || 1);
        new Uint8Array(wasmMemory.buffer).set(content, copy);
        request = Module['_WoW_ControllerNewData'](kind, copy, content.length);
        Module['_free'](copy);
      }
      pending[request] = resolve;
      Module['_WoW_ControllerPost'](request);
    });
  }

  return {
    readFile: function (name) { return call(1, name).then(function (a) { return a.ok ? a.bytes : null; }); },
    command: function (text) { return call(2, text).then(function (a) { return a.ok ? new TextDecoder().decode(a.bytes) : null; }); },
    worldQueries: function (bytes) { return call(3, bytes).then(function (a) { return a.ok ? a.bytes : null; }); },
    done: function (request)
    {
      var words = new Int32Array(wasmMemory.buffer), w = request >> 2, state = words[w], data = words[w + 3] >>> 0, size = words[w + 4] >>> 0;
      var bytes = state > 0 ? new Uint8Array(wasmMemory.buffer).slice(data, data + size) : null;
      var resolve = pending[request];

      delete pending[request];
      Module['_WoW_ControllerFree'](request);
      if (resolve) resolve({ ok: state > 0, bytes: bytes });
    }
  };
})();


Module['wowPortFile'] = function (name)
{
  return /\.txt$/i.test(name) || /^realmlist\.wtf$/i.test(name);
};

Module['wowSetCvar'] = function (key, value)
{
  var path = '/WoW/WTF/Config.wtf', text = '', found = false;
  var re = new RegExp('^\\s*SET\\s+' + key + '\\s', 'i');

  try { text = FS.readFile(path, { encoding: 'utf8' }); } catch (e) { }
  var lines = text.split(/\r?\n/);
  for (var i = 0; i < lines.length; i++)
  {
    if (re.test(lines[i])) { lines[i] = 'SET ' + key + ' "' + value + '"'; found = true; }
  }
  if (!found)
  {
    if (lines.length && lines[lines.length - 1] === '') lines.pop();
    lines.push('SET ' + key + ' "' + value + '"');
  }
  FS.mkdirTree('/WoW/WTF');
  FS.writeFile(path, lines.join('\n') + '\n');
};


Module['srwStreamObserver'] = (function ()
{
  var element = null, background = null, circle = null, waiting = 0, stallBytes = 0, lastEnd = 0, color = '';
  var TINTS = { green: [0, 1, 0], yellow: [1, 0.82, 0], red: [1, 0, 0] }, images = { background: {}, circle: {} };

  function tint(url, kind)
  {
    var image = new Image();
    image.onload = function ()
    {
      Object.keys(TINTS).forEach(function (name)
      {
        var canvas = document.createElement('canvas'), context, pixels, i, t = TINTS[name];
        canvas.width = image.width; canvas.height = image.height;
        context = canvas.getContext('2d');
        context.drawImage(image, 0, 0);
        pixels = context.getImageData(0, 0, canvas.width, canvas.height);
        for (i = 0; i < pixels.data.length; i += 4) { pixels.data[i] *= t[0]; pixels.data[i + 1] *= t[1]; pixels.data[i + 2] *= t[2]; }
        context.putImageData(pixels, 0, 0);
        images[kind][name] = canvas.toDataURL();
      });
      if (color) setColor(color, true);
    };
    image.src = url;
  }

  function layer(parent, url)
  {
    var img = document.createElement('img');
    if (url) img.src = url;
    img.alt = '';
    parent.appendChild(img);
    return img;
  }

  function create()
  {
    var style = document.createElement('style');
    style.textContent =
      '@keyframes wow-stream-spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }' +
      '#wow-stream-icon { --wow-picture-height: min(100vh, calc(100vw / var(--wow-stream-aspect, 1.3333)));' +
      '  position: fixed; pointer-events: none; z-index: 5; opacity: 0; transition: opacity 3.333s linear 1s;' +
      '  left: 50%; top: calc((100vh - var(--wow-picture-height)) / 2 + var(--wow-picture-height) * 28 / 768);' +
      '  width: calc(var(--wow-picture-height) * 48 / 768); height: calc(var(--wow-picture-height) * 48 / 768); transform: translateX(-50%); }' +
      '#wow-stream-icon.on { opacity: 1; transition: none; }' +
      '#wow-stream-icon > div { position: absolute; inset: 0; }' +
      '#wow-stream-icon img { position: absolute; inset: 0; width: 100%; height: 100%; }' +
      '#wow-stream-icon .spin { animation: wow-stream-spin 8s linear infinite; }' +
      'body:not(.page-game) #wow-stream-icon { display: none; }';
    document.head.appendChild(style);
    element = document.createElement('div');
    element.id = 'wow-stream-icon';
    var frame = document.createElement('div'), spin = document.createElement('div');
    spin.className = 'spin';
    element.appendChild(frame);
    element.appendChild(spin);
    background = layer(frame, null);
    layer(frame, 'resources/StreamFrame.png');
    circle = layer(spin, null);
    layer(spin, 'resources/StreamSpark.png');
    tint('resources/StreamBackground.png', 'background');
    tint('resources/StreamCircle.png', 'circle');
    document.body.appendChild(element);
  }

  function setColor(name, force)
  {
    if (name === color && !force) return;
    color = name;
    if (images.background[name]) background.src = images.background[name];
    if (images.circle[name]) circle.src = images.circle[name];
  }


  function update()
  {
    var stalled;

    if (!element) return;
    stalled = waiting > 0;
    if (stalled)
    {
      setColor(stallBytes < 1048576 ? 'green' : stallBytes < 8 * 1048576 ? 'yellow' : 'red');
      element.classList.add('on');
    }
    else
    {
      element.classList.remove('on');
    }
  }

  if (typeof document !== 'undefined')
  {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', create); else create();
    setInterval(update, 50);
  }

  return {
    frame: function (width, height)
    {
      if (element && width > 0 && height > 0) element.style.setProperty('--wow-stream-aspect', String(width / height));
    },
    started: function ()
    {
      if (waiting === 0 && performance.now() - lastEnd > 1000) stallBytes = 0;
      waiting++;
      update();
    },
    received: function (bytes) { stallBytes += bytes; },
    ended: function ()
    {
      if (waiting > 0) waiting--;
      if (waiting === 0) lastEnd = performance.now();
      update();
    }
  };
})();


Module['wowProfPie'] = (function ()
{
  var NAMES = ['Client code', 'Draw calls', 'Other Direct3D calls', 'Shader compiles', 'Uploads to WebGL', 'Present', 'Waiting for the big lock',
               'Sleep', 'Waiting for objects', 'Data reads', "Server's world questions"];
  var COLORS = ['#4e79a7', '#f28e2b', '#e15759', '#76b7b2', '#59a14f', '#edc948', '#b07aa1', '#ff9da7', '#9c755f', '#bab0ac', '#17becf'];
  var box = null, canvas = null, title = null, legend = null, sequence = -1;

  function create()
  {
    var style = document.createElement('style');
    style.textContent =
      '#wow-prof { position: fixed; right: 8px; bottom: 8px; z-index: 6; pointer-events: none; display: none; padding: 8px 10px;' +
      '  background: rgba(0, 0, 0, 0.6); color: #fff; font: 12px/1.35 monospace; border-radius: 4px; }' +
      '#wow-prof.on { display: block; }' +
      'body:not(.page-game) #wow-prof { display: none; }' +
      '#wow-prof .title { margin-bottom: 4px; white-space: nowrap; }' +
      '#wow-prof .row { white-space: nowrap; }' +
      '#wow-prof .sw { display: inline-block; width: 9px; height: 9px; margin-right: 6px; }' +
      '#wow-prof canvas { display: block; margin: 4px auto 6px; }';
    document.head.appendChild(style);
    box = document.createElement('div');
    box.id = 'wow-prof';
    title = document.createElement('div');
    title.className = 'title';
    canvas = document.createElement('canvas');
    canvas.width = canvas.height = 150;
    legend = document.createElement('div');
    box.appendChild(title);
    box.appendChild(canvas);
    box.appendChild(legend);
    document.body.appendChild(box);
    setInterval(poll, 100);
  }

  function poll()
  {
    var address = Module['wowProf'], words, on, total, frames, i, parts = [], angle, context, ms, sleeper;

    if (!address || typeof HEAP32 === 'undefined') return;
    words = address >> 2;
    on = Atomics.load(HEAP32, words) !== 0;
    box.classList.toggle('on', on);
    if (!on) { sequence = -1; return; }
    if (Atomics.load(HEAP32, words + 1) === sequence) return;
    sequence = Atomics.load(HEAP32, words + 1);
    frames = Atomics.load(HEAP32, words + 2);
    total = Atomics.load(HEAP32, words + 3);
    if (frames <= 0 || total <= 0) { title.textContent = 'Frame time: measuring…'; return; }
    sleeper = Atomics.load(HEAP32, words + 4) >>> 0;
    for (i = 0; i < NAMES.length; i++) parts.push({ index: i, micro: Atomics.load(HEAP32, words + 5 + i) });
    ms = total / 1000 / frames;
    title.textContent = 'Frame time ' + ms.toFixed(1) + ' ms (' + (1000 / ms).toFixed(0) + ' fps), last ' + frames + ' frames';
    context = canvas.getContext('2d');
    context.clearRect(0, 0, canvas.width, canvas.height);
    angle = -Math.PI / 2;
    parts.forEach(function (part)
    {
      var sweep = part.micro / total * Math.PI * 2;
      if (sweep <= 0) return;
      context.beginPath();
      context.moveTo(75, 75);
      context.arc(75, 75, 72, angle, angle + sweep);
      context.closePath();
      context.fillStyle = COLORS[part.index];
      context.fill();
      angle += sweep;
    });
    parts.sort(function (a, b) { return b.micro - a.micro; });
    legend.textContent = '';
    parts.forEach(function (part)
    {
      var row = document.createElement('div'), sw = document.createElement('span'), label = NAMES[part.index];
      if (part.index === 7 && sleeper) label += ' (' + sleeper.toString(16) + ')';
      row.className = 'row';
      sw.className = 'sw';
      sw.style.background = COLORS[part.index];
      row.appendChild(sw);
      row.appendChild(document.createTextNode((part.micro / total * 100).toFixed(1).padStart(5) + '%  ' + (part.micro / 1000 / frames).toFixed(2).padStart(6) + ' ms  ' + label));
      legend.appendChild(row);
    });
  }

  if (typeof document !== 'undefined')
  {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', create); else create();
  }
  return {};
})();
