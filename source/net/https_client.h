#pragma once
// HTTPS for the Jellyfin API (JSON, images), through the firmware's cellHttp and
// cellSsl.  Streaming video is NOT carried over this: the player's socket path
// stays plain HTTP, because decrypting 25 Mbps would compete with decode.
//
// Trust: the firmware CA store is old (no Let's Encrypt root), so a certificate
// that does not verify is handled trust-on-first-use: its MD5 fingerprint is
// pinned per host in jellyfin_tls_pin.txt on the first connection, and a later
// change is REJECTED.  Delete the file to re-pin.
#include <stdint.h>

// Shared firmware HTTPS runtime (modules, pools, CA store), reference counted.
// update_check and the API client both use it.
void https_rt_init(void);        // once, from http_init()
bool https_rt_acquire(void);     // false: could not be brought up
void https_rt_release(void);

bool https_url(const char *url);

// One request.  `auth` is the full Authorization header value.  `body` (POST)
// may be NULL.  The response body goes to out[0..out_size-1], NUL-terminated;
// *body_len is its full length (it may exceed out_size-1 when truncated).
// Returns the HTTP status, or -1 when the connection or handshake failed.
// The caller must serialise calls (http.cpp's request lock).
int https_request(const char *method, const char *url, const char *body,
                  const char *ctype, const char *auth, const char *accept,
                  char *out, int out_size, int *body_len, int timeout_secs);
