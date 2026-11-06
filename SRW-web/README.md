# SRW-web

The runtime that lets a program recompiled by SRW from a Win32 executable run in a browser (WebAssembly, Emscripten).

| Folder | Contents |
|---|---|
| `src/`, `include/` | The Windows functions the program imports (kernel32, user32, advapi32, gdi32 and the small libraries, sockets, DirectSound, wave output), the CPU, the big lock, the remote file system, the logging. |
| `d3d9-webgl/` | Direct3D 9 on WebGL 2, with the bridge for the recompiled program in `src/d3d9-bridge.cpp`. Its `LICENSE` is the upstream one. |
| `recompiler/` | The scripts between SRW and the compiler: `recompile.py`, `split_llasm.py`, `gen_glue.py`, the definitions of the imports (`*.def`) and `make_d3d9_def.py`. |
| `web/runtime.js` | The page's side of the runtime: streaming of files, sound, pointer, presentation of frames. |
| `SConscript` | The build of all this; the program's `SConstruct` calls it with the parameters listed at its top. |

What belongs to one program is supplied by that program: the description of its executable (`prepare_exe.py`, `SR.cfg`, `llasm/`), the definitions of the imports of its own libraries, its own C files and its names in `web/program.js`. The runtime calls the program through the hooks declared in `include/srw.h`; `src/hooks.c` holds a default for each, and a definition in the program replaces it.
