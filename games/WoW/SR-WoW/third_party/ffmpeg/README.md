FFmpeg 6.1.2 (https://ffmpeg.org, LGPL 2.1 or later): libavcodec with only the MPEG-4 part 2 decoder, and libavutil, for the client's
DivX movies (WoW-divx.c). Not in the repository: tools/build_ffmpeg.sh fetches the source and builds `build/` (the SConstruct runs it
when `build/lib/libavcodec.a` is missing). Linked statically into WoW.wasm: whoever distributes the build must also offer the FFmpeg
source (this script and the version above) and allow relinking (the object files of the port are in `build/`).
