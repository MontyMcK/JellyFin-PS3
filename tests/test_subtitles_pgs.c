// PGS segment demux + RLE decode -- source/player/subtitles_pgs.c compiled
// straight into this test, so what's under test is the exact code the PS3
// build compiles (same convention as test_subtitles.cpp / test_media_sources
// for their respective parsers).
//
// The fixture below is a hand-built two-epoch .sup buffer: PCS+PDS+ODS+END
// for one 4x2 dialogue bitmap at t=1000ms, then a "hide" PCS (zero
// composition objects) at t=2000ms. Every field was placed against the
// layout in subtitles_pgs.h's provenance note (cross-checked with ffmpeg's
// pgssubdec.c, not reconstructed from memory), so this exercises segment
// framing, palette lookup, RLE decode (both the "N pixels of color C" and
// end-of-line codes) and the epoch index/lookup together.

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../source/player/subtitles_pgs.h"

static int failures = 0;
static void check(bool ok, const char *what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

// ---- segment builders: append a 13-byte header + payload to buf ----------
static uint8_t buf[512];
static int     n = 0;

static void put_seg(uint32_t pts_90k, uint8_t type,
                    const uint8_t *payload, int plen) {
    buf[n++] = 'P'; buf[n++] = 'G';
    buf[n++] = (uint8_t)(pts_90k >> 24); buf[n++] = (uint8_t)(pts_90k >> 16);
    buf[n++] = (uint8_t)(pts_90k >> 8);  buf[n++] = (uint8_t)(pts_90k);
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;   // DTS, unused
    buf[n++] = type;
    buf[n++] = (uint8_t)(plen >> 8); buf[n++] = (uint8_t)(plen);
    memcpy(buf + n, payload, (size_t)plen);
    n += plen;
}

int main(void) {
    // ---- Epoch 1 @ 1000ms: a 4x2 object, palette id 1, at (10,20) --------
    const uint8_t pcs1[] = {
        0x07, 0x80, 0x04, 0x38,   // video 1920x1080 (unused by the decoder)
        0x10,                     // frame rate (unused)
        0x00, 0x00,               // composition number
        0x80,                     // composition state: epoch start
        0x00,                     // palette update flag
        0x01,                     // palette id = 1
        0x01,                     // 1 composition object
        // composition object: id=1, window=0, uncropped, pos (10,20)
        0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x14,
    };
    const uint8_t pds1[] = {
        0x01, 0x00,               // palette id = 1, version 0
        0x01, 200, 128, 128, 255, // idx 1: Y=200,Cr=128,Cb=128 -> gray(200,200,200)
        0x02,  80, 128, 128, 255, // idx 2: Y=80 -> gray(80,80,80)
    };
    // width=4 height=2, object_data_length = 17 (13 RLE bytes + 4):
    //   row0: 2px color1, 2px color2, EOL
    //   row1: 4px color1, EOL
    const uint8_t ods1[] = {
        0x00, 0x01,               // object id = 1
        0x00,                     // version
        0xC0,                     // first(0x80) | last(0x40) -- single segment
        0x00, 0x00, 0x11,         // object_data_length = 17
        0x00, 0x04, 0x00, 0x02,   // width=4 height=2
        0x00, 0x82, 0x01,         // run=2 color=1
        0x00, 0x82, 0x02,         // run=2 color=2
        0x00, 0x00,               // end of line
        0x00, 0x84, 0x01,         // run=4 color=1
        0x00, 0x00,               // end of line
    };
    put_seg(90000, 0x16, pcs1, sizeof(pcs1));
    uint32_t pcs1_off = 0;   // first segment in the buffer starts at 0
    put_seg(90000, 0x14, pds1, sizeof(pds1));
    put_seg(90000, 0x15, ods1, sizeof(ods1));
    put_seg(90000, 0x80, NULL, 0);   // END

    // ---- Epoch 2 @ 2000ms: explicit hide (0 composition objects) --------
    const uint8_t pcs2[] = {
        0x07, 0x80, 0x04, 0x38, 0x10, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00,
    };
    uint32_t pcs2_off = (uint32_t)n;
    put_seg(180000, 0x16, pcs2, sizeof(pcs2));
    put_seg(180000, 0x80, NULL, 0);

    // ---- Index build + lookup ---------------------------------------------
    PgsIndex idx;
    int count = pgs_build_index(buf, n, &idx);
    check(count == 2, "index: two epochs found");
    check(idx.epoch[0].start_ms == 1000, "index: epoch0 start = 1000ms");
    check(idx.epoch[0].pcs_offset == pcs1_off, "index: epoch0 pcs offset");
    check(idx.epoch[0].has_object == true, "index: epoch0 has an object");
    check(idx.epoch[1].start_ms == 2000, "index: epoch1 start = 2000ms");
    check(idx.epoch[1].pcs_offset == pcs2_off, "index: epoch1 pcs offset");
    check(idx.epoch[1].has_object == false, "index: epoch1 is a hide");

    check(pgs_find_epoch(&idx, 500)  == -1, "lookup: before first epoch -> -1");
    check(pgs_find_epoch(&idx, 1000) == 0,  "lookup: exactly at epoch0 start");
    check(pgs_find_epoch(&idx, 1500) == 0,  "lookup: mid epoch0");
    check(pgs_find_epoch(&idx, 2500) == 1,  "lookup: past epoch1 start (hide)");

    // ---- Decode epoch 0's bitmap ------------------------------------------
    static uint32_t rgba[4 * 2];
    PgsBitmap bmp;
    bool ok = pgs_decode_epoch(buf, n, idx.epoch[0].pcs_offset,
                               rgba, 4 * 2, &bmp);
    check(ok, "decode: epoch0 succeeds");
    check(bmp.x == 10 && bmp.y == 20, "decode: composition position (10,20)");
    check(bmp.width == 4 && bmp.height == 2, "decode: dimensions 4x2");
    check(bmp.frame_w == 1920 && bmp.frame_h == 1080,
         "decode: PCS coordinate space 1920x1080");

    uint32_t gray200 = 0xFFC8C8C8u;   // A=255,R=G=B=200
    uint32_t gray80  = 0xFF505050u;   // A=255,R=G=B=80
    check(bmp.rgba[0] == gray200 && bmp.rgba[1] == gray200,
         "decode: row0 px0-1 = palette idx1 (gray200)");
    check(bmp.rgba[2] == gray80  && bmp.rgba[3] == gray80,
         "decode: row0 px2-3 = palette idx2 (gray80)");
    check(bmp.rgba[4] == gray200 && bmp.rgba[5] == gray200 &&
         bmp.rgba[6] == gray200 && bmp.rgba[7] == gray200,
         "decode: row1 all 4px = palette idx1 (gray200)");

    // Undersized caller buffer: must fail and report the size actually
    // needed, never write into rgba, and never crash.
    PgsBitmap bmp2;
    uint32_t tiny[1];
    memset(&bmp2, 0xAA, sizeof(bmp2));
    ok = pgs_decode_epoch(buf, n, idx.epoch[0].pcs_offset, tiny, 1, &bmp2);
    check(!ok, "decode: undersized buffer reports failure");
    check(bmp2.width == 4 && bmp2.height == 2,
         "decode: undersized buffer still reports the needed size");

    // The "hide" epoch has no object at all: decoding it must fail cleanly.
    PgsBitmap bmp3;
    ok = pgs_decode_epoch(buf, n, idx.epoch[1].pcs_offset, rgba, 8, &bmp3);
    check(!ok, "decode: hide epoch has nothing to decode");

    puts(failures == 0 ? "pgs subtitle decoder: synthetic ok"
                       : "pgs subtitle decoder: FAILURES");
    return failures ? 1 : 0;
}
