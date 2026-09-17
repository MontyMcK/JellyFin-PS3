#pragma once
#include "avcodec.h"

/* dcadec.c ends with an FFCodec registration block that upstream does NOT
   guard with #if CONFIG_DCA_DECODER (unlike mlpdec.c, which is why the MLP
   shim could get away with a one-field stub).  Nothing here is ever
   registered or called -- dcahd_api.c invokes the static entry points
   directly -- so these declarations only have to COMPILE.  The fields
   mirror the initializer at the bottom of dcadec.c. */
typedef struct FFCodecP {
    const char    *name;
    const char    *long_name;
    int            type;
    int            id;
    int            capabilities;
    const AVClass *priv_class;
    const void    *profiles;
} FFCodecP;

typedef struct FFCodec {
    FFCodecP p;
    int      priv_data_size;
    int    (*init)(AVCodecContext *);
    union { int (*decode)(AVCodecContext *, AVFrame *, int *, AVPacket *); } cb;
    int    (*close)(AVCodecContext *);
    void   (*flush)(AVCodecContext *);
    int      caps_internal;
} FFCodec;

#define FF_CODEC_DECODE_CB(x) .cb.decode = (x)
#define CODEC_LONG_NAME(x)    .p.long_name = (x)
#ifndef FF_CODEC_CAP_INIT_CLEANUP
#define FF_CODEC_CAP_INIT_CLEANUP 0
#endif
#ifndef AV_CODEC_CAP_DR1
#define AV_CODEC_CAP_DR1 0
#endif
#ifndef AV_CODEC_CAP_CHANNEL_CONF
#define AV_CODEC_CAP_CHANNEL_CONF 0
#endif
#ifndef NULL_IF_CONFIG_SMALL
#define NULL_IF_CONFIG_SMALL(x) (x)
#endif
