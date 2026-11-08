// WebGL state queries answered from tracked state.
//
// A WebGL state query (gl.getParameter, gl.isEnabled) is a round trip to the browser's GPU process in browsers whose WebGL runs in another process
// (Chrome, Firefox): the thread waits until the GPU has decoded everything queued before the query, that is until the draws of the frame are done.
// Emscripten's present step (GL.blitOffscreenFramebuffer, with -sOFFSCREEN_FRAMEBUFFER) saves and restores state with such queries on every frame,
// and so does the Direct3D layer when it hands the frame to the page: 15-35 ms per frame, with the program waiting and not working.
//
// This file (linked with --post-js) wraps GL.registerContext: every context gets wrappers that note the state the queries ask for (the active
// texture unit, the bound program, buffers, vertex array, 2D textures and framebuffers, the enabled capabilities, the color mask and clear color),
// and getParameter / isEnabled answer those from the notes. All other queries go to WebGL as before.
//
// The notes are made by the same wrappers that pass the calls on, so they follow what the program does; they start from a real read of the state
// when the context is made (a single wait, at the start).
(function () {
  'use strict';
  if (typeof GL === 'undefined' || !GL.registerContext || GL.srwStateTracked) return;
  GL.srwStateTracked = true;

  var CAPS = { 3042: 1, 2884: 1, 2929: 1, 2960: 1, 3089: 1 };   // BLEND, CULL_FACE, DEPTH_TEST, STENCIL_TEST, SCISSOR_TEST
  var registerContext = GL.registerContext;

  function track(g) {
    if (g.srwTracked) return;
    g.srwTracked = true;
    var query = g.getParameter, queryEnabled = g.isEnabled;
    var s = {
      unit: query.call(g, 34016), program: query.call(g, 35725), arrayBuffer: query.call(g, 34964),
      vertexArray: g.bindVertexArray ? query.call(g, 34229) : null,
      drawFramebuffer: query.call(g, 36006), readFramebuffer: query.call(g, 36010),
      textures: {}, caps: {}, colorMask: query.call(g, 3107), clearColor: query.call(g, 3106)
    };
    s.textures[s.unit] = query.call(g, 32873);
    for (var cap in CAPS) s.caps[cap] = !!query.call(g, +cap);

    function wrap(name, make) {
      var original = g[name];
      if (typeof original !== 'function') return;
      g[name] = make(original);
    }
    wrap('enable', function (f) { return function (c) { if (c in CAPS) s.caps[c] = true; return f.call(g, c); }; });
    wrap('disable', function (f) { return function (c) { if (c in CAPS) s.caps[c] = false; return f.call(g, c); }; });
    wrap('activeTexture', function (f) { return function (u) { s.unit = u; return f.call(g, u); }; });
    wrap('bindTexture', function (f) { return function (t, x) { if (t === 3553) s.textures[s.unit] = x; return f.call(g, t, x); }; });
    wrap('deleteTexture', function (f) { return function (x) { for (var u in s.textures) if (s.textures[u] === x) s.textures[u] = null; return f.call(g, x); }; });
    wrap('useProgram', function (f) { return function (p) { s.program = p; return f.call(g, p); }; });
    wrap('bindBuffer', function (f) { return function (t, b) { if (t === 34962) s.arrayBuffer = b; return f.call(g, t, b); }; });
    wrap('deleteBuffer', function (f) { return function (b) { if (s.arrayBuffer === b) s.arrayBuffer = null; return f.call(g, b); }; });
    wrap('bindVertexArray', function (f) { return function (v) { s.vertexArray = v; return f.call(g, v); }; });
    wrap('deleteVertexArray', function (f) { return function (v) { if (s.vertexArray === v) s.vertexArray = null; return f.call(g, v); }; });
    wrap('bindFramebuffer', function (f) {
      return function (t, b) {
        if (t === 36160 || t === 36009) s.drawFramebuffer = b;
        if (t === 36160 || t === 36008) s.readFramebuffer = b;
        return f.call(g, t, b);
      };
    });
    wrap('deleteFramebuffer', function (f) {
      return function (b) {
        if (s.drawFramebuffer === b) s.drawFramebuffer = null;
        if (s.readFramebuffer === b) s.readFramebuffer = null;
        return f.call(g, b);
      };
    });
    wrap('colorMask', function (f) { return function (r, gr, b, a) { s.colorMask = [!!r, !!gr, !!b, !!a]; return f.call(g, r, gr, b, a); }; });
    wrap('clearColor', function (f) { return function (r, gr, b, a) { s.clearColor = new Float32Array([r, gr, b, a]); return f.call(g, r, gr, b, a); }; });

    g.getParameter = function (p) {
      switch (p) {
        case 34016: return s.unit;
        case 35725: return s.program;
        case 34964: return s.arrayBuffer;
        case 34229: if (g.bindVertexArray) return s.vertexArray; break;
        case 32873: return s.textures[s.unit] || null;
        case 36006: return s.drawFramebuffer;
        case 36010: return s.readFramebuffer;
        case 3107: return s.colorMask.slice();
        case 3106: return new Float32Array(s.clearColor);
        default: if (p in CAPS) return s.caps[p];
      }
      return query.apply(g, arguments);
    };
    g.isEnabled = function (c) { return c in CAPS ? s.caps[c] : queryEnabled.call(g, c); };
  }

  GL.registerContext = function (ctx, attributes) {
    var handle = registerContext.call(GL, ctx, attributes);
    try { track(ctx); } catch (e) { console.warn('[GLState] state queries are not tracked: ' + e); }
    return handle;
  };
})();
