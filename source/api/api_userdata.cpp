// Favourite and played: see api_userdata.h.

#include <stdio.h>

#include "api_userdata.h"
#include "userdata_request.h"
#include "jellyfin_api.h"
#include "plog.h"

static bool send_userdata(const UserDataRequest *r, const char *what, const char *id) {
    static char resp[2048];
    resp[0] = '\0';
    const int st = http_request(r->method, r->url, "", g_token, resp, sizeof resp);
    char msg[160];
    snprintf(msg, sizeof msg, "userdata: %s %.40s -> %d", what, id, st);
    plog(msg);
    return st >= 200 && st < 300;
}

bool jf_set_favourite(const char *item_id, bool favourite) {
    UserDataRequest r;
    if (!userdata_favourite_request(&r, g_server, g_userid, item_id, favourite)) return false;
    return send_userdata(&r, favourite ? "favourite" : "unfavourite", item_id);
}

bool jf_set_played(const char *item_id, bool played) {
    UserDataRequest r;
    if (!userdata_played_request(&r, g_server, g_userid, item_id, played)) return false;
    return send_userdata(&r, played ? "played" : "unplayed", item_id);
}
