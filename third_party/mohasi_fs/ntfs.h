#pragma once
// jf-port: vendored from mohasi/ps3-dev (Apache-2.0); see README.md in this directory.  Reads and
// writes.  Comments below that mention the VFS backend are the original's: here the caller is
// source/local/lfs.cpp, which holds the one lock around every call.

// ntfs - NTFS reader for the PS3 file manager. Sibling of exfat.h.
//
// Hand-written from the NTFS on-disk specification (docs/ntfs), with the
// read-path parsing (MFT / attributes / runlists / $I30 index) cross-checked
// against libfsntfs for identical input. Registers with the VFS as a backend
// exactly the way exFAT does: a probe/release/shutdown trio plus initNtfs(),
// called from initVfs() next to initExfat(). Covers the read path (mount,
// directory listing, stat, free space, file read of removable USB volumes via
// the lv2 storage manager); the write path lands in a later stage.
//
// The volume may sit at LBA 0 (superfloppy) or inside an MBR or GPT partition;
// mount finds it either way and records the partition's start sector in
// partitionOffset, which every absolute read is shifted by.
//
// Endianness: NTFS is little-endian on disk, the PPU is big-endian, so every
// multi-byte on-disk field is read through the explicit readLe16/readLe32/
// readLe64 helpers - never by reading a packed struct field directly.
//
// Sizes, cluster numbers (LCN/VCN) and runlist deltas are 64-bit throughout.

#include <stdint.h>

// One decoded runlist element: the cluster range [vcn, vcn+count) maps to LCNs starting at lcn.
// lcn < 0 marks a sparse run (a hole that reads as zeros). Runlists are decoded into bounded
// fixed arrays (no heap), so a pathologically fragmented attribute is rejected rather than grown.
#define NTFS_MFT_RUNS_MAX 32   // $MFT fragments we cache; a healthy MFT has only a handful
typedef struct {
   uint64_t vcn;
   uint64_t count;
   int64_t  lcn;   // < 0 => sparse hole
} NtfsRunEntry;

// A mounted NTFS volume on one storage drive (drive index == USB port). Geometry
// and the MFT location come from $Boot; the rest is filled as later stages parse
// the MFT. Mirrors ExfatVolume's shape and memory discipline.
typedef struct {
   int      mounted;
   int      storageHandle;       // lv2 storage handle
   uint32_t cacheEpoch;          // per-mount cache key (lv2 recycles storageHandle; see mountEpoch in ntfs.c)
   uint8_t  drive;               // storage drive index (USB port)
   uint32_t deviceSectorSize;    // physical sector size used for storage I/O
   uint32_t bytesPerSector;      // NTFS logical sector (validated == device size)
   uint32_t sectorsPerCluster;   // sectors per cluster
   uint32_t bytesPerCluster;     // bytesPerSector * sectorsPerCluster (cached)
   uint64_t partitionOffset;     // volume start sector on the device (0 if superfloppy)
   uint64_t totalSectors;        // total sectors in the volume (from $Boot)
   uint64_t mftLcn;              // first cluster of $MFT
   uint64_t mftMirrLcn;          // first cluster of $MFTMirr
   uint64_t mftDataSize;         // $MFT's own $DATA real size (bytes); record count = this / mftRecordSize
   NtfsRunEntry mftRuns[NTFS_MFT_RUNS_MAX];   // decoded $MFT runlist (VCN->LCN), for reading any record
   int      mftRunCount;         // number of valid entries in mftRuns
   NtfsRunEntry bitmapRuns[NTFS_MFT_RUNS_MAX];   // decoded cluster-$Bitmap runlist, for direct alloc/free I/O
   int      bitmapRunCount;      // valid entries in bitmapRuns; 0 => uncached (volume forced read-only)
   uint64_t bitmapDataSize;      // $Bitmap's $DATA real size; bounds the free-cluster scan (0 if uncached)
   uint64_t allocHint;           // next-free-cluster scan hint for allocateClusters (avoids rescanning the filled prefix)
                                 // jf-port: the STREAM cursor; smallHint is the cursor for everything else
   uint64_t smallHint;           // jf-port: scan start for small allocations (records, artwork, index blocks)
   int      allocStream;         // jf-port: set by appendData for the one allocateClusters call that follows
   uint64_t freeClusters;        // free cluster count: seeded once at mount, maintained on alloc/free (O(1) getNtfsFree)
   uint32_t mftRecordSize;       // bytes per FILE record (from the signed power-of-two field)
   uint32_t indexRecordSize;     // bytes per index buffer (from the signed power-of-two field)
   uint64_t volumeSerial;        // 64-bit volume serial number
   char     label[64];           // volume label, UTF-8 ("" until $Volume is parsed)
   char     segment[32];         // VFS mount segment chosen for this volume (label or "ntfs<port>")
   int      writable;            // 1 if mounted clean (writes allowed); 0 if the on-disk volume was dirty
   int      volumeDirty;         // 1 once we set the on-disk dirty flag this session (cleared on clean unmount)
   // NTFS version from $VOLUME_INFORMATION (major.minor, e.g. 3.1; 0.0 if absent). These are RAW,
   // ATTACKER-CONTROLLED on-disk bytes: do NOT trust them to imply on-disk structure (e.g. "major==3
   // => $Secure/$UsnJrnl present"). Any consumer must independently validate whatever it infers.
   uint8_t  versionMajor;
   uint8_t  versionMinor;
} NtfsVolume;

// Normalized metadata for one entry. mftReference locates the entry's FILE record
// (used to descend into a directory or open a file). validSize is ValidDataLength:
// bytes in [validSize, size) read as zero.
typedef struct {
   uint64_t size;          // data length in bytes ($DATA real size)
   uint64_t mtime;         // last-modified time, unix seconds (UTC; from FILETIME)
   int      isDir;
   uint64_t mftReference;  // 8-byte MFT file reference (index in low 48 bits)
   uint64_t validSize;     // valid data length: bytes in [validSize, size) read as zero
   int      isReparse;     // W12b: entry is a reparse point (symlink/junction/placeholder), not an ordinary file/dir
   uint32_t reparseTag;    // W12b: reparse tag (e.g. 0xA000000C symlink) when isReparse; 0 otherwise
   uint32_t attributes;    // DOS/Win32 FILE_ATTRIBUTE_* flags from $FILE_NAME (read-only/hidden/system/archive/...)
} NtfsInfo;

// A directory iterator over the $I30 index ($INDEX_ROOT + $INDEX_ALLOCATION).
// Holds only a position so it stays small enough to pool per open dir. The B-tree
// walk is iterative and bounded (no recursion); fields are finalized in S6.
typedef struct {
   const NtfsVolume *vol;
   uint64_t dirReference;   // MFT reference of the directory being listed
   uint64_t indexVcn;       // current $INDEX_ALLOCATION block VCN being scanned (~0 = root)
   uint32_t entryOffset;    // byte offset of the next index entry within the current node
   uint32_t blocksWalked;   // index blocks visited so far, to bound a cyclic/corrupt index
   int      inRoot;         // 1 while iterating the resident $INDEX_ROOT node
   int      ioError;        // a read failed mid-walk: surface as -1 at the VFS boundary
} NtfsDir;

// On-disk location of an entry for write-back (later stages): the MFT reference
// and the attribute id of the $DATA/$FILE_NAME being updated.
typedef struct {
   uint64_t mftReference;
   uint16_t attributeId;
} NtfsEntryLoc;

// An open file. Caches the most recently located runlist cursor so sequential
// reads don't re-decode the runlist from the start each time. Runlist details are
// finalized in S4/S5.
typedef struct {
   NtfsVolume *vol;         // non-const: the write path mutates volume metadata
   uint64_t mftReference;
   uint64_t size;           // $DATA real size
   uint64_t validSize;      // valid data length; reads past it return zero
   uint64_t position;
   int      resident;       // $DATA is resident (stored inside the FILE record)
   int      compressed;     // $DATA is $LZNT1-compressed / sparse / encrypted -> write-refused
   uint32_t compUnitClusters; // $LZNT1 compression unit in clusters (>0 => decode on read via readCompressed)
   int      writable;       // opened for writing (W1 overwrite-in-place)
   int      dirty;          // data size changed since open: sync the $FILE_NAME size copies on close
   int      spanned;        // $DATA runlist spread across records via $ATTRIBUTE_LIST (W8: read via gatherRuns)
   uint16_t dataAttrId;     // attribute id of the $DATA stream
   uint16_t dataName[32];   // W12a: named-stream ($DATA) name in UTF-16 ("" => the unnamed main stream)
   uint8_t  dataNameLen;    // W12a: length of dataName in UTF-16 units (0 => unnamed stream)
} NtfsFile;

// mountNtfs results: success, a transient not-ready device (retry), or a device
// that read fine but isn't NTFS (e.g. exFAT/FAT32 - hand to the next backend).
#define NTFS_MOUNT_OK         0
#define NTFS_MOUNT_NOT_READY  (-1)
#define NTFS_MOUNT_NOT_NTFS   (-2)

// THREADING / LOCKING INVARIANT (identical to exFAT)
// None of the functions below take any lock and they are NOT thread-safe on their own: they share
// static scratch buffers and caches across all volumes and handles. They are safe ONLY because every
// caller reaches them through the NTFS VFS backend (the *Op wrappers in ntfs.c), each of which holds
// the single global backend mutex (ntfsLock) for the whole call. Anyone calling these directly must
// hold ntfsLock for the entire call, and must not re-enter it (the mutex is non-recursive: no *Op
// calls another *Op).

// Mounts the NTFS volume on storage drive `drive` (USB port) into `vol`. Returns one of the
// NTFS_MOUNT_* results above.
int  mountNtfs(NtfsVolume *vol, int drive);
void unmountNtfs(NtfsVolume *vol);

// Directory listing over the $I30 index. openNtfsDir positions the iterator at the
// start of the directory identified by `dirReference`; readNtfsDir fills one entry
// per call: returns 1 with name/info set, 0 at end of directory, -1 on error.
void openNtfsDir(NtfsDir *dir, const NtfsVolume *vol, uint64_t dirReference);
int  readNtfsDir(NtfsDir *dir, char *name, int nameCap, NtfsInfo *info);
void closeNtfsDir(NtfsDir *dir);

// Resolves an in-volume path ("/" or "/a/b/c") to its metadata. Returns 0 if found, -1 otherwise.
int  statNtfs(const NtfsVolume *vol, const char *path, NtfsInfo *info);

// One parsed $LogFile RESTART_PAGE_HEADER (after its MULTI_SECTOR_HEADER signature/USA).
typedef struct {
   uint16_t usaOffset, usaCount;
   uint64_t chkdskLsn;
   uint32_t systemPageSize, logPageSize;
   uint16_t restartOffset, minorVersion, majorVersion;
} NtfsLogRestartPage;
// Validate the restart-page signature (RSTR/RCRD/CHKD) and parse the header. Returns 0 or -1.
int  logParseRestartPage(const uint8_t *data, uint32_t len, NtfsLogRestartPage *out);

// Opens a file by in-volume path for reading. Returns 0 on success, -1 if not found or it's a
// directory, -2 if the data stream is compressed/unsupported (distinct from corruption).
int  openNtfs(NtfsFile *file, NtfsVolume *vol, const char *path);
int  readNtfs(NtfsFile *file, void *buffer, int length);   // bytes read, or -1
// Write (W1 overwrite-in-place + W2 append/grow): overwrites within initialized data and/or appends
// at end-of-file, growing the file (resident, or non-resident with cluster allocation, promoting a
// resident file when it outgrows the record). Returns bytes written, or -1. Refuses a read-only
// mount, a compressed stream, a named ($DATA) stream (read-only - the write path only tracks the
// unnamed main stream), or a non-contiguous (gapped) write.
int  writeNtfs(NtfsFile *file, const void *buffer, int length);
// W2 truncate: shrinks the file to `newSize`, freeing the clusters past it (truncate-to-zero leaves
// an empty resident $DATA). Grow is done via writeNtfs. Returns 0 / -1. Refuses (-1) a compressed
// stream, a named ($DATA) stream, or a $DATA that spans $ATTRIBUTE_LIST extents (read-only here).
int  truncateNtfs(NtfsFile *file, uint64_t newSize);

// W3 create: makes an empty file / directory at an in-volume path (parent must exist). Allocates an
// MFT record and links a $FILE_NAME into the parent's $I30 index. Returns 0, -2 if it already
// exists, or -1 on error / refusal (no free MFT record, or the index node would have to split).
int  createNtfsPath(NtfsVolume *vol, const char *path);
int  mkdirNtfsPath(NtfsVolume *vol, const char *path);

// W4 delete: removes a file (unlink) or an empty directory (rmdir) at an in-volume path. Returns 0,
// -2 if already absent (idempotent), or -1 on error / refusal (non-empty or large-index directory).
int  unlinkNtfsPath(NtfsVolume *vol, const char *path);
int  rmdirNtfsPath(NtfsVolume *vol, const char *path);

// W5 rename/move (same volume): relinks a file or directory from `from` to `to`. Moves the
// $FILE_NAME entry between the parents' $I30 indexes and rewrites the file's $FILE_NAME (parent ref +
// name), preserving timestamps/sizes. Returns 0, -2 if the destination exists, or -1 on error /
// refusal (read-only, missing parent, large-index parent, or a source with a DOS short-name alias).
int  renameNtfsPath(NtfsVolume *vol, const char *from, const char *to);
void seekNtfs(NtfsFile *file, uint64_t position);
int  closeNtfs(NtfsFile *file);

// Free / total bytes from $Bitmap. Returns 0 on success, -1 on error.
int  getNtfsFree(const NtfsVolume *vol, uint64_t *freeBytes, uint64_t *totalBytes);

// jf-port: clears the on-disk dirty flag this session set; call with the lock held once no writer
// is open on the volume (what the original's clearDirtyIfQuiet did).  Returns 0 or -1.
int  ntfsClearDirty(NtfsVolume *vol);
