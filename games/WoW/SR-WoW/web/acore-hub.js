var acoreHub = (function ()
{
  var listeners = {};
  var early = {};
  var wake = function () { };

  function Connection(port)
  {
    this.port = port;
    this.attached = false;
    this.toServer = [];
    this.toServerBytes = 0;
    this.toClient = [];
    this.toClientOffset = 0;
    this.clientClosed = false;
    this.serverClosed = false;
  }

  return {
    connect: function (port)
    {
      var c = new Connection(port), listener = listeners[port];

      if (listener)
      {
        c.attached = true;
        listener.pending.push(c);
        wake();
      }
      else
      {
        (early[port] = early[port] || []).push(c);
      }
      return c;
    },
    clientSend: function (c, bytes)
    {
      if (!c.attached || c.clientClosed || c.serverClosed) return -1;
      c.toServer.push(bytes);
      c.toServerBytes += bytes.length;
      wake();
      return bytes.length;
    },
    clientClose: function (c)
    {
      c.clientClosed = true;
      wake();
    },

    setWake: function (fn) { wake = fn; },
    listen: function (port)
    {
      if (listeners[port]) return -1;
      var listener = listeners[port] = { pending: [] };

      (early[port] || []).forEach(function (c) { c.attached = true; listener.pending.push(c); });
      delete early[port];
      wake();
      return 0;
    },
    pending: function (port) { var l = listeners[port]; return l ? l.pending.length : 0; },
    accept: function (port)
    {
      var l = listeners[port];
      return l && l.pending.length ? l.pending.shift() : null;
    }
  };
})();
