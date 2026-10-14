#pragma once
#include <stdint.h>

// -------------------------------------------------------------------------
//  Offline downloads -- MPEG-TS sanity scan
// -------------------------------------------------------------------------
//  HTTP can only say "the bytes the server promised arrived".  A live
//  transcode promises nothing (chunked, no length), and when ffmpeg dies part
//  way the server can still end the chunked response cleanly -- so a film
//  cut off at minute 40 looks exactly like a finished one.
//
//  This scan is fed every byte as it is written (dl_manager.cpp, at each
//  batched flush), costs a few header reads per 188-byte packet, and answers
//  two questions at completion without re-reading the file:
//    * is it a transport stream at all?  (every packet starts with 0x47)
//    * how much time does it cover?      (first -> highest video PTS)
//  Not a checksum: Jellyfin gives none, and a TS from the same request is
//  not byte-reproducible anyway.
// -------------------------------------------------------------------------

#define DL_TS_PACKET 188

typedef struct {
    uint8_t  carry[DL_TS_PACKET];  // partial packet across feeds
    int      carry_n;
    uint64_t packets;
    uint64_t sync_errors;          // packets not starting with 0x47
    bool     have_pts;
    uint64_t first_pts;            // unwrapped, 90 kHz
    uint64_t max_pts;
    uint64_t prev_raw;             // last raw 33-bit PTS, for wrap detection
    uint64_t wrap_add;
} DlTsScan;

// One packet's header facts, shared by the scan below and by local-file
// seeking (player/stream/stream_local.cpp): the same parse, not a second one.
typedef struct {
    bool     sync;          // starts with 0x47
    uint16_t pid;
    bool     pusi;          // a PES/section starts here
    bool     rai;           // adaptation-field random_access_indicator (keyframe)
    bool     video_pts;     // a video PES header with a PTS starts here
    uint64_t pts;           // raw 33-bit, valid when video_pts
} DlTsPacketInfo;
void dl_ts_packet_info(const uint8_t *pkt, DlTsPacketInfo *out);

void dl_ts_init(DlTsScan *t);
void dl_ts_feed(DlTsScan *t, const uint8_t *data, int len);
// Seconds between the first and the highest video PTS seen; -1 if no video
// PTS was found.
int64_t dl_ts_span_secs(const DlTsScan *t);

// Completion rule for a download whose whole file went through the scan:
// true = plausible.  expect_secs 0 skips the duration check.  The duration
// check is deliberately loose (90% minus 10 s): its job is catching a
// transcode that died, and a false alarm would re-download a whole film.
bool dl_ts_plausible(const DlTsScan *t, uint64_t file_bytes,
                     uint32_t expect_secs, bool *short_out);
