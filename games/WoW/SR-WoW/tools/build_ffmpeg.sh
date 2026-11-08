#!/bin/bash
# Builds the part of FFmpeg that the movies need (libavcodec with only the MPEG-4 part 2 decoder, and libavutil) for the WebAssembly
# build: WoW-divx.c decodes the client's DivX ("DXGM") movies with it. LGPL 2.1, see third_party/ffmpeg/README.md.
# usage: tools/build_ffmpeg.sh [OUTPUT_DIR]   (default third_party/ffmpeg/build; needs emcc and the network once for the source)
set -e
version=6.1.2
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$here/third_party/ffmpeg/build}"
work="${TMPDIR:-/tmp}/ffmpeg-$version"
if [ ! -d "$work/FFmpeg-n$version" ]; then
    mkdir -p "$work"
    curl -sfL -o "$work/ffmpeg.tar.gz" "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n$version.tar.gz"
    tar xzf "$work/ffmpeg.tar.gz" -C "$work"
fi
cd "$work/FFmpeg-n$version"
emsdk_bin="$(dirname "$(command -v emcc)")/../bin"
./configure --prefix="$out" --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm="$emsdk_bin/llvm-nm" \
    --target-os=none --arch=x86_32 --enable-cross-compile --disable-asm --disable-x86asm --disable-inline-asm \
    --disable-programs --disable-doc --disable-everything --enable-decoder=mpeg4 \
    --disable-avdevice --disable-avformat --disable-swresample --disable-swscale --disable-postproc --disable-avfilter \
    --disable-network --disable-pthreads --disable-w32threads --disable-os2threads --disable-debug --disable-runtime-cpudetect \
    --disable-autodetect --extra-cflags="-O2 -pthread" --extra-ldflags="-pthread" > "$work/configure.log" 2>&1 || { tail -20 "$work/configure.log"; exit 1; }
make -j3 > "$work/make.log" 2>&1 || { tail -30 "$work/make.log"; exit 1; }
make install > /dev/null
echo "FFmpeg $version (MPEG-4 decoder) in $out"
