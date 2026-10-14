#pragma once
#include <stdint.h>

// -------------------------------------------------------------------------
//  Matroska subtitle blocks -> what the player's subtitle renderers take
// -------------------------------------------------------------------------
//  A subtitle track of a file on a drive is read while the film plays (video/mkv_ts hands each
//  block to a sink), not fetched whole from a server: the blocks are turned here into the lines
//  of text player/subtitles.cpp draws, or into the .sup segments its PGS renderer decodes.
//
//  Pure (no PS3 headers): tests/test_sub_conv.cpp compiles this file.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// The text of an S_TEXT/UTF8 block: <tags> and {tags} removed, CRs dropped, lines joined with '\n', trailing
// blank space cut, at most cap-1 bytes (cut between characters).  Returns the length; 0 when nothing is left to show.
int sub_srt_block_text(const uint8_t *data, int len, char *out, int cap);

// The text of an S_TEXT/ASS block, whose payload is the event without its times:
//   ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
// {override blocks} are dropped (and the vector drawing of {\p1}...{\p0}), \N and \n are line breaks, \h a space.
// Same limits and return as sub_srt_block_text.
int sub_ass_block_text(const uint8_t *data, int len, char *out, int cap);

// A PGS block (S_HDMV/PGS): segments of type (1), size (2, big-endian) and payload, without the "PG" headers a .sup
// stream carries.  Writes them as .sup segments ("PG", PTS, DTS 0, type, size, payload) with the block's time in
// 90 kHz ticks.  Returns the bytes written; 0 for a block with no whole segment; -1 when out (cap bytes) is too small.
int sub_pgs_block_to_sup(const uint8_t *data, int len, uint32_t pts90, uint8_t *out, int cap);

#ifdef __cplusplus
}
#endif
