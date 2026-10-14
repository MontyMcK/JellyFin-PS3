#!/usr/bin/env python3
"""Re-vendor mohasi's ntfs.c/ntfs.h WITH the write path, applying the jf-port edits.

usage: port_ntfs.py <upstream ntfs.c> <upstream ntfs.h> <out dir>
"""
import sys, re

src_c, src_h, out = sys.argv[1:4]
c = open(src_c, encoding="utf-8").read()
h = open(src_h, encoding="utf-8").read()

def sub_once(text, old, new, what):
    n = text.count(old)
    if n != 1:
        sys.exit(f"{what}: expected 1 match, got {n}")
    return text.replace(old, new)

# ---- ntfs.c --------------------------------------------------------------------------------
old_inc = '''#include "vfs.h"               // VfsOps + probe/release backend registration
#include "storage-device.h"    // device ids, info + raw sector I/O (shared lv2 storage layer)
#include "syscall.h"           // scCall1/2/4/7
#include "thread.h"            // sys_lwmutex helpers (lock/unlock)
#include "string-utilities.h"  // memCopy, memSet, utf16ToUtf8, strCmpICase
#include <sys/timer.h>         // sys_timer_usleep
#include <cell/rtc.h>          // cellRtcGetCurrentClock: hardware UTC clock (converted to NTFS FILETIME per spec)
'''
c = sub_once(c, old_inc,
    '#include "jf_port.h"           // jf-port: storage (read AND write), clock, sleep and string helpers\n'
    '                               // (replaces vfs/storage-device/syscall/thread/string-utilities/timer/rtc)\n',
    "includes")

c = sub_once(c, '''// lv2 storage syscalls. open/close/read live in storage-device.h (shared device layer);
// only the write path is per-backend.
#define STORAGE_WRITE      603

''', '', "STORAGE_WRITE")

c = sub_once(c, '''static int writeStorageRaw(int storageHandle, uint64_t sector, uint32_t count, const void *buffer, uint32_t *outWritten)
{
   return (int)scCall7(STORAGE_WRITE, (uint64_t)storageHandle, 0, sector, count,
                       (uint64_t)(uintptr_t)buffer, (uint64_t)(uintptr_t)outWritten, 0);
}

''', '', "writeStorageRaw (now in jf_port)")

# every raw transfer through an aligned buffer: several callers hand readSectors/writeSectors a stack
# array (growMft's record template, allocateClusters' bitmap chunk, ...), which lv2's storage DMA
# rejects unless it happens to sit on a 32-byte boundary -- so whether an operation worked depended
# on the compiler's stack layout.
old_rw = c[c.index('// Reads `count` sectors at `lba` into a 32-byte-aligned buffer'):c.index('// True if `p` meets the lv2 storage DMA alignment')]
c = c.replace(old_rw, '''// jf-port: lv2's storage DMA wants a 32-byte-aligned buffer, and several callers below pass a stack
// array (growMft's record template, allocateClusters' bitmap chunk, ...).  Upstream sent those
// straight to the syscall, so whether an operation worked depended on the compiler's stack layout.
// Here an unaligned buffer goes through ioBounce instead, in pieces; aligned ones are unchanged.
#define IO_BOUNCE_BYTES 16384
static uint8_t ioBounce[IO_BOUNCE_BYTES] __attribute__((aligned(32)));

// One storage call, retried while the device reports "not ready". `aligned` must be aligned.
static int readSectorsRaw(int storageHandle, uint64_t lba, uint32_t count, void *aligned)
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

static int writeSectorsRaw(int storageHandle, uint64_t lba, uint32_t count, const void *aligned)
{
   for (int attempt = 0; attempt < SYSIO_RETRY; attempt++) {
      uint32_t put = 0;
      int rc = writeStorageRaw(storageHandle, lba, count, aligned, &put);
      if (rc == 0 && put == count) return 0;
      if ((uint32_t)rc != STORAGE_BUSY) return -1;
      sys_timer_usleep(SYSIO_RETRY_US);
   }
   return -1;
}

// The device sector size behind each open storage handle (mountNtfs registers it right after the
// open), for splitting a bounced transfer.  Every supported size (512..4096) divides IO_BOUNCE_BYTES.
static struct { int handle; uint32_t sectorSize; } ioHandles[8];
static int ioHandleNext = 0;

static void ioRegisterHandle(int handle, uint32_t sectorSize)
{
   for (int i = 0; i < 8; i++)
      if (ioHandles[i].sectorSize && ioHandles[i].handle == handle) { ioHandles[i].sectorSize = sectorSize; return; }
   ioHandles[ioHandleNext].handle = handle;
   ioHandles[ioHandleNext].sectorSize = sectorSize;
   ioHandleNext = (ioHandleNext + 1) % 8;
}

static uint32_t ioSectorSizeOf(int handle)   // 0 = unknown: the bounce path refuses
{
   for (int i = 0; i < 8; i++)
      if (ioHandles[i].sectorSize && ioHandles[i].handle == handle) return ioHandles[i].sectorSize;
   return 0;
}

// Reads `count` sectors at `lba`, retrying while the device reports "not ready" (hotplug
// settling). Returns 0 on success, -1 on a hard error.
static int readSectors(int storageHandle, uint64_t lba, uint32_t count, void *buffer)
{
   if (((uintptr_t)buffer & 31) == 0) return readSectorsRaw(storageHandle, lba, count, buffer);
   const uint32_t ioSectorSize = ioSectorSizeOf(storageHandle);
   if (ioSectorSize == 0 || ioSectorSize > IO_BOUNCE_BYTES) return -1;
   const uint32_t per = IO_BOUNCE_BYTES / ioSectorSize;
   uint8_t *out = (uint8_t *)buffer;
   while (count) {
      uint32_t n = count < per ? count : per;
      if (readSectorsRaw(storageHandle, lba, n, ioBounce) != 0) return -1;
      memCopy(out, ioBounce, (int)(n * ioSectorSize));
      out += n * ioSectorSize; lba += n; count -= n;
   }
   return 0;
}

// Writes `count` sectors at `lba`, retrying while the device reports "not ready". Returns 0 on
// success, -1 on a hard error. Mirror of readSectors.
static int writeSectors(int storageHandle, uint64_t lba, uint32_t count, const void *buffer)
{
   if (((uintptr_t)buffer & 31) == 0) return writeSectorsRaw(storageHandle, lba, count, buffer);
   const uint32_t ioSectorSize = ioSectorSizeOf(storageHandle);
   if (ioSectorSize == 0 || ioSectorSize > IO_BOUNCE_BYTES) return -1;
   const uint32_t per = IO_BOUNCE_BYTES / ioSectorSize;
   const uint8_t *in = (const uint8_t *)buffer;
   while (count) {
      uint32_t n = count < per ? count : per;
      memCopy(ioBounce, in, (int)(n * ioSectorSize));
      if (writeSectorsRaw(storageHandle, lba, n, ioBounce) != 0) return -1;
      in += n * ioSectorSize; lba += n; count -= n;
   }
   return 0;
}

''')
c = sub_once(c, '''   if (openStorage(deviceId, &storageHandle) < 0) return NTFS_MOUNT_NOT_READY;
''', '''   if (openStorage(deviceId, &storageHandle) < 0) return NTFS_MOUNT_NOT_READY;
   ioRegisterHandle(storageHandle, deviceSectorSize);   // jf-port: for bounced (unaligned) transfers
''', "mount registers the handle")

# delete: unreference before freeing, and report failures
c = sub_once(c, '''static void freeMftRecord(NtfsVolume *vol, uint64_t reference)
{''', '''static int freeMftRecord(NtfsVolume *vol, uint64_t reference)
{''', "freeMftRecord int (def)")
c = sub_once(c, '''static void freeMftRecord(NtfsVolume *vol, uint64_t reference);   // clean rollback''',
             '''static int freeMftRecord(NtfsVolume *vol, uint64_t reference);   // clean rollback''', "freeMftRecord int (decl)")
c = sub_once(c, '''   if (readMftRecordByRef(vol, reference, mftRecord) != 0) return;
   writeLe16(mftRecord + FILE_FLAGS, (uint16_t)(readLe16(mftRecord + FILE_FLAGS) & ~FILE_FLAG_IN_USE));
   uint16_t seq = (uint16_t)(readLe16(mftRecord + 16) + 1);
   writeLe16(mftRecord + 16, seq ? seq : 1);            // sequence never 0
   writeMftRecord(vol, number, mftRecord);
   setMftRecordBit(vol, number, 0);
}''', '''   if (readMftRecordByRef(vol, reference, mftRecord) != 0) return -1;
   writeLe16(mftRecord + FILE_FLAGS, (uint16_t)(readLe16(mftRecord + FILE_FLAGS) & ~FILE_FLAG_IN_USE));
   uint16_t seq = (uint16_t)(readLe16(mftRecord + 16) + 1);
   writeLe16(mftRecord + 16, seq ? seq : 1);            // sequence never 0
   // jf-port: the result is reported (upstream ignored it), and the bitmap bit is only cleared once the
   // record itself says "not in use" -- a free bit over an in-use record is a record the next create reuses
   if (writeMftRecord(vol, number, mftRecord) != 0) return -1;
   return setMftRecordBit(vol, number, 0);
}

// jf-port: frees one record and THEN the clusters it referenced.  Upstream freed the clusters first
// and the record after, ignoring both results, so a write cut in between (the drive pulled) left an
// in-use record pointing at clusters marked free -- the next file would be given them -- and the
// delete still returned success.  The runs are read out first, since freeing the record is what
// makes them unreachable.  Returns 0, or -1 when a write failed (the caller reports it).
#define JF_FREE_RUNS_MAX 512
static NtfsRunEntry jfFreeRuns[JF_FREE_RUNS_MAX];
static int freeRecordThenClusters(NtfsVolume *vol, uint64_t reference)
{
   if (readMftRecordByRef(vol, reference, mftRecord) != 0) return -1;
   int total = 0;
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t usedSize   = readLe32(mftRecord + FILE_USED_SIZE);
   uint32_t limit      = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset     = readLe16(mftRecord + FILE_FIRST_ATTR_OFFSET);
   for (int guard = 0; guard < 256; guard++) {
      if (offset + 16 > limit) break;
      uint32_t attrType = readLe32(mftRecord + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) break;
      uint32_t attrLength = readLe32(mftRecord + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) break;
      NtfsAttr a;
      if (parseAttribute(mftRecord + offset, attrLength, &a) && !a.resident) {
         int count = 0;
         if (decodeRuns(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, jfFreeRuns + total,
                        JF_FREE_RUNS_MAX - total, &count) == 0)
            total += count;                               // (more than fit: the rest leak, chkdsk reclaims)
      }
      offset += attrLength;
   }
   if (freeMftRecord(vol, reference) != 0) return -1;   // the clusters stay marked: a leak at worst
   return freeClusterRuns(vol, jfFreeRuns, total) == 0 ? 0 : -1;
}''', "freeMftRecord result + freeRecordThenClusters")
c = sub_once(c, '''static void freeExtensionRecords(NtfsVolume *vol, uint64_t baseRef)
{''', '''static int freeExtensionRecords(NtfsVolume *vol, uint64_t baseRef)
{''', "freeExtensionRecords int")
# gather the extension refs first, free the base, then the extensions (the base references them)
c = sub_once(c, '''   if (readMftRecordByRef(vol, baseRef, mftRecord) != 0) return;   // F048: validate the base ref
   NtfsAttr listAttr;
   if (findAttribute(mftRecord, rs, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1 || !listAttr.resident) return;''',
'''   if (readMftRecordByRef(vol, baseRef, mftRecord) != 0) return -1;   // F048: validate the base ref
   NtfsAttr listAttr;
   if (findAttribute(mftRecord, rs, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1 || !listAttr.resident)
      return freeRecordThenClusters(vol, baseRef);          // jf-port: no extensions: just the base''', "freeExtensionRecords early")
c = sub_once(c, '''   for (int i = 0; i < nrefs; i++) { freeAllNonResident(vol, refs[i]); freeMftRecord(vol, refs[i]); }
}''', '''   // jf-port: the base first (it is what references the extensions), each record before its clusters;
   // a cut anywhere leaves orphans and leaked clusters, which chkdsk reclaims, never a dangling reference
   int rc = freeRecordThenClusters(vol, baseRef);
   for (int i = 0; i < nrefs; i++) if (freeRecordThenClusters(vol, refs[i]) != 0) rc = -1;
   return rc;
}''', "freeExtensionRecords order")
c = sub_once(c, '''   freeExtensionRecords(vol, refFull);
   freeAllNonResident(vol, refFull);
   freeMftRecord(vol, refFull);
   return 0;
}''', '''   // jf-port: base record (with any extensions) freed before its clusters, and a failure reported:
   // the index entry is already gone, so the caller must know the rest did not finish
   return freeExtensionRecords(vol, refFull) == 0 ? 0 : -1;
}''', "unlinkPath frees")
c = sub_once(c, '''static void freeAllNonResident(NtfsVolume *vol, uint64_t reference)
{''', '''static __attribute__((unused)) void freeAllNonResident(NtfsVolume *vol, uint64_t reference)
{''', "freeAllNonResident unused")

# a record bigger than a cluster: one storage write per physically contiguous stretch
c = sub_once(c, '''      uint32_t clusters = recordSize / bytesPerCluster;
      for (uint32_t c = 0; c < clusters; c++) {
         int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster + c);
         if (lcn < 0) return -1;
         uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster;
         if (writeSectors(vol->storageHandle, lba, bytesPerCluster / vol->bytesPerSector, record + c * bytesPerCluster) != 0)
            return -1;
      }''', '''      // jf-port: one storage write per physically contiguous stretch of the record (nearly always the
      // whole record).  Upstream wrote cluster by cluster, so with clusters smaller than a record a
      // write cut part way (the drive pulled) left a record torn in two that nothing can read.
      uint32_t clusters = recordSize / bytesPerCluster;
      for (uint32_t c = 0; c < clusters; ) {
         int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster + c);
         if (lcn < 0) return -1;
         uint32_t run = 1;
         while (c + run < clusters &&
                mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster + c + run) == lcn + (int64_t)run)
            run++;
         uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster;
         if (writeSectors(vol->storageHandle, lba, run * (bytesPerCluster / vol->bytesPerSector), record + c * bytesPerCluster) != 0)
            return -1;
         c += run;
      }''', "writeMftRecord contiguous")

# the mount gate: a $Volume record that cannot be read is not evidence of a clean volume
c = sub_once(c, '''         utf16ToUtf8(units, vol->label, (int)sizeof(vol->label));
      }
   }
   if (vol->writable && checkLogFileClean(vol) == NTFS_LOG_DIRTY)''', '''         utf16ToUtf8(units, vol->label, (int)sizeof(vol->label));
      }
   } else {
      vol->writable = 0;   // jf-port: $Volume unreadable (a torn record): its dirty flag is unknown -> read only
   }
   if (vol->writable && checkLogFileClean(vol) == NTFS_LOG_DIRTY)''', "mount gate volume unreadable")

c = sub_once(c, '''         if (readLe16(volumeInfo.value + VOLINFO_FLAGS_OFFSET) & VOLUME_FLAG_DIRTY)
            vol->writable = 0;
      }
''', '''         if (readLe16(volumeInfo.value + VOLINFO_FLAGS_OFFSET) & VOLUME_FLAG_DIRTY)
            vol->writable = 0;
      } else {
         vol->writable = 0;   // jf-port: no readable $VOLUME_INFORMATION: the dirty flag is unknown -> read only
      }
''', "mount gate no volinfo")

# growMft's record formatting, cluster by cluster
c = sub_once(c, '''        uint64_t bo = n * rs;
        int64_t lcn = mapVcnToLcn(nr, nrc, bo / cb);
        if (lcn < 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
        uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + (uint32_t)(bo % cb) / vol->bytesPerSector;
        if (writeSectors(vol->storageHandle, lba, rs / vol->bytesPerSector, tmpl) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }''',
'''        uint64_t bo = n * rs;
        // jf-port: map each cluster of the record.  Upstream mapped only the first and wrote the whole
        // record from there, so with clusters smaller than a record (512-byte clusters, 1 KB records)
        // a record straddling two runs wrote its second half over whatever cluster came next on the
        // disk -- another file's data.  (writeMftRecord already maps cluster by cluster.)
        for (uint32_t done = 0; done < rs; ) {
           const uint64_t b = bo + done;
           int64_t lcn = mapVcnToLcn(nr, nrc, b / cb);
           if (lcn < 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
           const uint32_t inCluster = (uint32_t)(b % cb);
           uint32_t take = cb - inCluster;
           while (done + take < rs && mapVcnToLcn(nr, nrc, (b + take) / cb) == lcn + (int64_t)((inCluster + take) / cb))
              take += cb;                                   // and the clusters right after it, in one write
           if (take > rs - done) take = rs - done;
           uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + inCluster / vol->bytesPerSector;
           if (writeSectors(vol->storageHandle, lba, take / vol->bytesPerSector, tmpl + done) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
           done += take;
        }''', "growMft format per cluster")

# $MFT growth in big steps (see README.md)
c = sub_once(c, '''   uint64_t oldRecs = data.realSize / rs;
   if (addRecords == 0) addRecords = 1;''', '''   uint64_t oldRecs = data.realSize / rs;
   // jf-port: grow by a quarter of the $MFT (at least 256 records), not by the 8 the callers ask for.
   // The $MFT may have at most NTFS_MFT_RUNS_MAX fragments, and each growth lands wherever the free
   // clusters after its tail are -- usually not adjacent once files have been written there -- so
   // 8-record steps ran out of fragments after ~250 files and every create failed from then on.
   // Capped at 1/16 of the free space, so a nearly full volume still grows by what it can.
   { uint64_t step = oldRecs / 4 < 256 ? 256 : oldRecs / 4;
     uint64_t capRecs = (vol->freeClusters / 16) * (uint64_t)cb / rs;
     if (step > capRecs) step = capRecs;
     if (addRecords < step) addRecords = step; }
   if (addRecords == 0) addRecords = 1;''', "growMft step")

# allocation regions for streaming files (see README.md)
c = sub_once(c, '''   // P5: with no caller tail-hint, resume from where the last allocation left off instead of rescanning
   // the filled prefix from 0 every time. It's only a scan-start hint - the wrap below still reaches any
   // freed clusters before it, so correctness is unaffected by a stale hint.
   uint64_t effectiveHint = hintLcn ? hintLcn : vol->allocHint;''', '''   // P5: with no caller tail-hint, resume from where the last allocation left off instead of rescanning
   // the filled prefix from 0 every time. It's only a scan-start hint - the wrap below still reaches any
   // freed clusters before it, so correctness is unaffected by a stale hint.
   // jf-port: two cursors.  A file growing past JF_STREAM_BYTES (a download) is a STREAM: with no
   // tail hint it starts in the stream region (from an eighth of the way into the volume) and then
   // extends from its own tail; everything else -- records, artwork, index blocks, $MFT bitmap --
   // takes the small cursor from the front.  Upstream had one cursor, so the record a download saves
   // at every checkpoint was allocated right after the download's tail and broke it into a new
   // fragment each time, until it hit NTFS_MAX_FILE_RUNS and the download could not grow.
   const int stream = vol->allocStream;
   vol->allocStream = 0;
   uint64_t effectiveHint = hintLcn ? hintLcn
                          : stream  ? (vol->allocHint ? vol->allocHint : clusterCount / 8)
                          :           vol->smallHint;''', "allocateClusters cursors")
c = sub_once(c, '''   if (*count > startCount && runs[*count - 1].lcn >= 0)
      vol->allocHint = (uint64_t)runs[*count - 1].lcn + runs[*count - 1].count;''', '''   if (*count > startCount && runs[*count - 1].lcn >= 0) {
      const uint64_t end = (uint64_t)runs[*count - 1].lcn + runs[*count - 1].count;
      if (stream) vol->allocHint = end;            // jf-port: each cursor moves on its own
      else if (!hintLcn) vol->smallHint = end;
   }''', "allocateClusters cursor update")
c = sub_once(c, '''      uint64_t reserveCap = (1u << 20) / bytesPerCluster;        // ~1 MB of reserve, scaled to cluster size''',
'''      // jf-port: 16 MB, not 1 MB: a download grows by 16 MB at a time (trimmed on close as before)
      uint64_t reserveCap = (newSize >= JF_STREAM_BYTES ? (16u << 20) : (1u << 20)) / bytesPerCluster;''', "reserve cap")
c = sub_once(c, '''   uint64_t hintLcn = (runCount > 0 && runs[runCount - 1].lcn >= 0)
                      ? (uint64_t)runs[runCount - 1].lcn + runs[runCount - 1].count : 0;
   if (addClusters && allocateClusters(vol, addClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, hintLcn) != 0)''', '''   uint64_t hintLcn = (runCount > 0 && runs[runCount - 1].lcn >= 0)
                      ? (uint64_t)runs[runCount - 1].lcn + runs[runCount - 1].count : 0;
   // jf-port: a stream (see allocateClusters) whose tail is still in the small region moves to the
   // stream region once, at JF_STREAM_BYTES; after that it extends from its tail
   if (newSize >= JF_STREAM_BYTES && addClusters) {
      vol->allocStream = 1;                        // read and cleared by the allocateClusters just below
      if (hintLcn < (vol->totalSectors / vol->sectorsPerCluster) / 8) hintLcn = 0;
   }
   if (addClusters && allocateClusters(vol, addClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, hintLcn) != 0)''', "appendData stream hint")
# clearing the dirty flag: $MFTMirr first, then $MFT, so a cut between the two leaves the PRIMARY
# saying dirty (chkdsk on the next Windows mount puts the mirror right) instead of saying clean
# over a mirror that disagrees (which ntfs-3g refuses to mount)
c = sub_once(c, '''static int writeMftRecord(const NtfsVolume *vol, uint64_t number, uint8_t *record)
{
   applyUsaWrite(record, vol->mftRecordSize, vol->bytesPerSector);
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t byteOffset = number * recordSize;
''', '''// jf-port: set by setVolumeDirty while it CLEARS the flag: $MFTMirr's copy is written first, so a cut
// between the two writes leaves $MFT (what every reader looks at) still saying dirty.
static int mftMirrorFirst = 0;

static int writeMftMirror(const NtfsVolume *vol, uint64_t number, const uint8_t *record)
{
   uint64_t mlba = vol->partitionOffset + vol->mftMirrLcn * vol->sectorsPerCluster
                 + (number * vol->mftRecordSize) / vol->bytesPerSector;
   return writeSectors(vol->storageHandle, mlba, vol->mftRecordSize / vol->bytesPerSector, record);
}

static int writeMftRecord(const NtfsVolume *vol, uint64_t number, uint8_t *record)
{
   applyUsaWrite(record, vol->mftRecordSize, vol->bytesPerSector);
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t byteOffset = number * recordSize;
   const int mirrored = number < NTFS_MFTMIRR_RECORDS && vol->mftMirrLcn != 0;
   if (mirrored && mftMirrorFirst && writeMftMirror(vol, number, record) != 0) return -1;   // jf-port
''', "writeMftRecord mirror-first")
c = sub_once(c, '''   if (number < NTFS_MFTMIRR_RECORDS && vol->mftMirrLcn != 0) {
      uint64_t mlba = vol->partitionOffset + vol->mftMirrLcn * vol->sectorsPerCluster
                    + (number * recordSize) / vol->bytesPerSector;
      if (writeSectors(vol->storageHandle, mlba, recordSize / vol->bytesPerSector, record) != 0) return -1;
   }''', '''   if (mirrored && !mftMirrorFirst && writeMftMirror(vol, number, record) != 0) return -1;''', "writeMftRecord mirror after")
c = sub_once(c, '''   writeLe16(flagsField, updated);
   return writeMftRecord(vol, MFT_RECORD_VOLUME, mftRecord);''', '''   writeLe16(flagsField, updated);
   mftMirrorFirst = !dirty;                                // jf-port: clearing -> mirror first
   int rc = writeMftRecord(vol, MFT_RECORD_VOLUME, mftRecord);
   mftMirrorFirst = 0;
   return rc;''', "setVolumeDirty mirror order")

# free-cluster accounting on allocateClusters' failure paths: upstream subtracted `allocated` only on
# the path that reaches the end of the scan, but freeTailClusters always adds back every bit it clears,
# so a fail from inside the scan (runs[] full, a bitmap I/O error) left freeClusters too high by the
# bits already written.  Count what reached the disk and settle exactly that.
c = sub_once(c, '''   uint64_t allocated = 0;
   // two ranges so the scan wraps once past the hint''', '''   uint64_t allocated = 0;
   uint64_t committed = 0;   // jf-port: bits set on disk and not yet taken off vol->freeClusters
   // two ranges so the scan wraps once past the hint''', "alloc committed decl")
c = sub_once(c, '''         int modified = 0;
         for (uint32_t i = 0; i < want && allocated < needed; i++) {''', '''         int modified = 0;
         const uint64_t allocatedBefore = allocated;
         for (uint32_t i = 0; i < want && allocated < needed; i++) {''', "alloc chunk start")
c = sub_once(c, '''         if (modified && writeBitmapBytes(vol, byteOffset, chunk, want) != 0) goto fail;
         byteOffset += want;''', '''         if (modified && writeBitmapBytes(vol, byteOffset, chunk, want) != 0) goto fail;
         committed += allocated - allocatedBefore;
         byteOffset += want;''', "alloc chunk commit")
c = sub_once(c, '''   vol->freeClusters -= allocated;   // each set bit was a free->used flip (fail path re-adds via setClusterBits)''',
'''   vol->freeClusters -= committed;   // each set bit was a free->used flip (fail path re-adds via setClusterBits)
   committed = 0;''', "alloc settle")
c = sub_once(c, '''fail:
   freeTailClusters(vol, runs, count, allocated);
   return -1;''', '''fail:
   vol->freeClusters -= committed;   // jf-port: what reached the disk, which freeTailClusters gives back
   freeTailClusters(vol, runs, count, allocated);
   return -1;''', "alloc fail settle")

# when the reserve does not fit, take exactly what the write needs (allocateClusters frees what it
# took on failure, so runs/runCount are as before the first try)
c = sub_once(c, '''   if (addClusters && allocateClusters(vol, addClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, hintLcn) != 0)
      return -1;''', '''   if (addClusters && allocateClusters(vol, addClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, hintLcn) != 0) {
      // jf-port: no room for the reserve-ahead: take exactly what this write needs, so a volume
      // fills to its last cluster instead of failing with the reserve's worth still free
      if (needClusters <= haveClusters || addClusters == needClusters - haveClusters) return -1;
      addClusters = needClusters - haveClusters;
      if (newSize >= JF_STREAM_BYTES) vol->allocStream = 1;
      if (allocateClusters(vol, addClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, hintLcn) != 0) return -1;
   }''', "appendData exact retry")
c = sub_once(c, '#define NTFS_MAX_FILE_RUNS', '#define JF_STREAM_BYTES (4ull << 20)   // jf-port: a file this big allocates as a stream (allocateClusters)\n#define NTFS_MAX_FILE_RUNS', "JF_STREAM_BYTES")

# the sparse-only compression-unit fix (see README.md)
c = sub_once(c, '''     out->compUnitClusters = (cu > 0 && cu < 24) ? (1u << cu) : 0; }''',
'''     // jf-port: the compression unit applies only to a COMPRESSED attribute.  A sparse-only one
     // (flag 0x8000, which Windows and ntfs-3g also give a unit of 16 clusters) stores its allocated
     // clusters as plain data, so it must read through the normal runlist path, holes as zeros.
     // Upstream decoded those clusters as LZNT1 and failed any sparse file with a written extent.
     out->compUnitClusters = ((readLe16(attr + ATTR_FLAGS_OFFSET) & ATTR_FLAG_COMPRESSED) && cu > 0 && cu < 24)
                                ? (1u << cu) : 0; }''', "sparse fix")

# clock: the RTC module call -> jf_port's UTC clock
m = re.search(r'static uint64_t nowFiletime\(void\)\n\{\n.*?\n\}\n', c, re.S)
if not m: sys.exit("nowFiletime not found")
c = c[:m.start()] + '''static uint64_t nowFiletime(void)
{
   // jf-port: lv2's UTC clock (jf_port_utc_seconds) instead of cellRtcGetCurrentClock; the same
   // fallback to the fixed constant when the clock is unset or out of range.
   uint64_t unixSeconds = jf_port_utc_seconds();
   if (unixSeconds < 946684800ULL || unixSeconds > 253402300799ULL) return NTFS_NEW_FILETIME;   // before 2000 / after 9999
   return unixSeconds * 10000000ULL + 116444736000000000ULL;   // 100 ns ticks since 1601-01-01
}
''' + c[m.end():]

c = sub_once(c, 'static uint64_t civilToFiletime(',
    "// jf-port: unused since nowFiletime reads lv2's clock as unix seconds; kept as upstream wrote it.\n"
    'static __attribute__((unused)) uint64_t civilToFiletime(', "civilToFiletime")

# drop the VFS backend half: everything from the pools banner to the end
banner = '''// ===========================================================================
// Fixed pools indexed by VfsDir/VfsFile.descriptor (mirror exfat.c sizes).'''
i = c.find(banner)
if i < 0: sys.exit("VFS banner not found")
c = c[:i] + '''// ===========================================================================
// jf-port: the VFS backend half of the original (volume and handle pools, the backend mutex, the
// open/close/dirty-flag wrappers and VFS registration) is not vendored.  source/local/lfs.cpp owns
// volumes, handles and the one lock, and does what the wrappers did around a write:
//   * a write open needs vol->writable (a clean mount);
//   * after a writer closes, or a create/mkdir/unlink/rmdir/rename finishes, and no writer is
//     still open, the on-disk dirty flag is cleared with ntfsClearDirty().
// ===========================================================================

// Clears the on-disk volume-dirty flag this session set (no-op when it never set it).  The caller
// holds the lock and has checked that no writer is open on the volume.  Returns 0 or -1.
int ntfsClearDirty(NtfsVolume *vol)
{
   if (!vol || !vol->mounted || !vol->volumeDirty || !vol->writable) return 0;
   if (setVolumeDirty(vol, 0) != 0) return -1;
   vol->volumeDirty = 0;
   return 0;
}
'''

# the file header note
c = sub_once(c, '#include "jf_port.h"', '''// jf-port: vendored from mohasi/ps3-dev (Apache-2.0); see README.md in this directory.
// Reads AND writes (the write path is upstream's, unchanged).  The VFS backend half is not
// vendored; source/local/lfs.cpp is the only caller and holds ONE lock around every call into
// this file, as the original's *Op wrappers did (the functions share static scratch buffers).
#include "jf_port.h"''', "header note")

for bad in ("scCall", "sys_lwmutex", "cellRtc", "VfsOps", "registerVfsBackend"):
    if bad in c:
        for ln, line in enumerate(c.splitlines(), 1):
            if bad in line: print(f"leftover {bad} at {ln}: {line.strip()}")

# ---- ntfs.h --------------------------------------------------------------------------------
h = sub_once(h, '''// Registers the NTFS backend with the VFS and mounts any present NTFS volumes as /ntfs<port>.
// Called by initVfs() so NTFS comes up with the VFS; stays light and prx-safe.
void initNtfs(void);''', '''// jf-port: clears the on-disk dirty flag this session set; call with the lock held once no writer
// is open on the volume (what the original's clearDirtyIfQuiet did).  Returns 0 or -1.
int  ntfsClearDirty(NtfsVolume *vol);''', "initNtfs decl")
h = sub_once(h, '''   uint64_t allocHint;           // next-free-cluster scan hint for allocateClusters (avoids rescanning the filled prefix)
''', '''   uint64_t allocHint;           // next-free-cluster scan hint for allocateClusters (avoids rescanning the filled prefix)
                                 // jf-port: the STREAM cursor; smallHint is the cursor for everything else
   uint64_t smallHint;           // jf-port: scan start for small allocations (records, artwork, index blocks)
   int      allocStream;         // jf-port: set by appendData for the one allocateClusters call that follows
''', "header cursors")
h = sub_once(h, '#pragma once\n', '''#pragma once
// jf-port: vendored from mohasi/ps3-dev (Apache-2.0); see README.md in this directory.  Reads and
// writes.  Comments below that mention the VFS backend are the original's: here the caller is
// source/local/lfs.cpp, which holds the one lock around every call.
''', "h note")

open(f"{out}/ntfs.c", "w", encoding="utf-8").write(c)
open(f"{out}/ntfs.h", "w", encoding="utf-8").write(h)
print("ok", len(c.splitlines()), "lines")
