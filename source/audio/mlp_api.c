// Entry points onto the vendored MLP/TrueHD decoder — see mlp_api.h.
//
// This file compiles the WHOLE vendored decoder as one translation unit by
// #including the upstream .c files.  That is deliberate (PROVENANCE.md):
// upstream keeps mlpdec.c's entry points static, and the PS3 Makefile
// compiles every .c under a SOURCES directory into one flat obj/, which the
// vendored tree stays out of.  The upstream files are unmodified.

#include "mlp_api.h"

#include "mlp/ff/libavcodec/avcodec.h"

#include "mlp/mlp.c"
#include "mlp/mlpdsp.c"
#include "mlp/mlp_parse.c"
#include "mlp/mlpdec.c"
#include "mlp/ff/libavcodec/vlc.c"
#include "mlp/ff/libavutil/crc.c"
#include "mlp/ff/libavutil/reverse.c"

// One instance = the shim's AVCodecContext plus the decoder's private state.
struct mlp_dec {
    AVCodecContext   avctx;
    MLPDecodeContext ctx;
};

int mlp_api_instance_size(void) { return (int)sizeof(struct mlp_dec); }

mlp_dec *mlp_api_open(void *mem, int32_t *pcm)
{
    struct mlp_dec *d = (struct mlp_dec *)mem;
    if (!d || !pcm) return NULL;
    memset(d, 0, sizeof(*d));
    d->avctx.priv_data = &d->ctx;
    d->avctx.codec_id  = AV_CODEC_ID_TRUEHD;
    d->avctx.opaque    = pcm;     // ff_get_buffer() hands this to the decoder
    if (mlp_decode_init(&d->avctx) < 0) return NULL;
    return d;
}

void mlp_api_flush(mlp_dec *d)
{
    if (d) mlp_decode_flush(&d->avctx);
}

int mlp_api_decode(mlp_dec *d, const uint8_t *data, int size,
                   int *channels, int *sample_rate, uint64_t *ch_mask)
{
    if (!d || !data || size <= 0) return -1;
    AVPacket pkt;
    AVFrame  frame;
    int      got = 0;
    pkt.data = data;
    pkt.size = size;
    memset(&frame, 0, sizeof(frame));
    int ret = read_access_unit(&d->avctx, &frame, &got, &pkt);
    if (ret < 0) return ret;
    if (channels)    *channels    = d->avctx.ch_layout.nb_channels;
    if (sample_rate) *sample_rate = d->avctx.sample_rate;
    if (ch_mask)     *ch_mask     = d->avctx.ch_layout.u.mask;
    return got ? frame.nb_samples : 0;
}
