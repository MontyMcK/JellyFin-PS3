#pragma once

// Opt-in, one-shot capture of the firmware code behind cellVideoOutConfigure2.
//
// Runs only when /dev_hdd0/tmp/jellyfin_fwcapture.txt exists.  Loads
// cellSysutilAvconfExt, reads where the loader put Configure2 and friends,
// and writes the loaded (already decrypted) segments of that module and of
// libsysutil to /dev_hdd0/tmp/jf_fwcap_*.bin plus an index.  It never calls
// any function it binds.  Call after plog is up and BEFORE the big heap
// reservations -- loading the module needs a little memory, and it is
// unloaded again before returning.
void avconf_capture_run(void);
