#pragma once

// jf-port: vendored from mohasi/ps3-dev (Apache-2.0); see README.md in this directory.
// READ ONLY.  The write API (write, truncate, create, mkdir, delete, rename) and the VFS
// backend half of the original are removed, and a volume is never written.  Comments below
// that describe writes or the VFS backend are the original's and no longer apply.
// Threading: the functions share static scratch buffers, so the caller (source/local/lfs.cpp)
// holds ONE lock around every call into this file, as the original's *Op wrappers did.

// exfat - minimal exFAT reader/writer for the PS3 file manager.
//
// Hand-written from the Microsoft exFAT specification, with the directory
// entry-set layout, timestamp format and cluster math cross-checked against
// ChaN's FatFs (the lib this replaced). Covers what the browser needs: mount,
// directory listing, stat, free space, and file read/write/create/delete/rename
// of removable USB volumes via the lv2 storage manager. No FAT12/16/32 and no
// formatting, and no libc (uses simple-lib-core's memCopy / utf16ToUtf8 etc.)
// so it links cleanly into prx plugins as well as apps.
//
// The volume may sit at LBA 0 (superfloppy) or inside an MBR or GPT partition;
// mount finds it either way and records the partition's start sector in
// partitionOffset, which every absolute read is shifted by.
//
// Endianness: exFAT is little-endian on disk, the PPU is big-endian, so every
// multi-byte field is read through the explicit readLe16/readLe32/readLe64 helpers.

#include <stdint.h>

// A mounted exFAT volume on one storage drive (drive index == USB port).
typedef struct {
   int      mounted;
   int      storageHandle;                 // lv2 storage handle
   uint32_t cacheEpoch;         // per-mount cache key (lv2 recycles storageHandle; see mountEpoch in exfat.c)
   uint8_t  drive;              // storage drive index (USB port)
   uint32_t deviceSectorSize;   // physical sector size used for storage I/O
   uint32_t bytesPerSector;     // exFAT logical sector (validated == device size)
   uint32_t sectorsPerCluster;  // sectors per cluster
   uint64_t partitionOffset;    // volume start sector on the device (0 if superfloppy)
   uint32_t fatOffset;          // first FAT, in sectors from volume start
   uint32_t clusterHeapOffset;  // data region, in sectors from volume start
   uint32_t clusterCount;       // number of clusters in the heap
   uint32_t rootCluster;        // first cluster of the root directory
   uint32_t bitmapCluster;      // first cluster of the allocation bitmap (0 if not located)
   uint64_t bitmapBytes;        // allocation bitmap length in bytes
   uint32_t upcaseCluster;      // first cluster of the up-case table (0 if not located)
   uint64_t upcaseBytes;        // up-case table length in bytes
   uint32_t allocHint;          // cluster to start the next bitmap allocation from (0 = start)
   uint32_t freeClusters;       // running count of free clusters (seeded at mount, kept by markClusterRun); getExfatFree returns it O(1)
   char     label[36];          // volume label, UTF-8 ("" if none); up to 11 UTF-16 units
   char     segment[32];        // VFS mount segment chosen for this volume (label or "exfat<port>")
   int      volumeDirty;        // 1 once the on-disk VolumeDirty flag was set this session (cleared on clean unmount)
} ExfatVolume;

// Normalized metadata for one entry. firstCluster/noFatChain are the location of
// the entry's data (used to descend into a directory or open a file).
typedef struct {
   uint64_t size;          // data length in bytes (DataLength)
   uint64_t mtime;         // last-modified time, unix seconds (UTC)
   int      isDir;
   uint32_t firstCluster;  // first cluster of the data (0 if empty)
   int      noFatChain;    // data clusters are contiguous (skip the FAT)
   uint64_t validSize;     // valid data length: bytes in [validSize, size) read as zero (ValidDataLength)
} ExfatInfo;

// A directory iterator. Holds only a position (entry data is read on demand
// through a shared sector cache), so it stays small enough to pool per open dir.
typedef struct {
   const ExfatVolume *vol;
   uint32_t cluster;        // current cluster of the chain (< 2 => exhausted)
   uint32_t sectorInClu;    // sector within the current cluster
   uint32_t entryInSector;  // 32-byte entry within the current sector
   int      noFatChain;     // directory clusters are contiguous (skip the FAT)
   uint32_t clustersWalked; // clusters visited so far, to bound a cyclic/corrupt directory FAT
   uint32_t clusterLimit;   // max clusters the directory may span (0 = unbounded; from DataLength)
   int      ioError;        // a sector read failed mid-walk: surface as -1 at the VFS boundary
} ExfatDir;

// On-disk location of a directory entry set: the position of its File (0x85) entry
// and the number of entries in the set. Lets the writer flush updated size/allocation
// back into the set without re-scanning the directory.
typedef struct {
   uint32_t cluster;
   uint32_t sectorInClu;
   uint32_t entryInSector;
   int      count;
   int      dirNoFatChain;   // chain mode of the directory holding the set (for stepping)
} ExfatEntryLoc;

// An open file. Caches the most recently located cluster so sequential reads
// don't re-walk the FAT from the start each time.
typedef struct {
   ExfatVolume *vol;        // non-const: writing mutates the bitmap / FAT / entry set
   uint32_t firstCluster;
   uint64_t size;
   uint64_t validSize;      // valid data length; reads past it return zero (kept == size by our writes)
   uint64_t position;
   int      noFatChain;
   uint32_t cachedIndex;    // cluster index currently held in cachedCluster
   uint32_t cachedCluster;
   uint32_t allocClusters;  // clusters currently allocated (>= ceil(size/clusterBytes); any extra is
                            // over-reservation for a streaming write, released on close)
   ExfatEntryLoc entry;     // this file's entry set, for flushing writes back
   int      writable;       // opened for writing
   int      appendMode;     // O_APPEND: every write repositions to end-of-file first
   int      dirty;          // size/allocation changed since open -> flush on close
} ExfatFile;

// mountExfat results: success, a transient not-ready device (retry), or a device that read fine
// but isn't exFAT (e.g. FAT32 - don't retry it on the hot path, that just blinks the USB LED).
#define EXFAT_MOUNT_OK         0
#define EXFAT_MOUNT_NOT_READY  (-1)
#define EXFAT_MOUNT_NOT_EXFAT  (-2)

// THREADING / LOCKING INVARIANT
// None of the functions below take any lock and they are NOT thread-safe on their own: they share
// static scratch buffers and caches across all volumes and handles, so two running at once would
// corrupt that state. The shared statics (all in exfat.c, all touched only under the lock) are:
// the dir/FAT/file sector caches and the boot/write scratch buffers; the up-case table; the
// end-of-directory cache (and its round-robin victim index); the per-mount epoch counter; the
// last-read entry-set position (lastSet*); and the rename set buffers (oldSet/newSet). They are safe
// ONLY because every caller reaches them through the exFAT VFS backend (the *Op wrappers in exfat.c),
// each of which holds the single global backend mutex (exfatLock) for the whole call. Anyone calling
// these directly must hold exfatLock for the entire call, and must not re-enter it (the mutex is not
// recursive: no *Op calls another *Op).

// Mounts the exFAT volume on storage drive `drive` (USB port) into `vol`. Returns one of the
// EXFAT_MOUNT_* results above.
int  mountExfat(ExfatVolume *vol, int drive);
void unmountExfat(ExfatVolume *vol);

// Directory listing. `firstCluster`/`noFatChain` come from the entry of the
// directory (the root uses rootCluster with noFatChain = 0). `byteLength` is the
// directory's DataLength, used to bound the walk (0 = unbounded, for the root whose
// size no entry records). readExfatDir fills one entry per call: returns 1 with
// name/info set, or 0 at end of directory.
void openExfatDir(ExfatDir *dir, const ExfatVolume *vol, uint32_t firstCluster, int noFatChain, uint64_t byteLength);
int  readExfatDir(ExfatDir *dir, char *name, int nameCap, ExfatInfo *info);
void closeExfatDir(ExfatDir *dir);

// Resolves an in-volume path ("/" or "/a/b/c") to its metadata. Case-insensitive
// (ASCII). Returns 0 if found, -1 otherwise.
int  statExfat(const ExfatVolume *vol, const char *path, ExfatInfo *info);

// Opens a file by in-volume path for reading. Returns 0 on success, -1 if not
// found or the path is a directory.
int  openExfat(ExfatFile *file, ExfatVolume *vol, const char *path);
int  readExfat(ExfatFile *file, void *buffer, int length);   // bytes read, or -1
void seekExfat(ExfatFile *file, uint64_t position);
int  closeExfat(ExfatFile *file);   // releases the handle; 0

// Free / total bytes from the allocation bitmap. Returns 0 on success, -1 on error.
int  getExfatFree(const ExfatVolume *vol, uint64_t *freeBytes, uint64_t *totalBytes);
