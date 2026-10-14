#pragma once

// Settings > Send Log to Server.  Uploads the tail of player_log.txt (plus a
// short header: app version, display, server reply) to the user's own Jellyfin
// server via POST /ClientLog/Document.  Jellyfin stores it in its log
// directory as upload_PS3_<date>.log, so a tester only has to say "sent" and
// the log is under Dashboard > Logs.  Runs on its own thread; never blocks UI.
enum { LOGUP_IDLE, LOGUP_SENDING, LOGUP_SENT, LOGUP_FAILED, LOGUP_NOLOG };

// False while a send is already running.
bool log_upload_start(void);
int  log_upload_state(void);
// Last HTTP status (or -1) for the failure hint.
int  log_upload_status(void);
