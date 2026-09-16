#pragma once

// Interleave one libdca output block (256 planar samples per channel) into
// PS3 CellAudio channel order.  Pure C, no PS3 headers — the same file is
// compiled on the host by tests/test_dts_map.c and tests/test_dts_decode.c,
// because a wrong channel map is inaudible in a build log and unfixable from
// a .pkg.  Mirrors ac3_map.h; the two decoders differ in plane order, which
// is exactly why each gets its own map file and its own tests.
//
// planes:    dca_samples(state) after one dca_block() — planar layout
//            determined by the GRANTED flags dca_frame() wrote back.
// dca_flags: those granted flags (DCA_* channel config, possibly | DCA_LFE).
// out:       256 interleaved frames, out_ch floats each.
// out_ch:    6 (PS3 5.1 program: FL FR FC LFE SL SR) or 2 (FL FR).
//            Unused slots are written as 0.0f every call.

#ifdef __cplusplus
extern "C" {
#endif

void dts_map_block(const float *planes, int dca_flags, float *out, int out_ch);

#ifdef __cplusplus
}
#endif
