# SR

A project to [statically recompile](https://en.wikipedia.org/wiki/Binary_translation "Static binary translation") DOS and Windows game executables to create Windows (x86/x64) or Linux (x86/x64/arm/arm64/riscv64) or MacOS (x64/arm64) versions of the games.

Following Windows games are supported:

* [World of Warcraft](https://en.wikipedia.org/wiki/World_of_Warcraft "World of Warcraft") 3.3.5a, as **WoW Eternal**: a web browser version ([WebAssembly](https://webassembly.org/ "WebAssembly"))

The source code is released with MIT license (except libraries, etc. by other people, which have their own license).
For the purpose of using the code in GPL projects, the source code is also released with GPLv2 or later and LGPLv2.1 or later.
The code of WoW Eternal (*games/WoW*) is released with MPL 2.0. See [LICENSE](LICENSE).

<hr/>

WoW Eternal:

* The game's executable is recompiled to llasm and compiled by [Emscripten](https://emscripten.org/ "Emscripten") to WebAssembly; the Windows functions it uses are implemented on top of the browser (Direct3D 9 with WebGL 2 through an extended [d3d9-webgl](https://github.com/LostMyCode/d3d9-webgl "d3d9-webgl"), sound with Web Audio, threads with Web Workers).
* The game's files are not part of the project. The page reads them from the player's own installation (a folder chosen in the browser) or from a web server, and keeps downloaded files in the browser's cache.
* The page runs its own server, **[EternalCore](https://github.com/Project-Eternal/EternalCore "EternalCore")**: a derivative of [AzerothCore](https://www.azerothcore.org/ "AzerothCore") compiled to WebAssembly, using SQLite and saving the characters in the browser. Its modules read the world data from the running game ([mod-wowcontroller](https://github.com/Project-Eternal/mod-wowcontroller "mod-wowcontroller")) and let players share a world through a public relay server, with no game server hosted by anyone ([mod-p2p](https://github.com/Project-Eternal/mod-p2p "mod-p2p")). EternalCore and its modules are released under their own licenses.
* The game can also use a small built-in server (a port of [AIO Sandbox](https://github.com/barncastle/AIO-Sandbox "AIO Sandbox")).

<hr/>

The projects consists of following subprojects (read the readme files in subproject directories for more information):

* **SR**
  * The static recompiler (for DOS executables) itself.
  * It takes the original executable as an input together with information about the original executable and produces x86 / arm / llasm / x64 assembler version of the executable.
  * The generated x86/arm/llasm/x64 assembler version of the executable is not part of the project.
* **SRW**
  * The static recompiler (for Windows executables) itself.
  * It takes the original executable as an input together with information about the original executable and produces x86 / llasm / x64 assembler version of the executable.
  * The generated x86/llasm/x64 assembler version of the executable is not part of the project.
* **SRW-web**
  * The runtime which lets a Windows executable recompiled by SRW (llasm version) run in a web browser as [WebAssembly](https://webassembly.org/ "WebAssembly"), built with [Emscripten](https://emscripten.org/ "Emscripten").
  * It implements the Windows functions used by the executable on top of the browser: Direct3D 9 with WebGL 2 (an extended [d3d9-webgl](https://github.com/LostMyCode/d3d9-webgl "d3d9-webgl")), sound, threads and files read over the network.
* **llasm**
  * Program which converts *.llasm* file to [LLVM](https://llvm.org/ "LLVM") language-independent intermediate representation, which can be compiled to native code.
* **SR-games**
  * The information about the original DOS executables.
* **SRW-games**
  * The information about the original Windows executables.
* **games**
  * Game specific source code.
  * Together with the generated assembler versions of the executables, these files can be used to build Windows (x86/x64) or Linux (x86/x64/arm/arm64/riscv64) or MacOS (x64/arm64) versions of the games, or the WebAssembly version of World of Warcraft (*games/WoW*).
  * Uses plugins to play (or play better) music or to enhance the displayed image in DOS games.
* **midi-plugins**
  * Plugins used by the games to play MIDI (and other types) music.
* **scaler-plugins**
  * Plugins used by the DOS games to enhance the displayed image.
* **pycfg**
  * A configuration utility that can be used on Linux (sorry Windows/MacOS users) to change settings in the configuration files (without editing the files).
* **SR64-loader**
  * Loader for older 64-bit versions. It's no longer needed.

<hr/>

Some notes:

* The project supports creating 32-bit or 64-bit, Windows or Linux or MacOS versions of the games.
* The generated versions are little-endian, not big-endian.
* The generated arm version supports softfp and hardfp calling conventions.
