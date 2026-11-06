#include "d3d9.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <array>
#ifndef D3D9_PIXEL_SHADERS_INC
#error D3D9_PIXEL_SHADERS_INC must name the table of translated pixel shaders (made from the game by tools/make_ps_shaders.py)
#endif
#include D3D9_PIXEL_SHADERS_INC
#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <cmath>

// What goes to WebGL, counted for the report of d3d9-bridge.cpp (scons diagnostics=1): 0 bytes to buffers, 1 bytes to textures (uncompressed
// ones counted as 4 bytes a pixel), 2 buffers made, 3 buffers deleted, 4 textures made, 5 deleted, 6 render buffers and frame buffers made, 7 deleted
unsigned long long g_srw_gl_stats[8];

// The frame time pie chart of the program (srw.h SRW_PROF_*): the time of the game thread in a scope goes to a category of its own.
extern "C" { extern volatile int SRW_ProfOn; void SRW_ProfEnter(int category); void SRW_ProfLeave(void); }
namespace { enum { PROF_SHADER_ = 3, PROF_UPLOAD_ = 4 }; }
struct ProfScope {
    bool on;
    explicit ProfScope(int category) : on(SRW_ProfOn != 0) { if (on) SRW_ProfEnter(category); }
    ~ProfScope() { if (on) SRW_ProfLeave(); }
};
#ifdef SRW_DIAGNOSTICS
static void srw_glBufferData(GLenum t, GLsizeiptr n, const void* d, GLenum u) { ProfScope prof_(PROF_UPLOAD_); g_srw_gl_stats[0] += (unsigned long long) n; glBufferData(t, n, d, u); }
static void srw_glBufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void* d) { ProfScope prof_(PROF_UPLOAD_); g_srw_gl_stats[0] += (unsigned long long) n; glBufferSubData(t, o, n, d); }
static void srw_glTexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum fm, GLenum ty, const void* p) { ProfScope prof_(PROF_UPLOAD_); if (p) g_srw_gl_stats[1] += (unsigned long long) w * h * 4; glTexImage2D(t, l, f, w, h, b, fm, ty, p); }
static void srw_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fm, GLenum ty, const void* p) { ProfScope prof_(PROF_UPLOAD_); g_srw_gl_stats[1] += (unsigned long long) w * h * 4; glTexSubImage2D(t, l, x, y, w, h, fm, ty, p); }
static void srw_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void* d) { g_srw_gl_stats[1] += (unsigned long long) n; glCompressedTexImage2D(t, l, f, w, h, b, n, d); }
static void srw_glGenBuffers(GLsizei n, GLuint* ids) { g_srw_gl_stats[2] += n; glGenBuffers(n, ids); }
static void srw_glDeleteBuffers(GLsizei n, const GLuint* ids) { g_srw_gl_stats[3] += n; glDeleteBuffers(n, ids); }
static void srw_glGenTextures(GLsizei n, GLuint* ids) { g_srw_gl_stats[4] += n; glGenTextures(n, ids); }
static void srw_glDeleteTextures(GLsizei n, const GLuint* ids) { g_srw_gl_stats[5] += n; glDeleteTextures(n, ids); }
static void srw_glGenRenderbuffers(GLsizei n, GLuint* ids) { g_srw_gl_stats[6] += n; glGenRenderbuffers(n, ids); }
static void srw_glGenFramebuffers(GLsizei n, GLuint* ids) { g_srw_gl_stats[6] += n; glGenFramebuffers(n, ids); }
static void srw_glDeleteRenderbuffers(GLsizei n, const GLuint* ids) { g_srw_gl_stats[7] += n; glDeleteRenderbuffers(n, ids); }
static void srw_glDeleteFramebuffers(GLsizei n, const GLuint* ids) { g_srw_gl_stats[7] += n; glDeleteFramebuffers(n, ids); }
#define glBufferData srw_glBufferData
#define glBufferSubData srw_glBufferSubData
#define glTexImage2D srw_glTexImage2D
#define glTexSubImage2D srw_glTexSubImage2D
#define glCompressedTexImage2D srw_glCompressedTexImage2D
#define glGenBuffers srw_glGenBuffers
#define glDeleteBuffers srw_glDeleteBuffers
#define glGenTextures srw_glGenTextures
#define glDeleteTextures srw_glDeleteTextures
#define glGenRenderbuffers srw_glGenRenderbuffers
#define glGenFramebuffers srw_glGenFramebuffers
#define glDeleteRenderbuffers srw_glDeleteRenderbuffers
#define glDeleteFramebuffers srw_glDeleteFramebuffers
#endif

static const char* g_canvas_selector = "#srwcanvas";

// The game runs in a worker that never returns to the event loop, so the implicit presentation of an
// OffscreenCanvas never happens. The worker renders into an OffscreenCanvas of its own and sends each frame to the page as an
// ImageBitmap (the page shows it, see web/runtime.js: Module.srwShowFrame).
EM_JS(int, srw_create_offscreen_canvas, (int width, int height), {
    try {
        if (typeof OffscreenCanvas === "undefined") return 0;
        GL.offscreenCanvases["srwcanvas"] = new OffscreenCanvas(width, height);
        return 1;
    } catch (e) {
        return 0;
    }
});

// (a fallback for Reset) sets the size of the OffscreenCanvas of the worker directly and lets Emscripten size its offscreen framebuffer
EM_JS(int, srw_resize_offscreen_canvas, (int width, int height), {
    var canvas = GL.offscreenCanvases["srwcanvas"];

    if (!canvas) return 0;
    canvas.width = width;
    canvas.height = height;
    try {
        if (GL.currentContext && GL.resizeOffscreenFramebuffer) GL.resizeOffscreenFramebuffer(GL.currentContext);
    } catch (e) {
    }
    return 1;
});

EM_JS(int, srw_present_frame, (int counter_pointer), {
    var canvas = GL.offscreenCanvases["srwcanvas"];

    if (!canvas || !canvas.transferToImageBitmap) return 0;
    // the page has not shown the frames sent before: skip this one
    if (Atomics.load(HEAP32, counter_pointer >> 2) >= 2) return 0;
    try {
        var bitmap = canvas.transferToImageBitmap();
        Atomics.add(HEAP32, counter_pointer >> 2, 1);
        postMessage({ cmd: "callHandler", handler: "srwShowFrame", args: [bitmap, counter_pointer] }, [bitmap]);
    } catch (e) {
        console.error("[D3D9] A frame could not be sent (canvas " + canvas.width + "x" + canvas.height + "): " + e);
        return 0;
    }
    return 1;
});

static volatile int g_frames_in_flight = 0;
static unsigned int g_gl_generation = 0;   // counts the WebGL contexts: objects of an older one are gone

// What the wrapper did in a frame (all builds, a few additions per draw): the `gfx` report and the perf line of the diagnostics build. The
// frame that was just presented is kept (g_stats_last, read by the report from another thread: plain numbers, a torn read is harmless).
struct FrameStats {
    unsigned draws, primitives, pixel_shader_draws, render_target_draws;
    unsigned alpha_test_draws;       // draws with the alpha test on
    unsigned alpha_coverage_draws;   // ... of them drawn as coverage of the multisampled back buffer
    unsigned alpha_blended_draws;    // ... not, because blending is on
    unsigned alpha_other_func_draws; // ... not, because the function is not GREATER / GREATEREQUAL
    unsigned alpha_no_samples_draws; // ... not, because the back buffer has one sample (or the setting d3dAlphaCoverage is 0 or a render target is drawn)
    unsigned aniso_applied[5];       // sampler settings applied with 1, 2, 4, 8, 16 samples of anisotropy (the texture parameter set)
    unsigned aniso_asked;            // sampler settings with an anisotropic filter in the program's state (D3DTEXF_ANISOTROPIC)
    unsigned aniso_no_mips;          // ... where the texture has no mipmaps (the filter is not applied)
    unsigned max_aniso_state;        // the largest D3DSAMP_MAXANISOTROPY seen with such a filter
    unsigned specular_enable_draws;  // draws with D3DRS_SPECULARENABLE on (the fixed function shader has no specular term: a hypothesis for the metal of some models)
    unsigned specular_vertex_draws;  // draws whose vertices have a specular color (D3DFVF_SPECULAR, not passed to the shader)
    unsigned lit_draws;              // draws with the lighting on
    unsigned texgen_draws;           // draws where a stage generates its coordinates (camera space normal, position, reflection vector, sphere map)
};
static FrameStats g_stats, g_stats_last, g_stats_window;
static unsigned g_stats_window_frames = 0;
static void stats_end_frame() {
    g_stats_last = g_stats;
    unsigned* w = (unsigned*) &g_stats_window;
    const unsigned* f = (const unsigned*) &g_stats;
    for (size_t i = 0; i < sizeof(FrameStats) / sizeof(unsigned); i++) w[i] += f[i];
    g_stats_window_frames++;
    if (g_stats_window_frames >= 20000) { memset(&g_stats_window, 0, sizeof(g_stats_window)); g_stats_window_frames = 0; }   // (only the diagnostics build's log resets it by time)
    memset(&g_stats, 0, sizeof(g_stats));
}
#define SRW_DRAW_STAT(count) do { g_stats.draws++; g_stats.primitives += (unsigned) (count); } while (0)

#ifdef SRW_DIAGNOSTICS
// where the time of a frame goes (diagnostics build): draw calls (submitting to WebGL), the read-back of the frame, the rest is the game
static double g_perf_draw_ms, g_perf_present_ms, g_perf_readback_ms, g_perf_commit_ms;
static unsigned int g_perf_draws;
struct PerfDrawTimer {
    double start = emscripten_get_now();
    ~PerfDrawTimer() { g_perf_draw_ms += emscripten_get_now() - start; g_perf_draws++; }
};
#define PERF_DRAW_TIMER PerfDrawTimer perf_draw_timer;
#else
#define PERF_DRAW_TIMER
#endif

// The frame as pixels for the page (setting "present" = pixels, the default): read back, flipped, two buffers taking turns; the page copies
// them into a 2D canvas. This avoids the drawing buffers of transferToImageBitmap, which the browser gives back through the event loop of
// the worker (which the game thread never reaches: the GPU memory of the tab grew until it filled the RAM).
EM_JS(int, srw_present_pixels, (int counter_pointer, int pixels_pointer, int width, int height), {
    Atomics.add(HEAP32, counter_pointer >> 2, 1);
    postMessage({ cmd: "callHandler", handler: "srwShowPixels", args: [pixels_pointer, width, height, counter_pointer] });
    return 1;
});

#include "../include/srw_debuglog.h"

#ifdef SRW_DIAGNOSTICS
static bool g_trace_frame = false;   // a frame whose draws and surface calls are all logged (category d3d): presents 149 and 150
// the calls that copy or read surfaces, logged to find out how the program builds small pictures of 3D models (the first 40 of each kind, category d3d)
#define LOG_SURFACE_CALL(counter, ...) do { static int count_##counter = 0; if (SRW_LOG_ON(SRW_LOG_D3D) && (count_##counter++ < 40 || g_trace_frame)) { printf("[D3D9] " __VA_ARGS__); printf("\n"); } } while (0)
#else
#define LOG_SURFACE_CALL(counter, ...) do { } while (0)
#endif

static bool present_as_pixels() {
    static int mode = -1;

    if (mode < 0) {
        char text[32] = "";
        FILE* f = fopen("/Port/present.txt", "r");

        if (f) { if (!fgets(text, sizeof(text), f)) text[0] = 0; fclose(f); }
        mode = strncmp(text, "bitmap", 6) == 0 ? 0 : 1;
        printf("[D3D9] Frames are handed to the page as %s.\n", mode ? "pixels" : "image bitmaps");
    }
    return mode == 1;
}

extern "C" void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data);   // WebGL 2, not in the GLES 3 header

// The read-back is asynchronous: each frame is read into a pixel buffer (the commands only queue), and the frame that is handed to the page is the
// one requested two frames before, whose read-back the GPU has finished by now. (No fences: WebGL reports a sync object as signaled only
// in a later task, and the game thread never ends its task.) A synchronous glReadPixels waited 40-70 ms per frame for the GPU process.
static bool present_pixels_frame(int width, int height) {
    static const int RING = 3;
    static uint8_t* buffers[2];
    static size_t capacity;
    static int turn;
    static GLuint pixel_buffers[RING];
    static int sizes[RING][2];           // the size of the frame in each pixel buffer; 0 = none
    static size_t allocated[RING];       // bytes of each pixel buffer
    static int next;                     // the pixel buffer that gets the next frame; also the oldest one
    static unsigned int generation = ~0u;
    static std::vector<uint8_t> rows;
    size_t bytes = (size_t) width * height * 4;

    if (width <= 0 || height <= 0) return false;
    if (generation != g_gl_generation) {
        // a new context: the objects of the old one are gone
        generation = g_gl_generation;
        for (int i = 0; i < RING; i++) { pixel_buffers[i] = 0; sizes[i][0] = sizes[i][1] = 0; allocated[i] = 0; }
        glGenBuffers(RING, pixel_buffers);
    }
    if (bytes > capacity) {
        // the old buffers are not freed: the page may still read one
        buffers[0] = (uint8_t*) malloc(bytes);
        buffers[1] = (uint8_t*) malloc(bytes);
        capacity = bytes;
    }
#ifdef SRW_DIAGNOSTICS
    double readback_start = emscripten_get_now();
#endif
    // the oldest frame (requested RING - 1 frames ago) is fetched first: its pixel buffer is the one that is reused now
    int w = sizes[next][0], h = sizes[next][1];
    size_t frame_bytes = (size_t) w * h * 4;
    bool sent = false, have_old = w > 0;

    if (have_old && g_frames_in_flight < 2 && frame_bytes <= capacity) {
        uint8_t* out = buffers[turn ^= 1];

        rows.resize(frame_bytes);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_buffers[next]);
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, frame_bytes, rows.data());
        // rows flipped (every pixel is opaque already: the alpha was cleared to 1 on the GPU before the read, see below)
        for (int y = 0; y < h; y++) memcpy(out + (size_t) y * w * 4, rows.data() + (size_t) (h - 1 - y) * w * 4, (size_t) w * 4);
        sent = srw_present_pixels((int) (intptr_t) &g_frames_in_flight, (int) (intptr_t) out, w, h) != 0;
    }
    // this frame. Every pixel opaque first: the back buffer is X8R8G8B8 (a D3D9 draw reads its alpha as 1), but what the draws left in
    // the alpha channel reached the page's canvas (the movie's margins have alpha 0: they showed as bright lines at the edges of the
    // scaled canvas). A clear of the alpha channel alone does it on the GPU (it was a pass over every pixel here).
    {
        GLboolean mask[4];
        GLfloat clear[4];
        GLint draw_fb = 0;
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        if (scissor) glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) draw_fb);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_buffers[next]);
    if (allocated[next] != bytes) { glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ); allocated[next] = bytes; }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    sizes[next][0] = width; sizes[next][1] = height;
    next = (next + 1) % RING;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#ifdef SRW_DIAGNOSTICS
    g_perf_readback_ms += emscripten_get_now() - readback_start;
#endif
    // the first frames have nothing to show yet, and a frame that the page could not take is a skipped one
    return sent || !have_old;
}


#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Map D3DBLEND to GL enum
static GLenum d3d_blend_to_gl(DWORD blend) {
    switch (blend) {
        case D3DBLEND_ZERO: return GL_ZERO;
        case D3DBLEND_ONE: return GL_ONE;
        case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
        case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
        default: return GL_ONE; 
    }
}

static GLenum d3d_cmp_to_gl(DWORD func) {
    switch (func) {
        case D3DCMP_NEVER: return GL_NEVER;
        case D3DCMP_LESS: return GL_LESS;
        case D3DCMP_EQUAL: return GL_EQUAL;
        case D3DCMP_LESSEQUAL: return GL_LEQUAL;
        case D3DCMP_GREATER: return GL_GREATER;
        case D3DCMP_NOTEQUAL: return GL_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
        case D3DCMP_ALWAYS: return GL_ALWAYS;
        default: return GL_ALWAYS;
    }
}

static GLenum d3d_stencil_op_to_gl(DWORD op) {
    switch (op) {
        case D3DSTENCILOP_KEEP: return GL_KEEP;
        case D3DSTENCILOP_ZERO: return GL_ZERO;
        case D3DSTENCILOP_REPLACE: return GL_REPLACE;
        case D3DSTENCILOP_INCRSAT: return GL_INCR;
        case D3DSTENCILOP_DECRSAT: return GL_DECR;
        case D3DSTENCILOP_INVERT: return GL_INVERT;
        case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
        case D3DSTENCILOP_DECR: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

// WebGL context
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_webgl_context = 0;
// Every device has a WebGL context of its own (the program makes a new device when the resolution changes) and the objects belong to it.
// An object made for an earlier context must not delete anything: its numbers mean other things in the current one.
static const void* g_current_device = nullptr;

// debugging aid: /d3ddebug.txt (written by the page from the setting d3dDebug) holds a mode number, see settings.js
#ifdef SRW_DIAGNOSTICS
static int g_debug_mode = 0;
static volatile int g_debug_mode_request = 0;
#else
#define g_debug_mode 0   // the debug modes (.gfx command) exist only in the diagnostics build; the branches vanish
#endif
static bool g_skip_this_draw = false;       // set by the census for the draw that is about to be made
static GLint g_cached_viewport[4] = {0, 0, 0, 0};  // Overwritten by first SetViewport
static bool g_check_draw_error = false;
#ifdef SRW_DIAGNOSTICS
static volatile int g_hide_state = -1;      // .gfx hide N: the draws of the census state N are skipped (the states are numbered while mode 9 or hide is on)
extern "C" void SRW_D3DHideState(int state) { g_hide_state = state; }
static volatile int g_hide_fvf = -1;         // .gfx hidefvf HEX: the draws with this vertex format are skipped
extern "C" void SRW_D3DHideFvf(int fvf) { g_hide_fvf = fvf; }   // set by the sandbox command .gfx (another thread), taken over by the game thread at the next draw
extern "C" void SRW_D3DSetDebugMode(int mode) { g_debug_mode_request = mode; }
extern "C" int SRW_D3DDescribeGraphics(char* out, size_t size);
// (the chat command .gfx without argument asks for the mode: it puts the report of the graphics in the log too, category-free, until the command gets its own word)
extern "C" int SRW_D3DDebugMode(void) { return g_debug_mode_request; }
#else
#define g_hide_state (-1)
#define g_hide_fvf (-1)
#endif

// Experiments to find where the GPU time of a frame goes (setting d3dDebug): 10 draws nothing, 11 draws with rasterization discarded (only vertex work),
// 12 draws only the first triangle of each call (per-draw cost without the size of the draw)
// With the frame time chart on (.prof) the draws that take long are logged with their program, and whether the program had been drawn with before: WebGL
// implementations may compile and link a program only at its first draw, which would show as a draw of hundreds of milliseconds.
static unsigned char g_program_drawn[8192];
static void note_draw(double began, GLenum mode, GLsizei count) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    bool first = program >= 0 && program < 8192 && !g_program_drawn[program];
    double ms = emscripten_get_now() - began;
    if (program >= 0 && program < 8192) g_program_drawn[program] = 1;
    if (ms >= 25.0) {
        static int logged = 0;
        if (logged++ < 80) printf("[D3D9] slow draw: %.0f ms, mode %x, %d vertices, program %d%s\n", ms, (unsigned) mode, (int) count, (int) program, first ? " (its first draw)" : "");
    }
}
static void srw_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (g_debug_mode == 10 || g_skip_this_draw) return;
    if (g_debug_mode == 12 && count > 3) count = 3;
    if (g_debug_mode == 11) { glEnable(GL_RASTERIZER_DISCARD); glDrawArrays(mode, first, count); glDisable(GL_RASTERIZER_DISCARD); return; }
    if (SRW_ProfOn) { double began = emscripten_get_now(); glDrawArrays(mode, first, count); note_draw(began, mode, count); return; }
    glDrawArrays(mode, first, count);
}
static void srw_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_debug_mode == 10 || g_skip_this_draw) return;
    if (g_debug_mode == 12 && count > 3) count = 3;
    if (g_debug_mode == 11) { glEnable(GL_RASTERIZER_DISCARD); glDrawElements(mode, count, type, indices); glDisable(GL_RASTERIZER_DISCARD); return; }
    if (SRW_ProfOn) { double began = emscripten_get_now(); glDrawElements(mode, count, type, indices); note_draw(began, mode, count); return; }
    glDrawElements(mode, count, type, indices);
#ifdef SRW_DIAGNOSTICS
    if (g_check_draw_error && SRW_LOG_ON(SRW_LOG_D3D)) {   // the draws in a small viewport (interface models) ask WebGL for an error
        static int logged = 0;
        GLenum error = glGetError();
        if (error != GL_NO_ERROR && logged++ < 30) printf("[D3D9] GL error %x after glDrawElements (mode %x, %d indices, type %x)\n", error, mode, (int) count, type);
    }
#endif
}
#define glDrawArrays srw_glDrawArrays
#define glDrawElements srw_glDrawElements

// A cache of GL state in front of WebGL: the uniform values of every program (a glUniform with the value the location already has is
// skipped: the fixed function path sets ~40 uniforms per draw, most unchanged, and every call costs a trip into JavaScript where
// Emscripten also looks the location up in a table), the current program and the bound array/element buffers (no vertex array objects
// here: both bindings are global state). Everything is forgotten with the context (g_gl_generation), a program's values when it is
// linked again or deleted, a buffer binding when the buffer is deleted.
namespace glcache {
struct Program { std::vector<std::vector<uint32_t>> values; };   // by location: the last value (empty: unknown)
static std::map<GLuint, Program> programs;
static GLuint current_program = 0, array_buffer = 0, element_buffer = 0;
// textures: the active unit, what each unit has bound per target (2D, cube), the parameters of every texture object; the enabled attribute arrays
static GLenum active_unit = GL_TEXTURE0;
static GLuint bound_2d[32], bound_cube[32];
// by texture name (Emscripten's names are small consecutive numbers): wrap S, wrap T, mag filter, min filter (0x7fffffff: not known)
static std::vector<std::array<GLint, 4>> texture_params;
static int param_slot(GLenum pname) {
    return pname == GL_TEXTURE_WRAP_S ? 0 : pname == GL_TEXTURE_WRAP_T ? 1 : pname == GL_TEXTURE_MAG_FILTER ? 2 : pname == GL_TEXTURE_MIN_FILTER ? 3 : -1;
}
static void forget_texture(GLuint texture) {
    if (texture < texture_params.size()) texture_params[texture].fill(0x7fffffff);
}
static bool attrib_known[32], attrib_enabled[32];
static unsigned int generation = ~0u;
static bool enabled = true;
static int parts = 31;   // /glcache.txt: 1 uniforms, 2 the program, 4 the buffer bindings, 8 textures (unit, bindings, parameters), 16 attribute arrays (0: none). Keyed on the context: reset by g_gl_generation alone, a new context's binds were skipped (same names as the old one's) and Dalaran's loading screen stayed for good

static void check_generation() {
    static bool configured = false;
    if (!configured) {   // /glcache.txt 0: no cache (to compare)
        configured = true;
        FILE* f = fopen("/Port/glcache.txt", "r");
        if (f) { int v = 7; if (fscanf(f, "%d", &v) == 1) parts = v; fclose(f); }
        enabled = parts != 0;
        printf("[D3D9] GL state cache: %s%s%s%s%s\n", parts & 1 ? "uniforms " : "", parts & 2 ? "program " : "", parts & 4 ? "buffers " : "",
               parts & 8 ? "textures " : "", parts & 16 ? "attributes" : "");
    }
    static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = 0;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE now = emscripten_webgl_get_current_context();
    // (a new context is also seen before g_gl_generation counts it: its object names repeat those of the old one)
    if (generation != g_gl_generation || context != now) {
        generation = g_gl_generation;
        context = now;
        programs.clear();
        current_program = array_buffer = element_buffer = 0;
        active_unit = GL_TEXTURE0;
        memset(bound_2d, 0, sizeof(bound_2d));
        memset(bound_cube, 0, sizeof(bound_cube));
        texture_params.clear();
        memset(attrib_known, 0, sizeof(attrib_known));
    }
}
// true when the location of the current program already has these words (and they are noted as its value now)
static bool same(GLint location, const void* data, size_t words) {
    check_generation();
    if (!(parts & 1) || location < 0 || current_program == 0) return false;
    Program& p = programs[current_program];
    if ((size_t) location >= p.values.size()) {
        if (location > 4096) return false;
        p.values.resize((size_t) location + 1);
    }
    std::vector<uint32_t>& v = p.values[(size_t) location];
    if (v.size() == words && memcmp(v.data(), data, words * 4) == 0) return true;
    v.assign((const uint32_t*) data, (const uint32_t*) data + words);
    return false;
}
static void forget_program(GLuint program) { check_generation(); programs.erase(program); }
}

static void srw_glUseProgram(GLuint program) {
    glcache::check_generation();
    if ((glcache::parts & 2) && program == glcache::current_program && program != 0) return;
    glcache::current_program = program;
    glUseProgram(program);
}
static void srw_glLinkProgram(GLuint program) { glcache::forget_program(program); glLinkProgram(program); }
static void srw_glDeleteProgram(GLuint program) {
    glcache::forget_program(program);
    if (program == glcache::current_program) glcache::current_program = 0;   // (it stays in use until another one is, as GL says)
    glDeleteProgram(program);
}
static void srw_glBindBuffer(GLenum target, GLuint buffer) {
    glcache::check_generation();
    GLuint* bound = target == GL_ARRAY_BUFFER ? &glcache::array_buffer : target == GL_ELEMENT_ARRAY_BUFFER ? &glcache::element_buffer : nullptr;
    if (bound && (glcache::parts & 4) && *bound == buffer) return;
    if (bound) *bound = buffer;
    glBindBuffer(target, buffer);
}
static void cache_glDeleteBuffers(GLsizei n, const GLuint* buffers) {   // (in front of the counting srw_glDeleteBuffers above)
    glcache::check_generation();
    for (GLsizei i = 0; i < n; i++) {
        if (buffers[i] == glcache::array_buffer) glcache::array_buffer = 0;      // (deleting a bound buffer unbinds it)
        if (buffers[i] == glcache::element_buffer) glcache::element_buffer = 0;
    }
    glDeleteBuffers(n, buffers);
}
static void srw_glUniform1i(GLint l, GLint a) { if (!glcache::same(l, &a, 1)) glUniform1i(l, a); }
static void srw_glUniform1f(GLint l, GLfloat a) { if (!glcache::same(l, &a, 1)) glUniform1f(l, a); }
static void srw_glUniform2f(GLint l, GLfloat a, GLfloat b) { GLfloat v[2] = {a, b}; if (!glcache::same(l, v, 2)) glUniform2f(l, a, b); }
static void srw_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { GLfloat v[3] = {a, b, c}; if (!glcache::same(l, v, 3)) glUniform3f(l, a, b, c); }
static void srw_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { GLfloat v[4] = {a, b, c, d}; if (!glcache::same(l, v, 4)) glUniform4f(l, a, b, c, d); }
static void srw_glUniform1iv(GLint l, GLsizei n, const GLint* v) { if (!glcache::same(l, v, (size_t) n)) glUniform1iv(l, n, v); }
static void srw_glUniform1fv(GLint l, GLsizei n, const GLfloat* v) { if (!glcache::same(l, v, (size_t) n)) glUniform1fv(l, n, v); }
static void srw_glUniform3fv(GLint l, GLsizei n, const GLfloat* v) { if (!glcache::same(l, v, 3 * (size_t) n)) glUniform3fv(l, n, v); }
static void srw_glUniform4fv(GLint l, GLsizei n, const GLfloat* v) { if (!glcache::same(l, v, 4 * (size_t) n)) glUniform4fv(l, n, v); }
static void srw_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v) {
    if (t || !glcache::same(l, v, 16 * (size_t) n)) glUniformMatrix4fv(l, n, t, v);
}
static void srw_glActiveTexture(GLenum unit) {
    glcache::check_generation();
    if ((glcache::parts & 8) && unit == glcache::active_unit) return;
    glcache::active_unit = unit;
    glActiveTexture(unit);
}
static GLuint* bound_slot(GLenum target) {
    unsigned u = (unsigned) (glcache::active_unit - GL_TEXTURE0);
    if (u >= 32) return nullptr;
    return target == GL_TEXTURE_2D ? &glcache::bound_2d[u] : target == GL_TEXTURE_CUBE_MAP ? &glcache::bound_cube[u] : nullptr;
}
static void srw_glBindTexture(GLenum target, GLuint texture) {
    glcache::check_generation();
    GLuint* slot = bound_slot(target);
    if (slot && (glcache::parts & 8) && *slot == texture) return;
    if (slot) *slot = texture;
    glBindTexture(target, texture);
}
static void srw_glTexParameteri(GLenum target, GLenum pname, GLint value) {
    glcache::check_generation();
    GLuint* slot = bound_slot(target);
    int k = glcache::param_slot(pname);
    if (slot && *slot != 0 && k >= 0 && (glcache::parts & 8) && *slot < 1u << 20) {
        if (*slot >= glcache::texture_params.size()) {
            size_t old = glcache::texture_params.size();
            glcache::texture_params.resize((size_t) *slot + 1024);
            for (size_t i = old; i < glcache::texture_params.size(); i++) glcache::texture_params[i].fill(0x7fffffff);
        }
        GLint& known = glcache::texture_params[*slot][k];
        if (known == value) return;
        known = value;
    }
    glTexParameteri(target, pname, value);
}
static void cache_glDeleteTextures(GLsizei n, const GLuint* textures) {   // (in front of the counting srw_glDeleteTextures above)
    glcache::check_generation();
    for (GLsizei i = 0; i < n; i++) {
        glcache::forget_texture(textures[i]);
        for (int u = 0; u < 32; u++) {   // (deleting a bound texture unbinds it from every unit)
            if (glcache::bound_2d[u] == textures[i]) glcache::bound_2d[u] = 0;
            if (glcache::bound_cube[u] == textures[i]) glcache::bound_cube[u] = 0;
        }
    }
    glDeleteTextures(n, textures);
}
static void srw_glEnableVertexAttribArray(GLuint index) {
    if (index == (GLuint) -1) return;   // (an attribute the shader compiler left out: GL ignores it, Emscripten's emulation throws)
    glcache::check_generation();
    if (index < 32 && (glcache::parts & 16) && glcache::attrib_known[index] && glcache::attrib_enabled[index]) return;
    if (index < 32) { glcache::attrib_known[index] = true; glcache::attrib_enabled[index] = true; }
    glEnableVertexAttribArray(index);
}
static void srw_glDisableVertexAttribArray(GLuint index) {
    if (index == (GLuint) -1) return;   // (an attribute the shader compiler left out: GL ignores it, Emscripten's emulation throws)
    glcache::check_generation();
    if (index < 32 && (glcache::parts & 16) && glcache::attrib_known[index] && !glcache::attrib_enabled[index]) return;
    if (index < 32) { glcache::attrib_known[index] = true; glcache::attrib_enabled[index] = false; }
    glDisableVertexAttribArray(index);
}
static void srw_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer) {
    if (index == (GLuint) -1) return;   // (as in the two above)
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}
#define glActiveTexture srw_glActiveTexture
#define glBindTexture srw_glBindTexture
#define glTexParameteri srw_glTexParameteri
#undef glDeleteTextures
#define glDeleteTextures cache_glDeleteTextures
#define glEnableVertexAttribArray srw_glEnableVertexAttribArray
#define glVertexAttribPointer srw_glVertexAttribPointer
#define glDisableVertexAttribArray srw_glDisableVertexAttribArray
#define glUseProgram srw_glUseProgram
#define glLinkProgram srw_glLinkProgram
#define glDeleteProgram srw_glDeleteProgram
#define glBindBuffer srw_glBindBuffer
#undef glDeleteBuffers
#define glDeleteBuffers cache_glDeleteBuffers
#define glUniform1i srw_glUniform1i
#define glUniform1f srw_glUniform1f
#define glUniform2f srw_glUniform2f
#define glUniform3f srw_glUniform3f
#define glUniform4f srw_glUniform4f
#define glUniform1iv srw_glUniform1iv
#define glUniform1fv srw_glUniform1fv
#define glUniform3fv srw_glUniform3fv
#define glUniform4fv srw_glUniform4fv
#define glUniformMatrix4fv srw_glUniformMatrix4fv
#ifdef SRW_DIAGNOSTICS
static int read_debug_mode() {
    FILE* f = fopen("/Port/d3ddebug.txt", "r");
    int mode = 0;
    if (f) {
        if (fscanf(f, "%d", &mode) != 1) mode = 0;
        fclose(f);
    }
    return mode;
}
#endif

// Shader sources
static const char* vertex_shader_source = R"(
in vec4 a_position;
in vec3 a_normal;
in vec4 a_color;
in vec2 a_texcoord;
in vec2 a_texcoord1;
in vec2 a_texcoord2;
in vec2 a_texcoord3;
in vec2 a_texcoord4;
in vec2 a_texcoord5;
in vec2 a_texcoord6;
in vec2 a_texcoord7;
out vec4 v_color;
out vec2 v_tc0;
out vec2 v_tc1;
out vec2 v_tc2;
out vec2 v_tc3;
out vec2 v_tc4;
out vec2 v_tc5;
out vec2 v_tc6;
out vec2 v_tc7;
out vec4 v_position_world;

// the texture coordinates of the eight stages: from a set of the vertices or generated (Direct3D's D3DTSS_TCI_* flags), then the texture matrix
// source: 0 to 7 the sets of the vertices, 8 camera space normal, 9 camera space position, 10 reflection vector, 11 sphere map
uniform int u_tc_source[8];
uniform int u_tc_count[8];        // components the texture matrix works on, 0: no matrix
uniform int u_tc_projected[8];
uniform mat4 u_tc_matrix[8];

uniform mat4 u_mvp;
uniform mat4 u_world;
uniform bool u_fbo_flip_y;
uniform vec2 u_half_pixel;    // 1 / viewport size, or 0: Direct3D 9 puts pixel centers on integer coordinates, OpenGL half a pixel further

// Fog
uniform bool u_fog_enabled;
uniform mat4 u_view_model;
out float v_eye_depth;

// D3D9 FFP Lighting
uniform bool u_lighting_enabled;
uniform vec4 u_material_diffuse;
uniform vec4 u_material_ambient;
uniform vec3 u_global_ambient;

// Lights: the enabled ones, packed (Direct3D 9 fixed function model; spot lights are treated as point lights)
uniform int u_light_count;
uniform int u_light_type[8];               // 1 point, 2 spot, 3 directional
uniform vec3 u_light_position[8];
uniform vec3 u_light_direction[8];
uniform vec4 u_light_diffuse[8];
uniform vec3 u_light_ambient[8];
uniform vec3 u_light_attenuation[8];       // (att0, att1, att2)
uniform float u_light_range[8];
uniform vec3 u_material_emissive;
uniform vec3 u_color_from_vertex;   // 1.0 where the vertex color (COLOR1) replaces material diffuse / ambient / emissive

vec3 calcLight(int i, vec3 worldPos, vec3 worldNormal, vec4 mat_diffuse, vec4 mat_ambient) {
    vec3 light_dir;
    float atten = 1.0;
    if (u_light_type[i] == 3) {
        light_dir = normalize(-u_light_direction[i]);
    } else {
        vec3 light_vec = u_light_position[i] - worldPos;
        float dist = length(light_vec);
        if (u_light_range[i] > 0.0 && dist > u_light_range[i]) return vec3(0.0);
        light_dir = light_vec / max(dist, 0.000001);
        atten = 1.0 / max(u_light_attenuation[i].x + u_light_attenuation[i].y * dist + u_light_attenuation[i].z * dist * dist, 0.000001);
    }
    float NdotL = max(dot(worldNormal, light_dir), 0.0);
    return (mat_ambient.rgb * u_light_ambient[i]
          + mat_diffuse.rgb * u_light_diffuse[i].rgb * NdotL) * atten;
}

vec2 texCoordFor(int stage, vec3 cam_pos, vec3 cam_normal) {
    int source = u_tc_source[stage];
    vec4 coord;
    if (source == 0) coord = vec4(a_texcoord, 0.0, 1.0);
    else if (source == 1) coord = vec4(a_texcoord1, 0.0, 1.0);
    else if (source == 2) coord = vec4(a_texcoord2, 0.0, 1.0);
    else if (source == 3) coord = vec4(a_texcoord3, 0.0, 1.0);
    else if (source == 4) coord = vec4(a_texcoord4, 0.0, 1.0);
    else if (source == 5) coord = vec4(a_texcoord5, 0.0, 1.0);
    else if (source == 6) coord = vec4(a_texcoord6, 0.0, 1.0);
    else if (source == 7) coord = vec4(a_texcoord7, 0.0, 1.0);
    else if (source == 8) coord = vec4(cam_normal, 1.0);
    else if (source == 9) coord = vec4(cam_pos, 1.0);
    else {
        vec3 e = normalize(cam_pos);
        vec3 r = e - 2.0 * dot(cam_normal, e) * cam_normal;
        if (source == 10) {
            coord = vec4(r, 1.0);
        } else {
            float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
            coord = vec4(r.x / m + 0.5, -r.y / m + 0.5, 0.0, 1.0);
        }
    }
    if (u_tc_count[stage] > 0) {
        vec4 t = u_tc_matrix[stage] * coord;
        if (u_tc_projected[stage] != 0 && u_tc_count[stage] == 3) return t.xy / t.z;
        if (u_tc_projected[stage] != 0 && u_tc_count[stage] == 4) return t.xy / t.w;
        return t.xy;
    }
    return coord.xy;
}

void main() {
    // v_position_world is also computed for XYZRHW vertices (screen space, a_position.w = rhw): a clip plane with those is not valid
    v_position_world = u_world * a_position;
    gl_Position = u_mvp * a_position;
    if (u_fbo_flip_y) gl_Position.y = -gl_Position.y;
    gl_Position.xy += vec2(u_half_pixel.x, u_fbo_flip_y ? u_half_pixel.y : -u_half_pixel.y) * gl_Position.w;
    {
        vec3 cam_pos = (u_view_model * a_position).xyz;
        vec3 cam_normal = normalize(mat3(u_view_model) * a_normal);
        v_tc0 = texCoordFor(0, cam_pos, cam_normal);
        v_tc1 = texCoordFor(1, cam_pos, cam_normal);
        v_tc2 = texCoordFor(2, cam_pos, cam_normal);
        v_tc3 = texCoordFor(3, cam_pos, cam_normal);
        v_tc4 = texCoordFor(4, cam_pos, cam_normal);
        v_tc5 = texCoordFor(5, cam_pos, cam_normal);
        v_tc6 = texCoordFor(6, cam_pos, cam_normal);
        v_tc7 = texCoordFor(7, cam_pos, cam_normal);
    }

    if (u_lighting_enabled) {
        // D3D9 FFP per-vertex lighting formula
        vec3 world_pos = (u_world * a_position).xyz;
        vec3 world_normal = normalize(mat3(u_world) * a_normal);

        // Emissive + Material.Ambient * GlobalAmbient
        vec4 vertex_color = a_color.bgra;
        vec4 mat_diffuse = mix(u_material_diffuse, vertex_color, u_color_from_vertex.x);
        vec4 mat_ambient = mix(u_material_ambient, vertex_color, u_color_from_vertex.y);
        vec3 mat_emissive = mix(u_material_emissive, vertex_color.rgb, u_color_from_vertex.z);
        vec3 lit_color = mat_emissive + mat_ambient.rgb * u_global_ambient;

        for (int i = 0; i < 8; i++) {
            if (i < u_light_count) {
                lit_color += calcLight(i, world_pos, world_normal, mat_diffuse, mat_ambient);
            }
        }

        v_color = vec4(clamp(lit_color, 0.0, 1.0), mat_diffuse.a);
    } else {
        v_color = a_color.bgra;   // D3DCOLOR is 0xAARRGGBB: the bytes in memory are B, G, R, A
    }

    // Fog: pass eye-space Z to fragment shader for per-pixel fog (D3D9 table fog)
    if (u_fog_enabled) {
        vec4 view_pos = u_view_model * a_position;
        v_eye_depth = abs(view_pos.z);
    } else {
        v_eye_depth = 0.0;
    }
}
)";

static const char* fragment_shader_source = R"(
    in vec4 v_color;
    in vec2 v_tc0;
    in vec2 v_tc1;
    in vec2 v_tc2;
    in vec2 v_tc3;
    in vec2 v_tc4;
    in vec2 v_tc5;
    in vec2 v_tc6;
    in vec2 v_tc7;
    in vec4 v_position_world;
    uniform sampler2D u_texture;
    uniform bool u_clip_plane_enabled;
    uniform vec4 u_clip_plane[6];
    uniform float u_clip_on[6];   // 1.0 for the planes enabled by D3DRS_CLIPPLANEENABLE
    uniform sampler2D u_texture_lm;
    uniform sampler2D u_texture4;   // stages 4 to 7 (the pixel shaders)
    uniform sampler2D u_texture5;
    uniform sampler2D u_texture6;
    uniform sampler2D u_texture7;
    uniform sampler2D u_texture2;
    uniform sampler2D u_texture3;
    // the texture stage cascade of the fixed function pipeline, stages 0 to 3 (operations, arguments, texture coordinate set, texture bound)
    uniform int u_stage_colorop[4];
    uniform int u_stage_colorarg0[4];
    uniform int u_stage_colorarg1[4];
    uniform int u_stage_colorarg2[4];
    uniform int u_stage_alphaop[4];
    uniform int u_stage_alphaarg0[4];
    uniform int u_stage_alphaarg1[4];
    uniform int u_stage_alphaarg2[4];
    uniform int u_stage_result[4];
    uniform int u_stage_has_tex[4];
    uniform bool u_alpha_test_enabled;
    uniform bool u_alpha_to_coverage;   // the alpha test as coverage of the multisampled back buffer (GL_SAMPLE_ALPHA_TO_COVERAGE)
    uniform float u_alpha_ref;
    uniform int u_alpha_func;      // D3DCMP_*: 1 never, 2 less, 3 equal, 4 less or equal, 5 greater, 6 not equal, 7 greater or equal, 8 always
    uniform vec4 u_texture_factor;
    uniform float u_lod_bias;
    uniform int u_debug_mode;
    uniform int u_ps;              // the pixel shader of the program (ps_shaders.inc), 0: the fixed function cascade
    uniform int u_debug_target;   // 1 for the draws that modes 18 and 19 are about: lit, blended, with vertex colors (the water)

    // Fog (per-pixel, matches D3D9 table fog)
    uniform bool u_fog_enabled;
    uniform float u_fog_start;
    uniform float u_fog_end;
    uniform vec3 u_fog_color;
    in float v_eye_depth;

    // an argument of a stage: D3DTA_DIFFUSE 0, CURRENT 1, TEXTURE 2, TFACTOR 3, SPECULAR 4, TEMP 5; with D3DTA_COMPLEMENT (0x10) and D3DTA_ALPHAREPLICATE (0x20)
    vec4 stageArg(int arg, vec4 current, vec4 temp, vec4 tex) {
        int source = arg - (arg / 8) * 8;
        int flags = arg / 16;
        vec4 v = current;
        if (source == 0) v = v_color;
        else if (source == 2) v = tex;
        else if (source == 3) v = u_texture_factor;
        else if (source == 4) v = vec4(0.0);
        else if (source == 5) v = temp;
        if (flags == 1 || flags == 3) v = vec4(1.0) - v;
        if (flags >= 2) v = vec4(v.a);
        return v;
    }

    // D3DTOP_*: the same operation for the color (rgb) and the alpha (a); the blends take their factor from blend
    vec4 stageOp(int op, vec4 a0, vec4 a1, vec4 a2, float blend) {
        vec4 r = a1;
        if (op == 2) r = a1;
        else if (op == 3) r = a2;
        else if (op == 4 || op == 17) r = a1 * a2;
        else if (op == 5) r = a1 * a2 * 2.0;
        else if (op == 6) r = a1 * a2 * 4.0;
        else if (op == 7) r = a1 + a2;
        else if (op == 8) r = a1 + a2 - 0.5;
        else if (op == 9) r = (a1 + a2 - 0.5) * 2.0;
        else if (op == 10) r = a1 - a2;
        else if (op == 11) r = a1 + a2 - a1 * a2;
        else if (op == 12 || op == 13 || op == 14 || op == 16) r = a1 * blend + a2 * (1.0 - blend);
        else if (op == 15) r = a1 + a2 * (1.0 - blend);
        else if (op == 18) r = vec4(a1.rgb + a1.a * a2.rgb, a1.a);
        else if (op == 19) r = vec4(a1.rgb * a2.rgb + a1.a, a1.a * a2.a);
        else if (op == 20) r = vec4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);
        else if (op == 21) r = vec4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);
        else if (op == 24) { float d = 4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5); r = vec4(d); }
        else if (op == 25) r = a0 + a1 * a2;
        else if (op == 26) r = a0 * a1 + (1.0 - a0) * a2;
        return clamp(r, 0.0, 1.0);
    }

    void main() {
#ifdef SRW_DEBUG_MODES
        // debug mode 2 answers at once: a test of how much the rest of this shader costs
        if (u_debug_mode == 2) {
            srw_FragColor = vec4(1.0, 0.5, 0.0, 1.0);
            return;
        }
#endif
        // Clip plane test (shader-based, replaces hardware clip planes for WebGL)
        if (u_clip_plane_enabled) {
            for (int i = 0; i < 6; i++) {
                if (u_clip_on[i] > 0.5) {
                    if (dot(v_position_world.xyz, u_clip_plane[i].xyz) + u_clip_plane[i].w < 0.0) discard;
                }
            }
        }

        // the textures of the stages, each with the coordinates its stage asks for
        vec4 tex0 = vec4(1.0), tex1 = vec4(1.0), tex2 = vec4(1.0), tex3 = vec4(1.0);
        if (u_stage_has_tex[0] != 0) tex0 = texture2D(u_texture, v_tc0, u_lod_bias);
        if (u_stage_has_tex[1] != 0) tex1 = texture2D(u_texture_lm, v_tc1, u_lod_bias);
        if (u_stage_has_tex[2] != 0) tex2 = texture2D(u_texture2, v_tc2, u_lod_bias);
        if (u_stage_has_tex[3] != 0) tex3 = texture2D(u_texture3, v_tc3, u_lod_bias);

        // the cascade: every stage combines its arguments and hands the result to the next; a disabled stage ends it
        vec4 current = v_color;
        vec4 temp = vec4(0.0);
        for (int i = 0; i < 4; i++) {
            int color_op = u_stage_colorop[i];
            int alpha_op = u_stage_alphaop[i];
            vec4 tex = tex0;
            if (i == 1) tex = tex1;
            else if (i == 2) tex = tex2;
            else if (i == 3) tex = tex3;
            if (color_op == 1) break;

            float color_blend = tex.a;
            if (color_op == 12) color_blend = v_color.a;
            else if (color_op == 14) color_blend = u_texture_factor.a;
            else if (color_op == 16) color_blend = current.a;
            vec4 color_result = stageOp(color_op, stageArg(u_stage_colorarg0[i], current, temp, tex), stageArg(u_stage_colorarg1[i], current, temp, tex),
                                        stageArg(u_stage_colorarg2[i], current, temp, tex), color_blend);

            float alpha_blend = tex.a;
            if (alpha_op == 12) alpha_blend = v_color.a;
            else if (alpha_op == 14) alpha_blend = u_texture_factor.a;
            else if (alpha_op == 16) alpha_blend = current.a;
            vec4 alpha_result = current;
            if (alpha_op != 1) {
                alpha_result = stageOp(alpha_op, stageArg(u_stage_alphaarg0[i], current, temp, tex), stageArg(u_stage_alphaarg1[i], current, temp, tex),
                                       stageArg(u_stage_alphaarg2[i], current, temp, tex), alpha_blend);
            }
            vec4 result = vec4(color_result.rgb, alpha_result.a);
            if (u_stage_result[i] == 5) temp = result;
            else current = result;
        }
        vec4 tex_color = tex0;
        vec4 final_color = current;
#ifdef SRW_PS
        // the program of a pixel shader (ps_shaders.inc): the function of that shader replaces the cascade
        final_color = SRW_PS_CALL(v_color, vec4(0.0), vec4(v_tc0, 0.0, 1.0), vec4(v_tc1, 0.0, 1.0), vec4(v_tc2, 0.0, 1.0), vec4(v_tc3, 0.0, 1.0), vec4(v_tc4, 0.0, 1.0), vec4(v_tc5, 0.0, 1.0), vec4(v_tc6, 0.0, 1.0), vec4(v_tc7, 0.0, 1.0));
#endif

        if (u_alpha_test_enabled) {
            // Direct3D compares the 8 bit alpha with the reference value
            float a = floor(final_color.a * 255.0 + 0.5);
            float r = floor(u_alpha_ref + 0.5);
            bool pass = true;
            if (u_alpha_func == 1) pass = false;
            else if (u_alpha_func == 2) pass = a < r;
            else if (u_alpha_func == 3) pass = a == r;
            else if (u_alpha_func == 4) pass = a <= r;
            else if (u_alpha_func == 5) pass = a > r;
            else if (u_alpha_func == 6) pass = a != r;
            else if (u_alpha_func == 7) pass = a >= r;
            if (u_alpha_to_coverage) {
                // the edge of the test as a ramp about a pixel wide around the reference: the samples of the multisampled back buffer
                // covered in proportion (the cut-out edges of leaves, fences and hair antialiased); the rest as the test would decide
                float width = max(fwidth(final_color.a * 255.0), 1.0);
                float coverage = clamp((final_color.a * 255.0 - r) / width + 0.5, 0.0, 1.0);
                if (coverage <= 0.0) discard;
                final_color.a = coverage;
            } else if (!pass) discard;
        }

        // Apply fog (per-pixel linear fog, matches D3D9 table fog)
        if (u_fog_enabled) {
            float fog_factor = clamp((u_fog_end - v_eye_depth) / (u_fog_end - u_fog_start), 0.0, 1.0);
            final_color.rgb = mix(u_fog_color, final_color.rgb, fog_factor);
        }

        srw_FragColor = final_color;

#ifdef SRW_DEBUG_MODES
        // debugging aids (the .gfx command, the setting d3dDebug of the page); only in the diagnostics build
        if (u_debug_mode == 2) {
            srw_FragColor = vec4(1.0, 0.5, 0.0, 1.0);
        } else if (u_debug_mode == 3) {
            srw_FragColor = vec4(v_color.rgb, 1.0);
        } else if (u_debug_mode == 4) {
            srw_FragColor = vec4(tex_color.rgb, 1.0);
        } else if (u_debug_mode == 14) {
            srw_FragColor = vec4(vec3(v_color.a), 1.0);        // the alpha of the vertex (lighting result)
        } else if (u_debug_mode == 15) {
            srw_FragColor = vec4(vec3(tex0.a), 1.0);           // the alpha of the texture of stage 0
        } else if (u_debug_mode == 16) {
            srw_FragColor = vec4(vec3(tex1.a), 1.0);           // the alpha of the texture of stage 1
        } else if (u_debug_mode == 17) {
            srw_FragColor = vec4(vec3(final_color.a), 1.0);    // the alpha that goes to the blend
        } else if (u_debug_mode == 18) {
            // translucent pixels (alpha below 1) red, with their real alpha: the blend then shows the scene through the red, the rest is normal
            if (u_debug_target != 0 && final_color.a < 0.98) srw_FragColor = vec4(1.0, 0.0, 0.0, final_color.a);
        } else if (u_debug_mode == 19) {
            // the same with a fixed alpha of one half: a test of the blend that does not depend on the values of the alpha
            if (u_debug_target != 0 && final_color.a < 0.98) srw_FragColor = vec4(1.0, 0.0, 0.0, 0.5);
        }
#endif
    }
)";

// Shader program
static GLuint g_shader_program = 0;
static GLint g_position_location = -1;
static GLint g_color_location = -1;
static GLint g_texcoord_location = -1;
static GLint g_texcoord1_location = -1;
static GLint g_texture_uniform = -1;
static GLint g_texture_lm_uniform = -1;
static GLint g_use_texture_uniform = -1;
static GLint g_use_lightmap_uniform = -1;
static GLint g_mvp_uniform = -1;
static GLint g_tex_matrix_uniform = -1;
static GLint g_use_tex_matrix_uniform = -1;
static GLint g_clip_plane_uniform = -1;
static GLint g_clip_plane_enabled_uniform = -1;
static GLint g_fbo_flip_y_uniform = -1;
static GLint g_stage0_colorop_uniform = -1;
static GLint g_stage0_colorarg1_uniform = -1;
static GLint g_stage0_colorarg2_uniform = -1;
static GLint g_stage0_alphaop_uniform = -1;
static GLint g_stage0_alphaarg1_uniform = -1;
static GLint g_stage0_alphaarg2_uniform = -1;
static GLint g_stage1_op_uniform = -1;
// the texture stage cascade (arrays of 4) and the samplers of stages 2 and 3
static GLint g_stage_array_uniform[11] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static GLint g_texture2_uniform = -1, g_texture3_uniform = -1, g_texture4_uniform = -1, g_texture5_uniform = -1, g_texture6_uniform = -1, g_texture7_uniform = -1;
static GLint g_texcoord_locs[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };   // the attributes of the eight texture coordinate sets
static GLint g_tc_source_uniform = -1, g_tc_count_uniform = -1, g_tc_projected_uniform = -1, g_tc_matrix_uniform = -1;
static GLint g_alpha_test_enabled_uniform = -1;
static GLint g_alpha_to_coverage_uniform = -1;
static bool g_alpha_to_coverage_on = false;   // GL_SAMPLE_ALPHA_TO_COVERAGE (state of the current context)

// the setting d3dAlphaCoverage (/d3dalphacoverage.txt, default 1): alpha tested draws antialiased with multisampling
static bool alpha_coverage_wanted() {
    static int wanted = -1;
    if (wanted < 0) {
        FILE* f = fopen("/Port/d3dalphacoverage.txt", "r");
        wanted = 1;
        if (f) {
            if (fscanf(f, "%d", &wanted) != 1) wanted = 1;
            fclose(f);
        }
        wanted = wanted != 0;
    }
    return wanted != 0;
}
static GLint g_alpha_ref_uniform = -1;
static GLint g_alpha_func_uniform = -1;
static GLint g_clip_on_uniform = -1;
static GLint g_ps_uniform = -1, g_ps_c_uniform = -1, g_half_pixel_uniform = -1;
static volatile int g_half_pixel_enabled = 1;   // .gfx halfpixel 0|1
#ifdef SRW_DIAGNOSTICS
extern "C" void SRW_D3DHalfPixel(int on) { g_half_pixel_enabled = on; }
#endif
static int g_current_ps_id = 0;               // the pixel shader set by the game (an id of ps_shaders.inc), 0: none or not translated
static float g_ps_constants[32 * 4];          // SetPixelShaderConstantF
static GLint g_texture_factor_uniform = -1;
static GLint g_lod_bias_uniform = -1;

// Fog uniforms
static GLint g_fog_enabled_uniform = -1;
static GLint g_fog_start_uniform = -1;
static GLint g_fog_end_uniform = -1;
static GLint g_fog_color_uniform = -1;
static GLint g_view_model_uniform = -1;

// FFP Lighting uniforms
static GLint g_normal_location = -1;
static GLint g_world_uniform = -1;
static GLint g_lighting_enabled_uniform = -1;
static GLint g_material_diffuse_uniform = -1;
static GLint g_material_ambient_uniform = -1;
static GLint g_material_emissive_uniform = -1;
static GLint g_color_from_vertex_uniform = -1;
static GLint g_light_count_uniform = -1;
static GLint g_light_type_uniform = -1;
static GLint g_light_position_uniform = -1;
static GLint g_light_direction_uniform = -1;
static GLint g_light_diffuse_uniform = -1;
static GLint g_light_ambient_uniform = -1;
static GLint g_light_attenuation_uniform = -1;
static GLint g_light_range_uniform = -1;
static GLint g_global_ambient_uniform = -1;
static GLint g_light0_enabled_uniform = -1;
static GLint g_light0_position_uniform = -1;
static GLint g_light0_diffuse_uniform = -1;
static GLint g_light0_ambient_uniform = -1;
static GLint g_light0_attenuation_uniform = -1;
static GLint g_light0_range_uniform = -1;
static GLint g_light1_enabled_uniform = -1;
static GLint g_light1_position_uniform = -1;
static GLint g_light1_diffuse_uniform = -1;
static GLint g_light1_ambient_uniform = -1;
static GLint g_light1_attenuation_uniform = -1;
static GLint g_light1_range_uniform = -1;
static GLint g_light2_enabled_uniform = -1;
static GLint g_light2_position_uniform = -1;
static GLint g_light2_diffuse_uniform = -1;
static GLint g_light2_ambient_uniform = -1;
static GLint g_light2_attenuation_uniform = -1;
static GLint g_light2_range_uniform = -1;


// One program for each pixel shader the game uses (and the base program for the fixed function pipeline): the locations of the uniforms of
// the program in use are in the variables g_*_uniform, and a program switch replaces them all
#define SRW_LOCATION_VARIABLES(X) \
    X(g_alpha_func_uniform) \
    X(g_alpha_ref_uniform) \
    X(g_alpha_test_enabled_uniform) \
    X(g_alpha_to_coverage_uniform) \
    X(g_clip_on_uniform) \
    X(g_clip_plane_enabled_uniform) \
    X(g_clip_plane_uniform) \
    X(g_color_from_vertex_uniform) \
    X(g_color_location) \
    X(g_fbo_flip_y_uniform) \
    X(g_fog_color_uniform) \
    X(g_fog_enabled_uniform) \
    X(g_fog_end_uniform) \
    X(g_fog_start_uniform) \
    X(g_global_ambient_uniform) \
    X(g_half_pixel_uniform) \
    X(g_light0_ambient_uniform) \
    X(g_light0_attenuation_uniform) \
    X(g_light0_diffuse_uniform) \
    X(g_light0_enabled_uniform) \
    X(g_light0_position_uniform) \
    X(g_light0_range_uniform) \
    X(g_light1_ambient_uniform) \
    X(g_light1_attenuation_uniform) \
    X(g_light1_diffuse_uniform) \
    X(g_light1_enabled_uniform) \
    X(g_light1_position_uniform) \
    X(g_light1_range_uniform) \
    X(g_light2_ambient_uniform) \
    X(g_light2_attenuation_uniform) \
    X(g_light2_diffuse_uniform) \
    X(g_light2_enabled_uniform) \
    X(g_light2_position_uniform) \
    X(g_light2_range_uniform) \
    X(g_light_ambient_uniform) \
    X(g_light_attenuation_uniform) \
    X(g_light_count_uniform) \
    X(g_light_diffuse_uniform) \
    X(g_light_direction_uniform) \
    X(g_light_position_uniform) \
    X(g_light_range_uniform) \
    X(g_light_type_uniform) \
    X(g_lighting_enabled_uniform) \
    X(g_lod_bias_uniform) \
    X(g_material_ambient_uniform) \
    X(g_material_diffuse_uniform) \
    X(g_material_emissive_uniform) \
    X(g_mvp_uniform) \
    X(g_normal_location) \
    X(g_position_location) \
    X(g_ps_c_uniform) \
    X(g_ps_uniform) \
    X(g_stage0_alphaarg1_uniform) \
    X(g_stage0_alphaarg2_uniform) \
    X(g_stage0_alphaop_uniform) \
    X(g_stage0_colorarg1_uniform) \
    X(g_stage0_colorarg2_uniform) \
    X(g_stage0_colorop_uniform) \
    X(g_stage1_op_uniform) \
    X(g_tc_count_uniform) \
    X(g_tc_matrix_uniform) \
    X(g_tc_projected_uniform) \
    X(g_tc_source_uniform) \
    X(g_tex_matrix_uniform) \
    X(g_texcoord1_location) \
    X(g_texcoord_location) \
    X(g_texture2_uniform) \
    X(g_texture3_uniform) \
    X(g_texture4_uniform) \
    X(g_texture5_uniform) \
    X(g_texture6_uniform) \
    X(g_texture7_uniform) \
    X(g_texture_factor_uniform) \
    X(g_texture_lm_uniform) \
    X(g_texture_uniform) \
    X(g_use_lightmap_uniform) \
    X(g_use_tex_matrix_uniform) \
    X(g_use_texture_uniform) \
    X(g_view_model_uniform) \
    X(g_world_uniform)

struct ProgramLocations {
    GLint value[81];
    GLint stage_array[11];
    GLint texcoords[8];
};
static void save_locations(ProgramLocations& out) {
    int i = 0;
#define X(n) out.value[i++] = n;
    SRW_LOCATION_VARIABLES(X)
#undef X
    memcpy(out.stage_array, g_stage_array_uniform, sizeof(out.stage_array));
    memcpy(out.texcoords, g_texcoord_locs, sizeof(out.texcoords));
}
static void load_locations(const ProgramLocations& in) {
    int i = 0;
#define X(n) n = in.value[i++];
    SRW_LOCATION_VARIABLES(X)
#undef X
    memcpy(g_stage_array_uniform, in.stage_array, sizeof(in.stage_array));
    memcpy(g_texcoord_locs, in.texcoords, sizeof(in.texcoords));
}

// Current FVF
static DWORD g_current_fvf = 0;

// The texture coordinate sets of a vertex format (eight, each a vec2 attribute: the first two components of sets of 1 to 4 floats)
static size_t fvf_tex_bytes(DWORD fvf, int set) {
    switch ((fvf >> (16 + 2 * set)) & 3) { case 0: return 8; case 1: return 12; case 2: return 16; default: return 4; }
}
static size_t fvf_tex_total(DWORD fvf) {
    size_t total = 0;
    for (int i = 0; i < (int) ((fvf >> 8) & 0xF); i++) total += fvf_tex_bytes(fvf, i);
    return total;
}
static void bind_texcoords(DWORD fvf, GLsizei stride, size_t first_offset) {
    int count = (int) ((fvf >> 8) & 0xF);
    size_t offset = first_offset;
    for (int i = 0; i < 8; i++) {
        GLint location = g_texcoord_locs[i];
        size_t bytes = i < count ? fvf_tex_bytes(fvf, i) : 0;
        if (location != -1) {
            if (i < count) {
                glEnableVertexAttribArray(location);
                glVertexAttribPointer(location, bytes == 4 ? 1 : 2, GL_FLOAT, GL_FALSE, stride, (void*) offset);
            } else {
                glDisableVertexAttribArray(location);
                glVertexAttrib2f(location, 0.0f, 0.0f);
            }
        }
        offset += bytes;
    }
}
static void unbind_texcoords() {
    for (int i = 0; i < 8; i++) if (g_texcoord_locs[i] != -1) glDisableVertexAttribArray(g_texcoord_locs[i]);
}


// Current texture
static GLuint g_texture_stages[8] = { 0 };
static GLuint g_last_bound_texture_id = 0xFFFFFFFF;
static GLuint g_last_bound_lightmap_id = 0xFFFFFFFF;

// Track which GL textures have a complete mipmap chain
static std::unordered_set<GLuint> g_mipmapped_textures;
struct TextureInfo { unsigned int width, height, levels, format; };
static std::map<GLuint, TextureInfo> g_texture_info;   // for the draw state census (d3dDebug 9)

// Anisotropic filtering (EXT_texture_filter_anisotropic)
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
static float g_max_anisotropy = 1.0f; // 1.0 = disabled (no extension)

// FFP State storage
static D3DLIGHT9 g_lights[8];
static bool g_light_enabled[8] = {false};
static D3DMATERIAL9 g_material;
static DWORD g_current_stencil_ref = 0;

static void init_shaders();
static void EnsureUPBuffers();
static GLuint g_up_vbo = 0;
static GLuint g_up_ibo = 0;
static size_t g_up_vbo_size = 0;  // Track VBO size for persistent buffer optimization
static size_t g_up_ibo_size = 0;  // Track IBO size for persistent buffer optimization

// State caching: g_cached_texture and g_last_bound_texture_id are the same variable, shared by DrawIndexedPrimitive and the UP draws
static GLuint& g_cached_texture = g_last_bound_texture_id;
static GLuint g_cached_program = 0xFFFFFFFF;  // Last used shader program
static void remember_program_switch() { g_cached_program = g_shader_program; }
static DWORD g_cached_fvf = 0xFFFFFFFF;       // Last FVF
static bool g_cached_vertex_attribs[4] = {false};  // pos, color, tex0, tex1

// Viewport and Scissor state caching (to avoid glGet* calls)
// (g_cached_viewport is defined further up)
static GLboolean g_cached_scissor_test = GL_FALSE;
static GLint g_cached_scissor_box[4] = {0, 0, 0, 0};  // Overwritten by first SetScissorRect

static DWORD g_texture_stage_states[8][33];
static D3DVIEWPORT9 g_viewport;
static int g_logical_width = 0;    // App's backbuffer resolution (set from D3DPRESENT_PARAMETERS)
static int g_logical_height = 0;

// Texture matrices
static D3DMATRIX g_texture_matrices[8];
// Sampler states [Sampler][Type]
static DWORD g_sampler_states[8][14]; // Up to D3DSAMP_DMAPOFFSET

// Transform matrices (for GetTransform)
D3DMATRIX g_transform_world;
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

// GLI library maps RGB DXT1 to a custom FOURCC 'GLI1'; treat like DXT1
#define D3DFMT_GLI1 ((D3DFORMAT)0x31494C47)

// Helper to check for DXT format
static bool IsCompressedFormat(D3DFORMAT format) {
    return (format == D3DFMT_DXT1 || format == D3DFMT_DXT3 || format == D3DFMT_DXT5
            || format == D3DFMT_GLI1);
}

static UINT GetFormatBpp(D3DFORMAT format) {
    switch (format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
            return 32;
        case D3DFMT_R8G8B8:
            return 24;
        case D3DFMT_R5G6B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_A1R5G5B5:
        case (D3DFORMAT) 24:  // D3DFMT_X1R5G5B5
        case D3DFMT_INDEX16:
            return 16;
        default:
            return 32;
    }
}

// Convert D3D DXT format to GL extension enum
// D3D9 DXT1 has 1-bit punch-through alpha (a block with c0 <= c1 makes index 3 transparent black), and games use it for foliage, icon
// borders and slot art. The RGB variant is only defined to decode those texels as OPAQUE black: some browsers/drivers (ANGLE D3D11)
// let the BC1 alpha through anyway, others (strict ones) give black boxes. The RGBA variant is the same everywhere.
GLenum GetGLCompressedFormat(D3DFORMAT fmt) {
    switch (fmt) {
        case D3DFMT_DXT1: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        case D3DFMT_GLI1: return GL_COMPRESSED_RGB_S3TC_DXT1_EXT; // GLI's RGB DXT1
        case D3DFMT_DXT3: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        case D3DFMT_DXT5: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        default: return 0;
    }
}


// Software DXT1/3/5 decoder, for GPUs without WEBGL_compressed_texture_s3tc (most phones: Adreno, Mali, PowerVR). Output is RGBA8.
static bool g_s3tc_available = true;
static void DecodeDXTToRGBA(D3DFORMAT fmt, const uint8_t* src, UINT w, UINT h, uint32_t* dst) {
    bool dxt1 = (fmt == D3DFMT_DXT1 || fmt == D3DFMT_GLI1);
    size_t bs = dxt1 ? 8 : 16;
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (UINT by = 0; by < bh; by++) for (UINT bx = 0; bx < bw; bx++) {
        const uint8_t* b = src + ((size_t) by * bw + bx) * bs;
        const uint8_t* c = dxt1 ? b : b + 8;
        unsigned c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
        uint8_t col[4][4];
        auto expand = [](unsigned v, uint8_t* o) { o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; };
        expand(c0, col[0]); expand(c1, col[1]);
        col[0][3] = col[1][3] = col[2][3] = col[3][3] = 255;
        if (c0 > c1 || !dxt1) {
            for (int i = 0; i < 3; i++) { col[2][i] = (2 * col[0][i] + col[1][i]) / 3; col[3][i] = (col[0][i] + 2 * col[1][i]) / 3; }
        } else {
            for (int i = 0; i < 3; i++) { col[2][i] = (col[0][i] + col[1][i]) / 2; col[3][i] = 0; }
            col[3][3] = 0;
        }
        uint32_t bits = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t) c[7] << 24);
        uint8_t alpha[16];
        if (fmt == D3DFMT_DXT3) {
            for (int i = 0; i < 16; i++) { unsigned n = (b[i / 2] >> ((i & 1) * 4)) & 15; alpha[i] = n * 17; }
        } else if (fmt == D3DFMT_DXT5) {
            uint8_t a[8]; a[0] = b[0]; a[1] = b[1];
            if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
            else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
            uint64_t ab = 0; for (int i = 0; i < 6; i++) ab |= (uint64_t) b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) alpha[i] = a[(ab >> (3 * i)) & 7];
        }
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
            UINT x = bx * 4 + px, y = by * 4 + py;
            if (x >= w || y >= h) continue;
            int i = py * 4 + px;
            const uint8_t* k = col[(bits >> (2 * i)) & 3];
            unsigned A = (fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT5) ? alpha[i] : k[3];
            dst[(size_t) y * w + x] = k[0] | (k[1] << 8) | (k[2] << 16) | (A << 24);
        }
    }
}


// Debugging helper
#ifdef DEBUG_TEXTURE_TEST
#define CHECK_GL_ERROR(op) { op; CheckGLError(#op); }
#else
#define CHECK_GL_ERROR(op) op
#endif

// Correct CheckGLError implementation
void CheckGLError(const char* label);

// Manual UP buffers (Moved up for RestoreContext)

void EnsureUPBuffers() {
    if (g_up_vbo == 0) {
        glGenBuffers(1, &g_up_vbo);
    }
    if (g_up_ibo == 0) {
        glGenBuffers(1, &g_up_ibo);
    }
}

// Check for OpenGL errors (silent in release builds)
void CheckGLError(const char* label) {
    GLenum err = glGetError();
    while (err != GL_NO_ERROR) {
        // Silently consume errors
        err = glGetError();
    }
}


static D3DMATRIX g_transform_view;
static D3DMATRIX g_transform_projection;

// The shaders are GLSL ES 3.00, the dialect of WebGL 2 (derivatives, fwidth, need nothing there; in ES 1.00 they need an extension that a WebGL 2
// context does not offer). This header goes first in every source: the version line, and for the fragment shaders the default precision, the
// output and the old name of the texture function, so that the translated pixel shaders (ps_shaders.inc, made by tools/make_ps_shaders.py
// with texture2D) stay as they are. The vertex and fragment sources use in / out themselves.
static const char shader_header_vertex[] = "#version 300 es\n";
static const char shader_header_fragment[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "layout(location = 0) out vec4 srw_FragColor;\n"
    "#define texture2D texture\n";

// Helper: Compile shader
static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    const std::string full = std::string(type == GL_VERTEX_SHADER ? shader_header_vertex : shader_header_fragment) + source;
    const char* sources[1] = { full.c_str() };
    glShaderSource(shader, 1, sources, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[1024];
        glGetShaderInfoLog(shader, sizeof(info_log), nullptr, info_log);
        printf("Shader compilation failed (%s shader): %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", info_log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Helper: Initialize shaders
// The vertex shader object of all programs
static GLuint g_vertex_shader_object = 0;

// One program of the wrapper: ps_id 0 draws with the fixed function pipeline (the cascade of stages), another id (ps_shaders.inc) with the
// translated pixel shader of the program. The fragment shader of such a program has that shader's function only: all of them in one
// program (a table of 60 shaders compiles slowly and is branched through on every pixel; some programs have more than 300.
struct Program { GLuint program; ProgramLocations locations; bool failed; };
static std::map<int, Program> g_programs;

static bool build_program(int ps_id, Program& out) {
    ProfScope prof_(PROF_SHADER_);
    std::string fragment_source(fragment_shader_source);
    out.program = 0;
    out.failed = true;
    if (g_vertex_shader_object == 0) g_vertex_shader_object = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint vs = g_vertex_shader_object;
#ifdef SRW_DIAGNOSTICS
    fragment_source = "#define SRW_DEBUG_MODES\n" + fragment_source;   // the debug modes of the .gfx command
#endif
    if (ps_id != 0) {
        const KnownPixelShader* known = nullptr;
        for (size_t i = 0; i < sizeof(known_pixel_shaders) / sizeof(*known_pixel_shaders); i++) {
            if (known_pixel_shaders[i].id == ps_id) known = &known_pixel_shaders[i];
        }
        if (known == nullptr) return false;
        char define[96];
        snprintf(define, sizeof(define), "#define SRW_PS 1\n#define SRW_PS_CALL ps_%d\nuniform vec4 u_ps_c[32];\n", ps_id);
        size_t main_position = fragment_source.rfind("void main()");
        if (main_position == std::string::npos) return false;
        // (the function and the declaration of its constants go in front of main(); the macros come first in the source)
        fragment_source.insert(main_position, std::string(define) + known->source + "\n");
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source.c_str());
    if (vs == 0 || fs == 0) return false;

    g_shader_program = glCreateProgram();
    glAttachShader(g_shader_program, vs);
    glAttachShader(g_shader_program, fs);
    glLinkProgram(g_shader_program);

    GLint success;
    glGetProgramiv(g_shader_program, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(g_shader_program, 512, nullptr, info_log);
        printf("Shader linking failed (pixel shader %d): %s\n", ps_id, info_log);
        glDeleteProgram(g_shader_program);
        glDeleteShader(fs);
        return false;
    }

    g_position_location = glGetAttribLocation(g_shader_program, "a_position");
    g_normal_location = glGetAttribLocation(g_shader_program, "a_normal");
    g_color_location = glGetAttribLocation(g_shader_program, "a_color");
    for (int i = 0; i < 8; i++) {
        char name[24];
        snprintf(name, sizeof(name), i == 0 ? "a_texcoord" : "a_texcoord%d", i);
        g_texcoord_locs[i] = glGetAttribLocation(g_shader_program, name);
    }
    g_texcoord_location = g_texcoord_locs[0];
    g_texcoord1_location = g_texcoord_locs[1];
    g_texture_uniform = glGetUniformLocation(g_shader_program, "u_texture");
    g_texture_lm_uniform = glGetUniformLocation(g_shader_program, "u_texture_lm");
    g_use_texture_uniform = glGetUniformLocation(g_shader_program, "u_use_texture");
    g_use_lightmap_uniform = glGetUniformLocation(g_shader_program, "u_use_lightmap");
    g_mvp_uniform = glGetUniformLocation(g_shader_program, "u_mvp");
    g_world_uniform = glGetUniformLocation(g_shader_program, "u_world");
    g_tex_matrix_uniform = glGetUniformLocation(g_shader_program, "u_tex_matrix");
    g_use_tex_matrix_uniform = glGetUniformLocation(g_shader_program, "u_use_tex_matrix");
    g_clip_plane_uniform = glGetUniformLocation(g_shader_program, "u_clip_plane[0]");
    g_clip_on_uniform = glGetUniformLocation(g_shader_program, "u_clip_on[0]");
    g_clip_plane_enabled_uniform = glGetUniformLocation(g_shader_program, "u_clip_plane_enabled");
    g_fbo_flip_y_uniform = glGetUniformLocation(g_shader_program, "u_fbo_flip_y");
    g_stage0_colorop_uniform = glGetUniformLocation(g_shader_program, "u_stage0_colorop");
    g_stage0_colorarg1_uniform = glGetUniformLocation(g_shader_program, "u_stage0_colorarg1");
    g_stage0_colorarg2_uniform = glGetUniformLocation(g_shader_program, "u_stage0_colorarg2");
    g_stage0_alphaop_uniform = glGetUniformLocation(g_shader_program, "u_stage0_alphaop");
    g_stage0_alphaarg1_uniform = glGetUniformLocation(g_shader_program, "u_stage0_alphaarg1");
    g_stage0_alphaarg2_uniform = glGetUniformLocation(g_shader_program, "u_stage0_alphaarg2");
    g_stage1_op_uniform = glGetUniformLocation(g_shader_program, "u_stage1_op");
    {
        static const char* stage_names[11] = { "u_stage_colorop", "u_stage_colorarg0", "u_stage_colorarg1", "u_stage_colorarg2", "u_stage_alphaop", "u_stage_alphaarg0",
                                               "u_stage_alphaarg1", "u_stage_alphaarg2", "u_stage_result", "u_stage_coord", "u_stage_has_tex" };
        for (int i = 0; i < 11; i++) g_stage_array_uniform[i] = glGetUniformLocation(g_shader_program, stage_names[i]);
        g_texture2_uniform = glGetUniformLocation(g_shader_program, "u_texture2");
        g_texture3_uniform = glGetUniformLocation(g_shader_program, "u_texture3");
        g_texture4_uniform = glGetUniformLocation(g_shader_program, "u_texture4");
        g_texture5_uniform = glGetUniformLocation(g_shader_program, "u_texture5");
        g_texture6_uniform = glGetUniformLocation(g_shader_program, "u_texture6");
        g_texture7_uniform = glGetUniformLocation(g_shader_program, "u_texture7");
        g_tc_source_uniform = glGetUniformLocation(g_shader_program, "u_tc_source");
        g_tc_count_uniform = glGetUniformLocation(g_shader_program, "u_tc_count");
        g_tc_projected_uniform = glGetUniformLocation(g_shader_program, "u_tc_projected");
        g_tc_matrix_uniform = glGetUniformLocation(g_shader_program, "u_tc_matrix");
    }
    g_alpha_test_enabled_uniform = glGetUniformLocation(g_shader_program, "u_alpha_test_enabled");
    g_alpha_to_coverage_uniform = glGetUniformLocation(g_shader_program, "u_alpha_to_coverage");
    g_alpha_ref_uniform = glGetUniformLocation(g_shader_program, "u_alpha_ref");
    g_alpha_func_uniform = glGetUniformLocation(g_shader_program, "u_alpha_func");
    g_ps_uniform = glGetUniformLocation(g_shader_program, "u_ps");
    g_half_pixel_uniform = glGetUniformLocation(g_shader_program, "u_half_pixel");
    g_ps_c_uniform = glGetUniformLocation(g_shader_program, "u_ps_c[0]");
    g_texture_factor_uniform = glGetUniformLocation(g_shader_program, "u_texture_factor");
    g_lod_bias_uniform = glGetUniformLocation(g_shader_program, "u_lod_bias");
    // Fog uniforms
    g_fog_enabled_uniform = glGetUniformLocation(g_shader_program, "u_fog_enabled");
    g_fog_start_uniform = glGetUniformLocation(g_shader_program, "u_fog_start");
    g_fog_end_uniform = glGetUniformLocation(g_shader_program, "u_fog_end");
    g_fog_color_uniform = glGetUniformLocation(g_shader_program, "u_fog_color");
    g_view_model_uniform = glGetUniformLocation(g_shader_program, "u_view_model");
    // FFP Lighting uniforms
    g_lighting_enabled_uniform = glGetUniformLocation(g_shader_program, "u_lighting_enabled");
    g_material_diffuse_uniform = glGetUniformLocation(g_shader_program, "u_material_diffuse");
    g_material_ambient_uniform = glGetUniformLocation(g_shader_program, "u_material_ambient");
    g_material_emissive_uniform = glGetUniformLocation(g_shader_program, "u_material_emissive");
    g_color_from_vertex_uniform = glGetUniformLocation(g_shader_program, "u_color_from_vertex");
    g_light_count_uniform = glGetUniformLocation(g_shader_program, "u_light_count");
    g_light_type_uniform = glGetUniformLocation(g_shader_program, "u_light_type");
    g_light_position_uniform = glGetUniformLocation(g_shader_program, "u_light_position");
    g_light_direction_uniform = glGetUniformLocation(g_shader_program, "u_light_direction");
    g_light_diffuse_uniform = glGetUniformLocation(g_shader_program, "u_light_diffuse");
    g_light_ambient_uniform = glGetUniformLocation(g_shader_program, "u_light_ambient");
    g_light_attenuation_uniform = glGetUniformLocation(g_shader_program, "u_light_attenuation");
    g_light_range_uniform = glGetUniformLocation(g_shader_program, "u_light_range");
    g_global_ambient_uniform = glGetUniformLocation(g_shader_program, "u_global_ambient");
    g_light0_enabled_uniform = glGetUniformLocation(g_shader_program, "u_light0_enabled");
    g_light0_position_uniform = glGetUniformLocation(g_shader_program, "u_light0_position");
    g_light0_diffuse_uniform = glGetUniformLocation(g_shader_program, "u_light0_diffuse");
    g_light0_ambient_uniform = glGetUniformLocation(g_shader_program, "u_light0_ambient");
    g_light0_attenuation_uniform = glGetUniformLocation(g_shader_program, "u_light0_attenuation");
    g_light0_range_uniform = glGetUniformLocation(g_shader_program, "u_light0_range");
    g_light1_enabled_uniform = glGetUniformLocation(g_shader_program, "u_light1_enabled");
    g_light1_position_uniform = glGetUniformLocation(g_shader_program, "u_light1_position");
    g_light1_diffuse_uniform = glGetUniformLocation(g_shader_program, "u_light1_diffuse");
    g_light1_ambient_uniform = glGetUniformLocation(g_shader_program, "u_light1_ambient");
    g_light1_attenuation_uniform = glGetUniformLocation(g_shader_program, "u_light1_attenuation");
    g_light1_range_uniform = glGetUniformLocation(g_shader_program, "u_light1_range");
    g_light2_enabled_uniform = glGetUniformLocation(g_shader_program, "u_light2_enabled");
    g_light2_position_uniform = glGetUniformLocation(g_shader_program, "u_light2_position");
    g_light2_diffuse_uniform = glGetUniformLocation(g_shader_program, "u_light2_diffuse");
    g_light2_ambient_uniform = glGetUniformLocation(g_shader_program, "u_light2_ambient");
    g_light2_attenuation_uniform = glGetUniformLocation(g_shader_program, "u_light2_attenuation");
    g_light2_range_uniform = glGetUniformLocation(g_shader_program, "u_light2_range");
    
    glDeleteShader(fs);
    out.program = g_shader_program;
    save_locations(out.locations);
    out.failed = false;
    return true;
}

// the base program: the fixed function pipeline
static void init_shaders() {
    if (g_shader_program != 0) return;

    // Initialize default sampler states
    for (int i = 0; i < 8; ++i) {
        g_sampler_states[i][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
        g_sampler_states[i][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        g_sampler_states[i][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        g_sampler_states[i][D3DSAMP_MINFILTER] = D3DTEXF_LINEAR;
        g_sampler_states[i][D3DSAMP_MAGFILTER] = D3DTEXF_LINEAR;
        g_sampler_states[i][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;  // D3D9 default; engine sets LINEAR for gameplay
    }

    Program base;
    if (!build_program(0, base)) { g_shader_program = 0; return; }
    g_programs[0] = base;
    g_shader_program = base.program;
    load_locations(base.locations);

#ifdef SRW_DIAGNOSTICS
    g_debug_mode = read_debug_mode();
    g_debug_mode_request = g_debug_mode;
    if (g_debug_mode != 0) {
        printf("[D3D9] debug mode %d\n", g_debug_mode);
        glUseProgram(g_shader_program);
        glUniform1i(glGetUniformLocation(g_shader_program, "u_debug_mode"), g_debug_mode);
    }
#endif
}

// The program for a pixel shader of the game (0: the fixed function pipeline); a shader whose program does not build draws with the
// base program. Returns the id of the program in use.
static void remember_program_switch();
static int select_program(int ps_id) {
    if (g_shader_program == 0) init_shaders();
    if (g_shader_program == 0) return 0;
    std::map<int, Program>::iterator it = g_programs.find(ps_id);
    if (it == g_programs.end()) {
        Program program;
        double start = emscripten_get_now();
        build_program(ps_id, program);
        if (program.failed) printf("[D3D9] The program of pixel shader %d did not build, so it draws with the fixed function pipeline.\n", ps_id);
        else if (SRW_LOG_ON(SRW_LOG_D3D)) printf("[D3D9] program of pixel shader %d built in %.0f ms\n", ps_id, emscripten_get_now() - start);
        it = g_programs.insert(std::make_pair(ps_id, program)).first;
        // (building a program leaves its locations in the variables)
        load_locations(g_programs[0].locations);
        g_shader_program = g_programs[0].program;
    }
    if (it->second.failed) ps_id = 0, it = g_programs.find(0);
    if (it->second.program != g_shader_program) {
        g_shader_program = it->second.program;
        load_locations(it->second.locations);
        glUseProgram(g_shader_program);
        remember_program_switch();
    }
    return ps_id;
}

// Helper: Determinant of upper-left 3x3 sub-matrix (only for the old winding behaviour, d3dCull setting 1)
static float det3x3(const D3DMATRIX& m) {
    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1])
         - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0])
         + m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
}


// Which faces are the front ones. Direct3D culls by the winding of the triangle on the screen (clockwise is the front) and does not
// look at the transforms, mirrored or not; rendering into a texture flips Y (u_fbo_flip_y), which reverses the winding. The
// d3dCull setting (/d3dcull.txt) exists to try the other ways: 1 is what the wrapper did before (the front faces of objects with a
// mirroring world-view matrix are the other ones), 2 turns everything around.
static int read_cull_mode() {
    FILE* f = fopen("/Port/d3dcull.txt", "r");
    int mode = 0;
    if (f) {
        if (fscanf(f, "%d", &mode) != 1) mode = 0;
        fclose(f);
    }
    return mode;
}

static GLenum choose_front_face(const D3DMATRIX& view_model, bool fbo_rendering, bool pretransformed) {
    static int mode = -1;
    if (mode < 0) mode = read_cull_mode();
    if (pretransformed) return GL_CW;
    bool flip = fbo_rendering;
    if (mode == 1 && det3x3(view_model) < 0.0f) flip = !flip;
    if (mode == 2) flip = !flip;
    return flip ? GL_CCW : GL_CW;
}

// Helper: Matrix multiplication
static void matrix_multiply(D3DMATRIX* result, const D3DMATRIX* a, const D3DMATRIX* b) {
    D3DMATRIX temp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp.m[i][j] = 0;
            for (int k = 0; k < 4; k++) {
                temp.m[i][j] += a->m[i][k] * b->m[k][j];
            }
        }
    }
    *result = temp;
}

static void matrix_identity(D3DMATRIX* m) {
    memset(m, 0, sizeof(D3DMATRIX));
    m->m[0][0] = 1.0f;
    m->m[1][1] = 1.0f;
    m->m[2][2] = 1.0f;
    m->m[3][3] = 1.0f;
}

// Sets a matrix uniform; the memory of a D3D matrix (row-major, v*M) is the same as GL's (column-major, M*v): no transpose
static void gl_set_matrix_uniform(GLint location, const D3DMATRIX* matrix) {
    if (location == -1 || !matrix) return;
    glUniformMatrix4fv(location, 1, GL_FALSE, (const float*)matrix);
}

// Helper: Set FFP lighting uniforms (material, ambient, lights 0-2)
static void SetFFPLightingUniforms(const D3DMATRIX* world, const DWORD* render_states, bool has_diffuse = false) {
    gl_set_matrix_uniform(g_world_uniform, world);

    // Material sources: D3DRS_COLORVERTEX (141) TRUE, DIFFUSEMATERIALSOURCE (145) COLOR1 by default; ambient (147) and emissive (148) MATERIAL
    // (1 is COLOR1; COLOR2 = specular is not supported and counts as material)
    bool color_vertex = has_diffuse && render_states[141];
    glUniform3f(g_color_from_vertex_uniform, color_vertex && render_states[145] == 1 ? 1.0f : 0.0f,
                color_vertex && render_states[147] == 1 ? 1.0f : 0.0f, color_vertex && render_states[148] == 1 ? 1.0f : 0.0f);

    glUniform4f(g_material_diffuse_uniform,
        g_material.Diffuse.r, g_material.Diffuse.g, g_material.Diffuse.b, g_material.Diffuse.a);
    glUniform4f(g_material_ambient_uniform,
        g_material.Ambient.r, g_material.Ambient.g, g_material.Ambient.b, g_material.Ambient.a);

    DWORD amb = render_states[D3DRS_AMBIENT];
    glUniform3f(g_global_ambient_uniform,
        ((amb >> 16) & 0xFF) / 255.0f,
        ((amb >> 8) & 0xFF) / 255.0f,
        (amb & 0xFF) / 255.0f);

    glUniform3f(g_material_emissive_uniform, g_material.Emissive.r, g_material.Emissive.g, g_material.Emissive.b);

    // the enabled lights, packed
    int count = 0;
    GLint types[8];
    float positions[24], directions[24], diffuses[32], ambients[24], attenuations[24], ranges[8];
    for (int i = 0; i < 8; i++) {
        if (!g_light_enabled[i]) continue;
        const D3DLIGHT9& l = g_lights[i];
        types[count] = (int) l.Type;
        positions[3 * count + 0] = l.Position.x; positions[3 * count + 1] = l.Position.y; positions[3 * count + 2] = l.Position.z;
        directions[3 * count + 0] = l.Direction.x; directions[3 * count + 1] = l.Direction.y; directions[3 * count + 2] = l.Direction.z;
        diffuses[4 * count + 0] = l.Diffuse.r; diffuses[4 * count + 1] = l.Diffuse.g; diffuses[4 * count + 2] = l.Diffuse.b; diffuses[4 * count + 3] = l.Diffuse.a;
        ambients[3 * count + 0] = l.Ambient.r; ambients[3 * count + 1] = l.Ambient.g; ambients[3 * count + 2] = l.Ambient.b;
        attenuations[3 * count + 0] = l.Attenuation0; attenuations[3 * count + 1] = l.Attenuation1; attenuations[3 * count + 2] = l.Attenuation2;
        ranges[count] = l.Range;
        count++;
    }
    glUniform1i(g_light_count_uniform, count);
    if (count > 0) {
        glUniform1iv(g_light_type_uniform, count, types);
        glUniform3fv(g_light_position_uniform, count, positions);
        glUniform3fv(g_light_direction_uniform, count, directions);
        glUniform4fv(g_light_diffuse_uniform, count, diffuses);
        glUniform3fv(g_light_ambient_uniform, count, ambients);
        glUniform3fv(g_light_attenuation_uniform, count, attenuations);
        glUniform1fv(g_light_range_uniform, count, ranges);
    }

#ifdef SRW_DIAGNOSTICS
    {
        static int logged = 0;

        if (SRW_LOG_ON(SRW_LOG_D3D) && logged < 6) {
            logged++;
            printf("[D3D9] lighting: %d lights (types", count);
            for (int i = 0; i < count; i++) printf(" %d", types[i]);
            printf("), material diffuse %.2f %.2f %.2f %.2f ambient %.2f %.2f %.2f emissive %.2f %.2f %.2f, global ambient %.2f %.2f %.2f\n",
                   g_material.Diffuse.r, g_material.Diffuse.g, g_material.Diffuse.b, g_material.Diffuse.a,
                   g_material.Ambient.r, g_material.Ambient.g, g_material.Ambient.b,
                   g_material.Emissive.r, g_material.Emissive.g, g_material.Emissive.b,
                   ((amb >> 16) & 0xFF) / 255.0f, ((amb >> 8) & 0xFF) / 255.0f, (amb & 0xFF) / 255.0f);
            for (int i = 0; i < count; i++) {
                printf("[D3D9]   light %d: diffuse %.2f %.2f %.2f ambient %.2f %.2f %.2f direction %.2f %.2f %.2f position %.1f %.1f %.1f range %.1f\n", i,
                       diffuses[4 * i], diffuses[4 * i + 1], diffuses[4 * i + 2], ambients[3 * i], ambients[3 * i + 1], ambients[3 * i + 2],
                       directions[3 * i], directions[3 * i + 1], directions[3 * i + 2], positions[3 * i], positions[3 * i + 1], positions[3 * i + 2], ranges[i]);
            }
        }
    }
#endif
}

// Global FBO for SetRenderTarget
#ifndef D3DTEXF_ANISOTROPIC
#define D3DTEXF_ANISOTROPIC ((D3DTEXTUREFILTERTYPE) 3)
#endif
#ifndef D3DSAMP_MAXANISOTROPY
#define D3DSAMP_MAXANISOTROPY ((D3DSAMPLERSTATETYPE) 10)
#endif
static GLuint g_fbo = 0;

// The back buffer is always a framebuffer of our own: renderbuffers for the color (RGBA8) and for the depth and stencil
// (DEPTH24_STENCIL8, as the game asks), multisampled when MultiSampleType is 2 or more and with one sample (plain renderbuffers) otherwise.
// Emscripten's own back buffer (framebuffer 0, its offscreen framebuffer: a texture and a 16 bit depth buffer without stencil, the
// context's antialias and depth attributes do not reach it) is only where the finished frame is copied by a blit when it is presented and
// before the back buffer is read (back_buffer_resolve). 0 is left only if our framebuffer cannot be made (then framebuffer 0 itself is the
// back buffer, as it was before).
static GLuint g_back_fbo = 0, g_back_color_rb = 0, g_back_depth_rb = 0;
static int g_back_samples = 0, g_back_width = 0, g_back_height = 0;   // g_back_samples: 0 when the back buffer has one sample
static int g_wanted_samples = 0;   // what the program asked for (D3DPRESENT_PARAMETERS.MultiSampleType)
static int g_back_depth_bits = 16, g_back_stencil_bits = 0;   // of the framebuffer that is drawn into (16 and 0: Emscripten's own, see above)

static void back_buffer_release() {
    if (g_back_fbo != 0) glDeleteFramebuffers(1, &g_back_fbo);
    if (g_back_color_rb != 0) glDeleteRenderbuffers(1, &g_back_color_rb);
    if (g_back_depth_rb != 0) glDeleteRenderbuffers(1, &g_back_depth_rb);
    g_back_fbo = g_back_color_rb = g_back_depth_rb = 0;
    g_back_samples = 0;
    g_back_depth_bits = 16;
    g_back_stencil_bits = 0;
}

static void back_buffer_setup(int samples, int width, int height) {
    back_buffer_release();
    if (width <= 0 || height <= 0) return;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    if (samples > max_samples) samples = max_samples;
    if (samples < 2) samples = 0;
    for (;;) {
        glGenRenderbuffers(1, &g_back_color_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g_back_color_rb);
        glGenRenderbuffers(1, &g_back_depth_rb);
        if (samples > 0) glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
        else glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, g_back_depth_rb);
        if (samples > 0) glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
        else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glGenFramebuffers(1, &g_back_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_back_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_back_color_rb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_back_depth_rb);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status == GL_FRAMEBUFFER_COMPLETE) break;
        printf("[D3D9] back buffer: the framebuffer is not complete (%x) with %d samples: %s\n", status, samples, samples > 0 ? "one sample" : "framebuffer 0 itself");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        back_buffer_release();
        if (samples == 0) return;
        samples = 0;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    g_back_samples = samples;
    g_back_width = width;
    g_back_height = height;
    g_back_depth_bits = 24;
    g_back_stencil_bits = 8;
    GLint rb_depth = 0, rb_stencil = 0, rb_samples = 0;
    glBindRenderbuffer(GL_RENDERBUFFER, g_back_depth_rb);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_DEPTH_SIZE, &rb_depth);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_STENCIL_SIZE, &rb_stencil);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &rb_samples);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (rb_depth > 0) g_back_depth_bits = rb_depth;
    g_back_stencil_bits = rb_stencil;
    printf("[D3D9] back buffer: %dx%d, %d samples (asked for %d), depth %d bits, stencil %d bits (framebuffer of our own, copied to the canvas' framebuffer at Present)\n",
           width, height, samples > 0 ? samples : 1, g_wanted_samples, g_back_depth_bits, g_back_stencil_bits);
}

// The framebuffer to read the back buffer from: ours when it has one sample (nothing to copy), else framebuffer 0 after back_buffer_resolve
static GLuint back_buffer_read_fbo() { return g_back_samples == 0 ? g_back_fbo : 0; }

// our back buffer into framebuffer 0 (before the frame is presented or the back buffer is read from there): a plain copy with one sample,
// the resolve of the samples with more
static void back_buffer_resolve() {
    if (g_back_fbo == 0) return;
    GLint read = 0, draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_back_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (g_cached_scissor_test) glDisable(GL_SCISSOR_TEST);   // (the scissor box limits blits too)
    glBlitFramebuffer(0, 0, g_back_width, g_back_height, 0, 0, g_back_width, g_back_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (g_cached_scissor_test) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) draw);
}
// The report of the `gfx` command (and of the log, category perf): what the wrapper does that decides whether alpha to coverage and the
// anisotropic filter take effect, from the last presented frame. Called from any thread; `window` takes the averages of the frames since the
// last print of the log instead.
static int describe_graphics(char* out, size_t size, bool window) {
    FrameStats avg = window ? g_stats_window : g_stats_last;
    if (window && g_stats_window_frames > 1) {
        unsigned* a = (unsigned*) &avg;
        for (size_t i = 0; i < sizeof(FrameStats) / sizeof(unsigned); i++) a[i] /= g_stats_window_frames;
        avg.max_aniso_state = g_stats_window.max_aniso_state;
    }
    return snprintf(out, size,
                    "back buffer %dx%d, %d sample%s (the game asked for %d), depth %d bits, stencil %d bits, %s; alpha to coverage: setting %s, %s; %s: %u draws, %u primitives "
                    "(%u with a pixel shader, %u into render target textures); alpha test %u draws: %u as coverage, %u blended, %u other function, %u without it (one sample, "
                    "setting off or render target); anisotropy: %.0fx at most in this browser, the game asked for it %u times (state up to %ux, %u without mipmaps), set on textures "
                    "1x %u, 2x %u, 4x %u, 8x %u, 16x %u; lit %u, specular enabled %u, specular vertex color %u, generated coordinates %u",
                    g_back_fbo != 0 ? g_back_width : g_logical_width, g_back_fbo != 0 ? g_back_height : g_logical_height, g_back_samples > 0 ? g_back_samples : 1,
                    g_back_samples > 1 ? "s" : "", g_wanted_samples, g_back_depth_bits, g_back_stencil_bits, g_back_fbo != 0 ? "framebuffer of our own" : "the canvas' framebuffer",
                    alpha_coverage_wanted() ? "on" : "off", g_back_samples > 0 ? "can take effect" : "cannot (the back buffer has one sample)",
                    window ? "average frame" : "last frame", avg.draws, avg.primitives, avg.pixel_shader_draws, avg.render_target_draws, avg.alpha_test_draws,
                    avg.alpha_coverage_draws, avg.alpha_blended_draws, avg.alpha_other_func_draws, avg.alpha_no_samples_draws, g_max_anisotropy, avg.aniso_asked,
                    avg.max_aniso_state, avg.aniso_no_mips, avg.aniso_applied[0], avg.aniso_applied[1], avg.aniso_applied[2], avg.aniso_applied[3], avg.aniso_applied[4],
                    avg.lit_draws, avg.specular_enable_draws, avg.specular_vertex_draws, avg.texgen_draws);
}
extern "C" int SRW_D3DDescribeGraphics(char* out, size_t size) { return describe_graphics(out, size, false); }

#ifdef SRW_DIAGNOSTICS
static bool g_probe_ui_done = false;
// The back buffer now: how much of each row is not black (rows from the top of the picture), to tell which part of the 3D picture
// is there at a given moment of the frame; `limit` reports per call site
static void probe_backbuffer(const char* where, int* counter, int limit, int every) {
    if (!SRW_LOG_ON(SRW_LOG_D3D)) return;
    const int n = (*counter)++;
    if (n >= limit && n % every != 0) return;
    const int w = g_logical_width, h = g_logical_height;
    if (w <= 0 || h <= 0) return;
    GLint read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    back_buffer_resolve();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    static const int rows_from_top[] = {0, 1, 2, 3, 8, 32, 128, 300, 400, 600, 700, 760, 767};
    char line[600];
    int len = snprintf(line, sizeof(line), "[D3D9] probe %s: non-black pixels of 32 per row (row from the top:count)", where);
    for (int r : rows_from_top) {
        if (r >= h) continue;
        int nonblack = 0;
        for (int x = 0; x < 32; x++) {
            unsigned char px[4] = {0, 0, 0, 0};
            glReadPixels(w * (2 * x + 1) / 64, h - 1 - r, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (px[0] || px[1] || px[2]) nonblack++;
        }
        len += snprintf(line + len, sizeof(line) - len, " %d:%d", r, nonblack);
    }
    printf("%s\n", line);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) read);
}
#endif
// WebGL wants the color and the depth attachment of a framebuffer in the same size, Direct3D 9 does not: a render target texture smaller than the
// depth surface of the game (the small pictures are 256x192, the surface 1024x768) gets a depth buffer of its own size from this pool
static std::map<unsigned int, GLuint> g_fbo_depth_pool;
static GLuint FboDepthOfSize(unsigned int width, unsigned int height) {
    unsigned int key = (width << 16) | (height & 0xFFFF);
    std::map<unsigned int, GLuint>::iterator found = g_fbo_depth_pool.find(key);

    if (found != g_fbo_depth_pool.end()) return found->second;
    GLuint object = 0;
    glGenRenderbuffers(1, &object);
    glBindRenderbuffer(GL_RENDERBUFFER, object);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei) width, (GLsizei) height);
    g_fbo_depth_pool[key] = object;
    return object;
}

class D3D9Texture;

// D3D9Surface class
class D3D9Surface : public IDirect3DSurface9 {
public:
    // A surface of a texture is one level of it: locking it locks that level of the texture, which is
    // how the program fills its textures (GetSurfaceLevel, LockRect, UnlockRect).
    D3D9Texture* m_owner = nullptr;
    UINT m_level = 0;
    D3DFORMAT m_format = D3DFMT_X8R8G8B8;
    D3DPOOL m_pool = D3DPOOL_DEFAULT;
    GLuint m_gl_object;      // Texture ID or Renderbuffer ID
    bool m_is_renderbuffer;  // True if Renderbuffer (Depth), False if Texture
    bool m_is_backbuffer;    // True if this represents the default backbuffer (Canvas)
    UINT m_width;
    UINT m_height;
    ULONG m_refCount;        // Reference count for COM-style lifetime management
    std::vector<uint8_t> m_lock_buffer;   // the pixels of a locked rectangle of the back buffer
    bool m_lock_buffer_in_use = false;
    bool m_owns_texture = false;          // a render target of its own (CreateRenderTarget): m_gl_object is its texture, rows top down

    D3D9Surface(UINT width, UINT height, GLuint gl_obj, bool is_rb, bool is_bb = false)
        : m_width(width), m_height(height), m_gl_object(gl_obj),
          m_is_renderbuffer(is_rb), m_is_backbuffer(is_bb), m_refCount(1) {}
    
    unsigned int m_generation = g_gl_generation;

    virtual ~D3D9Surface() {
        if (m_is_renderbuffer && m_gl_object != 0 && !m_is_backbuffer && m_generation == g_gl_generation) {
            glDeleteRenderbuffers(1, &m_gl_object);
        }
        if (m_owns_texture && m_gl_object != 0 && m_generation == g_gl_generation) glDeleteTextures(1, &m_gl_object);
        // If it's a texture surface, D3D9Texture owns the GL object, so we don't delete it here.
    }

    virtual ULONG AddRef() override {
        return ++m_refCount;
    }

    virtual ULONG Release() override {
        ULONG ref = --m_refCount;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }
    
    virtual HRESULT LockRect(D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) override;
    virtual HRESULT UnlockRect() override;

    virtual HRESULT GetDesc(D3DSURFACE_DESC* pDesc) override {
        if (!pDesc) return D3DERR_INVALIDCALL;
        memset(pDesc, 0, sizeof(D3DSURFACE_DESC));
        pDesc->Width = m_width;
        pDesc->Height = m_height;
        pDesc->Format = m_format;
        pDesc->Type = m_is_renderbuffer ? D3DRTYPE_SURFACE : D3DRTYPE_TEXTURE;
        pDesc->Pool = m_pool;
        return D3D_OK;
    }
};

// Converts a region of pixels of a D3D format to RGBA8 (rows of `pitch` bytes at `base`), for WebGL
static void ConvertRegionToRGBA(D3DFORMAT format, const uint8_t* base, UINT pitch, UINT w, UINT h, uint32_t* out) {
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = base + (size_t) y * pitch;
        uint32_t* dst = out + (size_t) y * w;

        if (format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8) {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(row);
            for (UINT x = 0; x < w; ++x) {
                uint32_t color = src[x];
                uint32_t b = (color >> 0) & 0xFF, g = (color >> 8) & 0xFF, r = (color >> 16) & 0xFF, a = (color >> 24) & 0xFF;
                if (format == D3DFMT_X8R8G8B8) a = 0xFF;   // only X8R8G8B8 is forced opaque
                dst[x] = (r << 0) | (g << 8) | (b << 16) | (a << 24);
            }
        } else if (format == D3DFMT_A4R4G4B4) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(row);
            for (UINT x = 0; x < w; ++x) {
                uint16_t color = src[x];
                uint32_t a = ((color >> 12) & 0xF); a = (a << 4) | a;
                uint32_t r = ((color >>  8) & 0xF); r = (r << 4) | r;
                uint32_t g = ((color >>  4) & 0xF); g = (g << 4) | g;
                uint32_t b = ((color >>  0) & 0xF); b = (b << 4) | b;
                dst[x] = (r << 0) | (g << 8) | (b << 16) | (a << 24);
            }
        } else if (format == D3DFMT_A1R5G5B5 || format == (D3DFORMAT) 24) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(row);
            for (UINT x = 0; x < w; ++x) {
                uint16_t color = src[x];
                uint32_t r = ((color >> 10) & 0x1F); r = (r << 3) | (r >> 2);
                uint32_t g = ((color >> 5) & 0x1F);  g = (g << 3) | (g >> 2);
                uint32_t b = ((color >> 0) & 0x1F);  b = (b << 3) | (b >> 2);
                uint32_t a = (format == D3DFMT_A1R5G5B5) ? (((color >> 15) & 0x1) ? 255 : 0) : 255;
                dst[x] = (r << 0) | (g << 8) | (b << 16) | (a << 24);
            }
        } else if (format == D3DFMT_R5G6B5) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(row);
            for (UINT x = 0; x < w; ++x) {
                uint16_t color = src[x];
                uint32_t r = ((color >> 11) & 0x1F); r = (r << 3) | (r >> 2);
                uint32_t g = ((color >> 5) & 0x3F);  g = (g << 2) | (g >> 4);
                uint32_t b = ((color >> 0) & 0x1F);  b = (b << 3) | (b >> 2);
                dst[x] = (r << 0) | (g << 8) | (b << 16) | 0xFF000000u;
            }
        } else if (format == D3DFMT_R8G8B8) {
            for (UINT x = 0; x < w; ++x) {
                dst[x] = ((uint32_t) row[x * 3 + 2] << 0) | ((uint32_t) row[x * 3 + 1] << 8) | ((uint32_t) row[x * 3 + 0] << 16) | 0xFF000000u;
            }
        } else {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(row);
            for (UINT x = 0; x < w; ++x) dst[x] = src[x];
        }
    }
}

// (for the pointer of the program, d3d9-bridge.cpp)
void srw_convert_region_to_rgba(D3DFORMAT format, const uint8_t* base, UINT pitch, UINT w, UINT h, uint32_t* out) {
    ConvertRegionToRGBA(format, base, pitch, w, h, out);
}

// D3D9Texture class
class D3D9Texture : public IDirect3DTexture9 {
public:
    GLuint m_texture;
    UINT m_width;
    UINT m_height;
    UINT m_levels;
    D3DFORMAT m_format;
    void* m_locked_data;
    DWORD m_usage = 0;
    D3DPOOL m_pool = D3DPOOL_MANAGED;
    // The program writes into the texture through locked rectangles (the program keeps its font glyphs in
    // a few large textures and adds glyphs by locking a small rectangle): the levels are kept here in the D3D format, and the
    // pointer of a lock points into them at the rectangle.
    std::vector<std::vector<uint8_t>> m_shadow;
    std::vector<bool> m_uploaded;
    UINT m_locked_level = 0;
    UINT m_locked_left = 0, m_locked_top = 0, m_locked_right = 0, m_locked_bottom = 0;
    bool m_is_locked = false;
    bool m_mips_generated = false;
    UINT m_top_level = 0;
    
public:
    // the number of levels: 0 asked for the whole chain
    UINT LevelCount() const {
        if (m_levels != 0) return m_levels;
        UINT n = 1, size = std::max(m_width, m_height);
        while (size > 1) { size >>= 1; n++; }
        return n;
    }

    D3D9Texture(UINT width, UINT height, UINT levels, D3DFORMAT format)
        : m_width(width), m_height(height), m_levels(levels), m_format(format), m_locked_data(nullptr) {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        // the texture binding caches are invalid: this binds outside of draw calls
        g_last_bound_texture_id = 0xFFFFFFFF;
        g_last_bound_lightmap_id = 0xFFFFFFFF;
        g_cached_texture = 0xFFFFFFFF;

        // GL_LINEAR: a mipmap filter on a texture without mipmaps is incomplete
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // level 0 only until the other levels are uploaded or generated
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

        // CLAMP_TO_EDGE: textures of sizes that are not powers of two
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    
    unsigned int m_generation = g_gl_generation;

    virtual ~D3D9Texture() {
        if (m_texture && m_generation == g_gl_generation) {
            // Invalidate caches if this texture was cached (ID may be recycled by GL)
            if (g_last_bound_texture_id == m_texture) g_last_bound_texture_id = 0xFFFFFFFF;
            if (g_last_bound_lightmap_id == m_texture) g_last_bound_lightmap_id = 0xFFFFFFFF;
            if (g_cached_texture == m_texture) g_cached_texture = 0xFFFFFFFF;
            g_mipmapped_textures.erase(m_texture);
            glDeleteTextures(1, &m_texture);
        }
        if (m_locked_data) {
            delete[] static_cast<uint8_t*>(m_locked_data);
        }
    }

    virtual ULONG AddRef() override {
        return 1; // Stub: no ref counting for textures (owned by game code)
    }

    virtual ULONG Release() override {
        delete this;
        return 0;
    }
    
    virtual HRESULT GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override {
        if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
        if (Level >= LevelCount()) return D3DERR_INVALIDCALL;

        // Return a Surface wrapper for this level of the texture
        D3D9Surface* surface = new D3D9Surface(std::max(1u, m_width >> Level), std::max(1u, m_height >> Level), m_texture, false);
        surface->m_owner = this;
        surface->m_level = Level;
        surface->m_format = m_format;
        surface->m_pool = m_pool;
        *ppSurfaceLevel = surface;
        return D3D_OK;
    }

    virtual DWORD GetLevelCount() override { return LevelCount(); }

    virtual HRESULT GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override {
        if (!pDesc || Level >= LevelCount()) return D3DERR_INVALIDCALL;
        memset(pDesc, 0, sizeof(D3DSURFACE_DESC));
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_SURFACE;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Width = std::max(1u, m_width >> Level);
        pDesc->Height = std::max(1u, m_height >> Level);
        return D3D_OK;
    }
    
    virtual HRESULT LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect,
                            const void* pRect, DWORD Flags) override {
        if (!pLockedRect || Level >= LevelCount()) return D3DERR_INVALIDCALL;
        if (m_is_locked) return D3DERR_INVALIDCALL;   // a level is locked at a time

        UINT mip_width = std::max(1u, m_width >> Level);
        UINT mip_height = std::max(1u, m_height >> Level);
        bool compressed = IsCompressedFormat(m_format);
        size_t block_size = (m_format == D3DFMT_DXT1 || m_format == D3DFMT_GLI1) ? 8 : 16;
        UINT pitch;
        size_t data_size;

        if (compressed) {
            size_t blocks_w = (mip_width + 3) / 4, blocks_h = (mip_height + 3) / 4;
            pitch = (UINT) (blocks_w * block_size);
            data_size = blocks_h * pitch;
        } else {
            pitch = (mip_width * GetFormatBpp(m_format) + 7) / 8;
            data_size = (size_t) pitch * mip_height;
        }
        if (m_shadow.size() < LevelCount()) { m_shadow.resize(LevelCount()); m_uploaded.resize(LevelCount(), false); }
        if (m_shadow[Level].size() != data_size) m_shadow[Level].assign(data_size, 0);

        UINT left = 0, top = 0, right = mip_width, bottom = mip_height;
        if (pRect) {
            const RECT* r = static_cast<const RECT*>(pRect);
            if (r->left >= 0 && r->top >= 0 && (UINT) r->right <= mip_width && (UINT) r->bottom <= mip_height && r->left < r->right && r->top < r->bottom) {
                left = (UINT) r->left; top = (UINT) r->top; right = (UINT) r->right; bottom = (UINT) r->bottom;
            }
        }
        size_t offset = compressed ? (size_t) (top / 4) * pitch + (size_t) (left / 4) * block_size
                                   : (size_t) top * pitch + (size_t) left * GetFormatBpp(m_format) / 8;

        pLockedRect->Pitch = (INT) pitch;
        pLockedRect->pBits = m_shadow[Level].data() + offset;
        m_is_locked = true;
        m_locked_level = Level;
        m_locked_left = left; m_locked_top = top; m_locked_right = right; m_locked_bottom = bottom;
        return D3D_OK;
    }
    
    virtual HRESULT UnlockRect(UINT Level) override {
        if (!m_is_locked || Level != m_locked_level) return D3DERR_INVALIDCALL;
        m_is_locked = false;

        glBindTexture(GL_TEXTURE_2D, m_texture);
        // the texture binding caches are invalid: this binds outside of draw calls
        g_last_bound_texture_id = 0xFFFFFFFF;
        g_last_bound_lightmap_id = 0xFFFFFFFF;
        g_cached_texture = 0xFFFFFFFF;

        UINT mip_width = std::max(1u, m_width >> Level);
        UINT mip_height = std::max(1u, m_height >> Level);
        const std::vector<uint8_t>& level_data = m_shadow[Level];
        bool whole = m_locked_left == 0 && m_locked_top == 0 && m_locked_right == mip_width && m_locked_bottom == mip_height;

        if (IsCompressedFormat(m_format)) {
            GLenum gl_fmt = GetGLCompressedFormat(m_format);
            size_t block_size = (m_format == D3DFMT_DXT1 || m_format == D3DFMT_GLI1) ? 8 : 16;
            size_t size = ((mip_width + 3) / 4) * ((mip_height + 3) / 4) * block_size;

            // (a rectangle of a compressed level is uploaded as the whole level, which is kept complete in the shadow)
            if (!g_s3tc_available) {
                std::vector<uint32_t> rgba((size_t) mip_width * mip_height);
                DecodeDXTToRGBA(m_format, level_data.data(), mip_width, mip_height, rgba.data());
                glTexImage2D(GL_TEXTURE_2D, Level, GL_RGBA, mip_width, mip_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            } else
            CHECK_GL_ERROR(glCompressedTexImage2D(GL_TEXTURE_2D, Level, gl_fmt, mip_width, mip_height, 0, size, level_data.data()));
        } else {
            UINT pitch = (mip_width * GetFormatBpp(m_format) + 7) / 8;

            if (!m_uploaded[Level] || whole) {
                std::vector<uint32_t> rgba(( size_t) mip_width * mip_height);
                ConvertRegionToRGBA(m_format, level_data.data(), pitch, mip_width, mip_height, rgba.data());
                glTexImage2D(GL_TEXTURE_2D, Level, GL_RGBA, mip_width, mip_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            } else {
                UINT w = m_locked_right - m_locked_left, h = m_locked_bottom - m_locked_top;
                size_t offset = (size_t) m_locked_top * pitch + (size_t) m_locked_left * GetFormatBpp(m_format) / 8;
                std::vector<uint32_t> rgba((size_t) w * h);
                ConvertRegionToRGBA(m_format, level_data.data() + offset, pitch, w, h, rgba.data());
                glTexSubImage2D(GL_TEXTURE_2D, Level, m_locked_left, m_locked_top, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            }
        }
        m_uploaded[Level] = true;
        if (Level > m_top_level) m_top_level = Level;

        // levels that are there: the ones uploaded
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, m_top_level);

        if (Level > 0) {
            // Compressed textures with pre-baked mip chain (DDS files)
            g_mipmapped_textures.insert(m_texture);
        } else if (!IsCompressedFormat(m_format) && LevelCount() > 1 && !m_mips_generated) {
            // Non-compressed Level 0 of a texture with more levels: generate the chain (the program may overwrite levels later).
            // (Textures of one level, such as the glyph textures, get none: they are updated often.)
            int max_level = (int) LevelCount() - 1;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_level);
            glGenerateMipmap(GL_TEXTURE_2D);
            m_mips_generated = true;
            m_top_level = (UINT) max_level;
            g_mipmapped_textures.insert(m_texture);
        }
        return D3D_OK;
    }
    
    GLuint GetGLTexture() const { return m_texture; }
};

HRESULT D3D9Surface::LockRect(D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) {
    if (!m_owner) {
        if (m_owns_texture && pLockedRect) {
            // a lockable render target (CreateRenderTarget): with multisampling the program copies the picture from the back
            // buffer into one (StretchRect) and locks it. Read back through a framebuffer of its own; rows are stored top down.
            static GLuint read_fbo = 0;
            static unsigned read_generation = 0;
            RECT r = { 0, 0, (LONG) m_width, (LONG) m_height };
            if (pRect) r = *pRect;
            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > (LONG) m_width) r.right = (LONG) m_width;
            if (r.bottom > (LONG) m_height) r.bottom = (LONG) m_height;
            if (r.right <= r.left || r.bottom <= r.top || m_lock_buffer_in_use || m_generation != g_gl_generation) return D3DERR_INVALIDCALL;
            if (read_fbo == 0 || read_generation != g_gl_generation) {
                glGenFramebuffers(1, &read_fbo);
                read_generation = g_gl_generation;
            }
            const size_t w = (size_t) (r.right - r.left), h = (size_t) (r.bottom - r.top);
            GLint previous_read = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gl_object, 0);
            m_lock_buffer.resize(w * h * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(r.left, r.top, (GLsizei) w, (GLsizei) h, GL_RGBA, GL_UNSIGNED_BYTE, m_lock_buffer.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) previous_read);
            for (size_t i = 0; i < w * h; i++) {   // R, G, B, A -> B, G, R, opaque (as the back buffer's read)
                uint8_t* p = m_lock_buffer.data() + i * 4;
                const uint8_t red = p[0];
                p[0] = p[2];
                p[2] = red;
                p[3] = 255;
            }
            m_lock_buffer_in_use = true;
            pLockedRect->Pitch = (INT) (w * 4);
            pLockedRect->pBits = m_lock_buffer.data();
            LOG_SURFACE_CALL(lockrt, "LockRect of a render target %ux%u: rectangle %d,%d - %d,%d (read back)", (unsigned) m_width, (unsigned) m_height,
                             (int) r.left, (int) r.top, (int) r.right, (int) r.bottom);
            return D3D_OK;
        }
        if (m_is_backbuffer && pLockedRect) {
            // reading the back buffer on the CPU (the program draws the pictures in a corner of the screen and copies the pixels into the textures):
            // the pixels of the rectangle, top row first, as B, G, R, A bytes (X8R8G8B8 / A8R8G8B8)
            RECT r = { 0, 0, (LONG) m_width, (LONG) m_height };
            if (pRect) r = *pRect;
            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > (LONG) m_width) r.right = (LONG) m_width;
            if (r.bottom > (LONG) m_height) r.bottom = (LONG) m_height;
            if (r.right <= r.left || r.bottom <= r.top || m_lock_buffer_in_use) return D3DERR_INVALIDCALL;
            const size_t w = (size_t) (r.right - r.left), h = (size_t) (r.bottom - r.top);
            std::vector<uint8_t> rows(w * h * 4);
            GLint previous_read = 0;

            if (g_back_samples > 0) back_buffer_resolve();
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, back_buffer_read_fbo());
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(r.left, (GLint) m_height - r.bottom, (GLsizei) w, (GLsizei) h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) previous_read);
            m_lock_buffer.resize(w * h * 4);
            for (size_t y = 0; y < h; y++) {
                const uint8_t* from = rows.data() + (h - 1 - y) * w * 4;   // GL rows come bottom up
                uint8_t* to = m_lock_buffer.data() + y * w * 4;
                for (size_t x = 0; x < w; x++) {
                    to[x * 4 + 0] = from[x * 4 + 2];
                    to[x * 4 + 1] = from[x * 4 + 1];
                    to[x * 4 + 2] = from[x * 4 + 0];
                    to[x * 4 + 3] = 255;
                }
            }
            m_lock_buffer_in_use = true;
            pLockedRect->Pitch = (INT) (w * 4);
            pLockedRect->pBits = m_lock_buffer.data();
            LOG_SURFACE_CALL(lockbb, "LockRect of the back buffer: rectangle %d,%d - %d,%d, flags %x (read back)", (int) r.left, (int) r.top, (int) r.right, (int) r.bottom, (unsigned) Flags);
            return D3D_OK;
        }
        // other surfaces that are not part of a texture cannot be locked (yet)
        LOG_SURFACE_CALL(lock, "LockRect of a surface that is not part of a texture (%s, %ux%u, flags %x): refused", m_is_backbuffer ? "back buffer" : (m_is_renderbuffer ? "render buffer" : "plain surface"),
                         (unsigned) m_width, (unsigned) m_height, (unsigned) Flags);
        return D3DERR_INVALIDCALL;
    }
    return m_owner->LockRect(m_level, pLockedRect, pRect, Flags);
}

HRESULT D3D9Surface::UnlockRect() {
    if (!m_owner) {
        if (m_lock_buffer_in_use) {
            m_lock_buffer_in_use = false;
            return D3D_OK;
        }
        return D3DERR_INVALIDCALL;
    }
    return m_owner->UnlockRect(m_level);
}

// D3D9IndexBuffer class
// Buffers keep a CPU copy of what the GPU holds: Unlock uploads only the bytes that differ (the game locks large dynamic buffers whole
// every frame and writes a small part of them)
static void UploadChanged(GLenum target, std::vector<uint8_t>& shadow, UINT length, UINT offset, UINT size, const uint8_t* data) {
#ifdef SRW_DIAGNOSTICS
    if (shadow.empty() && length <= 4 * 1024 * 1024) shadow.assign(length, 0);   // so that the geometry of small buffers can be logged
#endif
    if (size < 512 || offset + size > length) {
        glBufferSubData(target, offset, size, data);
        if (!shadow.empty() && offset + size <= length) memcpy(shadow.data() + offset, data, size);
        return;
    }
    if (shadow.empty()) shadow.assign(length, 0);   // glBufferData(nullptr) makes zeros
    uint8_t* old = shadow.data() + offset;
    UINT first = 0, last = size;
    // the first and the last byte that differ, 16 bytes at a time (two 64-bit compares), then bytewise
    for (; first + 16 <= size; first += 16) {
        uint64_t a0, a1, b0, b1;
        memcpy(&a0, old + first, 8); memcpy(&a1, old + first + 8, 8);
        memcpy(&b0, data + first, 8); memcpy(&b1, data + first + 8, 8);
        if ((a0 ^ b0) | (a1 ^ b1)) break;
    }
    while (first < size && old[first] == data[first]) first++;
    if (first == size) return;
    for (; last >= first + 16; last -= 16) {
        uint64_t a0, a1, b0, b1;
        memcpy(&a0, old + last - 16, 8); memcpy(&a1, old + last - 8, 8);
        memcpy(&b0, data + last - 16, 8); memcpy(&b1, data + last - 8, 8);
        if ((a0 ^ b0) | (a1 ^ b1)) break;
    }
    while (last > first && old[last - 1] == data[last - 1]) last--;
    if (last <= first) return;
    first &= ~3u;
    glBufferSubData(target, offset + first, last - first, data + first);
    memcpy(old + first, data + first, last - first);
}

class D3D9IndexBuffer : public IDirect3DIndexBuffer9 {
private:
    GLuint m_buffer;
    UINT m_length;
    D3DFORMAT m_format;
    void* m_locked_data;
    UINT m_locked_size;
    UINT m_locked_offset;
    std::vector<uint8_t> m_shadow;

public:
    D3D9IndexBuffer(UINT length, D3DFORMAT format) : m_length(length), m_format(format), m_locked_data(nullptr), m_locked_size(0), m_locked_offset(0) {
        glGenBuffers(1, &m_buffer);
        // Initialize buffer size
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, length, nullptr, GL_DYNAMIC_DRAW);   // the game rewrites parts of its buffers every frame: a static hint makes ANGLE rebuild them
        // (no unbinding: every user binds what it needs, and the GL state cache skips a binding that is already there)
    }
    
    unsigned int m_generation = g_gl_generation;

    virtual ~D3D9IndexBuffer() {
        if (m_buffer && m_generation == g_gl_generation) {
            glDeleteBuffers(1, &m_buffer);
        }
        if (m_locked_data) {
            delete[] static_cast<uint8_t*>(m_locked_data);
        }
    }

    virtual ULONG AddRef() override {
        return 1; // Stub: no ref counting for index buffers
    }

    virtual ULONG Release() override {
        delete this;
        return 0;
    }
    
    virtual HRESULT Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override {
        if (!ppbData) return -1;
        if (m_locked_data) return -1; // Already locked
        
        // Simple implementation: always lock the whole buffer or requested size
        UINT size = (SizeToLock == 0) ? (m_length - OffsetToLock) : SizeToLock;
        m_locked_data = new uint8_t[size];
        if (!m_shadow.empty() && OffsetToLock + size <= m_length) memcpy(m_locked_data, m_shadow.data() + OffsetToLock, size);
        m_locked_size = size;
        m_locked_offset = OffsetToLock;
        *ppbData = m_locked_data;
        
        return D3D_OK;
    }
    
    virtual HRESULT Unlock() override {
        if (!m_locked_data) return -1;
        
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffer);
        // Note: This implementation re-uploads only the locked part to the correct offset.
        UploadChanged(GL_ELEMENT_ARRAY_BUFFER, m_shadow, m_length, m_locked_offset, m_locked_size, static_cast<const uint8_t*>(m_locked_data));
        // (no unbinding, see above)
        
        delete[] static_cast<uint8_t*>(m_locked_data);
        m_locked_data = nullptr;
        return D3D_OK;
    }

    GLuint GetGLBuffer() const { return m_buffer; }
    D3DFORMAT GetFormat() const { return m_format; }
};

// D3D9VertexBuffer class
class D3D9VertexBuffer : public IDirect3DVertexBuffer9 {
private:
    GLuint m_buffer;
    UINT m_length;
    void* m_locked_data;
    UINT m_locked_size;
    UINT m_locked_offset;
    std::vector<uint8_t> m_shadow;
    
public:
    D3D9VertexBuffer(UINT length) : m_length(length), m_locked_data(nullptr), m_locked_size(0), m_locked_offset(0) {
        glGenBuffers(1, &m_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
        glBufferData(GL_ARRAY_BUFFER, length, nullptr, GL_DYNAMIC_DRAW);
        // (no unbinding, see above)
    }
    
    const uint8_t* ShadowData() const { return m_shadow.empty() ? nullptr : m_shadow.data(); }
    UINT ShadowSize() const { return (UINT) m_shadow.size(); }

    unsigned int m_generation = g_gl_generation;

    virtual ~D3D9VertexBuffer() {
        if (m_buffer && m_generation == g_gl_generation) {
            glDeleteBuffers(1, &m_buffer);
        }
        if (m_locked_data) {
            delete[] static_cast<uint8_t*>(m_locked_data);
        }
    }

    virtual ULONG AddRef() override {
        return 1; // Stub: no ref counting for vertex buffers
    }

    virtual ULONG Release() override {
        delete this;
        return 0;
    }
    
    virtual HRESULT Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override {
        if (!ppbData) return -1;
        if (m_locked_data) return -1; // Already locked
        
        UINT size = (SizeToLock == 0) ? (m_length - OffsetToLock) : SizeToLock;
        m_locked_data = new uint8_t[size];
        if (!m_shadow.empty() && OffsetToLock + size <= m_length) memcpy(m_locked_data, m_shadow.data() + OffsetToLock, size);
        m_locked_size = size;
        m_locked_offset = OffsetToLock;
        *ppbData = m_locked_data;
        
        return D3D_OK;
    }
    
    virtual HRESULT Unlock() override {
        if (!m_locked_data) return -1;
        
        glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
        UploadChanged(GL_ARRAY_BUFFER, m_shadow, m_length, m_locked_offset, m_locked_size, static_cast<const uint8_t*>(m_locked_data));
        // (no unbinding, see above)
        
        delete[] static_cast<uint8_t*>(m_locked_data);
        m_locked_data = nullptr;
        return D3D_OK;
    }
    
    GLuint GetGLBuffer() const { return m_buffer; }
};

// D3D9VertexDeclaration: the fixed function pipeline of this wrapper is driven by FVF codes, so a declaration is turned into
// the FVF that describes the same layout (position, normal, diffuse, specular and texture coordinates of the first stream, in
// the order Direct3D requires them in memory; declarations that are laid out differently draw wrongly)
class D3D9VertexDeclaration : public IDirect3DVertexDeclaration9 {
public:
    DWORD m_fvf;
    int m_references;
    std::vector<unsigned char> m_elements;   // the elements as given, with the end marker

    D3D9VertexDeclaration(const void* elements) : m_fvf(0), m_references(1) {
        struct Element { WORD stream; WORD offset; BYTE type; BYTE method; BYTE usage; BYTE usage_index; };
        const Element* e = (const Element*) elements;
        int texcoords = 0;
        bool warned = false;

        for (; e->stream != 0xFF; e++) {
            if (e->stream != 0 && !warned) {
                printf("[D3D9] A vertex declaration uses stream %u, but only stream 0 is drawn.\n", (unsigned) e->stream);
                warned = true;
            }
            if (e->stream != 0) continue;
            switch (e->usage) {
                case 0:  m_fvf |= D3DFVF_XYZ; break;                                  // POSITION
                case 9:  m_fvf |= D3DFVF_XYZRHW; break;                               // POSITIONT
                case 3:  m_fvf |= D3DFVF_NORMAL; break;                               // NORMAL
                case 10: m_fvf |= (e->usage_index == 0) ? 0x040 : 0x080; break;       // COLOR: diffuse, specular
                case 5:  // TEXCOORD: the number of the last set, and the size of each (float1 to float4: the size bits of an FVF)
                    if ((int) e->usage_index + 1 > texcoords) texcoords = (int) e->usage_index + 1;
                    if (e->usage_index < 8) {
                        static const DWORD size_bits[4] = { 3, 0, 1, 2 };
                        if (e->type < 4) m_fvf |= size_bits[e->type] << (16 + 2 * e->usage_index);
                    }
                    break;
                default: break;
            }
        }
        m_fvf |= (DWORD) texcoords << 8;
        m_elements.assign((const unsigned char*) elements, (const unsigned char*) (e + 1));
    }
    virtual ~D3D9VertexDeclaration() {}
    virtual ULONG AddRef() override { return (ULONG) ++m_references; }
    virtual ULONG Release() override {
        int remaining = --m_references;
        if (remaining <= 0) delete this;
        return (ULONG) (remaining > 0 ? remaining : 0);
    }
};

// D3D9PixelShader: the bytecode is not run; the program's own shaders are known (ps_shaders.inc) and become the id of a GLSL function
class D3D9PixelShader : public IDirect3DPixelShader9 {
public:
    int m_references = 1;
    int m_id = 0;
    virtual ~D3D9PixelShader() {}
    virtual ULONG AddRef() override { return (ULONG) ++m_references; }
    virtual ULONG Release() override {
        int remaining = --m_references;
        if (remaining <= 0) delete this;
        return (ULONG) (remaining > 0 ? remaining : 0);
    }
};

// D3D9StateBlock class
class D3D9StateBlock : public IDirect3DStateBlock9 {
public:
    virtual ~D3D9StateBlock() {}
    virtual ULONG AddRef() override {
        return 1; // Stub: no ref counting for state blocks
    }
    virtual ULONG Release() override {
        delete this;
        return 0;
    }
    virtual HRESULT Capture() override {
        return D3D_OK;
    }
    virtual HRESULT Apply() override {
        return D3D_OK;
    }
};

// Implementation classes
class D3D9Device : public IDirect3DDevice9 {
private:
    D3DMATRIX m_world;
    D3DMATRIX m_view;
    D3DMATRIX m_projection;
    GLuint m_current_index_buffer;
    D3DFORMAT m_current_ib_format;
    GLuint m_current_vertex_buffer;
    UINT m_current_stride;
    
    D3D9Surface* m_backbuffer_surface;
    UINT m_current_vb_offset = 0;      // OffsetInBytes of SetStreamSource
    D3D9VertexBuffer* m_current_vb_object = nullptr;   // (diagnostics: where the vertices of the draw are)
    D3D9Surface* m_current_rt;
    D3D9Surface* m_current_ds;
    
    // Clip Planes
    float m_clip_planes[6][4]; // the six user clip planes (world space)
    DWORD m_clip_mask = 0;     // D3DRS_CLIPPLANEENABLE: one bit for each plane
    bool m_clip_plane_enabled = false;
    void UploadClipPlanes() {
        float on[6];
        for (int i = 0; i < 6; i++) on[i] = (m_clip_mask >> i) & 1 ? 1.0f : 0.0f;
        glUniform4fv(g_clip_plane_uniform, 6, &m_clip_planes[0][0]);
        if (g_clip_on_uniform != -1) glUniform1fv(g_clip_on_uniform, 6, on);
    }

    // FBO rendering state (for D3D9/OpenGL Y-convention fix)
    bool m_fbo_rendering = false;

    // State Tracking
    GLenum m_src_blend = GL_ONE;
    GLenum m_dst_blend = GL_ZERO;
    DWORD m_alpha_ref = 0;
    bool m_alpha_test_enabled = false;
    D3DCOLOR m_texture_factor;

    // Fog state
    bool m_fog_enabled = false;
    float m_fog_start = 0.0f;
    float m_fog_end = 1.0f;
    D3DCOLOR m_fog_color = 0xFFFFFFFF;

    // Debug: Flags to suppress repetitive shader warnings (performance optimization)
    static bool s_pixel_shader_warning_shown;
    static bool s_vertex_shader_warning_shown;
    static bool s_vertex_shader_constant_warning_shown;
    RECT m_scissor_rect = {};

public:
    D3D9Device() : m_current_index_buffer(0), m_current_ib_format(D3DFMT_INDEX16), m_current_vertex_buffer(0), m_current_stride(0), m_texture_factor(0xFFFFFFFF) {
        g_current_device = this;
        init_shaders();
        
        g_viewport.X = 0;
        g_viewport.Y = 0;
        g_viewport.Width = g_logical_width;
        g_viewport.Height = g_logical_height;
        g_viewport.MinZ = 0.0f;
        g_viewport.MaxZ = 1.0f;

        if (g_fbo == 0) glGenFramebuffers(1, &g_fbo);

        m_scissor_rect = {0, 0, (LONG)g_logical_width, (LONG)g_logical_height};

        // Backbuffer surface matches the app's backbuffer resolution
        m_backbuffer_surface = new D3D9Surface(g_logical_width, g_logical_height, 0, false, true);
        m_current_rt = m_backbuffer_surface;
        m_current_ds = nullptr; // Default DS is implicit in WebGL context
        
        // Initialize matrices to identity
        matrix_identity(&m_world);
        matrix_identity(&m_view);
        matrix_identity(&m_projection);
        for (int i = 0; i < 8; i++) {
            matrix_identity(&g_texture_matrices[i]);
            // Default sampler states
            g_sampler_states[i][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
            g_sampler_states[i][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
            g_sampler_states[i][D3DSAMP_MAGFILTER] = D3DTEXF_LINEAR;
            g_sampler_states[i][D3DSAMP_MINFILTER] = D3DTEXF_LINEAR;
            g_sampler_states[i][D3DSAMP_MIPFILTER] = D3DTEXF_NONE; // D3D9 default: no mip filtering

            // Default texture stage states (D3D9 defaults)
            if (i == 0) {
                g_texture_stage_states[i][D3DTSS_COLOROP] = D3DTOP_MODULATE;
                g_texture_stage_states[i][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
            } else {
                g_texture_stage_states[i][D3DTSS_COLOROP] = D3DTOP_DISABLE;
                g_texture_stage_states[i][D3DTSS_ALPHAOP] = D3DTOP_DISABLE;
            }
            // (Direct3D 9 defaults: the second argument is the result of the stage before, the texture coordinates are those of the stage)
            g_texture_stage_states[i][D3DTSS_COLORARG0] = D3DTA_CURRENT;
            g_texture_stage_states[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            g_texture_stage_states[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
            g_texture_stage_states[i][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
            g_texture_stage_states[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            g_texture_stage_states[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            g_texture_stage_states[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
            g_texture_stage_states[i][D3DTSS_TEXCOORDINDEX] = (DWORD) i;
        }

        // Initialize state
        m_alpha_ref = 0;
        m_alpha_test_enabled = false;
        m_src_blend = GL_ONE;
        m_dst_blend = GL_ZERO;

        // Initialize render states with D3D9 defaults
        memset(m_render_states, 0, sizeof(m_render_states));
        m_render_states[D3DRS_LIGHTING]         = TRUE;             // D3D9 default: lighting enabled
        m_render_states[D3DRS_ZWRITEENABLE]     = TRUE;             // D3D9 default: depth write enabled
        m_render_states[D3DRS_COLORWRITEENABLE] = 0x0000000F;       // D3D9 default: all channels (R|G|B|A)
        m_render_states[D3DRS_ZFUNC]            = D3DCMP_LESSEQUAL; // D3D9 default: LESSEQUAL (not GL's LESS)
        m_render_states[25]                     = 8;                // D3DRS_ALPHAFUNC = D3DCMP_ALWAYS (the Direct3D 9 default)
        m_render_states[141]                    = TRUE;             // D3DRS_COLORVERTEX
        m_render_states[145]                    = 1;                // D3DRS_DIFFUSEMATERIALSOURCE = D3DMCS_COLOR1

        // Initial viewport call
        SetViewport(&g_viewport);

        // Enable depth test
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL); // D3D9 default ZFUNC is LESSEQUAL, not LESS
        glDepthMask(GL_TRUE);
        
        // Direct3D 9 has clockwise front faces, OpenGL counterclockwise
        glFrontFace(GL_CW);

        // Disable culling initially (will be set via SetRenderState)
        glDisable(GL_CULL_FACE);
    }
    
    virtual ~D3D9Device() {
        // Release owned backbuffer surface
        if (m_backbuffer_surface) {
            m_backbuffer_surface->Release();
            m_backbuffer_surface = nullptr;
        }
        m_current_rt = nullptr;
        m_current_ds = nullptr;

        printf("[D3D9] The device was destroyed (%s).\n", g_current_device == this ? "the current one" : "an old one, nothing to delete");
        if (g_current_device != this) return;   // a newer device owns the shared objects now
        g_current_device = nullptr;

        // Clean up GL resources owned by this device
        if (g_fbo) {
            glDeleteFramebuffers(1, &g_fbo);
            g_fbo = 0;
        }
        if (g_up_vbo) {
            glDeleteBuffers(1, &g_up_vbo);
            g_up_vbo = 0;
            g_up_vbo_size = 0;
        }
        if (g_up_ibo) {
            glDeleteBuffers(1, &g_up_ibo);
            g_up_ibo = 0;
            g_up_ibo_size = 0;
        }
        for (std::map<int, Program>::iterator it = g_programs.begin(); it != g_programs.end(); ++it) {
            if (it->second.program) glDeleteProgram(it->second.program);
        }
        g_programs.clear();
        g_shader_program = 0;
        g_vertex_shader_object = 0;
        g_current_ps_id = 0;
    }

    virtual ULONG AddRef() override {
        return 1; // Stub: no ref counting for device
    }

    virtual ULONG Release() override {
        delete this;
        return 0;
    }
    
    // Render target and depth stencil methods
    virtual HRESULT GetRenderTarget(DWORD Index, IDirect3DSurface9** ppRenderTarget) override {
        if (Index != 0) return D3DERR_INVALIDCALL;
        if (ppRenderTarget) {
            *ppRenderTarget = m_current_rt;
            if (m_current_rt) {
                m_current_rt->AddRef(); // Proper COM semantics: caller must Release
            }
        }
        return D3D_OK;
    }
    
    // the color texture and the depth buffer of the framebuffer for the current render target texture (same size, see FboDepthOfSize)
    void ApplyFboAttachments() {
        if (!m_current_rt || m_current_rt->m_is_backbuffer) return;
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_current_rt->m_gl_object, 0);
        GLuint depth = 0;
        if (m_current_ds) {
            if (m_current_ds->m_width == m_current_rt->m_width && m_current_ds->m_height == m_current_rt->m_height) depth = m_current_ds->m_gl_object;
            else depth = FboDepthOfSize(m_current_rt->m_width, m_current_rt->m_height);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    }

    virtual HRESULT SetRenderTarget(DWORD Index, IDirect3DSurface9* pRenderTarget) override {
        if (Index != 0 || !pRenderTarget) return D3DERR_INVALIDCALL;

        D3D9Surface* surf = static_cast<D3D9Surface*>(pRenderTarget);
        m_current_rt = surf;
#ifdef SRW_DIAGNOSTICS
        {
            static int logged = 0;
            if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 100) printf("[D3D9] SetRenderTarget: %s %ux%u\n", surf->m_is_backbuffer ? "back buffer" : "texture", (unsigned) surf->m_width, (unsigned) surf->m_height);
        }
#endif

        if (surf->m_is_backbuffer) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_back_fbo); // the back buffer (our own framebuffer; 0 only if it could not be made)
            m_fbo_rendering = false;
        } else {
            m_fbo_rendering = true;
            ApplyFboAttachments();
        }
        // glFrontFace is set for each draw (choose_front_face)

        // Reset viewport to match target size
        D3DVIEWPORT9 vp;
        vp.X = 0; vp.Y = 0;
        vp.Width = surf->m_width;
        vp.Height = surf->m_height;
        vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
        SetViewport(&vp);

        return D3D_OK;
    }
    
    virtual HRESULT GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override {
        if (ppZStencilSurface) {
            *ppZStencilSurface = m_current_ds;
            if (m_current_ds) {
                m_current_ds->AddRef(); // Proper COM semantics: caller must Release
            }
        }
        return D3D_OK;
    }
    
    virtual HRESULT SetDepthStencilSurface(IDirect3DSurface9* pZStencilSurface) override {
        // If backbuffer is active, we assume default DS. If FBO is active, we attach DS.
        m_current_ds = static_cast<D3D9Surface*>(pZStencilSurface);
        
        if (!m_current_rt->m_is_backbuffer) ApplyFboAttachments();
        return D3D_OK;
    }
    
    // Shader methods
    virtual HRESULT GetVertexShader(IDirect3DVertexShader9** ppShader) override {
        if (ppShader) *ppShader = nullptr;
        return D3D_OK;
    }
    
    virtual HRESULT GetPixelShader(IDirect3DPixelShader9** ppShader) override {
        if (ppShader) *ppShader = nullptr;
        return D3D_OK;
    }
    
    virtual HRESULT CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) override {
        if (ppShader) *ppShader = nullptr;
        return D3D_OK;
    }
    
    virtual HRESULT CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) override {
        D3D9PixelShader* shader = new D3D9PixelShader();
        static int logged = 0;
        size_t words = 0;
        unsigned int hash = 0;

        if (pFunction && pFunction[0] == 0xFFFF0200) {
            // ps_2_0: the length is in every instruction token; FNV-1a over the words up to the end token, as the generator does
            size_t pos = 1;
            while (pos < 4096) {
                DWORD t = pFunction[pos++];
                DWORD op = t & 0xFFFF;
                if (op == 0xFFFF) break;
                if (op == 0xFFFE) pos += (t >> 16) & 0x7FFF; else pos += (t >> 24) & 0xF;
            }
            words = pos;
            hash = 0x811C9DC5u;
            for (size_t i = 0; i < words; i++) {
                for (int b = 0; b < 4; b++) hash = (hash ^ ((pFunction[i] >> (8 * b)) & 0xFF)) * 0x01000193u;
            }
            for (size_t i = 0; i < sizeof(known_pixel_shaders) / sizeof(*known_pixel_shaders); i++) {
                if (known_pixel_shaders[i].hash == hash) { shader->m_id = known_pixel_shaders[i].id; break; }
            }
        }
        if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 120) {
            printf("[D3D9] pixel shader created: version %08x, %u words, hash %08x -> %s\n", pFunction ? (unsigned) pFunction[0] : 0u, (unsigned) words, hash,
                   shader->m_id ? known_pixel_shaders[shader->m_id - 1].name : "not translated: the fixed function pipeline draws instead");
        }
        if (ppShader) *ppShader = shader; else shader->Release();
        return D3D_OK;
    }

    virtual HRESULT SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override {
        if (pConstantData && StartRegister < 32) {
            UINT count = Vector4fCount;
            if (StartRegister + count > 32) count = 32 - StartRegister;
            memcpy(&g_ps_constants[StartRegister * 4], pConstantData, count * 4 * sizeof(float));
        }
        return D3D_OK;
    }
    
    // Drawing methods
#ifdef SRW_DIAGNOSTICS
    void trace_draw(const char* kind, unsigned count) {
        if (!g_trace_frame) return;
        printf("[D3D9] trace: %s %u, fvf %x, rt %s %ux%u, viewport %u,%u %ux%u, tex0 %u, blend %u (%u,%u), z %u/%u, alphatest %u, scissor %u, cull %u, ps %d\n", kind, count, (unsigned) g_current_fvf,
               m_current_rt && m_current_rt->m_is_backbuffer ? "back buffer" : "texture", m_current_rt ? (unsigned) m_current_rt->m_width : 0, m_current_rt ? (unsigned) m_current_rt->m_height : 0,
               (unsigned) g_viewport.X, (unsigned) g_viewport.Y, (unsigned) g_viewport.Width, (unsigned) g_viewport.Height, (unsigned) g_texture_stages[0],
               (unsigned) m_render_states[D3DRS_ALPHABLENDENABLE], (unsigned) m_render_states[D3DRS_SRCBLEND], (unsigned) m_render_states[D3DRS_DESTBLEND],
               (unsigned) m_render_states[D3DRS_ZENABLE], (unsigned) m_render_states[D3DRS_ZWRITEENABLE], (unsigned) m_render_states[D3DRS_ALPHATESTENABLE],
               (unsigned) m_render_states[D3DRS_SCISSORTESTENABLE], (unsigned) m_render_states[D3DRS_CULLMODE], g_current_ps_id);
    }
#else
    void trace_draw(const char*, unsigned) {}
#endif
    virtual HRESULT DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override {
        PERF_DRAW_TIMER
        SRW_DRAW_STAT(PrimitiveCount);
        trace_draw("DrawPrimitive", PrimitiveCount);
        // Non-indexed primitive drawing using hardware VB (used by cloth rendering)
        if (m_current_vertex_buffer == 0) return -1;
        if (g_shader_program == 0) init_shaders();
        glUseProgram(g_shader_program);

        // FBO Y-flip and clip plane uniforms
        glUniform1i(g_fbo_flip_y_uniform, m_fbo_rendering ? 1 : 0);
        glUniform1i(g_clip_plane_enabled_uniform, m_clip_plane_enabled ? 1 : 0);
        if (m_clip_plane_enabled) UploadClipPlanes();

        D3DMATRIX view_model, mvp;
        if (g_current_fvf & D3DFVF_XYZRHW) {
            float vw = (g_viewport.Width > 0) ? (float)g_viewport.Width : (float)g_logical_width;
            float vh = (g_viewport.Height > 0) ? (float)g_viewport.Height : (float)g_logical_height;
            memset(&mvp, 0, sizeof(D3DMATRIX));
            mvp.m[0][0] = 2.0f / vw;
            mvp.m[1][1] = -2.0f / vh;
            mvp.m[2][2] = 1.0f;
            mvp.m[3][0] = -1.0f;
            mvp.m[3][1] = 1.0f;
            mvp.m[3][3] = 1.0f;
        } else {
            matrix_multiply(&view_model, &m_world, &m_view);
            matrix_multiply(&mvp, &view_model, &m_projection);
        }

        // winding: see choose_front_face
        glFrontFace(choose_front_face(view_model, m_fbo_rendering, (g_current_fvf & D3DFVF_XYZRHW) != 0));

        gl_set_matrix_uniform(g_mvp_uniform, &mvp);

        // Set texture factor
        if (g_texture_factor_uniform != -1) {
            float r = ((m_texture_factor >> 16) & 0xFF) / 255.0f;
            float g = ((m_texture_factor >> 8) & 0xFF) / 255.0f;
            float b = (m_texture_factor & 0xFF) / 255.0f;
            float a = ((m_texture_factor >> 24) & 0xFF) / 255.0f;
            glUniform4f(g_texture_factor_uniform, r, g, b, a);
        }

        // mipmap LOD bias (D3D9 stores the float as a DWORD), with an offset of -0.75: WebGL selects lower resolution levels
        if (g_lod_bias_uniform != -1) {
            DWORD bias_bits = g_sampler_states[0][D3DSAMP_MIPMAPLODBIAS];
            float lod_bias;
            memcpy(&lod_bias, &bias_bits, sizeof(float));
            lod_bias -= 0.75f;
            glUniform1f(g_lod_bias_uniform, lod_bias);
        }

        // Fog uniforms
        {
            bool apply_fog = m_fog_enabled && !(g_current_fvf & D3DFVF_XYZRHW);
            glUniform1i(g_fog_enabled_uniform, apply_fog ? 1 : 0);
            if (apply_fog) {
                float r = ((m_fog_color >> 16) & 0xFF) / 255.0f;
                float g_c = ((m_fog_color >> 8) & 0xFF) / 255.0f;
                float b = (m_fog_color & 0xFF) / 255.0f;
                glUniform3f(g_fog_color_uniform, r, g_c, b);
                glUniform1f(g_fog_start_uniform, m_fog_start);
                glUniform1f(g_fog_end_uniform, m_fog_end);
                gl_set_matrix_uniform(g_view_model_uniform, &view_model);
            }
        }

        // Set Stage 0 texture stage states
        if (g_stage0_colorop_uniform != -1)
            glUniform1i(g_stage0_colorop_uniform, g_texture_stage_states[0][D3DTSS_COLOROP]);
        if (g_stage0_colorarg1_uniform != -1)
            glUniform1i(g_stage0_colorarg1_uniform, g_texture_stage_states[0][D3DTSS_COLORARG1]);
        if (g_stage0_colorarg2_uniform != -1)
            glUniform1i(g_stage0_colorarg2_uniform, g_texture_stage_states[0][D3DTSS_COLORARG2]);
        if (g_stage0_alphaop_uniform != -1)
            glUniform1i(g_stage0_alphaop_uniform, g_texture_stage_states[0][D3DTSS_ALPHAOP]);
        if (g_stage0_alphaarg1_uniform != -1)
            glUniform1i(g_stage0_alphaarg1_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG1]);
        if (g_stage0_alphaarg2_uniform != -1)
            glUniform1i(g_stage0_alphaarg2_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG2]);

        glBindBuffer(GL_ARRAY_BUFFER, m_current_vertex_buffer);

        // FVF Parsing
        bool has_xyz = (g_current_fvf & D3DFVF_XYZ) != 0;
        bool has_xyzrhw = (g_current_fvf & D3DFVF_XYZRHW) != 0;
        bool has_normal = (g_current_fvf & D3DFVF_NORMAL) != 0;
        bool has_diffuse = (g_current_fvf & D3DFVF_DIFFUSE) != 0;
        int tex_count = (g_current_fvf >> 8) & 0xF;

        // Calculate offsets accounting for all FVF components
        size_t offset = 0;
        size_t position_offset = 0;
        size_t normal_offset = 0;
        size_t color_offset = 0;
        size_t tex_offset = 0;

        if (has_xyz) { position_offset = offset; offset += 12; }
        if (has_xyzrhw) { position_offset = offset; offset += 16; }
        if (has_normal) { normal_offset = offset; offset += 12; }
        if (has_diffuse) { color_offset = offset; offset += 4; }
        tex_offset = offset;

        GLsizei stride = m_current_stride;

        if ((has_xyz || has_xyzrhw) && g_position_location != -1) {
            glEnableVertexAttribArray(g_position_location);
            glVertexAttribPointer(g_position_location, has_xyzrhw ? 4 : 3, GL_FLOAT, GL_FALSE, stride, (void*)(m_current_vb_offset + position_offset));
        }

        // Normal attribute for FFP lighting
        if (has_normal && g_normal_location != -1) {
            glEnableVertexAttribArray(g_normal_location);
            glVertexAttribPointer(g_normal_location, 3, GL_FLOAT, GL_FALSE, stride, (void*)(m_current_vb_offset + normal_offset));
        } else if (g_normal_location != -1) {
            glDisableVertexAttribArray(g_normal_location);
            glVertexAttrib3f(g_normal_location, 0.0f, 0.0f, 1.0f);
        }

        // D3D9 FFP Lighting
        bool lighting_enabled = has_normal && !has_xyzrhw && m_render_states[D3DRS_LIGHTING];
        if (lighting_enabled) {
            glUniform1i(g_lighting_enabled_uniform, 1);
            SetFFPLightingUniforms(&m_world, m_render_states, has_diffuse);

            glDisableVertexAttribArray(g_color_location);
            glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f);
            if (has_diffuse && g_color_location != -1) {
                glEnableVertexAttribArray(g_color_location);
                glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)(m_current_vb_offset + color_offset));
            }
        } else {
            glUniform1i(g_lighting_enabled_uniform, 0);
            // Always set world matrix for clip plane (v_position_world) even without lighting
            gl_set_matrix_uniform(g_world_uniform, &m_world);

            if (has_diffuse && g_color_location != -1) {
                glEnableVertexAttribArray(g_color_location);
                glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)(m_current_vb_offset + color_offset));
            } else if (g_color_location != -1) {
                glDisableVertexAttribArray(g_color_location);
                glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        bind_texcoords(g_current_fvf, stride, m_current_vb_offset + tex_offset);

        // Texture setup
        if (g_texture_stages[0] != 0 && tex_count > 0) {
            glActiveTexture(GL_TEXTURE0);
            if (g_cached_texture != g_texture_stages[0]) {
                glBindTexture(GL_TEXTURE_2D, g_texture_stages[0]);
                g_cached_texture = g_texture_stages[0];
            }
            ApplySamplerStates(0);
            glUniform1i(g_texture_uniform, 0);
            glUniform1i(g_use_texture_uniform, 1);
        } else {
            glUniform1i(g_use_texture_uniform, 0);
        }

        // Alpha test
        glUniform1i(g_alpha_test_enabled_uniform, m_alpha_test_enabled ? 1 : 0);
        glUniform1f(g_alpha_ref_uniform, (float)m_alpha_ref);
        UploadTextureStages(view_model);

        // Disable lightmap
        glUniform1i(g_use_lightmap_uniform, 0);

        GLenum type = GL_TRIANGLES;
        UINT vcount = 0;
        if (PrimitiveType == D3DPT_TRIANGLELIST) { type = GL_TRIANGLES; vcount = PrimitiveCount * 3; }
        else if (PrimitiveType == D3DPT_TRIANGLESTRIP) { type = GL_TRIANGLE_STRIP; vcount = PrimitiveCount + 2; }
        else if (PrimitiveType == D3DPT_TRIANGLEFAN) { type = GL_TRIANGLE_FAN; vcount = PrimitiveCount + 2; }

        glDrawArrays(type, StartVertex, vcount);

        return D3D_OK;
    }
    
    // Query and utility methods
    virtual HRESULT CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override {
        if (ppQuery) *ppQuery = nullptr;
        return D3D_OK;
    }
    
    virtual HRESULT StretchRect(IDirect3DSurface9* pSourceSurface, const void* pSourceRect,
                               IDirect3DSurface9* pDestSurface, const void* pDestRect,
                               D3DTEXTUREFILTERTYPE Filter) override {
        if (!pSourceSurface || !pDestSurface) return D3DERR_INVALIDCALL;

        D3D9Surface* src = static_cast<D3D9Surface*>(pSourceSurface);
        D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);
        LOG_SURFACE_CALL(stretch, "StretchRect: %s %ux%u (%s) -> %s %ux%u (%s), source rect %s, destination rect %s", src->m_is_backbuffer ? "back buffer" : "surface", (unsigned) src->m_width, (unsigned) src->m_height,
                         src->m_owner ? "texture" : "no texture", dst->m_is_backbuffer ? "back buffer" : "surface", (unsigned) dst->m_width, (unsigned) dst->m_height, dst->m_owner ? "texture" : "no texture",
                         pSourceRect ? "yes" : "none", pDestRect ? "yes" : "none");

        // Two framebuffers of their own with nothing but the color attachment (the render target framebuffer
        // g_fbo keeps its depth buffer, of another size than the destination: "incomplete dimensions", the copy of the multisampled back
        // buffer for the glow failed); the rectangles as given; the scissor test does not apply (D3D9). The multisampled back buffer is
        // resolved into framebuffer 0 first; a copy into it is not possible (it lands in framebuffer 0, which the next resolve overwrites).
        static GLuint blit_fbo[2] = {0, 0};
        static unsigned blit_generation = 0;
        if (blit_fbo[0] == 0 || blit_generation != g_gl_generation) {
            glGenFramebuffers(2, blit_fbo);
            blit_generation = g_gl_generation;
        }
        if (src->m_is_backbuffer && g_back_samples > 0) back_buffer_resolve();
        if (dst->m_is_backbuffer && g_back_samples > 0) LOG_SURFACE_CALL(stretchms, "StretchRect into the multisampled back buffer: lost");
        auto attach = [](GLenum target, D3D9Surface* surface) {
            if (surface->m_is_renderbuffer) glFramebufferRenderbuffer(target, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, surface->m_gl_object);
            else glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface->m_gl_object, 0);
        };
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src->m_is_backbuffer ? back_buffer_read_fbo() : blit_fbo[0]);
        if (!src->m_is_backbuffer) attach(GL_READ_FRAMEBUFFER, src);
        // (into the back buffer: our own framebuffer when it has one sample, a blit into a multisampled one is not allowed; with more samples it lands in framebuffer 0)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->m_is_backbuffer ? (g_back_samples == 0 ? g_back_fbo : 0) : blit_fbo[1]);
        if (!dst->m_is_backbuffer) attach(GL_DRAW_FRAMEBUFFER, dst);

        GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (read_status != GL_FRAMEBUFFER_COMPLETE || draw_status != GL_FRAMEBUFFER_COMPLETE) {
            static int logged = 0;
            if (logged++ < 10) printf("[D3D9] StretchRect: framebuffer incomplete (read %X, draw %X)\n", read_status, draw_status);
        } else {
            // the rectangles (D3D: rows from the top) in GL's rows: the back buffer is stored bottom up, render target textures top down
            struct Rect32 { int32_t left, top, right, bottom; };
            Rect32 sr = {0, 0, (int32_t) src->m_width, (int32_t) src->m_height}, dr = {0, 0, (int32_t) dst->m_width, (int32_t) dst->m_height};
            if (pSourceRect) sr = *(const Rect32*) pSourceRect;
            if (pDestRect) dr = *(const Rect32*) pDestRect;
            const GLint s_top = src->m_is_backbuffer ? (GLint) src->m_height - sr.top : sr.top;
            const GLint s_bottom = src->m_is_backbuffer ? (GLint) src->m_height - sr.bottom : sr.bottom;
            const GLint d_top = dst->m_is_backbuffer ? (GLint) dst->m_height - dr.top : dr.top;
            const GLint d_bottom = dst->m_is_backbuffer ? (GLint) dst->m_height - dr.bottom : dr.bottom;
            if (g_cached_scissor_test) glDisable(GL_SCISSOR_TEST);
#ifdef SRW_DIAGNOSTICS
            // (the copy of the finished 3D picture that the screen effects start from: sampled before and after, 16 x 12 pixels of what is read)
            static int copies = 0;
            const bool sample_copy = SRW_LOG_ON(SRW_LOG_D3D) && src->m_is_backbuffer && dst->m_width >= 512 && (copies++ % 60) == 0;
            auto sample = [&](const char* what, GLuint read_fbo, GLint width, GLint height) {
                int nonblack = 0;
                unsigned long sum_r = 0, sum_g = 0, sum_b = 0;

                glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
                for (int y = 0; y < 12; y++) for (int x = 0; x < 16; x++) {
                    unsigned char px[4] = {0, 0, 0, 0};
                    glReadPixels(width * (2 * x + 1) / 32, height * (2 * y + 1) / 24, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    if (px[0] || px[1] || px[2]) nonblack++;
                    sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
                }
                printf("[D3D9] StretchRect %s: %d of 192 sampled pixels are not black, mean %lu %lu %lu\n", what, nonblack, sum_r / 192, sum_g / 192, sum_b / 192);
            };
            if (sample_copy) sample("source (the back buffer)", 0, (GLint) src->m_width, (GLint) src->m_height);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src->m_is_backbuffer ? back_buffer_read_fbo() : blit_fbo[0]);
#endif
            glBlitFramebuffer(sr.left, s_top, sr.right, s_bottom, dr.left, d_top, dr.right, d_bottom, GL_COLOR_BUFFER_BIT,
                              Filter == D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
#ifdef SRW_DIAGNOSTICS
            if (sample_copy && !dst->m_is_backbuffer) sample("destination (the copy)", blit_fbo[1], (GLint) dst->m_width, (GLint) dst->m_height);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src->m_is_backbuffer ? back_buffer_read_fbo() : blit_fbo[0]);
#endif
            if (g_cached_scissor_test) glEnable(GL_SCISSOR_TEST);
        }

        // Restore original RT binding
        if (m_current_rt->m_is_backbuffer) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_back_fbo);
        } else {
            ApplyFboAttachments();
        }
        
        return D3D_OK;
    }
    
    virtual HRESULT Clear(DWORD Count, const void* pRects, DWORD Flags,
                         D3DCOLOR Color, FLOAT Z, DWORD Stencil) override {
        float r = ((Color >> 16) & 0xFF) / 255.0f;
        float g = ((Color >> 8) & 0xFF) / 255.0f;
        float b = (Color & 0xFF) / 255.0f;
        float a = ((Color >> 24) & 0xFF) / 255.0f;

#ifdef SRW_DIAGNOSTICS
        {
            static int logged = 0;
            if (SRW_LOG_ON(SRW_LOG_D3D) && (logged++ < 60 || g_trace_frame)) {
                const int32_t *rc = (const int32_t *) pRects;
                printf("[D3D9] Clear: flags %x, %u rects%s, color %08x z %.2f stencil %u, viewport %u,%u %ux%u, target %dx%d (%s), scissor test %d box %d,%d,%d,%d\n", (unsigned) Flags, (unsigned) Count,
                       Count > 0 && rc ? "" : " (the whole viewport)", (unsigned) Color, (double) Z, (unsigned) Stencil, (unsigned) g_viewport.X, (unsigned) g_viewport.Y, (unsigned) g_viewport.Width,
                       (unsigned) g_viewport.Height, m_current_rt ? (int) m_current_rt->m_width : g_logical_width, m_current_rt ? (int) m_current_rt->m_height : g_logical_height,
                       m_current_rt && m_current_rt->m_is_backbuffer ? "back buffer" : "texture", (int) g_cached_scissor_test, g_cached_scissor_box[0], g_cached_scissor_box[1], g_cached_scissor_box[2], g_cached_scissor_box[3]);
                if (Count > 0 && rc) for (DWORD i = 0; i < Count && i < 4; i++) printf("[D3D9]   rect %u: %d,%d - %d,%d\n", (unsigned) i, rc[4 * i], rc[4 * i + 1], rc[4 * i + 2], rc[4 * i + 3]);
            }
        }
#endif
        glClearColor(r, g, b, a);
        glClearDepthf(Z);
        glClearStencil(Stencil);

        GLbitfield mask = 0;
        if (Flags & D3DCLEAR_TARGET) mask |= GL_COLOR_BUFFER_BIT;
        if (Flags & D3DCLEAR_ZBUFFER) mask |= GL_DEPTH_BUFFER_BIT;
        if (Flags & D3DCLEAR_STENCIL) mask |= GL_STENCIL_BUFFER_BIT;

        // Save current mask states from tracked render states (no GPU readback)
        GLboolean color_write_enabled_r = (m_render_states[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_RED) ? GL_TRUE : GL_FALSE;
        GLboolean color_write_enabled_g = (m_render_states[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_GREEN) ? GL_TRUE : GL_FALSE;
        GLboolean color_write_enabled_b = (m_render_states[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_BLUE) ? GL_TRUE : GL_FALSE;
        GLboolean color_write_enabled_a = (m_render_states[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_ALPHA) ? GL_TRUE : GL_FALSE;
        GLboolean depth_write_enabled = m_render_states[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE;

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);

        // D3D9 clears the viewport only, or the given rectangles within it; glClear clears the whole
        // framebuffer (within the scissor box when the scissor test is on, as D3D9 does too). A clear of a smaller viewport (a model
        // frame, the world map) is limited with the scissor box; a clear of the whole target stays a plain glClear.
        {
            struct ClearRect { int32_t x1, y1, x2, y2; };
            const ClearRect *rects = (const ClearRect *) pRects;
            const int target_width = m_current_rt ? (int) m_current_rt->m_width : g_logical_width;
            const int target_height = m_current_rt ? (int) m_current_rt->m_height : g_logical_height;
            const int vx1 = (int) g_viewport.X, vy1 = (int) g_viewport.Y;
            const int vx2 = (int) std::min<int64_t>((int64_t) g_viewport.X + g_viewport.Width, target_width);
            const int vy2 = (int) std::min<int64_t>((int64_t) g_viewport.Y + g_viewport.Height, target_height);
            const DWORD count = (rects != nullptr && Count > 0) ? Count : 1;
            bool scissored = false;

            for (DWORD i = 0; i < count; i++) {
                int x1 = vx1, y1 = vy1, x2 = vx2, y2 = vy2;
                if (rects != nullptr && Count > 0) {
                    x1 = std::max(x1, (int) rects[i].x1); y1 = std::max(y1, (int) rects[i].y1);
                    x2 = std::min(x2, (int) rects[i].x2); y2 = std::min(y2, (int) rects[i].y2);
                }
                if (x1 >= x2 || y1 >= y2) continue;
                if (x1 == 0 && y1 == 0 && x2 == target_width && y2 == target_height && !g_cached_scissor_test) {
                    CHECK_GL_ERROR(glClear(mask));
                    continue;
                }
                // the box in GL's coordinates (rows from the bottom), within the scissor box when the scissor test is on
                int gx1 = x1, gy1 = target_height - y2, gx2 = x2, gy2 = target_height - y1;
                if (g_cached_scissor_test) {
                    gx1 = std::max(gx1, (int) g_cached_scissor_box[0]); gy1 = std::max(gy1, (int) g_cached_scissor_box[1]);
                    gx2 = std::min(gx2, (int) (g_cached_scissor_box[0] + g_cached_scissor_box[2]));
                    gy2 = std::min(gy2, (int) (g_cached_scissor_box[1] + g_cached_scissor_box[3]));
                    if (gx1 >= gx2 || gy1 >= gy2) continue;
                }
                if (!scissored && !g_cached_scissor_test) glEnable(GL_SCISSOR_TEST);
                scissored = true;
                glScissor(gx1, gy1, gx2 - gx1, gy2 - gy1);
                CHECK_GL_ERROR(glClear(mask));
            }
            if (scissored) {
                if (g_cached_scissor_test) glScissor(g_cached_scissor_box[0], g_cached_scissor_box[1], g_cached_scissor_box[2], g_cached_scissor_box[3]);
                else glDisable(GL_SCISSOR_TEST);
            }
        }

        // Restore masks from tracked state (no GPU readback)
        glColorMask(color_write_enabled_r, color_write_enabled_g, color_write_enabled_b, color_write_enabled_a);
        glDepthMask(depth_write_enabled);

        return D3D_OK;
    }
    
    virtual HRESULT BeginScene() override {
        return D3D_OK;
    }
    
    virtual HRESULT EndScene() override {
        return D3D_OK;
    }
    
    virtual HRESULT Present(const void* pSourceRect, const void* pDestRect,
                           HWND hDestWindowOverride, const void* pDirtyRegion) override {
        // the context runs in a worker with explicit swap control: the frame is handed to the page here
#ifdef SRW_DIAGNOSTICS
        struct PerfPresent {
            double start = emscripten_get_now();
            ~PerfPresent() {
                static double window_start = start;
                static unsigned int frames, skipped_reports;
                double now = emscripten_get_now();

                g_perf_present_ms += now - start;
                frames++;
                if (now - window_start >= 5000.0) {
                    double span = now - window_start;
                    double game = span - g_perf_draw_ms - g_perf_present_ms;

                    if (SRW_LOG_ON(SRW_LOG_PERF)) printf("[D3D9] perf: %.1f fps, per frame: %.1f ms total = draws %.1f ms (%.0f calls, %.1f us each) + present %.1f ms (commit %.1f ms, read-back %.1f ms) + game and everything else %.1f ms\n",
                           frames * 1000.0 / span, span / frames, g_perf_draw_ms / frames, g_perf_draws / (double) frames,
                           g_perf_draws ? g_perf_draw_ms * 1000.0 / g_perf_draws : 0.0, g_perf_present_ms / frames, g_perf_commit_ms / frames, g_perf_readback_ms / frames, game / frames);
                    if (SRW_LOG_ON(SRW_LOG_PERF)) {
                        char report[1400];
                        describe_graphics(report, sizeof(report), true);
                        printf("[D3D9] gfx: %s\n", report);
                    }
                    memset(&g_stats_window, 0, sizeof(g_stats_window)); g_stats_window_frames = 0;
                    window_start = now; frames = 0; g_perf_draw_ms = g_perf_present_ms = g_perf_readback_ms = g_perf_commit_ms = 0; g_perf_draws = 0; (void) skipped_reports;
                }
            }
        } perf_present;
#endif
        stats_end_frame();
        {
            // Debugging aid: now and then the frame is sampled, to tell "nothing was drawn" from "it does not reach the page"
            static int frames = 0;

            frames++;
            (void) frames;
#ifdef SRW_DIAGNOSTICS
            if (SRW_LOG_ON(SRW_LOG_D3D) && frames % 120 == 0) {   // (samples the frame: a read-back)
                const int columns = 16, rows = 12;
                int nonblack = 0;
                unsigned long sum_r = 0, sum_g = 0, sum_b = 0;

                for (int y = 0; y < rows; y++) {
                    for (int x = 0; x < columns; x++) {
                        unsigned char px[4] = {0, 0, 0, 0};
                        glReadPixels((g_logical_width * (2 * x + 1)) / (2 * columns), (g_logical_height * (2 * y + 1)) / (2 * rows), 1, 1,
                                     GL_RGBA, GL_UNSIGNED_BYTE, px);
                        if (px[0] || px[1] || px[2]) nonblack++;
                        sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
                    }
                }
                printf("[D3D9] frame %d: %d of %d sampled pixels are not black, mean color %lu %lu %lu (backbuffer %ux%u)\n", frames, nonblack,
                       columns * rows, sum_r / (columns * rows), sum_g / (columns * rows), sum_b / (columns * rows), (unsigned) g_logical_width,
                       (unsigned) g_logical_height);
            }
#endif
        }
#ifdef SRW_DIAGNOSTICS
        {
            static int probes = 0;
            static int trace_frames = 0;
            probe_backbuffer("at Present", &probes, 4, 120);
            g_probe_ui_done = false;
            trace_frames++;
            g_trace_frame = SRW_LOG_ON(SRW_LOG_D3D) && (trace_frames == 149 || trace_frames == 150);
        }
#endif
        if (g_debug_mode == 1) {
            glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        // the swap (commit) is only for the ImageBitmap way; in the pixels way it just waited for the browser (~70 ms per frame)
#ifdef SRW_DIAGNOSTICS
        double commit_start = emscripten_get_now();
#endif
        back_buffer_resolve();
        if (!present_as_pixels()) emscripten_webgl_commit_frame();
#ifdef SRW_DIAGNOSTICS
        g_perf_commit_ms += emscripten_get_now() - commit_start;
#endif
        {
            // frames that are not shown because the page has not shown the ones before: a run of them means the page has stopped
            static int skipped_in_a_row, reported;
            if (present_as_pixels() ? present_pixels_frame(g_logical_width, g_logical_height) : srw_present_frame((int) (intptr_t) &g_frames_in_flight)) {
                skipped_in_a_row = 0;
            } else if (++skipped_in_a_row == 60 && reported++ < 10) {
                printf("[D3D9] 60 frames in a row were not shown (%d in flight, canvas %dx%d).\n", (int) g_frames_in_flight, g_logical_width, g_logical_height);
            }
        }
        return D3D_OK;
    }
    
static GLint gl_address_mode(DWORD mode) {
    if (mode >= 4 && SRW_LOG_ON(SRW_LOG_D3D)) {
        static int logged = 0;
        if (logged++ < 10) printf("[D3D9] texture address mode %u (clamped to the edge)\n", (unsigned) mode);
    }
    switch (mode) {
    case 2: return GL_MIRRORED_REPEAT;                       // D3DTADDRESS_MIRROR
    case 3: case 4: case 5: return GL_CLAMP_TO_EDGE;         // CLAMP, BORDER (no border color in WebGL), MIRRORONCE
    default: return GL_REPEAT;                               // WRAP
    }
}

static void ApplySamplerStates(DWORD Sampler) {
    if (Sampler >= 8) return;

    // Address U and V: D3DTADDRESS_WRAP 1, MIRROR 2, CLAMP 3, BORDER 4, MIRRORONCE 5. WebGL has no border color: BORDER and MIRRORONCE
    // clamp to the edge (they repeated before: the movie's tiles showed the opposite edge as lines at the sides of the screen)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_address_mode(g_sampler_states[Sampler][D3DSAMP_ADDRESSU]));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_address_mode(g_sampler_states[Sampler][D3DSAMP_ADDRESSV]));
    
    // Mag Filter (mipmaps don't affect magnification)
    GLint mag_filter = GL_LINEAR;
    if (g_sampler_states[Sampler][D3DSAMP_MAGFILTER] == D3DTEXF_POINT) mag_filter = GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);

    // Min Filter: combine D3D9 MINFILTER + MIPFILTER into GL enum.
    // D3D9 has separate MINFILTER and MIPFILTER states, but OpenGL combines
    // them into a single GL_TEXTURE_MIN_FILTER value:
    //   MIN=LINEAR  + MIP=LINEAR  → GL_LINEAR_MIPMAP_LINEAR   (trilinear)
    //   MIN=LINEAR  + MIP=POINT   → GL_LINEAR_MIPMAP_NEAREST  (bilinear)
    //   MIN=POINT   + MIP=LINEAR  → GL_NEAREST_MIPMAP_LINEAR
    //   MIN=POINT   + MIP=POINT   → GL_NEAREST_MIPMAP_NEAREST
    //   MIN=*       + MIP=NONE    → GL_LINEAR / GL_NEAREST    (no mip)
    DWORD d3d_min = g_sampler_states[Sampler][D3DSAMP_MINFILTER];
    DWORD d3d_mip = g_sampler_states[Sampler][D3DSAMP_MIPFILTER];

    GLuint current_tex = g_texture_stages[Sampler];
    bool has_mipmaps = (current_tex != 0) && g_mipmapped_textures.count(current_tex);

    GLint min_filter;
    if (has_mipmaps && d3d_mip != D3DTEXF_NONE) {
        if (d3d_min == D3DTEXF_POINT) {
            min_filter = (d3d_mip == D3DTEXF_LINEAR) ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        } else {
            min_filter = (d3d_mip == D3DTEXF_LINEAR) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
        }
    } else {
        min_filter = (d3d_min == D3DTEXF_POINT) ? GL_NEAREST : GL_LINEAR;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);

    // Anisotropic filtering as the program asks: a minification filter D3DTEXF_ANISOTROPIC with D3DSAMP_MAXANISOTROPY samples, within what WebGL allows.
    if (g_max_anisotropy > 1.0f) {
        float anisotropy = 1.0f;
        if (has_mipmaps && d3d_mip != D3DTEXF_NONE &&
            (d3d_min == D3DTEXF_ANISOTROPIC || g_sampler_states[Sampler][D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC)) {
            anisotropy = (float) g_sampler_states[Sampler][D3DSAMP_MAXANISOTROPY];
            if (anisotropy < 1.0f) anisotropy = 1.0f;
            if (anisotropy > g_max_anisotropy) anisotropy = g_max_anisotropy;
        }
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
        const bool asked = d3d_min == D3DTEXF_ANISOTROPIC || g_sampler_states[Sampler][D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC;
        if (asked) {
            g_stats.aniso_asked++;
            if (anisotropy <= 1.0f && !(has_mipmaps && d3d_mip != D3DTEXF_NONE)) g_stats.aniso_no_mips++;
            if (g_sampler_states[Sampler][D3DSAMP_MAXANISOTROPY] > g_stats.max_aniso_state) g_stats.max_aniso_state = (unsigned) g_sampler_states[Sampler][D3DSAMP_MAXANISOTROPY];
        }
        g_stats.aniso_applied[anisotropy >= 16.0f ? 4 : anisotropy >= 8.0f ? 3 : anisotropy >= 4.0f ? 2 : anisotropy >= 2.0f ? 1 : 0]++;
#ifdef SRW_DIAGNOSTICS
        if (SRW_LOG_ON(SRW_LOG_D3D)) {
            // the parameter as WebGL has it, for the texture bound now (a check of the setting, a few times in all)
            static int checks = 0;
            if (asked && checks < 12 && (checks == 0 || (g_stats_window_frames % 120) == 0)) {
                float stored = -1.0f;
                glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &stored);
                printf("[D3D9] anisotropy check: sampler %u, texture %u, set %.0f, read back %.0f (game's state %u, extension max %.0f, mipmaps %d)\n", (unsigned) Sampler,
                       (unsigned) current_tex, anisotropy, stored, (unsigned) g_sampler_states[Sampler][D3DSAMP_MAXANISOTROPY], g_max_anisotropy, (int) has_mipmaps);
                checks++;
            }
        }
#endif
    }
}

// Uploads the texture stages for a draw: binds the textures of stages 0 to 3 (units 0 to 3), their sampler states and the operations
// of the cascade. A stage after a disabled one is not used, as in Direct3D.
void UploadTextureStages(const D3DMATRIX& view_model) {
    glUniform1i(g_alpha_func_uniform, (GLint) m_render_states[25]);   // D3DRS_ALPHAFUNC
    {
        // alpha to coverage: the alpha test (greater, greater or equal) of a draw without blending into the multisampled back buffer
        const DWORD func = m_render_states[25];
        const bool coverage = g_back_samples > 0 && !m_fbo_rendering && m_alpha_test_enabled && !m_render_states[D3DRS_ALPHABLENDENABLE] &&
                              (func == D3DCMP_GREATER || func == D3DCMP_GREATEREQUAL) && alpha_coverage_wanted();
        if (m_fbo_rendering) g_stats.render_target_draws++;
        if (m_alpha_test_enabled) {
            g_stats.alpha_test_draws++;
            if (coverage) g_stats.alpha_coverage_draws++;
            else if (m_render_states[D3DRS_ALPHABLENDENABLE]) g_stats.alpha_blended_draws++;
            else if (func != D3DCMP_GREATER && func != D3DCMP_GREATEREQUAL) g_stats.alpha_other_func_draws++;
            else g_stats.alpha_no_samples_draws++;
        }
        if (g_current_ps_id != 0) g_stats.pixel_shader_draws++;
        if (m_render_states[D3DRS_SPECULARENABLE]) g_stats.specular_enable_draws++;
        if (g_current_fvf & 0x80) g_stats.specular_vertex_draws++;   // D3DFVF_SPECULAR (the header of the wrapper has the wrong value for it)
        if (m_render_states[D3DRS_LIGHTING] && (g_current_fvf & D3DFVF_NORMAL)) g_stats.lit_draws++;
        if (coverage != g_alpha_to_coverage_on) {
            if (coverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            g_alpha_to_coverage_on = coverage;
        }
        glUniform1i(g_alpha_to_coverage_uniform, coverage ? 1 : 0);
    }
    const bool ps_on = g_current_ps_id != 0;
#ifdef SRW_DIAGNOSTICS
    if (SRW_LOG_ON(SRW_LOG_D3D)) {
        // draws into render target textures and draws that sample them, reported every five seconds
        struct RtStat { unsigned drawn = 0, sampled = 0, width = 0, height = 0, fvf = 0, last_prims = 0; };
        static std::map<GLuint, RtStat> stats;
        static double last_report = 0.0;

        if (m_fbo_rendering && m_current_rt) {
            RtStat& st = stats[m_current_rt->m_gl_object];
            st.drawn++; st.width = m_current_rt->m_width; st.height = m_current_rt->m_height; st.fvf = g_current_fvf;
        }
        for (int t = 0; t < 4; t++) {
            std::map<GLuint, RtStat>::iterator it = stats.find(g_texture_stages[t]);
            if (g_texture_stages[t] != 0 && it != stats.end()) it->second.sampled++;
        }
        // draws in a viewport much smaller than the target: the 3D models of the interface are drawn like that
        static std::map<std::string, unsigned> small_viewports;
        if (m_current_rt && (g_viewport.Width * 2 < m_current_rt->m_width || g_viewport.Height * 2 < m_current_rt->m_height)) {
            char key[260];
            std::map<GLuint, TextureInfo>::iterator ti = g_texture_info.find(g_texture_stages[0]);
            snprintf(key, sizeof(key), "viewport %u,%u %ux%u (%s), fvf %x, lighting %d blend %d cull %d z test %d write %d, texture %ux%u format %u", (unsigned) g_viewport.X, (unsigned) g_viewport.Y,
                     (unsigned) g_viewport.Width, (unsigned) g_viewport.Height, m_fbo_rendering ? "texture" : "screen", (unsigned) g_current_fvf, (int) m_render_states[D3DRS_LIGHTING],
                     (int) m_render_states[D3DRS_ALPHABLENDENABLE], (int) m_render_states[D3DRS_CULLMODE], (int) m_render_states[D3DRS_ZENABLE], (int) m_render_states[D3DRS_ZWRITEENABLE],
                     ti != g_texture_info.end() ? ti->second.width : 0u, ti != g_texture_info.end() ? ti->second.height : 0u, ti != g_texture_info.end() ? ti->second.format : 0u);
            small_viewports[key]++;
        }
        double now = emscripten_get_now();
        if (now - last_report > 5000.0) {
            last_report = now;
            for (std::map<std::string, unsigned>::iterator it = small_viewports.begin(); it != small_viewports.end(); ++it) printf("[D3D9] %u draws in a small %s\n", it->second, it->first.c_str());
            small_viewports.clear();
            for (std::map<GLuint, RtStat>::iterator it = stats.begin(); it != stats.end(); ++it) {
                printf("[D3D9] render target texture %u (%ux%u): %u draws into it, %u draws sampled it, last fvf %x\n", it->first, it->second.width, it->second.height,
                       it->second.drawn, it->second.sampled, it->second.fvf);
                it->second.drawn = it->second.sampled = 0;
            }
        }
    }
#endif
    if (m_fbo_rendering && m_current_rt) {
        // a render target texture: is the framebuffer complete (with the depth surface the game set)? logged once per target
        static std::set<std::pair<GLuint, void*> > checked;
        std::pair<GLuint, void*> id(m_current_rt->m_gl_object, (void*) m_current_ds);

        if (checked.size() < 40 && checked.insert(id).second) {
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE || SRW_LOG_ON(SRW_LOG_D3D)) printf("[D3D9] render to a texture: %ux%u, depth surface %s, framebuffer status %x (%s)\n", (unsigned) m_current_rt->m_width, (unsigned) m_current_rt->m_height,
                   m_current_ds ? "set" : "none", status, status == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE");
        }
    }
    if (g_half_pixel_uniform != -1) {
        float w = (float) g_viewport.Width, h = (float) g_viewport.Height;
        if (g_half_pixel_enabled && w > 0.0f && h > 0.0f) glUniform2f(g_half_pixel_uniform, 1.0f / w, 1.0f / h);
        else glUniform2f(g_half_pixel_uniform, 0.0f, 0.0f);
    }
    glUniform1i(g_ps_uniform, g_current_ps_id);
    if (ps_on) glUniform4fv(g_ps_c_uniform, 32, g_ps_constants);
    if (g_debug_mode == 18 || g_debug_mode == 19) {
        bool target = m_render_states[D3DRS_ALPHABLENDENABLE] && m_render_states[D3DRS_LIGHTING] && (g_current_fvf & D3DFVF_NORMAL) && (g_current_fvf & D3DFVF_DIFFUSE);

        glUniform1i(glGetUniformLocation(g_shader_program, "u_debug_target"), target ? 1 : 0);
    }
#ifdef SRW_DIAGNOSTICS
    if (g_debug_mode_request != g_debug_mode) {
        g_debug_mode = g_debug_mode_request;
        glUniform1i(glGetUniformLocation(g_shader_program, "u_debug_mode"), g_debug_mode);
        printf("[D3D9] debug mode %d\n", g_debug_mode);
    }
#endif
    static GLint tables[11][4];
    GLint tc_source[8], tc_count[8], tc_projected[8];
    float tc_matrix[8][16];
    bool ended = false, generated = false;

    for (int s = 0; s < 8; s++) {
        const DWORD* st = g_texture_stage_states[s];
        if (s < 4) {   // the fixed function cascade has four stages
            DWORD color_op = st[D3DTSS_COLOROP];
            if (ended || color_op == D3DTOP_DISABLE) { ended = true; color_op = D3DTOP_DISABLE; }
            tables[0][s] = (GLint) color_op;
            tables[1][s] = (GLint) st[D3DTSS_COLORARG0];
            tables[2][s] = (GLint) st[D3DTSS_COLORARG1];
            tables[3][s] = (GLint) st[D3DTSS_COLORARG2];
            tables[4][s] = (GLint) st[D3DTSS_ALPHAOP];
            tables[5][s] = (GLint) st[D3DTSS_ALPHAARG0];
            tables[6][s] = (GLint) st[D3DTSS_ALPHAARG1];
            tables[7][s] = (GLint) st[D3DTSS_ALPHAARG2];
            tables[8][s] = (GLint) st[D3DTSS_RESULTARG];
            tables[9][s] = 0;
            tables[10][s] = (g_texture_stages[s] != 0 && !ended) ? 1 : 0;
        }
        {
            DWORD index = st[D3DTSS_TEXCOORDINDEX], generation = (index >> 16) & 7, transform = st[D3DTSS_TEXTURETRANSFORMFLAGS];
            // 0 to 7 the sets of the vertices, then D3DTSS_TCI_CAMERASPACENORMAL, ...POSITION, ...REFLECTIONVECTOR, SPHEREMAP
            tc_source[s] = generation != 0 ? (GLint) (generation + 7) : (GLint) ((index & 0xFFFF) < 7 ? (index & 0xFFFF) : 7);
            if (generation != 0 && s < 4 && !ended) generated = true;
            tc_count[s] = (GLint) (transform & 0xFF);
            tc_projected[s] = (transform & D3DTTFF_PROJECTED) ? 1 : 0;
            memcpy(tc_matrix[s], &g_texture_matrices[s], sizeof(tc_matrix[s]));
        }
    }
    if (generated && !ps_on) g_stats.texgen_draws++;
    if (ps_on) {
        // a pixel shader replaces the cascade: the stages only carry the textures and the coordinates
        for (int s = 0; s < 4; s++) { tables[0][s] = D3DTOP_DISABLE; tables[10][s] = 0; }
    }
    for (int i = 0; i < 11; i++) {
        if (g_stage_array_uniform[i] != -1) glUniform1iv(g_stage_array_uniform[i], 4, tables[i]);
    }
    if (g_tc_source_uniform != -1) glUniform1iv(g_tc_source_uniform, 8, tc_source);
    if (g_tc_count_uniform != -1) glUniform1iv(g_tc_count_uniform, 8, tc_count);
    if (g_tc_projected_uniform != -1) glUniform1iv(g_tc_projected_uniform, 8, tc_projected);
    if (g_tc_matrix_uniform != -1) glUniformMatrix4fv(g_tc_matrix_uniform, 8, GL_FALSE, &tc_matrix[0][0]);
    gl_set_matrix_uniform(g_view_model_uniform, &view_model);
    const GLint samplers[8] = { g_texture_uniform, g_texture_lm_uniform, g_texture2_uniform, g_texture3_uniform, g_texture4_uniform, g_texture5_uniform, g_texture6_uniform, g_texture7_uniform };
    for (int s = 0; s < 8; s++) {
        glActiveTexture(GL_TEXTURE0 + s);
        if ((s < 4 && tables[10][s]) || (ps_on && g_texture_stages[s] != 0)) {
            glBindTexture(GL_TEXTURE_2D, g_texture_stages[s]);
            ApplySamplerStates(s);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        if (samplers[s] != -1) glUniform1i(samplers[s], s);
    }
    glActiveTexture(GL_TEXTURE0);
    g_cached_texture = g_texture_stages[0];
    g_last_bound_texture_id = g_texture_stages[0];
    g_last_bound_lightmap_id = g_texture_stages[1];
    g_skip_this_draw = (g_hide_fvf >= 0 && (int) g_current_fvf == g_hide_fvf);
#ifdef SRW_DIAGNOSTICS
    g_check_draw_error = m_current_rt && (g_viewport.Width * 2 < m_current_rt->m_width || g_viewport.Height * 2 < m_current_rt->m_height);
#endif
#ifdef SRW_DIAGNOSTICS
    if (g_debug_mode == 9 || g_hide_state >= 0) LogDrawState();
#endif
}

// d3dDebug 9: every distinct combination of states of a draw is logged once (state number, then how often it was drawn every few thousand draws)
#ifdef SRW_DIAGNOSTICS
void LogDrawState() {
    static std::map<std::string, int> numbers;
    static std::vector<int> counts;
    static std::vector<std::string> texts;
    static unsigned int draws = 0;
    char text[1400];
    int n = snprintf(text, sizeof(text), "fvf %x, blend %u (%u,%u), alpha test %u func %u, cull %u, lighting %u, fog %u, z %u/%u, factor %08x, specular %u, colorvertex %u source %u, material alpha %.2f, alpha ref %u",
                     (unsigned int) g_current_fvf, (unsigned int) m_render_states[D3DRS_ALPHABLENDENABLE], (unsigned int) m_render_states[D3DRS_SRCBLEND],
                     (unsigned int) m_render_states[D3DRS_DESTBLEND], (unsigned int) m_render_states[D3DRS_ALPHATESTENABLE], (unsigned int) m_render_states[25],
                     (unsigned int) m_render_states[D3DRS_CULLMODE], (unsigned int) m_render_states[D3DRS_LIGHTING], (unsigned int) m_render_states[D3DRS_FOGENABLE],
                     (unsigned int) m_render_states[D3DRS_ZENABLE], (unsigned int) m_render_states[D3DRS_ZWRITEENABLE], (unsigned int) m_texture_factor,
                     (unsigned int) m_render_states[D3DRS_SPECULARENABLE], (unsigned int) m_render_states[141], (unsigned int) m_render_states[145],
                     g_material.Diffuse.a, (unsigned int) m_render_states[D3DRS_ALPHAREF]);
    for (int st = 0; st < 4 && n < (int) sizeof(text) - 200; st++) {
        const DWORD* ss = g_texture_stage_states[st];
        if (ss[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
        std::map<GLuint, TextureInfo>::iterator info = g_texture_info.find(g_texture_stages[st]);
        n += snprintf(text + n, sizeof(text) - n, " | stage %d: %s %ux%u format %u levels %u, color op %u (%x %x %x) alpha op %u (%x %x %x) coords %x transform %x result %x", st,
                      g_texture_stages[st] == 0 ? "no texture" : "texture", info != g_texture_info.end() ? info->second.width : 0, info != g_texture_info.end() ? info->second.height : 0,
                      info != g_texture_info.end() ? info->second.format : 0, info != g_texture_info.end() ? info->second.levels : 0, (unsigned int) ss[D3DTSS_COLOROP],
                      (unsigned int) ss[D3DTSS_COLORARG0], (unsigned int) ss[D3DTSS_COLORARG1], (unsigned int) ss[D3DTSS_COLORARG2], (unsigned int) ss[D3DTSS_ALPHAOP],
                      (unsigned int) ss[D3DTSS_ALPHAARG0], (unsigned int) ss[D3DTSS_ALPHAARG1], (unsigned int) ss[D3DTSS_ALPHAARG2], (unsigned int) ss[D3DTSS_TEXCOORDINDEX],
                      (unsigned int) ss[D3DTSS_TEXTURETRANSFORMFLAGS], (unsigned int) ss[D3DTSS_RESULTARG]);
        n += snprintf(text + n, sizeof(text) - n, " address %u/%u filter %u/%u", (unsigned) g_sampler_states[st][D3DSAMP_ADDRESSU], (unsigned) g_sampler_states[st][D3DSAMP_ADDRESSV],
                      (unsigned) g_sampler_states[st][D3DSAMP_MINFILTER], (unsigned) g_sampler_states[st][D3DSAMP_MAGFILTER]);
    }
    std::string key(text);
    {
        // the alpha reference and the exact material alpha are left out of the key (they take many values); a translucent material is kept
        size_t r = key.find(", material alpha ");
        if (r != std::string::npos) {
            size_t e = key.find(" |", r);
            key.replace(r, e == std::string::npos ? std::string::npos : e - r, g_material.Diffuse.a < 0.99f ? ", translucent material" : "");
        }
    }
    {
        // what WebGL really has set now (the states above are what the game asked for): a mismatch is the bug
        GLint src_rgb = 0, dst_rgb = 0, src_a = 0, dst_a = 0, eq_rgb = 0, eq_a = 0, depth_mask = 0;
        GLboolean mask[4] = { 0, 0, 0, 0 };
        char gl[200];

        glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb); glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a); glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
        glGetIntegerv(GL_DEPTH_WRITEMASK, &depth_mask); glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        snprintf(gl, sizeof(gl), " || GL: blend %d func %x,%x alpha func %x,%x eq %x/%x color mask %d%d%d%d depth mask %d depth test %d",
                 (int) glIsEnabled(GL_BLEND), src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a, mask[0], mask[1], mask[2], mask[3], depth_mask, (int) glIsEnabled(GL_DEPTH_TEST));
        key += gl;
        n += snprintf(text + n, sizeof(text) - n, "%s", gl);
        char rt[80];
        snprintf(rt, sizeof(rt), " || rt %s %ux%u", (m_current_rt && !m_current_rt->m_is_backbuffer) ? "TEXTURE" : "screen",
                 m_current_rt ? (unsigned) m_current_rt->m_width : 0u, m_current_rt ? (unsigned) m_current_rt->m_height : 0u);
        key += rt;
        n += snprintf(text + n, sizeof(text) - n, "%s", rt);
        {
            // the viewport, in coarse classes so that the states stay few: the whole target, or the rectangle if it is smaller than half of it
            char vp[100];
            bool small = m_current_rt && (g_viewport.Width * 2 < m_current_rt->m_width || g_viewport.Height * 2 < m_current_rt->m_height);
            if (small) snprintf(vp, sizeof(vp), " || viewport %u,%u %ux%u", (unsigned) g_viewport.X, (unsigned) g_viewport.Y, (unsigned) g_viewport.Width, (unsigned) g_viewport.Height);
            else snprintf(vp, sizeof(vp), " || viewport full");
            key += vp;
            n += snprintf(text + n, sizeof(text) - n, "%s", vp);
        }
    }
    std::map<std::string, int>::iterator found = numbers.find(key);
    int number;
    if (found == numbers.end()) {
        number = (int) counts.size();
        numbers[key] = number;
        counts.push_back(0);
        texts.push_back(key);
        if (number < 600) printf("[D3D9] draw state %d: %s\n", number, text);
    } else {
        number = found->second;
    }
    counts[number]++;
    g_skip_this_draw = (number == g_hide_state);
    if (++draws % 4000 == 0) {
        // the states drawn most since the last report, with their text (the water is one of the busy ones when it is in view)
        static std::vector<int> previous;
        std::vector<std::pair<int, int> > delta;

        previous.resize(counts.size(), 0);
        for (size_t i = 0; i < counts.size(); i++) delta.push_back(std::make_pair(counts[i] - previous[i], (int) i));
        previous = counts;
        std::sort(delta.begin(), delta.end(), std::greater<std::pair<int, int> >());
        printf("[D3D9] busiest draw states of the last 4000 draws:\n");
        for (size_t i = 0; i < delta.size() && i < 8 && delta[i].first > 0; i++) printf("[D3D9]   state %d, %d draws: %s\n", delta[i].second, delta[i].first, texts[delta[i].second].c_str());
    }
}
#endif

    virtual HRESULT SetFVF(DWORD FVF) override {
        g_current_fvf = FVF;
        return D3D_OK;
    }
    
    virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                   UINT PrimitiveCount,
                                   const void* pVertexStreamZeroData,
                                   UINT VertexStreamZeroStride) override {
        PERF_DRAW_TIMER
        SRW_DRAW_STAT(PrimitiveCount);
        trace_draw("DrawPrimitiveUP", PrimitiveCount);
        if (!pVertexStreamZeroData) return -1;
#ifdef SRW_DIAGNOSTICS
        {
            static int logged = 0;
            if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 120 && (g_current_fvf & D3DFVF_XYZRHW) && VertexStreamZeroStride >= 16) {
                const float *v = (const float *) pVertexStreamZeroData;
                unsigned stride_floats = VertexStreamZeroStride / 4;
                printf("[D3D9] DrawPrimitiveUP %u: type %u, %u primitives, fvf %x, stride %u, rt %s %ux%u, texture0 %u, blend %u (%u,%u) z %u, scissor %u, viewport %u,%u %ux%u; vertices:", (unsigned) logged, (unsigned) PrimitiveType,
                       (unsigned) PrimitiveCount, (unsigned) g_current_fvf, (unsigned) VertexStreamZeroStride, m_current_rt && m_current_rt->m_is_backbuffer ? "back buffer" : "texture",
                       m_current_rt ? (unsigned) m_current_rt->m_width : 0, m_current_rt ? (unsigned) m_current_rt->m_height : 0, (unsigned) g_texture_stages[0],
                       (unsigned) m_render_states[D3DRS_ALPHABLENDENABLE], (unsigned) m_render_states[D3DRS_SRCBLEND], (unsigned) m_render_states[D3DRS_DESTBLEND],
                       (unsigned) m_render_states[D3DRS_ZENABLE], (unsigned) m_render_states[D3DRS_SCISSORTESTENABLE], (unsigned) g_viewport.X, (unsigned) g_viewport.Y, (unsigned) g_viewport.Width, (unsigned) g_viewport.Height);
                for (unsigned i = 0; i < 4 && i < PrimitiveCount + 2; i++) {
                    const float *q = v + i * stride_floats;
                    printf(" (%.1f %.1f %.2f %.2f", (double) q[0], (double) q[1], (double) q[2], (double) q[3]);
                    if (stride_floats >= 7) printf(" uv %.3f %.3f", (double) q[stride_floats - 2], (double) q[stride_floats - 1]);
                    printf(")");
                }
                printf("\n");
            }
        }
#endif

        if (g_shader_program == 0) init_shaders();

        // State Caching: Only call glUseProgram if program changed
        if (g_cached_program != g_shader_program) {
            glUseProgram(g_shader_program);
            g_cached_program = g_shader_program;
        }

        // FBO Y-flip and clip plane uniforms
        glUniform1i(g_fbo_flip_y_uniform, m_fbo_rendering ? 1 : 0);
        glUniform1i(g_clip_plane_enabled_uniform, m_clip_plane_enabled ? 1 : 0);
        if (m_clip_plane_enabled) UploadClipPlanes();

        EnsureUPBuffers();
        CHECK_GL_ERROR(glBindBuffer(GL_ARRAY_BUFFER, g_up_vbo));
        CHECK_GL_ERROR(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)); // No indices for UP

        // Dynamic FVF Parsing
        bool has_xyz = (g_current_fvf & D3DFVF_XYZ) != 0;
        bool has_xyzrhw = (g_current_fvf & D3DFVF_XYZRHW) != 0;
        bool has_normal = (g_current_fvf & D3DFVF_NORMAL) != 0;
        bool has_diffuse = (g_current_fvf & D3DFVF_DIFFUSE) != 0;
        int tex_count = (g_current_fvf >> 8) & 0xF;

        GLsizei stride = 0;
        size_t position_offset = 0;
        size_t normal_offset = 0;
        size_t color_offset = 0;
        size_t tex_offset = 0;

        // XYZ and XYZRHW are mutually exclusive in D3D9 FVF
        if (has_xyzrhw) { position_offset = stride; stride += 16; }
        else if (has_xyz) { position_offset = stride; stride += 12; }
        if (has_normal) { normal_offset = stride; stride += 12; }
        if (has_diffuse) { color_offset = stride; stride += 4; }
        tex_offset = stride;
        stride += tex_count * 8;
        
        if (VertexStreamZeroStride > 0) stride = VertexStreamZeroStride;

        // Override Viewport for UI rendering to ensure it covers the full canvas
        GLint saved_viewport[4];
        bool override_viewport = false;
        
        // Restore state variables
        GLboolean saved_scissor_test = GL_FALSE;
        GLint saved_scissor_box[4];

        if ((g_current_fvf & D3DFVF_XYZRHW) && m_current_rt && m_current_rt->m_is_backbuffer) {
            // Use cached viewport instead of glGetIntegerv (performance optimization)
            saved_viewport[0] = g_cached_viewport[0];
            saved_viewport[1] = g_cached_viewport[1];
            saved_viewport[2] = g_cached_viewport[2];
            saved_viewport[3] = g_cached_viewport[3];

#ifdef SRW_DIAGNOSTICS
            if (!g_probe_ui_done) {
                static int probes = 0;
                g_probe_ui_done = true;
                probe_backbuffer("before the first UI draw of the frame", &probes, 4, 120);
            }
#endif
            glViewport(0, 0, g_logical_width, g_logical_height);

            // Disable Culling and Depth Test for 2D UI elements to ensure visibility
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);

            // Use cached scissor state instead of glGet* (performance optimization)
            saved_scissor_test = g_cached_scissor_test;
            saved_scissor_box[0] = g_cached_scissor_box[0];
            saved_scissor_box[1] = g_cached_scissor_box[1];
            saved_scissor_box[2] = g_cached_scissor_box[2];
            saved_scissor_box[3] = g_cached_scissor_box[3];

            glEnable(GL_SCISSOR_TEST);

            // Scissor to current viewport area (WebGL Y is bottom-to-top)
            int sy = g_logical_height - (g_viewport.Y + g_viewport.Height);
            glScissor(g_viewport.X, sy, g_viewport.Width, g_viewport.Height);
            
            override_viewport = true;
        }

        // Calculate MVP
        D3DMATRIX view_model, mvp;
        if (g_current_fvf & D3DFVF_XYZRHW) {
            // For XYZRHW, use the app's logical resolution
            float sw = (float)g_logical_width;
            float sh = (float)g_logical_height;
            // If rendering to texture, use texture dimensions
            if (m_current_rt && !m_current_rt->m_is_backbuffer) {
                 sw = (float)m_current_rt->m_width;
                 sh = (float)m_current_rt->m_height;
            }

            memset(&mvp, 0, sizeof(D3DMATRIX));
            mvp.m[0][0] = 2.0f / sw;
            mvp.m[1][1] = -2.0f / sh; 
            mvp.m[3][0] = -1.0f;
            mvp.m[3][1] = 1.0f;
            mvp.m[2][2] = 1.0f;
            mvp.m[3][3] = 1.0f;
            mvp.m[3][2] = 0.0f;
        } else {
            matrix_multiply(&view_model, &m_world, &m_view);
            matrix_multiply(&mvp, &view_model, &m_projection);
        }

        // winding: see choose_front_face
        glFrontFace(choose_front_face(view_model, m_fbo_rendering, (g_current_fvf & D3DFVF_XYZRHW) != 0));

        gl_set_matrix_uniform(g_mvp_uniform, &mvp);

        if (g_texture_factor_uniform != -1) {
            float r = ((m_texture_factor >> 16) & 0xFF) / 255.0f;
            float g = ((m_texture_factor >> 8) & 0xFF) / 255.0f;
            float b = (m_texture_factor & 0xFF) / 255.0f;
            float a = ((m_texture_factor >> 24) & 0xFF) / 255.0f;
            glUniform4f(g_texture_factor_uniform, r, g, b, a);
        }

        // mipmap LOD bias (D3D9 stores the float as a DWORD), with an offset of -0.75: WebGL selects lower resolution levels
        if (g_lod_bias_uniform != -1) {
            DWORD bias_bits = g_sampler_states[0][D3DSAMP_MIPMAPLODBIAS];
            float lod_bias;
            memcpy(&lod_bias, &bias_bits, sizeof(float));
            lod_bias -= 0.75f;
            glUniform1f(g_lod_bias_uniform, lod_bias);
        }

        // Set Stage 0 texture stage states
        if (g_stage0_colorop_uniform != -1) {
            glUniform1i(g_stage0_colorop_uniform, g_texture_stage_states[0][D3DTSS_COLOROP]);
        }
        if (g_stage0_colorarg1_uniform != -1) {
            glUniform1i(g_stage0_colorarg1_uniform, g_texture_stage_states[0][D3DTSS_COLORARG1]);
        }
        if (g_stage0_colorarg2_uniform != -1) {
            glUniform1i(g_stage0_colorarg2_uniform, g_texture_stage_states[0][D3DTSS_COLORARG2]);
        }
        if (g_stage0_alphaop_uniform != -1) {
            glUniform1i(g_stage0_alphaop_uniform, g_texture_stage_states[0][D3DTSS_ALPHAOP]);
        }
        if (g_stage0_alphaarg1_uniform != -1) {
            glUniform1i(g_stage0_alphaarg1_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG1]);
        }
        if (g_stage0_alphaarg2_uniform != -1) {
            glUniform1i(g_stage0_alphaarg2_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG2]);
        }

        // Fog uniforms
        {
            bool apply_fog = m_fog_enabled && !(g_current_fvf & D3DFVF_XYZRHW);
            glUniform1i(g_fog_enabled_uniform, apply_fog ? 1 : 0);
            if (apply_fog) {
                float r = ((m_fog_color >> 16) & 0xFF) / 255.0f;
                float g_c = ((m_fog_color >> 8) & 0xFF) / 255.0f;
                float b = (m_fog_color & 0xFF) / 255.0f;
                glUniform3f(g_fog_color_uniform, r, g_c, b);
                glUniform1f(g_fog_start_uniform, m_fog_start);
                glUniform1f(g_fog_end_uniform, m_fog_end);
                gl_set_matrix_uniform(g_view_model_uniform, &view_model);
            }
        }

        const uint8_t* vertex_data = static_cast<const uint8_t*>(pVertexStreamZeroData);

        // Calculate vertex count and total size
        UINT vertex_count = 0;
         switch (PrimitiveType) {
            case D3DPT_TRIANGLELIST: vertex_count = PrimitiveCount * 3; break;
            case D3DPT_TRIANGLESTRIP: vertex_count = PrimitiveCount + 2; break;
            case D3DPT_TRIANGLEFAN: vertex_count = PrimitiveCount + 2; break;
            case D3DPT_LINELIST: vertex_count = PrimitiveCount * 2; break;
            case D3DPT_LINESTRIP: vertex_count = PrimitiveCount + 1; break;
            case D3DPT_POINTLIST: vertex_count = PrimitiveCount; break;
        }
        
        size_t total_size = stride * vertex_count;

        // Convert BGRA vertex colors to RGBA if needed
        bool converted = false;
        static std::vector<uint8_t> converted_data;
        if (has_diffuse) {
            converted_data.resize(total_size);
            memcpy(converted_data.data(), pVertexStreamZeroData, total_size);
            
            // Swap R and B in each vertex's color (BGRA -> RGBA)
            for (UINT i = 0; i < vertex_count; ++i) {
                uint8_t* color_ptr = converted_data.data() + (i * stride) + color_offset;
                std::swap(color_ptr[0], color_ptr[2]); 
                
                // Removed: Force Alpha to 255 if it's 0
                // This was breaking opacity functionality for UI elements (glow effects, etc.)
                // D3D9 FFP respects alpha=0 for transparency when D3DFVF_DIFFUSE is specified
            }
            vertex_data = converted_data.data();
            converted = true;
        }

        // Upload to VBO - Use persistent buffer for better performance
        if (total_size > g_up_vbo_size) {
            // Buffer too small, reallocate with 2x growth strategy
            size_t new_size = total_size * 2;
            CHECK_GL_ERROR(glBufferData(GL_ARRAY_BUFFER, new_size, nullptr, GL_DYNAMIC_DRAW));
            g_up_vbo_size = new_size;
        }
        // Just update the data (fast path - avoids buffer reallocation)
        CHECK_GL_ERROR(glBufferSubData(GL_ARRAY_BUFFER, 0, total_size, vertex_data));
        
        // Enable/Set Pointers (Use offsets!)
        if (has_xyzrhw) {
            CHECK_GL_ERROR(glEnableVertexAttribArray(g_position_location));
            CHECK_GL_ERROR(glVertexAttribPointer(g_position_location, 4, GL_FLOAT, GL_FALSE, stride, (void*)position_offset));
        } else if (has_xyz) {
            CHECK_GL_ERROR(glEnableVertexAttribArray(g_position_location));
            CHECK_GL_ERROR(glVertexAttribPointer(g_position_location, 3, GL_FLOAT, GL_FALSE, stride, (void*)position_offset));
        }

        // Normal attribute for FFP lighting
        if (has_normal && g_normal_location != -1) {
            CHECK_GL_ERROR(glEnableVertexAttribArray(g_normal_location));
            CHECK_GL_ERROR(glVertexAttribPointer(g_normal_location, 3, GL_FLOAT, GL_FALSE, stride, (void*)normal_offset));
        } else if (g_normal_location != -1) {
            CHECK_GL_ERROR(glDisableVertexAttribArray(g_normal_location));
            CHECK_GL_ERROR(glVertexAttrib3f(g_normal_location, 0.0f, 0.0f, 1.0f));
        }

        // D3D9 FFP Lighting: when FVF has NORMAL and D3DRS_LIGHTING is TRUE
        bool lighting_enabled = has_normal && !has_xyzrhw && m_render_states[D3DRS_LIGHTING];
        if (lighting_enabled) {
            CHECK_GL_ERROR(glUniform1i(g_lighting_enabled_uniform, 1));
            SetFFPLightingUniforms(&m_world, m_render_states, has_diffuse);

            CHECK_GL_ERROR(glDisableVertexAttribArray(g_color_location));
            CHECK_GL_ERROR(glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f));
            if (has_diffuse && g_color_location != -1) {
                glEnableVertexAttribArray(g_color_location);
                glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)color_offset);
            }
        } else {
            CHECK_GL_ERROR(glUniform1i(g_lighting_enabled_uniform, 0));
            // Always set world matrix for clip plane (v_position_world) even without lighting
            gl_set_matrix_uniform(g_world_uniform, &m_world);

            if (has_diffuse) {
                CHECK_GL_ERROR(glEnableVertexAttribArray(g_color_location));
                CHECK_GL_ERROR(glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)color_offset));
            } else {
                CHECK_GL_ERROR(glDisableVertexAttribArray(g_color_location));
                CHECK_GL_ERROR(glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f));
            }
        }

        bind_texcoords(g_current_fvf, stride, tex_offset);
        
        // Texture setup
        if (g_texture_stages[0] != 0 && tex_count > 0) {
            CHECK_GL_ERROR(glActiveTexture(GL_TEXTURE0));

            // State Caching: Only bind texture if it changed
            if (g_cached_texture != g_texture_stages[0]) {
                CHECK_GL_ERROR(glBindTexture(GL_TEXTURE_2D, g_texture_stages[0]));
                g_cached_texture = g_texture_stages[0];
            }

            CHECK_GL_ERROR(glUniform1i(g_texture_uniform, 0));
            CHECK_GL_ERROR(glUniform1i(g_use_texture_uniform, 1));

            // Apply D3D sampler states (Filter, Wrap)
            ApplySamplerStates(0);
        } else {
            // No texture case - bind texture 0
            // texture unit 0 must be active, or the unbinding takes the lightmap off unit 1
            CHECK_GL_ERROR(glActiveTexture(GL_TEXTURE0));
            if (g_cached_texture != 0) {
                CHECK_GL_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
                g_cached_texture = 0;
            }
            CHECK_GL_ERROR(glUniform1i(g_use_texture_uniform, 0));
        }

        CHECK_GL_ERROR(glUniform1i(g_alpha_test_enabled_uniform, m_alpha_test_enabled ? 1 : 0));
        CHECK_GL_ERROR(glUniform1f(g_alpha_ref_uniform, (float)m_alpha_ref));
        UploadTextureStages(view_model);

        // Disable lightmap to avoid stale state from DrawIndexedPrimitive
        CHECK_GL_ERROR(glUniform1i(g_use_lightmap_uniform, 0));

        // Draw
        GLenum gl_primitive_type = GL_TRIANGLES;
        switch (PrimitiveType) {
            case D3DPT_TRIANGLELIST: gl_primitive_type = GL_TRIANGLES; break;
            case D3DPT_TRIANGLESTRIP: gl_primitive_type = GL_TRIANGLE_STRIP; break;
            case D3DPT_TRIANGLEFAN: gl_primitive_type = GL_TRIANGLE_FAN; break;
            case D3DPT_LINELIST: gl_primitive_type = GL_LINES; break;
            case D3DPT_LINESTRIP: gl_primitive_type = GL_LINE_STRIP; break;
            case D3DPT_POINTLIST: gl_primitive_type = GL_POINTS; break;
        }

        CHECK_GL_ERROR(glDrawArrays(gl_primitive_type, 0, vertex_count));

        // Cleanup pointers (unbind RB to be safe?)
        // (no unbinding, see above)

        // Restore Viewport/Scissor/DepthTest/CullFace if overridden
        if (override_viewport) {
            glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

            if (saved_scissor_test) {
                glScissor(saved_scissor_box[0], saved_scissor_box[1], saved_scissor_box[2], saved_scissor_box[3]);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }

            // Restore depth test, depth mask, and cull face from tracked render states
            if (m_render_states[D3DRS_ZENABLE]) glEnable(GL_DEPTH_TEST);
            if (m_render_states[D3DRS_ZWRITEENABLE]) glDepthMask(GL_TRUE);
            if (m_render_states[D3DRS_CULLMODE] != D3DCULL_NONE) glEnable(GL_CULL_FACE);
        }

        return D3D_OK;
    }
    virtual HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                           UINT NumVertices, UINT PrimitiveCount,
                                           const void* pIndexData, D3DFORMAT IndexDataFormat,
                                           const void* pVertexStreamZeroData,
                                           UINT VertexStreamZeroStride) override {
        PERF_DRAW_TIMER
        SRW_DRAW_STAT(PrimitiveCount);
        trace_draw("DrawIndexedPrimitiveUP", PrimitiveCount);
        if (!pVertexStreamZeroData || !pIndexData) return -1;

        if (g_shader_program == 0) init_shaders();
        glUseProgram(g_shader_program);

        // FBO Y-flip and clip plane uniforms
        glUniform1i(g_fbo_flip_y_uniform, m_fbo_rendering ? 1 : 0);
        glUniform1i(g_clip_plane_enabled_uniform, m_clip_plane_enabled ? 1 : 0);
        if (m_clip_plane_enabled) UploadClipPlanes();

        EnsureUPBuffers();
        CHECK_GL_ERROR(glBindBuffer(GL_ARRAY_BUFFER, g_up_vbo));
        CHECK_GL_ERROR(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_up_ibo));

        // Respect game's blend state (set via SetRenderState before this draw call)

        // Calculate MVP
        D3DMATRIX view_model, mvp;
        if (g_current_fvf & D3DFVF_XYZRHW) {
            float vw = (g_viewport.Width > 0) ? (float)g_viewport.Width : (float)g_logical_width;
            float vh = (g_viewport.Height > 0) ? (float)g_viewport.Height : (float)g_logical_height;

            memset(&mvp, 0, sizeof(D3DMATRIX));
            mvp.m[0][0] = 2.0f / vw;
            mvp.m[3][0] = -1.0f;
            mvp.m[1][1] = -2.0f / vh;
            mvp.m[3][1] = 1.0f;
            mvp.m[2][2] = 1.0f;
            mvp.m[3][3] = 1.0f;
        } else {
            matrix_multiply(&view_model, &m_world, &m_view);
            matrix_multiply(&mvp, &view_model, &m_projection);
        }

        // winding: see choose_front_face
        glFrontFace(choose_front_face(view_model, m_fbo_rendering, (g_current_fvf & D3DFVF_XYZRHW) != 0));

        gl_set_matrix_uniform(g_mvp_uniform, &mvp);

        // Set texture factor
        if (g_texture_factor_uniform != -1) {
            float r = ((m_texture_factor >> 16) & 0xFF) / 255.0f;
            float g = ((m_texture_factor >> 8) & 0xFF) / 255.0f;
            float b = (m_texture_factor & 0xFF) / 255.0f;
            float a = ((m_texture_factor >> 24) & 0xFF) / 255.0f;
            glUniform4f(g_texture_factor_uniform, r, g, b, a);
        }

        // mipmap LOD bias (D3D9 stores the float as a DWORD), with an offset of -0.75: WebGL selects lower resolution levels
        if (g_lod_bias_uniform != -1) {
            DWORD bias_bits = g_sampler_states[0][D3DSAMP_MIPMAPLODBIAS];
            float lod_bias;
            memcpy(&lod_bias, &bias_bits, sizeof(float));
            lod_bias -= 0.75f;
            glUniform1f(g_lod_bias_uniform, lod_bias);
        }

        // Set Stage 0 texture stage states
        if (g_stage0_colorop_uniform != -1) {
            glUniform1i(g_stage0_colorop_uniform, g_texture_stage_states[0][D3DTSS_COLOROP]);
        }

        if (g_stage0_colorarg1_uniform != -1) {
            glUniform1i(g_stage0_colorarg1_uniform, g_texture_stage_states[0][D3DTSS_COLORARG1]);
        }
        if (g_stage0_colorarg2_uniform != -1) {
            glUniform1i(g_stage0_colorarg2_uniform, g_texture_stage_states[0][D3DTSS_COLORARG2]);
        }
        if (g_stage0_alphaop_uniform != -1) {
            glUniform1i(g_stage0_alphaop_uniform, g_texture_stage_states[0][D3DTSS_ALPHAOP]);
        }
        if (g_stage0_alphaarg1_uniform != -1) {
            glUniform1i(g_stage0_alphaarg1_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG1]);
        }
        if (g_stage0_alphaarg2_uniform != -1) {
            glUniform1i(g_stage0_alphaarg2_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG2]);
        }

        // Fog uniforms
        {
            bool apply_fog = m_fog_enabled && !(g_current_fvf & D3DFVF_XYZRHW);
            glUniform1i(g_fog_enabled_uniform, apply_fog ? 1 : 0);
            if (apply_fog) {
                float r = ((m_fog_color >> 16) & 0xFF) / 255.0f;
                float g_c = ((m_fog_color >> 8) & 0xFF) / 255.0f;
                float b = (m_fog_color & 0xFF) / 255.0f;
                glUniform3f(g_fog_color_uniform, r, g_c, b);
                glUniform1f(g_fog_start_uniform, m_fog_start);
                glUniform1f(g_fog_end_uniform, m_fog_end);
                gl_set_matrix_uniform(g_view_model_uniform, &view_model);
            }
        }

        // Dynamic FVF Parsing
        bool has_xyz = (g_current_fvf & D3DFVF_XYZ) != 0;
        bool has_xyzrhw = (g_current_fvf & D3DFVF_XYZRHW) != 0;
        bool has_normal = (g_current_fvf & D3DFVF_NORMAL) != 0;
        bool has_diffuse = (g_current_fvf & D3DFVF_DIFFUSE) != 0;
        int tex_count = (g_current_fvf >> 8) & 0xF;

        GLsizei stride = 0;
        size_t position_offset = 0;
        size_t normal_offset = 0;
        size_t color_offset = 0;
        size_t tex_offset = 0;

        // XYZ and XYZRHW are mutually exclusive in D3D9 FVF
        if (has_xyzrhw) { position_offset = stride; stride += 16; }
        else if (has_xyz) { position_offset = stride; stride += 12; }
        if (has_normal) { normal_offset = stride; stride += 12; }
        if (has_diffuse) { color_offset = stride; stride += 4; }
        tex_offset = stride;
        stride += tex_count * 8;

        if (VertexStreamZeroStride > 0) stride = VertexStreamZeroStride;

        const uint8_t* vertex_data = static_cast<const uint8_t*>(pVertexStreamZeroData);

        // Upload Vertices
        // only a pointer is given: NumVertices vertices are taken as the size
        
        size_t total_v_size = stride * NumVertices;
        
        // BGRA -> RGBA conversion
        static std::vector<uint8_t> converted_v_data;
        if (has_diffuse) {
            converted_v_data.resize(total_v_size);
            memcpy(converted_v_data.data(), pVertexStreamZeroData, total_v_size);
             for (UINT i = 0; i < NumVertices; ++i) {
                uint8_t* color_ptr = converted_v_data.data() + (i * stride) + color_offset;
                std::swap(color_ptr[0], color_ptr[2]);
                // alpha 0 stays transparent when D3DFVF_DIFFUSE is given
            }
            vertex_data = converted_v_data.data();
        }

        // Upload Vertices - Use persistent buffer for better performance
        if (total_v_size > g_up_vbo_size) {
            // Buffer too small, reallocate with 2x growth strategy
            size_t new_size = total_v_size * 2;
            CHECK_GL_ERROR(glBufferData(GL_ARRAY_BUFFER, new_size, nullptr, GL_DYNAMIC_DRAW));
            g_up_vbo_size = new_size;
        }
        // Just update the data (fast path - avoids buffer reallocation)
        CHECK_GL_ERROR(glBufferSubData(GL_ARRAY_BUFFER, 0, total_v_size, vertex_data));

        // Upload Indices
        // PrimitiveCount triangles/lines etc.
        size_t index_size = (IndexDataFormat == D3DFMT_INDEX16) ? 2 : 4;
        size_t index_count = 0;
        switch (PrimitiveType) {
            case D3DPT_TRIANGLELIST: index_count = PrimitiveCount * 3; break;
            case D3DPT_TRIANGLESTRIP: index_count = PrimitiveCount + 2; break; // Approximated
            case D3DPT_TRIANGLEFAN: index_count = PrimitiveCount + 2; break; // Approximated
            // Lines etc
            default: index_count = PrimitiveCount * 3; // Safe bet for alloc
        }
        
        // the primitives define how many indices are read
        size_t total_i_size = index_count * index_size;

        // Upload Indices - Use persistent buffer for better performance
        if (total_i_size > g_up_ibo_size) {
            // Buffer too small, reallocate with 2x growth strategy
            size_t new_size = total_i_size * 2;
            CHECK_GL_ERROR(glBufferData(GL_ELEMENT_ARRAY_BUFFER, new_size, nullptr, GL_DYNAMIC_DRAW));
            g_up_ibo_size = new_size;
        }
        // Just update the data (fast path - avoids buffer reallocation)
        CHECK_GL_ERROR(glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, total_i_size, pIndexData));

         // Enable/Set Pointers (Use offsets!)
        if (has_xyzrhw) {
            // all four components for XYZRHW: w goes to gl_Position.w
            CHECK_GL_ERROR(glEnableVertexAttribArray(g_position_location));
            CHECK_GL_ERROR(glVertexAttribPointer(g_position_location, 4, GL_FLOAT, GL_FALSE, stride, (void*)position_offset));
        } else if (has_xyz) {
            CHECK_GL_ERROR(glEnableVertexAttribArray(g_position_location));
            CHECK_GL_ERROR(glVertexAttribPointer(g_position_location, 3, GL_FLOAT, GL_FALSE, stride, (void*)position_offset));
        }

        // Normal attribute for FFP lighting
        if (has_normal && g_normal_location != -1) {
            CHECK_GL_ERROR(glEnableVertexAttribArray(g_normal_location));
            CHECK_GL_ERROR(glVertexAttribPointer(g_normal_location, 3, GL_FLOAT, GL_FALSE, stride, (void*)normal_offset));
        } else if (g_normal_location != -1) {
            CHECK_GL_ERROR(glDisableVertexAttribArray(g_normal_location));
            CHECK_GL_ERROR(glVertexAttrib3f(g_normal_location, 0.0f, 0.0f, 1.0f));
        }

        // D3D9 FFP Lighting: when FVF has NORMAL and D3DRS_LIGHTING is TRUE
        bool lighting_enabled = has_normal && !has_xyzrhw && m_render_states[D3DRS_LIGHTING];
        if (lighting_enabled) {
            CHECK_GL_ERROR(glUniform1i(g_lighting_enabled_uniform, 1));
            SetFFPLightingUniforms(&m_world, m_render_states, has_diffuse);

            CHECK_GL_ERROR(glDisableVertexAttribArray(g_color_location));
            CHECK_GL_ERROR(glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f));
            if (has_diffuse && g_color_location != -1) {
                glEnableVertexAttribArray(g_color_location);
                glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)color_offset);
            }
        } else {
            CHECK_GL_ERROR(glUniform1i(g_lighting_enabled_uniform, 0));
            // Always set world matrix for clip plane (v_position_world) even without lighting
            gl_set_matrix_uniform(g_world_uniform, &m_world);

            if (has_diffuse) {
                CHECK_GL_ERROR(glEnableVertexAttribArray(g_color_location));
                CHECK_GL_ERROR(glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)color_offset));
            } else {
                CHECK_GL_ERROR(glDisableVertexAttribArray(g_color_location));
                CHECK_GL_ERROR(glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f));
            }
        }

        bind_texcoords(g_current_fvf, stride, tex_offset);

        // Texture setup
        if (g_texture_stages[0] != 0 && tex_count > 0) {
            CHECK_GL_ERROR(glActiveTexture(GL_TEXTURE0));

            // State Caching: Only bind texture if it changed
            if (g_cached_texture != g_texture_stages[0]) {
                CHECK_GL_ERROR(glBindTexture(GL_TEXTURE_2D, g_texture_stages[0]));
                g_cached_texture = g_texture_stages[0];
            }

            CHECK_GL_ERROR(glUniform1i(g_texture_uniform, 0));
            CHECK_GL_ERROR(glUniform1i(g_use_texture_uniform, 1));

            // Apply D3D sampler states (Filter, Wrap)
            ApplySamplerStates(0);
        } else {
            // No texture case - bind texture 0
            // texture unit 0 must be active, or the unbinding takes the lightmap off unit 1
            CHECK_GL_ERROR(glActiveTexture(GL_TEXTURE0));
            if (g_cached_texture != 0) {
                CHECK_GL_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
                g_cached_texture = 0;
            }
            CHECK_GL_ERROR(glUniform1i(g_use_texture_uniform, 0));
        }

        CHECK_GL_ERROR(glUniform1i(g_alpha_test_enabled_uniform, m_alpha_test_enabled ? 1 : 0));
        CHECK_GL_ERROR(glUniform1f(g_alpha_ref_uniform, (float)m_alpha_ref));
        UploadTextureStages(view_model);

        // Stage 1 / Lightmap setup - must explicitly set to avoid stale state from DrawIndexedPrimitive
        if (g_texture_stages[1] != 0 && tex_count > 1) {
            CHECK_GL_ERROR(glActiveTexture(GL_TEXTURE1));
            CHECK_GL_ERROR(glBindTexture(GL_TEXTURE_2D, g_texture_stages[1]));
            g_last_bound_lightmap_id = g_texture_stages[1];
            CHECK_GL_ERROR(glUniform1i(g_texture_lm_uniform, 1));
            CHECK_GL_ERROR(glUniform1i(g_use_lightmap_uniform, 1));
            DWORD op = g_texture_stage_states[1][D3DTSS_COLOROP];
            CHECK_GL_ERROR(glUniform1i(g_stage1_op_uniform, op));
            CHECK_GL_ERROR(glActiveTexture(GL_TEXTURE0));
        } else {
            CHECK_GL_ERROR(glUniform1i(g_use_lightmap_uniform, 0));
        }

        GLenum gl_primitive_type = GL_TRIANGLES;
        switch (PrimitiveType) {
            case D3DPT_TRIANGLELIST: gl_primitive_type = GL_TRIANGLES; break;
            // Others ignored for simplicity as they are rare in indexed
            default: gl_primitive_type = GL_TRIANGLES;
        }

        GLenum gl_index_type = (IndexDataFormat == D3DFMT_INDEX16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;

        CHECK_GL_ERROR(glDrawElements(gl_primitive_type, index_count, gl_index_type, 0));

        // (no unbinding, see above)
        // (no unbinding, see above)

        // Blend state is managed by game's SetRenderState calls

        return D3D_OK;
    }
    virtual HRESULT CreateTexture(UINT Width, UINT Height, UINT Levels,
                                  DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                  IDirect3DTexture9** ppTexture, void* pSharedHandle) override {
        if (!ppTexture) return D3DERR_INVALIDCALL;

        D3D9Texture* tex = new D3D9Texture(Width, Height, Levels, Format);
        g_texture_info[tex->m_texture] = { Width, Height, Levels, (unsigned int) Format };
        tex->m_usage = Usage;
        tex->m_pool = Pool;
#ifdef SRW_DIAGNOSTICS
        {
            static int logged = 0;

            static int special = 0;

            if (SRW_LOG_ON(SRW_LOG_D3D) && Usage != 0 && special++ < 80) {
                printf("[D3D9] texture with usage %x: %ux%u, %u levels, format %u, pool %d\n", (unsigned) Usage, (unsigned) Width, (unsigned) Height, (unsigned) Levels,
                       (unsigned) Format, (int) Pool);
            }
            if (SRW_LOG_ON(SRW_LOG_D3D) && logged < 30) {
                logged++;
                printf("[D3D9] texture %d: %ux%u, %u levels, format %u, usage %x, pool %d\n", logged, (unsigned) Width, (unsigned) Height, (unsigned) Levels,
                       (unsigned) Format, (unsigned) Usage, (int) Pool);
            }
        }
#endif

        // If this is a render target, allocate texture storage immediately
        // so it can be attached to an FBO without causing "zero size attachment" errors
        if (Usage & D3DUSAGE_RENDERTARGET) {
            glBindTexture(GL_TEXTURE_2D, tex->m_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            // Invalidate texture binding cache
            g_last_bound_texture_id = 0xFFFFFFFF;
            g_last_bound_lightmap_id = 0xFFFFFFFF;
            g_cached_texture = 0xFFFFFFFF;
        }

        *ppTexture = tex;
        return D3D_OK;
    }
    
    virtual HRESULT SetTexture(DWORD Stage, IDirect3DTexture9* pTexture) override {
        if (Stage >= 8) return D3D_OK;
        
        GLuint texture_id = 0;
        if (pTexture) {
            D3D9Texture* tex = static_cast<D3D9Texture*>(pTexture);
            texture_id = tex->GetGLTexture();
        }
        
        // Optimization: return early if texture is already set for this stage
        if (g_texture_stages[Stage] == texture_id)
            return D3D_OK;

        g_texture_stages[Stage] = texture_id;
        return D3D_OK;
    }
    
    virtual HRESULT CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format,
                                     D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer,
                                     void* pSharedHandle) override {
        if (!ppIndexBuffer) return -1;
        *ppIndexBuffer = new D3D9IndexBuffer(Length, Format);
        return D3D_OK;
    }

    virtual HRESULT CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF,
                                      D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer,
                                      void* pSharedHandle) override {
        if (!ppVertexBuffer) return -1;
        *ppVertexBuffer = new D3D9VertexBuffer(Length);
        return D3D_OK;
    }

    virtual HRESULT SetIndices(IDirect3DIndexBuffer9* pIndexData) override {
        if (pIndexData) {
            D3D9IndexBuffer* ib = static_cast<D3D9IndexBuffer*>(pIndexData);
            m_current_index_buffer = ib->GetGLBuffer();
            m_current_ib_format = ib->GetFormat();
        } else {
            m_current_index_buffer = 0;
            m_current_ib_format = D3DFMT_INDEX16;
        }
        return D3D_OK;
    }
    
    virtual HRESULT SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
                                   UINT OffsetInBytes, UINT Stride) override {
#ifdef SRW_DIAGNOSTICS
        { static int logged = 0; if (SRW_LOG_ON(SRW_LOG_D3D) && StreamNumber != 0 && logged++ < 30) printf("[D3D9] SetStreamSource for stream %u (only stream 0 is supported)\n", (unsigned) StreamNumber); }
#endif
        if (StreamNumber != 0) return -1; // Only stream 0 supported for now
        
        m_current_stride = Stride; // Store stride
        m_current_vb_offset = pStreamData ? OffsetInBytes : 0;
#ifdef SRW_DIAGNOSTICS
        { static int logged = 0; if (SRW_LOG_ON(SRW_LOG_D3D) && OffsetInBytes != 0 && logged++ < 30) printf("[D3D9] SetStreamSource with an offset: %u bytes, stride %u\n", (unsigned) OffsetInBytes, (unsigned) Stride); }
#endif
        
        if (pStreamData) {
            D3D9VertexBuffer* vb = static_cast<D3D9VertexBuffer*>(pStreamData);
            m_current_vertex_buffer = vb->GetGLBuffer();
            m_current_vb_object = vb;
        } else {
            m_current_vertex_buffer = 0;
            m_current_vb_object = nullptr;
        }
        return D3D_OK;
    }
    
    virtual HRESULT SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override {
        if (!pMatrix) return -1;
        switch (State) {
            case D3DTS_WORLD: 
                m_world = *pMatrix;
                g_transform_world = *pMatrix;
#ifdef SRW_DIAGNOSTICS
                {
                    static int logged = 0;
                    if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 12)
                        printf("[D3D9] world %d: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]\n", logged,
                               pMatrix->m[0][0], pMatrix->m[0][1], pMatrix->m[0][2], pMatrix->m[0][3], pMatrix->m[1][0], pMatrix->m[1][1], pMatrix->m[1][2], pMatrix->m[1][3],
                               pMatrix->m[2][0], pMatrix->m[2][1], pMatrix->m[2][2], pMatrix->m[2][3], pMatrix->m[3][0], pMatrix->m[3][1], pMatrix->m[3][2], pMatrix->m[3][3]);
                }
#endif
                break;
            case D3DTS_VIEW: 
                m_view = *pMatrix;
                g_transform_view = *pMatrix;
#ifdef SRW_DIAGNOSTICS
                {
                    static int logged = 0;
                    if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 12)
                        printf("[D3D9] view %d: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]\n", logged,
                               pMatrix->m[0][0], pMatrix->m[0][1], pMatrix->m[0][2], pMatrix->m[0][3], pMatrix->m[1][0], pMatrix->m[1][1], pMatrix->m[1][2], pMatrix->m[1][3],
                               pMatrix->m[2][0], pMatrix->m[2][1], pMatrix->m[2][2], pMatrix->m[2][3], pMatrix->m[3][0], pMatrix->m[3][1], pMatrix->m[3][2], pMatrix->m[3][3]);
                }
#endif
                break;
            case D3DTS_PROJECTION: 
                m_projection = *pMatrix;
                g_transform_projection = *pMatrix;
                {
                    static int logged = 0;
                    if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 24)
                        printf("[D3D9] projection %d: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] (viewport %u,%u %ux%u)\n", logged,
                               pMatrix->m[0][0], pMatrix->m[0][1], pMatrix->m[0][2], pMatrix->m[0][3], pMatrix->m[1][0], pMatrix->m[1][1], pMatrix->m[1][2], pMatrix->m[1][3],
                               pMatrix->m[2][0], pMatrix->m[2][1], pMatrix->m[2][2], pMatrix->m[2][3], pMatrix->m[3][0], pMatrix->m[3][1], pMatrix->m[3][2], pMatrix->m[3][3],
                               (unsigned) g_viewport.X, (unsigned) g_viewport.Y, (unsigned) g_viewport.Width, (unsigned) g_viewport.Height);
                }
                break;
            case D3DTS_TEXTURE0: case D3DTS_TEXTURE1: case D3DTS_TEXTURE2: case D3DTS_TEXTURE3:
            case D3DTS_TEXTURE4: case D3DTS_TEXTURE5: case D3DTS_TEXTURE6: case D3DTS_TEXTURE7:
                g_texture_matrices[State - D3DTS_TEXTURE0] = *pMatrix;
                break;
            default:
#ifdef SRW_DIAGNOSTICS
                { static int logged = 0; if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 20) printf("[D3D9] SetTransform of state %u is not handled\n", (unsigned) State); }
#endif
                return -1;
        }
        return D3D_OK;
    }
    
    // D3D9Device methods continued...
    
    DWORD m_render_states[256];

    virtual HRESULT SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override {
        if ((UINT)State < 256) m_render_states[State] = Value;
        if ((int)State == 152) { // D3DRS_CLIPPLANEENABLE
             m_clip_mask = Value;
             m_clip_plane_enabled = (Value & 0x3F) != 0;
#ifdef SRW_DIAGNOSTICS
             { static int logged = 0; if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 40) printf("[D3D9] D3DRS_CLIPPLANEENABLE %x\n", (unsigned) Value); }
#endif
             return D3D_OK;
        }
        
        switch (State) {
            case D3DRS_ALPHABLENDENABLE:
                if (Value) glEnable(GL_BLEND);
                else glDisable(GL_BLEND);
                break;
                
            case D3DRS_SRCBLEND:
                m_src_blend = d3d_blend_to_gl(Value);
                glBlendFunc(m_src_blend, m_dst_blend);
                break;
                
            case D3DRS_DESTBLEND:
                m_dst_blend = d3d_blend_to_gl(Value);
                glBlendFunc(m_src_blend, m_dst_blend);
                break;

            case D3DRS_CULLMODE:
                // glFrontFace is GL_CW as in Direct3D: D3DCULL_CW (2) culls the front faces, D3DCULL_CCW (3) the back ones

                if (Value == D3DCULL_NONE) {
                    glDisable(GL_CULL_FACE);
                } else {
                    glEnable(GL_CULL_FACE);
                    glCullFace(Value == D3DCULL_CW ? GL_FRONT : GL_BACK);
                }
                break;

            case D3DRS_ZENABLE:
                if (Value) glEnable(GL_DEPTH_TEST);
                else glDisable(GL_DEPTH_TEST);
                break;
                
            case D3DRS_ZWRITEENABLE:
                glDepthMask(Value ? GL_TRUE : GL_FALSE);
                break;

            case D3DRS_ZFUNC:
                glDepthFunc(d3d_cmp_to_gl(Value));
                break;

            case D3DRS_ALPHATESTENABLE:
                m_alpha_test_enabled = (Value != 0);
                break;

            case D3DRS_ALPHAREF:
                m_alpha_ref = Value;
                break;

            case D3DRS_SCISSORTESTENABLE:
                if (Value) {
                    glEnable(GL_SCISSOR_TEST);
                    g_cached_scissor_test = GL_TRUE;
                } else {
                    glDisable(GL_SCISSOR_TEST);
                    g_cached_scissor_test = GL_FALSE;
                }
                break;

            case D3DRS_STENCILENABLE:
                if (Value) glEnable(GL_STENCIL_TEST);
                else glDisable(GL_STENCIL_TEST);
                break;
            case D3DRS_STENCILFUNC: glStencilFunc(d3d_cmp_to_gl(Value), g_current_stencil_ref, 0xFFFFFFFF); break;
            case D3DRS_STENCILREF: g_current_stencil_ref = Value; break;
            case D3DRS_STENCILFAIL: glStencilOp(d3d_stencil_op_to_gl(Value), GL_KEEP, GL_KEEP); break;
            case D3DRS_STENCILZFAIL: glStencilOp(GL_KEEP, d3d_stencil_op_to_gl(Value), GL_KEEP); break;
            case D3DRS_STENCILPASS: glStencilOp(GL_KEEP, GL_KEEP, d3d_stencil_op_to_gl(Value)); break;
            
            case D3DRS_COLORWRITEENABLE:
                glColorMask((Value & D3DCOLORWRITEENABLE_RED) ? GL_TRUE : GL_FALSE,
                           (Value & D3DCOLORWRITEENABLE_GREEN) ? GL_TRUE : GL_FALSE,
                           (Value & D3DCOLORWRITEENABLE_BLUE) ? GL_TRUE : GL_FALSE,
                           (Value & D3DCOLORWRITEENABLE_ALPHA) ? GL_TRUE : GL_FALSE);
                break;
            case D3DRS_TEXTUREFACTOR:
                m_texture_factor = Value;
                break;

            case D3DRS_FOGENABLE:
                m_fog_enabled = (Value != 0);
                break;
            case D3DRS_FOGCOLOR:
                m_fog_color = Value;
                break;
            case D3DRS_FOGSTART:
                m_fog_start = *(float*)&Value;
                break;
            case D3DRS_FOGEND:
                m_fog_end = *(float*)&Value;
                break;
            case D3DRS_FOGTABLEMODE:
                // Only D3DFOG_LINEAR is implemented. D3DFOG_EXP and D3DFOG_EXP2 are not supported.
                // TODO: implement EXP/EXP2 fog modes if needed.
                break;
            case D3DRS_FOGVERTEXMODE:
                // Vertex fog mode is ignored; table fog (per-pixel) is always used.
                // TODO: implement vertex fog mode if needed.
                break;
            case D3DRS_RANGEFOGENABLE:
                // Range-based fog is not implemented; depth (eye-space Z) is used instead.
                // TODO: implement range-based fog if needed.
                break;
        }
        return D3D_OK;
    }

    virtual HRESULT SetScissorRect(const RECT* pRect) override {
        if (!pRect) return D3DERR_INVALIDCALL;
        m_scissor_rect = *pRect;
        
        if (!m_current_rt) return D3D_OK;

        // WebGL Y is bottom-to-top: flip Y origin
        int gl_y = m_current_rt->m_height - pRect->bottom;
        int gl_w = pRect->right - pRect->left;
        int gl_h = pRect->bottom - pRect->top;
        glScissor(pRect->left, gl_y, gl_w, gl_h);
        g_cached_scissor_box[0] = pRect->left;
        g_cached_scissor_box[1] = gl_y;
        g_cached_scissor_box[2] = gl_w;
        g_cached_scissor_box[3] = gl_h;
        return D3D_OK;
    }

    virtual HRESULT GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override {
        if (!pValue) return D3DERR_INVALIDCALL;
        if ((UINT)State < 256) *pValue = m_render_states[State];
        else *pValue = 0;
        return D3D_OK;
    }

    virtual HRESULT CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                             D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                             BOOL Discard, IDirect3DSurface9** ppSurface, void* pSharedHandle) override {
        if (!ppSurface) return D3DERR_INVALIDCALL;
        
        GLuint rb;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, Width, Height); // Use standard D24S8
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        
        *ppSurface = new D3D9Surface(Width, Height, rb, true);
        return D3D_OK;
    }

    virtual HRESULT SetClipPlane(DWORD Index, const float* pPlane) override {
        if (Index >= 6 || !pPlane) return D3DERR_INVALIDCALL;
#ifdef SRW_DIAGNOSTICS
        { static int logged = 0; if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 40) printf("[D3D9] SetClipPlane %u: %g %g %g %g\n", (unsigned) Index, pPlane[0], pPlane[1], pPlane[2], pPlane[3]); }
#endif
        memcpy(m_clip_planes[Index], pPlane, 4 * sizeof(float));
        return D3D_OK;
    }

    virtual HRESULT GetClipPlane(DWORD Index, float* pPlane) override {
        if (Index >= 6 || !pPlane) return D3DERR_INVALIDCALL;
        memcpy(pPlane, m_clip_planes[Index], 4 * sizeof(float));
        return D3D_OK;
    }

    // FFP APIs Implementation
    virtual HRESULT GetDeviceCaps(D3DCAPS9* pCaps) override {
        if (!pCaps) return -1;
        
        memset(pCaps, 0, sizeof(D3DCAPS9));
        
#ifdef __EMSCRIPTEN__
        // CRITICAL: Lie about shader support to force FFP mode
        pCaps->VertexShaderVersion = 0;  // No vertex shader support
        pCaps->PixelShaderVersion = 0;   // No pixel shader support
#else
        // Native builds could report real capabilities
        pCaps->VertexShaderVersion = D3DVS_VERSION(2, 0);
        pCaps->PixelShaderVersion = D3DPS_VERSION(2, 0);
#endif
        
        // Additional capabilities
        pCaps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT; // Claim HW T&L support
        pCaps->MaxPrimitiveCount = 0xFFFFFFFF;
        pCaps->NumSimultaneousRTs = 1;
        pCaps->MaxStreams = 16;
        pCaps->MaxUserClipPlanes = 1; // Emulated via fragment shader discard
        pCaps->RasterCaps = 0; // No W-fog
        pCaps->MaxVertexShaderConst = 0; // No shader constants since VS version is 0
        
        return D3D_OK;
    }

    virtual UINT GetAvailableTextureMem() override {
        return 128 * 1024 * 1024; // 128MB
    }

    virtual HRESULT ValidateDevice(DWORD* pNumPasses) override {
        if (pNumPasses) *pNumPasses = 1;
        return D3D_OK;
    }
    
    virtual HRESULT SetLight(DWORD Index, const D3DLIGHT9* pLight) override {
        if (Index >= 8 || !pLight) return D3DERR_INVALIDCALL;
        g_lights[Index] = *pLight;
        return D3D_OK;
    }
    
    virtual HRESULT LightEnable(DWORD Index, BOOL Enable) override {
        if (Index >= 8) return D3DERR_INVALIDCALL;
        g_light_enabled[Index] = Enable != FALSE;
        return D3D_OK;
    }

    virtual HRESULT GetLightEnable(DWORD Index, BOOL* pEnable) override {
        if (Index >= 8 || !pEnable) return D3DERR_INVALIDCALL;
        *pEnable = g_light_enabled[Index] ? TRUE : FALSE;
        return D3D_OK;
    }

    virtual HRESULT GetLight(DWORD Index, D3DLIGHT9* pLight) override {
        if (Index >= 8 || !pLight) return D3DERR_INVALIDCALL;
        *pLight = g_lights[Index];
        return D3D_OK;
    }
    
    virtual HRESULT SetMaterial(const D3DMATERIAL9* pMaterial) override {
        if (!pMaterial) return D3DERR_INVALIDCALL;
        g_material = *pMaterial;
        return D3D_OK;
    }
    
    virtual HRESULT SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override {
        if (Stage >= 8 || Type >= 33) return D3DERR_INVALIDCALL;
        g_texture_stage_states[Stage][Type] = Value;
        return D3D_OK;
    }
    
    virtual HRESULT SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override {
        if (Sampler >= 8) return D3DERR_INVALIDCALL;
        if (Type < 14) {
            if (Type == D3DSAMP_MAXANISOTROPY && Value != g_sampler_states[Sampler][Type]) LOG_SURFACE_CALL(aniso, "SetSamplerState: sampler %u, max anisotropy %u", (unsigned) Sampler, (unsigned) Value);
            g_sampler_states[Sampler][Type] = Value;
        }
        return D3D_OK;
    }
    
    virtual HRESULT GetViewport(D3DVIEWPORT9* pViewport) override {
        if (!pViewport) return D3DERR_INVALIDCALL;
        *pViewport = g_viewport;
        return D3D_OK;
    }
    
    virtual HRESULT SetViewport(const D3DVIEWPORT9* pViewport) override {
        if (!pViewport) return D3DERR_INVALIDCALL;
        g_viewport = *pViewport;
        {
            static int logged = 0;
            if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 60)
                printf("[D3D9] SetViewport %d: %u,%u %ux%u z %.2f..%.2f (target %ux%u)\n", logged, (unsigned) pViewport->X, (unsigned) pViewport->Y, (unsigned) pViewport->Width,
                       (unsigned) pViewport->Height, pViewport->MinZ, pViewport->MaxZ, m_current_rt ? (unsigned) m_current_rt->m_width : 0u, m_current_rt ? (unsigned) m_current_rt->m_height : 0u);
        }

        if (!m_current_rt) return D3D_OK;
        if ((uint64_t) pViewport->X + pViewport->Width > m_current_rt->m_width || (uint64_t) pViewport->Y + pViewport->Height > m_current_rt->m_height) {
            static int logged = 0;   // (D3D9 refuses such a viewport; it is kept here, the draws are clipped to the target)
            if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 20)
                printf("[D3D9] SetViewport: %u,%u %ux%u is outside the %ux%u target\n", (unsigned) pViewport->X, (unsigned) pViewport->Y,
                       (unsigned) pViewport->Width, (unsigned) pViewport->Height, (unsigned) m_current_rt->m_width, (unsigned) m_current_rt->m_height);
        }

        // WebGL Y is bottom-to-top: flip Y origin
        int gl_y = m_current_rt->m_height - (pViewport->Y + pViewport->Height);
        glViewport(pViewport->X, gl_y, pViewport->Width, pViewport->Height);
        g_cached_viewport[0] = pViewport->X;
        g_cached_viewport[1] = gl_y;
        g_cached_viewport[2] = pViewport->Width;
        g_cached_viewport[3] = pViewport->Height;

        glDepthRangef(pViewport->MinZ, pViewport->MaxZ);
        return D3D_OK;
    }
    
    virtual HRESULT GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override {
        if (!pMatrix) return -1;
        
        switch (State) {
            case D3DTS_WORLD:
                *pMatrix = m_world;
                break;
            case D3DTS_VIEW:
                *pMatrix = m_view;
                break;
            case D3DTS_PROJECTION:
                *pMatrix = m_projection;
                break;
            default:
                return -1;
        }
        
        return D3D_OK;
    }
    
    // Shader stubs (log warnings if called)
    virtual HRESULT CreateVertexDeclaration(const void* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override {
        if (!pVertexElements || !ppDecl) return -1;
        *ppDecl = new D3D9VertexDeclaration(pVertexElements);
        return D3D_OK;
    }

    virtual HRESULT SetVertexDeclaration(void* pDecl) override {
        if (pDecl) g_current_fvf = ((D3D9VertexDeclaration*) pDecl)->m_fvf;
        return D3D_OK;
    }

    virtual HRESULT SetVertexShader(void* pShader) override {
        // Debug: Only log warning once to reduce log spam during loading
        if (!s_vertex_shader_warning_shown) {
            printf("[D3D9] Warning: the game called SetVertexShader, but vertex shaders are not supported (further warnings are left out).\n");
            s_vertex_shader_warning_shown = true;
        }
        return D3D_OK;
    }
    
    virtual HRESULT SetPixelShader(void* pShader) override {
        int id = pShader ? static_cast<D3D9PixelShader*>(static_cast<IDirect3DPixelShader9*>(pShader))->m_id : 0;
        // the program of the shader (built when it is first set); a shader that does not build draws with the fixed function pipeline
        g_current_ps_id = id != 0 ? select_program(id) : (select_program(0), 0);
        return D3D_OK;
    }
    
    virtual HRESULT SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override {
        // Debug: Only log warning once to reduce log spam during loading
        if (!s_vertex_shader_constant_warning_shown) {
            printf("[D3D9] Warning: the game called SetVertexShaderConstantF, but vertex shaders are not supported (further warnings are left out).\n");
            s_vertex_shader_constant_warning_shown = true;
        }
        return D3D_OK;
    }


    virtual HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                        INT BaseVertexIndex,
                                        UINT MinVertexIndex,
                                        UINT NumVertices,
                                        UINT startIndex,
                                        UINT primCount) override {
        PERF_DRAW_TIMER
        SRW_DRAW_STAT(primCount);
        trace_draw("DrawIndexedPrimitive", primCount);
        if (g_shader_program == 0) init_shaders();
        glUseProgram(g_shader_program);

        // FBO Y-flip and clip plane uniforms
        glUniform1i(g_fbo_flip_y_uniform, m_fbo_rendering ? 1 : 0);
        glUniform1i(g_clip_plane_enabled_uniform, m_clip_plane_enabled ? 1 : 0);
        if (m_clip_plane_enabled) UploadClipPlanes();

#ifdef SRW_DIAGNOSTICS
        {
            static int logged = 0;

            if (SRW_LOG_ON(SRW_LOG_D3D) && logged < 12) {
                logged++;
                printf("[D3D9] indexed draw %d: type %d, %u vertices, %u primitives, fvf %x, stride %u, vertex buffer %u, index buffer %u, rt %s\n",
                       logged, (int) PrimitiveType, (unsigned) NumVertices, (unsigned) primCount, (unsigned) g_current_fvf, (unsigned) m_current_stride,
                       (unsigned) m_current_vertex_buffer, (unsigned) m_current_index_buffer, m_fbo_rendering ? "texture" : "screen");
            }
        }
#endif
        if (m_current_vertex_buffer == 0 || m_current_index_buffer == 0) return -1;

        // Calculate MVP
        D3DMATRIX view_model, mvp;
        if (g_current_fvf & D3DFVF_XYZRHW) {
            float sw = (m_current_rt) ? (float)m_current_rt->m_width : (float)g_logical_width;
            float sh = (m_current_rt) ? (float)m_current_rt->m_height : (float)g_logical_height;
            
            memset(&mvp, 0, sizeof(D3DMATRIX));
            mvp.m[0][0] = 2.0f / sw;
            mvp.m[1][1] = -2.0f / sh;
            mvp.m[2][2] = 1.0f; 
            mvp.m[3][0] = -1.0f;
            mvp.m[3][1] = 1.0f;
            mvp.m[3][3] = 1.0f;
            
            // 2D UI states
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_FALSE);
        } else {
            matrix_multiply(&view_model, &m_world, &m_view);
            matrix_multiply(&mvp, &view_model, &m_projection);
#ifdef SRW_DIAGNOSTICS
            {
                /* where the vertices of the draw land on the screen: their position through the matrices on the CPU */
                static int logged = 0;

                if (SRW_LOG_ON(SRW_LOG_D3D) && (logged < 80 || g_trace_frame) && m_current_vb_object && m_current_vb_object->ShadowData() && (g_current_fvf & D3DFVF_XYZ)) {
                    const uint8_t* base = m_current_vb_object->ShadowData() + m_current_vb_offset;
                    UINT size = m_current_vb_object->ShadowSize();
                    UINT first = (BaseVertexIndex + MinVertexIndex), count = NumVertices < 64 ? NumVertices : 64, inside = 0, front = 0;
                    float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f, wmin = 1e9f, wmax = -1e9f;

                    for (UINT i = 0; i < count; i++) {
                        UINT at = m_current_vb_offset + (first + i) * m_current_stride;
                        if (at + 12 > size) break;
                        const float* p = reinterpret_cast<const float*>(base + (first + i) * m_current_stride);
                        float x = p[0] * mvp.m[0][0] + p[1] * mvp.m[1][0] + p[2] * mvp.m[2][0] + mvp.m[3][0];
                        float y = p[0] * mvp.m[0][1] + p[1] * mvp.m[1][1] + p[2] * mvp.m[2][1] + mvp.m[3][1];
                        float z = p[0] * mvp.m[0][2] + p[1] * mvp.m[1][2] + p[2] * mvp.m[2][2] + mvp.m[3][2];
                        float w = p[0] * mvp.m[0][3] + p[1] * mvp.m[1][3] + p[2] * mvp.m[2][3] + mvp.m[3][3];
                        if (w > 0) {
                            front++;
                            float nx = x / w, ny = y / w, nz = z / w;
                            if (nx < xmin) xmin = nx;
                            if (nx > xmax) xmax = nx;
                            if (ny < ymin) ymin = ny;
                            if (ny > ymax) ymax = ny;
                            if (nx >= -1 && nx <= 1 && ny >= -1 && ny <= 1 && nz >= 0 && nz <= 1) inside++;
                        }
                        if (w < wmin) wmin = w;
                        if (w > wmax) wmax = w;
                    }
                    logged++;
                    if (inside > 0 || logged <= 6 || g_trace_frame)
                        printf("[D3D9]   states: z %u/%u func %u, cull %u, blend %u (%u,%u), alphatest %u (func %u ref %u), fog %u (color %08x, %.2f..%.2f, density %.3f, mode %u), lighting %u, scissor %u [%d,%d,%d,%d], colorwrite %x, stencil %u, texture0 %u (colorop %u, alphaop %u)\n",
                               m_render_states[D3DRS_ZENABLE], m_render_states[D3DRS_ZWRITEENABLE], m_render_states[D3DRS_ZFUNC], m_render_states[D3DRS_CULLMODE],
                               m_render_states[D3DRS_ALPHABLENDENABLE], m_render_states[D3DRS_SRCBLEND], m_render_states[D3DRS_DESTBLEND], m_render_states[D3DRS_ALPHATESTENABLE],
                               m_render_states[D3DRS_ALPHAFUNC], m_render_states[D3DRS_ALPHAREF], m_render_states[D3DRS_FOGENABLE], (unsigned) m_render_states[D3DRS_FOGCOLOR],
                               *reinterpret_cast<float*>(&m_render_states[D3DRS_FOGSTART]), *reinterpret_cast<float*>(&m_render_states[D3DRS_FOGEND]),
                               *reinterpret_cast<float*>(&m_render_states[D3DRS_FOGDENSITY]), m_render_states[D3DRS_FOGTABLEMODE], m_render_states[D3DRS_LIGHTING],
                               m_render_states[D3DRS_SCISSORTESTENABLE], (int) m_scissor_rect.left, (int) m_scissor_rect.top, (int) m_scissor_rect.right, (int) m_scissor_rect.bottom,
                               (unsigned) m_render_states[D3DRS_COLORWRITEENABLE], m_render_states[D3DRS_STENCILENABLE], (unsigned) g_texture_stages[0], (unsigned) g_texture_stage_states[0][D3DTSS_COLOROP], (unsigned) g_texture_stage_states[0][D3DTSS_ALPHAOP]);
                    if (g_trace_frame && NumVertices <= 8) {
                        for (int stage = 0; stage < 4; stage++)
                            printf("[D3D9]   stage %d: texture %u, colorop %u args %u %u, alphaop %u args %u %u, texcoord index %u, transform flags %u; sampler min %u mag %u mip %u address %u %u\n", stage, (unsigned) g_texture_stages[stage],
                                   (unsigned) g_texture_stage_states[stage][D3DTSS_COLOROP], (unsigned) g_texture_stage_states[stage][D3DTSS_COLORARG1], (unsigned) g_texture_stage_states[stage][D3DTSS_COLORARG2],
                                   (unsigned) g_texture_stage_states[stage][D3DTSS_ALPHAOP], (unsigned) g_texture_stage_states[stage][D3DTSS_ALPHAARG1], (unsigned) g_texture_stage_states[stage][D3DTSS_ALPHAARG2],
                                   (unsigned) g_texture_stage_states[stage][D3DTSS_TEXCOORDINDEX], (unsigned) g_texture_stage_states[stage][D3DTSS_TEXTURETRANSFORMFLAGS],
                                   (unsigned) g_sampler_states[stage][D3DSAMP_MINFILTER], (unsigned) g_sampler_states[stage][D3DSAMP_MAGFILTER], (unsigned) g_sampler_states[stage][D3DSAMP_MIPFILTER],
                                   (unsigned) g_sampler_states[stage][D3DSAMP_ADDRESSU], (unsigned) g_sampler_states[stage][D3DSAMP_ADDRESSV]);
                        for (UINT i = 0; i < NumVertices; i++) {
                            const float* p = reinterpret_cast<const float*>(base + (first + i) * m_current_stride);
                            if (m_current_vb_offset + (first + i) * m_current_stride + 12 > size) break;
                            float x = p[0] * mvp.m[0][0] + p[1] * mvp.m[1][0] + p[2] * mvp.m[2][0] + mvp.m[3][0];
                            float y = p[0] * mvp.m[0][1] + p[1] * mvp.m[1][1] + p[2] * mvp.m[2][1] + mvp.m[3][1];
                            float z = p[0] * mvp.m[0][2] + p[1] * mvp.m[1][2] + p[2] * mvp.m[2][2] + mvp.m[3][2];
                            float w = p[0] * mvp.m[0][3] + p[1] * mvp.m[1][3] + p[2] * mvp.m[2][3] + mvp.m[3][3];
                            printf("[D3D9]   vertex %u: model %.2f %.2f %.2f -> clip %.3f %.3f %.3f %.3f (depth %.3f)", (unsigned) i, (double) p[0], (double) p[1], (double) p[2], (double) x, (double) y, (double) z, (double) w, w != 0 ? (double) (z / w * 0.5f + 0.5f) : 0.0);
                            if (m_current_stride >= 28) { const float* t = p + (m_current_stride / 4) - 2; printf(", last uv %.3f %.3f", (double) t[0], (double) t[1]); }
                            if (g_current_fvf & D3DFVF_DIFFUSE) { const uint32_t* c = reinterpret_cast<const uint32_t*>(p + 3 + ((g_current_fvf & D3DFVF_NORMAL) ? 3 : 0)); printf(", diffuse %08x", (unsigned) *c); }
                            printf("\n");
                        }
                    }
                    printf("[D3D9] draw %d (fvf %x, %u vertices, first %u): world at %.1f %.1f %.1f; of %u sampled, %u in front, %u inside the view; ndc x %.2f..%.2f y %.2f..%.2f, w %.2f..%.2f\n",
                           logged, (unsigned) g_current_fvf, (unsigned) NumVertices, (unsigned) first, m_world.m[3][0], m_world.m[3][1], m_world.m[3][2], (unsigned) count, (unsigned) front,
                           (unsigned) inside, xmin, xmax, ymin, ymax, wmin, wmax);
                }
            }
#endif
        }

        // winding: see choose_front_face
        glFrontFace(choose_front_face(view_model, m_fbo_rendering, (g_current_fvf & D3DFVF_XYZRHW) != 0));

        gl_set_matrix_uniform(g_mvp_uniform, &mvp);

        // Set texture factor
        if (g_texture_factor_uniform != -1) {
            float r = ((m_texture_factor >> 16) & 0xFF) / 255.0f;
            float g = ((m_texture_factor >> 8) & 0xFF) / 255.0f;
            float b = (m_texture_factor & 0xFF) / 255.0f;
            float a = ((m_texture_factor >> 24) & 0xFF) / 255.0f;
            glUniform4f(g_texture_factor_uniform, r, g, b, a);
        }

        // mipmap LOD bias (D3D9 stores the float as a DWORD), with an offset of -0.75: WebGL selects lower resolution levels
        if (g_lod_bias_uniform != -1) {
            DWORD bias_bits = g_sampler_states[0][D3DSAMP_MIPMAPLODBIAS];
            float lod_bias;
            memcpy(&lod_bias, &bias_bits, sizeof(float));
            lod_bias -= 0.75f;
            glUniform1f(g_lod_bias_uniform, lod_bias);
        }

        // Set Stage 0 texture stage states
        if (g_stage0_colorop_uniform != -1) {
            glUniform1i(g_stage0_colorop_uniform, g_texture_stage_states[0][D3DTSS_COLOROP]);
        }
        if (g_stage0_colorarg1_uniform != -1) {
            glUniform1i(g_stage0_colorarg1_uniform, g_texture_stage_states[0][D3DTSS_COLORARG1]);
        }
        if (g_stage0_colorarg2_uniform != -1) {
            glUniform1i(g_stage0_colorarg2_uniform, g_texture_stage_states[0][D3DTSS_COLORARG2]);
        }
        if (g_stage0_alphaop_uniform != -1) {
            glUniform1i(g_stage0_alphaop_uniform, g_texture_stage_states[0][D3DTSS_ALPHAOP]);
        }
        if (g_stage0_alphaarg1_uniform != -1) {
            glUniform1i(g_stage0_alphaarg1_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG1]);
        }
        if (g_stage0_alphaarg2_uniform != -1) {
            glUniform1i(g_stage0_alphaarg2_uniform, g_texture_stage_states[0][D3DTSS_ALPHAARG2]);
        }

        // Fog uniforms
        {
            bool apply_fog = m_fog_enabled && !(g_current_fvf & D3DFVF_XYZRHW);
            glUniform1i(g_fog_enabled_uniform, apply_fog ? 1 : 0);
            if (apply_fog) {
                float r = ((m_fog_color >> 16) & 0xFF) / 255.0f;
                float g_c = ((m_fog_color >> 8) & 0xFF) / 255.0f;
                float b = (m_fog_color & 0xFF) / 255.0f;
                glUniform3f(g_fog_color_uniform, r, g_c, b);
                glUniform1f(g_fog_start_uniform, m_fog_start);
                glUniform1f(g_fog_end_uniform, m_fog_end);
                gl_set_matrix_uniform(g_view_model_uniform, &view_model);
            }
        }

        // Bind buffers
        glBindBuffer(GL_ARRAY_BUFFER, m_current_vertex_buffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_current_index_buffer);
        
        // Dynamic FVF Parsing
        bool has_xyz = (g_current_fvf & D3DFVF_XYZ) != 0;
        bool has_xyzrhw = (g_current_fvf & D3DFVF_XYZRHW) != 0;
        bool has_normal = (g_current_fvf & D3DFVF_NORMAL) != 0;
        bool has_diffuse = (g_current_fvf & D3DFVF_DIFFUSE) != 0;
        int tex_count = (g_current_fvf >> 8) & 0xF;

        GLsizei stride = 0;
        size_t position_offset = 0;
        size_t normal_offset = 0;
        size_t color_offset = 0;
        size_t tex_offset = 0;

        // XYZ and XYZRHW are mutually exclusive in D3D9 FVF
        if (has_xyzrhw) { position_offset = stride; stride += 16; }
        else if (has_xyz) { position_offset = stride; stride += 12; }
        if (has_normal) { normal_offset = stride; stride += 12; }
        if (has_diffuse) { color_offset = stride; stride += 4; }
        tex_offset = stride;
        stride += tex_count * 8;

        // Use stored stride if valid
        if (m_current_stride > 0) stride = m_current_stride;

        // BaseVertexIndex support
        size_t base_offset = BaseVertexIndex * stride + m_current_vb_offset;

        // Enable/Set Pointers with safety guards
        if ((has_xyz || has_xyzrhw) && g_position_location != -1) {
            glEnableVertexAttribArray(g_position_location);
            GLint pos_component_count = has_xyzrhw ? 4 : 3;
            glVertexAttribPointer(g_position_location, pos_component_count, GL_FLOAT, GL_FALSE, stride, (void*)(base_offset + position_offset));
        }

        // D3D9 FFP Lighting
        bool lighting_enabled = has_normal && !has_xyzrhw && m_render_states[D3DRS_LIGHTING];
        if (lighting_enabled) {
            // Normal attribute
            if (g_normal_location != -1) {
                glEnableVertexAttribArray(g_normal_location);
                glVertexAttribPointer(g_normal_location, 3, GL_FLOAT, GL_FALSE, stride, (void*)(base_offset + normal_offset));
            }

            glUniform1i(g_lighting_enabled_uniform, 1);
            SetFFPLightingUniforms(&m_world, m_render_states, has_diffuse);

            glDisableVertexAttribArray(g_color_location);
            glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f);
            if (has_diffuse && g_color_location != -1) {
                glEnableVertexAttribArray(g_color_location);
                glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)(base_offset + color_offset));
            }
        } else {
            glUniform1i(g_lighting_enabled_uniform, 0);
            // Always set world matrix for clip plane (v_position_world) even without lighting
            gl_set_matrix_uniform(g_world_uniform, &m_world);
            if (g_normal_location != -1) {
                glDisableVertexAttribArray(g_normal_location);
            }

            if (has_diffuse && g_color_location != -1) {
                glEnableVertexAttribArray(g_color_location);
                glVertexAttribPointer(g_color_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)(base_offset + color_offset));
            } else if (g_color_location != -1) {
                glDisableVertexAttribArray(g_color_location);
                glVertexAttrib4f(g_color_location, 1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        bind_texcoords(g_current_fvf, stride, base_offset + tex_offset);
        
        // Texture setup
        if (g_texture_stages[0] != 0 && tex_count > 0) {
            glActiveTexture(GL_TEXTURE0);
            
            // Optimization: Avoid redundant texture binding
            if (g_last_bound_texture_id != g_texture_stages[0]) {
                glBindTexture(GL_TEXTURE_2D, g_texture_stages[0]);
                g_last_bound_texture_id = g_texture_stages[0];
            }
            
            // Apply cached sampler states
            ApplySamplerStates(0);

            glUniform1i(g_texture_uniform, 0);
            glUniform1i(g_use_texture_uniform, 1);

            // Texture transform
            if (g_texture_stage_states[0][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_DISABLE) {
                // D3D row-major matrices. Shader does u_tex_matrix * vec4(uv, 0, 1).
                // We need to pass the matrix correctly.
                glUniformMatrix4fv(g_tex_matrix_uniform, 1, GL_FALSE, &g_texture_matrices[0].m[0][0]);
                glUniform1i(g_use_tex_matrix_uniform, 1);
            } else {
                glUniform1i(g_use_tex_matrix_uniform, 0);
            }
        } else {
            glUniform1i(g_use_texture_uniform, 0);
            glUniform1i(g_use_tex_matrix_uniform, 0);
        }

        // Lightmap setup (Stage 1) - requires at least 2 texture coordinate sets
        if (g_texture_stages[1] != 0 && tex_count > 1) {
            glActiveTexture(GL_TEXTURE1);
            if (g_last_bound_lightmap_id != g_texture_stages[1]) {
                glBindTexture(GL_TEXTURE_2D, g_texture_stages[1]);
                g_last_bound_lightmap_id = g_texture_stages[1];
            }

            // Apply sampler states for Stage 1 (reuse shared logic for mipmap awareness)
            ApplySamplerStates(1);

            glUniform1i(g_texture_lm_uniform, 1);
            glUniform1i(g_use_lightmap_uniform, 1);

            // Apply texture stage operation mode
            DWORD op = g_texture_stage_states[1][D3DTSS_COLOROP];
            glUniform1i(g_stage1_op_uniform, op);

            // Restore active texture unit to TEXTURE0 so subsequent draw calls
            // (e.g., DrawIndexedPrimitiveUP/DrawPrimitiveUP) don't accidentally
            // operate on GL_TEXTURE1 when they assume TEXTURE0 is active.
            glActiveTexture(GL_TEXTURE0);
        } else {
            glUniform1i(g_use_lightmap_uniform, 0);
        }

        glUniform1i(g_alpha_test_enabled_uniform, m_alpha_test_enabled ? 1 : 0);
        glUniform1f(g_alpha_ref_uniform, (float)m_alpha_ref);
        UploadTextureStages(view_model);

        // Draw
        GLenum gl_primitive_type = GL_TRIANGLES;
        UINT index_count = 0;
        switch (PrimitiveType) {
            case D3DPT_TRIANGLELIST: gl_primitive_type = GL_TRIANGLES; index_count = primCount * 3; break;
            case D3DPT_TRIANGLESTRIP: gl_primitive_type = GL_TRIANGLE_STRIP; index_count = primCount + 2; break;
            case D3DPT_TRIANGLEFAN: gl_primitive_type = GL_TRIANGLE_FAN; index_count = primCount + 2; break;
            case D3DPT_LINELIST: gl_primitive_type = GL_LINES; index_count = primCount * 2; break;
            case D3DPT_LINESTRIP: gl_primitive_type = GL_LINE_STRIP; index_count = primCount + 1; break;
            case D3DPT_POINTLIST: gl_primitive_type = GL_POINTS; index_count = primCount; break;
        }

        // Offset in index buffer (respects 16-bit vs 32-bit index format)
        bool is_index32 = (m_current_ib_format == D3DFMT_INDEX32);
        GLenum gl_index_type = is_index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        size_t index_stride = is_index32 ? 4 : 2;
        void* indices = (void*)(uintptr_t)(startIndex * index_stride);

        glDrawElements(gl_primitive_type, index_count, gl_index_type, indices);

        glDisableVertexAttribArray(g_position_location);
        glDisableVertexAttribArray(g_color_location);
        unbind_texcoords();
        if (g_normal_location != -1) glDisableVertexAttribArray(g_normal_location);

        // (no unbinding, see above)
        // (no unbinding, see above)

        // Restore GL states if XYZRHW path overrode them
        if (g_current_fvf & D3DFVF_XYZRHW) {
            if (m_render_states[D3DRS_ZENABLE]) glEnable(GL_DEPTH_TEST);
            if (m_render_states[D3DRS_ZWRITEENABLE]) glDepthMask(GL_TRUE);
            if (m_render_states[D3DRS_CULLMODE] != D3DCULL_NONE) glEnable(GL_CULL_FACE);
            // Restore blend state from tracked render states
            if (!m_render_states[D3DRS_ALPHABLENDENABLE]) glDisable(GL_BLEND);
            else { glEnable(GL_BLEND); glBlendFunc(m_src_blend, m_dst_blend); }
        }

        return D3D_OK;
    }

    // a render target surface of its own: a texture, which StretchRect can copy into and LockRect reads back (multisampling
    // is not supported for these: the program asks for a lockable one without it)
    HRESULT CreateRenderTargetSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface9** ppSurface) {
        if (!ppSurface || Width == 0 || Height == 0) return D3DERR_INVALIDCALL;
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) Width, (GLsizei) Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        g_cached_texture = 0xFFFFFFFF;   // (the binding of unit 0 changed)
        g_last_bound_texture_id = 0xFFFFFFFF;
        D3D9Surface* surface = new D3D9Surface(Width, Height, texture, false);
        surface->m_owns_texture = true;
        surface->m_format = Format;
        *ppSurface = surface;
        LOG_SURFACE_CALL(creatert, "CreateRenderTarget: %ux%u, format %u", (unsigned) Width, (unsigned) Height, (unsigned) Format);
        return D3D_OK;
    }

    virtual HRESULT CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, void* pSharedHandle) override {
        if (!ppSurface) return D3DERR_INVALIDCALL;
        LOG_SURFACE_CALL(plain, "CreateOffscreenPlainSurface: %ux%u, format %u, pool %d", (unsigned) Width, (unsigned) Height, (unsigned) Format, (int) Pool);
        *ppSurface = new D3D9Surface(Width, Height, 0, false); 
        return D3D_OK;
    }

    virtual HRESULT GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override {
        D3D9Surface* from = static_cast<D3D9Surface*>(pRenderTarget);
        D3D9Surface* to = static_cast<D3D9Surface*>(pDestSurface);
        if (from && to) LOG_SURFACE_CALL(rtdata, "GetRenderTargetData: %s %ux%u -> %s %ux%u (nothing is copied yet)", from->m_is_backbuffer ? "back buffer" : "surface", (unsigned) from->m_width,
                                         (unsigned) from->m_height, to->m_is_backbuffer ? "back buffer" : "surface", (unsigned) to->m_width, (unsigned) to->m_height);
        return D3D_OK;
    }

    virtual HRESULT GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override {
        LOG_SURFACE_CALL(front, "GetFrontBufferData (nothing is copied yet)");
        return D3D_OK;
    }

    virtual HRESULT TestCooperativeLevel() override {
        return D3D_OK;
    }

    virtual HRESULT Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override {
        // A new back buffer size resizes the canvas (and the offscreen framebuffer that Emscripten renders into),
        // the back buffer is the render target again and the viewport covers all of it.
        double t0 = emscripten_get_now(), t_canvas = t0, t_back = t0, t_clear = t0;
        if (pPresentationParameters && pPresentationParameters->BackBufferWidth > 0 && pPresentationParameters->BackBufferHeight > 0) {
            g_logical_width = pPresentationParameters->BackBufferWidth;
            g_logical_height = pPresentationParameters->BackBufferHeight;
            int result = emscripten_set_canvas_element_size(g_canvas_selector, g_logical_width, g_logical_height);
            int width = 0, height = 0;
            emscripten_get_canvas_element_size(g_canvas_selector, &width, &height);
            if (width != g_logical_width || height != g_logical_height) {
                srw_resize_offscreen_canvas(g_logical_width, g_logical_height);
                emscripten_get_canvas_element_size(g_canvas_selector, &width, &height);
            }
            printf("[D3D9] Reset to %dx%d (windowed %d): set size %d, canvas %dx%d\n", g_logical_width, g_logical_height,
                   (int) pPresentationParameters->Windowed, result, width, height);
        }
        t_canvas = emscripten_get_now();
        if (m_backbuffer_surface) {
            m_backbuffer_surface->m_width = g_logical_width;
            m_backbuffer_surface->m_height = g_logical_height;
        }
        if (pPresentationParameters) g_wanted_samples = (int) pPresentationParameters->MultiSampleType;
        back_buffer_setup(g_wanted_samples, g_logical_width, g_logical_height);
        t_back = emscripten_get_now();
        glBindFramebuffer(GL_FRAMEBUFFER, g_back_fbo);
        m_fbo_rendering = false;
        m_current_rt = m_backbuffer_surface;
        m_current_ds = nullptr;
        g_viewport.X = 0;
        g_viewport.Y = 0;
        g_viewport.Width = g_logical_width;
        g_viewport.Height = g_logical_height;
        g_viewport.MinZ = 0.0f;
        g_viewport.MaxZ = 1.0f;
        glViewport(0, 0, g_logical_width, g_logical_height);
        glDisable(GL_SCISSOR_TEST);
        g_cached_scissor_test = GL_FALSE;
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        t_clear = emscripten_get_now();
        g_cached_viewport[0] = 0; g_cached_viewport[1] = 0; g_cached_viewport[2] = g_logical_width; g_cached_viewport[3] = g_logical_height;
        {
            GLenum error = glGetError(), status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (error != GL_NO_ERROR || status != GL_FRAMEBUFFER_COMPLETE) printf("[D3D9] Reset: GL error %x, framebuffer status %x\n", error, status);
        }
        printf("[D3D9] Reset timing: canvas %.1f ms, back buffer %.1f ms, clear %.1f ms, checks %.1f ms\n", t_canvas - t0, t_back - t_canvas,
               t_clear - t_back, emscripten_get_now() - t_clear);
        return D3D_OK;
    }
    virtual void SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) override {
    }

    virtual void GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override {
        if(pRamp) memset(pRamp, 0, sizeof(D3DGAMMARAMP));
    }

    virtual HRESULT GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override {
        if (!pMode) return D3DERR_INVALIDCALL;
        pMode->Width = g_logical_width;
        pMode->Height = g_logical_height;
        pMode->RefreshRate = 60;
        pMode->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }

    virtual HRESULT CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override {
        if (ppSB) *ppSB = new D3D9StateBlock();
        return D3D_OK;
    }
};

// Initialize static member variables for shader warning suppression
bool D3D9Device::s_pixel_shader_warning_shown = false;
bool D3D9Device::s_vertex_shader_warning_shown = false;
bool D3D9Device::s_vertex_shader_constant_warning_shown = false;


class D3D9 : public IDirect3D9 {
public:
    virtual HRESULT CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
                                HWND hFocusWindow, DWORD BehaviorFlags,
                                D3DPRESENT_PARAMETERS* pPresentationParameters,
                                IDirect3DDevice9** ppReturnedDeviceInterface) override {
        if (!ppReturnedDeviceInterface) return -1;
        
        // Create WebGL context
        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;
        attrs.minorVersion = 0;
        
        // D3D9 backbuffer is D3DFMT_X8R8G8B8 (no alpha channel)
        // Canvas alpha must be disabled to match, otherwise DXT3 texture alpha
        // leaks into framebuffer and browser compositing darkens the output
        attrs.alpha = EM_FALSE;
        // the game renders from a worker thread, which presents frames explicitly
        attrs.explicitSwapControl = EM_TRUE;
        attrs.renderViaOffscreenBackBuffer = EM_TRUE;
        attrs.antialias = EM_FALSE;
        attrs.stencil = EM_TRUE;
        attrs.depth = EM_TRUE;

        {
            int want_width = (pPresentationParameters && pPresentationParameters->BackBufferWidth > 0) ? (int) pPresentationParameters->BackBufferWidth : 800;
            int want_height = (pPresentationParameters && pPresentationParameters->BackBufferHeight > 0) ? (int) pPresentationParameters->BackBufferHeight : 600;

            if (!srw_create_offscreen_canvas(want_width, want_height)) {
                printf("Failed to create WebGL context\n");
                return -1;
            }
        }
        EMSCRIPTEN_WEBGL_CONTEXT_HANDLE previous_context = g_webgl_context;
        g_webgl_context = emscripten_webgl_create_context(g_canvas_selector, &attrs);
        if (g_webgl_context <= 0) {
            printf("Failed to create WebGL context\n");
            return -1;
        }
        emscripten_webgl_make_context_current(g_webgl_context);
        EM_ASM({
            try {
                var info = GLctx.getExtension("WEBGL_debug_renderer_info");
                console.log("[D3D9] graphics: " + (info ? GLctx.getParameter(info.UNMASKED_RENDERER_WEBGL) : GLctx.getParameter(GLctx.RENDERER)) +
                            " (" + (info ? GLctx.getParameter(info.UNMASKED_VENDOR_WEBGL) : GLctx.getParameter(GLctx.VENDOR)) + ")");
            } catch (e) {
            }
        });

        // S3TC for compressed textures
        if (emscripten_webgl_enable_extension(g_webgl_context, "WEBGL_compressed_texture_s3tc")) {
            printf("[D3D9] S3TC compressed textures available\n");
        } else {
            g_s3tc_available = false;
            printf("[D3D9] S3TC compressed textures not available: DXT textures are decoded in software\n");
        }

        // anisotropic filtering
        if (emscripten_webgl_enable_extension(g_webgl_context, "EXT_texture_filter_anisotropic")) {
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_max_anisotropy);
            if (g_max_anisotropy > 16.0f) g_max_anisotropy = 16.0f;
            printf("[D3D9] Anisotropic filtering enabled (max %.0fx)\n", g_max_anisotropy);
        } else {
            g_max_anisotropy = 1.0f;
            printf("[D3D9] Anisotropic filtering not available\n");
        }

        // Determine backbuffer resolution: use app's request, fall back to canvas size
        if (pPresentationParameters &&
            pPresentationParameters->BackBufferWidth > 0 &&
            pPresentationParameters->BackBufferHeight > 0) {
            g_logical_width = pPresentationParameters->BackBufferWidth;
            g_logical_height = pPresentationParameters->BackBufferHeight;
        } else {
            int w, h;
            emscripten_get_canvas_element_size(g_canvas_selector, &w, &h);
            g_logical_width = w;
            g_logical_height = h;
        }

        // Set canvas drawingBuffer to match backbuffer size (D3D9-compliant: fixed at CreateDevice)
        emscripten_set_canvas_element_size(g_canvas_selector, g_logical_width, g_logical_height);

        // Everything the wrapper remembers about the GL objects and state belongs to the context before: forget it
        g_gl_generation++;
        g_shader_program = 0;
        g_programs.clear();
        g_vertex_shader_object = 0;
        g_current_ps_id = 0;
        g_fbo = 0;
        g_up_vbo = g_up_ibo = 0;
        g_up_vbo_size = g_up_ibo_size = 0;
        memset(g_texture_stages, 0, sizeof(g_texture_stages));
        g_cached_texture = 0xFFFFFFFF;
        g_last_bound_texture_id = 0xFFFFFFFF;
        g_last_bound_lightmap_id = 0xFFFFFFFF;
        g_mipmapped_textures.clear();
        g_fbo_depth_pool.clear();
        g_current_fvf = 0;
        memset(g_cached_viewport, 0, sizeof(g_cached_viewport));
        memset(g_cached_scissor_box, 0, sizeof(g_cached_scissor_box));
        g_cached_scissor_test = GL_FALSE;

        *ppReturnedDeviceInterface = new D3D9Device();
        printf("[D3D9] build of the Direct3D wrapper: " __DATE__ " " __TIME__ "\n");
        printf("[D3D9] device created: context %d (the one before: %d), canvas %dx%d, shader program %u\n", (int) g_webgl_context, (int) previous_context,
               g_logical_width, g_logical_height, (unsigned int) g_shader_program);
        if (previous_context > 0 && previous_context != g_webgl_context) emscripten_webgl_destroy_context(previous_context);

        g_viewport.Width = g_logical_width;
        g_viewport.Height = g_logical_height;
        glViewport(0, 0, g_logical_width, g_logical_height);

        // the multisampled back buffer (the objects of the context before went with it)
        g_back_fbo = g_back_color_rb = g_back_depth_rb = 0;
        g_alpha_to_coverage_on = false;
        g_wanted_samples = pPresentationParameters ? (int) pPresentationParameters->MultiSampleType : 0;
        back_buffer_setup(g_wanted_samples, g_logical_width, g_logical_height);
        glBindFramebuffer(GL_FRAMEBUFFER, g_back_fbo);

        return D3D_OK;
    }

    // Extended overload: specify which canvas element to render to.
    // Falls back to the standard CreateDevice with g_canvas_selector set.
    HRESULT CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
                         HWND hFocusWindow, DWORD BehaviorFlags,
                         D3DPRESENT_PARAMETERS* pPresentationParameters,
                         IDirect3DDevice9** ppReturnedDeviceInterface,
                         const char* canvasSelector) {
        if (canvasSelector && canvasSelector[0]) {
            g_canvas_selector = canvasSelector;
        }
        return CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                            pPresentationParameters, ppReturnedDeviceInterface);
    }

    virtual ULONG AddRef() override {
        return 1; // Stub: no ref counting for D3D9 interface
    }

    virtual ULONG Release() override {
        delete this;
        return 0;
    }
    
    virtual UINT GetAdapterCount() override {
        return 1; // Single adapter for WebGL
    }

    virtual HRESULT GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override {
        if (!pIdentifier) return D3DERR_INVALIDCALL;
        memset(pIdentifier, 0, sizeof(D3DADAPTER_IDENTIFIER9));
        strncpy(pIdentifier->Driver, "emscripten_webgl", MAX_DEVICE_IDENTIFIER_STRING);
        strncpy(pIdentifier->Description, "Emscripten WebGL Wrapper", MAX_DEVICE_IDENTIFIER_STRING);
        return D3D_OK;
    }



    virtual UINT GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override {
        return 1;
    }

    virtual HRESULT EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override {
        if (!pMode) return D3DERR_INVALIDCALL;
        if (Mode >= 1) return D3DERR_INVALIDCALL;

        int w, h;
        emscripten_get_canvas_element_size(g_canvas_selector, &w, &h);
        pMode->Width = w;
        pMode->Height = h;
        pMode->RefreshRate = 60;
        pMode->Format = Format;
        return D3D_OK;
    }

    virtual HRESULT GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override {
        if (!pMode) return D3DERR_INVALIDCALL;
        int w, h;
        emscripten_get_canvas_element_size(g_canvas_selector, &w, &h);
        pMode->Width = w;
        pMode->Height = h;
        pMode->RefreshRate = 60;
        pMode->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }

    virtual HRESULT CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        return D3D_OK;
    }

    virtual HRESULT CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override {
        return D3D_OK;
    }


    virtual HRESULT GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override {
        // Shared implementation "lie"
        if (!pCaps) return -1;
        memset(pCaps, 0, sizeof(D3DCAPS9));
#ifdef __EMSCRIPTEN__
        pCaps->VertexShaderVersion = 0;
        pCaps->PixelShaderVersion = 0;
#else
        pCaps->VertexShaderVersion = D3DVS_VERSION(2, 0);
        pCaps->PixelShaderVersion = D3DPS_VERSION(2, 0);
#endif
        pCaps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT;
        pCaps->MaxPrimitiveCount = 0xFFFFFFFF;
        pCaps->NumSimultaneousRTs = 1;
        pCaps->MaxStreams = 16;
        pCaps->MaxUserClipPlanes = 1; // Emulated via fragment shader discard
        pCaps->RasterCaps = D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZTEST;
        pCaps->MaxVertexShaderConst = 0;
        return D3D_OK;
    }
};

// Helper: Load texture from file
// Note: In a real D3DX scenario, this would be D3DXCreateTextureFromFile
IDirect3DTexture9* LoadTextureFromFile(IDirect3DDevice9* device, const char* filename) {
    int w, h, channels;
    unsigned char* data = stbi_load(filename, &w, &h, &channels, 4); // Force 4 channels (RGBA)
    if (!data) {
        printf("Failed to load image: %s\n", filename);
        return nullptr;
    }
    
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
        stbi_image_free(data);
        printf("Failed to create texture for %s\n", filename);
        return nullptr;
    }
    
    D3DLOCKED_RECT rect;
    if (SUCCEEDED(texture->LockRect(0, &rect, nullptr, 0))) {
        // Data from stbi_load is RGBA. 
        // Our D3D9Texture::UnlockRect expects BGRA (ARGB in D3D terms) which it expects to SWIZZLE to RGBA.
        // ... (as before) ...
        uint32_t* src_pixels = (uint32_t*)data;
        uint32_t* dst_pixels = (uint32_t*)rect.pBits;
        int count = w * h;
        
        for (int i=0; i<count; i++) {
            uint32_t color = src_pixels[i];
            uint8_t* p = (uint8_t*)&color;
            uint8_t r = p[0];
            uint8_t g = p[1];
            uint8_t b = p[2];
            uint8_t a = p[3];
            
            
            // Write B G R A (D3D ARGB Little Endian: 0xAARRGGBB)
            uint8_t* d = (uint8_t*)&dst_pixels[i];
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = a;
        }
        
        texture->UnlockRect(0);
    }
    
    stbi_image_free(data);
    return texture;
}

// Entry point
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    return new D3D9();
}

extern "C" void D3D9_RegisterTextureName(IDirect3DTexture9*, const char*) {}

// Accessor functions for external modules (e.g. text rendering bridge)
extern "C" {
    void D3D9_GetTextureInfo(IDirect3DTexture9* pTex, unsigned int* gl_id, unsigned int* w, unsigned int* h) {
        if (!pTex) return;
        D3D9Texture* tex = (D3D9Texture*)pTex;
        if (gl_id) *gl_id = tex->m_texture;
        if (w) *w = tex->m_width;
        if (h) *h = tex->m_height;
    }

    // IDirect3DDevice9::CreateRenderTarget (not in the interface of the library): a lockable render target of its own
    HRESULT D3D9_CreateRenderTarget(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, IDirect3DSurface9** surface) {
        return static_cast<D3D9Device*>(device)->CreateRenderTargetSurface(width, height, format, surface);
    }

    void D3D9_InvalidateTextureCache() {
        g_last_bound_texture_id = 0xFFFFFFFF;
        g_last_bound_lightmap_id = 0xFFFFFFFF;
        g_cached_texture = 0xFFFFFFFF;
    }
}



