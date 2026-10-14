// Offline downloads -- the offline library.  See dl_library.h.

#include "dl_library.h"
#include "dl_http.h"      // DL_LIGHT_* thresholds
#include "dl_manager.h"
#include "dl_platform.h"
#include "dl_ts.h"        // DL_TS_PACKET

#include <stdio.h>
#include <string.h>

// The playability rule, on a status snapshot.  Fills out when non-NULL.
static bool verify(const DlStatus *st, DlLibraryEntry *out) {
    const DlRecord *r = &st->rec;
    if (r->state != DL_COMPLETED || st->active) return false;
    char media[DL_PATH_MAX];
    if (!dl_media_path(r->id, media, sizeof(media))) return false;
    const int64_t size = dl_plat_file_size(media);
    if (size <= 0) return false;
    if (r->bytes_total > 0 && (uint64_t)size != r->bytes_total) return false;
    if (strcmp(r->container, "ts") == 0 && size % DL_TS_PACKET != 0) return false;
    if (!out) return true;

    memset(out, 0, sizeof(*out));
    out->meta_ok = dl_load_meta(r->id, &out->meta);
    if (!out->meta_ok) {
        dl_meta_init(&out->meta);
        snprintf(out->meta.id, sizeof(out->meta.id), "%s", r->id);
        snprintf(out->meta.title, sizeof(out->meta.title), "%s", r->title);
        snprintf(out->meta.container, sizeof(out->meta.container), "%s", r->container);
    }
    // A container the record verified wins over a stale meta.txt.
    if (strcmp(r->container, "ts") == 0)
        snprintf(out->meta.container, sizeof(out->meta.container), "ts");
    snprintf(out->media_path, sizeof(out->media_path), "%s", media);
    const char *leaves[2] = { DL_FILE_POSTER, DL_FILE_BACKDROP };
    char *paths[2] = { out->poster_path, out->backdrop_path };
    for (int i = 0; i < 2; i++) {
        char p[DL_PATH_MAX];
        if (dl_store_item_file(p, sizeof(p), r->id, leaves[i]) &&
            dl_plat_file_size(p) > 0)
            snprintf(paths[i], DL_PATH_MAX, "%s", p);
    }
    out->bytes = (uint64_t)size;
    out->seq = r->seq;
    return true;
}

int dl_library_ids(char (*ids)[DL_ID_MAX], int max) {
    // Walk the queue in order one status at a time: no DL_MAX_ITEMS-sized
    // snapshot array (each status is ~2 KB).
    static char all[DL_MAX_ITEMS][DL_ID_MAX];
    int n_all = dl_completed_ids(all, DL_MAX_ITEMS);
    int n = 0;
    for (int i = 0; i < n_all && n < max; i++) {
        DlStatus st;
        if (dl_find(all[i], &st) && verify(&st, NULL))
            memcpy(ids[n++], all[i], DL_ID_MAX);   // same size, NUL-terminated
    }
    return n;
}

bool dl_library_get(const char *id, DlLibraryEntry *out) {
    DlStatus st;
    return id && dl_find(id, &st) && verify(&st, out);
}

bool dl_library_has(const char *id) {
    DlStatus st;
    return id && dl_find(id, &st) && verify(&st, NULL);
}

bool dl_meta_is_light(const DlMeta *m, bool meta_ok) {
    if (!meta_ok) return false;                         // unknown: heavy
    if (m->height <= 0 || m->height > DL_LIGHT_MAX_HEIGHT) return false;
    if (m->video_bitrate == 0 || m->video_bitrate > DL_LIGHT_MAX_VIDEO_BPS)
        return false;                                   // 0 = copied at source rate
    if (!m->audio_codec[0] || strcmp(m->audio_codec, "dts") == 0 ||
        strcmp(m->audio_codec, "truehd") == 0)
        return false;                                   // HD audio or unknown
    return true;
}

void dl_library_plan(const DlLibraryEntry *e, DlLocalPlan *out) {
    const DlMeta *m = &e->meta;
    // The file's frame ceiling; unknown or out-of-range falls back to the
    // largest this player allocates for, which holds any smaller frame.
    const bool dims_ok = e->meta_ok && m->width > 0 && m->height > 0 &&
                         m->width <= 1920 && m->height <= 1080;
    out->req_w = dims_ok ? (uint32_t)m->width  : 1920;
    out->req_h = dims_ok ? (uint32_t)m->height : 1080;
    out->runtime_secs = e->meta_ok ? m->runtime_secs : 0;
    out->light = dl_meta_is_light(m, e->meta_ok);
}
