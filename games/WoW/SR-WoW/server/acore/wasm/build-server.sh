#!/bin/bash
# Builds the embedded server module (AzerothCore + the SQLite shim + the in-page transport) for the browser and installs it into OUT (default
# publish/server/acore): worldserver.js/.wasm/.worker.js. Everything is incremental: dependencies that exist are not built again, the AzerothCore
# objects are left to cmake. The first run takes hours (OpenSSL, Boost, ~570 AzerothCore objects at -j3); a relink alone ~1 minute.
# usage: build-server.sh [WORKDIR [OUT]]     WORKDIR (default build/server-wasm) gets deps/ and acore/. Run from anywhere; needs emcc 3.1.29, cmake, perl.
# Called by `scons server=1`. The data files next to it (data/ and init/ from pack-assets.py, patch/ from the repository) come from pack-assets.py, they are not part of this step.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
work=$(mkdir -p "${1:-$repo/build/server-wasm}" && cd "${1:-$repo/build/server-wasm}" && pwd)
out=$(mkdir -p "${2:-$repo/publish/server/acore}" && cd "${2:-$repo/publish/server/acore}" && pwd)
deps=$work/deps
mkdir -p "$deps"
log() { echo "build-server: $*"; }

# 1. the SQLite amalgamation, out of tree (sqlite3.c does not exist in the submodule)
if [ ! -f "$deps/sqlite/sqlite3.c" ]; then
    log "SQLite amalgamation"
    mkdir -p "$deps/sqlite" && (cd "$deps/sqlite" && "$repo/third_party/sqlite/configure" --disable-readline --disable-tcl --enable-update-limit >/dev/null && make sqlite3.c sqlite3.h >/dev/null)
fi
# 2. OpenSSL, Boost, zlib/bzip2 (embuilder), readline stand-in
if [ ! -f "$deps/openssl/install/lib/libcrypto.a" ]; then log "OpenSSL"; "$here/build-openssl.sh" "$deps/openssl"; fi
if [ ! -f "$deps/boost/lib/cmake/Boost-1.83.0/BoostConfig.cmake" ]; then log "Boost"; "$here/build-boost.sh" "$deps/boost"; fi
embuilder build zlib bzip2 >/dev/null
if [ ! -f "$deps/libreadline.a" ]; then emcc -c -O2 "$here/stubs/readline.c" -o "$deps/readline.o" && emar rcs "$deps/libreadline.a" "$deps/readline.o"; fi
# 3. the MySQL-API shim over SQLite (always: cmake sees whether anything changed)
log "SQLite shim"
# (a build directory cannot be reused for another source path)
grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$repo/server/acore/mysql-sqlite" "$deps/shim/CMakeCache.txt" 2>/dev/null || rm -rf "$deps/shim"
if [ ! -f "$deps/shim/CMakeCache.txt" ]; then
    emcmake cmake -S "$repo/server/acore/mysql-sqlite" -B "$deps/shim" -DCMAKE_BUILD_TYPE=Release -DSQLITE_AMALGAMATION_DIR="$deps/sqlite" \
        -DCMAKE_C_FLAGS="-pthread -O2" -DCMAKE_CXX_FLAGS="-pthread -O2 -fexceptions" >/dev/null
fi
cmake --build "$deps/shim" -j3 >/dev/null
# 4. AzerothCore: configured when it never was or when the link flags below or configure-acore.sh changed (cmake keeps the objects: only a relink), then the worldserver is built
acore=$work/acore
# (a build directory cannot be reused for another source path: a full build follows)
[ -f "$acore/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$repo/server/acore/azerothcore" "$acore/CMakeCache.txt" && rm -rf "$acore"
# the web flavor: one module with a factory function, for the page and its workers; the first INITIAL_MEMORY and 8 MB stacks are needed by the
# world's start (ObjectMgr loads), the trap report prints the stack of a wasm trap in a worker, IDBFS keeps etc/ and characters/ (web/acore.js)
LINK_EXTRA="-sINITIAL_MEMORY=1610612736 -sSTACK_SIZE=8388608 -sDEFAULT_PTHREAD_STACK_SIZE=8388608 --pre-js $here/vnet/trap_report.js \
-sMODULARIZE=1 -sEXPORT_NAME=createAcoreServer -sENVIRONMENT=web,worker -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
-sEXPORTED_RUNTIME_METHODS=FS,ENV,callMain,stringToUTF8 -lidbfs.js --profiling-funcs"
stamp="$LINK_EXTRA $(sha256sum "$here/configure-acore.sh" | cut -c1-16)"   # (a change of the flags or of configure-acore.sh configures again)
if [ ! -f "$acore/CMakeCache.txt" ] || [ "$(cat "$acore/.configured" 2>/dev/null)" != "$stamp" ]; then
    log "configuring AzerothCore"
    # configure-acore.sh expects: DEPS/{openssl/install,boost,shim/libmysqlsqlite.a,libreadline.a}
    LINK_EXTRA="$LINK_EXTRA" VNET_BACKEND=browser_backend.js "$here/configure-acore.sh" "$deps" "$acore" >/dev/null
    echo "$stamp" > "$acore/.configured"
fi
log "building the worldserver"
cmake --build "$acore" --target worldserver -- -j3
for f in worldserver.js worldserver.wasm worldserver.worker.js; do cp "$acore/src/server/apps/$f" "$out/$f"; done
ls -l "$out"/worldserver.*
