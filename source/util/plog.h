#pragma once

// Persistent debug log — written to /dev_hdd0/tmp/player_log.txt.
// Defined in util/plog.cpp; included by all source files that need logging.
// Logging is OFF by default; plog() discards messages while disabled.
void plog(const char *msg);
void plog_start(void);
void plog_stop(void);

// User toggle, persisted in /dev_hdd0/tmp/jellyfin_settings.txt (survives
// logout — jellyfin_logout only removes jellyfin_config.txt).
bool plog_enabled(void);
void plog_set_enabled(bool on);  // start/stop the log thread + persist choice
void plog_load_setting(void);    // call once at boot: load choice, start if on
