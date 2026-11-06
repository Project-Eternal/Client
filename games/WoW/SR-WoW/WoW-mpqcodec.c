/* Decompression of the archives' sectors for the 3.3.5 client: its zlib decompressor (0x453bb0, inflate with a custom allocator, 1.2.3; the dispatcher of the
   compression types of a sector is at 0x458630) takes most of the CPU of the loader threads when they run as recompiled code. native_procs.llasm replaces it
   with the stub that calls this. Modes (the chat command .zlib): client = the original code, native (the default) = zlib of the program with the big lock released,
   check = both, compared (mismatches are logged). A result of 0xffffffff goes on in the original code (also for errors, so their handling is the client's). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <emscripten.h>

#include "srw_debuglog.h"
#include "WoW.h"

#define ORIGINAL_UNCOMPRESS 0x453be4u   /* the original body, in the padding after the replaced procedure */

static volatile int zlib_mode = 1;   /* 0 client, 1 native (the default), 2 check */
static volatile uint64_t native_calls, native_bytes, mismatches;
static volatile double native_ms;

void WoW_ZlibSetMode(int mode)
{
    zlib_mode = mode;
}

int WoW_ZlibGetMode(void)
{
    return zlib_mode;
}

/* bool uncompress(void *dest, uint32_t *dest_length, const void *source, uint32_t source_length), cdecl, 1 = success */
uint32_t CCALL WoW_Uncompress_c(uint32_t dest, uint32_t dest_length, uint32_t source, uint32_t source_length)
{
    uLongf length;
    uint8_t *shadow = NULL;
    uint32_t original_length = 0;
    double began;
    int z;

    if (zlib_mode == 0 || dest == 0 || dest_length == 0 || source == 0) return 0xffffffffu;
    original_length = *(uint32_t *) (uintptr_t) dest_length;
    length = original_length;
    if (zlib_mode == 2)
    {
        /* the original on a copy of the call (it writes the destination: give it its own buffer) */
        uint32_t args[4], copy_length = original_length, result;

        shadow = malloc(original_length > 0 ? original_length : 1);
        if (shadow == NULL) return 0xffffffffu;
        args[0] = (uint32_t) (uintptr_t) shadow; args[1] = (uint32_t) (uintptr_t) &copy_length; args[2] = source; args[3] = source_length;
        result = SRW_CallGuest(ORIGINAL_UNCOMPRESS, 4, args) & 0xff;
        began = emscripten_get_now();
        z = uncompress((Bytef *) (uintptr_t) dest, &length, (const Bytef *) (uintptr_t) source, source_length);
        native_ms += emscripten_get_now() - began;
        native_calls++;
        native_bytes += length;
        if ((z == Z_OK) != (result == 1) || (z == Z_OK && (copy_length != length || memcmp(shadow, (void *) (uintptr_t) dest, length) != 0)))
        {
            mismatches++;
            logprintf("Zlib check: MISMATCH: native %d, %u bytes; the client's %u, %u bytes (source %u bytes)", z, (unsigned int) length, (unsigned int) result, (unsigned int) copy_length, (unsigned int) source_length);
        }
        free(shadow);
        if (z != Z_OK) return 0xffffffffu;
        *(uint32_t *) (uintptr_t) dest_length = (uint32_t) length;
        return 1;
    }
    began = emscripten_get_now();
    SRW_GilRelease();
    z = uncompress((Bytef *) (uintptr_t) dest, &length, (const Bytef *) (uintptr_t) source, source_length);
    SRW_GilAcquire();
    native_ms += emscripten_get_now() - began;
    native_calls++;
    native_bytes += length;
    if (z != Z_OK) return 0xffffffffu;   /* the client decides what an error is */
    *(uint32_t *) (uintptr_t) dest_length = (uint32_t) length;
    {
        static double reported;
        double now = emscripten_get_now();

        if (SRW_LOG_ON(SRW_LOG_PERF) && now - reported > 5000.0)
        {
            reported = now;
            logprintf("Zlib: %llu native decompressions, %llu KB out, %.0f ms in all (%.0f MB/s)", (unsigned long long) native_calls, (unsigned long long) (native_bytes / 1024),
                      native_ms, native_ms > 0 ? native_bytes / 1048576.0 / (native_ms / 1000.0) : 0.0);
        }
    }
    return 1;
}
