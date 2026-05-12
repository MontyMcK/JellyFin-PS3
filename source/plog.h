#pragma once

// Persistent debug log — written to /dev_hdd0/tmp/player_log.txt.
// Defined in player.cpp; included by all source files that need logging.
void plog(const char *msg);
void plog_start(void);
void plog_stop(void);
