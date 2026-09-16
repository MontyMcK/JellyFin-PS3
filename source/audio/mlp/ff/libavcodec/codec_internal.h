#pragma once
#include "avcodec.h"
#define FF_CODEC_DECODE_CB(x) .cb.decode = (x)
#define CODEC_LONG_NAME(x) .p.long_name = (x)
typedef struct FFCodec { int dummy; } FFCodec;
