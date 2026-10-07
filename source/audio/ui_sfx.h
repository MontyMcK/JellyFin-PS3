#pragma once
// XMB sound effects for the menus -- the console's own cursor / OK / back /
// option sounds, read at startup out of /dev_flash/vsh/resource/system_plugin.rco.
//
// Nothing of Sony's ships in this app: the sounds come off the console the app
// is running on, so a console whose flash cannot be read (an emulator, a
// future firmware with a different RCO) is simply silent.
//
// The effects get their own stereo CellAudio port, mixed by the system under
// whatever else is playing (the music player's port included).  Video playback
// suspends it -- ui_sfx_suspend() before the player's audio_open(), and
// audio_close() resumes it -- so a bitstream or 8-channel program never has a
// second port mixed into it.
//
// jellyfin_uisfx.txt containing "0" turns the effects off; missing = on.

enum UiSfx {
    SFX_CURSOR,   // d-pad / left stick step, L1 / R1
    SFX_DECIDE,   // Cross
    SFX_CANCEL,   // Circle
    SFX_OPTION,   // Triangle
    SFX_COUNT
};

// Load the sounds and open the port.  Call once, after plog is up.
void ui_sfx_init(void);

// Queue a sound.  Cheap, main-thread, and a no-op while suspended, disabled
// or before init -- so input code can call it unconditionally.
void ui_sfx_play(int sfx);

// Close / reopen the port around video playback.  Both are idempotent.
void ui_sfx_suspend(void);
void ui_sfx_resume(void);
