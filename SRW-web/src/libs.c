/* gdi32, comctl32, imm32, shell32, winmm, wininet and wsock32: the parts of these libraries the program calls at
   startup, implemented as the least that keeps it going. The window is drawn to by Direct3D, so GDI has nothing to
   draw on; sockets and downloads are not available yet. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "srw.h"
#include "imports.h"
#include "srw_win32.h"

#define UNUSED1(a) (void) (a)

/* ------------------------------------------------------------------------------------------------------------------ */
/* gdi32 */

uint32_t CCALL BitBlt_c(const uint32_t *args)
{
    (void) args;
    return 1;
}

uint32_t CCALL ChoosePixelFormat_c(uint32_t hdc, void *ppfd)
{
    (void) hdc;
    (void) ppfd;
    return 1;
}

uint32_t CCALL DescribePixelFormat_c(uint32_t hdc, uint32_t iPixelFormat, uint32_t nBytes, void *ppfd)
{
    uint8_t *p = ppfd;

    (void) hdc;
    if (ppfd != NULL && nBytes >= 40 && iPixelFormat == 1)
    {
        memset(p, 0, 40);
        *(uint16_t *) p = 40;
        *(uint16_t *) (p + 2) = 1;
        *(uint32_t *) (p + 4) = 0x25; /* draw to window, OpenGL, double buffered */
        p[8] = 0;                     /* RGBA */
        p[9] = 32;
        p[23] = 24;                   /* cDepthBits */
        p[24] = 8;                    /* cStencilBits */
    }
    return 1;
}

uint32_t CCALL GetPixelFormat_c(uint32_t hdc)
{
    (void) hdc;
    return 1;
}

uint32_t CCALL SetPixelFormat_c(uint32_t hdc, uint32_t format, void *ppfd)
{
    (void) hdc;
    (void) format;
    (void) ppfd;
    return 1;
}

uint32_t CCALL CombineRgn_c(uint32_t hrgnDst, uint32_t hrgnSrc1, uint32_t hrgnSrc2, uint32_t iMode)
{
    (void) hrgnDst; (void) hrgnSrc1; (void) hrgnSrc2; (void) iMode;
    return 2; /* SIMPLEREGION */
}

uint32_t CCALL CreateCompatibleDC_c(uint32_t hdc)
{
    (void) hdc;
    return 0x21001;
}

/* (hdc, pbmi, usage, ppvBits, hSection, offset): the bits are real memory the program draws into */
uint32_t CCALL CreateDIBSection_c(uint32_t hdc, void *pbmi, uint32_t usage, void *ppvBits, uint32_t hSection, uint32_t offset)
{
    const int32_t *header = pbmi;
    int32_t width = header[1], height = header[2];
    uint32_t bits = *(const uint16_t *) ((const uint8_t *) pbmi + 14);
    size_t size;
    void *memory;

    (void) hdc; (void) usage; (void) hSection; (void) offset;
    if (height < 0) height = -height;
    size = (size_t) width * (size_t) height * (bits ? bits : 32) / 8 + 64;
    memory = calloc(1, size);
    if (ppvBits != NULL) *(uint32_t *) ppvBits = (uint32_t) (uintptr_t) memory;
    return 0x22001;
}

uint32_t CCALL CreateDIBitmap_c(uint32_t hdc, void *pbmih, uint32_t flInit, void *pjBits, void *pbmi, uint32_t iUsage)
{
    (void) hdc; (void) pbmih; (void) flInit; (void) pjBits; (void) pbmi; (void) iUsage;
    return 0x22002;
}

uint32_t CCALL CreateFontIndirectA_c(void *lplf)
{
    (void) lplf;
    return 0x23001;
}

uint32_t CCALL CreatePen_c(uint32_t iStyle, uint32_t cWidth, uint32_t color)
{
    (void) iStyle; (void) cWidth; (void) color;
    return 0x24001;
}

uint32_t CCALL CreateRectRgn_c(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    (void) x1; (void) y1; (void) x2; (void) y2;
    return 0x25001;
}

uint32_t CCALL CreateRectRgnIndirect_c(void *lprect)
{
    (void) lprect;
    return 0x25001;
}

uint32_t CCALL CreateSolidBrush_c(uint32_t color)
{
    (void) color;
    return 0x26001;
}

uint32_t CCALL DeleteDC_c(uint32_t hdc)
{
    (void) hdc;
    return 1;
}

uint32_t CCALL DeleteObject_c(uint32_t ho)
{
    (void) ho;
    return 1;
}

uint32_t CCALL FillRgn_c(uint32_t hdc, uint32_t hrgn, uint32_t hbr)
{
    (void) hdc; (void) hrgn; (void) hbr;
    return 1;
}

uint32_t CCALL GdiFlush_c(void)
{
    return 1;
}

uint32_t CCALL GetBkColor_c(uint32_t hdc)
{
    (void) hdc;
    return 0xffffff;
}

/* the ramp of the display is not adjustable from a page: report a linear one and accept changes silently */
uint32_t CCALL GetDeviceGammaRamp_c(uint32_t hdc, void *lpRamp)
{
    uint16_t *ramp = lpRamp;
    int channel, i;

    (void) hdc;
    for (channel = 0; channel < 3; channel++)
    {
        for (i = 0; i < 256; i++) ramp[channel * 256 + i] = (uint16_t) (i * 257);
    }
    return 1;
}

uint32_t CCALL SetDeviceGammaRamp_c(uint32_t hdc, void *lpRamp)
{
    (void) hdc;
    (void) lpRamp;
    return 1;
}

uint32_t CCALL GetObjectA_c(uint32_t h, uint32_t c, void *pv)
{
    (void) h; (void) c; (void) pv;
    return 0;
}

uint32_t CCALL GetStockObject_c(uint32_t i)
{
    return 0x27000 + i;
}

uint32_t CCALL GetTextExtentPoint32A_c(uint32_t hdc, const char *lpString, uint32_t c, void *lpsz)
{
    int32_t *size = lpsz;

    (void) hdc;
    (void) lpString;
    size[0] = (int32_t) c * 8;
    size[1] = 16;
    return 1;
}

uint32_t CCALL LineTo_c(uint32_t hdc, uint32_t x, uint32_t y)
{
    (void) hdc; (void) x; (void) y;
    return 1;
}

uint32_t CCALL MoveToEx_c(uint32_t hdc, uint32_t x, uint32_t y, void *lppt)
{
    (void) hdc; (void) x; (void) y; (void) lppt;
    return 1;
}

uint32_t CCALL OffsetViewportOrgEx_c(uint32_t hdc, uint32_t x, uint32_t y, void *lppt)
{
    (void) hdc; (void) x; (void) y; (void) lppt;
    return 1;
}

uint32_t CCALL SetViewportOrgEx_c(uint32_t hdc, uint32_t x, uint32_t y, void *lppt)
{
    (void) hdc; (void) x; (void) y; (void) lppt;
    return 1;
}

uint32_t CCALL SelectClipRgn_c(uint32_t hdc, uint32_t hrgn)
{
    (void) hdc; (void) hrgn;
    return 2;
}

uint32_t CCALL SelectObject_c(uint32_t hdc, uint32_t h)
{
    (void) hdc; (void) h;
    return 0x27001;
}

uint32_t CCALL SetBkColor_c(uint32_t hdc, uint32_t color)
{
    (void) hdc; (void) color;
    return 0xffffff;
}

uint32_t CCALL SetBkMode_c(uint32_t hdc, uint32_t mode)
{
    (void) hdc; (void) mode;
    return 2;
}

uint32_t CCALL SetMapMode_c(uint32_t hdc, uint32_t mode)
{
    (void) hdc; (void) mode;
    return 1;
}

uint32_t CCALL SetPixel_c(uint32_t hdc, uint32_t x, uint32_t y, uint32_t color)
{
    (void) hdc; (void) x; (void) y;
    return color;
}

uint32_t CCALL SetTextColor_c(uint32_t hdc, uint32_t color)
{
    (void) hdc; (void) color;
    return 0;
}

uint32_t CCALL TextOutA_c(uint32_t hdc, uint32_t x, uint32_t y, const char *lpString, uint32_t c)
{
    (void) hdc; (void) x; (void) y; (void) lpString; (void) c;
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* comctl32 */

uint32_t CCALL InitCommonControlsEx_c(void *picce)
{
    (void) picce;
    return 1;
}

uint32_t CCALL _TrackMouseEvent_c(void *lpEventTrack)
{
    (void) lpEventTrack;
    return 1;
}

uint32_t CCALL ImageList_Create_c(uint32_t cx, uint32_t cy, uint32_t flags, uint32_t cInitial, uint32_t cGrow)
{
    (void) cx; (void) cy; (void) flags; (void) cInitial; (void) cGrow;
    return 0xa0001;
}

uint32_t CCALL ImageList_Destroy_c(void *himl)
{
    (void) himl;
    return 1;
}

uint32_t CCALL ImageList_Add_c(void *himl, uint32_t hbmImage, uint32_t hbmMask)
{
    (void) himl; (void) hbmImage; (void) hbmMask;
    return 0;
}

uint32_t CCALL ImageList_Replace_c(void *himl, uint32_t i, uint32_t hbmImage, uint32_t hbmMask)
{
    (void) himl; (void) i; (void) hbmImage; (void) hbmMask;
    return 1;
}

uint32_t CCALL ImageList_GetImageCount_c(void *himl)
{
    (void) himl;
    return 0;
}

uint32_t CCALL ImageList_BeginDrag_c(void *himlTrack, uint32_t iTrack, uint32_t dxHotspot, uint32_t dyHotspot)
{
    (void) himlTrack; (void) iTrack; (void) dxHotspot; (void) dyHotspot;
    return 0;
}

uint32_t CCALL ImageList_EndDrag_c(void)
{
    return 0;
}

uint32_t CCALL ImageList_DragEnter_c(uint32_t hwndLock, uint32_t x, uint32_t y)
{
    (void) hwndLock; (void) x; (void) y;
    return 0;
}

uint32_t CCALL ImageList_DragLeave_c(uint32_t hwndLock)
{
    (void) hwndLock;
    return 0;
}

uint32_t CCALL ImageList_DragMove_c(uint32_t x, uint32_t y)
{
    (void) x; (void) y;
    return 0;
}

uint32_t CCALL ImageList_DragShowNolock_c(uint32_t fShow)
{
    (void) fShow;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* imm32: one input method context. A program that draws its own text boxes enables the input method while one has the focus (a
   non-null context) and disables it otherwise (null): the page learns of it through SRW_NoteInputMethod. */

#define IMM_CONTEXT 0x1c0001u
static volatile int imm_enabled;

uint32_t CCALL ImmGetContext_c(uint32_t hWnd)
{
    (void) hWnd;
    return IMM_CONTEXT;
}

uint32_t CCALL ImmReleaseContext_c(uint32_t hWnd, uint32_t hIMC)
{
    (void) hWnd; (void) hIMC;
    return 1;
}

uint32_t CCALL ImmAssociateContext_c(uint32_t hWnd, uint32_t hIMC)
{
    uint32_t previous = imm_enabled ? IMM_CONTEXT : 0;

    (void) hWnd;
    imm_enabled = hIMC != 0;
    SRW_NoteInputMethod(imm_enabled);
    return previous;
}

uint32_t CCALL ImmAssociateContextEx_c(uint32_t hWnd, uint32_t hIMC, uint32_t dwFlags)
{
    (void) dwFlags;
    ImmAssociateContext_c(hWnd, hIMC);
    return 1;
}

uint32_t CCALL ImmGetCandidateListA_c(uint32_t hIMC, uint32_t deIndex, void *lpCandList, uint32_t dwBufLen)
{
    (void) hIMC; (void) deIndex; (void) lpCandList; (void) dwBufLen;
    return 0;
}

uint32_t CCALL ImmGetCompositionStringA_c(uint32_t hIMC, uint32_t dwIndex, void *lpBuf, uint32_t dwBufLen)
{
    (void) hIMC; (void) dwIndex; (void) lpBuf; (void) dwBufLen;
    return 0;
}

uint32_t CCALL ImmGetConversionStatus_c(uint32_t hIMC, void *lpfdwConversion, void *lpfdwSentence)
{
    (void) hIMC; (void) lpfdwConversion; (void) lpfdwSentence;
    return 0;
}

uint32_t CCALL ImmSetConversionStatus_c(uint32_t hIMC, uint32_t fdwConversion, uint32_t fdwSentence)
{
    (void) hIMC; (void) fdwConversion; (void) fdwSentence;
    return 0;
}

uint32_t CCALL ImmNotifyIME_c(uint32_t hIMC, uint32_t dwAction, uint32_t dwIndex, uint32_t dwValue)
{
    (void) hIMC; (void) dwAction; (void) dwIndex; (void) dwValue;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* shell32, winmm */

uint32_t CCALL FindExecutableA_c(const char *lpFile, const char *lpDirectory, void *lpResult)
{
    (void) lpFile; (void) lpDirectory; (void) lpResult;
    return 2; /* not found */
}

/* opening a web address is the one thing worth doing: the page opens it in a new tab */
uint32_t CCALL ShellExecuteA_c(uint32_t hwnd, const char *lpOperation, const char *lpFile, const char *lpParameters, const char *lpDirectory, uint32_t nShowCmd)
{
    (void) hwnd; (void) lpOperation; (void) lpParameters; (void) lpDirectory; (void) nShowCmd;
    if (lpFile != NULL && (strncmp(lpFile, "http://", 7) == 0 || strncmp(lpFile, "https://", 8) == 0))
    {
        MAIN_THREAD_EM_ASM({
            window.open(UTF8ToString($0), "_blank");
        }, lpFile);
        return 33;
    }
    return 2;
}

uint32_t CCALL joyGetNumDevs_c(void)
{
    return 0;
}

uint32_t CCALL joyGetDevCapsA_c(uint32_t uJoyID, void *pjc, uint32_t cbjc)
{
    (void) uJoyID; (void) pjc; (void) cbjc;
    return 6; /* MMSYSERR_NODRIVER */
}

uint32_t CCALL joyGetPosEx_c(uint32_t uJoyID, void *pji)
{
    (void) uJoyID; (void) pji;
    return 167; /* JOYERR_UNPLUGGED */
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* wininet: the patch downloader is not available */

uint32_t CCALL InternetOpenA_c(const char *lpszAgent, uint32_t dwAccessType, const char *lpszProxy, const char *lpszProxyBypass, uint32_t dwFlags)
{
    (void) lpszAgent; (void) dwAccessType; (void) lpszProxy; (void) lpszProxyBypass; (void) dwFlags;
    SRW_SetLastError(12029); /* ERROR_INTERNET_CANNOT_CONNECT */
    return 0;
}

uint32_t CCALL InternetCloseHandle_c(uint32_t hInternet)
{
    (void) hInternet;
    return 1;
}

uint32_t CCALL InternetSetStatusCallback_c(void *hInternet, void *lpfnInternetCallback)
{
    (void) hInternet; (void) lpfnInternetCallback;
    return 0;
}

uint32_t CCALL InternetSetOptionA_c(void *hInternet, uint32_t dwOption, void *lpBuffer, uint32_t dwBufferLength)
{
    (void) hInternet; (void) dwOption; (void) lpBuffer; (void) dwBufferLength;
    return 0;
}

uint32_t CCALL InternetCrackUrlA_c(const char *lpszUrl, uint32_t dwUrlLength, uint32_t dwFlags, void *lpUrlComponents)
{
    (void) lpszUrl; (void) dwUrlLength; (void) dwFlags; (void) lpUrlComponents;
    return 0;
}

uint32_t CCALL InternetConnectA_c(void *hInternet, const char *lpszServerName, uint32_t nServerPort, const char *lpszUserName, const char *lpszPassword,
                                  uint32_t dwService, uint32_t dwFlags, uint32_t dwContext)
{
    (void) hInternet; (void) lpszServerName; (void) nServerPort; (void) lpszUserName; (void) lpszPassword; (void) dwService; (void) dwFlags; (void) dwContext;
    return 0;
}

uint32_t CCALL HttpOpenRequestA_c(void *hConnect, const char *lpszVerb, const char *lpszObjectName, const char *lpszVersion, const char *lpszReferrer,
                                  void *lplpszAcceptTypes, uint32_t dwFlags, uint32_t dwContext)
{
    (void) hConnect; (void) lpszVerb; (void) lpszObjectName; (void) lpszVersion; (void) lpszReferrer; (void) lplpszAcceptTypes; (void) dwFlags; (void) dwContext;
    return 0;
}

uint32_t CCALL HttpSendRequestA_c(void *hRequest, const char *lpszHeaders, uint32_t dwHeadersLength, void *lpOptional, uint32_t dwOptionalLength)
{
    (void) hRequest; (void) lpszHeaders; (void) dwHeadersLength; (void) lpOptional; (void) dwOptionalLength;
    return 0;
}

uint32_t CCALL HttpQueryInfoA_c(void *hRequest, uint32_t dwInfoLevel, void *lpBuffer, void *lpdwBufferLength, void *lpdwIndex)
{
    (void) hRequest; (void) dwInfoLevel; (void) lpBuffer; (void) lpdwBufferLength; (void) lpdwIndex;
    return 0;
}

uint32_t CCALL InternetReadFileExA_c(void *hFile, void *lpBuffersOut, uint32_t dwFlags, uint32_t dwContext)
{
    (void) hFile; (void) lpBuffersOut; (void) dwFlags; (void) dwContext;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* wsock32 is in winsock.c */

