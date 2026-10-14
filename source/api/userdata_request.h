#pragma once
#include <stdbool.h>
#include <stdio.h>
#include "http.h"          // HTTP_POST / HTTP_DELETE

// -------------------------------------------------------------------------
//  The requests that change a user's data on an item
// -------------------------------------------------------------------------
//  Pure: builds the method and URL, touches nothing else, so
//  tests/test_userdata.cpp compiles this header and checks them.  The calls
//  themselves are in api_userdata.cpp.
//
//    favourite  POST    {server}/Users/{user}/FavoriteItems/{item}
//    unfavourite DELETE same path
//    played     POST    {server}/Users/{user}/PlayedItems/{item}
//    unplayed   DELETE  same path
//
//  The item id goes into the path, so it is accepted only if it is a plain
//  Jellyfin id (letters, digits, '-', '_'): anything else could rewrite the
//  path or add a query.

typedef struct {
    int  method;      // HTTP_POST or HTTP_DELETE
    char url[512];
} UserDataRequest;

static inline bool userdata_id_ok(const char *id) {
    if (!id || !id[0]) return false;
    int n = 0;
    for (const char *p = id; *p; p++, n++) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok || n >= 63) return false;
    }
    return true;
}

static inline bool userdata_request(UserDataRequest *r, const char *server,
                                    const char *user_id, const char *collection,
                                    const char *item_id, bool set) {
    if (!server || !server[0] || !userdata_id_ok(user_id) || !userdata_id_ok(item_id))
        return false;
    const int n = snprintf(r->url, sizeof r->url, "%s/Users/%s/%s/%s",
                           server, user_id, collection, item_id);
    if (n <= 0 || n >= (int)sizeof r->url) return false;
    r->method = set ? HTTP_POST : HTTP_DELETE;
    return true;
}

static inline bool userdata_favourite_request(UserDataRequest *r, const char *server,
                                              const char *user_id, const char *item_id,
                                              bool favourite) {
    return userdata_request(r, server, user_id, "FavoriteItems", item_id, favourite);
}

static inline bool userdata_played_request(UserDataRequest *r, const char *server,
                                           const char *user_id, const char *item_id,
                                           bool played) {
    return userdata_request(r, server, user_id, "PlayedItems", item_id, played);
}
