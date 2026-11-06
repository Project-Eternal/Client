#!/bin/bash
# Configures AzerothCore (server/acore/azerothcore) for Emscripten against the wasm dependencies built by build-openssl.sh, build-boost.sh and the shim.
# usage: configure-acore.sh DEPS_DIR BUILD_DIR     DEPS_DIR has openssl/install, boost, shim/libmysqlsqlite.a, libreadline.a
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
acore_src=$repo/server/acore/azerothcore   # the AzerothCore submodule (its path is in .gitmodules)
deps=${1:?usage: configure-acore.sh DEPS_DIR BUILD_DIR}
build=${2:?usage: configure-acore.sh DEPS_DIR BUILD_DIR}
sysroot=$(em-config CACHE 2>/dev/null || echo /home/claude/emsdk/upstream/emscripten/cache)/sysroot
mkdir -p "$build"
# our modules (server/acore/modules/*) are linked into the acore tree's modules/ directory (untracked there): mod-embedded-auth runs the logon server inside the worldserver
for m in "$repo"/server/acore/modules/*/; do
    n=$(basename "$m"); [ -e "$acore_src/modules/$n" ] || ln -s "../../modules/$n" "$acore_src/modules/$n"
done
# link flags: threads (acore has a dozen of them: database workers, map updates, network), growable memory, a real stack; LINK_EXTRA adds more
LINK="-pthread -fexceptions -sUSE_PTHREADS=1 -sPTHREAD_POOL_SIZE=${POOL:-32} -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sMAXIMUM_MEMORY=2147483648 -sSTACK_SIZE=2097152 -sDEFAULT_PTHREAD_STACK_SIZE=2097152 -sASSERTIONS=1 $LINK_EXTRA"
# the in-process transport: vnet.c replaces the socket calls of libc (see vnet/), the backend (VNET_BACKEND, a file of vnet/) is the host side
emcc -O2 -pthread -fexceptions -c "$here/vnet/vnet.c" -o "$deps/vnet.o"
# CONF_DIR: where the server looks for modules/*.conf (the page's /AzerothCore/etc: web/acore.js; the default is the install prefix, the emscripten sysroot)
LINK="$LINK $deps/vnet.o $(tr '\n' ' ' < "$here/vnet/vnet-link-flags.txt") --pre-js $here/vnet/${VNET_BACKEND:-browser_backend.js}"
CF="-pthread -fexceptions -O2 -DNDEBUG -D_LIBCPP_ENABLE_EXPERIMENTAL -fmacro-prefix-map=$acore_src/= $EXTRA_CF"   # the last: libc++ 16 hides the C++20 ranges algorithms (std::ranges::any_of ...) without it
emcmake cmake -S "$acore_src" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$CF" -DCMAKE_CXX_FLAGS="$CF" -DCMAKE_EXE_LINKER_FLAGS="$LINK" \
    -DACORE_PRODUCT_NAME=EternalCore -DCONF_DIR=/AzerothCore/etc -DAPPS_BUILD=all -DTOOLS_BUILD=none -DSCRIPTS=minimal-static -DMODULES=static -DNOJEM=1 -DWITH_WARNINGS=0 -DUSE_COREPCH=0 -DUSE_SCRIPTPCH=0 -DWITHOUT_GIT=1 \
    -DCMAKE_FIND_ROOT_PATH="$deps/openssl/install;$deps/boost;$sysroot" \
    -DBoost_DIR="$deps/boost/lib/cmake/Boost-1.83.0" -DBoost_NO_SYSTEM_PATHS=ON \
    -DOPENSSL_ROOT_DIR="$deps/openssl/install" -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DOPENSSL_CRYPTO_LIBRARY="$deps/openssl/install/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$deps/openssl/install/lib/libssl.a" -DOPENSSL_INCLUDE_DIR="$deps/openssl/install/include" \
    -DMYSQL_INCLUDE_DIR="$repo/server/acore/mysql-sqlite/include" -DMYSQL_LIBRARY="$deps/shim/libmysqlsqlite.a" \
    -DREADLINE_INCLUDE_DIR="$here/stubs" -DREADLINE_LIBRARY="$deps/libreadline.a" \
    -Wno-dev
