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
// Apply the playback's output configuration AGAIN, if begin() applied one.
// A display mode change (the 24p switch, and its revert) re-establishes the
// HDMI link and the system puts ITS audio setting back -- 8ch when the PS3's
// own Audio Output Settings are on auto -- which undid the 6ch configure the
// soundbar's centre channel depends on (dialogue gone, 2026-09-27).
void audio_bitstream_reassert(const char *why);

// Once at startup, before anything opens an audio port: if the last session
// died with the output changed, put back what it journalled; if the output is
// on a compressed coding with no journal, reset it to LPCM.  See the .cpp.
void audio_bitstream_recover(void);

bool audio_bitstream_engaged(void);

// NOT a user setting.  It was briefly a Settings row called "5.1 Routing",
// and that was the wrong call: it is a workaround for a console quirk with an
// automatic back-off if a chain ever really would encode, so there is nothing
// for a listener to decide.  It is on unless jellyfin_bitstream.txt says
// otherwise, and that file exists for diagnosis -- it is what let all four
// candidate fixes be A/B'd over FTP without a reinstall.

#ifdef __cplusplus
}
#endif
