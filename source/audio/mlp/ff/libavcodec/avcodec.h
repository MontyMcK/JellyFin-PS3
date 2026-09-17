/*
 * Minimal AVCodecContext / AVFrame shim for the vendored MLP (TrueHD)
 * decoder.  Only the fields and calls libavcodec/mlpdec.c actually touches.
 */
#ifndef AVCODEC_AVCODEC_H
#define AVCODEC_AVCODEC_H

#include <stdint.h>
#include <stddef.h>

#include "libavutil/attributes.h"
#include "libavutil/channel_layout.h"
#include "libavutil/log.h"
#include "libavutil/samplefmt.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "codec_id.h"
#include "defs.h"   /* AV_EF_* error-recognition flags */

#define AV_CODEC_CAP_DR1           (1 << 1)
#define AV_CODEC_CAP_CHANNEL_CONF  (1 << 10)

#define AV_FRAME_FLAG_KEY (1 << 1)

typedef struct AVFrame {
    uint8_t *data[8];
    uint8_t *extended_data[8];
    int      nb_samples;
    int      format;
    int      flags;
} AVFrame;

typedef struct AVPacket {
    const uint8_t *data;
    int            size;
} AVPacket;

typedef struct AVCodecContext {
    const AVClass      *av_class;
    void               *priv_data;
    enum AVCodecID      codec_id;
    int                 sample_rate;
    enum AVSampleFormat sample_fmt;
    AVChannelLayout     ch_layout;
    int                 bits_per_raw_sample;
    int                 frame_size;
    int                 profile;
    int                 err_recognition;
    int64_t             bit_rate;
    int                 flags;

/* Only the one flag the DCA core tests (upstream value). */
#define AV_CODEC_FLAG_BITEXACT (1 << 23)
    /* Host hook: the PCM destination ff_get_buffer() hands to the decoder. */
    void               *opaque;
    /* Bytes available at `opaque`.  0 = unchecked (the MLP path, which
       predates this field and hands out a single interleaved plane). */
    int                 opaque_size;
} AVCodecContext;

void avpriv_request_sample(void *avc, const char *msg, ...);
int  ff_side_data_update_matrix_encoding(AVFrame *frame, int matrix_encoding);

#endif /* AVCODEC_AVCODEC_H */

/* --- downmix-metadata surface, used ONLY by ff_dca_export_downmix_matrix ---
   That function exports downmix COEFFICIENTS to a caller; it never touches
   decoded audio, and this player maps channels to the PS3 layout itself.
   These exist so the upstream file compiles and succeeds harmlessly. */
#include "libavutil/buffer.h"
#include "libavutil/downmix_info.h"
#ifndef AV_FRAME_DATA_DOWNMIX_MATRIX
#define AV_FRAME_DATA_DOWNMIX_MATRIX 1
#endif
typedef struct AVFrameSideData AVFrameSideData;
AVBufferRef *av_buffer_create(uint8_t *data, size_t size,
                              void (*f)(void *, uint8_t *), void *opaque, int flags);
void av_buffer_unref(AVBufferRef **buf);
AVFrameSideData *av_frame_new_side_data_from_buf(AVFrame *frame, int type, AVBufferRef *buf);
void av_fast_padded_malloc(void *ptr, unsigned int *size, size_t min_size);

