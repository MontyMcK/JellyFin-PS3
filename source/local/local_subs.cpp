// A Matroska subtitle track on its way to the screen: see local_subs.h.

#include "local_subs.h"
#include "sub_conv.h"
#include "subtitles.h"

#include <stdint.h>

int local_sub_kind_playable(LocalSubKind kind) { return kind == LS_SRT || kind == LS_ASS || kind == LS_PGS; }

void local_sub_sink(void *ctx, const MkvFrame *fr) {
    const LocalSubKind kind = (LocalSubKind)(intptr_t)ctx;
    if (fr->pts_ns < 0) return;
    const u32 start_ms = (u32)(fr->pts_ns / 1000000);
    if (kind == LS_SRT || kind == LS_ASS) {
        char text[256];
        const int n = kind == LS_SRT ? sub_srt_block_text(fr->data, (int)fr->size, text, 200)
                                     : sub_ass_block_text(fr->data, (int)fr->size, text, 200);
        if (n > 0) subs_local_add_text(start_ms, fr->duration_ns ? start_ms + (u32)(fr->duration_ns / 1000000) : 0, text);
    } else if (kind == LS_PGS) {
        // a display set is a few KB; one with a full-screen object can reach tens
        static uint8_t sup[256 * 1024];
        const int n = sub_pgs_block_to_sup(fr->data, (int)fr->size, (uint32_t)((uint64_t)fr->pts_ns * 9ULL / 100000ULL), sup, (int)sizeof sup);
        if (n > 0) subs_local_add_pgs(sup, n);
    }
}
