// Offline downloads -- what the screens say and do.  See dl_ui.h.

#include "dl_ui.h"
#include "i18n.h"

#include <stdio.h>
#include <string.h>

void dl_ui_format_bytes(uint64_t b, char *out, int cap) {
    const uint64_t KB = 1024, MB = KB * 1024, GB = MB * 1024;
    if (b >= 10 * GB)      snprintf(out, (size_t)cap, "%llu GB", (unsigned long long)(b / GB));
    else if (b >= GB)      snprintf(out, (size_t)cap, "%llu.%llu GB",
                                    (unsigned long long)(b / GB),
                                    (unsigned long long)((b % GB) * 10 / GB));
    else if (b >= MB)      snprintf(out, (size_t)cap, "%llu MB", (unsigned long long)(b / MB));
    else if (b > 0)        snprintf(out, (size_t)cap, "%llu KB",
                                    (unsigned long long)((b + KB - 1) / KB));
    else                   snprintf(out, (size_t)cap, "0 KB");
}

static int percent(const DlRecord *r) {
    int pm = dl_progress_permille(r);
    return pm < 0 ? -1 : pm / 10;
}

// ---------------------------------------------------------------------------
// Item page
// ---------------------------------------------------------------------------

DlUiAction dl_ui_item_action(const DlStatus *st, bool can_download,
                             const DlUiContext *cx) {
    if (!cx->ready) return DL_UI_NONE;
    if (!st) return can_download ? DL_UI_START : DL_UI_NONE;
    switch (st->rec.state) {
    case DL_QUEUED:
    case DL_DOWNLOADING: return DL_UI_PAUSE;
    case DL_PAUSED:      return DL_UI_RESUME;
    case DL_COMPLETED:   return DL_UI_PLAY_OFFLINE;
    case DL_FAILED:
    case DL_CANCELLED:   return can_download ? DL_UI_RETRY : DL_UI_NONE;
    default:             return DL_UI_NONE;
    }
}

void dl_ui_item_label(const DlStatus *st, bool can_download,
                      const DlUiContext *cx, char *out, int cap) {
    if (!cx->ready)   { snprintf(out, (size_t)cap, TR("Downloads unavailable")); return; }
    if (!st) {
        snprintf(out, (size_t)cap, "%s", can_download ? TR("Download") : TR("Can't download"));
        return;
    }
    const int pct = percent(&st->rec);
    switch (st->rec.state) {
    case DL_QUEUED:
        if (cx->auth_held)           snprintf(out, (size_t)cap, TR("Queued (sign in)"));
        else if (cx->playback_block) snprintf(out, (size_t)cap, TR("Queued (streaming)"));
        else if (st->retry_in_ms)    snprintf(out, (size_t)cap, TR("Retrying..."));
        else                         snprintf(out, (size_t)cap, TR("Queued"));
        break;
    case DL_DOWNLOADING:
        if (pct >= 0) snprintf(out, (size_t)cap, TR("Downloading %d%%"), pct);
        else          snprintf(out, (size_t)cap, TR("Downloading"));
        break;
    case DL_PAUSED:
        if (pct >= 0) snprintf(out, (size_t)cap, TR("Paused %d%%"), pct);
        else          snprintf(out, (size_t)cap, TR("Paused"));
        break;
    case DL_COMPLETED: snprintf(out, (size_t)cap, TR("Play offline")); break;
    case DL_FAILED:
    case DL_CANCELLED:
        snprintf(out, (size_t)cap, "%s", can_download ? TR("Retry download") : TR("Can't download"));
        break;
    default: snprintf(out, (size_t)cap, TR("Download")); break;
    }
}

const char *dl_ui_result_text(int r) {
    switch (r) {
    case DL_OK:          return "";
    case DL_E_NOT_READY: return TR("Downloads are unavailable (no writable HDD folder)");
    case DL_E_INVALID:   return TR("This version can't be downloaded");
    case DL_E_EXISTS:    return TR("Already downloaded or in the queue");
    case DL_E_FULL:      return TR("The download list is full");
    case DL_E_NO_SPACE:  return TR("Not enough HDD space");
    case DL_E_SPACE_UNKNOWN: return TR("Can't check free HDD space: downloads paused");
    case DL_E_SIZE_UNKNOWN:  return TR("Size unknown, can't check space");
    case DL_E_IO:        return TR("Could not write to the HDD");
    case DL_E_NOT_FOUND: return TR("No longer in the download list");
    case DL_E_STATE:     return TR("Not possible right now");
    case DL_E_NO_DRIVE:  return TR("The USB drive for downloads isn't ready (Settings > Download Location)");
    default:             return TR("Something went wrong");
    }
}

// ---------------------------------------------------------------------------
// Downloads list
// ---------------------------------------------------------------------------

const char *dl_ui_queue_banner(const DlUiContext *cx) {
    if (!cx->ready)         return TR("Downloads are unavailable: no writable HDD folder");
    if (cx->auth_held)      return TR("Sign in to continue downloads");
    if (cx->playback_block) return TR("Downloads pause while you stream -- they resume afterwards");
    return "";
}

// "10 GB" / "6.2 GB" for the numbers in the messages below.
static void gb(uint64_t b, char *out, int cap) { dl_ui_format_bytes(b, out, cap); }

void dl_ui_result_message(int r, const DlSpaceReport *rep, char *out, int cap) {
    if (r == DL_E_NO_SPACE && rep && rep->state == DL_SPACE_LOW) {
        char need[24], avail[24], keep[24];
        gb(rep->need_bytes, need, sizeof(need));
        gb(rep->avail_bytes, avail, sizeof(avail));
        gb(rep->reserve_bytes, keep, sizeof(keep));
        snprintf(out, (size_t)cap,
                 TR("Not enough space: needs %s, %s available (%s is always kept free)"),
                 need, avail, keep);
        return;
    }
    snprintf(out, (size_t)cap, "%s", dl_ui_result_text(r));
}

void dl_ui_banner(const DlUiContext *cx, const DlSpaceReport *space, bool held,
                  char *out, int cap) {
    const char *base = dl_ui_queue_banner(cx);
    if (base[0] || !held || !space) { snprintf(out, (size_t)cap, "%s", base); return; }
    if (space->state == DL_SPACE_UNKNOWN) {
        snprintf(out, (size_t)cap, TR("Can't check free HDD space: downloads paused"));
        return;
    }
    char keep[24];
    gb(space->reserve_bytes, keep, sizeof(keep));
    snprintf(out, (size_t)cap, TR("Paused: the HDD is down to %s free"), keep);
}

void dl_ui_hdd_header(const DlSpaceReport *space, char *out, int cap) {
    if (!space || space->state == DL_SPACE_UNKNOWN) {
        snprintf(out, (size_t)cap, TR("HDD: free space unknown \xC2\xB7 downloads are paused"));
        return;
    }
    char fr[24], keep[24];
    gb(space->free_bytes, fr, sizeof(fr));
    gb(space->reserve_bytes, keep, sizeof(keep));
    snprintf(out, (size_t)cap, TR("HDD: %s free \xC2\xB7 downloads keep %s free"), fr, keep);
}

void dl_ui_store_header(const DlSpaceReport *space, const char *drive, char *out, int cap) {
    if (!drive || !drive[0]) { dl_ui_hdd_header(space, out, cap); return; }
    if (!space || space->state == DL_SPACE_UNKNOWN) {
        snprintf(out, (size_t)cap, TR("%s: free space unknown \xC2\xB7 downloads are paused"), drive);
        return;
    }
    char fr[24], keep[24];
    gb(space->free_bytes, fr, sizeof(fr));
    gb(space->reserve_bytes, keep, sizeof(keep));
    snprintf(out, (size_t)cap, TR("%s: %s free \xC2\xB7 downloads keep %s free"), drive, fr, keep);
}

void dl_ui_row(const DlStatus *st, const DlUiContext *cx, DlUiRow *o) {
    memset(o, 0, sizeof(*o));
    const DlRecord *r = &st->rec;
    snprintf(o->title, sizeof(o->title), "%s", r->title[0] ? r->title : r->id);
    o->permille = -1;

    char done[24], total[24];
    dl_ui_format_bytes(r->bytes_done, done, sizeof(done));
    dl_ui_format_bytes(r->bytes_total, total, sizeof(total));
    if (r->state == DL_COMPLETED)
        snprintf(o->size, sizeof(o->size), "%s", total);
    else if (r->bytes_total > 0 && r->bytes_done > 0)
        snprintf(o->size, sizeof(o->size), "%s of %s", done, total);
    else if (r->bytes_done > 0)
        snprintf(o->size, sizeof(o->size), "%s", done);   // a transcode: no total

    const int pm = dl_progress_permille(r);
    switch (r->state) {
    case DL_DOWNLOADING:
        snprintf(o->status, sizeof(o->status), TR("Downloading"));
        o->permille = pm;          // -1 for a transcode (length unknown)
        o->emphasis = true;
        break;
    case DL_QUEUED:
        if (cx->auth_held) {
            snprintf(o->status, sizeof(o->status), TR("Waiting for sign-in"));
            o->warning = true;
        } else if (cx->playback_block) {
            snprintf(o->status, sizeof(o->status), TR("Paused while streaming"));
        } else if (st->retry_in_ms > 0) {
            snprintf(o->status, sizeof(o->status), TR("Retrying in %us -- %s"),
                     (unsigned)((st->retry_in_ms + 999) / 1000), tr(dl_error_text(r->error)));
            o->warning = true;
        } else {
            snprintf(o->status, sizeof(o->status), TR("Waiting..."));
        }
        if (r->bytes_done > 0) o->permille = pm;
        break;
    case DL_PAUSED:
        // The manager pauses an item for space; the reason is in its error.
        if (r->error == DL_ERR_NO_SPACE || r->error == DL_ERR_SPACE_UNKNOWN) {
            snprintf(o->status, sizeof(o->status), "%s", tr(dl_error_text(r->error)));
            o->warning = true;
        } else {
            snprintf(o->status, sizeof(o->status), TR("Paused"));
        }
        if (r->bytes_done > 0) o->permille = pm;
        break;
    case DL_COMPLETED:
        snprintf(o->status, sizeof(o->status), TR("Downloaded"));
        break;
    case DL_FAILED:
        snprintf(o->status, sizeof(o->status), TR("Failed -- %s"), tr(dl_error_text(r->error)));
        o->warning = true;
        break;
    case DL_CANCELLED:
        snprintf(o->status, sizeof(o->status), TR("Cancelled"));
        o->size[0] = '\0';
        break;
    default: break;
    }
}

DlUiAction dl_ui_row_primary(const DlStatus *st) {
    switch (st->rec.state) {
    case DL_QUEUED: case DL_DOWNLOADING: return DL_UI_PAUSE;
    case DL_PAUSED:                      return DL_UI_RESUME;
    case DL_COMPLETED:                   return DL_UI_PLAY_OFFLINE;
    // A failed/cancelled item retries with the request it already has; one
    // rebuilt from damaged files has none (dl_retry says DL_E_STATE) and
    // must be downloaded again from its page.
    case DL_FAILED: case DL_CANCELLED:   return st->rec.url[0] ? DL_UI_RETRY : DL_UI_NONE;
    default:                             return DL_UI_NONE;
    }
}

DlUiAction dl_ui_row_secondary(const DlStatus *st) {
    switch (st->rec.state) {
    case DL_QUEUED: case DL_DOWNLOADING: case DL_PAUSED: return DL_UI_CANCEL;
    case DL_COMPLETED: case DL_FAILED: case DL_CANCELLED: return DL_UI_REMOVE;
    default: return DL_UI_NONE;
    }
}

const char *dl_ui_action_label(DlUiAction a) {
    switch (a) {
    case DL_UI_START:        return TR("Download");
    case DL_UI_PAUSE:        return TR("Pause");
    case DL_UI_RESUME:       return TR("Resume");
    case DL_UI_RETRY:        return TR("Retry");
    case DL_UI_PLAY_OFFLINE: return TR("Play");
    case DL_UI_CANCEL:       return TR("Cancel");
    case DL_UI_REMOVE:       return TR("Delete");
    default:                 return "";
    }
}

bool dl_ui_action_needs_confirm(DlUiAction a, const DlStatus *st) {
    if (a == DL_UI_REMOVE) return st->rec.state == DL_COMPLETED;
    // Cancelling throws away what has arrived; ask once there is something.
    if (a == DL_UI_CANCEL) return st->rec.bytes_done > 0;
    return false;
}

// ---------------------------------------------------------------------------
// Offline library
// ---------------------------------------------------------------------------

void dl_ui_offline_lines(const DlMeta *m, bool meta_ok, uint64_t bytes,
                         char *title, int tcap, char *sub, int scap) {
    snprintf(title, (size_t)tcap, "%s", m->title[0] ? m->title : m->id);
    char parts[3][96];
    int n = 0;
    if (meta_ok && m->series[0]) {
        if (m->season >= 0 && m->episode >= 0)
            snprintf(parts[n++], sizeof(parts[0]), "%.60s  S%d E%d", m->series,
                     m->season, m->episode);
        else
            snprintf(parts[n++], sizeof(parts[0]), "%.60s", m->series);
    } else if (meta_ok && m->year > 0) {
        snprintf(parts[n++], sizeof(parts[0]), "%d", m->year);
    }
    if (meta_ok && m->runtime_secs >= 60) {
        const unsigned mins = m->runtime_secs / 60;
        if (mins >= 60) snprintf(parts[n++], sizeof(parts[0]), TR("%uh %02um"), mins / 60, mins % 60);
        else            snprintf(parts[n++], sizeof(parts[0]), TR("%u min"), mins);
    }
    dl_ui_format_bytes(bytes, parts[n++], sizeof(parts[0]));
    sub[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(sub);
        snprintf(sub + len, (size_t)scap - len, "%s%s", i ? "  \xC2\xB7  " : "", parts[i]);
    }
}

int dl_ui_clamp_selection(int sel, int count, int visible, int *top) {
    if (count <= 0) { *top = 0; return 0; }
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    if (visible < 1) visible = 1;
    if (*top > sel) *top = sel;
    if (sel >= *top + visible) *top = sel - visible + 1;
    if (*top > count - visible) *top = count - visible;
    if (*top < 0) *top = 0;
    return sel;
}
