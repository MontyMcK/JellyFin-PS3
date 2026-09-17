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
