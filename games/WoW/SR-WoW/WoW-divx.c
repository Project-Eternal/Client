/* DivxDecoder.dll for the client's movies (Interface\Cinematics\*.avi: Blizzard's DivX 5 variant "DXGM", MPEG-4 part 2, with an MP3 track).
   The client does everything else itself: it reads the AVI, plays the sound through FMOD, keeps the time, puts the frames into
   textures and shows the subtitles. It only hands each compressed frame to these four functions (cdecl, the DivX 5 "decore" API
   wrapped by Blizzard): InitializeDivxDecoder(slot, width, height), SetOutputFormat(slot, format, width, height) (the client asks for
   format 1: 32-bit RGB), DivxDecode(slot, DEC_FRAME *, 0), UnInitializeDivxDecoder(slot); 0 means success.
   The frames are decoded with FFmpeg's MPEG-4 decoder (third_party/ffmpeg, built by tools/build_ffmpeg.sh) and converted from YUV 4:2:0. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <emscripten.h>

#include "WoW.h"
#include "srw_debuglog.h"

#define MAX_DECODERS 8

typedef struct {
    uint32_t bmp;          /* the output (a guest address) */
    uint32_t bitstream;    /* the compressed frame */
    int32_t length;
    int32_t render_flag;   /* the frame is to be shown: convert it into bmp */
    uint32_t stride;       /* in pixels, 0: the width */
} DecFrame;

typedef struct {
    uint32_t slot;         /* the client's number for it (a counter that grows with every movie) */
    AVCodecContext *context;
    AVPacket *packet;
    AVFrame *frame;        /* the last picture */
    AVFrame *scratch;
    int width, height;
    int format;            /* SetOutputFormat's code; 1 is 32-bit RGB (B, G, R, X in memory) */
    int have_picture;      /* a picture was decoded (the first calls of a stream with B-frames give none) */
    int frames;
    int pictures;          /* pictures the decoder gave */
    double decode_ms, convert_ms;   /* time spent (debug log) */
} Decoder;

static Decoder decoders[MAX_DECODERS];

/* The original decoder writes RGB like a Windows bitmap with a positive height: the bottom row first. */
#ifndef WOW_DIVX_TOP_DOWN
#define WOW_DIVX_TOP_DOWN 0
#endif

static Decoder *decoder_of(uint32_t slot)
{
    int i;

    for (i = 0; i < MAX_DECODERS; i++)
    {
        if (decoders[i].context != NULL && decoders[i].slot == slot) return &decoders[i];
    }
    return NULL;
}

uint32_t CCALL InitializeDivxDecoder_c(uint32_t slot, uint32_t width, uint32_t height)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    Decoder *d;

    int i;

    if (codec == NULL) return 1;
    d = decoder_of(slot);
    for (i = 0; d == NULL && i < MAX_DECODERS; i++) if (decoders[i].context == NULL) d = &decoders[i];
    if (d == NULL) return 1;
    if (d->context != NULL) avcodec_free_context(&d->context);
    memset(d, 0, sizeof(*d));
    d->slot = slot;
    d->context = avcodec_alloc_context3(codec);
    if (d->context == NULL) return 1;
    d->context->width = (int) width;
    d->context->height = (int) height;
    d->context->codec_tag = MKTAG('D', 'X', '5', '0');   /* a DivX 5 stream: the decoder's DivX workarounds apply */
    d->context->thread_count = 1;
    if (avcodec_open2(d->context, codec, NULL) < 0)
    {
        avcodec_free_context(&d->context);
        return 1;
    }
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->scratch = av_frame_alloc();
    d->width = (int) width;
    d->height = (int) height;
    d->format = 1;
    SRW_LOG(SRW_LOG_SOUND, "Movie: decoder %u for %ux%u", (unsigned int) slot, (unsigned int) width, (unsigned int) height);
    return 0;
}

uint32_t CCALL SetOutputFormat_c(uint32_t slot, uint32_t format, uint32_t width, uint32_t height)
{
    Decoder *d = decoder_of(slot);

    if (d == NULL) return 1;
    if (format != 1 && format != 4)   /* 32-bit RGB (BI_RGB) or "ARGB": the same bytes for us; others are not used by the client */
    {
        warnprintf("Movie: output format %u is not supported", (unsigned int) format);
        return 1;
    }
    d->format = (int) format;
    d->width = (int) width;
    d->height = (int) height;
    return 0;
}

static uint8_t clamp_byte(int value)
{
    return (uint8_t) (value < 0 ? 0 : value > 255 ? 255 : value);
}

/* the decoded picture as 32-bit B, G, R, 255 (ITU-R BT.601, limited range) */
static void convert(const Decoder *d, uint8_t *out, uint32_t stride_pixels)
{
    const AVFrame *f = d->frame;
    int width = d->width < f->width ? d->width : f->width, height = d->height < f->height ? d->height : f->height;
    int x, y;

    if (f->format != AV_PIX_FMT_YUV420P)
    {
        static int reported;

        if (!reported++) warnprintf("Movie: the decoder gives pixel format %d, not YUV 4:2:0", f->format);
        return;
    }
    for (y = 0; y < height; y++)
    {
        const uint8_t *py = f->data[0] + (size_t) y * f->linesize[0];
        const uint8_t *pu = f->data[1] + (size_t) (y / 2) * f->linesize[1];
        const uint8_t *pv = f->data[2] + (size_t) (y / 2) * f->linesize[2];
        int row = WOW_DIVX_TOP_DOWN ? y : d->height - 1 - y;
        uint32_t *o = (uint32_t *) (out + (size_t) row * stride_pixels * 4);

        /* two pixels share their chroma: its terms once per pair, a pixel stored as one word (B, G, R, 255 in memory) */
        for (x = 0; x < width; x += 2)
        {
            int u = pu[x / 2] - 128, v = pv[x / 2] - 128;
            int blue = 516 * u + 128, green = -100 * u - 208 * v + 128, red = 409 * v + 128;
            int k;

            for (k = 0; k < 2 && x + k < width; k++)
            {
                int c = 298 * (py[x + k] - 16);

                o[x + k] = (uint32_t) clamp_byte((c + blue) >> 8) | ((uint32_t) clamp_byte((c + green) >> 8) << 8) |
                           ((uint32_t) clamp_byte((c + red) >> 8) << 16) | 0xff000000u;
            }
        }
    }
}

uint32_t CCALL DivxDecode_c(uint32_t slot, uint32_t frame_address, uint32_t info)
{
    Decoder *d = decoder_of(slot);
    const DecFrame *frame = (const DecFrame *) (uintptr_t) frame_address;
    int result;

    (void) info;
    if (d == NULL || frame == NULL) return 1;
    d->frames++;
    {
        static double reported;
        double now = emscripten_get_now();

        if (SRW_LOG_ON(SRW_LOG_SOUND) && now - reported > 5000.0)
        {
            reported = now;
            logprintf("Movie: decoder %u at frame %d (%d pictures)", (unsigned int) slot, d->frames, d->pictures);
        }
    }
    if (frame->length > 0 && frame->bitstream != 0)
    {
        double start = emscripten_get_now();

        d->packet->data = (uint8_t *) (uintptr_t) frame->bitstream;
        d->packet->size = frame->length;
        result = avcodec_send_packet(d->context, d->packet);
        if (result < 0 && result != AVERROR(EAGAIN) && SRW_LOG_ON(SRW_LOG_SOUND) && d->frames < 50)
            logprintf("Movie: frame %d could not be decoded (%d)", d->frames, result);
        /* a packed frame (DivX: P and B in one chunk) can give more than one picture: the last one counts. (avcodec_receive_frame clears
           the frame it is given also when it has nothing: a scratch frame, moved into d->frame when it holds a picture) */
        while (avcodec_receive_frame(d->context, d->scratch) == 0)
        {
            av_frame_unref(d->frame);
            av_frame_move_ref(d->frame, d->scratch);
            d->have_picture = 1;
            d->pictures++;
        }
        d->decode_ms += emscripten_get_now() - start;
    }
    if (frame->render_flag && d->have_picture && frame->bmp != 0)
    {
        double start = emscripten_get_now();

        convert(d, (uint8_t *) (uintptr_t) frame->bmp, frame->stride != 0 ? frame->stride : (uint32_t) d->width);
        d->convert_ms += emscripten_get_now() - start;
    }
    return 0;
}

uint32_t CCALL UnInitializeDivxDecoder_c(uint32_t slot)
{
    Decoder *d = decoder_of(slot);

    if (d == NULL) return 1;
    SRW_LOG(SRW_LOG_SOUND, "Movie: decoder %u released after %d frames (%d pictures), per frame %.1f ms decoding, %.1f ms converting", (unsigned int) slot,
            d->frames, d->pictures, d->frames ? d->decode_ms / d->frames : 0.0, d->frames ? d->convert_ms / d->frames : 0.0);
    av_frame_free(&d->frame);
    av_frame_free(&d->scratch);
    av_packet_free(&d->packet);
    avcodec_free_context(&d->context);
    return 0;
}
