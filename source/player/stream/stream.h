#pragma once
#include <ppu-types.h>

// Cooperative abort for stream_open()'s header wait (which can legitimately
// block for a long time while a transcode spins up).  The music player sets
// this before joining its stream thread so Back never hangs the UI; the
// video player leaves it alone.  Always cleared by stream_open on entry's
// caller — set it, join, then clear.
extern volatile bool g_stream_cancel;

// Open an HTTP connection to url and read the response headers.
// Returns a connected socket fd on success, -1 on failure.
int stream_open(const char *url);

// Read exactly 'size' bytes, transparently decoding chunked transfer encoding.
// Fully resumable across calls — all state is in static storage.
// Returns:  1 = success
//           0 = timed out (call again after checking buttons)
//          -1 = disconnect or terminal chunk
int stream_read(int sock, u8 *buf, int size);
