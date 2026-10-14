#pragma once

#include "local_probe.h"
#include "mkv_demux.h"

// -------------------------------------------------------------------------
//  A Matroska subtitle track on its way to the screen
// -------------------------------------------------------------------------
//  The sink video/mkv_ts calls with each block of the chosen subtitle track as the file is read:
//  text tracks (SRT, ASS) become cues of player/subtitles.cpp, PGS display sets go to its bitmap
//  path (local/sub_conv.h does the conversions).  Runs on the thread that reads the stream.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// ctx is the track's kind, as (void *)(intptr_t)LocalSubKind: LS_SRT, LS_ASS or LS_PGS; anything else is ignored.
void local_sub_sink(void *ctx, const MkvFrame *frame);

// Whether the kind is one the sink handles.
int local_sub_kind_playable(LocalSubKind kind);

#ifdef __cplusplus
}
#endif
