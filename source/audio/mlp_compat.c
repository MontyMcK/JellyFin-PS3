// Runtime half of the compatibility layer the vendored MLP/TrueHD decoder
// needs (source/audio/mlp/, see PROVENANCE.md there): logging, allocation,
// the few AVChannelLayout helpers it calls, and a buffer hand-out that points
// the decoder at a caller-owned PCM block.  Everything here replaces a piece
// of libavutil/libavcodec that would otherwise drag in the whole library.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mlp/ff/libavcodec/avcodec.h"
#include "mlp/ff/libavcodec/decode.h"
#include "mlp/ff/libavutil/channel_layout.h"
#include "mlp/ff/libavutil/mem.h"
#include "mlp/ff/libavutil/log.h"

/* ---- logging ---------------------------------------------------------- */

static int s_quiet = 1;

void av_log(void *avcl, int level, const char *fmt, ...)
{
    (void)avcl;
    if (s_quiet && level > AV_LOG_ERROR) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void av_log_set_quiet(int q) { s_quiet = q; }

const char *av_default_item_name(void *ptr) { (void)ptr; return "mlp"; }

void avpriv_request_sample(void *avc, const char *msg, ...)
{
    (void)avc; (void)msg;
}

int ff_side_data_update_matrix_encoding(AVFrame *frame, int matrix_encoding)
{
    (void)frame; (void)matrix_encoding;
    return 0;   /* the host does not consume Dolby matrix side data */
}

/* ---- allocation ------------------------------------------------------- */

void *av_malloc(size_t size)              { return malloc(size ? size : 1); }
void *av_mallocz(size_t size)             { void *p = malloc(size ? size : 1); if (p) memset(p, 0, size); return p; }
void *av_calloc(size_t n, size_t size)    { return calloc(n ? n : 1, size ? size : 1); }
void  av_free(void *p)                    { free(p); }
void  av_freep(void *p)                   { void **pp = (void **)p; free(*pp); *pp = NULL; }
void *av_realloc(void *p, size_t size)    { return realloc(p, size ? size : 1); }

/* ---- channel layout --------------------------------------------------- */
/* Only native-order (mask) layouts are ever built by the decoder. */

int av_channel_layout_from_mask(AVChannelLayout *ch, uint64_t mask)
{
    if (!mask) return -1;
    memset(ch, 0, sizeof(*ch));
    ch->order       = AV_CHANNEL_ORDER_NATIVE;
    ch->nb_channels = av_popcount64(mask);
    ch->u.mask      = mask;
    return 0;
}

void av_channel_layout_uninit(AVChannelLayout *ch)
{
    memset(ch, 0, sizeof(*ch));
}

int av_channel_layout_copy(AVChannelLayout *dst, const AVChannelLayout *src)
{
    *dst = *src;
    return 0;
}

int av_channel_layout_check(const AVChannelLayout *ch)
{
    return ch && ch->nb_channels > 0 &&
           ch->order == AV_CHANNEL_ORDER_NATIVE && ch->u.mask != 0;
}

int av_channel_layout_compare(const AVChannelLayout *a, const AVChannelLayout *b)
{
    if (!av_channel_layout_check(a) || !av_channel_layout_check(b))
        return 1;
    return a->u.mask != b->u.mask;
}

int av_channel_layout_index_from_channel(const AVChannelLayout *ch,
                                         enum AVChannel channel)
{
    if (!av_channel_layout_check(ch) || channel < 0 || channel >= 64)
        return -1;
    uint64_t bit = UINT64_C(1) << channel;
    if (!(ch->u.mask & bit)) return -1;
    return av_popcount64(ch->u.mask & (bit - 1));
}

uint64_t av_channel_layout_subset(const AVChannelLayout *ch, uint64_t mask)
{
    if (!av_channel_layout_check(ch)) return 0;
    return ch->u.mask & mask;
}

/* ---- decoded-frame buffer -------------------------------------------- */
/*
 * The decoder packs its output interleaved into frame->data[0].  avctx->opaque
 * points at a host-owned block big enough for the largest access unit; the
 * host reads it back as soon as the decode call returns, so no ownership or
 * reference counting is involved.
 */
int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    (void)flags;
    if (!avctx->opaque) return -1;
    uint8_t *base = (uint8_t *)avctx->opaque;
    frame->data[0]          = base;
    frame->extended_data[0] = base;
    frame->format           = avctx->sample_fmt;

    /* PLANAR output needs one pointer per channel.  The vendored DTS decoder
     * (source/audio/dcahd/) emits S32P/S16P -- one plane per speaker -- while
     * MLP emits interleaved S16/S32 and takes the early return below, leaving
     * its behaviour byte-for-byte what it was. */
    int bps;
    switch (avctx->sample_fmt) {
    case AV_SAMPLE_FMT_S16P: bps = 2; break;
    case AV_SAMPLE_FMT_S32P:
    case AV_SAMPLE_FMT_FLTP: bps = 4; break;
    default: return 0;                      /* interleaved: one plane is all */
    }

    const int nch = avctx->ch_layout.nb_channels;
    if (nch < 1 || nch > 8 || frame->nb_samples <= 0) return -1;

    /* Refuse rather than run off the end of a caller-owned block.  The frame
     * size is chosen by the BITSTREAM, so a malformed or unexpected stream can
     * ask for more than the host reserved. */
    const size_t need = (size_t)nch * (size_t)frame->nb_samples * (size_t)bps;
    if (avctx->opaque_size > 0 && need > (size_t)avctx->opaque_size) return -1;

    for (int i = 1; i < nch; i++)
        frame->extended_data[i] = frame->data[i] =
            base + (size_t)i * (size_t)frame->nb_samples * (size_t)bps;
    return 0;
}

/* vlc.c's allocation helpers (libavutil/mem.c in ffmpeg). */
void *av_malloc_array(size_t nmemb, size_t size)
{
    if (size && nmemb > (size_t)-1 / size) return NULL;
    return av_malloc(nmemb * size);
}

void *av_realloc_f(void *ptr, size_t nmemb, size_t size)
{
    if (size && nmemb > (size_t)-1 / size) { av_free(ptr); return NULL; }
    void *r = av_realloc(ptr, nmemb * size);
    if (!r) av_free(ptr);
    return r;
}
