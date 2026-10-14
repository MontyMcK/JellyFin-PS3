#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  lfs: the one file layer for local media
// -------------------------------------------------------------------------
//  Everything the player and the Media browser read from a drive or the
//  internal disk goes through here, so a downloaded file, a FAT32 stick, an
//  NTFS disk and an exFAT disk are the same thing to the code above.  The
//  only writes are to NTFS drives, for the offline downloads (see "writing"
//  below), and the internal /dev_hdd0/jellyfin_local folder, created when
//  missing so there is a place to put files.
//
//  Paths are "<drive>:/folders/file": "hdd" or "usb0".."usb7" (lfs_path.h).
//  The internal disk offers three folders, not the whole disk: "hdd:/video",
//  "hdd:/music" and "hdd:/jellyfin_local".
//
//  Drives come and go: lfs_poll_once() (run by a thread every two seconds, see
//  lfs_start) mounts what is plugged in and drops what is not.  A drive that lv2
//  has mounted (FAT32) is used through lv2; any other is probed for NTFS, then
//  exFAT, through the raw storage layer (third_party/mohasi_fs).  Handles on a
//  drive that is unplugged fail with LFS_E_REMOVED, they never hang or crash:
//  every wait inside is bounded.
//
//  Threading: any thread may call anything.  Reads on NTFS/exFAT take one lock
//  (their readers share scratch buffers), so a slow read delays a listing of
//  another NTFS/exFAT drive; run listings off the render thread.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LFS_HDD = 0,
    LFS_USB_FAT,
    LFS_USB_NTFS,
    LFS_USB_EXFAT,
    LFS_USB_UNSUPPORTED     // plugged in, but a file system this app cannot read
} lfs_kind;

typedef struct {
    char     id[16];        // "hdd", "usb0"
    char     label[48];     // the volume's name, or "USB drive 1"
    lfs_kind kind;
    uint64_t total, free;   // bytes; 0 when unknown
} lfs_drive;

typedef struct {
    char     name[256];
    bool     is_dir;
    uint64_t size;          // files; 0 for folders
    uint64_t mtime;         // unix seconds; 0 when the drive does not say
} lfs_entry;

#define LFS_E_IO        (-1)    // a read or a listing failed
#define LFS_E_REMOVED   (-2)    // the drive was unplugged
#define LFS_E_NOTFOUND  (-3)
#define LFS_E_BADPATH   (-4)
#define LFS_E_TOOMANY   (-5)    // too many files open

// What is present now, internal disk first, then usb0.. in port order.  Returns the count.
int  lfs_drives(lfs_drive *out, int max);
// Bumped whenever a drive appears or disappears, so a screen can poll one number.
uint32_t lfs_generation(void);
// "Internal", "FAT32", "NTFS", "exFAT", "Unsupported".
const char *lfs_kind_name(lfs_kind k);

// The folders and playable files of a directory (hidden names and other files
// left out), folders first, then natural order.  Returns the number of entries
// in the directory (not the number copied), or a LFS_E_ code.  `offset`/`max`
// page through it; a big folder is read once and kept until the path or the set
// of drives changes.  "hdd:/" and every "usbN:/" are valid directories.
int  lfs_list(const char *path, lfs_entry *out, int max, int offset);
// One entry (a file or a folder).  False when it does not exist.
bool lfs_stat(const char *path, lfs_entry *out);

// Files.  lfs_open takes an lfs path, or an internal absolute path ("/dev_hdd0/...",
// where downloads live).  Returns a handle >= 0 or a LFS_E_ code.
int      lfs_open(const char *path);
// Positional read: up to n bytes at `off`.  Returns the bytes read (fewer than
// asked only at the end of the file), 0 at the end, or a LFS_E_ code.
int      lfs_read(int h, uint64_t off, void *buf, uint32_t n);
uint64_t lfs_size(int h);
void     lfs_close(int h);

// ---- writing: NTFS drives only (the offline downloads' store on a USB drive) ------------
// The internal disk is written through lv2 directly (it is not an lfs drive for writing), FAT32
// and exFAT never.  An NTFS drive is writable when it mounted clean: not marked dirty, no pending
// $LogFile recovery, not hibernated by Windows.  While anything is being written the drive's NTFS
// dirty flag is up, and it comes down once the last writer closes -- so a drive pulled mid-write
// is checked by Windows (chkdsk) the next time it is plugged into a PC.  A write that fails leaves
// the flag up and the drive read-only until it is plugged in again: it is never cleared over
// something that may be half done.
#define LFS_E_READONLY  (-6)    // not a writable NTFS drive (or a write already failed on it)
#define LFS_E_EXISTS    (-7)    // rename: the destination is there

// True when the drive holding `path` takes writes now.
bool lfs_drive_writable(const char *path);
// Why it does not, in a few words for a screen ("" when it does): TR()'d.
const char *lfs_drive_why_readonly(const char *path);
int  lfs_mkdir(const char *path);                   // 0, also when it is there already
int  lfs_remove(const char *path);                  // a file; 0, also when it is not there
int  lfs_rmdir(const char *path);                   // an empty folder; 0, also when it is not there
int  lfs_rename(const char *from, const char *to);  // same drive; LFS_E_EXISTS when `to` is there
// Opens a file for appending, creating it when missing; the handle reads too (lfs_read).
int  lfs_open_append(const char *path);
int  lfs_write(int h, const void *buf, uint32_t n); // n, or a LFS_E_ code (and the drive goes read-only)
// Every entry of a folder, unfiltered and unsorted (lfs_list shows media only).  Returns the
// number of entries reported, or a LFS_E_ code.  Works on any drive kind.
int  lfs_list_names(const char *path, void (*cb)(const char *name, bool is_dir, void *ctx), void *ctx);
// Free and total bytes of the drive holding `path`.  False when unknown.
bool lfs_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

// Mount what is plugged in and drop what was removed; call about every two
// seconds (lfs_start does).  Cheap when nothing changed.
void lfs_poll_once(void);
// Start / stop the thread that calls lfs_poll_once.  Safe to call twice.  lfs_stop also closes
// anything still open for writing and unmounts the NTFS drives cleanly (their dirty flag down).
void lfs_start(void);
void lfs_stop(void);
// The clean unmount on its own (lfs_stop calls it; the host tests call it directly).
void lfs_unmount_all(void);
// Called on the polling thread after every poll (about every two seconds), with no lfs lock held:
// the offline downloads use it to follow their store's drive.  One hook; NULL removes it.
void lfs_set_poll_hook(void (*hook)(void));

#ifdef __cplusplus
}
#endif
