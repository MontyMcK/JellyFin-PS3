#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  H.264 sequence parameter sets: what a local file's video is
// -------------------------------------------------------------------------
//  Pure (no PS3 headers; tests/test_h264_sps.cpp compiles this file).  Used
//  to say, before anything reaches the decoder, whether the console can play a
//  video stream at all, and what its frame rate is.  The PS3's decoder takes
//  8-bit 4:2:0 H.264 up to Level 4.2 and 1920x1080; HEVC, 10-bit, 4:2:2 and
//  anything bigger must be refused with a reason, never handed to it.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      profile_idc;        // 66 baseline, 77 main, 88 extended, 100 high, 110/122/244 high 10 / 4:2:2 / 4:4:4 ...
    int      level_idc;          // 41 = level 4.1 ("level_idc / 10")
    int      chroma_format_idc;  // 1 = 4:2:0
    int      bit_depth;          // luma
    int      width, height;      // coded size after cropping
    bool     frame_mbs_only;     // false = interlaced coding is possible
    int      max_num_ref_frames;
    bool     has_timing;         // the VUI says how often pictures come
    uint32_t num_units_in_tick, time_scale;
    double   fps;                // time_scale / (2 * num_units_in_tick); 0 when has_timing is false
} H264SpsInfo;

// Parses one SPS NAL unit.  `nal` starts at the NAL header byte (type 7), without a start code or a
// length prefix; emulation-prevention bytes are removed here.  False when it is not a well-formed SPS.
bool h264_parse_sps(const uint8_t *nal, int len, H264SpsInfo *out);

// Can the PS3's decoder play it?  False with a short reason ("10-bit", "Level 5.1", "4K") for the
// file row.  `reason` may be NULL.
bool h264_playable(const H264SpsInfo *sps, char *reason, int reason_cap);

#ifdef __cplusplus
}
#endif
