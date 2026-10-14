#pragma once
// Host stand-in for api/jellyfin_api.h, with what music/music_player.cpp calls.  Nothing here talks to a server:
// tests/test_music_engine.cpp defines the functions, and a queue of files never reaches them.
#include <ppu-types.h>

#define RESPONSE_SIZE 4096
extern char g_server[256];
extern char g_token[256];
extern char g_userid[64];
extern char responseBuffer[RESPONSE_SIZE];
#define HTTP_GET 0

const char *jf_device_id(void);
bool jellyfin_get_play_session_id(const char *item_id, char *out, int cap, unsigned *total_secs);
void jellyfin_report_playing(const char *item_id, const char *session, u64 ticks);
void jellyfin_report_stopped(const char *item_id, const char *session, u64 ticks);
void jellyfin_report_progress_async(const char *item_id, const char *session, u64 ticks, bool paused);
void jellyfin_stop_transcode(const char *session);
void jellyfin_report_init(void);
void jellyfin_report_flush(void);
int  http_request(int method, const char *url, const char *body, const char *token, char *resp, int cap);
