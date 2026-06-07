#pragma once
#include "http.h"

#define JF_MAX   100
#define JF_PAGE  18

typedef struct {
    char id[64];
    char name[128];
    char type[32];
} JFItem;

// Global session state (defined in jellyfin_api.cpp)
extern char g_server[256];
extern char g_username[64];
extern char g_token[256];
extern char g_userid[64];
extern char responseBuffer[RESPONSE_SIZE];

// JSON helpers
int  json_get_string(const char *json, const char *key, char *out, int out_size);
void url_encode_query(const char *in, char *out, int out_size);

// Item helpers
bool is_container(const char *type);
int  parse_jf_items(const char *json, JFItem *arr, int max);

// Config
void save_config(void);
int  load_config(void);

// Full per-item detail (populated by jellyfin_fetch_item_detail)
typedef struct {
    char overview[1024];       // Plot summary
    char tagline[256];         // "How fast do you like it?"
    char official_rating[16];  // "NZ-M" / "PG-13" / etc.
    char community_rating[8];  // "6.5"
    char critic_rating[8];     // "37%"
    char video_info[64];       // "1080p H264 SDR"
    char audio_info[128];      // "English EAC3 5.1"
    char genres[128];          // "Action, Crime, Thriller"
    char studios[256];         // "Universal Pictures, Original Film"
} XMBItemDetail;

bool jellyfin_fetch_item_detail(const char *item_id, XMBItemDetail *out);

// Playback session
// POST /Users/{userId}/Items/{item_id}/PlaybackInfo with a PS3 device profile.
// Extracts PlaySessionId from the response.
// Returns true and fills out_session_id on success; false on any failure.
bool jellyfin_get_play_session_id(const char *item_id,
                                   char *out_session_id, int out_len);

// Log out of the current session.  Best-effort notifies the server
// (POST /Sessions/Logout), clears the in-memory credentials, and removes the
// saved config so the next launch returns to the login screen.  The server URL
// is preserved so the user only needs to re-enter their credentials.
void jellyfin_logout(void);

// Screens (each blocks until the user navigates away)
int  do_login(void);
void show_library_browser(void);
void show_search(void);
void show_main_menu(void);
