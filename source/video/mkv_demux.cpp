// Matroska demuxer: see mkv_demux.h.

#include "mkv_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WIN_SIZE   (256 * 1024)
#define SCAN_MAX   (8u * 1024u * 1024u)        // how far a cluster search reads before giving up

// element ids
enum {
    ID_EBML = 0x1A45DFA3, ID_DOCTYPE = 0x4282,
    ID_SEGMENT = 0x18538067, ID_SEEKHEAD = 0x114D9B74, ID_SEEK = 0x4DBB, ID_SEEKID = 0x53AB, ID_SEEKPOS = 0x53AC,
    ID_INFO = 0x1549A966, ID_TIMESCALE = 0x2AD7B1, ID_DURATION = 0x4489,
    ID_TRACKS = 0x1654AE6B, ID_TRACKENTRY = 0xAE, ID_TRACKNUMBER = 0xD7, ID_TRACKTYPE = 0x83,
    ID_FLAGENABLED = 0xB9, ID_FLAGDEFAULT = 0x88, ID_FLAGFORCED = 0x55AA, ID_DEFAULTDURATION = 0x23E383,
    ID_NAME = 0x536E, ID_LANGUAGE = 0x22B59C, ID_LANGUAGEBCP47 = 0x22B59D, ID_CODECID = 0x86, ID_CODECPRIVATE = 0x63A2,
    ID_VIDEO = 0xE0, ID_PIXELW = 0xB0, ID_PIXELH = 0xBA,
    ID_AUDIO = 0xE1, ID_SAMPLERATE = 0xB5, ID_CHANNELS = 0x9F, ID_BITDEPTH = 0x6264,
    ID_CONTENTENCODINGS = 0x6D80, ID_CONTENTENCODING = 0x6240, ID_ENCTYPE = 0x5033, ID_ENCCOMP = 0x5034,
    ID_COMPALGO = 0x4254, ID_COMPSETTINGS = 0x4255, ID_ENCENC = 0x5035,
    ID_CUES = 0x1C53BB6B, ID_CUEPOINT = 0xBB, ID_CUETIME = 0xB3, ID_CUETRACKPOS = 0xB7, ID_CUETRACK = 0xF7,
    ID_CUECLUSTERPOS = 0xF1,
    ID_CLUSTER = 0x1F43B675, ID_TIMECODE = 0xE7, ID_SIMPLEBLOCK = 0xA3, ID_BLOCKGROUP = 0xA0, ID_BLOCK = 0xA1,
    ID_BLOCKDURATION = 0x9B, ID_REFERENCEBLOCK = 0xFB,
    ID_TAGS = 0x1254C367, ID_CHAPTERS = 0x1043A770, ID_ATTACHMENTS = 0x1941A469
};

static const uint64_t UNKNOWN_SIZE = UINT64_MAX;

// ---- the read window --------------------------------------------------------------------

static int rd_loop(MkvFile *f, uint64_t off, uint8_t *dst, int n) {
    int got = 0;
    while (got < n) {
        const int r = f->rd(f->ctx, off + (uint64_t)got, dst + got, n - got);
        if (r < 0) return got ? got : r;
        if (r == 0) break;
        got += r;
    }
    return got;
}

// Copies up to n bytes at off; returns the count (short only at the end of the file), or < 0.
static int fread_at(MkvFile *f, uint64_t off, uint8_t *dst, int n) {
    if (n <= 0) return 0;
    if (n > WIN_SIZE / 2) return rd_loop(f, off, dst, n);
    if (!(f->win_len > 0 && off >= f->win_off && off + (uint64_t)n <= f->win_off + (uint64_t)f->win_len)) {
        const uint64_t start = off & ~(uint64_t)4095;
        const int got = rd_loop(f, start, f->win, WIN_SIZE);
        if (got < 0) { f->win_len = 0; return got; }
        f->win_off = start; f->win_len = got;
        if (off >= start + (uint64_t)got) return 0;
    }
    const uint64_t rel = off - f->win_off;
    int avail = f->win_len - (int)rel;
    if (avail > n) avail = n;
    memcpy(dst, f->win + rel, (size_t)avail);
    return avail;
}

static bool fread_exact(MkvFile *f, uint64_t off, uint8_t *dst, int n) {
    return fread_at(f, off, dst, n) == n;
}

// ---- EBML primitives ----------------------------------------------------------------------

static int clz8(unsigned b) { int n = 0; for (unsigned m = 0x80; m && !(b & m); m >>= 1) n++; return n; }

// A variable-length integer from memory.  len = bytes used; *unknown when every value bit is 1.
static bool vint_mem(const uint8_t *p, int avail, uint64_t *val, int *len, bool *unknown) {
    if (avail < 1 || p[0] == 0) return false;
    const int l = clz8(p[0]) + 1;
    if (l > 8 || l > avail) return false;
    uint64_t v = p[0] & (0xFFu >> l);
    bool ones = v == (0xFFu >> l);
    for (int i = 1; i < l; i++) { v = (v << 8) | p[i]; if (p[i] != 0xFF) ones = false; }
    *val = v; *len = l;
    if (unknown) *unknown = ones;
    return true;
}

static bool id_mem(const uint8_t *p, int avail, uint32_t *id, int *len) {
    if (avail < 1 || p[0] == 0) return false;
    const int l = clz8(p[0]) + 1;
    if (l > 4 || l > avail) return false;
    uint32_t v = 0;
    for (int i = 0; i < l; i++) v = (v << 8) | p[i];
    *id = v; *len = l;
    return true;
}

// An element header at file offset `off`.  False at the end of the file or when it is not one.
static bool elem_header(MkvFile *f, uint64_t off, uint32_t *id, uint64_t *size, int *hdr) {
    uint8_t b[12];
    const int n = fread_at(f, off, b, 12);
    if (n < 2) return false;
    int il = 0, sl = 0;
    bool unk = false;
    uint64_t sz = 0;
    if (!id_mem(b, n, id, &il)) return false;
    if (!vint_mem(b + il, n - il, &sz, &sl, &unk)) return false;
    *size = unk ? UNKNOWN_SIZE : sz;
    *hdr = il + sl;
    return true;
}

static uint64_t uint_at(MkvFile *f, uint64_t off, uint64_t size) {
    uint8_t b[8];
    if (size == 0 || size > 8 || !fread_exact(f, off, b, (int)size)) return 0;
    uint64_t v = 0;
    for (uint64_t i = 0; i < size; i++) v = (v << 8) | b[i];
    return v;
}

static double float_at(MkvFile *f, uint64_t off, uint64_t size) {
    uint8_t b[8];
    if ((size != 4 && size != 8) || !fread_exact(f, off, b, (int)size)) return 0;
    if (size == 4) {
        uint32_t u = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        float fl; memcpy(&fl, &u, 4);
        return fl;
    }
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | b[i];
    double d; memcpy(&d, &u, 8);
    return d;
}

static void string_at(MkvFile *f, uint64_t off, uint64_t size, char *out, size_t cap) {
    size_t n = size < cap - 1 ? (size_t)size : cap - 1;
    if (!fread_exact(f, off, (uint8_t *)out, (int)n)) n = 0;
    out[n] = '\0';
    for (size_t i = 0; i < n; i++) if (out[i] == '\0') { out[i] = '\0'; break; }
}

// Calls fn for every child of the master element occupying [start, end).  fn returns false to stop.
typedef bool (*ChildFn)(MkvFile *f, uint32_t id, uint64_t data_off, uint64_t size, void *user);
static bool walk(MkvFile *f, uint64_t start, uint64_t end, ChildFn fn, void *user) {
    uint64_t pos = start;
    while (pos < end) {
        uint32_t id; uint64_t size; int hdr;
        if (!elem_header(f, pos, &id, &size, &hdr)) return false;
        const uint64_t data = pos + (uint64_t)hdr;
        if (size == UNKNOWN_SIZE || data + size > end) size = data < end ? end - data : 0;   // clamp: never read past the parent
        if (!fn(f, id, data, size, user)) return true;
        pos = data + size;
    }
    return true;
}

// ---- Tracks ----------------------------------------------------------------------------

struct TrackCtx { MkvTrack *t; int enc_count; };

static bool enc_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user);
static bool comp_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    MkvTrack *t = (MkvTrack *)user;
    if (id == ID_COMPALGO) { if (uint_at(f, off, size) != 3) t->unsupported_encoding = true; }
    else if (id == ID_COMPSETTINGS) {
        if (size > MKV_STRIP_MAX) t->unsupported_encoding = true;
        else if (fread_exact(f, off, t->strip, (int)size)) t->strip_len = (int)size;
    }
    return true;
}
static bool enc_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    MkvTrack *t = (MkvTrack *)user;
    if (id == ID_ENCTYPE) { if (uint_at(f, off, size) != 0) t->unsupported_encoding = true; }
    else if (id == ID_ENCENC) t->unsupported_encoding = true;
    else if (id == ID_ENCCOMP) {
        // ContentCompAlgo defaults to 0 (zlib) when absent: header stripping must say 3
        bool saw_algo = false;
        uint64_t p = off;
        while (p < off + size) {
            uint32_t cid; uint64_t csz; int h;
            if (!elem_header(f, p, &cid, &csz, &h)) break;
            if (cid == ID_COMPALGO) saw_algo = true;
            p += (uint64_t)h + csz;
        }
        if (!saw_algo) t->unsupported_encoding = true;
        walk(f, off, off + size, comp_child, t);
    }
    return true;
}
static bool encs_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    TrackCtx *c = (TrackCtx *)user;
    if (id == ID_CONTENTENCODING) {
        if (++c->enc_count > 1) c->t->unsupported_encoding = true;     // stacked encodings: not handled
        walk(f, off, off + size, enc_child, c->t);
    }
    return true;
}
static bool video_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    MkvTrack *t = (MkvTrack *)user;
    if (id == ID_PIXELW) t->width = (int)uint_at(f, off, size);
    else if (id == ID_PIXELH) t->height = (int)uint_at(f, off, size);
    return true;
}
static bool audio_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    MkvTrack *t = (MkvTrack *)user;
    if (id == ID_SAMPLERATE) t->sample_rate = float_at(f, off, size);
    else if (id == ID_CHANNELS) t->channels = (int)uint_at(f, off, size);
    else if (id == ID_BITDEPTH) t->bit_depth = (int)uint_at(f, off, size);
    return true;
}
static bool entry_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    TrackCtx *c = (TrackCtx *)user;
    MkvTrack *t = c->t;
    switch (id) {
    case ID_TRACKNUMBER:     t->number = (int)uint_at(f, off, size); break;
    case ID_TRACKTYPE:       t->type = (int)uint_at(f, off, size); break;
    case ID_FLAGENABLED:     t->enabled = uint_at(f, off, size) != 0; break;
    case ID_FLAGDEFAULT:     t->is_default = uint_at(f, off, size) != 0; break;
    case ID_FLAGFORCED:      t->is_forced = uint_at(f, off, size) != 0; break;
    case ID_DEFAULTDURATION: t->default_duration_ns = uint_at(f, off, size); break;
    case ID_NAME:            string_at(f, off, size, t->name, sizeof t->name); break;
    case ID_LANGUAGE:        string_at(f, off, size, t->language, sizeof t->language); break;
    case ID_LANGUAGEBCP47:   if (t->language[0] == '\0' || !strcmp(t->language, "und")) string_at(f, off, size, t->language, sizeof t->language); break;
    case ID_CODECID:         string_at(f, off, size, t->codec_id, sizeof t->codec_id); break;
    case ID_CODECPRIVATE: {
        t->cp_total = size > 0x7FFFFFFF ? 0x7FFFFFFF : (int)size;
        const int n = size < MKV_CP_MAX ? (int)size : MKV_CP_MAX;
        if (fread_exact(f, off, t->codec_private, n)) t->cp_len = n;
        break;
    }
    case ID_VIDEO:           walk(f, off, off + size, video_child, t); break;
    case ID_AUDIO:           walk(f, off, off + size, audio_child, t); break;
    case ID_CONTENTENCODINGS: walk(f, off, off + size, encs_child, c); break;
    default: break;
    }
    return true;
}
static bool tracks_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    (void)user;
    if (id != ID_TRACKENTRY || f->n_tracks >= MKV_MAX_TRACKS) return true;
    MkvTrack *t = &f->tracks[f->n_tracks];
    memset(t, 0, sizeof *t);
    t->enabled = true; t->is_default = true;
    snprintf(t->language, sizeof t->language, "eng");           // the Matroska default
    TrackCtx c = { t, 0 };
    walk(f, off, off + size, entry_child, &c);
    if (t->number > 0 && t->type > 0) f->n_tracks++;
    return true;
}

// ---- Info, SeekHead, Cues -----------------------------------------------------------------

static bool info_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    (void)user;
    if (id == ID_TIMESCALE) { const uint64_t v = uint_at(f, off, size); if (v) f->timescale_ns = v; }
    else if (id == ID_DURATION) f->duration_ns = float_at(f, off, size);   // ticks for now; scaled by the caller
    return true;
}

struct SeekEntry { uint32_t id; uint64_t pos; };
struct SeekCtx { SeekEntry e[16]; int n; };
static bool seek_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    SeekEntry *e = (SeekEntry *)user;
    if (id == ID_SEEKID) {
        uint8_t b[4]; uint32_t v = 0;
        if (size >= 1 && size <= 4 && fread_exact(f, off, b, (int)size)) for (uint64_t i = 0; i < size; i++) v = (v << 8) | b[i];
        e->id = v;
    } else if (id == ID_SEEKPOS) e->pos = uint_at(f, off, size);
    return true;
}
static bool seekhead_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    SeekCtx *c = (SeekCtx *)user;
    if (id != ID_SEEK || c->n >= 16) return true;
    SeekEntry e = { 0, 0 };
    walk(f, off, off + size, seek_child, &e);
    if (e.id) c->e[c->n++] = e;
    return true;
}

static void cue_add(MkvFile *f, uint64_t time_ns, uint64_t cluster_pos) {
    if (f->n_cues > 0 && time_ns <= f->cues[f->n_cues - 1].time_ns) return;       // keep time ascending
    if (f->n_cues >= f->cap_cues) {
        if (f->cap_cues >= MKV_MAX_CUES) {                                        // full: keep every other one
            int k = 0;
            for (int i = 0; i < f->n_cues; i += 2) f->cues[k++] = f->cues[i];
            f->n_cues = k;
        } else {
            const int cap = f->cap_cues ? f->cap_cues * 2 : 256;
            MkvCue *c = (MkvCue *)realloc(f->cues, (size_t)cap * sizeof(MkvCue));
            if (!c) return;
            f->cues = c; f->cap_cues = cap;
        }
    }
    f->cues[f->n_cues].time_ns = time_ns;
    f->cues[f->n_cues].cluster_pos = cluster_pos;
    f->n_cues++;
}

struct CueCtx { MkvFile *f; uint64_t time_ticks; bool have_time; };
struct PosCtx { int track; uint64_t cluster; bool have_cluster; };
static bool pos_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    PosCtx *p = (PosCtx *)user;
    if (id == ID_CUETRACK) p->track = (int)uint_at(f, off, size);
    else if (id == ID_CUECLUSTERPOS) { p->cluster = uint_at(f, off, size); p->have_cluster = true; }
    return true;
}
static bool cuepoint_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    CueCtx *c = (CueCtx *)user;
    if (id == ID_CUETIME) { c->time_ticks = uint_at(f, off, size); c->have_time = true; }
    else if (id == ID_CUETRACKPOS && c->have_time) {
        PosCtx p = { 0, 0, false };
        walk(f, off, off + size, pos_child, &p);
        if (p.have_cluster && (f->cue_track == 0 || p.track == f->cue_track))
            cue_add(f, c->time_ticks * f->timescale_ns, p.cluster);
    }
    return true;
}
static bool cues_child(MkvFile *f, uint32_t id, uint64_t off, uint64_t size, void *user) {
    (void)user;
    if (id == ID_CUEPOINT) {
        CueCtx c = { f, 0, false };
        walk(f, off, off + size, cuepoint_child, &c);
    }
    return true;
}

// ---- cluster search ------------------------------------------------------------------------

// The first Cluster element at or after file offset `from` (bounded).  *tc = its Timecode, in ticks.
static bool find_cluster(MkvFile *f, uint64_t from, uint64_t limit, uint64_t *pos, int64_t *tc) {
    static const uint8_t magic[4] = { 0x1F, 0x43, 0xB6, 0x75 };
    // the scan buffer is 64 KB: on the heap, since this runs on worker threads whose stack is 128 KB
    const int BUF = 64 * 1024;
    uint8_t *buf = (uint8_t *)malloc((size_t)BUF);
    if (!buf) return false;
    bool found = false;
    uint64_t at = from;
    const uint64_t stop = limit < f->seg_end ? limit : f->seg_end;
    while (!found && at < stop && at - from < SCAN_MAX) {
        const int n = fread_at(f, at, buf, BUF);
        if (n < 4) break;
        for (int i = 0; i + 4 <= n && !found; i++) {
            if (buf[i] != magic[0] || memcmp(buf + i, magic, 4) != 0) continue;
            const uint64_t p = at + (uint64_t)i;
            uint32_t id; uint64_t size; int hdr;
            if (!elem_header(f, p, &id, &size, &hdr) || id != ID_CLUSTER) continue;
            // a real cluster has a Timecode within its first few children
            uint64_t q = p + (uint64_t)hdr;
            for (int k = 0; k < 4; k++) {
                uint32_t cid; uint64_t csz; int ch;
                if (!elem_header(f, q, &cid, &csz, &ch) || csz == UNKNOWN_SIZE) break;
                if (cid == ID_TIMECODE) {
                    *pos = p; *tc = (int64_t)uint_at(f, q + (uint64_t)ch, csz);
                    found = true;
                    break;
                }
                q += (uint64_t)ch + csz;
            }
        }
        at += (uint64_t)(n - 3);
    }
    free(buf);
    return found;
}

// ---- open ------------------------------------------------------------------------------------

static void estimate_duration(MkvFile *f) {
    if (f->duration_ns > 0) return;
    if (f->n_cues > 0) { f->duration_ns = (double)f->cues[f->n_cues - 1].time_ns; return; }
    // the last cluster in the final few megabytes
    const uint64_t from = f->seg_end > SCAN_MAX / 2 ? f->seg_end - SCAN_MAX / 2 : f->first_cluster;
    uint64_t at = from > f->first_cluster ? from : f->first_cluster, pos;
    int64_t tc, best = -1;
    while (find_cluster(f, at, f->seg_end, &pos, &tc)) { best = tc; at = pos + 4; }
    if (best >= 0) f->duration_ns = (double)best * (double)f->timescale_ns;
}

int mkv_open(MkvFile *f, MkvReadAt rd, void *ctx, uint64_t file_size) {
    memset(f, 0, sizeof *f);
    f->rd = rd; f->ctx = ctx; f->file_size = file_size;
    f->timescale_ns = 1000000;
    f->win = (uint8_t *)malloc(WIN_SIZE);
    if (!f->win) { snprintf(f->err, sizeof f->err, "out of memory"); return -1; }

    uint32_t id; uint64_t size; int hdr;
    if (!elem_header(f, 0, &id, &size, &hdr) || id != ID_EBML || size == UNKNOWN_SIZE) {
        snprintf(f->err, sizeof f->err, "not a Matroska file"); mkv_close(f); return -1;
    }
    {   // DocType
        uint64_t p = (uint64_t)hdr, end = (uint64_t)hdr + size;
        while (p < end) {
            uint32_t cid; uint64_t csz; int ch;
            if (!elem_header(f, p, &cid, &csz, &ch)) break;
            if (cid == ID_DOCTYPE) {
                char dt[16];
                string_at(f, p + (uint64_t)ch, csz, dt, sizeof dt);
                f->is_webm = strcasecmp(dt, "webm") == 0;
                if (strcasecmp(dt, "webm") != 0 && strcasecmp(dt, "matroska") != 0) {
                    snprintf(f->err, sizeof f->err, "not a Matroska file"); mkv_close(f); return -1;
                }
            }
            p += (uint64_t)ch + csz;
        }
    }
    uint64_t pos = (uint64_t)hdr + size;
    if (!elem_header(f, pos, &id, &size, &hdr) || id != ID_SEGMENT) {
        snprintf(f->err, sizeof f->err, "no Segment"); mkv_close(f); return -1;
    }
    f->seg_data = pos + (uint64_t)hdr;
    f->seg_end = size == UNKNOWN_SIZE || f->seg_data + size > file_size ? file_size : f->seg_data + size;

    SeekCtx seeks;
    memset(&seeks, 0, sizeof seeks);
    bool have_info = false, have_tracks = false;
    uint64_t cues_off = 0, cues_size = 0;
    int guard = 0;
    pos = f->seg_data;
    while (pos < f->seg_end && guard++ < 4096) {
        if (!elem_header(f, pos, &id, &size, &hdr)) break;
        const uint64_t data = pos + (uint64_t)hdr;
        if (id == ID_CLUSTER) { f->first_cluster = pos; break; }
        if (size == UNKNOWN_SIZE) break;
        switch (id) {
        case ID_SEEKHEAD: walk(f, data, data + size, seekhead_child, &seeks); break;
        case ID_INFO:     walk(f, data, data + size, info_child, NULL); have_info = true; break;
        case ID_TRACKS:   walk(f, data, data + size, tracks_child, NULL); have_tracks = true; break;
        case ID_CUES:     cues_off = data; cues_size = size; break;
        default: break;
        }
        pos = data + size;
    }
    // Info, Tracks or Cues the SeekHead put after the clusters
    for (int i = 0; i < seeks.n; i++) {
        const uint64_t at = f->seg_data + seeks.e[i].pos;
        uint32_t sid; uint64_t ssz; int sh;
        if (at >= f->seg_end || !elem_header(f, at, &sid, &ssz, &sh) || sid != seeks.e[i].id || ssz == UNKNOWN_SIZE) continue;
        const uint64_t data = at + (uint64_t)sh;
        if (sid == ID_INFO && !have_info)     { walk(f, data, data + ssz, info_child, NULL); have_info = true; }
        if (sid == ID_TRACKS && !have_tracks) { walk(f, data, data + ssz, tracks_child, NULL); have_tracks = true; }
        if (sid == ID_CUES && !cues_off)      { cues_off = data; cues_size = ssz; }
    }
    if (f->n_tracks == 0) { snprintf(f->err, sizeof f->err, "no tracks"); mkv_close(f); return -1; }
    if (!f->first_cluster) {
        uint64_t cpos; int64_t tc;
        if (find_cluster(f, f->seg_data, f->seg_end, &cpos, &tc)) f->first_cluster = cpos;
        else { snprintf(f->err, sizeof f->err, "no video data"); mkv_close(f); return -1; }
    }
    f->duration_ns *= (double)f->timescale_ns;                     // the Info stored ticks
    const MkvTrack *v = mkv_first_track(f, MKV_TRACK_VIDEO);
    f->cue_track = v ? v->number : 0;
    if (cues_off) walk(f, cues_off, cues_off + cues_size, cues_child, NULL);
    estimate_duration(f);
    return 0;
}

void mkv_close(MkvFile *f) {
    free(f->win); free(f->cues);
    f->win = NULL; f->cues = NULL; f->n_cues = f->cap_cues = 0; f->win_len = 0;
}

const MkvTrack *mkv_track(const MkvFile *f, int number) {
    for (int i = 0; i < f->n_tracks; i++) if (f->tracks[i].number == number) return &f->tracks[i];
    return NULL;
}

const MkvTrack *mkv_first_track(const MkvFile *f, int type) {
    for (int i = 0; i < f->n_tracks; i++)
        if (f->tracks[i].type == type && f->tracks[i].enabled) return &f->tracks[i];
    return NULL;
}

// ---- codec classes ----------------------------------------------------------------------------

static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

MkvVideoCodec mkv_video_codec(const MkvTrack *t) {
    const char *c = t->codec_id;
    if (!strcmp(c, "V_MPEG4/ISO/AVC")) return MKV_VC_AVC;
    if (!strcmp(c, "V_MPEGH/ISO/HEVC")) return MKV_VC_HEVC;
    if (!strcmp(c, "V_MS/VFW/FOURCC")) return MKV_VC_VC1;           // VC-1 (and others) in VfW form
    if (starts(c, "V_MPEG2")) return MKV_VC_MPEG2;
    if (starts(c, "V_MPEG4")) return MKV_VC_MPEG4;
    if (!strcmp(c, "V_VP9")) return MKV_VC_VP9;
    if (!strcmp(c, "V_AV1")) return MKV_VC_AV1;
    return MKV_VC_OTHER;
}

MkvAudioCodec mkv_audio_codec(const MkvTrack *t) {
    const char *c = t->codec_id;
    if (!strcmp(c, "A_AC3")) return MKV_AC_AC3;
    if (!strcmp(c, "A_EAC3")) return MKV_AC_EAC3;
    if (starts(c, "A_DTS")) return MKV_AC_DTS;
    if (!strcmp(c, "A_TRUEHD")) return MKV_AC_TRUEHD;
    if (!strcmp(c, "A_MPEG/L3")) return MKV_AC_MP3;
    if (!strcmp(c, "A_MPEG/L2") || !strcmp(c, "A_MPEG/L1")) return MKV_AC_MP2;
    if (starts(c, "A_AAC")) return MKV_AC_AAC;
    if (!strcmp(c, "A_FLAC")) return MKV_AC_FLAC;
    if (starts(c, "A_PCM")) return MKV_AC_PCM;
    if (starts(c, "A_VORBIS")) return MKV_AC_VORBIS;
    if (starts(c, "A_OPUS")) return MKV_AC_OPUS;
    return MKV_AC_OTHER;
}

MkvSubCodec mkv_sub_codec(const MkvTrack *t) {
    const char *c = t->codec_id;
    if (!strcmp(c, "S_TEXT/UTF8") || !strcmp(c, "S_TEXT/ASCII")) return MKV_SC_SRT;
    if (!strcmp(c, "S_TEXT/ASS") || !strcmp(c, "S_TEXT/SSA") || !strcmp(c, "S_ASS") || !strcmp(c, "S_SSA")) return MKV_SC_ASS;
    if (!strcmp(c, "S_HDMV/PGS")) return MKV_SC_PGS;
    if (!strcmp(c, "S_VOBSUB")) return MKV_SC_VOBSUB;
    return MKV_SC_OTHER;
}

bool mkv_parse_avcc(const uint8_t *cp, int len, MkvAvcConfig *out) {
    memset(out, 0, sizeof *out);
    if (len < 7 || cp[0] != 1) return false;
    out->nal_length_size = (cp[4] & 3) + 1;
    int p = 5;
    const int ns = cp[p++] & 0x1F;
    for (int i = 0; i < ns; i++) {
        if (p + 2 > len) return false;
        const int l = (cp[p] << 8) | cp[p + 1];
        p += 2;
        if (p + l > len) return false;
        if (out->n_sps < 4 && l <= 256) { memcpy(out->sps[out->n_sps], cp + p, (size_t)l); out->sps_len[out->n_sps++] = l; }
        p += l;
    }
    if (p + 1 > len) return false;
    const int np = cp[p++];
    for (int i = 0; i < np; i++) {
        if (p + 2 > len) return false;
        const int l = (cp[p] << 8) | cp[p + 1];
        p += 2;
        if (p + l > len) return false;
        if (out->n_pps < 4 && l <= 256) { memcpy(out->pps[out->n_pps], cp + p, (size_t)l); out->pps_len[out->n_pps++] = l; }
        p += l;
    }
    return out->n_sps > 0 && out->n_pps > 0;
}

// ---- frames -----------------------------------------------------------------------------------

void mkv_reader_init(MkvReader *r, MkvFile *f, uint8_t *buf, uint32_t cap) {
    memset(r, 0, sizeof *r);
    r->f = f; r->buf = buf; r->cap = cap;
    r->cluster_end = UINT64_MAX;
    r->pos = f->first_cluster;
}

bool mkv_reader_at_cluster(MkvReader *r, uint64_t cluster_pos) {
    const uint64_t at = r->f->seg_data + cluster_pos;
    uint32_t id; uint64_t size; int hdr;
    if (at >= r->f->seg_end || !elem_header(r->f, at, &id, &size, &hdr) || id != ID_CLUSTER) return false;
    r->pos = at;
    r->in_cluster = false; r->eof = false; r->lace_n = r->lace_i = 0;
    return true;
}

static bool level1(uint32_t id) {
    return id == ID_CLUSTER || id == ID_CUES || id == ID_TAGS || id == ID_SEEKHEAD || id == ID_INFO ||
           id == ID_TRACKS || id == ID_CHAPTERS || id == ID_ATTACHMENTS;
}

// Sets up the lacing state for the block payload at buf[off, off+size) (header included).
static bool unpack_block(MkvReader *r, uint32_t off, uint32_t size, bool simple_key, bool is_simple) {
    const uint8_t *b = r->buf + off;
    uint64_t track; int tl;
    if (size < 4 || !vint_mem(b, (int)size, &track, &tl, NULL)) return false;
    if ((uint32_t)tl + 3 > size) return false;
    const int16_t rel = (int16_t)(((uint16_t)b[tl] << 8) | b[tl + 1]);
    const uint8_t flags = b[tl + 2];
    uint32_t p = (uint32_t)tl + 3;
    r->blk_track = (int)track;
    r->blk_pts_ns = (r->cluster_tc + rel) * (int64_t)r->f->timescale_ns;
    if (is_simple) { r->blk_key = (flags & 0x80) != 0; r->blk_discardable = (flags & 0x01) != 0; }
    else { r->blk_key = simple_key; r->blk_discardable = false; }
    const int lacing = (flags >> 1) & 3;
    int n = 1;
    uint32_t sizes[MKV_MAX_LACE];
    const uint32_t total = size;
    if (lacing == 0) {
        sizes[0] = total - p;
    } else {
        if (p >= total) return false;
        n = b[p++] + 1;
        if (lacing == 1) {                                  // Xiph
            uint32_t sum = 0;
            for (int i = 0; i < n - 1; i++) {
                uint32_t s = 0;
                for (;;) { if (p >= total) return false; const uint8_t v = b[p++]; s += v; if (v != 255) break; }
                sizes[i] = s; sum += s;
            }
            if (sum > total - p) return false;
            sizes[n - 1] = total - p - sum;
        } else if (lacing == 3) {                           // EBML: the first size absolute, the rest as differences
            if (n == 1) {
                sizes[0] = total - p;
            } else {
                uint64_t first; int l;
                if (!vint_mem(b + p, (int)(total - p), &first, &l, NULL)) return false;
                p += (uint32_t)l;
                sizes[0] = (uint32_t)first;
                int64_t prev = (int64_t)first;
                uint64_t sum = first;
                for (int i = 1; i < n - 1; i++) {
                    uint64_t raw; int dl;
                    if (!vint_mem(b + p, (int)(total - p), &raw, &dl, NULL)) return false;
                    p += (uint32_t)dl;
                    const int64_t bias = ((int64_t)1 << (7 * dl - 1)) - 1;
                    prev += (int64_t)raw - bias;
                    if (prev < 0) return false;
                    sizes[i] = (uint32_t)prev; sum += (uint64_t)prev;
                }
                if (sum > total - p) return false;
                sizes[n - 1] = (uint32_t)(total - p - sum);
            }
        } else {                                            // fixed size
            const uint32_t rest = total - p;
            if (rest % (uint32_t)n) return false;
            for (int i = 0; i < n; i++) sizes[i] = rest / (uint32_t)n;
        }
    }
    r->lace_n = n; r->lace_i = 0;
    memcpy(r->lace_size, sizes, (size_t)n * sizeof sizes[0]);
    r->lace_off = off + p;
    return true;
}

// Frames in the buffer from `first_hdr`: walks a BlockGroup's children in memory.
static bool unpack_group(MkvReader *r, uint32_t size) {
    uint32_t p = 0, block_off = 0, block_size = 0;
    bool have_block = false, ref = false;
    uint64_t dur_ticks = 0;
    while (p < size) {
        uint32_t id; int il; uint64_t sz; int sl; bool unk = false;
        if (!id_mem(r->buf + p, (int)(size - p), &id, &il)) return false;
        if (!vint_mem(r->buf + p + il, (int)(size - p) - il, &sz, &sl, &unk) || unk) return false;
        const uint32_t data = p + (uint32_t)(il + sl);
        if (sz > size - data) return false;
        if (id == ID_BLOCK) { have_block = true; block_off = data; block_size = (uint32_t)sz; }
        else if (id == ID_REFERENCEBLOCK) ref = true;
        else if (id == ID_BLOCKDURATION) {
            for (uint64_t i = 0; i < sz && i < 8; i++) dur_ticks = (dur_ticks << 8) | r->buf[data + i];
        }
        p = data + (uint32_t)sz;
    }
    if (!have_block) return false;
    if (!unpack_block(r, block_off, block_size, !ref, false)) return false;
    r->blk_dur_ns = dur_ticks * r->f->timescale_ns;
    return true;
}

int mkv_next_frame(MkvReader *r, MkvFrame *out) {
    MkvFile *f = r->f;
    for (;;) {
        if (r->lace_i < r->lace_n) {
            const int i = r->lace_i++;
            uint32_t off = r->lace_off;
            for (int k = 0; k < i; k++) off += r->lace_size[k];
            const MkvTrack *t = mkv_track(f, r->blk_track);
            uint64_t dur = t && t->default_duration_ns ? t->default_duration_ns
                         : (r->blk_dur_ns && r->lace_n ? r->blk_dur_ns / (uint64_t)r->lace_n : 0);
            out->track = r->blk_track;
            out->pts_ns = r->blk_pts_ns + (int64_t)dur * i;
            out->duration_ns = dur;
            out->key = r->blk_key;
            out->discardable = r->blk_discardable;
            out->data = r->buf + off;
            out->size = r->lace_size[i];
            out->prefix = t && t->strip_len ? t->strip : NULL;
            out->prefix_len = t ? t->strip_len : 0;
            return 1;
        }
        r->lace_n = r->lace_i = 0;
        if (r->eof || r->pos >= f->seg_end) return 0;

        uint32_t id; uint64_t size; int hdr;
        if (!elem_header(f, r->pos, &id, &size, &hdr)) {
            // the end of the data, or damage: look for the next cluster
            uint64_t cpos; int64_t tc;
            r->damaged++;
            if (find_cluster(f, r->pos + 1, f->seg_end, &cpos, &tc)) { r->pos = cpos; r->in_cluster = false; continue; }
            r->eof = true;
            return 0;
        }
        const uint64_t data = r->pos + (uint64_t)hdr;

        if (!r->in_cluster) {
            if (id == ID_CLUSTER) {
                r->in_cluster = true;
                r->cluster_end = size == UNKNOWN_SIZE ? UINT64_MAX : data + size;
                r->cluster_tc = 0;
                r->pos = data;
                continue;
            }
            if (size == UNKNOWN_SIZE) { r->eof = true; return 0; }
            r->pos = data + size;                           // Cues, Tags, Void ...
            continue;
        }
        if (r->cluster_end != UINT64_MAX && r->pos >= r->cluster_end) { r->in_cluster = false; continue; }
        if (level1(id) && (r->cluster_end == UINT64_MAX || id == ID_CLUSTER)) { r->in_cluster = false; continue; }
        if (size == UNKNOWN_SIZE) { r->eof = true; return 0; }
        if (r->cluster_end != UINT64_MAX && data + size > r->cluster_end) {       // an element that runs out of its cluster
            r->damaged++; r->in_cluster = false; r->pos = r->cluster_end; continue;
        }

        if (id == ID_TIMECODE) {
            r->cluster_tc = (int64_t)uint_at(f, data, size);
            r->pos = data + size;
        } else if (id == ID_SIMPLEBLOCK || id == ID_BLOCKGROUP) {
            r->pos = data + size;
            if (size > r->cap) { r->oversize++; continue; }
            const int got = fread_at(f, data, r->buf, (int)size);
            if (got < 0) return -1;
            if (got < (int)size) { r->eof = true; return 0; }            // the file ends inside this block
            const bool ok = id == ID_SIMPLEBLOCK ? unpack_block(r, 0, (uint32_t)size, false, true)
                                                 : unpack_group(r, (uint32_t)size);
            if (id == ID_SIMPLEBLOCK) r->blk_dur_ns = 0;
            if (!ok) { r->damaged++; r->lace_n = r->lace_i = 0; }
        } else {
            r->pos = data + size;
        }
    }
}

// ---- seeking ----------------------------------------------------------------------------------

bool mkv_seek(MkvFile *f, MkvReader *r, uint64_t target_ns, uint64_t *cluster_time_ns) {
    if (f->n_cues > 0) {
        int lo = 0, hi = f->n_cues - 1, best = 0;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (f->cues[mid].time_ns <= target_ns) { best = mid; lo = mid + 1; } else hi = mid - 1;
        }
        if (target_ns < f->cues[0].time_ns) best = 0;
        if (mkv_reader_at_cluster(r, f->cues[best].cluster_pos)) {
            *cluster_time_ns = f->cues[best].time_ns;
            return true;
        }
        // a bad cue: fall through to the search
    }
    // no cues: a bounded search for the last cluster that starts at or before the target
    uint64_t lo_pos = f->first_cluster, hi_pos = f->seg_end;
    int64_t tc0 = 0;
    uint64_t tmp;
    find_cluster(f, f->first_cluster, f->seg_end, &tmp, &tc0);
    uint64_t lo_t = (uint64_t)tc0 * f->timescale_ns, hi_t = (uint64_t)f->duration_ns;
    if (target_ns <= lo_t || hi_t <= lo_t) {
        r->pos = lo_pos; r->in_cluster = false; r->eof = false; r->lace_n = r->lace_i = 0;
        *cluster_time_ns = lo_t;
        return true;
    }
    for (int i = 0; i < 40 && hi_pos - lo_pos > (1u << 20); i++) {
        const double frac = (double)(target_ns - lo_t) / (double)(hi_t - lo_t);
        uint64_t guess = lo_pos + (uint64_t)(frac * (double)(hi_pos - lo_pos));
        if (guess <= lo_pos) guess = lo_pos + 1;
        if (guess >= hi_pos) guess = hi_pos - 1;
        uint64_t cpos; int64_t tc;
        if (!find_cluster(f, guess, hi_pos, &cpos, &tc)) { hi_pos = guess; continue; }
        const uint64_t t = (uint64_t)tc * f->timescale_ns;
        if (t <= target_ns) { lo_pos = cpos; lo_t = t; } else { hi_pos = cpos; hi_t = t; }
    }
    // the interval is small now: step cluster by cluster to the last one that starts at or before the target
    for (int i = 0; i < 4096; i++) {
        uint64_t cpos; int64_t tc;
        if (!find_cluster(f, lo_pos + 4, f->seg_end, &cpos, &tc)) break;
        const uint64_t t = (uint64_t)tc * f->timescale_ns;
        if (t > target_ns) break;
        lo_pos = cpos; lo_t = t;
    }
    r->pos = lo_pos; r->in_cluster = false; r->eof = false; r->lace_n = r->lace_i = 0;
    *cluster_time_ns = lo_t;
    return true;
}
