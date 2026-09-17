#pragma once
#include <ppu-types.h>

// Cooperative abort for stream_open()'s header wait (which can legitimately
// block for a long time while a transcode spins up).  The music player sets
// this before joining its stream thread so Back never hangs the UI; the
// video player leaves it alone.  Always cleared by stream_open on entry's
// caller — set it, join, then clear.
extern volatile bool g_stream_cancel;

// Called roughly twice a second while stream_open() is waiting for response
// headers.  Return false to abort the open.
//
// Without this the app simply stops: switching quality makes the server start
// a fresh transcode, which can take tens of seconds, and stream_open() would
// sit there for up to STREAM_HDR_DEADLINE_US with nothing drawn and no way
// out.  That is indistinguishable from a crash, and was reported as one.
typedef bool (*stream_wait_fn)(unsigned elapsed_ms);
void stream_set_wait_cb(stream_wait_fn cb);

// Open an HTTP connection to url and read the response headers.
// Returns a connected socket fd on success, -1 on failure.
int stream_open(const char *url);

// Why the last stream_open() failed, in words fit for the error screen
// ("Server returned HTTP 400", "Could not connect to 192.168.0.5:8096",
// "Server did not respond in 120s").  "Stream connection failed" on its own
// cannot tell a refused connection from a 400 caused by a bad MediaSourceId,
// and those need completely different fixes.  Valid until the next
// stream_open().
const char *stream_last_error(void);

// Read exactly 'size' bytes, transparently decoding chunked transfer encoding.
// Fully resumable across calls — all state is in static storage.
// Returns:  1 = success
//           0 = timed out (call again after checking buttons)
//          -1 = disconnect or terminal chunk
int stream_read(int sock, u8 *buf, int size);
