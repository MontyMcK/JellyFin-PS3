// Single-TU wrapper over the vendored FFmpeg DTS decoder.
#include <string.h>
#include "mlp/ff/libavutil/internal.h"   /* SUINT, used by dca_xll.c */
#include "mlp/ff/libavcodec/avcodec.h"

// vlc.c, crc.c and reverse.c are NOT included here: mlp_api.c already
// compiles them, and every symbol they export is external, so this
// decoder links against that single copy instead of duplicating it.
#include "dcahd/dcadata.c"
#include "dcahd/dca.c"
#include "dcahd/dcadsp.c"
#include "dcahd/dcadct.c"
#include "dcahd/synth_filter.c"
#include "dcahd/dcahuff.c"
#include "dcahd/dcaadpcm.c"
#include "dcahd/dca_sample_rate_tab.c"
#include "dcahd/dca_exss.c"
#include "dcahd/dca_core.c"

// DTS Express (LBR) is a LOSSY low-bitrate extension, used for secondary
// audio and streaming profiles -- never for the lossless DTS-HD MA data this
// decoder exists to reach.  Excluding dca_lbr.c drops its bytestream/MDCT
// dependencies entirely; these stubs make the core decline LBR substreams
// cleanly instead of pretending to decode them.
int  ff_dca_lbr_init(DCALbrDecoder *s) { (void)s; return 0; }
void ff_dca_lbr_flush(DCALbrDecoder *s) { (void)s; }
int  ff_dca_lbr_parse(DCALbrDecoder *s, const uint8_t *data, DCAExssAsset *asset)
{ (void)s; (void)data; (void)asset; return AVERROR_INVALIDDATA; }
int  ff_dca_lbr_filter_frame(DCALbrDecoder *s, AVFrame *frame)
{ (void)s; (void)frame; return AVERROR_INVALIDDATA; }
void ff_dca_lbr_close(DCALbrDecoder *s) { (void)s; }


// --- shim implementations (see avcodec.h) --------------------------------
// The DCA decoder exports downmix metadata alongside the audio.  Nothing in
// this player consumes it, but the export must SUCCEED rather than fail:
// dca_core.c and dca_xll.c both propagate its return value, so returning an
// error would abort an otherwise good frame.
static AVDownmixMatrix s_dmix_stub;
static AVDownmixCoeff  s_dmix_coeff[2][64];

AVDownmixMatrix *av_downmix_matrix_alloc(enum AVDownmixType t, int nch, size_t *size)
{
    memset(&s_dmix_stub, 0, sizeof(s_dmix_stub));
    memset(s_dmix_coeff, 0, sizeof(s_dmix_coeff));
    s_dmix_stub.preferred_downmix_type = t;
    s_dmix_stub.nb_channels = nch;
    if (size) *size = sizeof(s_dmix_stub);
    return &s_dmix_stub;
}

AVDownmixCoeff *av_downmix_matrix_coeff(AVDownmixMatrix *dm, int out, int in)
{
    (void)dm; (void)in;
    return s_dmix_coeff[(out == 1) ? 1 : 0];
}

static AVBufferRef s_buf_stub;
AVBufferRef *av_buffer_create(uint8_t *data, size_t size,
                              void (*f)(void *, uint8_t *), void *opaque, int flags)
{
    (void)f; (void)opaque; (void)flags;
    s_buf_stub.data = data; s_buf_stub.size = size;
    return &s_buf_stub;      // static: never freed, never owns the data
}
void av_buffer_unref(AVBufferRef **buf) { if (buf) *buf = NULL; }

AVFrameSideData *av_frame_new_side_data_from_buf(AVFrame *frame, int type, AVBufferRef *buf)
{
    (void)frame; (void)type;
    return (AVFrameSideData *)buf;   // non-NULL == accepted
}

// Upstream keeps a reusable scratch buffer for the XLL PBR area. Padding is
// what lets the bit reader over-read the tail safely, so it is not optional.
void av_fast_padded_malloc(void *ptr, unsigned int *size, size_t min_size)
{
    void **p = (void **)ptr;
    if (*p && *size >= min_size) return;
    av_free(*p);
    *p = av_mallocz(min_size + AV_INPUT_BUFFER_PADDING_SIZE);
    *size = *p ? (unsigned int)min_size : 0;
}

#include "dcahd/dcadec.c"
#include "mlp/ff/libavutil/fixed_dsp.c"

// =========================================================================
//  Public entry points — see dcahd_api.h
// =========================================================================

#include "dcahd_api.h"

struct dcahd_dec {
    AVCodecContext avctx;
    DCAContext     ctx;
    uint8_t       *planes;          // caller-owned sample block
    int            planes_bytes;
    int32_t       *plane_ptr[DCAHD_MAX_CHANNELS];
    int            nb_channels;
    int            lossless;
};

int dcahd_api_instance_size(void) { return (int)sizeof(struct dcahd_dec); }

dcahd_dec *dcahd_api_open(void *mem, void *planes, int planes_bytes)
{
    struct dcahd_dec *d = (struct dcahd_dec *)mem;
    if (!d || !planes || planes_bytes < (int)DCAHD_SAMPLE_BYTES) return NULL;
    memset(d, 0, sizeof(*d));

    d->planes       = (uint8_t *)planes;
    d->planes_bytes = planes_bytes;

    d->avctx.priv_data   = &d->ctx;
    d->avctx.codec_id    = AV_CODEC_ID_DTS;
    d->avctx.opaque      = planes;        // ff_get_buffer lays planes out here
    d->avctx.opaque_size = planes_bytes;

    // Force the FIXED-POINT path.  dca_core.c selects it on this flag:
    //     if ((avctx->flags & AV_CODEC_FLAG_BITEXACT) || ... XLL ...)
    //         ret = filter_frame_fixed(...)
    // which matters for two independent reasons.  Lossless output requires
    // exact integer arithmetic, and the float path is the ONLY caller of
    // av_tx -- the MDCT framework this build stubs rather than vendors
    // (dcahd/PROVENANCE.md).  Setting this makes "the stub is never reached"
    // a property of the configuration instead of a hope about the bitstream.
    d->avctx.flags |= AV_CODEC_FLAG_BITEXACT;

    if (dcadec_init(&d->avctx) < 0) return NULL;
    return d;
}

void dcahd_api_close(dcahd_dec *d)
{
    if (d) dcadec_close(&d->avctx);
}

void dcahd_api_flush(dcahd_dec *d)
{
    if (!d) return;
    dcadec_flush(&d->avctx);
    d->nb_channels = 0;
    d->lossless    = 0;
}

// S16P -> int32 planes, in place.
//
// A 16-bit source makes the decoder emit S16P, so its planes are half as wide
// and packed half as far apart as the int32 ones every caller expects.  Both
// loops run BACKWARDS so a plane's destination never lands on bytes not yet
// read: plane i reads at i*n*2 and writes at i*n*4, and descending i means an
// already-written plane starts above where any later one ends.
static void dcahd_widen_s16p(struct dcahd_dec *d, int nch, int n)
{
    for (int i = nch - 1; i >= 0; i--) {
        const int16_t *src = (const int16_t *)(d->planes + (size_t)i * n * 2);
        int32_t       *dst = (int32_t *)(d->planes + (size_t)i * n * 4);
        for (int k = n - 1; k >= 0; k--)
            dst[k] = (int32_t)src[k] << 16;
    }
}

int dcahd_api_decode(dcahd_dec *d, const uint8_t *data, int size,
                     int *channels, int *sample_rate, uint64_t *ch_mask,
                     int *lossless)
{
    if (!d || !data || size <= 0) return -1;

    AVPacket pkt;
    AVFrame  frame;
    int      got = 0;

    pkt.data = (uint8_t *)data;
    pkt.size = size;
    memset(&frame, 0, sizeof(frame));

    int ret = dcadec_decode_frame(&d->avctx, &frame, &got, &pkt);
    if (ret < 0) return ret;
    if (!got)   return 0;

    const int nch = d->avctx.ch_layout.nb_channels;
    const int n   = frame.nb_samples;
    if (nch < 1 || nch > DCAHD_MAX_CHANNELS || n <= 0) return -1;

    if (frame.format == AV_SAMPLE_FMT_S16P)
        dcahd_widen_s16p(d, nch, n);

    for (int i = 0; i < nch; i++)
        d->plane_ptr[i] = (int32_t *)(d->planes + (size_t)i * n * 4);
    d->nb_channels = nch;

    // DCA_PACKET_XLL means the samples came from the lossless extension
    // rather than the lossy core -- the whole reason this decoder exists.
    d->lossless = (d->ctx.packet & DCA_PACKET_XLL) ? 1 : 0;

    if (channels)    *channels    = nch;
    if (sample_rate) *sample_rate = d->avctx.sample_rate;
    if (ch_mask)     *ch_mask     = d->avctx.ch_layout.u.mask;
    if (lossless)    *lossless    = d->lossless;
    return n;
}

const int32_t *const *dcahd_api_planes(const dcahd_dec *d)
{
    return d ? (const int32_t *const *)d->plane_ptr : NULL;
}
