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

// Screens (each blocks until the user navigates away)
int  do_login(void);
void show_library_browser(void);
void show_search(void);
void show_main_menu(void);
