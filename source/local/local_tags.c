// Music file tags and stream properties: see local_tags.h.

#include "local_tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  Reading the file through a window
// ---------------------------------------------------------------------------

#define WIN_BYTES 16384

typedef struct {
    LaRead   rd;
    void    *ctx;
    uint64_t size;
    uint64_t off;           // where buf starts
    int      len;           // bytes in buf
    uint8_t  buf[WIN_BYTES];
} Win;

// n bytes at off (n <= WIN_BYTES), from the window or by moving it; NULL when the file ends before them
// or a read fails.  The pointer is good until the next call.
static const uint8_t *win_get(Win *w, uint64_t off, int n) {
    if (n <= 0 || n > WIN_BYTES || off > w->size || (uint64_t)n > w->size - off) return NULL;
    if (w->len > 0 && off >= w->off && off + (uint64_t)n <= w->off + (uint64_t)w->len) return w->buf + (off - w->off);
    int want = WIN_BYTES;
    if ((uint64_t)want > w->size - off) want = (int)(w->size - off);
    int got = 0;
    while (got < want) {
        const int r = w->rd(w->ctx, off + (uint64_t)got, w->buf + got, want - got);
        if (r <= 0) break;
        got += r;
    }
    w->off = off;
    w->len = got;
    if (got < n) { w->len = 0; return NULL; }
    return w->buf;
}

// Up to n bytes at off into a fresh buffer (the caller frees it); *got is how many arrived.
static uint8_t *read_block(Win *w, uint64_t off, uint32_t n, uint32_t *got) {
    *got = 0;
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    if (!b) return NULL;
    while (*got < n) {
        const uint32_t want = n - *got > 16384 ? 16384 : n - *got;
        const int r = w->rd(w->ctx, off + *got, b + *got, (int)want);
        if (r <= 0) break;
        *got += (uint32_t)r;
    }
    w->len = 0;
    return b;
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// ---------------------------------------------------------------------------
//  Text
// ---------------------------------------------------------------------------

static bool ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return true;
}

// Appends one code point as UTF-8 when all of it fits (leaving room for the terminator).
static void put_cp(char *dst, int cap, int *n, uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) cp = ' ';
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
    uint8_t b[4];
    int k;
    if (cp < 0x80)         { b[0] = (uint8_t)cp; k = 1; }
    else if (cp < 0x800)   { b[0] = (uint8_t)(0xC0 | (cp >> 6)); b[1] = (uint8_t)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) { b[0] = (uint8_t)(0xE0 | (cp >> 12)); b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (uint8_t)(0x80 | (cp & 0x3F)); k = 3; }
    else                   { b[0] = (uint8_t)(0xF0 | (cp >> 18)); b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (cp & 0x3F)); k = 4; }
    if (*n + k > cap - 1) return;
    for (int i = 0; i < k; i++) dst[(*n)++] = (char)b[i];
}

static void finish_text(char *dst, int n) {
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
    int lead = 0;
    while (dst[lead] == ' ') lead++;
    if (lead) memmove(dst, dst + lead, (size_t)(n - lead + 1));
}

// UTF-8 in (Vorbis comments, ID3 encoding 3): an invalid byte becomes U+FFFD.
static void text_utf8(char *dst, int cap, const uint8_t *s, int len) {
    int n = 0;
    for (int i = 0; i < len;) {
        const uint8_t c = s[i];
        if (c == 0) break;
        uint32_t cp;
        int k;
        if (c < 0x80) { cp = c; k = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; k = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; k = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; k = 4; }
        else { put_cp(dst, cap, &n, 0xFFFD); i++; continue; }
        bool ok = i + k <= len;
        for (int j = 1; ok && j < k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (uint32_t)(s[i + j] & 0x3F);
        }
        if (!ok || (k == 2 && cp < 0x80) || (k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000)) { put_cp(dst, cap, &n, 0xFFFD); i++; continue; }
        put_cp(dst, cap, &n, cp);
        i += k;
    }
    finish_text(dst, n);
}

static void text_latin1(char *dst, int cap, const uint8_t *s, int len) {
    int n = 0;
    for (int i = 0; i < len && s[i]; i++) put_cp(dst, cap, &n, s[i]);
    finish_text(dst, n);
}

static void text_utf16(char *dst, int cap, const uint8_t *s, int len, bool big) {
    int n = 0;
    int i = 0;
    if (len >= 2 && s[0] == 0xFF && s[1] == 0xFE) { big = false; i = 2; }
    else if (len >= 2 && s[0] == 0xFE && s[1] == 0xFF) { big = true; i = 2; }
    for (; i + 1 < len; i += 2) {
        uint32_t u = big ? be16(s + i) : le16(s + i);
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
            const uint32_t lo = big ? be16(s + i + 2) : le16(s + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        put_cp(dst, cap, &n, u);
    }
    finish_text(dst, n);
}

// An ID3 text value: the encoding byte, then the text; the first of several (they are separated by a terminator).
static void id3_text(char *dst, int cap, const uint8_t *p, int len) {
    dst[0] = '\0';
    if (len < 1) return;
    switch (p[0]) {
    case 0:  text_latin1(dst, cap, p + 1, len - 1); break;
    case 1:  text_utf16(dst, cap, p + 1, len - 1, false); break;
    case 2:  text_utf16(dst, cap, p + 1, len - 1, true); break;
    default: text_utf8(dst, cap, p + 1, len - 1); break;
    }
}

// "3", "3/12", " 07 ": the leading number.
static int lead_number(const char *s) {
    while (*s == ' ') s++;
    int v = 0, d = 0;
    while (*s >= '0' && *s <= '9' && d < 6) { v = v * 10 + (*s - '0'); s++; d++; }
    return v;
}

static void mime_of(const uint8_t *s, int len, char *out, int cap) {
    int n = 0;
    for (int i = 0; i < len && s[i] >= 0x20 && s[i] < 0x7F && n < cap - 1; i++) out[n++] = (char)s[i];
    out[n] = '\0';
}

// ---------------------------------------------------------------------------
//  FLAC
// ---------------------------------------------------------------------------

// The artist is ARTIST; ALBUMARTIST stands in when there is none, whichever comes first in the block.
static void vorbis_comments(LaMeta *m, const uint8_t *p, int len) {
    char album_artist[96] = "", artist[96] = "";
    if (len < 8) return;
    const uint32_t vendor = le32(p);
    if (vendor > (uint32_t)len - 8) return;
    int at = 4 + (int)vendor;
    const uint32_t count = le32(p + at);
    at += 4;
    char tmp[64];
    for (uint32_t i = 0; i < count && at + 4 <= len; i++) {
        const uint32_t l = le32(p + at);
        at += 4;
        if (l > (uint32_t)(len - at)) break;
        const uint8_t *kv = p + at;
        const char *k = (const char *)kv;
        at += (int)l;
        int eq = -1;
        for (uint32_t j = 0; j < l; j++) if (kv[j] == '=') { eq = (int)j; break; }
        if (eq <= 0) continue;
        const uint8_t *val = kv + eq + 1;
        const int vl = (int)l - eq - 1;
        if (eq == 5 && ieq(k, "TITLE", 5)) text_utf8(m->title, (int)sizeof m->title, val, vl);
        else if (eq == 6 && ieq(k, "ARTIST", 6)) { if (!artist[0]) text_utf8(artist, (int)sizeof artist, val, vl); }
        else if (eq == 11 && ieq(k, "ALBUMARTIST", 11)) text_utf8(album_artist, (int)sizeof album_artist, val, vl);
        else if (eq == 5 && ieq(k, "ALBUM", 5)) text_utf8(m->album, (int)sizeof m->album, val, vl);
        else if (eq == 11 && ieq(k, "TRACKNUMBER", 11)) { text_utf8(tmp, (int)sizeof tmp, val, vl); m->track_no = lead_number(tmp); }
        else if (eq == 10 && ieq(k, "DISCNUMBER", 10)) { text_utf8(tmp, (int)sizeof tmp, val, vl); m->disc_no = lead_number(tmp); }
    }
    snprintf(m->artist, sizeof m->artist, "%s", artist[0] ? artist : album_artist);
}

// A PICTURE block (len bytes at body; p holds its first `take`): the front cover wins over any other.
static void flac_picture(LaMeta *m, const uint8_t *p, int take, uint32_t len, uint64_t body, bool *have_front) {
    if (take < 32) return;
    const uint32_t kind = be32(p);
    const uint32_t ml = be32(p + 4);
    uint32_t at = 8;
    if (ml > (uint32_t)take - at) return;
    char mime[24];
    mime_of(p + at, (int)(ml > 23 ? 23 : ml), mime, sizeof mime);
    at += ml;
    if (at + 4 > (uint32_t)take) return;
    const uint32_t dl = be32(p + at);
    at += 4;
    if (dl > (uint32_t)take - at) return;
    at += dl;
    if (at + 20 > (uint32_t)take) return;                         // width, height, depth, colours, data length
    at += 16;
    const uint32_t pl = be32(p + at);
    at += 4;
    if (pl == 0 || pl > len - at || !strcmp(mime, "-->")) return;
    if ((kind == 3 && !*have_front) || m->pic_len == 0) {
        m->pic_off = body + at;
        m->pic_len = pl;
        snprintf(m->pic_mime, sizeof m->pic_mime, "%s", mime);
        if (kind == 3) *have_front = true;
    }
}

static bool meta_flac(Win *w, uint64_t size, LaMeta *m) {
    uint64_t off = 0;
    const uint8_t *p = win_get(w, 0, 10);
    if (!p) return false;
    if (!memcmp(p, "ID3", 3)) {                                       // an ID3v2 tag in front of the stream
        off = 10 + (((uint64_t)(p[6] & 0x7F) << 21) | ((uint64_t)(p[7] & 0x7F) << 14) |
                    ((uint64_t)(p[8] & 0x7F) << 7) | (uint64_t)(p[9] & 0x7F)) + ((p[5] & 0x10) ? 10 : 0);
    }
    p = win_get(w, off, 4);
    if (!p || memcmp(p, "fLaC", 4)) return false;
    m->stream_off = off;
    off += 4;
    bool have_info = false, have_front = false;
    for (int blocks = 0; blocks < 4096; blocks++) {
        p = win_get(w, off, 4);
        if (!p) return false;
        const bool last = (p[0] & 0x80) != 0;
        const int type = p[0] & 0x7F;
        const uint32_t len = be24(p + 1);
        const uint64_t body = off + 4;
        if (body + len > size) return false;
        if (type == 0) {
            if (len < 34) return false;
            p = win_get(w, body, 34);
            if (!p) return false;
            const uint64_t v = be64(p + 10);
            m->flac_min_block = (int)be16(p);
            m->flac_max_block = (int)be16(p + 2);
            m->sample_rate = (int)(v >> 44);
            m->channels = (int)((v >> 41) & 7) + 1;
            m->bits = (int)((v >> 36) & 31) + 1;
            m->total_frames = v & 0xFFFFFFFFFULL;
            have_info = true;
        } else if (type == 4) {
            uint32_t got;
            uint8_t *b = read_block(w, body, len > 65536 ? 65536 : len, &got);
            if (b) { vorbis_comments(m, b, (int)got); free(b); }
        } else if (type == 6 && len >= 32) {
            const int take = len > 1024 ? 1024 : (int)len;
            p = win_get(w, body, take);
            if (p) flac_picture(m, p, take, len, body, &have_front);
        }
        off = body + len;
        if (last) break;
    }
    if (!have_info || m->sample_rate <= 0 || m->channels < 1) return false;
    m->data_off = off;
    m->data_len = size > off ? size - off : 0;
    if (m->total_frames) m->duration_secs = (uint32_t)((m->total_frames + (uint64_t)m->sample_rate / 2) / (uint64_t)m->sample_rate);
    return true;
}

// ---------------------------------------------------------------------------
//  MP3
// ---------------------------------------------------------------------------

typedef struct {
    int  version;           // 1 = MPEG 1; 2 = MPEG 2 or 2.5
    int  bitrate;           // kbit/s
    int  rate;
    int  channels;
    int  spf;
    int  bytes;             // the frame's length
} Mp3H;

static bool mp3_header(const uint8_t *h, Mp3H *o) {
    static const int BR1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int BR2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const int RATE1[3] = { 44100, 48000, 32000 };
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    const int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;      // ver: 3 = MPEG 1, 2 = MPEG 2, 0 = MPEG 2.5
    if (ver == 1 || layer != 1) return false;
    const int bi = h[2] >> 4, si = (h[2] >> 2) & 3;
    if (bi == 0 || bi == 15 || si == 3) return false;
    const int pad = (h[2] >> 1) & 1;
    o->version = ver == 3 ? 1 : 2;
    o->bitrate = ver == 3 ? BR1[bi] : BR2[bi];
    o->rate = RATE1[si] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    o->channels = ((h[3] >> 6) & 3) == 3 ? 1 : 2;
    o->spf = ver == 3 ? 1152 : 576;
    o->bytes = (ver == 3 ? 144000 : 72000) * o->bitrate / o->rate + pad;
    return o->bytes >= 8;
}

// A frame header at off, with another right behind it (or the file ending there).
static bool mp3_frame_at(Win *w, uint64_t end, uint64_t off, Mp3H *o) {
    const uint8_t *p = win_get(w, off, 4);
    if (!p || !mp3_header(p, o)) return false;
    const uint64_t next = off + (uint64_t)o->bytes;
    if (next + 4 > end) return true;
    Mp3H n;
    p = win_get(w, next, 4);
    return p && mp3_header(p, &n) && n.rate == o->rate;
}

static uint32_t syncsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) | ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

// Removes the unsynchronisation bytes (FF 00 -> FF) in place; returns the new length.
static int unsync(uint8_t *p, int len) {
    int o = 0;
    for (int i = 0; i < len; i++) {
        p[o++] = p[i];
        if (p[i] == 0xFF && i + 1 < len && p[i + 1] == 0x00) i++;
    }
    return o;
}

// The header of an APIC / PIC frame body: the picture's type, its MIME type, and how many bytes come before the picture.
static bool id3_pic_head(const uint8_t *body, int len, bool v22, int *kind, char *mime, int mime_cap, int *header_bytes) {
    if (len < 6) return false;
    const int enc = body[0];
    int at = 1;
    mime[0] = '\0';
    if (v22) {
        if (!memcmp(body + 1, "JPG", 3)) snprintf(mime, (size_t)mime_cap, "image/jpeg");
        else if (!memcmp(body + 1, "PNG", 3)) snprintf(mime, (size_t)mime_cap, "image/png");
        at = 4;
    } else {
        int e = at;
        while (e < len && body[e]) e++;
        if (e >= len) return false;
        mime_of(body + at, e - at > mime_cap - 1 ? mime_cap - 1 : e - at, mime, mime_cap);
        at = e + 1;
    }
    if (at >= len) return false;
    *kind = body[at++];
    // the description ends with a terminator: one byte for the single-byte encodings, two for UTF-16
    if (enc == 1 || enc == 2) { while (at + 1 < len && !(body[at] == 0 && body[at + 1] == 0)) at += 2; at += 2; }
    else { while (at < len && body[at]) at++; at++; }
    if (at >= len) return false;
    if (!strcmp(mime, "image/jpg")) snprintf(mime, (size_t)mime_cap, "image/jpeg");
    *header_bytes = at;
    return true;
}

static void id3v2(Win *w, uint64_t size, LaMeta *m, uint64_t *tag_end) {
    *tag_end = 0;
    const uint8_t *h = win_get(w, 0, 10);
    if (!h || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4) return;
    const int ver = h[3], flags = h[5];
    uint64_t total = syncsafe(h + 6);
    const uint64_t end = 10 + total + ((flags & 0x10) && ver == 4 ? 10 : 0);
    *tag_end = end > size ? size : end;
    if (10 + total > size) total = size - 10;
    const uint64_t at = 10;                              // the tag body in the file

    // v2.3 with the whole-tag unsynchronisation flag: undo it over the tag first (offsets into the file are then lost)
    uint8_t *whole = NULL;
    int whole_len = 0;
    if (ver == 3 && (flags & 0x80)) {
        if (total > 262144) return;
        uint32_t got;
        whole = read_block(w, at, (uint32_t)total, &got);
        if (!whole) return;
        whole_len = unsync(whole, (int)got);
    }
    const uint64_t body_len = whole ? (uint64_t)whole_len : total;

    uint64_t pos = 0;                                    // into the tag body
    if (flags & 0x40) {                                  // an extended header
        uint8_t eh[4];
        const uint8_t *e = whole ? (whole_len >= 4 ? whole : NULL) : win_get(w, at, 4);
        if (e) { memcpy(eh, e, 4); pos = ver == 4 ? syncsafe(eh) : 4 + (uint64_t)be32(eh); }
    }
    bool have_front = false;
    char num[32];
    for (int frames = 0; frames < 600; frames++) {
        const int hsz = ver == 2 ? 6 : 10;
        uint8_t fh[10] = { 0 };
        if (pos + (uint64_t)hsz > body_len) break;
        if (whole) memcpy(fh, whole + pos, (size_t)hsz);
        else {
            const uint8_t *p = win_get(w, at + pos, hsz);
            if (!p) break;
            memcpy(fh, p, (size_t)hsz);
        }
        if (fh[0] == 0) break;                           // padding
        uint32_t fsize;
        char id[5] = "";
        bool fz = false;                                 // this frame is unsynchronised
        int prefix = 0;                                  // grouping byte, data length indicator: bytes before the value
        bool unreadable = false;                         // compressed or encrypted
        if (ver == 2) { memcpy(id, fh, 3); fsize = be24(fh + 3); }
        else {
            memcpy(id, fh, 4);
            fsize = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
            if (ver == 4) {
                fz = (fh[9] & 0x02) != 0;
                unreadable = (fh[9] & 0x0C) != 0;
                prefix = ((fh[9] & 0x40) ? 1 : 0) + ((fh[9] & 0x01) ? 4 : 0);
            } else {
                unreadable = (fh[9] & 0xC0) != 0;
                prefix = (fh[9] & 0x20) ? 1 : 0;
            }
        }
        const uint64_t fstart = pos + (uint64_t)hsz;
        if (fstart + fsize > body_len) break;
        pos = fstart + fsize;
        if (unreadable || fsize <= (uint32_t)prefix) continue;
        const uint64_t fbody = fstart + (uint64_t)prefix;
        fsize -= (uint32_t)prefix;
        const bool want_text =
            ver == 2 ? (!strcmp(id, "TT2") || !strcmp(id, "TP1") || !strcmp(id, "TP2") || !strcmp(id, "TAL") || !strcmp(id, "TRK") || !strcmp(id, "TPA"))
                     : (!strcmp(id, "TIT2") || !strcmp(id, "TPE1") || !strcmp(id, "TPE2") || !strcmp(id, "TALB") || !strcmp(id, "TRCK") || !strcmp(id, "TPOS"));
        const bool want_pic = ver == 2 ? !strcmp(id, "PIC") : !strcmp(id, "APIC");
        if (!want_text && !want_pic) continue;
        const int take = fsize > 512 ? 512 : (int)fsize;
        uint8_t buf[512];
        if (whole) memcpy(buf, whole + fbody, (size_t)take);
        else {
            const uint8_t *p = win_get(w, at + fbody, take);
            if (!p) continue;
            memcpy(buf, p, (size_t)take);
        }
        int tl = take;
        if (fz) tl = unsync(buf, take);
        if (want_text) {
            if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) id3_text(m->title, (int)sizeof m->title, buf, tl);
            else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) id3_text(m->artist, (int)sizeof m->artist, buf, tl);
            else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) { if (!m->artist[0]) id3_text(m->artist, (int)sizeof m->artist, buf, tl); }
            else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) id3_text(m->album, (int)sizeof m->album, buf, tl);
            else {
                id3_text(num, (int)sizeof num, buf, tl);
                if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) m->track_no = lead_number(num);
                else m->disc_no = lead_number(num);
            }
        } else if (!whole && !fz) {                      // (an unsynchronised picture is not the file's bytes: it cannot be pointed at)
            int kind = 0, header_bytes = 0;
            char mime[24];
            if (id3_pic_head(buf, take, ver == 2, &kind, mime, sizeof mime, &header_bytes) && fsize > (uint32_t)header_bytes &&
                ((kind == 3 && !have_front) || m->pic_len == 0)) {
                m->pic_off = at + fbody + (uint64_t)header_bytes;
                m->pic_len = fsize - (uint32_t)header_bytes;
                snprintf(m->pic_mime, sizeof m->pic_mime, "%s", mime);
                if (kind == 3) have_front = true;
            }
        }
    }
    free(whole);
}

static void id3v1(Win *w, uint64_t size, LaMeta *m, uint64_t *data_end) {
    *data_end = size;
    if (size < 128) return;
    const uint8_t *t = win_get(w, size - 128, 128);
    if (!t || memcmp(t, "TAG", 3)) return;
    *data_end = size - 128;
    if (!m->title[0]) text_latin1(m->title, (int)sizeof m->title, t + 3, 30);
    if (!m->artist[0]) text_latin1(m->artist, (int)sizeof m->artist, t + 33, 30);
    if (!m->album[0]) text_latin1(m->album, (int)sizeof m->album, t + 63, 30);
    if (!m->track_no && t[125] == 0 && t[126] != 0) m->track_no = t[126];
}

static bool meta_mp3(Win *w, uint64_t size, LaMeta *m) {
    uint64_t tag_end = 0, data_end = size;
    id3v2(w, size, m, &tag_end);
    id3v1(w, size, m, &data_end);

    // the first frame: a header with another right behind it
    Mp3H h;
    uint64_t first = tag_end;
    bool found = false;
    for (uint64_t scan = 0; scan < 65536 && first + scan + 4 <= data_end; scan++) {
        const uint8_t *p = win_get(w, first + scan, 4);
        if (!p) break;
        if (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 && mp3_frame_at(w, data_end, first + scan, &h)) { first += scan; found = true; break; }
    }
    if (!found) return false;
    m->sample_rate = h.rate;
    m->channels = h.channels;
    m->bits = 16;
    m->bitrate_kbps = h.bitrate;
    m->mp3_spf = h.spf;
    m->data_off = first;
    m->data_len = data_end > first ? data_end - first : 0;

    // a Xing / Info header in the first frame, with LAME's delay and padding behind it
    uint64_t frames = 0, xbytes = 0;
    const int side = h.version == 1 ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
    const int xo = 4 + side;
    const int fn = data_end - first < 512 ? (int)(data_end - first) : 512;
    const uint8_t *fr = fn > 0 ? win_get(w, first, fn) : NULL;
    bool lame_used = false;
    if (fr && fn >= xo + 8 && (!memcmp(fr + xo, "Xing", 4) || !memcmp(fr + xo, "Info", 4))) {
        const uint32_t fl = be32(fr + xo + 4);
        int at = xo + 8;
        if ((fl & 1) && at + 4 <= fn) frames = be32(fr + at);
        if (fl & 1) at += 4;
        if ((fl & 2) && at + 4 <= fn) xbytes = be32(fr + at);
        if (fl & 2) at += 4;
        if ((fl & 4) && at + 100 <= fn) { memcpy(m->mp3_toc, fr + at, 100); m->mp3_has_toc = true; }
        if (fl & 4) at += 100;
        if (fl & 8) at += 4;
        if (at + 24 <= fn && (fr[at] == 'L' || fr[at] == 'G')) {
            const int d = (fr[at + 21] << 4) | (fr[at + 22] >> 4);
            const int p = ((fr[at + 22] & 0x0F) << 8) | fr[at + 23];
            if (frames > 0 && (uint64_t)(d + p) < frames * (uint64_t)h.spf) {
                m->mp3_skip = d + 529;                   // the encoder's delay and the decoder's own
                m->mp3_gapless = true;
                m->total_frames = frames * (uint64_t)h.spf - (uint64_t)d - (uint64_t)p;
                lame_used = true;
            }
        }
        // the Xing frame itself is not music: the audio starts after it
        m->data_off = first + (uint64_t)h.bytes;
        m->data_len = data_end > m->data_off ? data_end - m->data_off : 0;
    } else if (fr && fn >= 36 + 18 && !memcmp(fr + 36, "VBRI", 4)) {
        xbytes = be32(fr + 36 + 10);
        frames = be32(fr + 36 + 14);
        m->data_off = first + (uint64_t)h.bytes;
        m->data_len = data_end > m->data_off ? data_end - m->data_off : 0;
    }
    if (m->data_off > data_end) { m->data_off = data_end; m->data_len = 0; }
    if (frames > 0) {
        if (!lame_used) m->total_frames = frames * (uint64_t)h.spf;
        m->duration_secs = (uint32_t)((m->total_frames + (uint64_t)h.rate / 2) / (uint64_t)h.rate);
        const uint64_t bytes = xbytes ? xbytes : m->data_len;
        if (m->duration_secs > 0) m->bitrate_kbps = (int)(bytes * 8 / m->duration_secs / 1000);
    } else if (h.bitrate > 0) {                          // constant bit rate: the length follows from the size
        m->duration_secs = (uint32_t)(m->data_len * 8 / ((uint64_t)h.bitrate * 1000));
        m->total_frames = (uint64_t)m->duration_secs * (uint64_t)h.rate;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  WAVE
// ---------------------------------------------------------------------------

static void wav_info(Win *w, uint64_t at, uint32_t len, LaMeta *m) {
    uint64_t off = at + 4;                                      // after "INFO"
    const uint64_t end = at + len;
    for (int i = 0; i < 64 && off + 8 <= end; i++) {
        const uint8_t *p = win_get(w, off, 8);
        if (!p) return;
        char id[5];
        memcpy(id, p, 4);
        id[4] = '\0';
        const uint32_t l = le32(p + 4);
        if (off + 8 + l > end) return;
        const int take = l > 200 ? 200 : (int)l;
        if (l > 0 && (!strcmp(id, "INAM") || !strcmp(id, "IART") || !strcmp(id, "IPRD") || !strcmp(id, "ITRK"))) {
            const uint8_t *s = win_get(w, off + 8, take);
            if (s) {
                if (!strcmp(id, "INAM")) text_utf8(m->title, (int)sizeof m->title, s, take);
                else if (!strcmp(id, "IART")) text_utf8(m->artist, (int)sizeof m->artist, s, take);
                else if (!strcmp(id, "IPRD")) text_utf8(m->album, (int)sizeof m->album, s, take);
                else { char t[32]; text_utf8(t, (int)sizeof t, s, take > 30 ? 30 : take); m->track_no = lead_number(t); }
            }
        }
        off += 8 + (uint64_t)l + (l & 1);
    }
}

static bool meta_wav(Win *w, uint64_t size, LaMeta *m) {
    const uint8_t *p = win_get(w, 0, 12);
    if (!p || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return false;
    uint64_t off = 12;
    bool have_fmt = false, have_data = false;
    for (int chunks = 0; chunks < 256 && off + 8 <= size; chunks++) {
        p = win_get(w, off, 8);
        if (!p) break;
        char id[5];
        memcpy(id, p, 4);
        id[4] = '\0';
        const uint32_t len = le32(p + 4);
        const uint64_t body = off + 8;
        if (!strcmp(id, "fmt ")) {
            if (len < 16 || body + len > size) return false;
            const int take = len > 40 ? 40 : (int)len;
            p = win_get(w, body, take);
            if (!p) return false;
            int format = (int)le16(p);
            m->channels = (int)le16(p + 2);
            m->sample_rate = (int)le32(p + 4);
            m->block_align = (int)le16(p + 12);
            m->bits = (int)le16(p + 14);
            if (format == 0xFFFE && take >= 26) format = (int)le16(p + 24);      // extensible: the sub-format's first two bytes
            m->wav_format = format;
            have_fmt = true;
        } else if (!strcmp(id, "data")) {
            if (!have_fmt) return false;
            m->data_off = body;
            m->data_len = (len == 0 || len == 0xFFFFFFFFu || body + len > size) ? size - body : len;
            have_data = true;
        } else if (!strcmp(id, "LIST") && len >= 4 && body + len <= size) {
            const uint8_t *t = win_get(w, body, 4);
            if (t && !memcmp(t, "INFO", 4)) wav_info(w, body, len, m);
        }
        off = body + len + (len & 1);
    }
    if (!have_fmt || !have_data) return false;
    const bool pcm = m->wav_format == 1 && (m->bits == 8 || m->bits == 16 || m->bits == 24 || m->bits == 32);
    const bool flt = m->wav_format == 3 && (m->bits == 32 || m->bits == 64);
    if (!pcm && !flt) return false;
    if (m->channels < 1 || m->channels > 8 || m->sample_rate < 1000 || m->sample_rate > 400000) return false;
    if (m->block_align != m->channels * (m->bits / 8)) return false;
    m->total_frames = m->data_len / (uint64_t)m->block_align;
    m->duration_secs = (uint32_t)((m->total_frames + (uint64_t)m->sample_rate / 2) / (uint64_t)m->sample_rate);
    return true;
}

// ---------------------------------------------------------------------------

LaKind la_kind_of(const char *name) {
    if (!name) return LAF_NONE;
    const char *dot = strrchr(name, '.');
    if (!dot) return LAF_NONE;
    if (strlen(dot) == 5 && ieq(dot, ".flac", 5)) return LAF_FLAC;
    if (strlen(dot) == 4 && ieq(dot, ".mp3", 4)) return LAF_MP3;
    if (strlen(dot) == 4 && ieq(dot, ".wav", 4)) return LAF_WAV;
    return LAF_NONE;
}

void la_format_line(const LaMeta *m, char *out, int cap) {
    if (cap <= 0) return;
    if (m->kind == LAF_MP3) {
        if (m->bitrate_kbps > 0) snprintf(out, (size_t)cap, "%d kbps MP3", m->bitrate_kbps);
        else snprintf(out, (size_t)cap, "MP3");
        return;
    }
    char rate[16];                                      // kHz with as many decimals as the rate needs: 48, 44.1, 22.05
    if (m->sample_rate % 1000 == 0) snprintf(rate, sizeof rate, "%d", m->sample_rate / 1000);
    else {
        snprintf(rate, sizeof rate, "%d.%03d", m->sample_rate / 1000, m->sample_rate % 1000);
        for (size_t n = strlen(rate); n > 0 && rate[n - 1] == '0'; n--) rate[n - 1] = '\0';
    }
    snprintf(out, (size_t)cap, "%s %s kHz / %d-bit", m->kind == LAF_FLAC ? "FLAC" : "WAV", rate, m->bits);
}

bool la_read_meta(LaRead rd, void *ctx, uint64_t size, LaKind kind, LaMeta *m) {
    memset(m, 0, sizeof *m);
    m->kind = kind;
    if (!rd || kind == LAF_NONE || size < 16) return false;
    Win *w = (Win *)malloc(sizeof *w);
    if (!w) return false;
    w->rd = rd;
    w->ctx = ctx;
    w->size = size;
    w->off = 0;
    w->len = 0;
    bool ok = false;
    switch (kind) {
    case LAF_FLAC: ok = meta_flac(w, size, m); break;
    case LAF_MP3:  ok = meta_mp3(w, size, m); break;
    case LAF_WAV:  ok = meta_wav(w, size, m); break;
    default: break;
    }
    free(w);
    if (!ok) { memset(m, 0, sizeof *m); m->kind = kind; }
    return ok;
}
