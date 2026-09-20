#pragma once
// PGS (Presentation Graphic Stream) bitmap subtitle decode -- the "separate
// piece of work" subtitles.h flags. PGS/VOBSUB tracks are bitmaps, not text:
// Jellyfin cannot convert them to SubRip, so until this existed they could
// only be shown by asking the server to burn them into the video, which
// forces a full transcode and throws away the stream-copy path this whole
// fork exists to protect (see subtitles.h). This decodes the bitmap
// on-device instead, the same way subtitles.cpp already does for text.
//
// Pure C, no I/O and no PS3 dependencies: the caller supplies the raw .sup
// elementary-stream bytes and gets back display-set boundaries and decoded
// RGBA pixels. That split is what makes this file host-testable
// (tests/test_subtitles_pgs.c) the same way the audio decoders under
// source/audio/ already are -- the code under test here is the exact code
// the PS3 build compiles, not a copy that can drift from it.
//
// Format reference (segment header, PCS/WDS/PDS/ODS layout, RLE codes,
// YCbCr->RGB) cross-checked against ffmpeg's libavcodec/pgssubdec.c rather
// than any single blog writeup -- this project has been bitten before by
// trusting a format description that turned out subtly wrong on real data.
//
// SCOPE: one composition window, one (uncropped) composition object per
// display set, each epoch fully self-contained -- its own WDS + PDS + ODS
// between its PCS and the next segment boundary. That covers ordinary
// movie/TV dialogue subtitles, which is what this exists for. It does NOT
// support two simultaneously-visible objects (rare -- overlapping on-screen
// dialogue), a cropped composition object, or a display set that reuses an
// object/palette ID defined in an EARLIER epoch without redefining it
// (occasionally used by typesetting-heavy anime subs for a static logo).
// All three are detected and skipped with a log line rather than silently
// misrendering -- see pgs_decode_epoch.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGS_MAX_EPOCHS 4096

typedef struct {
    uint32_t start_ms;     // when this display set becomes active
    uint32_t pcs_offset;   // byte offset of its PCS segment header
    bool     has_object;   // false = explicit "hide subtitle" composition
} PgsEpoch;

typedef struct {
    PgsEpoch epoch[PGS_MAX_EPOCHS];
    int      n;
} PgsIndex;

// Scans every segment in buf[0..len) and records one PgsEpoch per
// Presentation Composition Segment. Cheap: no bitmap decoding happens here,
// only header fields. Returns the number of epochs indexed (0 for a buffer
// with no valid segments -- e.g. Jellyfin returned an HTML error page
// instead of a .sup body, which callers should treat as "no PGS available"
// exactly like subs_load() treats an empty SubRip fetch).
int pgs_build_index(const uint8_t *buf, int len, PgsIndex *out);

// Binary-search idx->epoch for the one active at time_ms, mirroring
// subtitles.cpp's subs_text_at() lookup. Returns -1 if time_ms is before the
// first epoch or the index is empty; otherwise an index into idx->epoch.
// A returned epoch with has_object == false means "subtitles are explicitly
// off right now" -- the caller should stop drawing, not reuse the last frame.
int pgs_find_epoch(const PgsIndex *idx, uint32_t time_ms);

// One decoded bitmap, ready to alpha-blit over the video frame.
typedef struct {
    int      x, y;           // position, in the PCS's OWN coordinate space
                              // (frame_w x frame_h below) -- NOT necessarily
                              // the display's pixel space. A BD disc's PG
                              // stream is authored against a fixed video
                              // size (typically 1920x1080) that can differ
                              // from both the decoded stream's actual pixel
                              // dimensions and the PS3's display output, so
                              // a caller must scale x/y/width/height by
                              // (display_w/frame_w, display_h/frame_h)
                              // before drawing, the same way it already
                              // scales the decoded video frame itself.
    int      width, height;  // bitmap dimensions, in that same space
    int      frame_w, frame_h;  // the PCS's declared video_width/video_height
    uint32_t *rgba;          // points into the caller's rgba_out, top-down,
                              // 0xAARRGGBB, straight (non-premultiplied) alpha
} PgsBitmap;

// Upper bound a caller should grow rgba_out to before giving up on an
// object as unreasonably large (a malformed stream, or a disc doing
// something this decoder's SCOPE does not cover) rather than an unbounded
// realloc. Generous above any ordinary cropped-dialogue subtitle region
// (a full 1920x1080 object would be 8 MB of RGBA, which a subtitle track
// legitimately sending one would be pathological, not typical).
#define PGS_MAX_W 1280
#define PGS_MAX_H 480

// Re-walks the segments belonging to the epoch at buf[pcs_offset..) (its own
// WDS/PDS/ODS, up to the next PCS or END segment) and decodes its one
// composition object into rgba_out, which the caller owns and must size for
// at least rgba_cap pixels.
//
// On success, returns true and fills *out (out->rgba points into rgba_out).
// If rgba_cap is too small for the object, returns false and still fills
// out->width/out->height with the size actually needed so the caller can
// grow its buffer and call again -- the same realloc-on-miss shape
// thumbnail_cache.cpp uses for card bitmaps, so a season with an unusually
// large typeset subtitle costs one extra allocation, not a fixed worst-case
// buffer paid by everyone.
//
// Returns false with out->width == out->height == 0 for anything out of
// SCOPE above (empty composition, cropped object, second object, or a
// missing palette/object definition), after a plog-style caller-supplied
// log callback describes which. Never partially fills rgba_out on failure.
bool pgs_decode_epoch(const uint8_t *buf, int len, uint32_t pcs_offset,
                      uint32_t *rgba_out, int rgba_cap, PgsBitmap *out);

// Log hook: the PS3 build wires this to plog(); the host test leaves it
// NULL. Never called from a hot per-frame path -- only on index build and
// on an actual epoch decode (at most a few times a second).
typedef void (*pgs_log_fn)(const char *msg);
void pgs_set_log(pgs_log_fn fn);

#ifdef __cplusplus
}
#endif
