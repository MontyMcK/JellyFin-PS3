#pragma once

// Version built into this binary.  Keep in step with the GitHub release tag
// (leading 'v'/'V' is ignored when comparing, so "2.0-beta" matches
// "V2.0-beta").  Bump this when cutting a release.
#define APP_VERSION "2.2-beta"

// One-shot background check of the project's GitHub releases.  Runs entirely
// on its own low-priority thread: start it once after http_init() succeeds
// and it never blocks or slows the caller.  Every failure (no network, DNS,
// TLS handshake, timeout, bad JSON) is treated as "no update available".
// Each run traces its steps and the raw API response to
// /dev_hdd0/tmp/update_detection.txt regardless of the Debug Logging setting.
void update_check_start(void);

// Join the worker before tearing the network down at exit.  Safe to call
// even if update_check_start() was never called.
void update_check_shutdown(void);

// True once the check has finished AND found a release newer than
// APP_VERSION; copies the release tag (e.g. "V2.1") into out.  Poll it from
// a render loop — it never blocks.
bool update_check_result(char *out, int out_size);
