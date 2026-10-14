#pragma once

// -------------------------------------------------------------------------
//  Channels -> the PS3's: a decoder's channels, in CellAudio order
// -------------------------------------------------------------------------
//  AAC (libfaad), FLAC and PCM hand over interleaved floats in their own channel order.  This turns
//  them into the port's frame: 0=FL 1=FR 2=FC 3=LFE 4=SL 5=SR (six wide, a 5.1 program) or FL FR
//  (two wide).  The caller says where each channel belongs with the position codes below (libfaad's,
//  which it reports per channel); chan_wave_positions gives them for the order WAVE, FLAC and Matroska
//  PCM use.
//
//  Pure (no PS3 headers): tests/test_aac.cpp and tests/test_chan_map.cpp compile this file.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// libfaad's position codes (neaacdec.h), repeated so this file needs no libfaad.
enum {
    CH_POS_UNKNOWN = 0, CH_POS_FRONT_CENTER = 1, CH_POS_FRONT_LEFT = 2, CH_POS_FRONT_RIGHT = 3,
    CH_POS_SIDE_LEFT = 4, CH_POS_SIDE_RIGHT = 5, CH_POS_BACK_LEFT = 6, CH_POS_BACK_RIGHT = 7,
    CH_POS_BACK_CENTER = 8, CH_POS_LFE = 9
};

// The positions of the channels of a WAVE / FLAC / Matroska PCM stream of `ch` channels (1..8):
//   1 C | 2 L R | 3 L R C | 4 L R BL BR | 5 L R C BL BR | 6 L R C LFE BL BR | 7 L R C LFE BC SL SR |
//   8 L R C LFE BL BR SL SR.  More than 8, or 0: all unknown.  pos must hold `ch` entries.
void chan_wave_positions(int ch, unsigned char *pos);

// Mixes `frames` interleaved frames of `ch` channels (positions pos[0..ch)) into out_ch-wide frames
// (6 or 2).  Six wide: each channel goes to its own slot; side and back channels share the surround
// slots (a 7.1 program's second pair is folded in at -3 dB), a back centre goes to both surrounds at
// -3 dB, mono goes to the centre.  Two wide: the front pair, plus the centre and the surrounds at -3 dB,
// all scaled by 1/(1+0.707+0.707) as the AC-3 downmix is so the sum cannot clip (a plain stereo or mono
// file is left alone, mono to both sides), and no LFE.  A channel of unknown position is left out; if
// every position is unknown the channels are taken in the usual order (L R C LFE SL SR).
void chan_map_frames(const float *in, int frames, int ch, const unsigned char *pos, float *out, int out_ch);

#ifdef __cplusplus
}
#endif
