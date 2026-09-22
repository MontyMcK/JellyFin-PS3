#pragma once
#include "avcodec.h"
int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags);
