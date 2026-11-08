// Test harness for the web client, to be injected into the page (the page of a diagnostics build, run with debugLog=perf):
//
//   evaluate this file in the page, then:  WT.drive({...})   starts the game, logs in, enters the world and measures; it runs on its own in the page,
//                                          so a tool call that times out does not stop it
//                                          WT.status()       where it is, and the latest numbers
//                                          WT.report()       the result: medians of the perf rows after the world was entered, the buffer update line
//                                          WT.grep(/re/, n)  the last n log lines that match
//
// The page's log (console.log and the like, which is where the program's printf goes) is kept in WT.lines. Keys are given to the game with its own
// exported function (no mouse, no focus needed), and the foreground flag is set, so the game does not slow itself to 30 fps as in a background tab.
(function () {
  'use strict';
  if (window.WT) return;
  const WT = window.WT = { lines: [], cursor: 0, state: 'idle', result: null, t0: performance.now() };

  for (const kind of ['log', 'info', 'warn', 'error']) {
    const original = console[kind];
    console[kind] = function () {
      try {
        const text = Array.prototype.map.call(arguments, x => typeof x === 'string' ? x : (x && x.message) || String(x)).join(' ');
        if (WT.lines.length < 200000) WT.lines.push(text);
      } catch (e) {}
      return original.apply(console, arguments);
    };
  }

  const sleep = ms => new Promise(r => setTimeout(r, ms));
  WT.sleep = sleep;
  WT.grep = (re, n) => WT.lines.filter(l => re.test(l)).slice(-(n || 5));

  // wait for a line that matches, looking at the lines from the cursor on; resolves with the line, or null at the timeout
  WT.waitFor = async function (re, timeoutMs) {
    const end = performance.now() + timeoutMs;
    while (performance.now() < end) {
      for (let i = WT.cursor; i < WT.lines.length; i++) {
        if (re.test(WT.lines[i])) { WT.cursor = i + 1; return WT.lines[i]; }
      }
      WT.cursor = Math.max(WT.cursor, WT.lines.length > 100000 ? WT.lines.length - 100000 : 0);
      await sleep(250);
    }
    return null;
  };

  const call = (name, types, args) => Module.ccall(name, null, types, args);
  WT.key = (vk, ch) => call('SRW_InjectKey', ['number', 'number'], [vk, ch === undefined ? vk : ch]);
  WT.active = on => call('SRW_SetActive', ['number'], [on ? 1 : 0]);
  WT.start = () => document.getElementById('btn-start').click();

  const PERF = /\[D3D9\] perf: ([\d.]+) fps, per frame: ([\d.]+) ms total = draws ([\d.]+) ms \((\d+) calls, [\d.]+ us each\) \+ present ([\d.]+) ms \(commit ([\d.]+) ms, read-back ([\d.]+) ms\) \+ game and everything else ([\d.]+) ms/;
  const UPD = /\[D3D9\] buffer updates: ([\d.]+) per frame, ([\d.]+) of them after a draw of this frame read the buffer \(([\d.]+)%\); VB ([\d.]+)\/([\d.]+), IB ([\d.]+)\/([\d.]+); ([\d.]+) KB per frame, average ([\d.]+) KB, largest ([\d.]+) KB/;
  const rowsAfter = (from) => {
    const perf = [], upd = [];
    for (let i = from; i < WT.lines.length; i++) {
      let m = PERF.exec(WT.lines[i]);
      if (m) perf.push({ fps: +m[1], frame: +m[2], draws: +m[3], drawCalls: +m[4], present: +m[5], commit: +m[6], readback: +m[7], other: +m[8] });
      m = UPD.exec(WT.lines[i]);
      if (m) {
        const row = { updates: +m[1], hazards: +m[2], hazardPct: +m[3], vb: +m[4], vbHazards: +m[5], ib: +m[6], ibHazards: +m[7], kbPerFrame: +m[8], avgKB: +m[9], maxKB: +m[10] };
        const st = /streamed: ([\d.]+) uploads, ([\d.]+) KB per frame, (\d+) buffers/.exec(WT.lines[i]);
        if (st) { row.streamUploads = +st[1]; row.streamKB = +st[2]; row.streamBuffers = +st[3]; }
        upd.push(row);
      }
    }
    return { perf, upd };
  };
  const median = a => { if (!a.length) return null; const s = a.slice().sort((x, y) => x - y); return +s[s.length >> 1].toFixed(2); };
  WT.summarize = function (from) {
    const { perf, upd } = rowsAfter(from || 0), field = (rows, k) => median(rows.map(r => r[k]));
    const o = { windows: perf.length };
    for (const k of ['fps', 'frame', 'draws', 'present', 'other', 'drawCalls']) o[k] = field(perf, k);
    o.bufferUpdates = upd.length ? Object.fromEntries(Object.keys(upd[0]).map(k => [k, field(upd, k)])) : null;
    return o;
  };

  WT.status = function () {
    const { perf } = rowsAfter(Math.max(0, WT.lines.length - 400));
    return { state: WT.state, seconds: Math.round((performance.now() - WT.t0) / 1000), lines: WT.lines.length, lastPerf: perf[perf.length - 1] || null, errors: WT.grep(/cannot be used|failed|Error:|exception/i, 3) };
  };
  WT.report = () => ({ state: WT.state, result: WT.result, lastLines: { perf: WT.grep(/\[D3D9\] perf:/, 2), updates: WT.grep(/\[D3D9\] buffer updates:/, 1), gfx: WT.grep(/\[D3D9\] gfx:/, 1) } });

  // The scripted run: start, log in (Enter), enter the world (Enter), settle, then measure. cfg: settleMs, collectMs.
  WT.drive = async function (cfg) {
    cfg = Object.assign({ settleMs: 12000, collectMs: 25000, startTimeoutMs: 120000 }, cfg || {});
    WT.result = null; WT.cursor = WT.lines.length;
    try {
      WT.state = 'starting'; WT.active(true);
      WT.start();
      if (!await WT.waitFor(/\[D3D9\] perf:/, cfg.startTimeoutMs)) throw new Error('the renderer did not start (' + JSON.stringify(WT.grep(/cannot be used|failed|error/i, 3)) + ')');
      await sleep(3000);
      WT.state = 'logging in';
      let characters = null;
      for (let attempt = 0; attempt < 4 && !characters; attempt++) {
        WT.key(13); characters = await WT.waitFor(/COP_GET_CHARACTERS code=44 result=TRUE/, 15000);
      }
      if (!characters) throw new Error('no character list after Enter');
      await sleep(2000);
      WT.state = 'entering the world';
      let entered = null;
      for (let attempt = 0; attempt < 4 && !entered; attempt++) {
        WT.key(13); entered = await WT.waitFor(/COP_LOGIN_CHARACTER code=77 result=TRUE/, 20000);
      }
      if (!entered) throw new Error('did not enter the world after Enter');
      WT.state = 'settling'; WT.active(true);
      await sleep(cfg.settleMs);
      const from = WT.lines.length;
      WT.state = 'measuring';
      await sleep(cfg.collectMs);
      WT.result = WT.summarize(from);
      WT.state = 'done';
    } catch (e) {
      WT.state = 'failed: ' + e.message;
    }
  };
  WT.reset = () => { WT.state = 'idle'; WT.result = null; WT.cursor = WT.lines.length; };
})();
