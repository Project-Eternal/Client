#!/usr/bin/env node
// WebSocket to TCP proxy for the WoW client in the browser (no dependencies, Node 14 or newer).
//
//   node wow-proxy.js [--port 8090] [--host 127.0.0.1] [--allow-ports 3724,8085,8086] [--allow-hosts a.example,b.example]
//
// The page opens   ws://HOST:PORT/?host=login.twinstar-wow.com&port=3724   and the bytes of the WebSocket (binary messages)
// go to that TCP connection and back. TLS is not done here: put it behind a web server that speaks wss (for Apache:
// mod_proxy_wstunnel, ProxyPass "/wow/proxy" "ws://127.0.0.1:8090/").
//
// (--allow-private lets it reach the local network, for tests only; --map FAKE=REAL,... connects a requested host to another address,
// e.g. --map 10.99.0.1=127.0.0.1 for a server on this machine whose realm list gives an address the client does not treat as loopback.)
// Only the listed ports (and hosts, when a list is given) are reachable, and never addresses of the local network.
'use strict';
const http = require('http');
const net = require('net');
const dns = require('dns');
const crypto = require('crypto');

const options = { port: 8090, host: '127.0.0.1', ports: [3724, 8085, 8086, 8087, 8088], hosts: null, allowPrivate: false, map: {} };
for (let i = 2; i < process.argv.length; i += 2)
{
  if (process.argv[i] === '--allow-private') { options.allowPrivate = true; i--; continue; }
  const name = process.argv[i], value = process.argv[i + 1];
  if (name === '--port') options.port = parseInt(value, 10);
  else if (name === '--host') options.host = value;
  else if (name === '--allow-ports') options.ports = value.split(',').map(Number);
  else if (name === '--map') for (const pair of value.split(',')) options.map[pair.split('=')[0].toLowerCase()] = pair.split('=')[1];
  else if (name === '--allow-hosts') options.hosts = value.toLowerCase().split(',');
  else { console.error('unknown option ' + name); process.exit(1); }
}

function isPrivate(address)
{
  if (net.isIPv6(address)) return address === '::1' || /^f[cd]/i.test(address) || /^fe80/i.test(address);
  const p = address.split('.').map(Number);
  return p[0] === 10 || p[0] === 127 || p[0] === 0 || (p[0] === 172 && p[1] >= 16 && p[1] <= 31) || (p[0] === 192 && p[1] === 168) ||
    (p[0] === 169 && p[1] === 254) || (p[0] === 100 && p[1] >= 64 && p[1] <= 127);
}

function frame(opcode, payload)
{
  let header;
  if (payload.length < 126) header = Buffer.from([0x80 | opcode, payload.length]);
  else if (payload.length < 65536) { header = Buffer.alloc(4); header[0] = 0x80 | opcode; header[1] = 126; header.writeUInt16BE(payload.length, 2); }
  else { header = Buffer.alloc(10); header[0] = 0x80 | opcode; header[1] = 127; header.writeBigUInt64BE(BigInt(payload.length), 2); }
  return Buffer.concat([header, payload]);
}

const server = http.createServer((request, response) => {
  response.writeHead(426, { 'Content-Type': 'text/plain' });
  response.end('WebSocket only\n');
});

server.on('upgrade', (request, socket) => {
  const url = new URL(request.url, 'http://localhost');
  const host = url.searchParams.get('host') || '', port = parseInt(url.searchParams.get('port') || '', 10);
  const key = request.headers['sec-websocket-key'];
  const log = (text) => console.log(new Date().toISOString() + ' ' + socket.remoteAddress + ' -> ' + host + ':' + port + ' ' + text);

  if (!key || !/^[A-Za-z0-9.-]+$/.test(host) || !options.ports.includes(port) || (options.hosts && !options.hosts.includes(host.toLowerCase())))
  {
    log('refused');
    socket.end('HTTP/1.1 403 Forbidden\r\n\r\n');
    return;
  }
  dns.lookup(options.map[host.toLowerCase()] || host, (error, address) => {
    if (error || (!options.allowPrivate && isPrivate(address)))
    {
      log(error ? 'unknown host' : 'refused (local address ' + address + ')');
      socket.end('HTTP/1.1 502 Bad Gateway\r\n\r\n');
      return;
    }
    const upstream = net.connect({ host: address, port: port });
    let open = false, closed = false;

    upstream.setNoDelay(true);
    socket.setNoDelay(true);
    upstream.on('connect', () => {
      open = true;
      log('connected');
      const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
      const protocol = (request.headers['sec-websocket-protocol'] || '').split(',')[0].trim();
      socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept +
                   (protocol ? '\r\nSec-WebSocket-Protocol: ' + protocol : '') + '\r\n\r\n');
    });
    const finish = (text) => {
      if (closed) return;
      closed = true;
      log(text);
      upstream.destroy();
      socket.destroy();
    };
    upstream.on('data', (data) => { if (open && !closed) socket.write(frame(2, data)); });
    upstream.on('close', () => { if (!closed) { if (open) socket.write(frame(8, Buffer.alloc(0))); finish('closed by the server'); } });
    upstream.on('error', (e) => {
      if (!open) socket.end('HTTP/1.1 502 Bad Gateway\r\n\r\n');
      finish('error ' + e.code);
    });
    socket.on('close', () => finish('closed by the page'));
    socket.on('error', () => finish('socket error'));

    // frames of the page
    let buffer = Buffer.alloc(0), message = [];
    socket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      for (;;)
      {
        if (buffer.length < 2) return;
        const opcode = buffer[0] & 0x0f, fin = (buffer[0] & 0x80) !== 0, masked = (buffer[1] & 0x80) !== 0;
        let length = buffer[1] & 0x7f, offset = 2;
        if (length === 126) { if (buffer.length < 4) return; length = buffer.readUInt16BE(2); offset = 4; }
        else if (length === 127) { if (buffer.length < 10) return; length = Number(buffer.readBigUInt64BE(2)); offset = 10; }
        if (buffer.length < offset + (masked ? 4 : 0) + length) return;
        const mask = masked ? buffer.subarray(offset, offset + 4) : null;
        if (masked) offset += 4;
        const payload = Buffer.from(buffer.subarray(offset, offset + length));
        if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
        buffer = buffer.subarray(offset + length);
        if (opcode === 8) { finish('closed by the page'); return; }
        else if (opcode === 9) socket.write(frame(10, payload));
        else if (opcode === 0 || opcode === 1 || opcode === 2)
        {
          message.push(payload);
          if (fin) { if (open) upstream.write(Buffer.concat(message)); if (process.env.PROXY_TRACE) require('fs').appendFileSync(process.env.PROXY_TRACE, Date.now() + ' ' + port + ' ' + message.reduce((n, m) => n + m.length, 0) + '\n'); message = []; }   // (PROXY_TRACE=file: when the page sent how many bytes to which port, for measuring the silence of the game)
        }
      }
    });
  });
});

server.listen(options.port, options.host, () => {
  console.log('WoW proxy on ' + options.host + ':' + options.port + ', ports ' + options.ports.join(',') + (options.hosts ? ', hosts ' + options.hosts.join(',') : ', any public host'));
});
