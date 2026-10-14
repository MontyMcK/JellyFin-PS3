#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  Resume points for files on a drive
// -------------------------------------------------------------------------
//  A file from a USB drive has no server to remember where it stopped, and the
//  drive itself is never written to, so the position goes to a small text file
//  on the console's own disk.  A file is recognised by its path, size and
//  modification time together, so a replaced file does not resume at the old
//  spot; the table keeps the 500 most recently played files.
//
//  Pure over a table in memory, plus two stdio functions for the file (the
//  tests run them on a temp file).  tests/test_local_resume.cpp.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#define LRES_MAX 500

typedef struct { uint64_t key; uint32_t secs, total; } LResEntry;
typedef struct { LResEntry e[LRES_MAX]; int n; } LResTable;       // oldest first, newest last

// A 64-bit key over the path, the size and the modification time (FNV-1a).  The drive's own
// paths ("usb0:/Films/x.mkv") are part of it: the same file on another port is another entry.
uint64_t lres_key(const char *path, uint64_t size, uint64_t mtime);

// A position worth coming back to: at least 30 s in, and not in the last minute or the last 5 % of
// the file (total 0 = length unknown: only the first rule).
bool lres_worth_resuming(uint32_t secs, uint32_t total);

// Remembers the position (the file becomes the newest entry), or forgets the file when the position
// is not worth resuming from.  The oldest entry goes when the table is full.
void lres_update(LResTable *t, uint64_t key, uint32_t secs, uint32_t total);
void lres_forget(LResTable *t, uint64_t key);
bool lres_find(const LResTable *t, uint64_t key, uint32_t *secs, uint32_t *total);

// Text form: a header line, then one "key secs total" line per entry, oldest first.  Returns the
// length, or -1 when it does not fit in cap (needs about 28 bytes per entry).
int  lres_format(const LResTable *t, char *out, int cap);
// Reads that text.  Lines that are not an entry are skipped; false when the header is missing.
bool lres_parse(LResTable *t, const char *text);

// The table in a file.  Load: false when the file is missing or not ours (the table is then empty).
// Save: written beside the file and swapped in; false when it could not be written.
bool lres_load_file(LResTable *t, const char *path);
bool lres_save_file(const LResTable *t, const char *path);

#ifdef __cplusplus
}
#endif
