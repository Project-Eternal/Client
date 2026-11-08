#!/usr/bin/env node
// A read-only static file server for the game's files, for the page's "Client files location" (usage: node serve-gamefiles.js ROOT PORT).
// ROOT holds Data/ and the executable (links are followed) and the manifest.txt that tools/make_data_manifest.py writes. Range requests, CORS and
// Cross-Origin-Resource-Policy are answered, as the page (cross-origin isolated) needs; it listens on 127.0.0.1 only.
'use strict';
const http = require('http'), fs = require('fs'), path = require('path');
const root = fs.realpathSync(process.argv[2]), port = +process.argv[3];
http.createServer((req, res) => {
  const h = { 'Access-Control-Allow-Origin': '*', 'Cross-Origin-Resource-Policy': 'cross-origin', 'Access-Control-Expose-Headers': 'Content-Length,Content-Range,Accept-Ranges', 'Accept-Ranges': 'bytes' };
  if (req.method === 'OPTIONS') { res.writeHead(204, { ...h, 'Access-Control-Allow-Headers': 'Range,Cache-Control', 'Access-Control-Allow-Methods': 'GET,HEAD' }); return res.end(); }
  const p = path.join(root, decodeURIComponent(req.url.split('?')[0]));
  if (!p.startsWith(root)) { res.writeHead(403); return res.end(); }
  fs.stat(p, (e, st) => {
    if (e || !st.isFile()) { res.writeHead(404, h); return res.end(); }
    const m = /bytes=(\d*)-(\d*)/.exec(req.headers.range || '');
    let s = 0, t = st.size - 1, code = 200;
    if (m) { s = m[1] ? +m[1] : st.size - +m[2]; t = m[1] && m[2] ? Math.min(+m[2], st.size - 1) : st.size - 1; code = 206; h['Content-Range'] = `bytes ${s}-${t}/${st.size}`; }
    h['Content-Length'] = t - s + 1; h['Content-Type'] = 'application/octet-stream';
    res.writeHead(code, h);
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(p, { start: s, end: t }).pipe(res);
  });
}).listen(port, '127.0.0.1', () => console.log('serving', root, 'on', port));
