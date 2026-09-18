#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Dialogue / centre-channel handling
// -------------------------------------------------------------------------
//  Dialogue in a film mix lives almost entirely in the CENTRE channel, which
//  the decoders place in PS3 CellAudio slot 2 (0=FL 1=FR 2=FC 3=LFE 4=SL
//  5=SR 6=BL 7=BR — confirmed against RPCS3's cellAudioAdd6chData, which
//  mixes a 6ch source as L,R,C,LFE,rearL,rearR into an 8ch port).
//
//  Two independent things can make that channel inaudible, and only the
//  listener's hardware can tell them apart:
//
//    1. The AV chain never renders slot 2 — the sink is taking only the
//       first two channels of the 8-wide LPCM frame (an ARC link, a bar in
//       a stereo/PCM-2.0 mode, or "Linear PCM 7.1 Ch." ticked in XMB Sound
//       Settings for a sink that cannot actually take 8 channels).  The
//       decode is perfect and the speaker is simply never fed.
//
//    2. The centre is rendered but sits too low.  None of the three surround
//       decoders here applies dynamic range compression — liba52's DRC is
//       switched off deliberately (adec_ac3.cpp), libdca's dca_dynrng() is
//       the documented desync trap and is never called, and the MLP decoder
//       has no DRC at all.  Full cinema dynamic range on a compact system
//       puts dialogue well below effects and music.
//
//  The boosts fix (2).  They are not applied by default: NORMAL is the exact
//  behaviour that shipped, bit-for-bit.
//
//  PHANTOM was the fix for (1) -- fold centre into L/R and mute slot 2 -- and
//  is now RETIRED.  It existed because slot 2 never reached a speaker on the
//  author's chain; as of 2026-09-18 it does, with Dialogue on NORMAL, so
//  PHANTOM would now throw away a working centre channel to solve a problem
//  that is gone.  STEREO stays, because a chain that carries only the front
//  pair (a bar behind a TV's ARC link) is a real configuration and PHANTOM
//  would rescue its dialogue while still losing the surrounds.
//
//  Retired rather than deleted: the mode is PERSISTED as a digit, so the
//  values must not shift under a saved file.  centermix_sanitize() maps it
//  onto NORMAL, which is where someone who needed PHANTOM now wants to be.
//
//  STEREO is for the chain that carries ONLY the front pair -- a soundbar
//  with no HDMI input, reached through a TV's ARC link, is the common case:
//  the TV hands on two channels and everything in slots 2..7 is discarded
//  before it reaches a speaker.  PHANTOM alone would rescue the dialogue but
//  still lose the surrounds, so STEREO folds centre AND both surround pairs
//  into L/R (a LoRo downmix at -3 dB, with 3 dB of headroom against the sum)
//  and mutes the rest.  Nothing in the mix is thrown away.  The LFE is left
//  out on purpose: the bar runs its own crossover to its subwoofer, and
//  folding a full-range LFE into the mains only muddies them.
//
//  Applied centrally in audio.cpp on the staged source frame, immediately
//  before it is placed in the DMA block, so it covers every codec at once
//  (AC-3, DTS/DTS-HD, TrueHD) instead of being repeated in three maps.
//  A stereo program has no centre and is never touched.
//
//  Persisted as "0".."4" in the app data dir beside the other settings.

typedef enum {
    CENTER_NORMAL  = 0,   // untouched — the shipped path
    CENTER_P3      = 1,   // centre +3 dB
    CENTER_P6      = 2,   // centre +6 dB
    CENTER_P10     = 3,   // centre +10 dB
    CENTER_PHANTOM = 4,   // RETIRED -- fold centre into L/R, mute slot 2
    CENTER_STEREO  = 5,   // full LoRo downmix into L/R, mute every other slot
    CENTER_COUNT   = 6,
} center_mode_t;

// Offered modes, in the order the Dialogue row cycles them.  PHANTOM is
// absent; the enum keeps its value so saved digits do not shift.
extern const center_mode_t CENTERMIX_ORDER[];
extern const int           CENTERMIX_ORDER_N;

// Map a persisted value onto one that is still offered.
center_mode_t centermix_sanitize(int v);

void          centermix_load(void);
void          centermix_save(void);
center_mode_t centermix_get(void);
void          centermix_set(center_mode_t m);
void          centermix_cycle(void);
const char   *centermix_label(void);

// True when the mode would change the samples — lets the caller skip the
// whole pass on the default setting.
bool centermix_active(void);

// Apply the current mode in place to `frames` interleaved float frames of
// `ch` channels.  No-op when ch < 3 (no centre channel exists) or the mode
// is NORMAL.  Output is clamped to [-1,1]: a boost on a mix that is already
// near full scale would otherwise wrap in the DAC.
void centermix_apply(float *frames, int n, int ch);
