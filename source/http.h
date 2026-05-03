#pragma once

#define HTTP_USER_AGENT  "JellyfinPS3/0.1"
#define RESPONSE_SIZE    (128*1024)
#define HTTP_SUCCESS     1
#define HTTP_FAILED      0

int  http_init(void);
void http_end(void);

// Returns HTTP status code, or -1 on connection failure.
// Response body written to out[0..out_size-1].
int  http_request(int is_post, const char *url, const char *body,
                  const char *token, char *out, int out_size);
