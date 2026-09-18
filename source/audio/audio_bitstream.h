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

#define BITSTREAM_OFF  0   // LPCM, the behaviour this app has always had
#define BITSTREAM_AC3  1   // Dolby Digital, encoded by the console  ("Mix")
#define BITSTREAM_DTS  2   // DTS, encoded by the console            ("Mix")
#define BITSTREAM_RAW  3   // coding type 0xff -- the long shot      ("Direct")

int  bitstream_mode(void);

// Ask for the configured mode, verify by read-back, and revert if the console
// did not take it.  Safe to call when the mode is OFF: it does nothing.
void audio_bitstream_begin(int port_channels);

// Restore whatever the output was set to before.  Safe to call unconditionally,
// including when begin() failed part-way.
void audio_bitstream_end(void);

bool audio_bitstream_engaged(void);

#ifdef __cplusplus
}
#endif
