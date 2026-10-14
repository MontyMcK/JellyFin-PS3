#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  Local file paths: parsing and the browser's rules
// -------------------------------------------------------------------------
//  Pure (no PS3 headers) so tests/test_lfs_path.cpp compiles the same file the
//  console runs.  A local path is "<drive>:/<folders>/<file>": the drive is
//  "hdd" or "usb0".."usb7".  The part after the colon is slash-separated, has
//  no "." or ".." (they are refused, not resolved) and no empty parts.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Splits "usb0:/Movies/a.mkv" into the drive ("usb0") and the cleaned rest
// ("/Movies/a.mkv"; "/" for the drive's root).  False for a path with no drive,
// an unknown drive, or a "." / ".." part.  `rest` may be NULL.
bool lfs_path_split(const char *path, char *drive, size_t drive_cap, char *rest, size_t rest_cap);

// "usb0:/Movies" + "a.mkv" -> "usb0:/Movies/a.mkv" (one slash between).  False when out is too small.
bool lfs_path_join(const char *dir, const char *name, char *out, size_t cap);

// "usb0:/Movies/a.mkv" -> "usb0:/Movies"; "usb0:/Movies" -> "usb0:/"; a drive root has no parent (false).
bool lfs_path_parent(const char *path, char *out, size_t cap);

// The last part of the path ("a.mkv"), or the drive id for a drive root.
const char *lfs_path_leaf(const char *path);

// The browser shows folders and the files the player can open.
typedef enum { LFS_KIND_OTHER = 0, LFS_KIND_VIDEO, LFS_KIND_MUSIC } lfs_media_kind;
// By extension, case-insensitively: .mkv .ts .m2ts .mts = video; .flac .mp3 .wav = music.
lfs_media_kind lfs_media_kind_of(const char *name);

// Names no listing shows: dot files, "$RECYCLE.BIN" and the other "$..." system names,
// "System Volume Information", ".Trashes", "lost+found", macOS "._x" droppings.
bool lfs_name_hidden(const char *name);

// Natural, case-insensitive order: "Episode 2" before "Episode 10"; digit runs compare as
// numbers (leading zeros ignored, then shorter first); everything else as lower-case bytes
// (UTF-8 bytes order by code point).  <0, 0, >0.
int lfs_natural_cmp(const char *a, const char *b);

// Folders before files, then natural order.
int lfs_entry_cmp(bool a_dir, const char *a_name, bool b_dir, const char *b_name);

#ifdef __cplusplus
}
#endif
