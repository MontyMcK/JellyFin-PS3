#pragma once

// Diagnostic GUI / thumbnail-cache log — /dev_hdd0/tmp/GUIlog.txt.
// Gated by the SAME user logging toggle as plog (plog_enabled()): writes
// nothing at all while logging is off, so it costs nothing in normal use.
// Kept in a separate file so thumbnail-cache tracing doesn't drown in the
// player/decoder stream in player_log.txt.
void glog(const char *msg);
void glogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
