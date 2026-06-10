#pragma once
#include <ppu-types.h>

// Open an HTTP connection to url and read the response headers.
// Returns a connected socket fd on success, -1 on failure.
int stream_open(const char *url);

// Read exactly 'size' bytes, transparently decoding chunked transfer encoding.
// Fully resumable across calls — all state is in static storage.
// Returns:  1 = success
//           0 = timed out (call again after checking buttons)
//          -1 = disconnect or terminal chunk
int stream_read(int sock, u8 *buf, int size);
