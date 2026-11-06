// The host side of the server's virtual sockets in a web page: connections come from the game of the same page through web/acore-hub.js (plain queues on the main
// thread, no network). The server module's pthread workers must not create it (MAIN_THREAD_EM_ASM runs it on the page's main thread).
if (typeof ENVIRONMENT_IS_PTHREAD === 'undefined' || !ENVIRONMENT_IS_PTHREAD) {
  (function () {
    var hub = (typeof window !== 'undefined' ? window : self).acoreHub;
    var wakeAddress = 0;
    var conns = {};      // id -> hub connection
    var nextId = 1;

    function wake() {
      if (!wakeAddress) return;
      var i32 = new Int32Array(wasmMemory.buffer);
      Atomics.add(i32, wakeAddress >> 2, 1);
      Atomics.notify(i32, wakeAddress >> 2);
    }
    function heap() { return new Uint8Array(wasmMemory.buffer); }

    Module['acoreNet'] = {
      setWakeAddress: function (address) { wakeAddress = address; hub.setWake(wake); },
      listen: function (port) { return hub.listen(port); },
      pending: function (port) { return hub.pending(port); },
      accept: function (port, ipPtr, portPtr) {
        var c = hub.accept(port);
        if (!c) return 0;
        var id = nextId++;
        conns[id] = c;
        var u32 = new Uint32Array(wasmMemory.buffer);
        u32[ipPtr >> 2] = 0x0100007f;      // 127.0.0.1 (only for logs)
        u32[portPtr >> 2] = 1024 + (id & 0xfff);
        return id;
      },
      readable: function (id) {
        var c = conns[id];
        if (!c) return -1;
        return c.toServerBytes > 0 ? c.toServerBytes : (c.clientClosed ? -1 : 0);
      },
      recv: function (id, ptr, max) {
        var c = conns[id];
        if (!c) return -1;
        if (c.toServerBytes === 0) return c.clientClosed ? -1 : 0;
        var out = heap(), n = 0;
        while (n < max && c.toServer.length) {
          var chunk = c.toServer[0], take = Math.min(chunk.length, max - n);
          out.set(chunk.subarray(0, take), ptr + n);
          n += take;
          if (take === chunk.length) c.toServer.shift(); else c.toServer[0] = chunk.subarray(take);
        }
        c.toServerBytes -= n;
        return n;
      },
      send: function (id, ptr, len) {
        var c = conns[id];
        if (!c || c.clientClosed) return -1;
        c.toClient.push(heap().slice(ptr, ptr + len));
        return len;
      },
      shutdown: function (id, how) { var c = conns[id]; if (c && how !== 0) c.serverClosed = true; },
      close: function (id) { var c = conns[id]; if (c) { c.serverClosed = true; delete conns[id]; } }
    };
  })();
}
