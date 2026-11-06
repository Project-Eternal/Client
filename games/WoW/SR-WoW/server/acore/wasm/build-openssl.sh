#!/bin/bash
# OpenSSL (third_party/openssl, 3.0 LTS) as static wasm32 libraries for the AzerothCore server module: libcrypto.a and liblegacy.a
# (RC4 lives in the legacy provider; the module registers it as a built-in, see the glue of the server). usage: build-openssl.sh OUTDIR
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../../third_party/openssl" && pwd)
out=${1:?usage: build-openssl.sh OUTDIR}
mkdir -p "$out/build" && cd "$out/build"
export CC=emcc AR=emar RANLIB=emranlib CXX=em++
export PERL5LIB="$here/perl${PERL5LIB:+:$PERL5LIB}"   # IPC::Cmd stand-in (can_run only)
"$src/Configure" linux-generic32 no-asm no-shared no-dso no-engine no-async no-ui-console no-tests no-afalgeng no-sock no-dgram no-secure-memory \
    enable-legacy --prefix="$out/install" -pthread -O2 -DNO_SYSLOG 2>&1 | tail -3
make -j3 build_libs 2>&1 | tail -3
mkdir -p "$out/install/lib" "$out/install/include"
cp libcrypto.a "$out/install/lib/"
[ -f providers/liblegacy.a ] && cp providers/liblegacy.a "$out/install/lib/"
cp -r "$src/include/openssl" "$out/install/include/"
cp -r include/openssl/* "$out/install/include/openssl/"   # the generated headers (configuration.h ...)
ls -l "$out/install/lib"
