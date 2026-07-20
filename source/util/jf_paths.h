#pragma once

// Absolute path for an app-data file (login, device id, settings).
//
// Location is target-specific (see jf_paths.cpp): on the real PS3 it is
// /dev_hdd0/tmp — the one place writable at runtime, since the installed game
// USRDIR is not reliably writable while the title runs, so the login silently
// failed to save there.  Under RPCS3 it stays in the game USRDIR (writable and
// persistent there), unchanged.
const char *jf_data_path(const char *name);
