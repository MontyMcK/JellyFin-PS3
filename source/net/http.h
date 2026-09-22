#pragma once
#include <stdint.h>

#define HTTP_USER_AGENT  "JellyfinPS3/0.1"
// Response body cap.  This was 128 KB, and PlaybackInfo blew straight past it
// on any item a source plugin (Gelato/AIOStreams and friends) answers for:
// every MediaSource carries its full MediaStreams array, so ~15 of them fill
// 128 KB and the JSON was cut off mid-array — silently.  The version picker
// then showed only the sources that happened to fit, which are the first ones
// the server lists, which is why it looked like "15 entries and all of them
// 4K".  384 KB holds roughly 45 such sources; http_request() now also logs
// when a body is still too big, so this never fails silently again.
#define RESPONSE_SIZE    (384*1024)
#define HTTP_SUCCESS     1
#define HTTP_FAILED      0

int  http_init(void);
void http_end(void);

// HTTP method codes for http_request()'s first argument.
#define HTTP_GET     0
#define HTTP_POST    1
#define HTTP_DELETE  2

// Returns HTTP status code, or -1 on connection failure.
// Response body written to out[0..out_size-1].  `method` is one of the
// HTTP_* codes above (legacy callers pass 0/1 for GET/POST).
int  http_request(int method, const char *url, const char *body,
                  const char *token, char *out, int out_size);

// Fetches a binary resource (e.g. JPEG image) into caller-supplied buffer.
// Returns number of bytes written, or -1 on failure.
int http_fetch_binary(const char *url, const char *token,
                      uint8_t *out, int out_size);
