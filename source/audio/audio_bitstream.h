#pragma once
#include <ppu-types.h>

// Compressed output on the HDMI wire instead of LPCM.  See audio_bitstream.cpp
// for what each mode means and why it is reachable at all.
//
// Selected by a digit in /dev_hdd0/tmp/jellyfin_bitstream.txt -- a file rather
// than a menu row on purpose, so a mode that produces silence can be changed
// back over FTP without navigating a UI you cannot hear.

#ifdef __cplusplus
extern "C" {
#endif

#define BITSTREAM_OFF  0   // no cellAudioOut calls AT ALL -- the v1.0 behaviour
#define BITSTREAM_AC3  1   // Dolby Digital, encoded by the console  ("Mix")
#define BITSTREAM_DTS  2   // DTS, encoded by the console            ("Mix")
#define BITSTREAM_RAW  3   // coding type 0xff -- the long shot      ("Direct")
// Re-configure the output to 6ch LPCM, no downmixer -- i.e. make the same
// audioOutConfigure() call as the modes above but ASK FOR NOTHING NEW.  This
// exists to answer one question and is worth a mode of its own: the centre
// channel started working on the build that introduced these calls, while the
// wire stayed LPCM (audioOutGetState reported type=0), so it may be the ACT of
// configuring that fixes the routing rather than anything about AC-3.
//   OFF  vs 4  -> does calling audioOutConfigure at all matter?
//   4    vs 1  -> does asking for AC-3 specifically matter?
// If 4 is what works, that is what should ship, under an honest name.
#define BITSTREAM_LPCM 4
// LPCM again, but reached by an actual TRANSITION: configure the output wide
// (8ch) first, then back to 6ch.  Mode 4 measured as a no-op -- the output was
// already ch=6 encoder=0, so asking for ch=6 encoder=0 changed nothing and the
// dialogue stayed missing, while AC-3 (which really does change encoder 0->1)
// brings it back.  So the operative thing may be that the configuration CHANGES
// at all, not that it names AC-3.  This asks that question directly, and if the
// answer is yes it is what should ship: it ends on the coding type the wire
// actually carries instead of one the hardware has already refused.
#define BITSTREAM_LPCM_KICK 5

int  bitstream_mode(void);

// Ask for the configured mode, verify against audioOutGetState, and revert if
// the console did not take it.  When the mode is OFF this makes NO cellAudioOut
// calls whatsoever, which is what lets OFF serve as the control in the A/B
// above.
void audio_bitstream_begin(int port_channels);

// Restore whatever the output was set to before -- and nothing at all if this
// app never changed it.  Safe to call unconditionally, including when begin()
// failed part-way.
void audio_bitstream_end(void);

bool audio_bitstream_engaged(void);

// -------------------------------------------------------------------------
//  The Settings row
// -------------------------------------------------------------------------
//  What the user sees is a single On/Off called "5.1 Routing", because that
//  is what it does for them: it decides whether the console is told to treat
//  the output as 5.1, which on some chains is the difference between hearing
//  dialogue and not.  The AC-3 request behind it is an implementation detail
//  and a misleading thing to put in a menu -- nothing is encoded, and
//  begin() backs off if a console ever really would.  The numbered modes
//  above stay reachable by editing the file, for diagnosis.
bool bitstream_routing_enabled(void);
void bitstream_routing_toggle(void);
const char *bitstream_routing_label(void);

#ifdef __cplusplus
}
#endif
