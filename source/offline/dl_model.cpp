// Offline downloads -- data model, state machine, record format.
// See dl_model.h.

#include "dl_model.h"
#include "i18n.h"

#include <stddef.h>   // offsetof
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -------------------------------------------------------------------------
// Names
// -------------------------------------------------------------------------

static const char *const k_state_names[DL_STATE_COUNT] = {
    "queued", "downloading", "paused", "completed", "failed", "cancelled",
};

static const char *const k_error_names[DL_ERR_COUNT] = {
    "none", "unreachable", "timeout", "network", "partial", "server",
    "bad_response", "http", "auth", "not_found", "no_space", "disk",
    "unsupported", "corrupt", "bad_media", "space_unknown",
};

static const char *const k_error_text[DL_ERR_COUNT] = {
    "",
    TRN("Server unreachable"),
    TRN("Server timed out"),
    TRN("Connection lost"),
    TRN("Download interrupted"),
    TRN("Server error"),
    TRN("Unexpected server response"),
    TRN("Server refused the request"),
    TRN("Sign-in expired"),
    TRN("No longer on the server"),
    TRN("Paused: the HDD is down to its free-space reserve"),
    TRN("Could not write to the HDD"),
    TRN("Unsupported address (https)"),
    TRN("Download data is damaged"),
    TRN("Server sent an incomplete video"),
    TRN("Cannot check free HDD space: downloads paused"),
};

const char *dl_state_name(DlState s) {
    return (s >= 0 && s < DL_STATE_COUNT) ? k_state_names[s] : "?";
}

bool dl_state_from_name(const char *n, DlState *out) {
    for (int i = 0; i < DL_STATE_COUNT; i++)
        if (strcmp(n, k_state_names[i]) == 0) { *out = (DlState)i; return true; }
    return false;
}

const char *dl_error_name(DlError e) {
    return (e >= 0 && e < DL_ERR_COUNT) ? k_error_names[e] : "?";
}

bool dl_error_from_name(const char *n, DlError *out) {
    for (int i = 0; i < DL_ERR_COUNT; i++)
        if (strcmp(n, k_error_names[i]) == 0) { *out = (DlError)i; return true; }
    return false;
}

const char *dl_error_text(DlError e) {
    return (e >= 0 && e < DL_ERR_COUNT) ? k_error_text[e] : "";
}

// -------------------------------------------------------------------------
// State machine
// -------------------------------------------------------------------------
//
//             START            COMPLETE
//   QUEUED ----------> DOWNLOADING ------> COMPLETED
//     ^  ^               |  |  |
//     |  +- FAIL_RETRY --+  |  +-- FAIL --> FAILED --RETRY--> QUEUED
//     |  +- INTERRUPTED -+  |
//     |                     +-- PAUSE ---> PAUSED --RESUME--> QUEUED
//     |
//   CANCEL from QUEUED / DOWNLOADING / PAUSED / FAILED --> CANCELLED
//   RETRY from CANCELLED --> QUEUED (starts over; the partial was deleted)
//
// COMPLETED is terminal: the only way out is removing the item, which is not
// a transition (the record ceases to exist).

bool dl_next_state(DlState s, DlEvent ev, DlState *out) {
    DlState n;
    switch (s) {
    case DL_QUEUED:
        switch (ev) {
        case DL_EV_START:  n = DL_DOWNLOADING; break;
        case DL_EV_PAUSE:  n = DL_PAUSED;      break;
        case DL_EV_CANCEL: n = DL_CANCELLED;   break;
        default: return false;
        }
        break;
    case DL_DOWNLOADING:
        switch (ev) {
        case DL_EV_PAUSE:       n = DL_PAUSED;    break;
        case DL_EV_CANCEL:      n = DL_CANCELLED; break;
        case DL_EV_COMPLETE:    n = DL_COMPLETED; break;
        case DL_EV_FAIL_RETRY:  n = DL_QUEUED;    break;
        case DL_EV_INTERRUPTED: n = DL_QUEUED;    break;
        case DL_EV_FAIL:        n = DL_FAILED;    break;
        default: return false;
        }
        break;
    case DL_PAUSED:
        switch (ev) {
        case DL_EV_RESUME: n = DL_QUEUED;    break;
        case DL_EV_CANCEL: n = DL_CANCELLED; break;
        default: return false;
        }
        break;
    case DL_FAILED:
        switch (ev) {
        case DL_EV_RETRY:  n = DL_QUEUED;    break;
        case DL_EV_CANCEL: n = DL_CANCELLED; break;
        default: return false;
        }
        break;
    case DL_CANCELLED:
        if (ev != DL_EV_RETRY) return false;
        n = DL_QUEUED;
        break;
    default:   // DL_COMPLETED and anything out of range
        return false;
    }
    *out = n;
    return true;
}

bool dl_state_is_active(DlState s) {
    return s == DL_QUEUED || s == DL_DOWNLOADING;
}

bool dl_error_retryable(DlError e) {
    switch (e) {
    case DL_ERR_UNREACHABLE: case DL_ERR_TIMEOUT: case DL_ERR_NETWORK:
    case DL_ERR_PARTIAL:     case DL_ERR_SERVER:  case DL_ERR_BAD_RESPONSE:
    case DL_ERR_BAD_MEDIA:
        return true;
    default:
        return false;
    }
}

DlError dl_error_for_http_status(int status) {
    if (status >= 200 && status < 300) return DL_ERR_NONE;
    // 401 is the session: the token is dead and a new login fixes it.  403
    // is the account: this user may not have this item (parental rating,
    // library access), which no retry or re-login changes.
    if (status == 401) return DL_ERR_AUTH;
    if (status == 403) return DL_ERR_HTTP;
    if (status == 404 || status == 410) return DL_ERR_NOT_FOUND;
    // 408 Request Timeout and 429 Too Many Requests are the server asking us
    // to come back later, not refusing the request.
    if (status == 408 || status == 429) return DL_ERR_SERVER;
    if (status >= 500 && status < 600) return DL_ERR_SERVER;
    if (status >= 400 && status < 500) return DL_ERR_HTTP;
    return DL_ERR_BAD_RESPONSE;   // 1xx/3xx/garbage: nothing we can follow
}

uint32_t dl_backoff_ms(uint32_t attempts) {
    const uint32_t cap = 300000;   // 5 minutes
    if (attempts == 0) return 0;
    if (attempts > 8) return cap;  // 2 s << 7 = 256 s; past that it is capped
    uint32_t ms = 2000u << (attempts - 1);
    return ms > cap ? cap : ms;
}

int dl_progress_permille(const DlRecord *r) {
    if (r->state == DL_COMPLETED) return 1000;
    if (r->bytes_total == 0) return -1;
    if (r->bytes_done >= r->bytes_total) return 1000;
    // done/total in 64-bit, without overflowing done*1000 on huge files
    if (r->bytes_total > (UINT64_MAX / 1000))
        return (int)(r->bytes_done / (r->bytes_total / 1000));
    return (int)((r->bytes_done * 1000) / r->bytes_total);
}

bool dl_id_valid(const char *id) {
    int n = 0;
    for (const char *p = id; *p; p++, n++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok || n >= DL_ID_MAX - 1) return false;
    }
    return n > 0;
}

void dl_record_init(DlRecord *r) {
    memset(r, 0, sizeof(*r));
    r->state = DL_QUEUED;
    r->error = DL_ERR_NONE;
}

void dl_meta_init(DlMeta *m) {
    memset(m, 0, sizeof(*m));
    m->season = -1;
    m->episode = -1;
    m->audio_stream_index = -1;
}

// -------------------------------------------------------------------------
// key=value text
// -------------------------------------------------------------------------
// One entry per line.  Values escape \\, \n and \r so a title or overview
// with a line break cannot forge a key.  The first line is a magic+version
// header and the last is "end": a record missing its end line was cut off
// mid-write and is rejected rather than half-loaded.

#define REC_MAGIC  "jfdl-state 1"
#define META_MAGIC "jfdl-meta 1"

typedef struct { char *p; int cap; int n; bool over; } Out;

static void out_raw(Out *o, const char *s) {
    for (; *s; s++) {
        if (o->n >= o->cap - 1) { o->over = true; return; }
        o->p[o->n++] = *s;
    }
}

static void out_kv(Out *o, const char *key, const char *val) {
    out_raw(o, key);
    out_raw(o, "=");
    char e[3] = { '\\', 0, 0 };
    for (const char *s = val; *s; s++) {
        if      (*s == '\\') { e[1] = '\\'; out_raw(o, e); }
        else if (*s == '\n') { e[1] = 'n';  out_raw(o, e); }
        else if (*s == '\r') { e[1] = 'r';  out_raw(o, e); }
        else { char c[2] = { *s, 0 }; out_raw(o, c); }
    }
    out_raw(o, "\n");
}

static void out_ku(Out *o, const char *key, unsigned long long v) {
    char b[24];
    snprintf(b, sizeof(b), "%llu", v);
    out_kv(o, key, b);
}

static void out_ki(Out *o, const char *key, long long v) {
    char b[24];
    snprintf(b, sizeof(b), "%lld", v);
    out_kv(o, key, b);
}

static int out_done(Out *o) {
    if (o->over || o->cap <= 0) { if (o->cap > 0) o->p[0] = '\0'; return -1; }
    o->p[o->n] = '\0';
    return o->n;
}

// Iterate lines of `text`: calls fn(key, value) with unescaped value.
// Returns false if the magic header is wrong or the end line is missing, or
// if fn rejected a value.
typedef bool (*kv_fn)(const char *key, const char *val, void *ctx);

static bool parse_kv(const char *text, const char *magic, kv_fn fn, void *ctx) {
    const char *p = text;
    size_t ml = strlen(magic);
    if (strncmp(p, magic, ml) != 0 || (p[ml] != '\n' && p[ml] != '\r'))
        return false;
    p += ml;
    bool ended = false;
    // On the stack, not static: records are parsed from the UI thread and
    // the worker's restore path alike.  ~1.1 KB, fine on either stack.
    char key[32];
    char val[DL_URL_MAX + 64];          // longest value is url/overview
    while (*p) {
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (le > ls && le[-1] == '\r') le--;
        if (le - ls == 3 && strncmp(ls, "end", 3) == 0) { ended = true; break; }
        const char *eq = (const char *)memchr(ls, '=', (size_t)(le - ls));
        if (!eq || eq == ls || eq - ls >= (int)sizeof(key)) return false;
        memcpy(key, ls, (size_t)(eq - ls));
        key[eq - ls] = '\0';
        int vn = 0;
        for (const char *s = eq + 1; s < le; s++) {
            char c = *s;
            if (c == '\\' && s + 1 < le) {
                s++;
                c = (*s == 'n') ? '\n' : (*s == 'r') ? '\r' : *s;
            }
            if (vn >= (int)sizeof(val) - 1) return false;
            val[vn++] = c;
        }
        val[vn] = '\0';
        if (!fn(key, val, ctx)) return false;
    }
    return ended;
}

// Strict numeric parsers: the whole string must be the number.
static bool parse_u64(const char *s, uint64_t *out) {
    if (!*s || *s == '-' || *s == '+') return false;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end) return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_i32(const char *s, int *out) {
    if (!*s) return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end || v < -2147483647L || v > 2147483647L) return false;
    *out = (int)v;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out) {
    uint64_t v;
    if (!parse_u64(s, &v) || v > 0xFFFFFFFFull) return false;
    *out = (uint32_t)v;
    return true;
}

static void copy_str(char *dst, int cap, const char *src) {
    snprintf(dst, (size_t)cap, "%s", src);
}

// -------------------------------------------------------------------------
// DlRecord
// -------------------------------------------------------------------------

int dl_record_format(const DlRecord *r, char *out, int cap) {
    Out o = { out, cap, 0, false };
    out_raw(&o, REC_MAGIC "\n");
    out_kv(&o, "id",          r->id);
    out_kv(&o, "title",       r->title);
    out_kv(&o, "url",         r->url);
    out_kv(&o, "state",       dl_state_name(r->state));
    out_kv(&o, "error",       dl_error_name(r->error));
    out_ki(&o, "http_status", r->http_status);
    out_ku(&o, "seq",         r->seq);
    out_ku(&o, "attempts",    r->attempts);
    out_ku(&o, "bytes_done",  r->bytes_done);
    out_ku(&o, "bytes_total", r->bytes_total);
    out_ku(&o, "est_bytes",   r->est_bytes);
    out_ku(&o, "resumable",   r->resumable ? 1 : 0);
    out_kv(&o, "container",   r->container);
    out_ku(&o, "expect_secs", r->expect_secs);
    out_kv(&o, "poster_url",  r->poster_url);
    out_kv(&o, "backdrop_url", r->backdrop_url);
    out_ku(&o, "art_tries",   r->art_tries);
    out_raw(&o, "end\n");
    return out_done(&o);
}

struct RecCtx { DlRecord *r; bool have_id, have_state; };

static bool rec_kv(const char *k, const char *v, void *vctx) {
    RecCtx *c = (RecCtx *)vctx;
    DlRecord *r = c->r;
    uint64_t u;
    if (strcmp(k, "id") == 0) {
        if (!dl_id_valid(v)) return false;
        copy_str(r->id, sizeof(r->id), v);
        c->have_id = true;
    } else if (strcmp(k, "title") == 0) {
        copy_str(r->title, sizeof(r->title), v);
    } else if (strcmp(k, "url") == 0) {
        if (strlen(v) >= sizeof(r->url)) return false;
        copy_str(r->url, sizeof(r->url), v);
    } else if (strcmp(k, "state") == 0) {
        if (!dl_state_from_name(v, &r->state)) return false;
        c->have_state = true;
    } else if (strcmp(k, "error") == 0) {
        if (!dl_error_from_name(v, &r->error)) return false;
    } else if (strcmp(k, "http_status") == 0) {
        if (!parse_i32(v, &r->http_status)) return false;
    } else if (strcmp(k, "seq") == 0) {
        if (!parse_u32(v, &r->seq)) return false;
    } else if (strcmp(k, "attempts") == 0) {
        if (!parse_u32(v, &r->attempts)) return false;
    } else if (strcmp(k, "bytes_done") == 0) {
        if (!parse_u64(v, &r->bytes_done)) return false;
    } else if (strcmp(k, "bytes_total") == 0) {
        if (!parse_u64(v, &r->bytes_total)) return false;
    } else if (strcmp(k, "est_bytes") == 0) {
        if (!parse_u64(v, &r->est_bytes)) return false;
    } else if (strcmp(k, "container") == 0) {
        copy_str(r->container, sizeof(r->container), v);
    } else if (strcmp(k, "expect_secs") == 0) {
        if (!parse_u32(v, &r->expect_secs)) return false;
    } else if (strcmp(k, "poster_url") == 0 || strcmp(k, "backdrop_url") == 0) {
        if (strlen(v) >= DL_ART_URL_MAX) return false;
        copy_str(k[0] == 'p' ? r->poster_url : r->backdrop_url, DL_ART_URL_MAX, v);
    } else if (strcmp(k, "art_tries") == 0) {
        if (!parse_u32(v, &r->art_tries)) return false;
    } else if (strcmp(k, "resumable") == 0) {
        if (!parse_u64(v, &u) || u > 1) return false;
        r->resumable = (uint8_t)u;
    }
    // unknown keys: ignored (forward compatible)
    return true;
}

bool dl_record_parse(const char *text, DlRecord *out) {
    DlRecord r;
    dl_record_init(&r);
    RecCtx c = { &r, false, false };
    if (!parse_kv(text, REC_MAGIC, rec_kv, &c)) return false;
    if (!c.have_id || !c.have_state) return false;
    if (r.bytes_total > 0 && r.bytes_done > r.bytes_total) return false;
    *out = r;
    return true;
}

// -------------------------------------------------------------------------
// DlMeta
// -------------------------------------------------------------------------

// One table drives both directions, so a field cannot be written and then
// forgotten by the reader (or the reverse).  id and title are required and
// the artwork names are validated, so those three are handled by name below.
enum { MF_STR, MF_INT, MF_U32 };
typedef struct { const char *key; int kind; size_t off; size_t cap; } MetaField;
#define MF(key, kind, field) \
    { key, kind, offsetof(DlMeta, field), sizeof(((DlMeta *)0)->field) }

static const MetaField k_meta_fields[] = {
    MF("id",                 MF_STR, id),
    MF("type",               MF_STR, type),
    MF("title",              MF_STR, title),
    MF("series",             MF_STR, series),
    MF("series_id",          MF_STR, series_id),
    MF("season",             MF_INT, season),
    MF("episode",            MF_INT, episode),
    MF("year",               MF_INT, year),
    MF("runtime_secs",       MF_U32, runtime_secs),
    MF("overview",           MF_STR, overview),
    MF("container",          MF_STR, container),
    MF("video_codec",        MF_STR, video_codec),
    MF("audio_codec",        MF_STR, audio_codec),
    MF("width",              MF_INT, width),
    MF("height",             MF_INT, height),
    MF("audio_channels",     MF_INT, audio_channels),
    MF("video_bitrate",      MF_U32, video_bitrate),
    MF("media_source_id",    MF_STR, media_source_id),
    MF("audio_stream_index", MF_INT, audio_stream_index),
    MF("quality",            MF_STR, quality),
    MF("video_info",         MF_STR, video_info),
    MF("audio_info",         MF_STR, audio_info),
    MF("poster",             MF_STR, poster),
    MF("backdrop",           MF_STR, backdrop),
};
#define N_META_FIELDS ((int)(sizeof(k_meta_fields) / sizeof(k_meta_fields[0])))

int dl_meta_format(const DlMeta *m, char *out, int cap) {
    Out o = { out, cap, 0, false };
    const char *base = (const char *)m;
    out_raw(&o, META_MAGIC "\n");
    for (int i = 0; i < N_META_FIELDS; i++) {
        const MetaField *f = &k_meta_fields[i];
        const void *p = base + f->off;
        if (f->kind == MF_STR)      out_kv(&o, f->key, (const char *)p);
        else if (f->kind == MF_INT) out_ki(&o, f->key, *(const int *)p);
        else                        out_ku(&o, f->key, *(const uint32_t *)p);
    }
    out_raw(&o, "end\n");
    return out_done(&o);
}

struct MetaCtx { DlMeta *m; bool have_id, have_title; };

// Artwork names are joined onto the item directory, so the same rule as
// ids applies, plus one dot for the extension.
static bool art_name_valid(const char *s) {
    if (!*s) return true;   // "" = no artwork
    int dots = 0, n = 0;
    for (const char *p = s; *p; p++, n++) {
        char c = *p;
        if (c == '.') { if (++dots > 1 || p == s) return false; continue; }
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return n < 32;
}

static bool meta_kv(const char *k, const char *v, void *vctx) {
    MetaCtx *c = (MetaCtx *)vctx;
    char *base = (char *)c->m;
    for (int i = 0; i < N_META_FIELDS; i++) {
        const MetaField *f = &k_meta_fields[i];
        if (strcmp(k, f->key) != 0) continue;
        void *p = base + f->off;
        if (f->kind == MF_INT) return parse_i32(v, (int *)p);
        if (f->kind == MF_U32) return parse_u32(v, (uint32_t *)p);
        if (f->off == offsetof(DlMeta, id)) {
            if (!dl_id_valid(v)) return false;
            c->have_id = true;
        } else if (f->off == offsetof(DlMeta, title)) {
            c->have_title = v[0] != '\0';
        } else if (f->off == offsetof(DlMeta, poster) ||
                   f->off == offsetof(DlMeta, backdrop)) {
            if (!art_name_valid(v)) return false;
        }
        copy_str((char *)p, (int)f->cap, v);
        return true;
    }
    return true;   // unknown key: ignored (forward compatible)
}

bool dl_meta_parse(const char *text, DlMeta *out) {
    DlMeta m;
    dl_meta_init(&m);
    MetaCtx c = { &m, false, false };
    if (!parse_kv(text, META_MAGIC, meta_kv, &c)) return false;
    // An item with no id cannot be found and one with no title cannot be
    // shown; either means the file is not what it claims to be.
    if (!c.have_id || !c.have_title) return false;
    *out = m;
    return true;
}
