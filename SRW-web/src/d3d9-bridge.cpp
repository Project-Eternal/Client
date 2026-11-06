// Direct3D 9 for the recompiled client: the program's calls are forwarded to d3d9-webgl (d3d9-webgl/), which draws with WebGL.
//
// The program uses the interfaces through their vtables in the order Windows has them; d3d9-webgl has classes with virtual
// methods in an order of its own, and its own reduced structures. So the program gets small "guest" objects: a pointer to a
// vtable made of procedures (SRW/d3d9.def, generated glue) and a pointer to the object of d3d9-webgl behind it. Every method
// that is not implemented here answers D3DERR_NOTAVAILABLE (SRW_D3DStub_c logs it), which is what a driver without the feature
// does.
#include <emscripten.h>
#include <emscripten/heap.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "d3d9-webgl/d3d9.h"

#ifndef CCALL
#define CCALL
#endif

#include "srw.h"
#include "srw_debuglog.h"

extern "C" {
extern unsigned long long g_srw_gl_stats[8];
void SRW_MemoryStats(int64_t *out);
void SRW_CursorImage(const uint32_t *rgba, int width, int height, int hot_x, int hot_y);
int SRW_CursorD3DShow(int show);
int SRW_WindowClientSize(uint32_t hwnd, int *width, int *height);
void warnprint(const char *text);
void SRW_BlockedTake(double *out);
void SRW_NotePresent(void);
void SRW_RemoteNoteGameThread(void);
uint32_t SRW_LastCaller(void);
void SRW_NoteDevice(int windowed, int width, int height);
HRESULT D3D9_CreateRenderTarget(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, IDirect3DSurface9 **surface);
void SRW_DisplaySizes(int32_t *canvas_width, int32_t *canvas_height, int32_t *screen_width, int32_t *screen_height);
uint32_t SRW_FindProc(const char *name);
IDirect3D9 *WebGLDirect3DCreate9(UINT SDKVersion);
}

#define OK 0
#define E_NOINTERFACE_ ((uint32_t) 0x80004002u)
#define E_FAIL_ ((uint32_t) 0x80004005u)
#define D3DERR_NOTAVAILABLE_ ((uint32_t) 0x8876086au)
#define D3DERR_INVALIDCALL_ ((uint32_t) 0x8876086cu)

namespace {

enum Kind {
    K_D3D, K_DEVICE, K_TEXTURE, K_SURFACE, K_VERTEXBUFFER, K_INDEXBUFFER, K_VERTEXDECL, K_VERTEXSHADER, K_PIXELSHADER,
    K_STATEBLOCK, K_QUERY, K_KINDS
};

// what the program holds: the vtable pointer comes first, as in a COM object
struct Guest {
    uint32_t vtable;
    Kind kind;
    int references;
    void *host;
    Guest *device;          // the device that made the object
    uint32_t creation[4];   // of a device: adapter, type, window, flags
    Guest *direct3d;        // of a device: the object that created it
};

#define D3D_VTABLE(interface, ...) const char *const interface##_names[] = {__VA_ARGS__};
#include "d3d9-methods.inc"
#undef D3D_VTABLE

uint32_t vtables[K_KINDS][160];
bool vtables_ready;

void build(Kind kind, const char *const *names, unsigned count)
{
    for (unsigned i = 0; i < count; i++) vtables[kind][i] = SRW_FindProc(names[i]);
}

void build_vtables()
{
    if (vtables_ready) return;
    vtables_ready = true;
    build(K_D3D, IDirect3D9_names, sizeof(IDirect3D9_names) / sizeof(*IDirect3D9_names));
    build(K_DEVICE, IDirect3DDevice9_names, sizeof(IDirect3DDevice9_names) / sizeof(*IDirect3DDevice9_names));
    build(K_TEXTURE, IDirect3DTexture9_names, sizeof(IDirect3DTexture9_names) / sizeof(*IDirect3DTexture9_names));
    build(K_SURFACE, IDirect3DSurface9_names, sizeof(IDirect3DSurface9_names) / sizeof(*IDirect3DSurface9_names));
    build(K_VERTEXBUFFER, IDirect3DVertexBuffer9_names, sizeof(IDirect3DVertexBuffer9_names) / sizeof(*IDirect3DVertexBuffer9_names));
    build(K_INDEXBUFFER, IDirect3DIndexBuffer9_names, sizeof(IDirect3DIndexBuffer9_names) / sizeof(*IDirect3DIndexBuffer9_names));
    build(K_VERTEXDECL, IDirect3DVertexDeclaration9_names, sizeof(IDirect3DVertexDeclaration9_names) / sizeof(*IDirect3DVertexDeclaration9_names));
    build(K_VERTEXSHADER, IDirect3DVertexShader9_names, sizeof(IDirect3DVertexShader9_names) / sizeof(*IDirect3DVertexShader9_names));
    build(K_PIXELSHADER, IDirect3DPixelShader9_names, sizeof(IDirect3DPixelShader9_names) / sizeof(*IDirect3DPixelShader9_names));
    build(K_STATEBLOCK, IDirect3DStateBlock9_names, sizeof(IDirect3DStateBlock9_names) / sizeof(*IDirect3DStateBlock9_names));
    build(K_QUERY, IDirect3DQuery9_names, sizeof(IDirect3DQuery9_names) / sizeof(*IDirect3DQuery9_names));
}

Guest *make(Kind kind, void *host, Guest *device)
{
    Guest *g = (Guest *) calloc(1, sizeof(Guest));

    build_vtables();
    g->vtable = (uint32_t) (uintptr_t) vtables[kind];
    g->kind = kind;
    g->references = 1;
    g->host = host;
    g->device = device;
    return g;
}

inline Guest *G(uint32_t address)
{
    return (Guest *) (uintptr_t) address;
}

template <class T> inline T *host(uint32_t address)
{
    return address != 0 ? (T *) G(address)->host : nullptr;
}

template <class T> inline T *pointer(uint32_t address)
{
    return (T *) (uintptr_t) address;
}

// returns a new object to the program through an out pointer
uint32_t give(uint32_t out, Kind kind, void *host_object, Guest *device)
{
    if (out == 0) return D3DERR_INVALIDCALL_;
    if (host_object == nullptr)
    {
        *pointer<uint32_t>(out) = 0;
        return D3DERR_NOTAVAILABLE_;
    }
    *pointer<uint32_t>(out) = (uint32_t) (uintptr_t) make(kind, host_object, device);
    return OK;
}

// the capabilities of the device in the layout of D3DCAPS9 (304 bytes); the numbers describe a fixed-function GPU with
// what d3d9-webgl can do
void fill_caps(uint8_t *caps)
{
    uint32_t *c = (uint32_t *) caps;
    float *f = (float *) caps;

    memset(caps, 0, 304);
    c[0] = 1;                       // DeviceType HAL
    c[2] = 0x20000;                 // Caps: READ_SCANLINE
    c[3] = 0x20000 | 0x10000000 | 0x20000000;  // Caps2: FULLSCREENGAMMA, CANMANAGERESOURCE, DYNAMICTEXTURES
    c[4] = 0x20 | 0x80 | 0x100;     // Caps3
    c[5] = 0x80000001u;             // PresentationIntervals: IMMEDIATE | ONE
    c[6] = 1;                       // CursorCaps: COLOR
    c[7] = 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x2000 | 0x8000 | 0x10000 | 0x80000;  // DevCaps
    c[8] = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x20000;  // PrimitiveMiscCaps
    c[9] = 0x10 | 0x80 | 0x100 | 0x2000 | 0x20000 | 0x1000000 | 0x2000000 | 0x4000000;  // RasterCaps
    c[10] = 0xff;                   // ZCmpCaps
    c[11] = 0x3fff;                 // SrcBlendCaps
    c[12] = 0x3fff;                 // DestBlendCaps
    c[13] = 0xff;                   // AlphaCmpCaps
    c[14] = 0x84208;                // ShadeCaps
    c[15] = 0x1 | 0x4 | 0x80 | 0x400 | 0x4000;  // TextureCaps: PERSPECTIVE, ALPHA, TEXREPEATNOTSCALEDBYSIZE (0x80), PROJECTED, MIPMAP
    c[16] = 0x07030700;             // TextureFilterCaps
    c[19] = 0x3f;                   // TextureAddressCaps
    c[22] = 4096;                   // MaxTextureWidth
    c[23] = 4096;                   // MaxTextureHeight
    c[24] = 256;                    // MaxVolumeExtent
    c[25] = 8192;                   // MaxTextureRepeat
    c[26] = 4096;                   // MaxTextureAspectRatio
    c[27] = 16;                     // MaxAnisotropy
    f[28] = 1e10f;                  // MaxVertexW
    f[29] = -32768.0f;              // GuardBandLeft
    f[30] = -32768.0f;              // GuardBandTop
    f[31] = 32768.0f;               // GuardBandRight
    f[32] = 32768.0f;               // GuardBandBottom
    c[34] = 0xff;                   // StencilCaps
    c[35] = 8;                      // FVFCaps: 8 texture coordinate sets
    c[36] = 0x3ffffff & ~(0x200000 | 0x400000);  // TextureOpCaps: every D3DTOP the stage cascade of the wrapper does (all but the bump mapping ones)
    c[37] = 8;                      // MaxTextureBlendStages
    c[38] = 8;                      // MaxSimultaneousTextures
    c[39] = 0x8 | 0x10 | 0x20;      // VertexProcessingCaps
    c[40] = 8;                      // MaxActiveLights
    c[41] = 6;                      // MaxUserClipPlanes
    c[42] = 4;                      // MaxVertexBlendMatrices
    f[44] = 64.0f;                  // MaxPointSize
    c[45] = 0xfffff;                // MaxPrimitiveCount
    c[46] = 0xfffff;                // MaxVertexIndex
    c[47] = 16;                     // MaxStreams
    c[48] = 255;                    // MaxStreamStride
    c[49] = 0;                      // VertexShaderVersion: none, the program takes the fixed-function path
    {
        // pixel shaders: the shaders of the program are translated to GLSL (d3d9-webgl/ps_shaders.inc); the setting d3dShaders (0 off) is read once
        static int shaders = -1;

        if (shaders < 0)
        {
            FILE *f = fopen("/Port/d3dshaders.txt", "r");
            int value = 1;

            if (f != nullptr)
            {
                if (fscanf(f, "%d", &value) != 1) value = 1;
                fclose(f);
            }
            shaders = value != 0;
        }
        if (shaders)
        {
            c[51] = 0xFFFF0200;     // PixelShaderVersion: 2.0 (the program picks the ps_2_0 variants of its shaders)
            f[52] = 8.0f;           // PixelShader1xMaxValue
            c[66] = 0x1 | 0x10;     // PS20Caps.Caps: ARBITRARYSWIZZLE, NOTEXINSTRUCTIONLIMIT
            c[68] = 12;             // PS20Caps.NumTemps
            c[70] = 96;             // PS20Caps.NumInstructionSlots
            c[73] = 0xFFFF;         // MaxPShaderInstructionsExecuted
        }
    }
    c[60] = 1;                      // NumSimultaneousRTs
    c[61] = 0x3000300;              // StretchRectFilterCaps
}

// how often the program calls what, reported every few seconds (a black screen is either "nothing is drawn" or "drawn wrongly")
enum Counter { C_PRESENT, C_CLEAR, C_BEGINSCENE, C_DRAW, C_DRAWINDEXED, C_DRAWUP, C_DRAWINDEXEDUP, C_SETTEXTURE, C_SETSTREAM, C_SETDECL, C_SETFVF, C_SETSTATE, C_COUNTERS };
const char *const counter_names[C_COUNTERS] = { "Present", "Clear", "BeginScene", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
                                                "DrawIndexedPrimitiveUP", "SetTexture", "SetStreamSource", "SetVertexDeclaration", "SetFVF", "SetRenderState" };
unsigned counts[C_COUNTERS];
double times[C_COUNTERS];   // milliseconds spent inside the calls
unsigned reports;
double frame_ms_total;
unsigned frame_count;

// counts a call and the time it takes (until the end of the function that holds it)
struct Timed {
    Counter which;
    double start;
    static int category(Counter c) { return c == C_PRESENT ? SRW_PROF_PRESENT : (c >= C_DRAW && c <= C_DRAWINDEXEDUP) ? SRW_PROF_DRAW : SRW_PROF_D3D; }
#ifdef SRW_DIAGNOSTICS
    explicit Timed(Counter c) : which(c), start(emscripten_get_now()) { counts[c]++; SRW_PROF_ENTER(category(c)); }
    ~Timed() { times[which] += emscripten_get_now() - start; SRW_PROF_LEAVE(); }
#else
    explicit Timed(Counter c) : which(c), start(0) { SRW_PROF_ENTER(category(c)); }
    ~Timed() { SRW_PROF_LEAVE(); }
#endif
};

[[maybe_unused]] void report_counts(uint64_t now_ms)
{
    static uint64_t last_ms;
    static double last_present;
    char line[800];
    size_t n;
    int i;
    double now = (double) now_ms, inside = 0;

    if (last_present != 0)
    {
        frame_ms_total += now - last_present;
        frame_count++;
    }
    last_present = now;
    if (last_ms == 0) last_ms = now_ms;
    if (now_ms - last_ms < 5000) return;
    last_ms = now_ms;
    reports++;
    n = (size_t) snprintf(line, sizeof(line), "Direct3D 9 calls in the last seconds (count, ms inside):");
    for (i = 0; i < C_COUNTERS && n < sizeof(line) - 60; i++)
    {
        if (counts[i] != 0) n += (size_t) snprintf(line + n, sizeof(line) - n, " %s %u (%.0f),", counter_names[i], counts[i], times[i]);
        inside += times[i];
        counts[i] = 0;
        times[i] = 0;
    }
    double blocked[3];
    SRW_BlockedTake(blocked);
    {
        static unsigned long long before[8];
        int64_t guest[6];
        struct mallinfo info = mallinfo();
        char memory[600];

        SRW_MemoryStats(guest);
        snprintf(memory, sizeof(memory),
                 "Memory: heap of the module %.1f MB (in use %.1f MB), guest heap functions %.1f MB in %lld blocks (%lld calls), VirtualAlloc %.1f MB in %lld blocks (%lld calls); "
                 "to WebGL in this interval: %.1f MB to buffers, %.1f MB to textures, buffers +%llu -%llu, textures +%llu -%llu, render/frame buffers +%llu -%llu",
                 emscripten_get_heap_size() / 1048576.0, info.uordblks / 1048576.0, guest[0] / 1048576.0, (long long) guest[1], (long long) guest[4],
                 guest[2] / 1048576.0, (long long) guest[3], (long long) guest[5], (g_srw_gl_stats[0] - before[0]) / 1048576.0,
                 (g_srw_gl_stats[1] - before[1]) / 1048576.0, g_srw_gl_stats[2] - before[2], g_srw_gl_stats[3] - before[3],
                 g_srw_gl_stats[4] - before[4], g_srw_gl_stats[5] - before[5], g_srw_gl_stats[6] - before[6], g_srw_gl_stats[7] - before[7]);
        memcpy(before, g_srw_gl_stats, sizeof(before));
        if (SRW_LOG_ON(SRW_LOG_MEMORY)) logprint(memory);
    }
    n += (size_t) snprintf(line + n, sizeof(line) - n, " all calls %.0f ms, %.1f ms per frame; this thread slept %.0f ms, waited for objects %.0f ms, for the big lock %.0f ms",
                           inside, frame_count ? frame_ms_total / frame_count : 0.0, blocked[0], blocked[1], blocked[2]);
    frame_ms_total = 0;
    frame_count = 0;
    if (SRW_LOG_ON(SRW_LOG_PERF)) logprint(line);
}

}  // namespace

extern "C" {

uint32_t CCALL SRW_D3DStub_c(uint32_t id);

// ------------------------------------------------------------------------------------------------------------------
// IUnknown

uint32_t CCALL IUnknown_QueryInterface_c(uint32_t self, uint32_t riid, uint32_t out)
{
    (void) self;
    (void) riid;
    if (out != 0) *pointer<uint32_t>(out) = 0;
    return E_NOINTERFACE_;
}

uint32_t CCALL IUnknown_AddRef_c(uint32_t self)
{
    return (uint32_t) __atomic_add_fetch(&G(self)->references, 1, __ATOMIC_SEQ_CST);
}

uint32_t CCALL IUnknown_Release_c(uint32_t self)
{
    Guest *g = G(self);
    int remaining = __atomic_sub_fetch(&g->references, 1, __ATOMIC_SEQ_CST);

    if (remaining == 0)
    {
        if (g->host != nullptr && g->kind != K_DEVICE) ((IUnknown *) g->host)->Release();
        g->vtable = 0;
        free(g);
    }
    return (uint32_t) remaining;
}

// ------------------------------------------------------------------------------------------------------------------
// IDirect3D9

uint32_t CCALL IDirect3D9_GetAdapterCount_c(uint32_t self)
{
    return host<IDirect3D9>(self)->GetAdapterCount();
}

uint32_t CCALL IDirect3D9_GetAdapterIdentifier_c(uint32_t self, uint32_t adapter, uint32_t flags, uint32_t identifier)
{
    return (uint32_t) host<IDirect3D9>(self)->GetAdapterIdentifier(adapter, flags, pointer<D3DADAPTER_IDENTIFIER9>(identifier));
}

/* The display modes: the canvas (the current mode, the page's viewport) and the screen (the page in full screen), in CSS pixels, the
   smaller first. (The program lists them as resolutions only with its widescreen setting; otherwise it has a fixed list.) */
static int display_modes(D3DDISPLAYMODE *modes)
{
    int32_t canvas_width, canvas_height, screen_width, screen_height;
    int count = 0;

    SRW_DisplaySizes(&canvas_width, &canvas_height, &screen_width, &screen_height);
    modes[count].Width = (UINT) canvas_width;
    modes[count++].Height = (UINT) canvas_height;
    if (screen_width != canvas_width || screen_height != canvas_height)
    {
        modes[count].Width = (UINT) screen_width;
        modes[count++].Height = (UINT) screen_height;
        if ((uint64_t) screen_width * (uint64_t) screen_height < (uint64_t) canvas_width * (uint64_t) canvas_height)
        {
            D3DDISPLAYMODE swap = modes[0];

            modes[0] = modes[1];
            modes[1] = swap;
        }
    }
    for (int i = 0; i < count; i++)
    {
        modes[i].RefreshRate = 60;
        modes[i].Format = D3DFMT_X8R8G8B8;
    }
    return count;
}

uint32_t CCALL IDirect3D9_GetAdapterModeCount_c(uint32_t self, uint32_t adapter, uint32_t format)
{
    D3DDISPLAYMODE modes[2];

    (void) self;
    (void) format;
    return adapter == 0 ? (uint32_t) display_modes(modes) : 0;
}

uint32_t CCALL IDirect3D9_EnumAdapterModes_c(uint32_t self, uint32_t adapter, uint32_t format, uint32_t mode, uint32_t out)
{
    D3DDISPLAYMODE modes[2];
    int count = display_modes(modes);

    (void) self;
    if (adapter != 0 || out == 0 || mode >= (uint32_t) count) return (uint32_t) D3DERR_INVALIDCALL;
    modes[mode].Format = (D3DFORMAT) format;
    *pointer<D3DDISPLAYMODE>(out) = modes[mode];
    return D3D_OK;
}

uint32_t CCALL IDirect3D9_GetAdapterDisplayMode_c(uint32_t self, uint32_t adapter, uint32_t out)
{
    int32_t canvas_width, canvas_height, screen_width, screen_height;
    D3DDISPLAYMODE *mode = pointer<D3DDISPLAYMODE>(out);

    (void) self;
    if (adapter != 0 || out == 0) return (uint32_t) D3DERR_INVALIDCALL;
    SRW_DisplaySizes(&canvas_width, &canvas_height, &screen_width, &screen_height);
    mode->Width = (UINT) canvas_width;
    mode->Height = (UINT) canvas_height;
    mode->RefreshRate = 60;
    mode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

uint32_t CCALL IDirect3D9_CheckDeviceType_c(uint32_t self, uint32_t adapter, uint32_t type, uint32_t adapter_format, uint32_t back_format, uint32_t windowed)
{
    (void) self; (void) adapter; (void) type; (void) adapter_format; (void) back_format; (void) windowed;
    return OK;
}

uint32_t CCALL IDirect3D9_CheckDeviceFormat_c(uint32_t self, uint32_t adapter, uint32_t type, uint32_t adapter_format, uint32_t usage, uint32_t rtype, uint32_t check)
{
    return (uint32_t) host<IDirect3D9>(self)->CheckDeviceFormat(adapter, (D3DDEVTYPE) type, (D3DFORMAT) adapter_format, usage, (D3DRESOURCETYPE) rtype, (D3DFORMAT) check);
}

uint32_t CCALL IDirect3D9_CheckDeviceMultiSampleType_c(uint32_t self, uint32_t adapter, uint32_t type, uint32_t format, uint32_t windowed, uint32_t multisample, uint32_t levels)
{
    // 2, 4 and 8 samples (d3d9-webgl: a multisampled back buffer, at most what WebGL allows: usually 4 or 8 on a desktop GPU), one
    // quality level each
    (void) self; (void) adapter; (void) type; (void) format; (void) windowed;
    {
        static int logged;
        if (SRW_LOG_ON(SRW_LOG_D3D) && logged++ < 20)
            logprintf("Direct3D CheckDeviceMultiSampleType: type %u, format %u, windowed %u, %u samples, levels %s", (unsigned int) type, (unsigned int) format,
                      (unsigned int) windowed, (unsigned int) multisample, levels != 0 ? "asked" : "not asked");
    }
    if (levels != 0) *pointer<uint32_t>(levels) = 1;
    return (multisample <= 2 || multisample == 4 || multisample == 8) ? OK : D3DERR_NOTAVAILABLE_;
}

uint32_t CCALL IDirect3D9_CheckDepthStencilMatch_c(uint32_t self, uint32_t adapter, uint32_t type, uint32_t adapter_format, uint32_t target_format, uint32_t depth_format)
{
    return (uint32_t) host<IDirect3D9>(self)->CheckDepthStencilMatch(adapter, (D3DDEVTYPE) type, (D3DFORMAT) adapter_format, (D3DFORMAT) target_format, (D3DFORMAT) depth_format);
}

uint32_t CCALL IDirect3D9_CheckDeviceFormatConversion_c(uint32_t self, uint32_t adapter, uint32_t type, uint32_t source, uint32_t target)
{
    (void) self; (void) adapter; (void) type; (void) source; (void) target;
    return OK;
}

uint32_t CCALL IDirect3D9_GetDeviceCaps_c(uint32_t self, uint32_t adapter, uint32_t type, uint32_t caps)
{
    (void) self; (void) adapter; (void) type;
    if (caps == 0) return D3DERR_INVALIDCALL_;
    fill_caps(pointer<uint8_t>(caps));
    return OK;
}

uint32_t CCALL IDirect3D9_GetAdapterMonitor_c(uint32_t self, uint32_t adapter)
{
    (void) self; (void) adapter;
    return 0x90001;
}

uint32_t CCALL IDirect3D9_CreateDevice_c(uint32_t self, uint32_t adapter, uint32_t type, uint32_t window, uint32_t flags, uint32_t parameters, uint32_t out)
{
    IDirect3DDevice9 *device = nullptr;
    D3DPRESENT_PARAMETERS *requested = pointer<D3DPRESENT_PARAMETERS>(parameters);
    HRESULT result;

    if (requested != nullptr)
        logprintf("Direct3D CreateDevice: %ux%u, format %u, %u buffers, windowed %d, multisampling %u", (unsigned int) requested->BackBufferWidth, (unsigned int) requested->BackBufferHeight,
                  (unsigned int) requested->BackBufferFormat, (unsigned int) requested->BackBufferCount, (int) requested->Windowed, (unsigned int) requested->MultiSampleType);
    result = host<IDirect3D9>(self)->CreateDevice(adapter, (D3DDEVTYPE) type, (HWND) (uintptr_t) window, flags,
                                                          pointer<D3DPRESENT_PARAMETERS>(parameters), &device);

    if (result != D3D_OK || device == nullptr)
    {
        logprintf("Direct3D 9: CreateDevice failed (%x)", (unsigned int) result);
        if (out != 0) *pointer<uint32_t>(out) = 0;
        return D3DERR_NOTAVAILABLE_;
    }
    Guest *guest = make(K_DEVICE, device, nullptr);

    guest->creation[0] = adapter;
    guest->creation[1] = type;
    guest->creation[2] = window;
    guest->creation[3] = flags;
    guest->direct3d = G(self);
    __atomic_add_fetch(&G(self)->references, 1, __ATOMIC_SEQ_CST);
    *pointer<uint32_t>(out) = (uint32_t) (uintptr_t) guest;
    if (requested != nullptr) SRW_NoteDevice(requested->Windowed != 0, (int) requested->BackBufferWidth, (int) requested->BackBufferHeight);
    return OK;
}

// ------------------------------------------------------------------------------------------------------------------
// IDirect3DDevice9

#define DEVICE(self) host<IDirect3DDevice9>(self)

uint32_t CCALL IDirect3DDevice9_TestCooperativeLevel_c(uint32_t self)
{
    return (uint32_t) DEVICE(self)->TestCooperativeLevel();
}

uint32_t CCALL IDirect3DDevice9_GetAvailableTextureMem_c(uint32_t self)
{
    return DEVICE(self)->GetAvailableTextureMem();
}

uint32_t CCALL IDirect3DDevice9_EvictManagedResources_c(uint32_t self)
{
    (void) self;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_GetDirect3D_c(uint32_t self, uint32_t out)
{
    Guest *d3d = G(self)->direct3d;

    if (out == 0 || d3d == nullptr) return D3DERR_INVALIDCALL_;
    __atomic_add_fetch(&d3d->references, 1, __ATOMIC_SEQ_CST);
    *pointer<uint32_t>(out) = (uint32_t) (uintptr_t) d3d;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_GetDeviceCaps_c(uint32_t self, uint32_t caps)
{
    (void) self;
    if (caps == 0) return D3DERR_INVALIDCALL_;
    fill_caps(pointer<uint8_t>(caps));
    return OK;
}

uint32_t CCALL IDirect3DDevice9_GetDisplayMode_c(uint32_t self, uint32_t swap_chain, uint32_t mode)
{
    return (uint32_t) DEVICE(self)->GetDisplayMode(swap_chain, pointer<D3DDISPLAYMODE>(mode));
}

uint32_t CCALL IDirect3DDevice9_GetCreationParameters_c(uint32_t self, uint32_t parameters)
{
    if (parameters == 0) return D3DERR_INVALIDCALL_;
    memcpy(pointer<uint32_t>(parameters), G(self)->creation, sizeof(G(self)->creation));
    return OK;
}

// the pointer of the program: its bitmap (a texture level of 32 x 32 pixels) becomes the CSS cursor of the canvas (cursor.c)
extern "C++" void srw_convert_region_to_rgba(D3DFORMAT format, const uint8_t* base, UINT pitch, UINT w, UINT h, uint32_t* out);

uint32_t CCALL IDirect3DDevice9_SetCursorProperties_c(uint32_t self, uint32_t x, uint32_t y, uint32_t bitmap)
{
    IDirect3DSurface9 *surface = bitmap != 0 ? host<IDirect3DSurface9>(bitmap) : nullptr;
    D3DSURFACE_DESC description;
    D3DLOCKED_RECT locked;

    (void) self;
    if (surface == nullptr) return D3DERR_INVALIDCALL_;
    if (surface->GetDesc(&description) == D3D_OK && description.Width <= 128 && description.Height <= 128 && surface->LockRect(&locked, nullptr, 0x10) == D3D_OK)
    {
        std::vector<uint32_t> rgba((size_t) description.Width * description.Height);

        srw_convert_region_to_rgba(description.Format, static_cast<const uint8_t *>(locked.pBits), (UINT) locked.Pitch, description.Width, description.Height, rgba.data());
        surface->UnlockRect();
        if (SRW_LOG_ON(SRW_LOG_INPUT))
        {
            uint32_t sum = 0;
            size_t opaque = 0;

            for (uint32_t pixel : rgba) { sum = sum * 31u + pixel; opaque += (pixel >> 24) != 0; }
            logprintf("SetCursorProperties: surface %p %ux%u format %u pool %u usage %x, %u non-transparent pixels of %u, checksum %08x, hot spot %u,%u, from %x", (void *) surface,
                      (unsigned int) description.Width, (unsigned int) description.Height, (unsigned int) description.Format, (unsigned int) description.Pool, (unsigned int) description.Usage,
                      (unsigned int) opaque, (unsigned int) rgba.size(), (unsigned int) sum, (unsigned int) x, (unsigned int) y, (unsigned int) SRW_LastCaller());
        }
        SRW_CursorImage(rgba.data(), (int) description.Width, (int) description.Height, (int) x, (int) y);
    }
    else if (SRW_LOG_ON(SRW_LOG_INPUT)) logprintf("SetCursorProperties: the surface %p (%ux%u format %u) could not be read", (void *) surface, (unsigned int) description.Width, (unsigned int) description.Height, (unsigned int) description.Format);
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetCursorPosition_c(uint32_t self, uint32_t x, uint32_t y, uint32_t flags)
{
    (void) self; (void) x; (void) y; (void) flags;
    return 0;   // the pointer of the page is where the user has it
}

uint32_t CCALL IDirect3DDevice9_ShowCursor_c(uint32_t self, uint32_t show)
{
    (void) self;
    return (uint32_t) SRW_CursorD3DShow(show != 0);
}

uint32_t CCALL IDirect3DDevice9_GetNumberOfSwapChains_c(uint32_t self)
{
    (void) self;
    return 1;
}

#ifdef SRW_DIAGNOSTICS
extern "C" volatile double SRW_ResizeStarted;   // (user32.c)
static double g_reset_done;   // when the last Reset ended (0: no frames to time)
static int g_reset_frames;
#endif

uint32_t CCALL IDirect3DDevice9_Reset_c(uint32_t self, uint32_t parameters)
{
    D3DPRESENT_PARAMETERS *p = pointer<D3DPRESENT_PARAMETERS>(parameters);
    static int reports;

    if (p != nullptr)
    {
        int width = 0, height = 0;

        // 0 x 0 asks for the size of the window (windowed mode)
        if ((p->BackBufferWidth == 0 || p->BackBufferHeight == 0) && SRW_WindowClientSize((uint32_t) (uintptr_t) p->hDeviceWindow, &width, &height))
        {
            p->BackBufferWidth = (UINT) width;
            p->BackBufferHeight = (UINT) height;
        }
        if (reports++ < 20)
            logprintf("Direct3D Reset: %ux%u, format %u, %u buffers, windowed %d, window %x", (unsigned int) p->BackBufferWidth, (unsigned int) p->BackBufferHeight,
                      (unsigned int) p->BackBufferFormat, (unsigned int) p->BackBufferCount, (int) p->Windowed, (unsigned int) (uintptr_t) p->hDeviceWindow);
    }
#ifdef SRW_DIAGNOSTICS
    {
        // the cost of a resize: from the WM_SIZE to this Reset, the Reset itself, then the frames until they are short again (Present)
        double began = emscripten_get_now();
        uint32_t result = (uint32_t) DEVICE(self)->Reset(p);
        double done = emscripten_get_now();

        g_reset_done = done;
        g_reset_frames = 0;
        logprintf("Resize timing: Reset %.0f ms after WM_SIZE, the Reset itself %.1f ms", SRW_ResizeStarted > 0 ? began - SRW_ResizeStarted : -1.0, done - began);
        return result;
    }
#else
    return (uint32_t) DEVICE(self)->Reset(p);
#endif
}

uint32_t CCALL IDirect3DDevice9_Present_c(uint32_t self, uint32_t source, uint32_t dest, uint32_t window, uint32_t dirty)
{
    if (SRW_ProfOn) SRW_ProfFrame();   // the frame that was running is over
    Timed timed_(C_PRESENT);
    SRW_RemoteNoteGameThread();
    SRW_SafePoint(2);
#ifdef SRW_DIAGNOSTICS
    report_counts((uint64_t) emscripten_get_now());
    SRW_NotePresent();
    if (g_reset_done > 0) {
        // the frames after a Reset: each one's length, until 8 frames or one second of short ones
        static double last;
        double now = emscripten_get_now();

        if (g_reset_frames == 0) last = g_reset_done;
        logprintf("Resize timing: frame %d after the Reset: %.0f ms (%.0f ms since the WM_SIZE)", g_reset_frames + 1, now - last,
                  SRW_ResizeStarted > 0 ? now - SRW_ResizeStarted : -1.0);
        last = now;
        if (++g_reset_frames >= 8) g_reset_done = 0;
    }
#endif
    return (uint32_t) DEVICE(self)->Present(pointer<void>(source), pointer<void>(dest), (HWND) (uintptr_t) window, pointer<void>(dirty));
}

uint32_t CCALL IDirect3DDevice9_GetBackBuffer_c(uint32_t self, uint32_t swap_chain, uint32_t index, uint32_t type, uint32_t out)
{
    IDirect3DSurface9 *surface = nullptr;

    (void) swap_chain; (void) index; (void) type;
    if (DEVICE(self)->GetRenderTarget(0, &surface) != D3D_OK) return D3DERR_NOTAVAILABLE_;
    return give(out, K_SURFACE, surface, G(self));
}

uint32_t CCALL IDirect3DDevice9_GetRasterStatus_c(uint32_t self, uint32_t swap_chain, uint32_t status)
{
    (void) self; (void) swap_chain;
    if (status != 0) memset(pointer<void>(status), 0, 8);
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetDialogBoxMode_c(uint32_t self, uint32_t enable)
{
    (void) self; (void) enable;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetGammaRamp_c(uint32_t self, uint32_t swap_chain, uint32_t flags, uint32_t ramp)
{
    DEVICE(self)->SetGammaRamp(swap_chain, flags, pointer<D3DGAMMARAMP>(ramp));
    return 0;
}

uint32_t CCALL IDirect3DDevice9_GetGammaRamp_c(uint32_t self, uint32_t swap_chain, uint32_t ramp)
{
    DEVICE(self)->GetGammaRamp(swap_chain, pointer<D3DGAMMARAMP>(ramp));
    return 0;
}

// (Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
uint32_t CCALL IDirect3DDevice9_CreateTexture_c(const uint32_t *args)
{
    IDirect3DTexture9 *texture = nullptr;
    HRESULT result = DEVICE(args[0])->CreateTexture(args[1], args[2], args[3], args[4], (D3DFORMAT) args[5], (D3DPOOL) args[6], &texture, nullptr);

    if (result != D3D_OK) return (uint32_t) result;
    return give(args[7], K_TEXTURE, texture, G(args[0]));
}

uint32_t CCALL IDirect3DDevice9_CreateVertexBuffer_c(uint32_t self, uint32_t length, uint32_t usage, uint32_t fvf, uint32_t pool, uint32_t out, uint32_t shared)
{
    IDirect3DVertexBuffer9 *buffer = nullptr;
    HRESULT result = DEVICE(self)->CreateVertexBuffer(length, usage, fvf, (D3DPOOL) pool, &buffer, nullptr);

    (void) shared;
    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_VERTEXBUFFER, buffer, G(self));
}

uint32_t CCALL IDirect3DDevice9_CreateIndexBuffer_c(uint32_t self, uint32_t length, uint32_t usage, uint32_t format, uint32_t pool, uint32_t out, uint32_t shared)
{
    IDirect3DIndexBuffer9 *buffer = nullptr;
    HRESULT result = DEVICE(self)->CreateIndexBuffer(length, usage, (D3DFORMAT) format, (D3DPOOL) pool, &buffer, nullptr);

    (void) shared;
    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_INDEXBUFFER, buffer, G(self));
}

static void surface_op(const char *what, IDirect3DSurface9 *a, IDirect3DSurface9 *b, const void *rect_a, const void *rect_b, uint32_t extra);

// (Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle): a program's read of the back buffer with
// multisampling (a lockable surface the size of the back buffer, StretchRect into it, LockRect)
uint32_t CCALL IDirect3DDevice9_CreateRenderTarget_c(const uint32_t *args)
{
    IDirect3DSurface9 *surface = nullptr;
    HRESULT result = D3D9_CreateRenderTarget(DEVICE(args[0]), args[1], args[2], (D3DFORMAT) args[3], &surface);

    surface_op("CreateRenderTarget", surface, nullptr, nullptr, nullptr, (uint32_t) result);
    if (result != D3D_OK) return (uint32_t) result;
    return give(args[7], K_SURFACE, surface, G(args[0]));
}

// (Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle)
uint32_t CCALL IDirect3DDevice9_CreateDepthStencilSurface_c(const uint32_t *args)
{
    IDirect3DSurface9 *surface = nullptr;
    HRESULT result = DEVICE(args[0])->CreateDepthStencilSurface(args[1], args[2], (D3DFORMAT) args[3], (D3DMULTISAMPLE_TYPE) args[4], args[5], args[6], &surface, nullptr);

    if (result != D3D_OK) return (uint32_t) result;
    return give(args[7], K_SURFACE, surface, G(args[0]));
}

// the copies, locks and creations of surfaces (category d3d, at most 200 lines; the glow's copy of the whole back buffer every frame left out)
static void surface_op(const char *what, IDirect3DSurface9 *a, IDirect3DSurface9 *b, const void *rect_a, const void *rect_b, uint32_t extra)
{
    static int logged;
    D3DSURFACE_DESC da = {}, db = {};
    char ra[48] = "-", rb[48] = "-";

    if (!SRW_LOG_ON(SRW_LOG_D3D) || logged >= 200) return;
    if (a != nullptr) a->GetDesc(&da);
    if (b != nullptr) b->GetDesc(&db);
    if (strcmp(what, "StretchRect") == 0 && da.Width == db.Width && da.Height == db.Height) return;
    if (strcmp(what, "LockRect") == 0 && da.Pool == D3DPOOL_MANAGED) return;   // (texture uploads)
    if (rect_a != nullptr) { const int32_t *r = (const int32_t *) rect_a; snprintf(ra, sizeof(ra), "%d,%d-%d,%d", r[0], r[1], r[2], r[3]); }
    if (rect_b != nullptr) { const int32_t *r = (const int32_t *) rect_b; snprintf(rb, sizeof(rb), "%d,%d-%d,%d", r[0], r[1], r[2], r[3]); }
    logged++;
    logprintf("Direct3D %s: %ux%u format %u usage %x pool %u ms %u rect %s -> %ux%u format %u usage %x rect %s (%x), from %x", what, (unsigned int) da.Width,
              (unsigned int) da.Height, (unsigned int) da.Format, (unsigned int) da.Usage, (unsigned int) da.Pool, (unsigned int) da.MultiSampleType, ra,
              (unsigned int) db.Width, (unsigned int) db.Height, (unsigned int) db.Format, (unsigned int) db.Usage, rb, (unsigned int) extra, (unsigned int) SRW_LastCaller());
}

uint32_t CCALL IDirect3DDevice9_GetRenderTargetData_c(uint32_t self, uint32_t target, uint32_t dest)
{
    surface_op("GetRenderTargetData", host<IDirect3DSurface9>(target), host<IDirect3DSurface9>(dest), nullptr, nullptr, 0);
    return (uint32_t) DEVICE(self)->GetRenderTargetData(host<IDirect3DSurface9>(target), host<IDirect3DSurface9>(dest));
}

uint32_t CCALL IDirect3DDevice9_GetFrontBufferData_c(uint32_t self, uint32_t swap_chain, uint32_t dest)
{
    return (uint32_t) DEVICE(self)->GetFrontBufferData(swap_chain, host<IDirect3DSurface9>(dest));
}

uint32_t CCALL IDirect3DDevice9_StretchRect_c(uint32_t self, uint32_t source, uint32_t source_rect, uint32_t dest, uint32_t dest_rect, uint32_t filter)
{
    surface_op("StretchRect", host<IDirect3DSurface9>(source), host<IDirect3DSurface9>(dest), pointer<void>(source_rect), pointer<void>(dest_rect), filter);
    return (uint32_t) DEVICE(self)->StretchRect(host<IDirect3DSurface9>(source), pointer<void>(source_rect), host<IDirect3DSurface9>(dest), pointer<void>(dest_rect), (D3DTEXTUREFILTERTYPE) filter);
}

uint32_t CCALL IDirect3DDevice9_CreateOffscreenPlainSurface_c(uint32_t self, uint32_t width, uint32_t height, uint32_t format, uint32_t pool, uint32_t out, uint32_t shared)
{
    IDirect3DSurface9 *surface = nullptr;
    HRESULT result = DEVICE(self)->CreateOffscreenPlainSurface(width, height, (D3DFORMAT) format, (D3DPOOL) pool, &surface, nullptr);

    (void) shared;
    surface_op("CreateOffscreenPlainSurface", surface, nullptr, nullptr, nullptr, (uint32_t) result);
    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_SURFACE, surface, G(self));
}

uint32_t CCALL IDirect3DDevice9_SetRenderTarget_c(uint32_t self, uint32_t index, uint32_t surface)
{
    return (uint32_t) DEVICE(self)->SetRenderTarget(index, host<IDirect3DSurface9>(surface));
}

uint32_t CCALL IDirect3DDevice9_GetRenderTarget_c(uint32_t self, uint32_t index, uint32_t out)
{
    IDirect3DSurface9 *surface = nullptr;
    HRESULT result = DEVICE(self)->GetRenderTarget(index, &surface);

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_SURFACE, surface, G(self));
}

uint32_t CCALL IDirect3DDevice9_SetDepthStencilSurface_c(uint32_t self, uint32_t surface)
{
    return (uint32_t) DEVICE(self)->SetDepthStencilSurface(host<IDirect3DSurface9>(surface));
}

uint32_t CCALL IDirect3DDevice9_GetDepthStencilSurface_c(uint32_t self, uint32_t out)
{
    IDirect3DSurface9 *surface = nullptr;
    HRESULT result = DEVICE(self)->GetDepthStencilSurface(&surface);

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_SURFACE, surface, G(self));
}

uint32_t CCALL IDirect3DDevice9_BeginScene_c(uint32_t self)
{
    Timed timed_(C_BEGINSCENE);
    return (uint32_t) DEVICE(self)->BeginScene();
}

uint32_t CCALL IDirect3DDevice9_EndScene_c(uint32_t self)
{
    return (uint32_t) DEVICE(self)->EndScene();
}

uint32_t CCALL IDirect3DDevice9_Clear_c(uint32_t self, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil)
{
    Timed timed_(C_CLEAR);
    float depth;

    memcpy(&depth, &z, sizeof(depth));
    return (uint32_t) DEVICE(self)->Clear(count, pointer<void>(rects), flags, color, depth, stencil);
}

uint32_t CCALL IDirect3DDevice9_SetTransform_c(uint32_t self, uint32_t state, uint32_t matrix)
{
    return (uint32_t) DEVICE(self)->SetTransform((D3DTRANSFORMSTATETYPE) state, pointer<D3DMATRIX>(matrix));
}

uint32_t CCALL IDirect3DDevice9_GetTransform_c(uint32_t self, uint32_t state, uint32_t matrix)
{
    return (uint32_t) DEVICE(self)->GetTransform((D3DTRANSFORMSTATETYPE) state, pointer<D3DMATRIX>(matrix));
}

uint32_t CCALL IDirect3DDevice9_SetViewport_c(uint32_t self, uint32_t viewport)
{
    return (uint32_t) DEVICE(self)->SetViewport(pointer<D3DVIEWPORT9>(viewport));
}

uint32_t CCALL IDirect3DDevice9_GetViewport_c(uint32_t self, uint32_t viewport)
{
    return (uint32_t) DEVICE(self)->GetViewport(pointer<D3DVIEWPORT9>(viewport));
}

uint32_t CCALL IDirect3DDevice9_SetMaterial_c(uint32_t self, uint32_t material)
{
    return (uint32_t) DEVICE(self)->SetMaterial(pointer<D3DMATERIAL9>(material));
}

uint32_t CCALL IDirect3DDevice9_SetLight_c(uint32_t self, uint32_t index, uint32_t light)
{
    return (uint32_t) DEVICE(self)->SetLight(index, pointer<D3DLIGHT9>(light));
}

uint32_t CCALL IDirect3DDevice9_GetLight_c(uint32_t self, uint32_t index, uint32_t light)
{
    return (uint32_t) DEVICE(self)->GetLight(index, pointer<D3DLIGHT9>(light));
}

uint32_t CCALL IDirect3DDevice9_LightEnable_c(uint32_t self, uint32_t index, uint32_t enable)
{
    return (uint32_t) DEVICE(self)->LightEnable(index, enable);
}

uint32_t CCALL IDirect3DDevice9_GetLightEnable_c(uint32_t self, uint32_t index, uint32_t enable)
{
    return (uint32_t) DEVICE(self)->GetLightEnable(index, pointer<BOOL>(enable));
}

uint32_t CCALL IDirect3DDevice9_SetClipPlane_c(uint32_t self, uint32_t index, uint32_t plane)
{
    return (uint32_t) DEVICE(self)->SetClipPlane(index, pointer<float>(plane));
}

uint32_t CCALL IDirect3DDevice9_GetClipPlane_c(uint32_t self, uint32_t index, uint32_t plane)
{
    return (uint32_t) DEVICE(self)->GetClipPlane(index, pointer<float>(plane));
}

uint32_t CCALL IDirect3DDevice9_SetRenderState_c(uint32_t self, uint32_t state, uint32_t value)
{
    Timed timed_(C_SETSTATE);
    return (uint32_t) DEVICE(self)->SetRenderState((D3DRENDERSTATETYPE) state, value);
}

uint32_t CCALL IDirect3DDevice9_GetRenderState_c(uint32_t self, uint32_t state, uint32_t value)
{
    return (uint32_t) DEVICE(self)->GetRenderState((D3DRENDERSTATETYPE) state, pointer<DWORD>(value));
}

uint32_t CCALL IDirect3DDevice9_CreateStateBlock_c(uint32_t self, uint32_t type, uint32_t out)
{
    IDirect3DStateBlock9 *block = nullptr;
    HRESULT result = DEVICE(self)->CreateStateBlock((D3DSTATEBLOCKTYPE) type, &block);

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_STATEBLOCK, block, G(self));
}

uint32_t CCALL IDirect3DDevice9_SetClipStatus_c(uint32_t self, uint32_t status)
{
    (void) self; (void) status;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetTexture_c(uint32_t self, uint32_t stage, uint32_t texture)
{
    Timed timed_(C_SETTEXTURE);
    return (uint32_t) DEVICE(self)->SetTexture(stage, host<IDirect3DTexture9>(texture));
}

uint32_t CCALL IDirect3DDevice9_GetTexture_c(uint32_t self, uint32_t stage, uint32_t out)
{
    (void) self; (void) stage;
    if (out != 0) *pointer<uint32_t>(out) = 0;
    return D3DERR_NOTAVAILABLE_;
}

uint32_t CCALL IDirect3DDevice9_GetTextureStageState_c(uint32_t self, uint32_t stage, uint32_t type, uint32_t value)
{
    (void) self; (void) stage; (void) type;
    if (value != 0) *pointer<uint32_t>(value) = 0;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetTextureStageState_c(uint32_t self, uint32_t stage, uint32_t type, uint32_t value)
{
    return (uint32_t) DEVICE(self)->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE) type, value);
}

uint32_t CCALL IDirect3DDevice9_GetSamplerState_c(uint32_t self, uint32_t sampler, uint32_t type, uint32_t value)
{
    (void) self; (void) sampler; (void) type;
    if (value != 0) *pointer<uint32_t>(value) = 0;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetSamplerState_c(uint32_t self, uint32_t sampler, uint32_t type, uint32_t value)
{
    return (uint32_t) DEVICE(self)->SetSamplerState(sampler, (D3DSAMPLERSTATETYPE) type, value);
}

uint32_t CCALL IDirect3DDevice9_ValidateDevice_c(uint32_t self, uint32_t passes)
{
    (void) self;
    if (passes != 0) *pointer<uint32_t>(passes) = 1;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetScissorRect_c(uint32_t self, uint32_t rect)
{
    return (uint32_t) DEVICE(self)->SetScissorRect(pointer<RECT>(rect));
}

uint32_t CCALL IDirect3DDevice9_SetSoftwareVertexProcessing_c(uint32_t self, uint32_t software)
{
    (void) self; (void) software;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_GetSoftwareVertexProcessing_c(uint32_t self)
{
    (void) self;
    return 0;
}

uint32_t CCALL IDirect3DDevice9_SetNPatchMode_c(uint32_t self, uint32_t segments)
{
    (void) self; (void) segments;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_GetNPatchMode_c(uint32_t self)
{
    (void) self;
    return 0;
}

uint32_t CCALL IDirect3DDevice9_DrawPrimitive_c(uint32_t self, uint32_t type, uint32_t start, uint32_t count)
{
    Timed timed_(C_DRAW);
    return (uint32_t) DEVICE(self)->DrawPrimitive((D3DPRIMITIVETYPE) type, start, count);
}

uint32_t CCALL IDirect3DDevice9_DrawIndexedPrimitive_c(uint32_t self, uint32_t type, uint32_t base, uint32_t min_index, uint32_t vertices, uint32_t start, uint32_t count)
{
    Timed timed_(C_DRAWINDEXED);
    return (uint32_t) DEVICE(self)->DrawIndexedPrimitive((D3DPRIMITIVETYPE) type, (INT) base, min_index, vertices, start, count);
}

uint32_t CCALL IDirect3DDevice9_DrawPrimitiveUP_c(uint32_t self, uint32_t type, uint32_t count, uint32_t data, uint32_t stride)
{
    Timed timed_(C_DRAWUP);
    return (uint32_t) DEVICE(self)->DrawPrimitiveUP((D3DPRIMITIVETYPE) type, count, pointer<void>(data), stride);
}

// (Type, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride)
uint32_t CCALL IDirect3DDevice9_DrawIndexedPrimitiveUP_c(const uint32_t *args)
{
    Timed timed_(C_DRAWINDEXEDUP);
    return (uint32_t) DEVICE(args[0])->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE) args[1], args[2], args[3], args[4], pointer<void>(args[5]), (D3DFORMAT) args[6], pointer<void>(args[7]), args[8]);
}

uint32_t CCALL IDirect3DDevice9_CreateVertexDeclaration_c(uint32_t self, uint32_t elements, uint32_t out)
{
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    HRESULT result = DEVICE(self)->CreateVertexDeclaration(pointer<void>(elements), &declaration);

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_VERTEXDECL, declaration, G(self));
}

uint32_t CCALL IDirect3DDevice9_SetVertexDeclaration_c(uint32_t self, uint32_t declaration)
{
    Timed timed_(C_SETDECL);
    return (uint32_t) DEVICE(self)->SetVertexDeclaration(host<void>(declaration));
}

uint32_t CCALL IDirect3DDevice9_SetFVF_c(uint32_t self, uint32_t fvf)
{
    Timed timed_(C_SETFVF);
    return (uint32_t) DEVICE(self)->SetFVF(fvf);
}

uint32_t CCALL IDirect3DDevice9_CreateVertexShader_c(uint32_t self, uint32_t function, uint32_t out)
{
    IDirect3DVertexShader9 *shader = nullptr;
    HRESULT result = DEVICE(self)->CreateVertexShader(pointer<DWORD>(function), &shader);

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_VERTEXSHADER, shader, G(self));
}

uint32_t CCALL IDirect3DDevice9_SetVertexShader_c(uint32_t self, uint32_t shader)
{
    return (uint32_t) DEVICE(self)->SetVertexShader(host<void>(shader));
}

uint32_t CCALL IDirect3DDevice9_SetVertexShaderConstantF_c(uint32_t self, uint32_t start, uint32_t data, uint32_t count)
{
    return (uint32_t) DEVICE(self)->SetVertexShaderConstantF(start, pointer<float>(data), count);
}

uint32_t CCALL IDirect3DDevice9_SetStreamSource_c(uint32_t self, uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride)
{
    Timed timed_(C_SETSTREAM);
    return (uint32_t) DEVICE(self)->SetStreamSource(stream, host<IDirect3DVertexBuffer9>(buffer), offset, stride);
}

uint32_t CCALL IDirect3DDevice9_SetStreamSourceFreq_c(uint32_t self, uint32_t stream, uint32_t divider)
{
    (void) self; (void) stream; (void) divider;
    return OK;
}

uint32_t CCALL IDirect3DDevice9_SetIndices_c(uint32_t self, uint32_t buffer)
{
    return (uint32_t) DEVICE(self)->SetIndices(host<IDirect3DIndexBuffer9>(buffer));
}

uint32_t CCALL IDirect3DDevice9_CreatePixelShader_c(uint32_t self, uint32_t function, uint32_t out)
{
    IDirect3DPixelShader9 *shader = nullptr;
    HRESULT result = DEVICE(self)->CreatePixelShader(pointer<DWORD>(function), &shader);

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_PIXELSHADER, shader, G(self));
}

uint32_t CCALL IDirect3DDevice9_SetPixelShader_c(uint32_t self, uint32_t shader)
{
    return (uint32_t) DEVICE(self)->SetPixelShader(host<void>(shader));
}

uint32_t CCALL IDirect3DDevice9_SetPixelShaderConstantF_c(uint32_t self, uint32_t start, uint32_t data, uint32_t count)
{
    return (uint32_t) DEVICE(self)->SetPixelShaderConstantF(start, pointer<float>(data), count);
}

uint32_t CCALL IDirect3DDevice9_CreateQuery_c(uint32_t self, uint32_t type, uint32_t out)
{
    IDirect3DQuery9 *query = nullptr;
    HRESULT result = DEVICE(self)->CreateQuery((D3DQUERYTYPE) type, &query);
#ifdef SRW_DIAGNOSTICS
    {
        // what the program does with queries (the occlusion test, type 9): how often, with or without an object to fill, from where
        static unsigned calls;

        if (SRW_LOG_ON(SRW_LOG_D3D) && (calls++ < 8 || calls % 1000 == 0))
            logprintf("CreateQuery #%u: type %u, %s, from %x", calls, (unsigned) type, out != 0 ? "object asked for" : "support check only", (unsigned) SRW_LastCaller());
    }
#endif

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_QUERY, query, G(self));
}

// ------------------------------------------------------------------------------------------------------------------
// resources

uint32_t CCALL IDirect3DResource9_GetDevice_c(uint32_t self, uint32_t out)
{
    Guest *device = G(self)->device;

    if (out == 0 || device == nullptr) return D3DERR_INVALIDCALL_;
    __atomic_add_fetch(&device->references, 1, __ATOMIC_SEQ_CST);
    *pointer<uint32_t>(out) = (uint32_t) (uintptr_t) device;
    return OK;
}

uint32_t CCALL IDirect3DResource9_SetPrivateData_c(uint32_t self, uint32_t guid, uint32_t data, uint32_t size, uint32_t flags)
{
    (void) self; (void) guid; (void) data; (void) size; (void) flags;
    return OK;
}

uint32_t CCALL IDirect3DResource9_GetPrivateData_c(uint32_t self, uint32_t guid, uint32_t data, uint32_t size)
{
    (void) self; (void) guid; (void) data; (void) size;
    return D3DERR_NOTAVAILABLE_;
}

uint32_t CCALL IDirect3DResource9_FreePrivateData_c(uint32_t self, uint32_t guid)
{
    (void) self; (void) guid;
    return OK;
}

uint32_t CCALL IDirect3DResource9_SetPriority_c(uint32_t self, uint32_t priority)
{
    (void) self; (void) priority;
    return 0;
}

uint32_t CCALL IDirect3DResource9_GetPriority_c(uint32_t self)
{
    (void) self;
    return 0;
}

uint32_t CCALL IDirect3DResource9_PreLoad_c(uint32_t self)
{
    (void) self;
    return 0;
}

uint32_t CCALL IDirect3DResource9_GetType_c(uint32_t self)
{
    switch (G(self)->kind)
    {
    case K_SURFACE: return 1;
    case K_TEXTURE: return 3;
    case K_VERTEXBUFFER: return 6;
    case K_INDEXBUFFER: return 7;
    default: return 0;
    }
}

uint32_t CCALL IDirect3DBaseTexture9_SetLOD_c(uint32_t self, uint32_t lod)
{
    return host<IDirect3DTexture9>(self)->SetLOD(lod);
}

uint32_t CCALL IDirect3DBaseTexture9_GetLOD_c(uint32_t self)
{
    return host<IDirect3DTexture9>(self)->GetLOD();
}

uint32_t CCALL IDirect3DBaseTexture9_GetLevelCount_c(uint32_t self)
{
    return host<IDirect3DTexture9>(self)->GetLevelCount();
}

uint32_t CCALL IDirect3DBaseTexture9_SetAutoGenFilterType_c(uint32_t self, uint32_t type)
{
    (void) self; (void) type;
    return OK;
}

uint32_t CCALL IDirect3DBaseTexture9_GetAutoGenFilterType_c(uint32_t self)
{
    (void) self;
    return 2; // D3DTEXF_LINEAR
}

uint32_t CCALL IDirect3DBaseTexture9_GenerateMipSubLevels_c(uint32_t self)
{
    (void) self;
    return 0;
}

uint32_t CCALL IDirect3DTexture9_GetLevelDesc_c(uint32_t self, uint32_t level, uint32_t desc)
{
    return (uint32_t) host<IDirect3DTexture9>(self)->GetLevelDesc(level, pointer<D3DSURFACE_DESC>(desc));
}

uint32_t CCALL IDirect3DTexture9_GetSurfaceLevel_c(uint32_t self, uint32_t level, uint32_t out)
{
    IDirect3DSurface9 *surface = nullptr;
    HRESULT result = host<IDirect3DTexture9>(self)->GetSurfaceLevel(level, &surface);

    if (result != D3D_OK) return (uint32_t) result;
    return give(out, K_SURFACE, surface, G(self)->device);
}

uint32_t CCALL IDirect3DTexture9_LockRect_c(uint32_t self, uint32_t level, uint32_t locked, uint32_t rect, uint32_t flags)
{
    return (uint32_t) host<IDirect3DTexture9>(self)->LockRect(level, pointer<D3DLOCKED_RECT>(locked), pointer<void>(rect), flags);
}

uint32_t CCALL IDirect3DTexture9_UnlockRect_c(uint32_t self, uint32_t level)
{
    return (uint32_t) host<IDirect3DTexture9>(self)->UnlockRect(level);
}

uint32_t CCALL IDirect3DTexture9_AddDirtyRect_c(uint32_t self, uint32_t rect)
{
    (void) self; (void) rect;
    return OK;
}

uint32_t CCALL IDirect3DSurface9_GetDesc_c(uint32_t self, uint32_t desc)
{
    return (uint32_t) host<IDirect3DSurface9>(self)->GetDesc(pointer<D3DSURFACE_DESC>(desc));
}

uint32_t CCALL IDirect3DSurface9_LockRect_c(uint32_t self, uint32_t locked, uint32_t rect, uint32_t flags)
{
    uint32_t result = (uint32_t) host<IDirect3DSurface9>(self)->LockRect(pointer<D3DLOCKED_RECT>(locked), pointer<RECT>(rect), flags);

    surface_op("LockRect", host<IDirect3DSurface9>(self), nullptr, pointer<void>(rect), nullptr, result);
    return result;
}

uint32_t CCALL IDirect3DSurface9_UnlockRect_c(uint32_t self)
{
    return (uint32_t) host<IDirect3DSurface9>(self)->UnlockRect();
}

uint32_t CCALL IDirect3DVertexBuffer9_Lock_c(uint32_t self, uint32_t offset, uint32_t size, uint32_t data, uint32_t flags)
{
    return (uint32_t) host<IDirect3DVertexBuffer9>(self)->Lock(offset, size, pointer<void *>(data), flags);
}

uint32_t CCALL IDirect3DVertexBuffer9_Unlock_c(uint32_t self)
{
    return (uint32_t) host<IDirect3DVertexBuffer9>(self)->Unlock();
}

uint32_t CCALL IDirect3DIndexBuffer9_Lock_c(uint32_t self, uint32_t offset, uint32_t size, uint32_t data, uint32_t flags)
{
    return (uint32_t) host<IDirect3DIndexBuffer9>(self)->Lock(offset, size, pointer<void *>(data), flags);
}

uint32_t CCALL IDirect3DIndexBuffer9_Unlock_c(uint32_t self)
{
    return (uint32_t) host<IDirect3DIndexBuffer9>(self)->Unlock();
}

uint32_t CCALL IDirect3DStateBlock9_Capture_c(uint32_t self)
{
    return (uint32_t) host<IDirect3DStateBlock9>(self)->Capture();
}

uint32_t CCALL IDirect3DStateBlock9_Apply_c(uint32_t self)
{
    return (uint32_t) host<IDirect3DStateBlock9>(self)->Apply();
}

uint32_t CCALL IDirect3DQuery9_Issue_c(uint32_t self, uint32_t flags)
{
    return (uint32_t) host<IDirect3DQuery9>(self)->Issue(flags);
}

uint32_t CCALL IDirect3DQuery9_GetData_c(uint32_t self, uint32_t data, uint32_t size, uint32_t flags)
{
    return (uint32_t) host<IDirect3DQuery9>(self)->GetData(pointer<void>(data), size, flags);
}

uint32_t CCALL IDirect3DQuery9_GetDataSize_c(uint32_t self)
{
    (void) self;
    return 4;
}

// ------------------------------------------------------------------------------------------------------------------
// the entry point the program looks up in d3d9.dll

uint32_t CCALL Direct3DCreate9_c(uint32_t SDKVersion)
{
    IDirect3D9 *direct3d = WebGLDirect3DCreate9(SDKVersion);

    if (direct3d == nullptr)
    {
        warnprint("Direct3D 9 could not be created");
        return 0;
    }
    return (uint32_t) (uintptr_t) make(K_D3D, direct3d, nullptr);
}

}  // extern "C"
