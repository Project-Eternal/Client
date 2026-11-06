#!/bin/bash
# The compiled Boost libraries AzerothCore needs (filesystem, program_options, iostreams, regex) as static wasm32 libraries, from third_party/boost
# (the boostorg superproject, 1.83.0, headers made by `b2 headers` into third_party/boost/boost), plus a CMake package file so that
# find_package(Boost COMPONENTS ...) finds them. usage: build-boost.sh OUTDIR   -> OUTDIR/lib/libboost_*.a, OUTDIR/lib/cmake/Boost-1.83.0/
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../third_party/boost" && pwd)
out=${1:?usage: build-boost.sh OUTDIR}
[ -d "$root/boost" ] || (cd "$root" && ./bootstrap.sh --with-toolset=gcc >/dev/null && ./b2 headers >/dev/null)
mkdir -p "$out/obj" "$out/lib" "$out/lib/cmake/Boost-1.83.0"
FLAGS="-std=c++17 -O2 -pthread -fexceptions -I$root -DBOOST_ALL_NO_LIB -DBOOST_FILESYSTEM_STATIC_LINK=1 -DBOOST_SYSTEM_STATIC_LINK=1 -DBOOST_FILESYSTEM_NO_CXX20_ATOMIC_REF"
build_lib() {   # name, source files...
    local name=$1; shift
    local objs=()
    for f in "$@"; do
        local o="$out/obj/$name-$(basename "$f" .cpp).o"
        [ "$o" -nt "$f" ] || em++ $FLAGS -c "$f" -o "$o" || { echo "FAILED: $f" >&2; return 1; }
        objs+=("$o")
    done
    rm -f "$out/lib/libboost_$name.a"
    emar rcs "$out/lib/libboost_$name.a" "${objs[@]}"
    echo "libboost_$name.a: ${#objs[@]} objects"
}
build_lib filesystem "$root"/libs/filesystem/src/*.cpp
build_lib program_options "$root"/libs/program_options/src/*.cpp
build_lib iostreams "$root"/libs/iostreams/src/{bzip2,file_descriptor,gzip,mapped_file,zlib}.cpp
build_lib regex "$root"/libs/regex/src/*.cpp
# thread: AzerothCore's CMake asks for it as soon as one module is built in
FLAGS="$FLAGS -DBOOST_THREAD_POSIX -DBOOST_THREAD_BUILD_LIB=1 -DBOOST_THREAD_USES_CHRONO"
build_lib thread "$root"/libs/thread/src/pthread/{thread.cpp,once.cpp} "$root"/libs/thread/src/future.cpp
cat > "$out/lib/cmake/Boost-1.83.0/BoostConfig.cmake" <<C
set(Boost_FOUND TRUE)
set(Boost_VERSION 1.83.0)
set(Boost_INCLUDE_DIRS "$root")
set(Boost_LIBRARIES "$out/lib/libboost_filesystem.a;$out/lib/libboost_program_options.a;$out/lib/libboost_iostreams.a;$out/lib/libboost_regex.a;$out/lib/libboost_thread.a")
foreach(c filesystem program_options iostreams regex thread)
  set(Boost_\${c}_FOUND TRUE)
  string(TOUPPER \${c} u)
  set(Boost_\${u}_FOUND TRUE)
  set(Boost_\${u}_LIBRARY "$out/lib/libboost_\${c}.a")
endforeach()
C
cat > "$out/lib/cmake/Boost-1.83.0/BoostConfigVersion.cmake" <<'C'
set(PACKAGE_VERSION 1.83.0)
if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
C
