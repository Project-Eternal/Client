Module['srwCanvasArea'] = function ()
{
  var canvas = Module['canvas'] || (typeof document !== 'undefined' && document.getElementById('canvas'));
  if (!canvas || !canvas.width || !canvas.height) return null;
  var r = canvas.getBoundingClientRect();
  if (r.width <= 0 || r.height <= 0) return null;
  var scale = Math.min(r.width / canvas.width, r.height / canvas.height);
  return { left: r.left + (r.width - canvas.width * scale) / 2, top: r.top + (r.height - canvas.height * scale) / 2, scale: scale,
           width: canvas.width, height: canvas.height };
};


Module['srwShowFrame'] = function (bitmap, counterPointer)
{
  if (Module['srwStreamObserver']) Module['srwStreamObserver'].frame(bitmap.width, bitmap.height);
  try
  {
    var canvas = Module['canvas'] || (typeof document !== 'undefined' && document.getElementById('canvas'));

    if (canvas)
    {
      if (canvas.width !== bitmap.width || canvas.height !== bitmap.height)
      {
        canvas.width = bitmap.width;
        canvas.height = bitmap.height;
      }
      if (!canvas.srwContext) canvas.srwContext = canvas.getContext('bitmaprenderer');
      canvas.srwContext.transferFromImageBitmap(bitmap);
    }
    else
    {
      bitmap.close();
    }
  }
  catch (e)
  {
    console.error('[Shell] A frame could not be shown: ' + e);
  }
  Atomics.sub(HEAP32, counterPointer >> 2, 1);
};


Module['srwShowPixels'] = function (pointer, width, height, counterPointer)
{
  if (Module['srwStreamObserver']) Module['srwStreamObserver'].frame(width, height);
  try
  {
    var canvas = Module['canvas'] || (typeof document !== 'undefined' && document.getElementById('canvas'));

    if (canvas)
    {
      if (canvas.width !== width || canvas.height !== height)
      {
        canvas.width = width;
        canvas.height = height;
        canvas.srwImage = null;
      }
      if (!canvas.srwContext) canvas.srwContext = canvas.getContext('2d', { alpha: false });
      if (!canvas.srwImage) canvas.srwImage = canvas.srwContext.createImageData(width, height);
      canvas.srwImage.data.set(HEAPU8.subarray(pointer, pointer + width * height * 4));
      canvas.srwContext.putImageData(canvas.srwImage, 0, 0);
    }
  }
  catch (e)
  {
    console.error('[Shell] Pixels could not be shown: ' + e);
  }
  Atomics.sub(HEAP32, counterPointer >> 2, 1);
};

if (typeof process === 'object' && typeof require === 'function' && process.env)
{
  if (process.env.SRW_FAKE_GL && typeof OffscreenCanvas === 'undefined')
  {
    globalThis.OffscreenCanvas = function (width, height) { this.width = width; this.height = height; };
    if (typeof WebGLRenderingContext === 'undefined') globalThis.WebGLRenderingContext = function () {};
    if (typeof navigator === 'undefined') globalThis.navigator = { userAgent: 'Node', hardwareConcurrency: 4 };
    globalThis.OffscreenCanvas.prototype.getContext = function (kind, attributes)
    {
      var canvas = this, nothing = function () {};
      var create = function () { return {}; };
      var parameters = {
        0x0D33: 8192, 0x84E8: 8192, 0x851C: 8192, 0x8869: 16, 0x8872: 16, 0x8B4D: 32, 0x8B4C: 16, 0x8DFB: 1024, 0x8DFC: 32, 0x8DFD: 1024,
        0x84FF: 16, 0x8CDF: 8, 0x8824: 8, 0x8D57: 4, 0x0D3A: new Int32Array([8192, 8192]), 0x846E: new Float32Array([1, 1]),
        0x846D: new Float32Array([1, 1024]), 0x1F00: 'Node', 0x1F01: 'fake WebGL', 0x1F02: 'WebGL 2.0 (fake)', 0x8B8C: 'WebGL GLSL ES 3.00',
        0x9245: 'Node', 0x9246: 'fake WebGL', 0x0D50: 8, 0x0D52: 8, 0x0D53: 8, 0x0D54: 8, 0x0D55: 8, 0x0D56: 24, 0x0D57: 8
      };
      var context = {
        canvas: canvas,
        get drawingBufferWidth() { return canvas.width; },
        get drawingBufferHeight() { return canvas.height; },
        getParameter: function (name) { if (name === 0x0BA2) return new Int32Array([0, 0, canvas.width, canvas.height]); return name in parameters ? parameters[name] : 0; },
        getContextAttributes: function () { return attributes || {}; },
        isContextLost: function () { return false; },
        getSupportedExtensions: function () { return ['WEBGL_compressed_texture_s3tc', 'EXT_texture_filter_anisotropic', 'WEBGL_debug_renderer_info']; },
        getExtension: function (name)
        {
          if (name === 'WEBGL_debug_renderer_info') return { UNMASKED_VENDOR_WEBGL: 0x9245, UNMASKED_RENDERER_WEBGL: 0x9246 };
          if (name === 'WEBGL_compressed_texture_s3tc') return { COMPRESSED_RGB_S3TC_DXT1_EXT: 0x83F0, COMPRESSED_RGBA_S3TC_DXT1_EXT: 0x83F1, COMPRESSED_RGBA_S3TC_DXT3_EXT: 0x83F2, COMPRESSED_RGBA_S3TC_DXT5_EXT: 0x83F3 };
          if (name === 'EXT_texture_filter_anisotropic') return { TEXTURE_MAX_ANISOTROPY_EXT: 0x84FE, MAX_TEXTURE_MAX_ANISOTROPY_EXT: 0x84FF };
          return null;
        },
        getError: function () { return 0; },
        checkFramebufferStatus: function () { return 0x8CD5; },
        getShaderParameter: function () { return true; },
        getProgramParameter: function (program, name) { return name === 0x8B82 || name === 0x8B83 || name === 0x8B81 ? true : 0; },
        getShaderInfoLog: function () { return ''; },
        getProgramInfoLog: function () { return ''; },
        getShaderPrecisionFormat: function () { return { rangeMin: 127, rangeMax: 127, precision: 23 }; },
        getUniformLocation: create,
        getAttribLocation: function () { return 0; },
        getActiveUniform: function () { return null; },
        getActiveAttrib: function () { return null; },
        clientWaitSync: function () { return 0x911A; },
        getSyncParameter: function () { return 0x9119; },
        getQueryParameter: function (query, name) { return name === 0x8867 ? true : 0; },
        getInternalformatParameter: function () { return new Int32Array([4]); },
        isEnabled: function () { return false; }
      };
      ['createShader', 'createProgram', 'createBuffer', 'createTexture', 'createFramebuffer', 'createRenderbuffer', 'createVertexArray',
       'createQuery', 'createSampler', 'createTransformFeedback', 'fenceSync'].forEach(function (name) { context[name] = create; });
      return new Proxy(context, { get: function (target, name) { return name in target ? target[name] : (typeof name === 'string' && /^[a-z]/.test(name) ? nothing : undefined); } });
    };
  }
}


Module['srwData'] = (function ()
{
  var config = Module['srwProgram'];
  var CLIENT_CACHE = config.caches.client, SERVER_CACHE = config.caches.server;
  var OLD_CACHES = config.caches.old;
  var opened = {};
  var versions = {};
  var knownVersions = {};
  var stats = { hits: 0, misses: 0, fetched: 0, prefetched: 0, local: 0, requests: 0, retries: 0, shorts: 0, active: 0 };
  var net = { retries: 4, pause: 250, onChange: null };
  var localFiles = {};
  var inflight = {};
  var joined = {};
  var gone = {};

  function missingError(url)
  {
    var e = new Error(url + ': the file is missing');

    e.missing = true;
    return e;
  }

  function blockKey(url, start, length, tag)
  {
    return url + (url.indexOf('?') < 0 ? '?' : '&') + config.name + '-block=' + start + '-' + length + '-' + encodeURIComponent(tag);
  }

  function purge(url)
  {
    if (typeof caches === 'undefined') return Promise.resolve();
    return Promise.all([CLIENT_CACHE, SERVER_CACHE].map(function (name)
    {
      return caches.open(name).then(function (cache)
      {
        return cache.keys(url, { ignoreSearch: true }).then(function (keys)
        {
          return Promise.all(keys.map(function (request) { return cache.delete(request); }));
        });
      }).catch(function () {});
    }));
  }

  function partVersion(url)
  {
    if (knownVersions[url]) return Promise.resolve({ tag: knownVersions[url], size: 0 });
    return netFetch(url, { method: 'HEAD', cache: 'no-cache' }).then(function (response)
    {
      var length = response.headers.get('content-length');
      var tag = response.headers.get('etag') || response.headers.get('last-modified') || ('size-' + length);

      return { tag: tag, size: Number(length) || 0 };
    });
  }

  function versionOf(url)
  {
    if (versions[url]) return versions[url];
    var storedKey = config.name + '-version:' + url, stored = null;

    try { stored = localStorage.getItem(storedKey); } catch (e) { }
    var asked = joined[url] ? Promise.all(joined[url].parts.map(function (part) { return partVersion(part.url); })).then(function (list)
    {
      return { tag: list.map(function (version) { return version.tag; }).join('+'), size: 0 };
    }) : partVersion(url);

    asked = asked.then(null, function (e)
    {
      if (e && e.missing) gone[url] = true;
      throw e;
    });
    versions[url] = asked.then(function (version)
    {
      var changed = stored !== null && stored !== version.tag;

      try { localStorage.setItem(storedKey, version.tag); } catch (e) { }
      return changed ? purge(url).then(function () { return version; }) : version;
    }, function ()
    {
      return { tag: stored || 'unknown', size: 0 };
    });
    return versions[url];
  }

  function netFetch(url, options)
  {
    options = options || {};
    var ranged = options.start !== undefined, head = options.method === 'HEAD', want = ranged ? options.end : Infinity;
    var chunks = [], first = 0, next = 0;
    var tag = null, firstAnswer = null, failures = 0, restart = false, unverifiable = false;

    function changed(delta)
    {
      var icon = Module['srwStreamObserver'];

      stats.active += delta;
      if (icon) { try { if (delta > 0) icon.started(); else icon.ended(); } catch (e) { } }
      if (net.onChange) { try { net.onChange(stats.active); } catch (e) { } }
    }

    function forget() { chunks = []; first = next = 0; tag = null; firstAnswer = null; }

    function retryable(message)
    {
      var e = new Error(message);

      e.retry = true;
      return e;
    }

    function readBody(response, origin)
    {
      var length = response.headers.get('content-length');

      unverifiable = (response.headers.get('content-encoding') || 'identity').toLowerCase() !== 'identity';
      length = (length === null || unverifiable) ? null : Number(length);
      if (chunks.length && response.status === 200) forget();
      if (chunks.length && origin !== next)
      {
        forget();
        response.body && response.body.cancel && response.body.cancel().catch(function () {});
        return Promise.reject(retryable('range'));
      }
      if (!chunks.length)
      {
        first = next = origin;
        firstAnswer = { status: response.status, headers: response.headers };
        var etag = response.headers.get('etag'), modified = response.headers.get('last-modified');

        tag = (etag && !/^W\//.test(etag)) ? etag : (modified || null);
      }
      var answered = 0;

      function complete()
      {
        if (next >= want) return true;
        if (length === null) return true;
        return answered === length;
      }
      function got(bytes)
      {
        if (!bytes.length) return;
        chunks.push(bytes);
        if (Module['srwStreamObserver']) Module['srwStreamObserver'].received(bytes.length);
        next += bytes.length;
        answered += bytes.length;
      }

      if (!response.body || !response.body.getReader)
      {
        return response.arrayBuffer().then(function (buffer) { got(new Uint8Array(buffer)); if (!complete()) throw retryable('short'); });
      }
      var reader = response.body.getReader();

      return (function readNext()
      {
        return reader.read().then(function (r)
        {
          if (r.done) { if (!complete()) throw retryable('short'); return; }
          got(r.value);
          if (next >= want) { reader.cancel().catch(function () {}); return; }
          return readNext();
        });
      })();
    }

    function attempt()
    {
      var init = { method: head ? 'HEAD' : 'GET' }, from = chunks.length ? next : (ranged ? options.start : 0), headers = {};

      if (options.cache) init.cache = options.cache;
      if (!head && (chunks.length || ranged))
      {
        headers['Range'] = 'bytes=' + from + '-' + (ranged ? want - 1 : '');
        if (chunks.length && tag) headers['If-Range'] = tag;
        init.headers = headers;
      }
      stats.requests++;
      return fetch(url, init).then(null, function (e) { e.noAnswer = true; throw e; }).then(function (response)
      {
        if (response.status !== 200 && response.status !== 206)
        {
          var e = new Error('status ' + response.status);

          e.status = response.status;
          e.statusText = response.statusText;
          if (response.status === 404 || response.status === 410 || response.status === 416) { e.final = true; e.missing = true; }
          else if (response.status >= 400 && response.status < 500) e.client = true;
          else if (response.status < 500 || response.status > 599) e.final = true;
          throw e;
        }
        if (head) { firstAnswer = { status: response.status, headers: response.headers }; return; }
        var range = /^bytes (\d+)-/.exec(response.headers.get('content-range') || '');

        return readBody(response, response.status === 206 ? (range ? Number(range[1]) : from) : 0);
      }).catch(function (e)
      {
        if (e.final) throw e;
        if (!e.retry && unverifiable) forget();
        else if (e.noAnswer && init.headers && init.headers['If-Range']) forget();
        if (e.retry) stats.shorts++;
        if (++failures > net.retries) { if (e.client) e.missing = true; throw e; }
        stats.retries++;
        return new Promise(function (resolve) { setTimeout(resolve, net.pause * Math.pow(2, failures - 1)); }).then(attempt);
      });
    }

    changed(1);
    return attempt().then(function ()
    {
      var buffer;

      if (head) return { status: firstAnswer.status, headers: firstAnswer.headers, buffer: new ArrayBuffer(0) };
      var joined = new Uint8Array(next - first), at = 0;

      chunks.forEach(function (chunk) { joined.set(chunk, at); at += chunk.length; });
      if (ranged)
      {
        var from = options.start - first, to = Math.min(want, next) - first;

        buffer = joined.slice(Math.max(0, from), Math.max(0, to)).buffer;
      }
      else
      {
        if (first !== 0) throw new Error('the answer does not start at the beginning of the file');
        buffer = joined.buffer;
      }
      return { status: firstAnswer.status, headers: firstAnswer.headers, buffer: buffer };
    }).then(function (result) { changed(-1); return result; }, function (e) { changed(-1); throw e; });
  }

  function fetchRange(url, start, end)
  {
    if (gone[url]) return Promise.reject(missingError(url));
    var asked;

    if (joined[url])
    {
      var at = 0, pieces = [];

      joined[url].parts.forEach(function (part)
      {
        var from = Math.max(start, at), to = Math.min(end, at + part.size);

        if (from < to) pieces.push(netFetch(part.url, { start: from - at, end: to - at }).then(function (result)
        {
          if (result.buffer.byteLength !== to - from) throw new Error('short read');
          return result.buffer;
        }));
        at += part.size;
      });
      asked = Promise.all(pieces).then(function (buffers)
      {
        var all = new Uint8Array(end - start), position = 0;

        buffers.forEach(function (buffer) { all.set(new Uint8Array(buffer), position); position += buffer.byteLength; });
        if (position !== end - start) throw new Error('short read');
        return all.buffer;
      });
    }
    else
    {
      asked = netFetch(url, { start: start, end: end }).then(function (result)
      {
        if (result.buffer.byteLength !== end - start) throw new Error('short read');
        return result.buffer;
      });
    }
    return asked.then(function (buffer) { stats.fetched += buffer.byteLength; return buffer; }, function (e)
    {
      if (e && e.missing) gone[url] = true;
      throw e;
    });
  }

  function wholeFile(url)
  {
    if (gone[url]) return Promise.reject(missingError(url));
    var addresses = joined[url] ? joined[url].parts.map(function (part) { return part.url; }) : [url];

    return Promise.all(addresses.map(function (address)
    {
      return netFetch(address, { cache: 'no-cache' }).then(function (result) { return result.buffer; });
    })).then(function (buffers)
    {
      if (buffers.length === 1) return buffers[0];
      var all = new Uint8Array(buffers.reduce(function (sum, buffer) { return sum + buffer.byteLength; }, 0)), position = 0;

      buffers.forEach(function (buffer) { all.set(new Uint8Array(buffer), position); position += buffer.byteLength; });
      return all.buffer;
    }).then(null, function (e)
    {
      var error = e.status ? new Error(url + ': ' + e.status) : e;

      if (e.missing) { gone[url] = true; error.missing = true; }
      throw error;
    });
  }

  function storage(name)
  {
    if (Module['srwDataCache'] === false || typeof caches === 'undefined') return Promise.resolve(null);
    if (!opened[name])
    {
      OLD_CACHES.forEach(function (old) { caches.delete(old).catch(function () {}); });
      opened[name] = caches.open(name).catch(function () { return null; });
    }
    return opened[name];
  }

  function finish(request, state, got, cached, heap)
  {
    var words = new Int32Array(heap());


    words[(request >> 2) + 9] = got;
    words[(request >> 2) + 10] = cached || 0;
    Atomics.store(words, request >> 2, state);
    Atomics.notify(words, request >> 2);
  }

  var underNode = typeof process === 'object' && typeof require === 'function' && typeof window === 'undefined';

  function isHostPath(url) { return underNode && (/^join:/.test(url) || !/^[a-z][a-z0-9+.-]*:/i.test(url)); }

  function readText(url)
  {
    if (isHostPath(url)) { try { return Promise.resolve(require('fs').readFileSync(url, 'utf8')); } catch (e) { return Promise.resolve(null); } }
    return netFetch(url, { cache: 'no-cache' }).then(function (result) { return new TextDecoder().decode(result.buffer); }, function () { return null; });
  }

  function readFile(url, cacheName)
  {
    if (isHostPath(url))
    {
      var fs = require('fs'), match = /^join:((?:\d+:)+)(.*)$/.exec(url);

      if (!match) return Promise.resolve(new Uint8Array(fs.readFileSync(url)));
      return Promise.resolve(new Uint8Array(Buffer.concat(match[1].split(':').filter(Boolean).map(function (size, k)
      {
        return fs.readFileSync(match[2] + '.' + ('00' + k).slice(-3));
      }))));
    }
    return Module['srwData'].bytes(url, cacheName);
  }

  function encodePath(path) { return path.split('/').map(encodeURIComponent).join('/'); }

  function parseManifest(text, root, name)
  {
    var entries = [];

    text.split(/\r?\n/).forEach(function (line)
    {
      var fields = line.split('\t');

      if (fields.length < 2 || line.charAt(0) === '#') return;
      var path = fields[0].replace(/\\/g, '/').replace(/^\/+/, ''), url = isHostPath(root) ? root + path : root + encodePath(path);

      if (fields[2]) knownVersions[url] = fields[2];
      entries.push({ path: path, size: Number(fields[1]) || 0, url: url, layer: name, version: fields[2] || null });
    });
    return entries;
  }

  function joinParts(entries)
  {
    var byPath = {}, taken = {}, made = {};

    entries.forEach(function (entry) { byPath[entry.path.toLowerCase()] = entry; });
    entries.forEach(function (entry)
    {
      var lower = entry.path.toLowerCase();

      if (lower.length <= 4 || lower.slice(-4) !== '.000') return;
      var base = lower.slice(0, -4);

      if (byPath[base]) return;
      var parts = [entry], next;

      for (var k = 1; (next = byPath[base + '.' + ('00' + k).slice(-3)]) && k < 1000; k++) parts.push(next);
      parts.forEach(function (part) { taken[part.path.toLowerCase()] = true; if (part.version) knownVersions[part.url] = part.version; });
      var path = entry.path.slice(0, -4), url = entry.url.slice(0, -4), size = parts.reduce(function (sum, part) { return sum + part.size; }, 0);
      var versions = parts.map(function (part) { return part.version; }), host = isHostPath(entry.url);

      if (host) url = 'join:' + parts.map(function (part) { return part.size; }).join(':') + ':' + url;
      made[lower] = { path: path, size: size, url: url, layer: entry.layer, version: versions.every(function (v) { return v; }) ? versions.join('+') : null,
                      parts: parts.map(function (part) { return { url: part.url, size: part.size, version: part.version }; }) };
      if (!host && url.indexOf('local:') !== 0) joined[url] = made[lower];
      if (made[lower].version) knownVersions[url] = made[lower].version;
    });
    if (!Object.keys(made).length) return entries;
    var result = [];

    entries.forEach(function (entry)
    {
      var lower = entry.path.toLowerCase();

      if (!taken[lower]) result.push(entry);
      else if (made[lower]) result.push(made[lower]);
    });
    return result;
  }

  function droppedDirectories()
  {
    var kept = typeof Kept !== 'undefined' ? Kept_Owner('game').dirs : ['WTF', 'Cache', 'Screenshots'];

    return kept.concat(['Logs', 'Errors']).map(function (dir) { return dir.toLowerCase() + '/'; });
  }

  function withoutPlayerDirectories(entries)
  {
    var dropped = droppedDirectories();

    return entries.filter(function (entry)
    {
      var path = entry.path.toLowerCase();

      return !dropped.some(function (dir) { return path.indexOf(dir) === 0; });
    });
  }

  function loadDataLayer(url, name, keepAll)
  {
    var root = /\/$/.test(url) ? url : url + '/';
    var read = isHostPath(root) ? new Promise(function (resolve, reject)
    {
      try { resolve(require('fs').readFileSync(root + 'manifest.txt', 'utf8')); } catch (e) { reject(new Error(root + 'manifest.txt: ' + e.message)); }
    }) : netFetch(root + 'manifest.txt', { cache: 'no-cache' }).then(function (result)
    {
      return new TextDecoder().decode(result.buffer);
    }, function (e)
    {
      if (e.status) throw new Error(root + 'manifest.txt: the server answered ' + e.status + ' ' + e.statusText);
      throw new Error(root + 'manifest.txt: no answer (the server cannot be reached, or it does not allow this page to read it from another site)');
    });

    return read.then(function (text) { var entries = joinParts(parseManifest(text, root, name || 'data'));

      return keepAll ? entries : withoutPlayerDirectories(entries);
    });
  }

  function linkLocale(entries, locale)
  {
    if (!/^[a-z]{2}[A-Z]{2}$/.test(locale || '')) return entries;
    var have = {}, added = [], target = 'Data/' + locale + '/';

    entries.forEach(function (entry) { have[entry.path.toLowerCase()] = true; });
    ['data/' + locale.substring(0, 2).toLowerCase() + '/', 'data/_/'].forEach(function (prefix)
    {
      entries.forEach(function (entry)
      {
        if (entry.path.toLowerCase().indexOf(prefix) !== 0) return;
        var path = target + entry.path.substring(prefix.length), key = path.toLowerCase();

        if (have[key]) return;
        have[key] = true;
        added.push({ path: path, size: entry.size, url: entry.url, layer: entry.layer, version: entry.version, parts: entry.parts });
      });
    });
    return added.length ? entries.concat(added) : entries;
  }

  function missingFiles(entries, locale)
  {
    var required = typeof Game_Version !== 'undefined' && Game_Version.required ? Game_Version.required : [], have = {};

    required = required.map(function (path) { return path.replace(/\{locale\}/g, locale || 'enGB'); });
    entries.forEach(function (entry) { have[entry.path.toLowerCase()] = true; });
    return required.filter(function (path) { return !have[path.toLowerCase()]; });
  }

  function loadRemote(roots)
  {
    var names = ['data', 'patch', 'init', 'mod'];

    return Promise.all(names.map(function (name)
    {
      var root = roots[name];

      if (name === 'data' && roots.dataLayer) return roots.dataLayer;
      if (name === 'mod') return roots.modLayer || null;
      if (!root) return null;
      if (!/\/$/.test(root)) root += '/';
      return readText(root + 'manifest.txt').then(function (text)
      {
        if (text === null) { console.warn('[Shell] No manifest for the remote file system ' + root); return []; }
        return joinParts(parseManifest(text, root, name));
      });
    })).then(function (layers)
    {
      var files = {}, init = {};

      layers = layers.map(function (layer) { return layer && roots.locale ? linkLocale(layer, roots.locale) : layer; });
      (layers[0] || []).forEach(function (e) { files[e.path.toLowerCase()] = e; });
      (layers[3] || []).forEach(function (e) { files[e.path.toLowerCase()] = e; });
      (layers[2] || []).forEach(function (e) { var key = e.path.toLowerCase(); init[key] = e; delete files[key]; });
      (layers[1] || []).forEach(function (e) { var key = e.path.toLowerCase(); files[key] = e; delete init[key]; });
      return { files: files, init: init, counts: layers.map(function (l) { return l ? l.length : 0; }) };
    });
  }

  function materialize(FS, root, remote, cacheName, everything, label)
  {
    var wanted = [], total = 0, done = 0, filesDone = 0, missing = 0, began = Date.now(), reported = false;

    function megabytes(n) { return (n / 1048576).toFixed(1) + ' MB'; }
    function shown() { return label && typeof Shell_Progress === 'function'; }
    function report()
    {
      if (!shown() || done >= total || Date.now() - began < 200) return;
      reported = true;
      Shell_Progress(label + ' (' + filesDone + '/' + wanted.length + ' files, ' + megabytes(done) + ' of ' + megabytes(total) + ')', total > 0 ? done / total : null);
    }

    Object.keys(remote.init).forEach(function (key)
    {
      var entry = remote.init[key];

      if (!FS.analyzePath(root + '/' + entry.path).exists) wanted.push(entry);
    });
    if (everything) Object.keys(remote.files).forEach(function (key) { wanted.push(remote.files[key]); });
    wanted.forEach(function (entry) { total += entry.size || 0; });
    return Promise.all(wanted.map(function (entry)
    {
      return readFile(entry.url, cacheName).then(function (bytes)
      {
        var target = root + '/' + entry.path;

        FS.mkdirTree(target.substring(0, target.lastIndexOf('/')));
        FS.writeFile(target, bytes);
        filesDone++;
        done += entry.size || bytes.length;
        report();
      }, function (e)
      {
        if (!e || !e.missing) throw e;
        console.warn('[Shell] ' + entry.path + ' is missing on the host: left out (' + e.message + ')');
        missing++;
        filesDone++;
        done += entry.size || 0;
        report();
      });
    })).then(function ()
    {
      if (reported) Shell_Progress(null);
      return wanted.length - missing;
    }, function (e)
    {
      if (reported) Shell_Progress(null);
      throw e;
    });
  }

  return {
    stats: stats,
    net: net,
    netFetch: netFetch,
    linkLocale: linkLocale,
    joinParts: joinParts,
    CLIENT_CACHE: CLIENT_CACHE,
    loadRemote: loadRemote,
    materialize: materialize,

    prepareClient: function (dataUrl, options)
    {
      options = options || {};
      var base = underNode ? __dirname + '/' : new URL('.', location.href).href;
      var absolute = function (url) { return !isHostPath(url) && !underNode ? new URL(url, location.href).href : url; };
      var data = options.data ? Promise.resolve(options.data) : loadDataLayer(absolute(dataUrl));
      var mod = options.mod ? Promise.resolve(options.mod) : options.modUrl ? loadDataLayer(absolute(options.modUrl), 'mod') : Promise.resolve(null);

      return Promise.all([data, mod]).then(function (layers)
      {
        return loadRemote({ dataLayer: layers[0], modLayer: layers[1], patch: base + 'patch/', init: base + 'init/', locale: options.locale });
      }).then(function (remote)
      {
        return materialize(FS, config.root, remote, CLIENT_CACHE, false, "Preparing the game's files").then(function (written)
        {
          var lines = Object.keys(remote.files).sort().map(function (key) { var e = remote.files[key]; return e.path + '\t' + e.size + '\t' + e.url; });

          FS.writeFile('/Port/files.txt', lines.join('\n') + '\n');
          console.log('[Shell] Remote files: ' + lines.length + ' streamed (data ' + remote.counts[0] + ', mod ' + remote.counts[3] + ', patch ' + remote.counts[1] + '), init ' +
                      remote.counts[2] + ' (' + written + ' new)' + (options.locale ? ', locale ' + options.locale : ''));
        });
      }).catch(function (e) { console.error('[Shell] The remote file systems could not be prepared: ' + e); });
    },

    SERVER_CACHE: SERVER_CACHE,
    loadDataLayer: loadDataLayer,
    missingFiles: missingFiles,
    droppedDirectories: droppedDirectories,

    useLocalFiles: function (files) { localFiles = files; },

    bytes: function (url, cacheName)
    {
      if (url.indexOf('local:') === 0)
      {
        var file = localFiles[url];

        return (file ? file.arrayBuffer() : Promise.reject(new Error(url + ': not a file of the chosen folder'))).then(function (buffer)
        {
          stats.local += buffer.byteLength;
          return new Uint8Array(buffer);
        });
      }
      return versionOf(url).then(function (version)
      {
        var key = blockKey(url, 0, 'all', version.tag);

        return storage(cacheName).then(function (cache)
        {
          var lookup = cache ? cache.match(key).catch(function () { return null; }) : Promise.resolve(null);

          return lookup.then(function (hit)
          {
            if (hit) { stats.hits++; return hit.arrayBuffer(); }
            stats.misses++;
            return wholeFile(url).then(function (buffer)
            {
              stats.fetched += buffer.byteLength;
              if (!cache) return buffer;
              return cache.put(key, new Response(buffer)).then(function () { return buffer; }, function (e)
              {
                console.warn('[Shell] Not stored: ' + url + ' (' + e + ')');
                return buffer;
              });
            });
          });
        });
      }).then(function (buffer) { return new Uint8Array(buffer); });
    },

    read: function (request, heap, cacheName)
    {
      heap = heap || function () { return wasmMemory.buffer; };
      var words = new Int32Array(heap()), w = request >> 2;
      var bytes = new Uint8Array(heap()), at = words[w + 1], end = at;
      while (bytes[end]) end++;
      var url = new TextDecoder().decode(bytes.slice(at, end)), length = words[w + 2], dest = words[w + 3];
      var offset = new Float64Array(heap())[(request + 16) >> 3], block = words[w + 6];
      var count = Math.ceil(length / block), keys = [], k;

      if (url.indexOf('local:') === 0)
      {
        var file = localFiles[url];

        (file ? file.slice(offset, offset + length).arrayBuffer() : Promise.reject(new Error('not a file of the chosen folder'))).then(function (buffer)
        {
          if (buffer.byteLength !== length) throw new Error('short read');
          new Uint8Array(heap()).set(new Uint8Array(buffer), dest);
          stats.local += length;
          finish(request, 1, length, 0, heap);
        }).catch(function (e)
        {
          console.warn('[Shell] Reading ' + url + ' at ' + offset + ' failed: ' + e);
          finish(request, -1, 0, 0, heap);
        });
        return;
      }
      versionOf(url).then(function (version)
      {
        for (k = 0; k < count; k++)
        {
          keys.push(blockKey(url, offset + k * block, Math.min(block, length - k * block), version.tag));
        }
        return storage(cacheName || CLIENT_CACHE);
      }).then(function (cache)
      {
        return Promise.all(keys.map(function (key)
        {
          if (inflight[key]) return inflight[key];
          if (!cache) return null;
          return cache.match(key).then(function (response) { return response ? response.arrayBuffer() : null; }, function () { return null; });
        })).then(function (parts) { return { cache: cache, parts: parts }; });
      }).then(function (found)
      {
        var parts = found.parts, first = -1, last = -1;

        for (k = 0; k < count; k++)
        {
          var want = Math.min(block, length - k * block);

          if (parts[k] && parts[k].byteLength !== want) parts[k] = null;
          if (!parts[k]) { if (first < 0) first = k; last = k; }
        }
        stats.hits += count - (first < 0 ? 0 : last - first + 1);
        if (first < 0) return { cache: found.cache, parts: parts, fetched: null, first: 0 };
        var start = offset + first * block, end = Math.min(offset + length, offset + (last + 1) * block);

        stats.misses += last - first + 1;
        return fetchRange(url, start, end).then(function (buffer)
        {
          return { cache: found.cache, parts: parts, fetched: buffer, first: first, last: last };
        });
      }).then(function (got)
      {
        var memory = new Uint8Array(heap()), cached = 0;

        for (k = 0; k < count; k++)
        {
          var want = Math.min(block, length - k * block);

          if (got.parts[k])
          {
            memory.set(new Uint8Array(got.parts[k]), dest + k * block);
            cached += want;
          }
          else
          {
            var piece = got.fetched.slice((k - got.first) * block, (k - got.first) * block + want);

            memory.set(new Uint8Array(piece), dest + k * block);
            if (got.cache) got.cache.put(keys[k], new Response(piece)).catch(function () {});
          }
        }
        finish(request, 1, length, cached, heap);
      }).catch(function (e)
      {
        console.warn('[Shell] Reading ' + url + ' at ' + offset + ' failed: ' + e);
        finish(request, e && e.missing ? -2 : -1, 0, 0, heap);
      });
    },

    prefetch: function (url, offset, length, size, block)
    {
      var keys = [], k, count = Math.ceil(length / block);

      if (url.indexOf('local:') === 0) return;
      versionOf(url).then(function (version)
      {
        for (k = 0; k < count; k++) keys.push(blockKey(url, offset + k * block, Math.min(block, length - k * block), version.tag));
        if (keys.every(function (key) { return inflight[key]; })) return 'skip';
        return storage(CLIENT_CACHE);
      }).then(function (cache)
      {
        if (cache === 'skip') return;
        return Promise.all(keys.map(function (key)
        {
          if (inflight[key] || !cache) return !!inflight[key];
          return cache.match(key).then(function (response) { return !!response; }, function () { return false; });
        })).then(function (have)
        {
          var first = have.indexOf(false), last = have.lastIndexOf(false);

          if (first < 0) return;
          var start = offset + first * block, end = Math.min(offset + length, offset + (last + 1) * block);
          var fetched = fetchRange(url, start, end);

          for (k = first; k <= last; k++)
          {
            (function (index, key)
            {
              if (inflight[key]) return;
              var want = Math.min(block, length - index * block);
              inflight[key] = fetched.then(function (buffer)
              {
                var piece = buffer.slice((index - first) * block, (index - first) * block + want);

                stats.prefetched += piece.byteLength;
                if (cache) return cache.put(key, new Response(piece)).then(function () { return piece; }, function () { return piece; });
                return piece;
              }, function () { return null; });
              inflight[key].then(function () { delete inflight[key]; });
            })(k, keys[k]);
          }
        });
      }).catch(function () {});
    },

    disable: function ()
    {
      Module['srwDataCache'] = false;
      if (typeof caches !== 'undefined') [CLIENT_CACHE, SERVER_CACHE].concat(OLD_CACHES).forEach(function (name) { caches.delete(name).catch(function () {}); });
    }
  };
})();

if (typeof document !== 'undefined')
{
  document.addEventListener('paste', function (event)
  {
    if (!document.body.classList.contains('page-game')) return;
    function isField(e) { return !!(e && e.nodeType === 1 && (e.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(e.tagName))); }
    if (isField(event.target) || isField(document.activeElement)) return;
    var text = (event.clipboardData || window.clipboardData).getData('text');

    Module.srwPasteText = text;
    try { Module.ccall('SRW_ClipboardPasted', null, [], []); } catch (e) { return; }
    event.preventDefault();
  });
}

Module['srwNet'] = (function ()
{
  var connections = {};

  function memory(pointer, length)
  {
    return new Uint8Array(wasmMemory.buffer, pointer, length);
  }

  return {
    open: function (id, pointer, length)
    {
      var url = new TextDecoder().decode(new Uint8Array(memory(pointer, length)));
      var connection = { socket: null, chunks: [], offset: 0, state: 0, hub: null };

      connections[id] = connection;
      if (url.indexOf('acore:') === 0)
      {
        var port = /[?&]port=(\d+)/.exec(url);

        connection.hub = (typeof acoreHub !== 'undefined' && port) ? acoreHub.connect(parseInt(port[1], 10)) : null;
        connection.state = connection.hub ? 0 : 3;
        return 0;
      }
      try
      {
        connection.socket = new WebSocket(url);
        connection.socket.binaryType = 'arraybuffer';
        connection.socket.onopen = function () { if (connection.state === 0) connection.state = 1; };
        connection.socket.onmessage = function (event) { connection.chunks.push(new Uint8Array(event.data)); };
        connection.socket.onerror = function () { console.warn('[Shell] WebSocket error for ' + url + ' (state ' + connection.state + ')'); if (connection.state !== 2) connection.state = 3; };
        connection.socket.onclose = function (event) { console.log('[Shell] WebSocket closed, code ' + event.code + (event.reason ? ' (' + event.reason + ')' : '') + ', was ' + (connection.state === 0 ? 'not yet open' : 'open') + ', ' + url); if (connection.state === 1) connection.state = 2; else if (connection.state === 0) connection.state = 3; };
      }
      catch (e)
      {
        console.error('[Shell] The connection to ' + url + ' could not be opened: ' + e);
        connection.state = 3;
      }
      return 0;
    },

    poll: function (id, pointer, maximum)
    {
      var connection = connections[id], count = 0;

      if (!connection) return 3 << 28;
      if (connection.hub)
      {
        var hub = connection.hub;

        while (count < maximum && hub.toClient.length > 0)
        {
          var piece = hub.toClient[0], m = Math.min(maximum - count, piece.length - hub.toClientOffset);

          memory(pointer + count, m).set(piece.subarray(hub.toClientOffset, hub.toClientOffset + m));
          count += m;
          hub.toClientOffset += m;
          if (hub.toClientOffset >= piece.length)
          {
            hub.toClient.shift();
            hub.toClientOffset = 0;
          }
        }
        var hubState = !hub.attached ? 0 : (hub.serverClosed && hub.toClient.length === 0 ? 2 : 1);

        return count | (hubState << 28);
      }
      while (count < maximum && connection.chunks.length > 0)
      {
        var chunk = connection.chunks[0], n = Math.min(maximum - count, chunk.length - connection.offset);

        memory(pointer + count, n).set(chunk.subarray(connection.offset, connection.offset + n));
        count += n;
        connection.offset += n;
        if (connection.offset >= chunk.length)
        {
          connection.chunks.shift();
          connection.offset = 0;
        }
      }
      var state = connection.state;
      if ((state === 2 || state === 3) && connection.chunks.length > 0) state = 1;
      return count | (state << 28);
    },

    send: function (id, pointer, length)
    {
      var connection = connections[id];

      if (connection && connection.hub) return acoreHub.clientSend(connection.hub, new Uint8Array(memory(pointer, length)));
      if (!connection || connection.state !== 1) return -1;
      connection.socket.send(new Uint8Array(memory(pointer, length)));
      return length;
    },

    close: function (id)
    {
      var connection = connections[id];

      if (connection && connection.hub) acoreHub.clientClose(connection.hub);
      if (connection && connection.socket)
      {
        connection.state = 2;
        try { connection.socket.close(); } catch (e) { }
      }
      delete connections[id];
      return 0;
    }
  };
})();


Module['srwCursor'] = (function ()
{
  var image = null, current = null;
  var SYSTEM = {
    32512: 'default', 32513: 'text', 32514: 'wait', 32515: 'crosshair', 32516: 'n-resize', 32642: 'nwse-resize', 32643: 'nesw-resize',
    32644: 'ew-resize', 32645: 'ns-resize', 32646: 'move', 32648: 'not-allowed', 32649: 'pointer', 32650: 'progress', 32651: 'help'
  };

  function canvas()
  {
    return Module['canvas'] || document.getElementById('canvas');
  }

  if (typeof document === 'undefined') return { setImage: function () { return 0; }, apply: function () { return 0; }, lock: function () { return 0; } };

  return {
    setImage: function (pointer, width, height, hotX, hotY)
    {
      var picture = document.createElement('canvas'), pixels = new Uint8ClampedArray(width * height * 4);

      pixels.set(new Uint8Array(wasmMemory.buffer, pointer, width * height * 4));
      picture.width = width;
      picture.height = height;
      picture.getContext('2d').putImageData(new ImageData(pixels, width, height), 0, 0);
      image = 'url(' + picture.toDataURL('image/png') + ') ' + hotX + ' ' + hotY + ', auto';
      current = null;
      return 0;
    },

    apply: function (kind, systemId)
    {
      var value = (kind === 1 && image) ? image : (kind === 2) ? 'none' : (kind === 3) ? (SYSTEM[systemId] || 'default') : 'default';
      var element = canvas();

      if (element && value !== current)
      {
        current = value;
        element.style.cursor = value;
      }
      return 0;
    },

    lock: function (on)
    {
      var element = canvas();

      try
      {
        if (on)
        {
          if (element && document.pointerLockElement !== element && element.requestPointerLock) element.requestPointerLock();
        }
        else if (document.pointerLockElement)
        {
          document.exitPointerLock();
        }
      }
      catch (e)
      {
      }
      return 0;
    }
  };
})();

if (typeof window !== 'undefined')
{
  window.addEventListener('contextmenu', function (event) {
    var element = Module['canvas'] || document.getElementById('canvas');

    if (element && (event.target === element || document.pointerLockElement === element)) event.preventDefault();
  });
}

Module['srwKeyboard'] = (function ()
{
  var none = { isPhone: false, typing: false, owns: function () { return false; }, textEntry: function () {}, toggle: function () { return false; } };

  if (typeof document === 'undefined' || typeof window === 'undefined') return none;

  function phoneDevice()
  {
    try
    {
      if (navigator.userAgentData && typeof navigator.userAgentData.mobile === 'boolean') return navigator.userAgentData.mobile;
    }
    catch (e)
    {
    }
    return /Android|webOS|iPhone|iPad|iPod|BlackBerry|Windows Phone/i.test(navigator.userAgent);
  }

  var phone = phoneDevice(), field = null, active = false, rect = null, shift = 0, backspaceSent = false, composing = false;
  var resizeTimer = null, reportedWidth = window.innerWidth, reportedHeight = window.innerHeight;
  var KEYS = { Enter: 13, Escape: 27, Tab: 9, Backspace: 8 };

  function canvas()
  {
    return Module['canvas'] || document.getElementById('canvas');
  }

  function focused()
  {
    return field !== null && document.activeElement === field;
  }

  function call(name, types, values)
  {
    if (!Module.calledRun) return;
    try
    {
      Module.ccall(name, null, types, values);
    }
    catch (e)
    {
    }
  }

  function pressKey(code)
  {
    call('SRW_InjectKey', ['number', 'number'], [code, code]);
  }

  function deliver(text)
  {
    if (!text) return;
    if (active)
    {
      call('SRW_InjectText', ['string'], [text]);
      return;
    }
    Array.from(text).forEach(function (character)
    {
      var c = character.charCodeAt(0), code = 0;

      if (character.length === 1)
      {
        if (c >= 48 && c <= 57) code = c;
        else if (c >= 97 && c <= 122) code = c - 32;
        else if (c >= 65 && c <= 90) code = c;
        else if (c === 32) code = 32;
      }
      if (code) call('SRW_InjectKey', ['number', 'number'], [code, c]);
      else call('SRW_InjectText', ['string'], [character]);
    });
  }

  function clear()
  {
    field.value = '';
  }

  function createField()
  {
    field = document.createElement('input');
    field.type = 'text';
    field.autocomplete = 'off';
    field.autocapitalize = 'off';
    field.spellcheck = false;
    field.tabIndex = -1;
    field.setAttribute('aria-hidden', 'true');
    field.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;border:0;padding:0;margin:0;font-size:16px;pointer-events:none;';
    document.body.appendChild(field);

    field.addEventListener('keydown', function (event)
    {
      event.stopImmediatePropagation();
      if (!KEYS.hasOwnProperty(event.key)) return;
      event.preventDefault();
      if (event.key === 'Backspace')
      {
        backspaceSent = true;
        setTimeout(function () { backspaceSent = false; }, 0);
      }
      pressKey(KEYS[event.key]);
    });
    field.addEventListener('keyup', function (event) { event.stopImmediatePropagation(); });
    field.addEventListener('keypress', function (event) { event.stopImmediatePropagation(); });
    field.addEventListener('compositionstart', function () { composing = true; });
    field.addEventListener('compositionend', function (event)
    {
      composing = false;
      deliver(event.data || field.value);
      clear();
    });
    field.addEventListener('input', function (event)
    {
      event.stopImmediatePropagation();
      if (composing || event.isComposing || event.inputType === 'insertCompositionText') return;
      if (event.inputType === 'deleteContentBackward' || event.inputType === 'deleteContentForward')
      {
        if (!backspaceSent) pressKey(8);
      }
      else if (event.inputType === 'insertLineBreak' || event.inputType === 'insertParagraph') pressKey(13);
      else deliver(field.value);
      clear();
    });
    field.addEventListener('blur', function ()
    {
      shift = 0;
      place();
      scheduleResize();
    });
  }

  function focusField()
  {
    if (!phone || document.activeElement === field) return;
    if (field === null) createField();
    clear();
    field.focus({ preventScroll: true });
    place();
  }

  function blurField()
  {
    if (!focused()) return;
    field.blur();
    var element = canvas();

    if (element && element.focus) element.focus({ preventScroll: true });
  }

  function place()
  {
    var element = canvas(), view = window.visualViewport, area, bottom, top, limit, wanted = 0;

    if (!element) return;
    if (phone && active && rect && view && focused() && (area = Module['srwCanvasArea']()))
    {
      bottom = area.top + shift + rect.bottom * area.scale;
      top = area.top + shift + rect.top * area.scale;
      limit = view.offsetTop + view.height - 8;
      if (bottom > limit) wanted = Math.min(bottom - limit, Math.max(0, top - view.offsetTop));
    }
    if (wanted === shift) return;
    shift = wanted;
    element.style.transform = shift > 0 ? 'translateY(' + (-shift) + 'px)' : '';
  }

  function scheduleResize()
  {
    if (resizeTimer !== null) clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function ()
    {
      resizeTimer = null;
      if (focused() || !Module.calledRun) return;
      if (window.innerWidth === reportedWidth && window.innerHeight === reportedHeight) return;
      reportedWidth = window.innerWidth;
      reportedHeight = window.innerHeight;
      call('SRW_CanvasResized', ['number', 'number'], [reportedWidth, reportedHeight]);
    }, 250);
  }

  window.addEventListener('resize', scheduleResize);
  if (window.visualViewport)
  {
    window.visualViewport.addEventListener('resize', place);
    window.visualViewport.addEventListener('scroll', place);
  }

  if (phone)
  {
    (function ()
    {
      var element = canvas(), last = null, fingers = 0;

      if (!element) return;
      element.addEventListener('touchstart', function (event) { fingers = Math.max(fingers, event.touches.length); }, { passive: true });
      element.addEventListener('touchend', function (event)
      {
        var touch = event.changedTouches[0], now = Date.now(), single = fingers === 1;

        if (event.touches.length === 0) fingers = 0;
        if (active && !focused()) focusField();
        if (!single || event.touches.length !== 0 || !touch) return;
        if (last && now - last.time < 300 && Math.abs(touch.clientX - last.x) < 40 && Math.abs(touch.clientY - last.y) < 40)
        {
          last = null;
          setTimeout(function ()
          {
            call('SRW_InjectRightButton', ['number'], [1]);
            setTimeout(function () { call('SRW_InjectRightButton', ['number'], [0]); }, 40);
          }, 50);
        }
        else last = { time: now, x: touch.clientX, y: touch.clientY };
      }, { passive: true });
    })();
  }

  return {
    isPhone: phone,
    get typing() { return focused(); },
    owns: focused,

    textEntry: function (on, hasRect, left, top, right, bottom)
    {
      active = on !== 0;
      rect = active && hasRect !== 0 ? { left: left, top: top, right: right, bottom: bottom } : null;
      if (!phone) return;
      if (active) focusField();
      else blurField();
      place();
    },

    toggle: function ()
    {
      if (!phone) return false;
      if (focused()) blurField();
      else focusField();
      return focused();
    }
  };
})();


Module['srwAudio'] = (function ()
{
  var context = null;
  var missing = 0, missingReported = 0, lastReport = 0;

  var workletSource =
    "class SrwRing extends AudioWorkletProcessor {\n" +
    "  constructor() {\n" +
    "    super();\n" +
    "    this.ready = false; this.missing = 0; this.played = 0;\n" +
    "    this.port.onmessage = (event) => {\n" +
    "      const d = event.data;\n" +
    "      this.counters = new Int32Array(d.buffer);\n" +
    "      this.samples = new Float32Array(d.buffer, d.ring, d.frames * 2);\n" +
    "      this.read = d.read >> 2; this.write = d.write >> 2; this.mask = d.frames - 1;\n" +
    "      this.ready = true;\n" +
    "    };\n" +
    "  }\n" +
    "  process(inputs, outputs) {\n" +
    "    const out = outputs[0], left = out[0], right = out.length > 1 ? out[1] : out[0];\n" +
    "    if (!this.ready) return true;\n" +
    "    const read = Atomics.load(this.counters, this.read), write = Atomics.load(this.counters, this.write);\n" +
    "    const available = (write - read) | 0, n = Math.min(left.length, available > 0 ? available : 0);\n" +
    "    let i = 0;\n" +
    "    for (; i < n; i++) {\n" +
    "      const index = ((read + i) & this.mask) * 2;\n" +
    "      left[i] = this.samples[index];\n" +
    "      right[i] = this.samples[index + 1];\n" +
    "    }\n" +
    "    for (; i < left.length; i++) { left[i] = 0; right[i] = 0; }\n" +
    "    Atomics.store(this.counters, this.read, (read + n) | 0);\n" +
    "    if (this.played > 0) this.missing += left.length - n;\n" +
    "    this.played += n;\n" +
    "    if ((++this.calls & 1023) === 0) this.port.postMessage(this.missing);\n" +
    "    return true;\n" +
    "  }\n" +
    "}\n" +
    "SrwRing.prototype.calls = 0;\n" +
    "registerProcessor('srw-ring', SrwRing);\n";

  function noteMissing(total)
  {
    var now = performance.now();

    missing = total;
    if (now - lastReport >= 10000 && missing > missingReported)
    {
      console.log('[Shell] Sound output: ' + (missing - missingReported) + ' frames were not ready in time in the last ' + Math.round((now - lastReport) / 1000) + ' s (the mixer was late)');
      missingReported = missing;
      lastReport = now;
    }
    else if (lastReport === 0) lastReport = now;
  }

  function scriptProcessor(ringPointer, readPointer, writePointer, frames)
  {
    var node = context.createScriptProcessor(2048, 0, 2), played = 0, lost = 0;

    node.onaudioprocess = function (event)
    {
      var left = event.outputBuffer.getChannelData(0), right = event.outputBuffer.getChannelData(1);
      var counters = new Int32Array(wasmMemory.buffer), samples = new Float32Array(wasmMemory.buffer, ringPointer, frames * 2);
      var read = Atomics.load(counters, readPointer >> 2), write = Atomics.load(counters, writePointer >> 2);
      var available = (write - read) | 0, n = Math.min(left.length, available > 0 ? available : 0), i, index;

      for (i = 0; i < n; i++)
      {
        index = ((read + i) & (frames - 1)) * 2;
        left[i] = samples[index];
        right[i] = samples[index + 1];
      }
      for (; i < left.length; i++)
      {
        left[i] = 0;
        right[i] = 0;
      }
      Atomics.store(counters, readPointer >> 2, (read + n) | 0);
      if (played > 0) lost += left.length - n;
      played += n;
      noteMissing(lost);
    };
    node.connect(context.destination);
  }

  return {
    start: function (ringPointer, readPointer, writePointer, frames)
    {
      var AudioContextClass = (typeof window !== 'undefined') ? (window.AudioContext || window.webkitAudioContext) : null;
      var resume;

      if (!AudioContextClass) return 0;
      try
      {
        context = new AudioContextClass({ latencyHint: 'interactive' });
        if (context.audioWorklet && typeof AudioWorkletNode !== 'undefined' && typeof SharedArrayBuffer !== 'undefined' && wasmMemory.buffer instanceof SharedArrayBuffer)
        {
          var url = URL.createObjectURL(new Blob([workletSource], { type: 'application/javascript' }));

          context.audioWorklet.addModule(url).then(function ()
          {
            var node = new AudioWorkletNode(context, 'srw-ring', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });

            node.port.onmessage = function (event) { noteMissing(event.data); };
            node.port.postMessage({ buffer: wasmMemory.buffer, ring: ringPointer, read: readPointer, write: writePointer, frames: frames });
            node.connect(context.destination);
            console.log('[Shell] Sound output: AudioWorklet at ' + context.sampleRate + ' Hz');
          }).catch(function (e)
          {
            console.warn('[Shell] Sound output: no AudioWorklet (' + e + '), a script processor instead');
            scriptProcessor(ringPointer, readPointer, writePointer, frames);
          });
        }
        else
        {
          console.log('[Shell] Sound output: script processor (no AudioWorklet or no shared memory)');
          scriptProcessor(ringPointer, readPointer, writePointer, frames);
        }
        resume = function ()
        {
          if (context.state !== 'running') context.resume();
        };
        ['pointerdown', 'keydown', 'touchstart'].forEach(function (name) { window.addEventListener(name, resume, true); });
        resume();
        return context.sampleRate;
      }
      catch (e)
      {
        console.error('[Shell] No sound: ' + e);
        return 0;
      }
    }
  };
})();
