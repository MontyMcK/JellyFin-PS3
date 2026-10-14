#pragma once
#include <stdint.h>
#include "dl_model.h"

// -------------------------------------------------------------------------
//  Offline downloads -- HTTP/1.1 pieces for a resumable GET
// -------------------------------------------------------------------------
//  Pure functions over bytes: no sockets, no files.  The transfer loop in
//  dl_manager.cpp owns the connection and feeds these.
//
//  Why not http.cpp's http_request()?  It reads the WHOLE body into a
//  384 KB buffer, which is right for JSON and useless for a multi-GB film.
//  Why not stream.cpp?  Its reader keeps its state in statics shared with
//  the player, and a download must be able to run beside a stream (or at
//  least never corrupt one).  So the download path owns a small parser of its
//  own -- a streaming one -- and nothing else about HTTP is duplicated: the
//  connection itself still comes from http.cpp (see dl_ps3.cpp).
// -------------------------------------------------------------------------

typedef struct {
    char host[256];
    int  port;
    char path[DL_URL_MAX];   // path + query, starting with '/'
} DlUrl;

// http:// only.  https:// returns false (this client has no TLS on the
// streaming path either), as does anything malformed or too long to hold.
bool dl_url_parse(const char *url, DlUrl *out);

// GET request.  range_from > 0 adds "Range: bytes=<from>-".  auth_header is
// a complete header line without CRLF ("X-Emby-Authorization: ...") or
// NULL.  Returns the request length, or -1 if it does not fit in cap.
int dl_http_build_get(char *out, int cap, const DlUrl *u, uint64_t range_from,
                      const char *auth_header);

typedef struct {
    int      status;           // 0 if the status line was unreadable
    int64_t  content_length;   // -1 = not sent
    bool     chunked;
    bool     accept_ranges;    // "Accept-Ranges: bytes"
    bool     has_range;        // a Content-Range header was present
    bool     range_unsatisfied;// Content-Range: bytes */<total>
    uint64_t range_start;
    uint64_t range_end;
    int64_t  range_total;      // -1 = "*" (unknown)
    char     content_type[64];
} DlHttpHead;

// Parse a complete response head (status line + headers, CRLFCRLF optional).
// Returns false if the status line is not HTTP or a length/range header is
// malformed -- a download must not guess at either.
bool dl_http_parse_head(const char *text, int len, DlHttpHead *out);

// Accumulates the response head across however many reads it arrives in.
#define DL_HEAD_MAX 8192
typedef struct {
    char buf[DL_HEAD_MAX];
    int  n;
    bool done;
} DlHeadReader;

void dl_head_reader_init(DlHeadReader *r);
// Consumes bytes up to and including the blank line that ends the head.
// Returns how many of `len` were consumed (the rest is body), or -1 if the
// head exceeds DL_HEAD_MAX.  r->done is set once the head is complete.
int  dl_head_reader_feed(DlHeadReader *r, const uint8_t *data, int len);

// Transfer-Encoding: chunked, decoded incrementally.  Any split of the
// input -- mid size line, mid CRLF, mid payload -- decodes identically.
typedef struct {
    int      st;          // internal
    uint64_t remain;      // payload bytes left in the current chunk
    int      line_n;
    bool     done;        // terminating 0-size chunk seen
    bool     error;       // malformed framing
} DlChunked;

void dl_chunked_init(DlChunked *c);
// Decodes `in` into `out` (which may be the same buffer: output never runs
// ahead of input).  Returns payload bytes produced, or -1 on malformed
// framing.  Bytes after the terminating chunk are ignored.
int  dl_chunked_decode(DlChunked *c, const uint8_t *in, int len, uint8_t *out);

// Content types a media download refuses outright: an HTML error page or a
// JSON error body with a 200 status must not be saved as a film.
bool dl_http_content_type_is_error_page(const char *ct);

// Value of query parameter `key` in url (exact, case-sensitive name match on
// '?'/'&' boundaries, so "Bitrate" never matches "VideoBitrate").  False if
// absent or too long for out.
bool dl_url_query_get(const char *url, const char *key, char *out, int cap);

// -------------------------------------------------------------------------
//  Is a playback stream light enough to share the network with downloads?
// -------------------------------------------------------------------------
//  Decided from the stream URL the player ACTUALLY built (build_stream_url in
//  player/core/player_session.cpp), so the answer can never disagree with
//  what was requested -- and there is no second capability engine to drift.
//
//  Light means the 480p step or below:
//    MaxHeight <= 480, a VideoBitrate ceiling <= 1.5 Mbps (absent = direct
//    copy at source bitrate = heavy), no HD audio stream copy (a copied
//    TrueHD track alone can run past 10 Mbps), AudioBitrate <= 640 kbps.
//  Anything missing or unparseable counts as heavy: when in doubt, the stream
//  wins.
#define DL_LIGHT_MAX_HEIGHT     480
#define DL_LIGHT_MAX_VIDEO_BPS  1500000u
#define DL_LIGHT_MAX_AUDIO_BPS  640000u
bool dl_stream_is_light(const char *stream_url);
