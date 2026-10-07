// PGS segment demux + RLE bitmap decode. See subtitles_pgs.h for scope and
// provenance -- the segment layout, RLE codes and colour conversion here are
// cross-checked against ffmpeg's libavcodec/pgssubdec.c, not reconstructed
// from memory of a blog post.

#include "subtitles_pgs.h"
#include <string.h>
#include <stdlib.h>

#define PGS_TYPE_PDS 0x14
#define PGS_TYPE_ODS 0x15
#define PGS_TYPE_PCS 0x16
#define PGS_TYPE_WDS 0x17
#define PGS_TYPE_END 0x80

static pgs_log_fn s_log = NULL;
void pgs_set_log(pgs_log_fn fn) { s_log = fn; }
static void logf_(const char *msg) { if (s_log) s_log(msg); }

// ---- big-endian field reads (the wire format is BE regardless of host) ---
static uint32_t rb16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t rb24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint32_t rb32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

typedef struct {
    uint8_t  type;
    uint32_t pts_90k;
    uint32_t payload_off;
    uint32_t payload_len;
} PgsSeg;

// Reads the segment header at buf[off] into *seg and returns the offset of
// the segment following it, or (uint32_t)len on end-of-buffer, truncation,
// or a bad "PG" magic (desync -- stop rather than scan byte-by-byte for a
// resync point, matching how the rest of this codebase treats a corrupt
// stream: stop cleanly, don't guess).
static uint32_t next_segment(const uint8_t *buf, int len, uint32_t off,
                             PgsSeg *seg) {
    const uint32_t L = (uint32_t)len;
    if (off + 13 > L) return L;
    if (buf[off] != 'P' || buf[off + 1] != 'G') return L;
    seg->pts_90k     = rb32(buf + off + 2);
    seg->type        = buf[off + 10];
    seg->payload_len = rb16(buf + off + 11);
    seg->payload_off = off + 13;
    uint32_t next = seg->payload_off + seg->payload_len;
    if (next > L || next <= off) return L;
    return next;
}

int pgs_build_index(const uint8_t *buf, int len, PgsIndex *out) {
    out->n = 0;
    if (!buf || len <= 0) return 0;
    uint32_t off = 0;
    while (off < (uint32_t)len && out->n < PGS_MAX_EPOCHS) {
        PgsSeg seg;
        uint32_t next = next_segment(buf, len, off, &seg);
        if (next <= off) break;
        if (seg.type == PGS_TYPE_PCS && seg.payload_len >= 11) {
            uint8_t n_objects = buf[seg.payload_off + 10];
            PgsEpoch *e = &out->epoch[out->n++];
            e->start_ms   = seg.pts_90k / 90;
            e->pcs_offset = off;
            e->has_object = (n_objects > 0);
        }
        off = next;
    }
    if (out->n == PGS_MAX_EPOCHS)
        logf_("pgs: index full (PGS_MAX_EPOCHS) -- later cues dropped");
    return out->n;
}

int pgs_find_epoch(const PgsIndex *idx, uint32_t time_ms) {
    if (!idx || idx->n <= 0) return -1;
    int lo = 0, hi = idx->n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (idx->epoch[mid].start_ms <= time_ms) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

// ---- YCbCr (BT.601 or BT.709, by height, matching pgssubdec) -> RGB ------
static void ycbcr_to_rgb(uint8_t y, uint8_t cb, uint8_t cr, bool hd,
                         uint8_t *r, uint8_t *g, uint8_t *b) {
    double Y = y, Cb = (double)cb - 128.0, Cr = (double)cr - 128.0;
    double R, G, B;
    if (hd) {   // BT.709 -- used above 576 lines, matching pgssubdec
        R = Y + 1.5748 * Cr;
        G = Y - 0.1873 * Cb - 0.4681 * Cr;
        B = Y + 1.8556 * Cb;
    } else {    // BT.601
        R = Y + 1.402 * Cr;
        G = Y - 0.344136 * Cb - 0.714136 * Cr;
        B = Y + 1.772 * Cb;
    }
    *r = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R);
    *g = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G);
    *b = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B);
}

// Raw RLE payload for one object, reassembled across continuation ODS
// segments. Capped well above any real cropped-dialogue subtitle's
// compressed size; a bigger one is logged and the epoch is skipped rather
// than growing an unbounded heap allocation for a malformed/hostile stream.
#define PGS_MAX_RLE 262144

bool pgs_decode_epoch(const uint8_t *buf, int len, uint32_t pcs_offset,
                      uint32_t *rgba_out, int rgba_cap, PgsBitmap *out) {
    memset(out, 0, sizeof(*out));
    if (!buf || pcs_offset >= (uint32_t)len) return false;

    PgsSeg pcs;
    if (next_segment(buf, len, pcs_offset, &pcs) <= pcs_offset ||
        pcs.type != PGS_TYPE_PCS || pcs.payload_len < 11) {
        logf_("pgs: decode_epoch called on a non-PCS offset");
        return false;
    }
    const uint8_t *pp = buf + pcs.payload_off;
    int pcs_video_w = (int)rb16(pp + 0);
    int pcs_video_h = (int)rb16(pp + 2);
    uint8_t palette_id = pp[9];
    uint8_t n_objects  = pp[10];
    if (n_objects == 0) return false;   // an explicit "hide" epoch: no bitmap
    if (n_objects > 1) {
        logf_("pgs: epoch has >1 composition object, unsupported -- skipped");
        return false;
    }
    if (pcs.payload_len < 11 + 8) { logf_("pgs: truncated PCS"); return false; }
    const uint8_t *co = pp + 11;
    uint16_t object_id   = (uint16_t)rb16(co + 0);
    uint8_t  cropped_flag = co[3];
    int      obj_x = (int)rb16(co + 4);
    int      obj_y = (int)rb16(co + 6);
    if (cropped_flag & 0x40) {
        logf_("pgs: cropped composition object, unsupported -- skipped");
        return false;
    }

    // Walk this epoch's own segments (PCS exclusive .. next PCS/END) looking
    // for the palette_id and object_id the composition just named. Anything
    // else (a WDS, a PDS/ODS for a DIFFERENT id) is skipped -- see SCOPE.
    uint32_t clut[256];
    memset(clut, 0, sizeof(clut));   // undefined index -> transparent, not garbage
    bool have_palette = false;

    static uint8_t s_rle[PGS_MAX_RLE];
    uint32_t rle_len = 0, rle_total = 0;
    int obj_w = 0, obj_h = 0;
    bool have_object = false, object_done = false;

    uint32_t off = pcs.payload_off + pcs.payload_len;   // just past the PCS
    while (off < (uint32_t)len) {
        PgsSeg seg;
        uint32_t next = next_segment(buf, len, off, &seg);
        if (next <= off) break;
        if (seg.type == PGS_TYPE_PCS || seg.type == PGS_TYPE_END) break;

        if (seg.type == PGS_TYPE_PDS && seg.payload_len >= 2) {
            const uint8_t *p = buf + seg.payload_off;
            if (p[0] == palette_id) {
                uint32_t n = (seg.payload_len - 2) / 5;
                const uint8_t *e = p + 2;
                bool hd = false;   // resolved against object height once known
                for (uint32_t i = 0; i < n; i++, e += 5) {
                    uint8_t idx = e[0], y = e[1], cr = e[2], cb = e[3], a = e[4];
                    uint8_t r, g, b;
                    ycbcr_to_rgb(y, cb, cr, hd, &r, &g, &b);
                    clut[idx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                ((uint32_t)g << 8) | b;
                }
                have_palette = true;
            }
        } else if (seg.type == PGS_TYPE_ODS && seg.payload_len >= 4 &&
                  !object_done) {
            const uint8_t *p = buf + seg.payload_off;
            uint16_t this_id = (uint16_t)rb16(p + 0);
            if (this_id == object_id) {
                // The 0x80 bit is the one thing confirmed straight from
                // ffmpeg's pgssubdec.c: set on the first (or only) segment
                // of an object, clear on a continuation. Completion is NOT
                // taken from a flag bit here -- it is driven by the
                // object_data_length the first segment itself declares
                // (rle_total), which is self-verifying: a truncated or
                // malformed stream just never reaches it rather than
                // silently trusting an unconfirmed "last" bit meaning.
                uint8_t seq = p[3];
                bool first = (seq & 0x80) != 0;
                const uint8_t *data = p + 4;
                uint32_t data_len   = seg.payload_len - 4;
                if (first) {
                    if (data_len < 7) { logf_("pgs: truncated first ODS"); break; }
                    rle_total = rb24(data) - 4;   // object_data_length - w/h fields
                    obj_w = (int)rb16(data + 3);
                    obj_h = (int)rb16(data + 5);
                    data += 7; data_len -= 7;
                    rle_len = 0;
                    have_object = true;
                }
                if (have_object) {
                    if (rle_len + data_len > PGS_MAX_RLE) {
                        logf_("pgs: object exceeds PGS_MAX_RLE -- skipped");
                        have_object = false;
                    } else {
                        memcpy(s_rle + rle_len, data, data_len);
                        rle_len += data_len;
                        if (rle_len >= rle_total) object_done = true;
                    }
                }
            }
        }
        off = next;
    }

    if (!have_palette) { logf_("pgs: no matching palette in this epoch"); return false; }
    if (!have_object || !object_done) {
        logf_("pgs: no complete matching object in this epoch");
        return false;
    }
    if (obj_w <= 0 || obj_h <= 0 || (int64_t)obj_w * obj_h > rgba_cap) {
        out->width = obj_w; out->height = obj_h;   // "try again with this much room"
        return false;
    }
    // Re-run the palette conversion with the correct BT.601/BT.709 switch now
    // that obj_h is known (pgssubdec keys it off decoded picture height).
    if (obj_h > 576) {
        memset(clut, 0, sizeof(clut));
        // Re-walk just the PDS for this palette with hd=true. Cheap: PDS
        // payloads are at most 256*5+2 bytes, and this runs once per epoch.
        uint32_t off2 = pcs.payload_off + pcs.payload_len;
        while (off2 < (uint32_t)len) {
            PgsSeg seg;
            uint32_t next2 = next_segment(buf, len, off2, &seg);
            if (next2 <= off2) break;
            if (seg.type == PGS_TYPE_PCS || seg.type == PGS_TYPE_END) break;
            if (seg.type == PGS_TYPE_PDS && seg.payload_len >= 2) {
                const uint8_t *p = buf + seg.payload_off;
                if (p[0] == palette_id) {
                    uint32_t n = (seg.payload_len - 2) / 5;
                    const uint8_t *e = p + 2;
                    for (uint32_t i = 0; i < n; i++, e += 5) {
                        uint8_t idx = e[0], y = e[1], cr = e[2], cb = e[3], a = e[4];
                        uint8_t r, g, b;
                        ycbcr_to_rgb(y, cb, cr, true, &r, &g, &b);
                        clut[idx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                    ((uint32_t)g << 8) | b;
                    }
                }
            }
            off2 = next2;
        }
    }

    // RLE decode: see subtitles_pgs.h provenance note for the code table.
    memset(rgba_out, 0, (size_t)obj_w * obj_h * 4);
    uint32_t p = 0;
    int col = 0, row = 0;
    while (p < rle_len && row < obj_h) {
        uint8_t b0 = s_rle[p++];
        uint32_t run; uint8_t color;
        if (b0 != 0x00) {
            run = 1; color = b0;
        } else {
            if (p >= rle_len) break;
            uint8_t flags = s_rle[p++];
            run = flags & 0x3F;
            if (flags & 0x40) {
                if (p >= rle_len) break;
                run = (run << 8) | s_rle[p++];
            }
            if (flags & 0x80) {
                if (p >= rle_len) break;
                color = s_rle[p++];
            } else color = 0;
            if (run == 0) { row++; col = 0; continue; }   // end-of-line marker
        }
        if (col + (int)run > obj_w) run = (uint32_t)(obj_w - col);   // defensive clamp
        uint32_t rgba = clut[color];
        uint32_t *dst = rgba_out + (size_t)row * obj_w + col;
        for (uint32_t i = 0; i < run; i++) dst[i] = rgba;
        col += (int)run;
    }

    out->x = obj_x; out->y = obj_y;
    out->width = obj_w; out->height = obj_h;
    out->frame_w = pcs_video_w; out->frame_h = pcs_video_h;
    out->rgba = rgba_out;
    return true;
}
