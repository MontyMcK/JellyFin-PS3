#pragma once
#include <stdint.h>

// -------------------------------------------------------------------------
//  Offline downloads -- platform seam
// -------------------------------------------------------------------------
//  Everything the download core needs from the machine it runs on, and
//  nothing else.  The core (dl_model / dl_http / dl_store / dl_manager) is
//  plain C++ with no PSL1GHT headers, so the SAME code the console runs is
//  compiled on the host by tests/Makefile.host and driven there against a
//  scripted fake server and a real temporary directory.
//
//  Two implementations exist and they are chosen at link time, not by a
//  vtable:
//    source/offline/dl_ps3.cpp       -- the console (libnet + lv2 fs + lv2
//                                        mutex), compiled by the main Makefile
//    tests/dl_platform_fake.cpp      -- host tests only
//
//  Small on purpose.  This is not a portability framework; it is the list of
//  calls that differ between a PS3 and a Linux box.
// -------------------------------------------------------------------------

// dl_plat_recv() outcomes other than a byte count.
#define DL_RECV_CLOSED    0     // peer closed the connection
#define DL_RECV_TIMEOUT  (-1)   // nothing arrived within the socket timeout
#define DL_RECV_ERROR    (-2)   // connection reset / hard error

// dl_plat_free_bytes() when the filesystem will not say.
#define DL_FREE_UNKNOWN  UINT64_MAX

// ---- network: one outgoing TCP connection per handle -------------------
// Resolves the host (name or dotted quad) and connects with a bounded wait.
// Returns a handle >= 0, or -1 when the server cannot be reached.  The
// handle's receive timeout is short (about a second) so the transfer loop
// can notice a pause/cancel promptly; idle deadlines are the core's job.
int  dl_plat_connect(const char *host, int port);
// Sends everything or fails.  Returns len, or -1.
int  dl_plat_send(int h, const void *buf, int len);
// >0 bytes received, or one of the DL_RECV_* codes above.
int  dl_plat_recv(int h, void *buf, int cap);
void dl_plat_close(int h);

// ---- filesystem ---------------------------------------------------------
// Paths are absolute lv2 paths ("/dev_hdd0/...") or, for a store on a USB
// drive, lfs paths ("usb0:/...", source/local/lfs.h) -- the console side
// routes each call by the path.  Small text records are whole-file reads and
// writes; the MEDIA file can pass 4 GB, so it gets explicit 64-bit calls.
bool     dl_plat_mkdir(const char *path);          // true if it exists after
bool     dl_plat_rmdir(const char *path);          // empty directory only
bool     dl_plat_remove(const char *path);         // true if gone after
bool     dl_plat_rename(const char *from, const char *to);
int64_t  dl_plat_file_size(const char *path);      // -1 if missing
bool     dl_plat_exists(const char *path);         // file or directory
bool     dl_plat_truncate(const char *path);       // to 0 bytes, creating it
// A small file whole (records, artwork): read up to cap bytes, returning the
// count or -1 when it cannot be opened; written by creating/replacing it.
int      dl_plat_read_file(const char *path, void *buf, int cap);
bool     dl_plat_write_file(const char *path, const void *data, int len);
// Calls cb once per sub-directory name of path ("." and ".." skipped).
// Returns the number of entries reported, or -1 if path cannot be read.
int      dl_plat_list_dirs(const char *path,
                           void (*cb)(const char *name, void *ctx), void *ctx);
// Free bytes on the filesystem holding path, or DL_FREE_UNKNOWN when the
// filesystem will not say (the call failed, the block size came back 0, the
// library that answers is not loaded).  Never a guess: the manager treats
// unknown as "no downloads", so a wrong number is worse than none.
uint64_t dl_plat_free_bytes(const char *path);
// The optional user setting that RAISES the free-space reserve, in whole
// GB; 0 when absent or unreadable.  The manager clamps it to DL_RESERVE_MIN
// from below, so no value here can lower the floor.
int      dl_plat_reserve_gb(void);

// Media file, append-only.  Opened positioned at end-of-file.
int  dl_plat_file_open_append(const char *path);   // handle >= 0, or -1
int  dl_plat_file_write(int fh, const void *buf, int len);  // len, or -1
bool dl_plat_file_sync(int fh);
void dl_plat_file_close(int fh);

// ---- time, locking, logging --------------------------------------------
uint64_t dl_plat_now_ms(void);        // monotonic, any epoch
void     dl_plat_sleep_ms(unsigned ms);
void     dl_plat_lock(void);          // guards the manager's item table
void     dl_plat_unlock(void);
void     dl_plat_log(const char *line);

// ---- the worker thread ----------------------------------------------------
// One thread, started once by dl_service.cpp.  The host fake does not run
// it: tests drive dl_svc_tick() directly.
bool dl_plat_thread_start(void (*fn)(void));
void dl_plat_thread_join(void);
bool dl_plat_app_running(void);       // false once the app is quitting
