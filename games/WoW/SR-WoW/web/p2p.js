var P2P_CMD = { Open: 1, Close: 2, Join: 3, Leave: 4, Publish: 5, Watch: 6, Unwatch: 7, Send: 8, Connect: 9, Disconnect: 10, Will: 11, Quality: 12, NodeAdd: 13, NodeRemove: 14 };
var P2P_EVT = { NodeUp: 1, NodeDown: 2, RoomMessage: 3, InboxMessage: 4, LinkUp: 5, LinkDown: 6, DirectFailed: 7, NodeFailed: 8, NodeList: 9 };
var P2P_ROUTE_ANY = 255, P2P_REASON = { NotTried: 0, NoPath: 1, TimedOut: 2, Disabled: 3 };

function P2P_EncodeEvent(e)
{
  var name = new TextEncoder().encode(e.name || ''), data = e.data || new Uint8Array(0), out = new Uint8Array(1 + 4 + name.length + 4 + data.length + 4), v = new DataView(out.buffer), at = 0;

  out[at++] = e.kind;
  v.setUint32(at, name.length, true); at += 4;
  out.set(name, at); at += name.length;
  v.setUint32(at, data.length, true); at += 4;
  out.set(data, at); at += data.length;
  out[at++] = e.direct ? 1 : 0;
  out[at++] = e.node || 0;
  out[at++] = e.route || 0;
  out[at++] = e.reason || 0;
  return out;
}

function P2P_DecodeCommand(bytes)
{
  var v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), at = 0, dec = new TextDecoder();

  function str() { var n = v.getUint32(at, true); at += 4; var s = dec.decode(bytes.subarray(at, at + n)); at += n; return s; }
  function blob() { var n = v.getUint32(at, true); at += 4; var b = bytes.slice(at, at + n); at += n; return b; }
  var c = { kind: bytes[at++] };

  c.name = str();
  c.text = str();
  c.data = blob();
  c.channel = bytes[at++];
  c.route = at < bytes.length ? bytes[at] : P2P_ROUTE_ANY;
  return c;
}

function P2PHost(options)
{
  var self = this, mqtt = options.mqtt, RTC = options.RTCPeerConnection || (typeof RTCPeerConnection !== 'undefined' ? RTCPeerConnection : null);
  var urls = (options.urls || (options.url ? [options.url] : [])).slice(), nodes = [], inbox = '', prefix = '', will = null, willTopic = '';
  var rooms = {}, watched = {}, known = {}, links = {}, seenSignals = {}, seenOrder = [];
  var log = options.log || function () { }, emit = function (e) { sink(e); }, sink = options.onEvent || function () { };
  var KIND_FRAME = 0, KIND_SIGNAL = 1, MAX_LINKS = 80, LINK_TIMEOUT = 20000, MAX_NODES = 8;
  var DIRECT_ENABLED = function () { return !options.relayOnly && RTC; };

  this.stats = { relayedOut: 0, relayedIn: 0, directOut: 0, directIn: 0, links: 0 };

  function inboxTopic(id) { return prefix + '/inbox/' + id; }
  function copy(payload) { return new Uint8Array(payload); }
  function nodeUp(k) { return !!(nodes[k] && nodes[k].connected); }
  function anyNode() { for (var k = 0; k < nodes.length; k++) if (nodeUp(k)) return k; return -1; }
  function nodesOf(route) { var out = [], k; if (route === P2P_ROUTE_ANY || route === undefined) { for (k = 0; k < nodes.length; k++) if (nodeUp(k)) out.push(k); } else if (nodeUp(route - 1)) out.push(route - 1); return out; }

  function wrap(kind, data)
  {
    var from = new TextEncoder().encode(inbox), out = new Uint8Array(2 + from.length + data.length);

    out[0] = kind;
    out[1] = from.length;
    out.set(from, 2);
    out.set(data, 2 + from.length);
    return out;
  }

  function relay(k, to, kind, data, reliable)
  {
    if (!nodeUp(k)) return;
    nodes[k].client.publish(inboxTopic(to), wrap(kind, data), { qos: reliable ? 1 : 0 });
    self.stats.relayedOut++;
  }

  function label(k) { return 'relay #' + (k + 1); }
  function textOfError(message)
  {
    var m = String(message || '');

    if (/timeout|timed out/i.test(m)) return 'it timed out';
    if (/refused|ECONNREFUSED/i.test(m)) return 'the connection was refused';
    if (/ENOTFOUND|EAI_AGAIN|getaddrinfo|resolve/i.test(m)) return 'the address does not exist';
    if (/certificate|CERT|ssl|tls/i.test(m)) return 'its certificate was not accepted';
    return m ? 'the address does not answer (' + m + ')' : 'the address does not answer';
  }
  function emitList()
  {
    emit({ kind: P2P_EVT.NodeList, data: new TextEncoder().encode(urls.map(function (u) { return u || ''; }).join('\n') + '\n') });
  }

  function connectNode(k)
  {
    var node = nodes[k] = { url: urls[k], client: null, connected: false, delay: 3000, lastError: '', everConnected: false, failures: 0, failedAt: 0 };

    node.client = mqtt.connect(node.url, {
      clientId: 'p2p-' + inbox.substring(0, 16),
      clean: true,
      reconnectPeriod: node.delay,
      connectTimeout: 15000,
      keepalive: 30,
      protocolVersion: 4,
      will: { topic: willTopic, payload: will, qos: 1, retain: false }
    });
    node.client.on('connect', function ()
    {
      if (!node.client) return;
      var topics = [inboxTopic(inbox)];

      node.connected = true;
      node.everConnected = true;
      node.failures = 0;
      node.lastError = '';
      node.delay = 3000;
      log('Connected to ' + label(k) + '.');
      if (node.client) node.client.options.reconnectPeriod = node.delay;
      if (options.debug) log('Connected to ' + label(k) + '; subscribing to ' + (Object.keys(rooms).length + Object.keys(watched).length + 1) + ' topics.');
      Object.keys(rooms).forEach(function (r) { topics.push(r); });
      Object.keys(watched).forEach(function (r) { topics.push(r); });
      node.client.subscribe(topics, { qos: 0 });
      emit({ kind: P2P_EVT.NodeUp, name: node.url, node: k });
    });
    node.client.on('close', function ()
    {
      node.delay = Math.min(60000, node.delay * 2);
      if (!node.client) return;
      node.client.options.reconnectPeriod = node.delay;
      if (node.connected)
      {
        node.connected = false;
        log('The connection to ' + label(k) + ' was lost.');
        emit({ kind: P2P_EVT.NodeDown, name: node.url, node: k });
      }
      else
      {
        var now = Date.now();

        node.failures++;
        if (node.failures === 1 || now - node.failedAt >= 600000)
        {
          var why = textOfError(node.lastError || 'the server closed the connection');

          node.failedAt = now;
          log('The connection to ' + label(k) + ' failed: ' + why + (node.failures > 1 ? ' (' + node.failures + ' attempts)' : '') + '.');
          emit({ kind: P2P_EVT.NodeFailed, name: node.url, node: k, data: new TextEncoder().encode(why) });
        }
      }
    });
    node.client.on('error', function (e) { node.lastError = e && e.message ? e.message : String(e); if (options.debug) log('Error on the connection to ' + label(k) + ': ' + node.lastError + '.'); });
    node.client.on('message', function (topic, payload)
    {
      var bytes = copy(payload);

      if (options.debug) log('Message on ' + topic + ' from relay #' + (k + 1) + ' (' + bytes.length + ' bytes).');
      if (topic === inboxTopic(inbox)) { onInbox(bytes, k); return; }
      if (rooms[topic] || watched[topic]) emit({ kind: P2P_EVT.RoomMessage, name: topic, data: bytes, node: k });
    });
  }

  function open(cmd)
  {
    inbox = cmd.name;
    prefix = cmd.text;
    will = cmd.data;
    willTopic = cmd.text + '/gone/' + cmd.name;
    urls = urls.slice(0, MAX_NODES);
    urls.forEach(function (u, k) { connectNode(k); });
    emitList();
  }

  function addNode(address)
  {
    var k;

    if (!inbox) return;
    if (urls.length >= MAX_NODES) { emit({ kind: P2P_EVT.NodeFailed, name: address, node: MAX_NODES - 1, data: new TextEncoder().encode('too many relay nodes (' + MAX_NODES + ')') }); return; }
    k = urls.length;
    urls.push(address);
    log('Added relay #' + (k + 1) + ' for this session.');
    emitList();
    connectNode(k);
  }

  function removeNode(k)
  {
    var node = nodes[k];

    if (!node || !urls[k]) return;
    log('Removed ' + label(k) + ' for this session.');
    if (node.client) { node.client.end(true); node.client = null; }
    if (node.connected) emit({ kind: P2P_EVT.NodeDown, name: node.url, node: k });
    node.connected = false;
    nodes[k] = { url: '', client: null, connected: false, removed: true };
    urls[k] = '';
    emitList();
  }

  function onInbox(bytes, k)
  {
    var kind, n, from, data;

    if (bytes.length < 3) return;
    kind = bytes[0];
    n = bytes[1];
    if (bytes.length < 2 + n) return;
    from = new TextDecoder().decode(bytes.subarray(2, 2 + n));
    data = bytes.subarray(2 + n);
    if (kind === KIND_FRAME)
    {
      self.stats.relayedIn++;
      emit({ kind: P2P_EVT.InboxMessage, name: from, data: data, route: k + 1 });
    }
    else if (kind === KIND_SIGNAL && known[from]) onSignal(from, data);
  }

  function iceServers() { return options.iceServers || [{ urls: 'stun:stun.l.google.com:19302' }]; }

  function failed(link, reason)
  {
    if (link.reported || link.open) return;
    link.reported = true;
    emit({ kind: P2P_EVT.DirectFailed, name: link.peer, reason: reason });
  }

  function newLink(peer)
  {
    var link = { peer: peer, pc: new RTC({ iceServers: iceServers() }), rel: null, unrel: null, open: false, ice: [], remote: false, timer: 0, reported: false };

    links[peer] = link;
    self.stats.links++;
    link.pc.onicecandidate = function (ev)
    {
      if (options.debug) log('Candidate ' + (ev.candidate ? ev.candidate.candidate : 'end') + '.');
      if (ev.candidate) signal(peer, { t: 'ice', c: ev.candidate });
    };
    link.pc.onconnectionstatechange = function ()
    {
      var s = link.pc.connectionState;

      if (options.debug) log('Direct link to ' + peer.substring(0, 8) + ': ' + s + '.');

      if (s === 'failed' || s === 'closed' || s === 'disconnected')
      {
        if (s === 'failed') failed(link, P2P_REASON.NoPath);
        closeLink(peer, links[peer] === link);
      }
    };
    link.pc.ondatachannel = function (ev) { attach(link, ev.channel); };
    link.timer = setTimeout(function ()
    {
      if (!link.open && links[peer] === link)
      {
        log('The direct link to ' + peer.substring(0, 8) + ' timed out.');
        failed(link, P2P_REASON.TimedOut);
        closeLink(peer, true);
      }
    }, LINK_TIMEOUT);
    return link;
  }

  function attach(link, ch)
  {
    ch.binaryType = 'arraybuffer';
    if (ch.label === 'r')
    {
      link.rel = ch;
      ch.onopen = function () { link.open = true; clearTimeout(link.timer); emit({ kind: P2P_EVT.LinkUp, name: link.peer, direct: true }); };
      ch.onclose = function () { if (links[link.peer] === link) closeLink(link.peer, true); };
    }
    else link.unrel = ch;
    ch.onmessage = function (ev) { self.stats.directIn++; emit({ kind: P2P_EVT.InboxMessage, name: link.peer, data: new Uint8Array(ev.data), route: 0 }); };
  }

  function signal(peer, obj)
  {
    var data;

    obj.id = Math.random().toString(36).substring(2, 12);
    data = new TextEncoder().encode(JSON.stringify(obj));
    nodesOf(P2P_ROUTE_ANY).forEach(function (k) { relay(k, peer, KIND_SIGNAL, data, true); });
  }

  function connectTo(peer)
  {
    var link;

    if (!DIRECT_ENABLED()) { emit({ kind: P2P_EVT.DirectFailed, name: peer, reason: P2P_REASON.Disabled }); return; }
    if (links[peer] || Object.keys(links).length >= MAX_LINKS) return;
    link = newLink(peer);
    attach(link, link.pc.createDataChannel('r', { ordered: true }));
    attach(link, link.pc.createDataChannel('u', { ordered: false, maxRetransmits: 0 }));
    link.pc.createOffer().then(function (offer)
    {
      return link.pc.setLocalDescription(offer).then(function () { signal(peer, { t: 'offer', sdp: link.pc.localDescription }); });
    }).catch(function (e) { log('The offer for the direct link to ' + peer.substring(0, 8) + ' failed: ' + e + '.'); failed(link, P2P_REASON.NoPath); closeLink(peer, true); });
  }

  function onSignal(peer, bytes)
  {
    var msg, link;

    try { msg = JSON.parse(new TextDecoder().decode(bytes)); } catch (e) { return; }
    if (msg.id)
    {
      if (seenSignals[msg.id]) return;
      seenSignals[msg.id] = 1;
      seenOrder.push(msg.id);
      if (seenOrder.length > 512) delete seenSignals[seenOrder.shift()];
    }
    if (options.debug) log('Signal ' + msg.t + ' from ' + peer.substring(0, 8) + '.');
    if (!DIRECT_ENABLED()) return;
    link = links[peer];
    if (msg.t === 'offer')
    {
      if (link) closeLink(peer, false);
      if (Object.keys(links).length >= MAX_LINKS) return;
      link = newLink(peer);
      link.pc.setRemoteDescription(msg.sdp).then(function ()
      {
        link.remote = true;
        link.ice.forEach(function (c) { link.pc.addIceCandidate(c).catch(function () { }); });
        link.ice = [];
        return link.pc.createAnswer();
      }).then(function (answer)
      {
        return link.pc.setLocalDescription(answer).then(function () { signal(peer, { t: 'answer', sdp: link.pc.localDescription }); });
      }).catch(function (e) { log('The answer for the direct link to ' + peer.substring(0, 8) + ' failed: ' + e + '.'); closeLink(peer, true); });
    }
    else if (msg.t === 'answer' && link)
    {
      link.pc.setRemoteDescription(msg.sdp).then(function ()
      {
        link.remote = true;
        link.ice.forEach(function (c) { link.pc.addIceCandidate(c).catch(function () { }); });
        link.ice = [];
      }).catch(function (e) { log('The answer from ' + peer.substring(0, 8) + ' could not be applied: ' + e + '.'); });
    }
    else if (msg.t === 'ice' && link)
    {
      if (link.remote) link.pc.addIceCandidate(msg.c).catch(function () { });
      else link.ice.push(msg.c);
    }
  }

  function closeLink(peer, tell)
  {
    var link = links[peer];

    if (!link) return;
    delete links[peer];
    clearTimeout(link.timer);
    self.stats.links--;
    try { link.pc.close(); } catch (e) { }
    if (link.open) emit({ kind: P2P_EVT.LinkDown, name: peer, direct: true });
  }

  function send(cmd)
  {
    var link = links[cmd.name], route = cmd.route === undefined ? P2P_ROUTE_ANY : cmd.route, ch, k;

    if (route === 0 || route === P2P_ROUTE_ANY)
    {
      if (link && link.open)
      {
        ch = (cmd.channel === 1 && link.unrel && link.unrel.readyState === 'open') ? link.unrel : link.rel;
        if (ch && ch.readyState === 'open')
        {
          ch.send(cmd.data);
          self.stats.directOut++;
          return;
        }
      }
      if (route === 0) return;
      k = anyNode();
    }
    else k = route - 1;
    if (k >= 0) relay(k, cmd.name, KIND_FRAME, cmd.data, cmd.channel !== 1);
  }

  function publishWill()
  {
    nodes.forEach(function (n, k) { if (nodeUp(k) && will) n.client.publish(willTopic, will, { qos: 1 }); });
  }

  function quality(text)
  {
    var f = String(text || '').split(' ');

    if (options.onQuality) options.onQuality({ state: f[0], latency: parseFloat(f[1]) || 0, loss: parseFloat(f[2]) || 0, count: parseInt(f[3], 10) || 0 });
  }

  this.command = function (cmd)
  {
    var ks;

    switch (cmd.kind)
    {
      case P2P_CMD.Open: open(cmd); break;
      case P2P_CMD.Will: publishWill(); break;
      case P2P_CMD.Quality: quality(cmd.text); break;
      case P2P_CMD.NodeAdd: addNode(cmd.name); break;
      case P2P_CMD.NodeRemove: removeNode(cmd.route); break;
      case P2P_CMD.Close:
        Object.keys(links).forEach(function (p) { closeLink(p, false); });
        nodes.forEach(function (n) { if (n.client) { n.client.end(true); n.client = null; n.connected = false; } });
        break;
      case P2P_CMD.Join:
        if (options.debug) log('Joining the shard ' + cmd.name + '.');
        rooms[cmd.name] = 1;
        nodesOf(cmd.route).forEach(function (k) { nodes[k].client.subscribe(cmd.name, { qos: 0 }); });
        break;
      case P2P_CMD.Leave: delete rooms[cmd.name]; nodesOf(P2P_ROUTE_ANY).forEach(function (k) { nodes[k].client.unsubscribe(cmd.name); }); break;
      case P2P_CMD.Publish: nodesOf(cmd.route).forEach(function (k) { nodes[k].client.publish(cmd.name, cmd.data, { qos: 0 }); }); break;
      case P2P_CMD.Watch:
        watched[cmd.name] = 1;
        known[cmd.name.substring(cmd.name.lastIndexOf('/') + 1)] = 1;
        nodesOf(cmd.route).forEach(function (k) { nodes[k].client.subscribe(cmd.name, { qos: 1 }); });
        break;
      case P2P_CMD.Unwatch:
        delete watched[cmd.name];
        delete known[cmd.name.substring(cmd.name.lastIndexOf('/') + 1)];
        nodesOf(P2P_ROUTE_ANY).forEach(function (k) { nodes[k].client.unsubscribe(cmd.name); });
        break;
      case P2P_CMD.Send: send(cmd); break;
      case P2P_CMD.Connect: connectTo(cmd.name); break;
      case P2P_CMD.Disconnect: closeLink(cmd.name, true); break;
    }
  };

  this.abort = function () { nodes.forEach(function (n) { if (n.client && n.client.stream) n.client.stream.destroy(); }); };
  this.setEventSink = function (fn) { sink = fn; };
  this.log = log;
  this.publishWill = publishWill;
  this.close = function () { self.command({ kind: P2P_CMD.Close }); };
  this.isConnected = function () { return anyNode() >= 0; };
  this.nodeState = function () { return nodes.map(function (n) { return { url: n.url, connected: n.connected, removed: !!n.removed }; }); };
  this.linkState = function ()
  {
    var out = {};

    Object.keys(links).forEach(function (p) { out[p] = links[p].open; });
    return out;
  };
}

function P2P_AttachRings(host, getBuffer, toHostPtr, toModulePtr)
{
  var pending = [], timer = 0, running = true;

  function ring(ptr) { var i = new Int32Array(getBuffer(), ptr, 4), bytes = new Uint8Array(getBuffer(), ptr + 16, i[2]); return { i: i, bytes: bytes, mask: i[2] - 1 }; }

  function pop(r)
  {
    var head = Atomics.load(r.i, 0), tail = Atomics.load(r.i, 1), len, out, k;

    if (head === tail) return null;
    len = 0;
    for (k = 0; k < 4; k++) len |= r.bytes[(head + k) & r.mask] << (8 * k);
    len >>>= 0;
    out = new Uint8Array(len);
    for (k = 0; k < len; k++) out[k] = r.bytes[(head + 4 + k) & r.mask];
    Atomics.store(r.i, 0, (head + 4 + len) | 0);
    return out;
  }

  function push(r, bytes)
  {
    var head = Atomics.load(r.i, 0), tail = Atomics.load(r.i, 1), capacity = r.mask + 1, need = bytes.length + 4, k;

    if (capacity - ((tail - head) | 0) < need) return false;
    for (k = 0; k < 4; k++) r.bytes[(tail + k) & r.mask] = (bytes.length >>> (8 * k)) & 255;
    for (k = 0; k < bytes.length; k++) r.bytes[(tail + 4 + k) & r.mask] = bytes[k];
    Atomics.store(r.i, 1, (tail + need) | 0);
    Atomics.add(r.i, 3, 1);
    Atomics.notify(r.i, 3);
    return true;
  }

  host.setEventSink(function (e) { pending.push(P2P_EncodeEvent(e)); });

  function pump()
  {
    var out = ring(toHostPtr), inn = ring(toModulePtr), rec;

    while ((rec = pop(out)) !== null)
    {
      try { host.command(P2P_DecodeCommand(rec)); } catch (e) { host.log('A command of the server was not understood: ' + e + '.'); }
    }
    while (pending.length && push(inn, pending[0])) pending.shift();
  }

  timer = setInterval(function () { if (running) pump(); }, 20);
  return { pump: pump, stop: function () { running = false; clearInterval(timer); } };
}

if (typeof module !== 'undefined') module.exports = { P2PHost: P2PHost, P2P_AttachRings: P2P_AttachRings, P2P_CMD: P2P_CMD, P2P_EVT: P2P_EVT, P2P_EncodeEvent: P2P_EncodeEvent, P2P_DecodeCommand: P2P_DecodeCommand };
