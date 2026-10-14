// exfat.c - minimal exFAT reader for the PS3 file manager. See exfat.h.
//
// jf-port: vendored from mohasi/ps3-dev (Apache-2.0), see third_party/mohasi_fs/README.md.
// The write path and the VFS backend half are removed; this file never writes a sector.
//
// Hand-written from the Microsoft exFAT specification; the directory entry-set
// layout, timestamps and cluster math were cross-checked against ChaN's FatFs
// (the lib this replaced). The lv2 storage syscall numbers, argument order and
// device-info struct are taken verbatim from the in-tree references
// (apps/ManaGunZ/payloads/rawseciso/storage.h, apps/xai_plugin functions.cpp):
//   600 open, 601 close, 602 read, 609 get_device_info.
// No libc: storage I/O goes through jf_port.h's four storage calls and the
// string helpers (memCopy / memSet / utf16ToUtf8 / strCmpICase), so this links
// into prx plugins too.

#include "exfat.h"
#include "jf_port.h"          // jf-port: storage, timing and string helpers (replaces vfs/storage-device/syscall/thread/string-utilities/rtc)
#include <malloc.h>           // jf-port: memalign for the scratch block (was sysMemAllocate)

#define STORAGE_BUSY       0x80010002u   // lv2 "device not ready" (settling / ejected)
#define SYSIO_RETRY        8
#define SYSIO_RETRY_US     50000
#define SYSIO_SETTLE_US    62500         // settle gap after open, before the first read

#define STORAGE_ALIGN      32            // lv2 storage DMA buffer alignment
#define EXFAT_MAX_SECTOR   4096          // largest sector we support
#define EXFAT_READ_BOUNCE  32768         // file-read bounce: up to this many bytes
                                         // (one ~32 KB cluster) per sys_storage_read,
                                         // instead of one 512-byte sector at a time

#define DIR_ENTRY_BYTES    32
#define MAX_SET_ENTRIES    19            // File + Stream + ceil(255/15)=17 Name entries
#define ENTRY_END          0x00          // end of directory
#define ENTRY_BITMAP       0x81          // allocation bitmap
#define ENTRY_FILE         0x85          // file/dir entry (in-use)
#define ENTRY_STREAM       0xC0          // stream extension
#define ENTRY_NAME         0xC1          // file name fragment
#define ATTR_DIRECTORY     0x10
#define FLAG_NO_FAT_CHAIN  0x02          // GeneralSecondaryFlags: contiguous data
#define EXFAT_EOC          0xFFFFFFFFu

#define MBR_PART_TABLE     446
#define MBR_PART_ENTRIES   4
#define MBR_PART_SIZE      16
#define MBR_TYPE_GPT       0xEE   // protective-MBR partition type that marks a GPT disk
#define GPT_HEADER_LBA     1      // the GPT header occupies the second sector
#define GPT_MAX_ENTRIES    128    // cap on the partition-entry scan (the GPT default)
#define GPT_ENTRY_MIN_SIZE 128    // smallest valid GPT partition-entry size
// Fallback ceiling for partition-table LBAs when the device size is unknown (getStorageInfo failed,
// deviceSectors == 0): 2^36 sectors is >=32 TB at 512-byte sectors - past any real removable medium,
// yet finite, so a hostile partition entry can't steer a scan read to an arbitrary 64-bit sector.
#define EXFAT_SCAN_LBA_CAP (1ull << 36)

// device ids, info and raw sector I/O come from jf_port.h.

// little-endian readers (exFAT is LE on disk, the PPU is big-endian).
static uint16_t readLe16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t readLe32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t readLe64(const uint8_t *p) { return (uint64_t)readLe32(p) | ((uint64_t)readLe32(p + 4) << 32); }

// Shared, 32-byte-aligned scratch. Only touched while the (single) caller holds
// the exFAT backend's lock, so these are safe to share across volumes/handles.
// Per-mount epoch: an id assigned to each successful mount, used as the shared-cache key instead of
// the lv2 storageHandle (which lv2 RECYCLES after closeStorage, so a reused handle could otherwise
// validate a stale cache entry against a different volume). Each mount gets an id no earlier mount
// had, so a cache entry can never match a later mount even if a reset were missed. 0 = "none" sentinel.
static uint32_t mountEpoch;          // last epoch handed out (first mount gets 1; wraps skip 0)

// All large scratch lives in ONE heap block allocated on first mount and freed when the last volume
// unmounts (ensureScratch / releaseScratchIfIdle), so an idle system with no USB inserted holds none of
// it. Pointers below carve that block; they are valid only while a volume is mounted. mount holds
// exfatLock so these shared buffers are safe, and keeping them off the stack lets the hotplug poll
// thread run on a small stack.
static uint8_t *dirSector;       // cached directory sector (lba/epoch below as before)
static uint64_t dirSectorLba = ~0ULL;
static uint32_t dirSectorEpoch = 0;
static uint8_t *fatSector;       // cached FAT sector
static uint64_t fatSectorLba = ~0ULL;
static uint32_t fatSectorEpoch = 0;
static uint8_t *fileSector;      // single-sector scratch for getExfatFree + ~32 KB read bounce
static uint8_t *metaScratch;    // sector scratch for the bitmap / FAT / directory reads at mount
static uint8_t *bootSector;      // boot-sector scratch for mountExfat

// Up-case casefold table, sparse: only the code points that differ from identity are stored (a standard
// exFAT $UpCase has ~900 of 65536), as parallel sorted-by-code-point arrays. exFAT casefold and NameHash
// map through upcaseOf() instead of re-reading the on-disk table on every create / rename / lookup.
// Reset on mount and unmount, not by per-operation cache invalidation, so a burst of writes keeps it warm.
#define EXFAT_UPCASE_MAX 2048    // headroom over the ~900 non-identity entries a standard $UpCase defines
static uint16_t *upcaseCp;       // code points whose up-cased form differs (ascending)
static uint16_t *upcaseUp;       // their up-cased forms (parallel to upcaseCp)
static uint32_t  upcaseCount;
static uint32_t  upcaseTableEpoch = 0;

// One 64 KB-page heap block holds every buffer above. lv2 rejects a SYS_PAGE_64K request whose size is
// not a 64 KB multiple, so round the ~56 KB the buffers need up to a single 64 KB page (which is also the
// smallest page sys_memory_allocate offers). Costs one page while any volume is mounted, nothing otherwise.
#define EXFAT_SCRATCH_USED  (4u * EXFAT_MAX_SECTOR + EXFAT_READ_BOUNCE + 4u * EXFAT_UPCASE_MAX)
#define EXFAT_SCRATCH_BYTES ((EXFAT_SCRATCH_USED + 0xFFFFu) & ~0xFFFFu)
// keep the working set inside a single 64 KB page: growing it past one page would break the VSH budget
typedef char exfatScratchFitsOnePage[(EXFAT_SCRATCH_BYTES <= 0x10000u) ? 1 : -1];
static void *scratchBlock;       // jf-port: the aligned allocation behind the buffers above (0 = none)
static int   mountedCount;       // jf-port: volumes mounted, so the last one out frees the block

// Allocates and carves the shared scratch block if it isn't already. Returns 0 / -1.
static int ensureScratch(void)
{
   if (scratchBlock) return 0;
   uint8_t *base = (uint8_t *)memalign(128, EXFAT_SCRATCH_BYTES);   // jf-port: was sysMemAllocate
   if (!base) return -1;
   scratchBlock = base;
   dirSector    = base;
   fatSector    = base + 1u * EXFAT_MAX_SECTOR;
   metaScratch = base + 2u * EXFAT_MAX_SECTOR;
   bootSector   = base + 3u * EXFAT_MAX_SECTOR;
   fileSector   = base + 4u * EXFAT_MAX_SECTOR;
   uint8_t *up  = fileSector + EXFAT_READ_BOUNCE;       // EXFAT_MAX_SECTOR is a multiple of 32: u16-aligned
   upcaseCp     = (uint16_t *)up;
   upcaseUp     = (uint16_t *)(up + 2u * EXFAT_UPCASE_MAX);
   upcaseCount  = 0;
   dirSectorLba = fatSectorLba = ~0ULL;                 // the carved caches start empty
   return 0;
}

// jf-port: replaces the pool-owned release the VFS half used to provide.
static void releaseScratchIfIdle(void)
{
   if (mountedCount > 0 || !scratchBlock) return;
   free(scratchBlock);
   scratchBlock = 0;
   dirSector = fatSector = fileSector = metaScratch = bootSector = 0;
   upcaseCp = upcaseUp = 0;
   upcaseCount = 0;
}

static void invalidateCaches(void)
{
   dirSectorLba = ~0ULL;
   dirSectorEpoch = 0;
   fatSectorLba = ~0ULL;
   fatSectorEpoch = 0;
}

// Reads `count` sectors at `lba` into a 32-byte-aligned buffer, retrying while the
// device reports "not ready" (hotplug settling). Every caller passes an aligned
// static buffer or aligned local, which is what lv2 storage DMA requires.
// Returns 0 on success, -1 on a hard error.
static int readSectors(int storageHandle, uint64_t lba, uint32_t count, void *aligned)
{
   for (int attempt = 0; attempt < SYSIO_RETRY; attempt++) {
      uint32_t got = 0;
      int rc = readStorageRaw(storageHandle, lba, count, aligned, &got);
      if (rc == 0 && got == count) return 0;
      if ((uint32_t)rc != STORAGE_BUSY) return -1;
      sys_timer_usleep(SYSIO_RETRY_US);
   }
   return -1;
}

static uint32_t getClusterBytes(const ExfatVolume *vol)
{
   return vol->bytesPerSector * vol->sectorsPerCluster;
}

// first absolute sector (LBA) of a data cluster (clusters are numbered from 2).
static uint64_t clusterToSector(const ExfatVolume *vol, uint32_t cluster)
{
   return vol->partitionOffset + (uint64_t)vol->clusterHeapOffset
        + (uint64_t)(cluster - 2) * vol->sectorsPerCluster;
}

static int isClusterValid(const ExfatVolume *vol, uint32_t cluster)
{
   // <= 0xFFFFFFF6 keeps the bad-cluster (0xFFFFFFF7) and EOC markers out of the valid range
   // explicitly, so a FAT entry holding one is never followed as a real cluster.
   return cluster >= 2 && cluster <= 0xFFFFFFF6u && (cluster - 2) < vol->clusterCount;
}

// LBA of the FAT sector holding `cluster`'s 32-bit entry, with its byte offset within.
static uint64_t getFatEntryLba(const ExfatVolume *vol, uint32_t cluster, uint32_t *within)
{
   uint64_t byteOffset = (uint64_t)cluster * 4;
   *within = (uint32_t)(byteOffset % vol->bytesPerSector);
   return vol->partitionOffset + (uint64_t)vol->fatOffset + byteOffset / vol->bytesPerSector;
}

// Ensures the shared FAT read-cache (fatSector) holds the sector at `lba`. Returns 0 / -1.
static int loadFatSector(const ExfatVolume *vol, uint64_t lba)
{
   if (lba != fatSectorLba || vol->cacheEpoch != fatSectorEpoch) {
      if (readSectors(vol->storageHandle, lba, 1, fatSector) != 0) return -1;
      fatSectorLba = lba;
      fatSectorEpoch  = vol->cacheEpoch;
   }
   return 0;
}

// Follows the FAT one link. Returns the next cluster, or 0 at end-of-chain.
static uint32_t getNextCluster(const ExfatVolume *vol, uint32_t cluster)
{
   uint32_t within;
   uint64_t lba = getFatEntryLba(vol, cluster, &within);
   if (loadFatSector(vol, lba) != 0) return 0;
   uint32_t next = readLe32(fatSector + within);
   return (next == EXFAT_EOC || !isClusterValid(vol, next)) ? 0 : next;
}

// Converts an exFAT timestamp to unix seconds (UTC). exFAT stores LOCAL time plus a per-field
// UtcOffset byte (bit 7 = valid, bits 0-6 = signed offset in 15-minute units); `tzoffset` is that
// byte, applied here to normalize the local time to UTC. A byte of 0 (no offset recorded) leaves
// the value as-is, matching how other readers treat an unanchored timestamp.
static uint64_t timestampToUnix(uint32_t timestamp, uint8_t tzoffset)
{
   uint32_t sec  = (timestamp & 0x1F) * 2;
   uint32_t min  = (timestamp >> 5) & 0x3F;
   uint32_t hour = (timestamp >> 11) & 0x1F;
   uint32_t day  = (timestamp >> 16) & 0x1F;
   uint32_t mon  = (timestamp >> 21) & 0x0F;
   uint32_t year = 1980 + ((timestamp >> 25) & 0x7F);
   if (mon < 1)  mon = 1;
   if (mon > 12) mon = 12;   // a corrupt/foreign month (the field holds 0-15) must not feed garbage
   if (day < 1)  day = 1;    // into the civil-days math below
   if (day > 31) day = 31;

   // days from civil (Howard Hinnant's algorithm), epoch 1970-01-01.
   int y = (int)year - (mon <= 2);
   int era = (y >= 0 ? y : y - 399) / 400;
   uint32_t yoe = (uint32_t)(y - era * 400);
   uint32_t doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
   uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   long days = (long)era * 146097 + (long)doe - 719468;
   int64_t unixTime = (int64_t)((uint64_t)days * 86400 + hour * 3600 + min * 60 + sec);

   if (tzoffset & 0x80) {                              // OffsetValid: convert local -> UTC
      int offsetQuarters = (int)(int8_t)(tzoffset << 1) / 2;   // sign-extend the 7-bit field
      unixTime -= (int64_t)offsetQuarters * 15 * 60;
   }
   return (uint64_t)unixTime;
}

// true if buffer holds an exFAT boot sector ("EXFAT   " at offset 3, sig 0xAA55).
static int hasExfatBoot(const uint8_t *boot)
{
   static const char tag[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
   for (int i = 0; i < 8; i++) {
      if (boot[3 + i] != (uint8_t)tag[i]) return 0;
   }
   return boot[510] == 0x55 && boot[511] == 0xAA;
}

// true if a sector begins with the GPT header signature "EFI PART".
static int hasGptHeader(const uint8_t *sector)
{
   static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
   for (int i = 0; i < 8; i++) {
      if (sector[i] != (uint8_t)sig[i]) return 0;
   }
   return 1;
}

// If sector `lba` holds an exFAT boot sector, copies it into `boot`, records the start in
// *volStart and returns 1; otherwise 0. Reads through the caller's aligned `vbr` scratch.
static int tryExfatVbr(int storageHandle, uint64_t lba, uint32_t sectorBytes, uint64_t deviceSectors,
                       uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   // Reject an out-of-range starting LBA (an attacker-controlled partition field) BEFORE reading it,
   // so a crafted table can't steer a read to a wild sector. When the device size is unknown
   // (deviceSectors == 0) fall back to a finite ceiling instead of trusting the field unbounded.
   uint64_t sectorBound = deviceSectors ? deviceSectors : EXFAT_SCAN_LBA_CAP;
   if (lba == 0 || lba >= sectorBound) return 0;
   if (readSectors(storageHandle, lba, 1, vbr) != 0 || !hasExfatBoot(vbr)) return 0;
   memCopy(boot, vbr, (int)sectorBytes);
   *volStart = lba;
   return 1;
}

// Walks a GPT partition table (the disk is GPT when its protective-MBR entry is type 0xEE) and
// returns the first partition whose first sector is an exFAT VBR. `scratch` reads the GPT header
// and entry array; `vbr` reads partition candidates (a separate buffer so the entry array it is
// iterating isn't clobbered). Returns 1 on success (boot/volStart set), 0 otherwise.
static int locateExfatInGpt(int storageHandle, uint32_t sectorBytes, uint64_t deviceSectors,
                            uint8_t *scratch, uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   if (readSectors(storageHandle, GPT_HEADER_LBA, 1, scratch) != 0 || !hasGptHeader(scratch)) return 0;
   uint64_t entriesLba = readLe64(scratch + 72);   // PartitionEntryLBA
   uint32_t entryCount = readLe32(scratch + 80);   // NumberOfPartitionEntries
   uint32_t entrySize  = readLe32(scratch + 84);   // SizeOfPartitionEntry
   if (entrySize < GPT_ENTRY_MIN_SIZE || entrySize > sectorBytes) return 0;
   uint64_t sectorBound = deviceSectors ? deviceSectors : EXFAT_SCAN_LBA_CAP;   // finite ceiling when size unknown
   if (entriesLba >= sectorBound) return 0;   // entry-array LBA from a hostile header (or wild when size unknown)
   if (entryCount > GPT_MAX_ENTRIES) entryCount = GPT_MAX_ENTRIES;
   uint32_t perSector = sectorBytes / entrySize;

   for (uint32_t i = 0; i < entryCount; i++) {
      if (i % perSector == 0 && readSectors(storageHandle, entriesLba + i / perSector, 1, scratch) != 0) return 0;
      const uint8_t *entry = scratch + (i % perSector) * entrySize;
      int used = 0;
      for (int k = 0; k < 16; k++) if (entry[k]) { used = 1; break; }   // non-zero PartitionTypeGUID
      // entry+32 = StartingLBA
      if (used && tryExfatVbr(storageHandle, readLe64(entry + 32), sectorBytes, deviceSectors, vbr, boot, volStart)) return 1;
   }
   return 0;
}

// Locates the exFAT volume reachable through `storageHandle`: a superfloppy at LBA 0, or a partition listed
// in an MBR or GPT table. On success `boot` holds the volume's VBR and *volStart its start LBA.
// `scratch` and `vbr` are caller-owned aligned one-sector buffers. Returns 1 / 0.
static int locateExfatVolume(int storageHandle, uint8_t *boot, uint8_t *scratch, uint8_t *vbr,
                             uint32_t sectorBytes, uint64_t deviceSectors, uint64_t *volStart)
{
   if (hasExfatBoot(boot)) { *volStart = 0; return 1; }              // superfloppy (volume at LBA 0)
   if (boot[510] != 0x55 || boot[511] != 0xAA) return 0;            // neither exFAT nor partitioned

   for (int i = 0; i < MBR_PART_ENTRIES; i++) {
      const uint8_t *part = boot + MBR_PART_TABLE + i * MBR_PART_SIZE;
      uint8_t type = part[4];
      if (type == 0) continue;                                       // unused entry
      if (type == MBR_TYPE_GPT) {                                    // GPT disk: walk its table
         if (locateExfatInGpt(storageHandle, sectorBytes, deviceSectors, scratch, vbr, boot, volStart)) return 1;
         continue;
      }
      // part+8 = MBR partition StartingLBA
      if (tryExfatVbr(storageHandle, readLe32(part + 8), sectorBytes, deviceSectors, vbr, boot, volStart)) return 1;
   }
   return 0;
}

// Scans the root directory for the allocation-bitmap (0x81), up-case (0x82) and
// volume-label (0x83) entries and records them on the volume. Defined below.
static void loadVolumeMeta(ExfatVolume *vol);

// Counts free clusters by scanning the whole allocation bitmap (used once at mount to seed the
// running free-cluster total). Returns 0 / -1. Defined below.
static int countFreeClustersOnDisk(const ExfatVolume *vol, uint32_t *outFree);

// Validates the (untrusted, removable-media) VBR geometry before any of it is used to compute
// LBAs. The shifts must be in spec range (no undefined-behaviour shift, no absurd cluster size),
// NumberOfFats must be 1 or 2, and the FAT/heap layout, cluster count and root cluster must all
// fit the device. Returns 1 if the geometry is usable, 0 to reject the volume. `deviceSectors`
// of 0 means the device size is unknown, so the size bound is skipped.
static int validExfatGeometry(const uint8_t *boot, uint32_t deviceSectorSize,
                              uint64_t deviceSectors, uint64_t volStart)
{
   uint8_t bytesPerSectorShift    = boot[108];   // BytesPerSectorShift
   uint8_t sectorsPerClusterShift = boot[109];   // SectorsPerClusterShift
   uint8_t numberOfFats           = boot[110];   // NumberOfFats

   if (bytesPerSectorShift < 9 || bytesPerSectorShift > 12) return 0;       // 512..4096 bytes
   if (sectorsPerClusterShift > 25 - bytesPerSectorShift) return 0;         // cluster <= 32 MB, no shift UB
   if ((1u << bytesPerSectorShift) != deviceSectorSize) return 0;           // must match the device
   // Reject NumberOfFats == 2 (TexFAT): this driver reads/writes only the first FAT and does not
   // consult VolumeFlags.ActiveFat, so on a 2-FAT volume the second FAT would silently go stale (or,
   // with ActiveFat == 1, the active FAT would be ignored entirely). A 1-FAT volume can't have that
   // ambiguity. Almost all removable media is formatted with a single FAT.
   if (numberOfFats != 1) return 0;

   uint32_t bytesPerSector    = 1u << bytesPerSectorShift;
   uint32_t sectorsPerCluster = 1u << sectorsPerClusterShift;
   uint32_t fatOffset         = readLe32(boot + 80);
   uint32_t fatLength         = readLe32(boot + 84);
   uint32_t clusterHeapOffset = readLe32(boot + 88);
   uint32_t clusterCount      = readLe32(boot + 92);
   uint32_t rootCluster       = readLe32(boot + 96);

   if (clusterCount > 0xFFFFFFF5u) return 0;                                // exFAT maximum cluster count
   if (rootCluster < 2 || (rootCluster - 2) >= clusterCount) return 0;      // root must be a real data cluster
   if (fatOffset < 24 || clusterHeapOffset < fatOffset) return 0;   // 24-sector boot region, then FAT, then heap

   // The FAT must physically hold every cluster's 4-byte entry (entries 0,1 reserved + clusterCount)
   // and must fit between FatOffset and the cluster heap. Without this, a crafted/oversized FatLength
   // (or a heap that overlaps the FAT) would let a FAT entry LBA land inside the data heap, so a
   // write while chaining clusters could corrupt file data. (FatLength was previously unvalidated.)
   uint64_t fatBytesNeeded = ((uint64_t)clusterCount + 2) * 4;
   uint64_t fatSectorsNeeded = (fatBytesNeeded + bytesPerSector - 1) / bytesPerSector;
   if (fatLength < fatSectorsNeeded) return 0;                              // FAT too small to map every cluster
   if ((uint64_t)fatOffset + fatLength > clusterHeapOffset) return 0;       // FAT must end before the heap

   uint64_t heapEnd = (uint64_t)clusterHeapOffset + (uint64_t)clusterCount * sectorsPerCluster;
   if (heapEnd < clusterHeapOffset) return 0;                               // overflow
   // Bound the heap by the VBR's self-declared VolumeLength too, so a hostile ClusterHeapOffset near
   // 2^32 is rejected even when the device size is unknown (deviceSectors == 0) and the check below is
   // skipped - otherwise a cluster->LBA could resolve to a wild (but in-range) sector on such media.
   uint64_t volumeLength = readLe64(boot + 72);                            // VolumeLength, in sectors
   if (volumeLength != 0 && heapEnd > volumeLength) return 0;               // heap must fit the declared volume
   if (deviceSectors != 0 && volStart + heapEnd > deviceSectors) return 0;  // heap must fit the device
   return 1;
}

int mountExfat(ExfatVolume *vol, int drive)
{
   memSet(vol, 0, (int)sizeof(*vol));

   uint64_t deviceId = getUsbDeviceId(drive);
   StorageDeviceInfo info;
   int      haveInfo         = (getStorageInfo(deviceId, &info) == 0);
   uint32_t deviceSectorSize = haveInfo ? info.sectorSize  : 512;
   uint64_t deviceSectors    = haveInfo ? info.sectorCount : 0;   // 0 = unknown (skip the device-size bound)
   if (deviceSectorSize == 0 || deviceSectorSize > EXFAT_MAX_SECTOR) return EXFAT_MOUNT_NOT_READY;

   int storageHandle;
   if (openStorage(deviceId, &storageHandle) < 0) return EXFAT_MOUNT_NOT_READY;
   // lv2 storage faults if the first read lands too soon after open; settle like the
   // reference stacks (libntfs ps3_io.c, IRISMAN) do, not via log-induced delays.
   sys_timer_usleep(SYSIO_SETTLE_US);

   // bring up the shared scratch (needed from the first read below); release it again on any failure
   if (ensureScratch() != 0) { closeStorage(storageHandle); return EXFAT_MOUNT_NOT_READY; }

   // Read LBA 0 and locate the exFAT volume: superfloppy, or an MBR- or GPT-partitioned disk.
   // The scan needs two scratch sectors. Rather than spend 8 KB of stack (this can run on a size-
   // sensitive 16 KB plugin thread), reuse the operational sector caches AS generic scratch: the
   // mount holds exfatLock so nothing else touches them, and invalidateCaches() below drops
   // whatever the scan leaves before the caches are first used for real. Aliased so the scan reads
   // as plain scratch, not as the dir/FAT caches.
   uint8_t *scanScratch = dirSector;   // partition table / GPT-entry sectors
   uint8_t *scanVbr     = fatSector;   // candidate boot-record sectors
   uint8_t *boot = bootSector;   // off the stack (see bootSector) so the mount path fits a small stack
   if (readSectors(storageHandle, 0, 1, boot) != 0) {
      closeStorage(storageHandle);
      releaseScratchIfIdle();
      return EXFAT_MOUNT_NOT_READY;
   }
   uint64_t volStart = 0;
   if (!locateExfatVolume(storageHandle, boot, scanScratch, scanVbr, deviceSectorSize, deviceSectors, &volStart)) {
      closeStorage(storageHandle);                       // read OK but no exFAT volume here (e.g. FAT32/NTFS)
      releaseScratchIfIdle();
      return EXFAT_MOUNT_NOT_EXFAT;
   }

   // Validate the geometry from the (untrusted) VBR before trusting any field that feeds a
   // cluster->LBA computation. A hostile/malformed exFAT image is rejected, not acted on.
   if (!validExfatGeometry(boot, deviceSectorSize, deviceSectors, volStart)) {
      closeStorage(storageHandle);
      releaseScratchIfIdle();
      return EXFAT_MOUNT_NOT_EXFAT;
   }

   vol->storageHandle                = storageHandle;
   vol->cacheEpoch        = (mountEpoch + 1) ? ++mountEpoch : (mountEpoch = 1);   // fresh cache key; skip the 0 sentinel on wrap
   vol->drive             = (uint8_t)drive;
   vol->deviceSectorSize  = deviceSectorSize;
   vol->bytesPerSector    = 1u << boot[108];    // BytesPerSectorShift
   vol->sectorsPerCluster = 1u << boot[109];    // SectorsPerClusterShift
   vol->partitionOffset   = volStart;
   vol->fatOffset         = readLe32(boot + 80);    // FatOffset
   vol->clusterHeapOffset = readLe32(boot + 88);    // ClusterHeapOffset
   vol->clusterCount      = readLe32(boot + 92);    // ClusterCount
   vol->rootCluster       = readLe32(boot + 96);    // FirstClusterOfRootDirectory
   vol->mounted           = 1;
   mountedCount++;
   invalidateCaches();
   upcaseTableEpoch = 0;    // fresh volume: drop any cached up-case table
   loadVolumeMeta(vol);   // bitmap location, up-case table and the volume label
   // Seed the running free-cluster count once (one bitmap scan) so getExfatFree is O(1) thereafter;
   // markClusterRun keeps it in sync on every alloc/free. 0 if the bitmap is missing/unreadable.
   uint32_t seedFree = 0;
   vol->freeClusters = (vol->bitmapCluster != 0 && countFreeClustersOnDisk(vol, &seedFree) == 0) ? seedFree : 0;
   return EXFAT_MOUNT_OK;
}

void unmountExfat(ExfatVolume *vol)
{
   if (!vol->mounted) return;
   closeStorage(vol->storageHandle);
   vol->mounted = 0;
   mountedCount--;
   invalidateCaches();   // drop the sector caches keyed by the old epoch
   upcaseTableEpoch = 0;   // and the up-case table cache
   releaseScratchIfIdle();   // last volume out frees the shared scratch block
}

void openExfatDir(ExfatDir *dir, const ExfatVolume *vol, uint32_t firstCluster, int noFatChain, uint64_t byteLength)
{
   dir->vol            = vol;
   dir->cluster        = firstCluster;
   dir->sectorInClu    = 0;
   dir->entryInSector  = 0;
   dir->noFatChain     = noFatChain;
   dir->clustersWalked = 0;
   dir->ioError        = 0;
   // Bound the walk by the directory's own DataLength so a malformed entry can't make us read
   // past the allocation (a contiguous/NoFatChain directory would otherwise spill into whatever
   // physically follows it and parse foreign clusters as entries). 0 = unbounded (the root, whose
   // size no entry records; its FAT chain is bounded by the clusterCount cycle guard instead).
   if (byteLength == 0) {
      dir->clusterLimit = 0;
   } else {
      uint32_t clusterBytes = getClusterBytes(vol);
      uint64_t clusters = (byteLength + clusterBytes - 1) / clusterBytes;
      dir->clusterLimit = clusters > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)clusters;
   }
}

void closeExfatDir(ExfatDir *dir)
{
   dir->cluster = 0;
   dir->vol     = 0;
}

// advances the directory chain to the next cluster (0 when it ends). Bounds the walk by the
// volume's cluster count so a cyclic/corrupt directory FAT ends the listing instead of spinning.
static uint32_t advanceDirCluster(ExfatDir *dir)
{
   if (++dir->clustersWalked > dir->vol->clusterCount) return 0;            // cycle guard
   if (dir->clusterLimit != 0 && dir->clustersWalked >= dir->clusterLimit) return 0;   // past DataLength
   if (dir->noFatChain) {
      uint32_t next = dir->cluster + 1;
      return isClusterValid(dir->vol, next) ? next : 0;
   }
   return getNextCluster(dir->vol, dir->cluster);
}

// returns the next 32-byte directory entry (into dirSector), or 0 at chain end.
static const uint8_t *getNextEntry(ExfatDir *dir)
{
   const ExfatVolume *vol = dir->vol;
   // vol is NULL once the device was yanked while this dir was open (detachVolumeHandles),
   // so test it BEFORE dereferencing mounted - a late read on a stale handle must end the
   // walk, not crash (mirrors readExfat).
   if (!vol || !vol->mounted) return 0;   // ejected mid-walk: end the dir, never touch the closed storageHandle
   if (!isClusterValid(vol, dir->cluster)) return 0;

   uint64_t lba = clusterToSector(vol, dir->cluster) + dir->sectorInClu;
   if (lba != dirSectorLba || vol->cacheEpoch != dirSectorEpoch) {
      if (readSectors(vol->storageHandle, lba, 1, dirSector) != 0) {
         dir->ioError = 1;   // a real I/O fault, NOT end-of-directory: the VFS boundary turns this into -1
         dir->cluster = 0;
         return 0;
      }
      dirSectorLba = lba;
      dirSectorEpoch  = vol->cacheEpoch;
   }
   const uint8_t *entry = &dirSector[dir->entryInSector * DIR_ENTRY_BYTES];

   uint32_t entriesPerSector = vol->bytesPerSector / DIR_ENTRY_BYTES;
   if (++dir->entryInSector >= entriesPerSector) {
      dir->entryInSector = 0;
      if (++dir->sectorInClu >= vol->sectorsPerCluster) {
         dir->sectorInClu = 0;
         dir->cluster = advanceDirCluster(dir);
      }
   }
   return entry;
}

// On-disk location of the entry set readExfatDir last returned (the 0x85 entry's
// position + the set size). Valid until the next readdir/backend call; shared like the
// sector caches, so only touched under the backend lock. The delete path reads this to
// find the entries to invalidate without re-parsing the directory.
static uint32_t lastSetCluster, lastSetSectorInClu, lastSetEntryInSector;
static int      lastSetCount, lastSetDirNoFatChain;

// Copies the position of the entry set readExfatDir most recently returned (the shared lastSet*
// globals) into `loc`. Valid only until the next readdir/backend call; caller holds the lock.
static void captureLastSet(ExfatEntryLoc *loc)
{
   loc->cluster       = lastSetCluster;
   loc->sectorInClu   = lastSetSectorInClu;
   loc->entryInSector = lastSetEntryInSector;
   loc->count         = lastSetCount;
   loc->dirNoFatChain = lastSetDirNoFatChain;
}

// Rolling exFAT SetChecksum: folds one 32-byte directory entry into `sum`. The primary (File)
// entry skips its own checksum field (bytes 2-3); secondary entries fold every byte.
static uint16_t addEntryChecksum(uint16_t sum, const uint8_t *entry, int isPrimary)
{
   for (int i = 0; i < DIR_ENTRY_BYTES; i++) {
      if (isPrimary && (i == 2 || i == 3)) continue;
      sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + entry[i]);
   }
   return sum;
}

int readExfatDir(ExfatDir *dir, char *name, int nameCap, ExfatInfo *info)
{
   for (;;) {
      uint32_t setCluster = dir->cluster, setSic = dir->sectorInClu, setEis = dir->entryInSector;
      const uint8_t *entry = getNextEntry(dir);
      if (!entry) return 0;

      uint8_t type = entry[0];
      if (type == ENTRY_END) {       // no further entries in this directory
         dir->cluster = 0;
         return 0;
      }
      if (type != ENTRY_FILE) continue;   // deleted entry or volume/bitmap/upcase

      // File directory entry: capture its fields before the next read overwrites
      // the shared sector buffer.
      uint8_t  secondaryCount = entry[1];
      if ((int)secondaryCount + 1 > MAX_SET_ENTRIES) continue;   // malformed: more entries than a set can hold
      uint16_t attributes     = readLe16(entry + 4);
      uint64_t mtime          = timestampToUnix(readLe32(entry + 12), entry[23]);   // LastModified + its UtcOffset
      int      isDir          = (attributes & ATTR_DIRECTORY) ? 1 : 0;

      // Fold the whole set into the SetChecksum as it is read (the primary skips its checksum
      // field) and reject the set if the on-disk SetChecksum doesn't match - a corrupt directory
      // entry is skipped rather than acted on.
      uint16_t storedChecksum = readLe16(entry + 2);
      uint16_t calcChecksum   = addEntryChecksum(0, entry, 1);

      const uint8_t *stream = getNextEntry(dir);
      if (!stream || stream[0] != ENTRY_STREAM) continue;   // malformed set
      calcChecksum = addEntryChecksum(calcChecksum, stream, 0);
      uint8_t  nameLength   = stream[3];
      // The set must carry enough File Name (0xC1) entries to hold the claimed NameLength: the spec
      // requires SecondaryCount == 1 (stream) + ceil(NameLength/15) name entries (vendor extensions
      // may add more, so this is a lower bound). Rejects a set that under-declares its name entries
      // (which would otherwise yield a truncated name that still happens to pass the SetChecksum).
      if (nameLength < 1 || (int)secondaryCount < 1 + ((int)nameLength + 14) / 15) continue;
      int      noFatChain   = (stream[1] & FLAG_NO_FAT_CHAIN) ? 1 : 0;
      uint32_t firstCluster = readLe32(stream + 20);
      uint64_t dataLength   = readLe64(stream + 24);
      uint64_t validLength  = readLe64(stream + 8);                  // ValidDataLength
      if (validLength > dataLength) validLength = dataLength;    // clamp a malformed value

      uint16_t utf16[256];
      int collected = 0;
      int truncated = 0;
      for (int remaining = (int)secondaryCount - 1; remaining > 0; remaining--) {
         const uint8_t *frag = getNextEntry(dir);
         if (!frag) { truncated = 1; break; }
         calcChecksum = addEntryChecksum(calcChecksum, frag, 0);
         if (frag[0] != ENTRY_NAME) continue;
         for (int i = 0; i < 15 && collected < (int)nameLength && collected < 255; i++)
            utf16[collected++] = readLe16(frag + 2 + i * 2);
      }
      utf16[collected] = 0;
      if (truncated || calcChecksum != storedChecksum) continue;   // incomplete or corrupt set: skip it

      utf16ToUtf8(utf16, name, nameCap);
      if (info) {
         info->size         = dataLength;
         info->mtime        = mtime;
         info->isDir        = isDir;
         info->firstCluster = firstCluster;
         info->noFatChain   = noFatChain;
         info->validSize    = validLength;
      }
      lastSetCluster       = setCluster;
      lastSetSectorInClu   = setSic;
      lastSetEntryInSector = setEis;
      lastSetCount         = (int)secondaryCount + 1;
      lastSetDirNoFatChain = dir->noFatChain;
      return 1;
   }
}

// Case-insensitive name compare through the volume's up-case table (full exFAT casefold, not just
// ASCII), so lookups match exactly the names the on-disk NameHash treats as equal. Defined below.
static int namesEqualFold(const ExfatVolume *vol, const char *a, const char *b);

// finds `target` (case-insensitive) directly in one directory; fills info.
static int findInDir(const ExfatVolume *vol, uint32_t dirCluster, int dirNoFatChain,
                     uint64_t dirByteLength, const char *target, ExfatInfo *info)
{
   ExfatDir dir;
   openExfatDir(&dir, vol, dirCluster, dirNoFatChain, dirByteLength);
   char name[256];
   while (readExfatDir(&dir, name, (int)sizeof(name), info)) {
      if (namesEqualFold(vol, name, target)) return 1;
   }
   return dir.ioError ? -1 : 0;   // distinguish a mid-scan I/O fault from a genuine miss
}

int statExfat(const ExfatVolume *vol, const char *path, ExfatInfo *info)
{
   const char *p = path;
   while (*p == '/') p++;

   if (*p == 0) {   // the root directory itself
      info->size         = 0;
      info->mtime        = 0;
      info->isDir        = 1;
      info->firstCluster = vol->rootCluster;
      info->noFatChain   = 0;
      info->validSize    = 0;
      return 0;
   }

   uint32_t dirCluster    = vol->rootCluster;
   int      dirNoFatChain = 0;
   uint64_t dirByteLength = 0;   // root: size unknown, bounded by the FAT cycle guard
   char     component[256];
   for (;;) {
      int n = 0;
      while (p[n] && p[n] != '/' && n < 255) { component[n] = p[n]; n++; }
      component[n] = 0;
      p += n;
      while (*p == '/') p++;

      if (n == 0) return -1;   // malformed (empty component)
      if (findInDir(vol, dirCluster, dirNoFatChain, dirByteLength, component, info) != 1) return -1;   // miss or I/O error
      if (*p == 0) return 0;            // last component matched
      if (!info->isDir) return -1;      // an intermediate must be a directory
      dirCluster    = info->firstCluster;
      dirNoFatChain = info->noFatChain;
      dirByteLength = info->size;       // bound the next level by this directory's DataLength
   }
}

// returns the cluster holding the file's cluster #index (0 if past the end).
static uint32_t getClusterAt(ExfatFile *file, uint32_t index)
{
   const ExfatVolume *vol = file->vol;
   uint32_t cluster, i;
   if (index >= file->cachedIndex && isClusterValid(vol, file->cachedCluster)) {
      cluster = file->cachedCluster;   // resume from the cached position
      i       = file->cachedIndex;
   } else {
      cluster = file->firstCluster;    // walk from the start
      i       = 0;
   }
   uint32_t walked = 0;
   while (i < index && isClusterValid(vol, cluster)) {
      cluster = file->noFatChain ? cluster + 1 : getNextCluster(vol, cluster);
      i++;
      if (++walked > vol->clusterCount) { cluster = 0; break; }   // cyclic/corrupt FAT chain: stop
   }
   file->cachedIndex   = index;
   file->cachedCluster = cluster;
   return cluster;
}

// Largest physically-contiguous span on disk, in sectors (capped at maxSectors), starting at
// file cluster #index, sector sectorInClu - so a multi-cluster read/write can issue ONE storage
// call instead of one per cluster. *lba gets the start LBA; the file's cluster cache is left on
// the last cluster of the span. Returns 0 if `index` is past the end. Uses the cluster+FAT caches.
static uint32_t findContigSpan(ExfatFile *file, uint32_t index, uint32_t sectorInClu,
                           uint32_t maxSectors, uint64_t *lba)
{
   ExfatVolume *vol = file->vol;
   uint32_t sectorsPerCluster = vol->sectorsPerCluster;
   uint32_t cluster = getClusterAt(file, index);
   if (!isClusterValid(vol, cluster)) return 0;
   *lba = clusterToSector(vol, cluster) + sectorInClu;
   uint32_t span = sectorsPerCluster - sectorInClu;            // sectors left in the start cluster
   uint32_t lastIndex = index;
   while (span < maxSectors) {                    // absorb following physically-adjacent clusters
      uint32_t nextCluster = file->noFatChain ? cluster + 1 : getNextCluster(vol, cluster);
      if (nextCluster != cluster + 1 || !isClusterValid(vol, nextCluster)) break;
      cluster = nextCluster; lastIndex++;
      span += sectorsPerCluster;
   }
   if (span > maxSectors) span = maxSectors;
   file->cachedIndex   = lastIndex;               // resume cache on the last cluster of the span
   file->cachedCluster = cluster;
   return span;
}

// True if `p` meets the lv2 storage DMA alignment, so it can be read/written in place
// without bouncing through an aligned scratch buffer.
static int isDmaAligned(const void *p)
{
   return ((uintptr_t)p & (STORAGE_ALIGN - 1)) == 0;
}

// Fills an open-file handle from a located/created entry: its data location (info) and
// the on-disk position of its entry set.
static void setupFileHandle(ExfatFile *file, ExfatVolume *vol, const ExfatInfo *info, const ExfatEntryLoc *entry)
{
   file->vol           = vol;
   file->firstCluster  = info->firstCluster;
   file->size          = info->size;
   file->validSize     = info->validSize;
   file->position      = 0;
   file->noFatChain    = info->noFatChain;
   file->cachedIndex   = 0;
   file->cachedCluster = info->firstCluster;
   uint32_t clusterBytes         = getClusterBytes(vol);
   // size-derived count; reserveClusters resyncs it to the real chain length before extending
   file->allocClusters = (uint32_t)((info->size + clusterBytes - 1) / clusterBytes);
   file->entry         = *entry;
   file->writable      = 0;
   file->appendMode    = 0;
   file->dirty         = 0;
}

int readExfat(ExfatFile *file, void *buffer, int length)
{
   if (length < 0) return -1;
   const ExfatVolume *vol = file->vol;
   // vol is NULL once the device was yanked while this file was open (detachVolumeHandles), so
   // test it BEFORE dereferencing mounted - a late read on a stale handle must fail, not crash.
   if (!vol || !vol->mounted) return -1;

   uint64_t want = (uint64_t)length;
   uint64_t remaining = file->size - file->position;
   if (want > remaining) want = remaining;

   uint8_t *out = (uint8_t *)buffer;

   // exFAT: bytes in [validSize, size) read as zero. Serve disk bytes only up to validSize and
   // zero-fill anything past it (diverges only for foreign files written with validSize < size).
   uint64_t zeroFill = 0;
   if (file->position >= file->validSize) {        // wholly inside the zero tail
      memSet(out, 0, (int)want);
      file->position += want;
      return (int)want;
   }
   if (file->position + want > file->validSize) {  // read straddles validSize: split off the zero tail
      zeroFill = file->position + want - file->validSize;
      want    -= zeroFill;
   }

   uint32_t clusterBytes  = getClusterBytes(vol);
   uint32_t bytesPerSector = vol->bytesPerSector;
   uint32_t bounceSectors = EXFAT_READ_BOUNCE / bytesPerSector;   // cap when bouncing
   if (bounceSectors == 0) bounceSectors = 1;
   int total = 0;
   while (want > 0) {
      uint32_t index       = (uint32_t)(file->position / clusterBytes);
      uint32_t offsetInClu = (uint32_t)(file->position % clusterBytes);
      uint32_t sectorInClu = offsetInClu / bytesPerSector;
      uint32_t offsetInSec = offsetInClu % bytesPerSector;

      // Fast path: aligned destination on a sector boundary with at least a full sector to go.
      // Read as many physically-contiguous sectors as we can straight into the caller's buffer
      // (no bounce, spanning whole clusters) - one storage call for a multi-cluster run.
      if (offsetInSec == 0 && want >= bytesPerSector && isDmaAligned(out + total)) {
         uint32_t maxSectors = (uint32_t)(want / bytesPerSector);
         uint64_t lba;
         uint32_t run = findContigSpan(file, index, sectorInClu, maxSectors, &lba);
         if (run == 0) break;
         if (readSectors(vol->storageHandle, lba, run, out + total) != 0) break;
         uint32_t n = run * bytesPerSector;
         total += (int)n; file->position += n; want -= n;
         continue;
      }

      // Slow path: partial leading/trailing sector, or an unaligned caller buffer - bounce one
      // contiguous run (capped at the bounce) through the aligned scratch, then copy out.
      uint32_t cluster = getClusterAt(file, index);
      if (!isClusterValid(vol, cluster)) break;
      uint64_t lba = clusterToSector(vol, cluster) + sectorInClu;
      uint32_t runSectors = vol->sectorsPerCluster - sectorInClu;
      if (runSectors > bounceSectors) runSectors = bounceSectors;
      if (readSectors(vol->storageHandle, lba, runSectors, fileSector) != 0) break;
      uint32_t n = runSectors * bytesPerSector - offsetInSec;
      if ((uint64_t)n > want) n = (uint32_t)want;
      memCopy(out + total, fileSector + offsetInSec, (int)n);
      total += (int)n; file->position += n; want -= n;
   }

   // append the zero tail once the disk-backed portion is fully served (want hit 0, no I/O break)
   if (want == 0 && zeroFill > 0) {
      memSet(out + total, 0, (int)zeroFill);
      total += (int)zeroFill;
      file->position += zeroFill;
   }
   return total;
}

void seekExfat(ExfatFile *file, uint64_t position)
{
   if (position > file->size) position = file->size;
   file->position = position;
}

// jf-port: read-only.  Nothing is ever dirty, so closing a file writes nothing back.
int closeExfat(ExfatFile *file)
{
   file->vol = 0;
   return 0;
}

// Counts free clusters by popcounting the whole allocation bitmap, reading it through the 32 KB
// bounce (many sectors per storage call, ~2048 sectors on a 256 GB volume becomes a couple dozen
// reads). Uses vol->bitmapCluster/bitmapBytes recorded at mount (no root rescan). Returns 0 / -1.
static int countFreeClustersOnDisk(const ExfatVolume *vol, uint32_t *outFree)
{
   if (vol->bitmapCluster == 0) return -1;
   uint64_t freeClusters = 0;
   uint32_t bitsLeft     = vol->clusterCount;
   uint64_t bytesLeft    = ((uint64_t)vol->clusterCount + 7) / 8;
   if (bytesLeft > vol->bitmapBytes) bytesLeft = vol->bitmapBytes;
   uint32_t cluster = vol->bitmapCluster;
   uint32_t bounceSectors = EXFAT_READ_BOUNCE / vol->bytesPerSector;
   if (bounceSectors == 0) bounceSectors = 1;

   while (bytesLeft > 0 && isClusterValid(vol, cluster)) {
      uint64_t clusterLba = clusterToSector(vol, cluster);
      for (uint32_t s = 0; s < vol->sectorsPerCluster && bytesLeft > 0; ) {
         uint32_t runSectors = vol->sectorsPerCluster - s;
         if (runSectors > bounceSectors) runSectors = bounceSectors;
         if (readSectors(vol->storageHandle, clusterLba + s, runSectors, fileSector) != 0) return -1;
         uint64_t chunk = (uint64_t)runSectors * vol->bytesPerSector;
         if (chunk > bytesLeft) chunk = bytesLeft;
         for (uint64_t b = 0; b < chunk; b++) {
            uint8_t byte = fileSector[b];
            if (bitsLeft >= 8) {                 // whole byte of real clusters: count zero bits at once
               freeClusters += 8u - (uint32_t)__builtin_popcount(byte);
               bitsLeft -= 8;
            } else {                             // final partial byte: count only the valid bits
               for (int bit = 0; bit < 8 && bitsLeft > 0; bit++, bitsLeft--)
                  if (!((byte >> bit) & 1)) freeClusters++;
            }
         }
         bytesLeft -= chunk;
         s += runSectors;
      }
      cluster = getNextCluster(vol, cluster);   // bitmap may span clusters
   }
   *outFree = (uint32_t)freeClusters;
   return 0;
}

int getExfatFree(const ExfatVolume *vol, uint64_t *freeBytes, uint64_t *totalBytes)
{
   uint32_t clusterBytes = getClusterBytes(vol);
   if (totalBytes) *totalBytes = (uint64_t)vol->clusterCount * clusterBytes;
   if (vol->bitmapCluster == 0) return -1;   // no usable bitmap (loadVolumeMeta rejected it)
   // O(1): return the running free-cluster total seeded at mount and kept by markClusterRun, instead
   // of rescanning the whole bitmap on every query (the file manager polls free space on each refresh).
   if (freeBytes) *freeBytes = (uint64_t)vol->freeClusters * clusterBytes;
   return 0;
}

// ============================================================================
// Volume metadata: the allocation bitmap's location, the up-case table and the
// volume label, read once at mount.  metaScratch is a read buffer here.
// ============================================================================

#define ENTRY_VOLLABEL  0x83          // volume label (primary)
#define ENTRY_UPCASE    0x82          // up-case table (primary)
#define ATTR_DIR_BITS   0x10          // FileAttributes: Directory
#define ATTR_ARCHIVE    0x20          // FileAttributes: Archive (set on created files)
#define STREAM_FLAGS_NEW (0x01 | FLAG_NO_FAT_CHAIN)   // AllocationPossible + NoFatChain (contiguous)
#define STREAM_FLAGS_CHAIN 0x01       // AllocationPossible, FAT-chained (data follows the FAT)
#define STREAM_FLAGS_EMPTY 0x00       // no allocation (0-length file): FirstCluster/DataLength must be 0
#define NAME_PER_ENTRY  15            // UTF-16 units per File Name entry

// A valid placeholder DOS timestamp (2025-01-01 00:00:00), used only as the fallback when the RTC
// is unavailable (getNowTimestamp reads the real time otherwise). fsck only requires a structurally
// valid timestamp.
#define EXFAT_DEFAULT_TS 0x5A210000u

static void loadVolumeMeta(ExfatVolume *vol)
{
   vol->bitmapCluster = 0; vol->bitmapBytes = 0;
   vol->upcaseCluster = 0; vol->upcaseBytes = 0;
   vol->label[0]      = 0;

   ExfatDir dir;
   openExfatDir(&dir, vol, vol->rootCluster, 0, 0);
   for (;;) {
      const uint8_t *entry = getNextEntry(&dir);
      if (!entry || entry[0] == ENTRY_END) break;
      if (entry[0] == ENTRY_BITMAP) {
         uint32_t bmCluster = readLe32(entry + 20);
         uint64_t bmBytes   = readLe64(entry + 24);
         // Validate before trusting: the bitmap drives every alloc/free, so a misplaced FirstCluster
         // or an undersized length would let bit flips scribble inside a victim file's data. Require a
         // real start cluster and a length that covers at least one bit per cluster. A bad bitmap
         // leaves bitmapCluster == 0, which disables writes (markClusterRun returns -1) but keeps
         // reads working - safer than acting on a hostile pointer.
         uint64_t bmBytesNeeded = ((uint64_t)vol->clusterCount + 7) / 8;
         if (isClusterValid(vol, bmCluster) && bmBytes >= bmBytesNeeded) {
            vol->bitmapCluster = bmCluster;
            vol->bitmapBytes   = bmBytes;
         }
      } else if (entry[0] == ENTRY_UPCASE) {
         vol->upcaseCluster = readLe32(entry + 20);
         vol->upcaseBytes   = readLe64(entry + 24);
      } else if (entry[0] == ENTRY_VOLLABEL) {
         // Volume Label entry: offset 1 = character count (UTF-16 units, <= 11),
         // offset 2 = label. Decode to UTF-8 for display.
         int count = entry[1];
         if (count > 11) count = 11;
         uint16_t units[12];
         for (int i = 0; i < count; i++) units[i] = readLe16(entry + 2 + i * 2);
         units[count] = 0;
         utf16ToUtf8(units, vol->label, (int)sizeof(vol->label));
      }
   }
}

// Decompresses the volume's on-disk up-case table (run-length compressed with the 0xFFFF marker)
// into the shared upcaseTable[] cache, once per storageHandle. Entries outside the table's defined range stay
// identity. Returns 0 / -1 (I/O). After this, upcaseTable[c] is the up-cased form of any unit c.
// Decodes the on-disk $UpCase into the sparse cache: only code points whose up-cased form differs from
// identity are recorded (in ascending code-point order, ready for binary search). The on-disk table is
// the run-length form (a 0xFFFF marker means the next u16 is a count of identity code points to skip).
static int ensureUpcaseTable(const ExfatVolume *vol)
{
   if (upcaseTableEpoch == vol->cacheEpoch) return 0;                                // already cached
   upcaseCount = 0;                                  // identity default = empty sparse table
   if (vol->upcaseCluster == 0 || vol->upcaseBytes == 0) { upcaseTableEpoch = vol->cacheEpoch; return 0; }

   uint64_t total    = vol->upcaseBytes & ~1ULL;    // whole u16 units only
   uint64_t consumed = 0;
   uint32_t cp       = 0;                            // current code point index
   int      pending  = 0;                            // next u16 is an identity run length
   uint32_t upcaseCluster       = vol->upcaseCluster;
   uint32_t guard    = 0;

   while (isClusterValid(vol, upcaseCluster) && consumed < total && cp < 0x10000) {
      uint64_t cs = clusterToSector(vol, upcaseCluster);
      for (uint32_t s = 0; s < vol->sectorsPerCluster && consumed < total && cp < 0x10000; s++) {
         if (readSectors(vol->storageHandle, cs + s, 1, metaScratch) != 0) return -1;
         uint32_t span = vol->bytesPerSector;
         for (uint32_t o = 0; o + 1 < span && consumed < total && cp < 0x10000; o += 2, consumed += 2) {
            uint16_t v = readLe16(metaScratch + o);
            if (pending) { cp += v; pending = 0; continue; }   // identity run of `v` code points
            if (v == 0xFFFF) { pending = 1; continue; }
            if (v != (uint16_t)cp && upcaseCount < EXFAT_UPCASE_MAX) {   // store only real mappings
               upcaseCp[upcaseCount] = (uint16_t)cp;
               upcaseUp[upcaseCount] = v;
               upcaseCount++;
            }
            cp++;
         }
      }
      if (++guard > vol->clusterCount) break;   // corrupt/cyclic up-case chain: stop
      upcaseCluster = getNextCluster(vol, upcaseCluster);
   }
   upcaseTableEpoch = vol->cacheEpoch;
   return 0;
}

// Up-cased form of UTF-16 unit `c` via the sparse table (binary search; identity if not mapped).
static uint16_t upcaseOf(uint16_t c)
{
   int lo = 0, hi = (int)upcaseCount - 1;
   while (lo <= hi) {
      int mid = (lo + hi) >> 1;
      uint16_t key = upcaseCp[mid];
      if (key == c) return upcaseUp[mid];
      if (key < c) lo = mid + 1; else hi = mid - 1;
   }
   return c;
}

// Number of UTF-16 units in a NUL-terminated UTF-16 string (capped at 255).
static int getUtf16Len(const uint16_t *s)
{
   int n = 0;
   while (s[n] && n < 255) n++;
   return n;
}

// Case-insensitive name compare via the volume's up-case table (full exFAT casefold). Falls back
// to ASCII folding only if the table can't be read. Returns 1 if the names are equal, 0 if not.
static int namesEqualFold(const ExfatVolume *vol, const char *a, const char *b)
{
   uint16_t a16[256], b16[256];
   utf8ToUtf16(a, a16, 255);
   utf8ToUtf16(b, b16, 255);
   int la = getUtf16Len(a16), lb = getUtf16Len(b16);
   if (la != lb) return 0;
   if (ensureUpcaseTable(vol) != 0) return strCmpICase(a, b) == 0;   // I/O failure: ASCII fallback
   for (int i = 0; i < la; i++)
      if (upcaseOf(a16[i]) != upcaseOf(b16[i])) return 0;
   return 1;
}

// Splits an in-volume path into its parent directory path and leaf name. Returns 0,
// or -1 if the path is the root or its leaf is empty. parent is "/" for a top-level
// entry; otherwise the substring before the last '/'.
static int splitParentLeaf(const char *path, char *parent, int parentCap, char *leaf, int leafCap)
{
   const char *p = path;
   while (*p == '/') p++;
   if (*p == 0) return -1;                  // root has no parent/leaf

   int lastSlash = -1;
   for (int i = 0; path[i]; i++) if (path[i] == '/') lastSlash = i;

   const char *l = (lastSlash >= 0) ? path + lastSlash + 1 : path;
   int n = 0;
   while (l[n] && n < leafCap - 1) { leaf[n] = l[n]; n++; }
   leaf[n] = 0;
   if (n == 0) return -1;
   if (l[n] != 0) return -1;   // leaf didn't fit: refuse rather than operate on a truncated name

   if (lastSlash <= 0) { parent[0] = '/'; parent[1] = 0; }
   else {
      if (lastSlash > parentCap - 1) return -1;   // parent path doesn't fit
      int j = 0;
      for (; j < lastSlash && j < parentCap - 1; j++) parent[j] = path[j];
      parent[j] = 0;
   }
   return 0;
}

// Current time as an exFAT (DOS-form) timestamp, in UTC - the inverse of timestampToUnix. Entries
// are written with this plus a UtcOffset byte of 0x80 (OffsetValid, offset 0 = UTC), so the stored
// time is internally consistent and anchored for other readers. Falls back to EXFAT_DEFAULT_TS when
// the RTC is unavailable. Requires the host app to have loaded CELL_SYSMODULE_RTC (initRtc).
#define EXFAT_TZ_UTC 0x80   // UtcOffset byte: OffsetValid set, offset 0 (UTC)
// Resolves a destination directory path for an insert: fills *info with its metadata
// and *entry with the directory's own entry-set location so placeEntrySet can grow it.
// The root has no parent entry, so *hasEntry is 0 for it. Must capture the entry before
// any further readdir (which would overwrite the shared location). Returns 0 / -1.
static int resolveParentDir(ExfatVolume *vol, const char *parent, ExfatInfo *info,
                            ExfatEntryLoc *entry, int *hasEntry)
{
   if (statExfat(vol, parent, info) != 0 || !info->isDir) return -1;
   *hasEntry = !(parent[0] == '/' && parent[1] == '\0');
   if (*hasEntry) captureLastSet(entry);
   return 0;
}

// On-disk location and metadata of a found entry set (filled by locateEntrySet).
typedef struct {
   ExfatEntryLoc entry;   // position of the 0x85 entry + the set size
   ExfatInfo     info;
} ExsetFatLoc;

// Finds `target` (case-insensitive) in one directory and records the on-disk location
// of its entry set (via the readdir position capture). Returns 1 if found, 0 otherwise.
static int locateEntrySet(const ExfatVolume *vol, uint32_t dirCluster, int dirNoFatChain,
                          uint64_t dirByteLength, const char *target, ExsetFatLoc *loc)
{
   ExfatDir dir;
   openExfatDir(&dir, vol, dirCluster, dirNoFatChain, dirByteLength);
   char name[256];
   ExfatInfo info;
   while (readExfatDir(&dir, name, (int)sizeof name, &info) == 1) {
      if (namesEqualFold(vol, name, target)) {
         captureLastSet(&loc->entry);
         loc->info = info;
         return 1;
      }
   }
   return dir.ioError ? -1 : 0;   // -1 = mid-scan I/O fault (not "absent"); 0 = genuine miss
}

// Opens the file at an in-volume path into `file`.  Returns 0 on success; -1 if it is not found,
// is a directory, or on an I/O fault.  (jf-port: the create half of openOrCreateExfat is gone.)
int openExfat(ExfatFile *file, ExfatVolume *vol, const char *path)
{
   char parent[512], leaf[256];
   if (splitParentLeaf(path, parent, (int)sizeof parent, leaf, (int)sizeof leaf) != 0) return -1;

   ExfatInfo parentInfo;
   ExfatEntryLoc parentEntry;
   int hasParentEntry;
   if (resolveParentDir(vol, parent, &parentInfo, &parentEntry, &hasParentEntry) != 0) return -1;

   ExsetFatLoc found;
   int located = locateEntrySet(vol, parentInfo.firstCluster, parentInfo.noFatChain, parentInfo.size, leaf, &found);
   if (located != 1) return -1;
   if (found.info.isDir) return -1;                     // can't open a directory as a file
   setupFileHandle(file, vol, &found.info, &found.entry);
   return 0;
}
