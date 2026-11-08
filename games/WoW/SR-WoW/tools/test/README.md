# Running the web client without a person at the keyboard

For measurements of the diagnostics build (`scons release=1 diagnostics=1 ...`), driven from a browser that a tool can control.

## 1. The environment

```
tools/test/env.sh up                       # game files over HTTP (8082), WebSocket proxy (8090); reports the standard server's containers
tools/test/env.sh serve publish 8092       # a build on a port: cross-origin isolation headers, no caching
tools/test/env.sh status | down
```

Needs the standard server running (`docker compose up -d` in `~/wow-eternal-server/src`: ac-database, ac-authserver, ac-worldserver) with the account `PLAYER`
(the client's patched login screen sends it; see `patch/Data/Interface/GlueXML/Connect.lua`) and the realm address `10.99.0.2` that the proxy maps to this machine.

## 2. The page

```
http://localhost:PORT/#embeddedServer=0&netProxy=ws://localhost:8090/&realmlist=10.99.0.2&locale=enUS&dataUrl=http://localhost:8082/&debugLog=perf&present=bitmap
```

Other settings can be added to the fragment (`d3dDebug=11`, `d3dShaders=0`, ...). A change of the fragment alone does not reload the page (and the page asks before it
is left): load a new address (add `?x=2` before the `#`) or remove the guard first: `window.removeEventListener('beforeunload', window.handleBeforeUnload)`.

## 3. The harness (`wowtest.js`)

Evaluate `wowtest.js` in the page (a browser tool's JavaScript evaluation), then:

```
WT.drive({settleMs: 12000, collectMs: 25000})   // starts, logs in (Enter), enters the world (Enter), settles, measures; runs on in the page
WT.status()                                     // state and the latest perf row
WT.report()                                     // medians over the measuring window: fps, frame, draws, present, other, buffer updates
WT.grep(/re/, n)  WT.key(vk)  WT.active(1)  WT.lines
```

It keeps the page's log, gives keys through the game's exported `SRW_InjectKey`, and sets the foreground flag with `SRW_SetActive`, so no mouse, no focus and no
visible page are needed for the game's own logic; the browser still needs the page to be shown for its graphics to run at full speed (in the Claude desktop app:
show the preview pane and front the tab).

A tool call may time out while `WT.drive` goes on; poll `WT.status()`.

## 4. What the diagnostics build prints every 5 seconds (with `debugLog=perf`)

```
[D3D9] perf: ...                  frame time: draws (the GL calls made for them), present (the wait for the GPU), the rest
[D3D9] gfx: ...                   what was drawn
[D3D9] buffer updates: ...        updates of GL buffers per frame, and how many of them came after a draw of the same frame read the buffer (a hazard: it costs
                                  Chrome/ANGLE about 0.13 ms each, whatever the size; see SRW-web/bench/draw-bench.html)
```

## 5. Notes

- Use the default frame handoff (no `present=` in the fragment) for runs of more than a minute or two. With `present=bitmap` the page crashed ("Target crashed") after a few
  minutes at 140 fps: the code's own comment says the image bitmaps of that mode grow the GPU memory until it is full. The default mode ran 4.5 minutes at a steady 137-142 fps.
- A release build (no diagnostics) logs no perf lines, so `WT.drive` cannot be used with it; count the page's frame handoffs instead (wrap `CanvasRenderingContext2D.prototype.drawImage` and
  `putImageData`) and wait for the log line `Frames are handed to the page`, then press Enter twice as `WT.drive` does.
- `tools/test/env.sh serve DIR PORT` mounts the directory itself: if the build output (publish/) is rebuilt while it is served, copy it first.

- The game limits itself to 30 fps when it believes it is in the background (the page's blur/focus events); `WT.active(1)` and no real mouse or key events keep it in the foreground.
- `gl.finish()` does not wait for the GPU in Chrome; the benchmark waits with a one-pixel `readPixels`.
- DevTools open and focused counts as a background window.
