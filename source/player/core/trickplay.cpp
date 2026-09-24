// See trickplay.h. Fetches the manifest once per title, then one sheet JPEG
// at a time as the scrub position crosses into a new sheet.
//
// MEMORY: this is the one real hardware risk in this feature, and it is
// unresolved without a console test -- flagged here rather than buried. A
// typical manifest (320x134 tiles, 10x10 grid) decodes to a ~3200x1340 RGBA
// sheet, i.e. ~16-17 MB, allocated with a PLAIN malloc (not the thumbnail
// decode arena -- see below) while the video decoder, jitter buffer and
// read-ahead ring are ALL already live and tightly budgeted (see HANDOFF.md
// "1080p still dips"). thumb_cache_shutdown() exists specifically because
// this project already hit CELL_VDEC_ERROR_FATAL from exactly this kind of
// pressure once. So: gate on meminfo_avail_kb() with real headroom before
// ever attempting the decode, free the sheet the instant scrubbing ends
// rather than caching it for the rest of playback, and treat "not enough
// memory right now" as "no preview this time", never as a crash.
//
// Deliberately does NOT use img_arena (thumbnail_cache.cpp's boot-reserved
// scratch for stb_image): that arena is armed per-thread by
// thumb_cache_init()'s fetch thread and is explicitly inert for any other
// thread (img_arena.cpp: img_arena_malloc falls back to plain malloc when
// not armed on the calling thread) -- exactly what happens here, called
// from the main/display thread via player_seek.cpp, and that fallback is
// the intended, safe behaviour, not an oversight.

#include "trickplay.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include "http.h"
#include "jellyfin_api.h"
#include "trickplay_info.h"
#include "plog.h"
#include "meminfo.h"
#include "stb_image.h"   // implementation compiled once, in thumbnail_cache.cpp

// One sheet JPEG. Generous over the ~320x134/10x10 example Jellyfin's own
// docs use, but this is a guess pending a real fetch against a real server
// -- see trickplay.h and the HANDOFF note this feature was added under.
#define TP_FETCH_BUF (2 * 1024 * 1024)

// Fixed safety margin required to remain free AFTER the sheet allocation,
// on top of the sheet's own bytes, before attempting a decode at all.
#define TP_MEM_HEADROOM_KB (8 * 1024)

static bool          s_have_info   = false;
static bool          s_info_failed = false;
static char          s_item_id[64] = "";
static TrickplayInfo s_info;

static u32  *s_sheet       = NULL;
static int   s_sheet_w = 0, s_sheet_h = 0;
static int   s_sheet_index = -1;
static bool  s_sheet_ok    = false;

static TrickplayTile s_cur;
static bool           s_cur_ok = false;

static uint8_t *s_fetch_buf = NULL;

static void free_sheet(void) {
    free(s_sheet);
    s_sheet = NULL;
    s_sheet_w = s_sheet_h = 0;
    s_sheet_index = -1;
    s_sheet_ok = false;
}

void trickplay_reset(void) {
    s_have_info = false;
    s_info_failed = false;
    s_item_id[0] = '\0';
    s_cur_ok = false;
    free_sheet();   // give the memory straight back -- see file header
}

void trickplay_release_sheet(void) {
    s_cur_ok = false;
    free_sheet();
}

static bool ensure_info(const char *item_id, const char *media_source_id) {
    if (strcmp(s_item_id, item_id) != 0) {
        // A new title: any cached sheet belonged to the old one.
        snprintf(s_item_id, sizeof(s_item_id), "%s", item_id);
        s_have_info = false;
        s_info_failed = false;
        free_sheet();
    }
    if (s_have_info) return true;
    if (s_info_failed) return false;

    char url[512];
    snprintf(url, sizeof(url), "%s/Users/%s/Items/%s?Fields=Trickplay",
             g_server, g_userid, item_id);
    static char *body = NULL;
    if (!body) body = (char *)malloc(RESPONSE_SIZE);
    if (!body) { s_info_failed = true; return false; }
    int rc = http_request(HTTP_GET, url, NULL, g_token, body, RESPONSE_SIZE);
    if (rc <= 0 || !trickplay_parse_info(body, media_source_id, &s_info)) {
        plog("trickplay: no manifest for this title");
        s_info_failed = true;
        return false;
    }
    s_have_info = true;
    char b[144];
    snprintf(b, sizeof(b),
             "trickplay: manifest tile=%dx%d grid=%dx%d interval=%dms count=%d",
             s_info.width, s_info.height, s_info.tile_cols, s_info.tile_rows,
             s_info.interval_ms, s_info.thumbnail_count);
    plog(b);
    return true;
}

static bool fetch_sheet(const char *item_id, const char *media_source_id,
                        int sheet_index) {
    int want_w = s_info.width  * s_info.tile_cols;
    int want_h = s_info.height * s_info.tile_rows;
    size_t want_bytes = (size_t)want_w * want_h * 4;

    u32 free_kb = meminfo_avail_kb();
    if (free_kb == 0 ||
        (u32)(want_bytes / 1024) + TP_MEM_HEADROOM_KB > free_kb) {
        char b[128];
        snprintf(b, sizeof(b),
                 "trickplay: skipping decode, not enough headroom "
                 "(need ~%uKB+%uKB margin, have %uKB free)",
                 (unsigned)(want_bytes / 1024), (unsigned)TP_MEM_HEADROOM_KB,
                 (unsigned)free_kb);
        plog(b);
        return false;
    }

    if (!s_sheet || s_sheet_w != want_w || s_sheet_h != want_h) {
        free(s_sheet);
        s_sheet = (u32 *)memalign(16, want_bytes);
        s_sheet_w = want_w;
        s_sheet_h = want_h;
    }
    if (!s_sheet) { plog("trickplay: sheet alloc failed"); return false; }

    if (!s_fetch_buf) s_fetch_buf = (uint8_t *)malloc(TP_FETCH_BUF);
    if (!s_fetch_buf) return false;

    char url[640];
    snprintf(url, sizeof(url),
             "%s/Videos/%s/Trickplay/%s/%d.jpg?MediaSourceId=%s",
             g_server, item_id, s_info.width_key, sheet_index,
             (media_source_id && media_source_id[0]) ? media_source_id : item_id);
    int bytes = http_fetch_binary(url, g_token, s_fetch_buf, TP_FETCH_BUF);
    if (bytes <= 0) {
        char b[112];
        snprintf(b, sizeof(b), "trickplay: sheet fetch failed idx=%d rc=%d",
                 sheet_index, bytes);
        plog(b);
        return false;
    }

    int w, h, ch;
    unsigned char *px = stbi_load_from_memory(
        (const stbi_uc *)s_fetch_buf, bytes, &w, &h, &ch, 4);
    if (!px) {
        plog("trickplay: sheet decode failed");
        return false;
    }
    int copy_w = (w < want_w) ? w : want_w;
    int copy_h = (h < want_h) ? h : want_h;
    if (w != want_w || h != want_h) {
        char b[112];
        snprintf(b, sizeof(b), "trickplay: sheet %dx%d != manifest grid %dx%d",
                 w, h, want_w, want_h);
        plog(b);
    }
    for (int y = 0; y < copy_h; y++) {
        const unsigned char *src = px + (size_t)y * w * 4;
        u32 *dst = s_sheet + (size_t)y * want_w;
        for (int x = 0; x < copy_w; x++) {
            const unsigned char *s = src + (size_t)x * 4;
            dst[x] = ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2];   // 0x00RRGGBB
        }
    }
    stbi_image_free(px);
    return true;
}

void trickplay_scrub_update(const char *item_id, const char *media_source_id,
                            u32 time_ms) {
    s_cur_ok = false;
    if (!item_id || !item_id[0]) return;
    if (!ensure_info(item_id, media_source_id)) return;

    int per_sheet = s_info.tile_cols * s_info.tile_rows;
    if (per_sheet <= 0 || s_info.interval_ms <= 0) return;

    int thumb_index = (int)(time_ms / (u32)s_info.interval_ms);
    if (s_info.thumbnail_count > 0 && thumb_index >= s_info.thumbnail_count)
        thumb_index = s_info.thumbnail_count - 1;
    if (thumb_index < 0) thumb_index = 0;
    int sheet_index = thumb_index / per_sheet;
    int slot         = thumb_index % per_sheet;
    int col          = slot % s_info.tile_cols;
    int row          = slot / s_info.tile_cols;

    if (sheet_index != s_sheet_index || !s_sheet_ok) {
        s_sheet_ok    = fetch_sheet(item_id, media_source_id, sheet_index);
        s_sheet_index = sheet_index;
    }
    if (!s_sheet_ok) return;

    s_cur.sheet_rgb = s_sheet;
    s_cur.sheet_w   = s_sheet_w;
    s_cur.sheet_h   = s_sheet_h;
    s_cur.tile_x    = col * s_info.width;
    s_cur.tile_y    = row * s_info.height;
    s_cur.tile_w    = s_info.width;
    s_cur.tile_h    = s_info.height;
    s_cur_ok        = true;
}

const TrickplayTile *trickplay_current(void) {
    return s_cur_ok ? &s_cur : NULL;
}
