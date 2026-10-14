#pragma once
#include <ppu-types.h>
#include "mkv_ts.h"        // MkvSubSink

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

// How long the NEXT stream_open() waits for response headers, in seconds; 0
// (the default) is STREAM_HDR_DEADLINE_US.  Applies to one open only.  A live
// channel that has not answered in 20 s is not going to.
void stream_set_header_deadline(unsigned secs);

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
// The same message in the interface language, for the error screen.
const char *stream_last_error_ui(void);

// Read exactly 'size' bytes, transparently decoding chunked transfer encoding.
// Fully resumable across calls — all state is in static storage.
// Returns:  1 = success
//           0 = timed out (call again after checking buttons)
//          -1 = disconnect or terminal chunk
int stream_read(int sock, u8 *buf, int size);
// True when stream_read() has something to return without blocking (see stream.cpp).
bool stream_readable(int sock, int timeout_ms);
// The open connection showed sender backoff and later connections in this run
// use a smaller receive window (stream.cpp): the player should reopen at the
// current position.  _take clears it; stream_open() clears it too.
bool stream_window_reopen_pending(void);
bool stream_window_reopen_take(void);

// Cumulative receive stats since process start: bytes delivered by netRecv,
// microseconds spent blocked inside it, and how many times it was called.
// The heartbeat diffs these to report throughput and blocked-time share.
void stream_rx_stats(u64 *bytes, u64 *wait_us, u32 *calls);

// One-line, non-consuming description of the socket for the lifecycle trace
// (see lclog.h): whether the peer has closed it, whether data is waiting, and
// what stream_read still holds in its own buffers.  Must not be called while
// another thread is reading the socket.
void stream_probe(int sock, char *out, int outsz);

// ---- Local files (offline playback, Stage 4) ------------------------------
// The player's other kind of source: a downloaded media.ts read from the HDD
// through the same buffer and the same stream_read().  The handle is tagged
// so it can never be mistaken for a socket.  offset must be a TS packet
// boundary (stream_local.h picks it).
int  stream_open_file(const char *path, u64 offset);
// A Blu-ray .m2ts (192-byte source packets), served as the 188-byte transport stream inside
// it.  ts_offset counts in that stripped stream (a multiple of 188), as stream_local.h's
// functions do over local_m2ts_read_at.
int  stream_open_m2ts(const char *path, u64 ts_offset);
// A Matroska file, served as the MPEG-TS video/mkv_ts.h makes of it, starting at the key frame
// at or before start_ns (0 = the beginning).  *actual_start_ns is where that frame is in the
// file.  audio_track is an MKV track number (0 = no audio).  The parsed file stays open across
// opens of the same path until stream_local_release().
// sub_track (0 = none) and sink: the blocks of that subtitle track go to the sink as the file is read (mkv_ts.h).
int  stream_open_mkv(const char *path, int video_track, int audio_track, int sub_track, MkvSubSink sub_sink,
                     void *sub_ctx, u64 start_ns, u64 *actual_start_ns);
// End of a playback session: closes the Matroska file and frees its frame buffer.  Safe to call
// whenever, and twice.
void stream_local_release(void);
bool stream_is_file(int h);
// The open file's last read failed because its drive was unplugged (not a clean end).
bool stream_file_removed(void);
// Close either kind: netClose for a socket, exactly as before.
void stream_close(int h);
// SO_RCVTIMEO for a socket, exactly as before; nothing for a file.
void stream_set_timeout(int h, u32 usec);
// The stream's 256 KB buffer as scratch -- only while no stream is being
// read (before an open, or mid-seek after the close).  Offline seeking
// probes the file with it instead of allocating.
u8  *stream_scratch(int *cap);
