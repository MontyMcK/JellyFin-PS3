#pragma once

// Interleave one liba52 output block (256 planar samples per channel) into
// PS3 CellAudio channel order.  Pure C, no PS3 headers — the same file is
// compiled on the host by tests/test_ac3_map.c, because a wrong channel map
// is inaudible in a build log and unfixable from a .pkg.
//
// planes:    a52_samples(state) after one a52_block() — planar layout
//            determined by the GRANTED flags a52_frame() wrote back.
// a52_flags: those granted flags (A52_* channel config, possibly | A52_LFE).
// out:       256 interleaved frames, out_ch floats each.
// out_ch:    6 (PS3 5.1 program: FL FR FC LFE SL SR) or 2 (FL FR).
//            Unused slots are written as 0.0f every call.

#ifdef __cplusplus
extern "C" {
#endif

void ac3_map_block(const float *planes, int a52_flags, float *out, int out_ch);

#ifdef __cplusplus
}
#endif
