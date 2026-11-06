#!/bin/bash
# Builds the setup tool (tools/setup.cpp: AzerothCore's databases as SQLite files from AzerothCore's SQL) natively with the host compiler.
# usage: build-setup.sh WORKDIR     -> WORKDIR/acore-db-setup (WORKDIR also gets the SQLite amalgamation and the cmake build; incremental)
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
if [ ! -f "$work/sqlite/sqlite3.c" ]; then
    mkdir -p "$work/sqlite" && (cd "$work/sqlite" && "$repo/third_party/sqlite/configure" --disable-readline --disable-tcl --enable-update-limit >/dev/null && make sqlite3.c sqlite3.h >/dev/null)
fi
[ -f "$work/build/CMakeCache.txt" ] || cmake -S "$here/.." -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DSQLITE_AMALGAMATION_DIR="$work/sqlite" >/dev/null
cmake --build "$work/build" --target acore-db-setup -j3 >/dev/null
ln -sf build/acore-db-setup "$work/acore-db-setup"
