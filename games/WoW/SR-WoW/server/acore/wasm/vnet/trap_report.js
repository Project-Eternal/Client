// A trap (memory access out of bounds...) in a pthread reaches the page only as a message without a stack: report the stack from inside the worker.
if (typeof importScripts === 'function' && typeof self !== 'undefined' && self.addEventListener)
  self.addEventListener('error', function (e) {
    try { Module['printErr']('trap in a server thread: ' + (e.error && e.error.stack ? e.error.stack : e.message)); } catch (x) { }
  });
