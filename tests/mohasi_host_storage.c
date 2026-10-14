// Host stand-in for the lv2 storage syscalls the vendored NTFS/exFAT readers call
// (third_party/mohasi_fs/jf_port.h): each USB port's "device" is an image file.
//
// Writes go to the image (open it O_RDWR on a copy).  `g_port_fail[p]` makes every read and
// write fail the way a pulled drive does; `g_port_write_budget[p]` (>= 0) lets that many more
// writes through and then pulls the drive, to cut a write sequence at any point.

#define _FILE_OFFSET_BITS 64
#include "jf_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#if defined(__linux__) && !defined(__powerpc64__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

int g_port_fd[8]   = { -1, -1, -1, -1, -1, -1, -1, -1 };   // the image on each port, opened read-only
int g_port_fail[8] = { 0 };                                // 1 = every read fails (device removed)
unsigned long g_port_reads[8] = { 0 };                     // sector reads served per port
unsigned long g_port_writes[8] = { 0 };                    // write calls served per port
long g_port_write_budget[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };   // writes left before the drive is pulled; -1 = no limit
uint64_t g_utc_seconds = 1760000000ULL;                    // the clock files are stamped with (2025-10-09)

uint64_t jf_port_utc_seconds(void) { return g_utc_seconds; }

void jf_port_usleep(unsigned usec) { (void)usec; }   // nothing to wait for on the host

static int port_of(uint64_t id)
{
   if (id >= 0x10300000000000AULL && id <= 0x10300000000000AULL + 5) return (int)(id - 0x10300000000000AULL);
   if (id >= 0x10300000000001FULL && id <= 0x10300000000001FULL + 1) return 6 + (int)(id - 0x10300000000001FULL);
   return -1;
}

int getStorageInfo(uint64_t deviceId, StorageDeviceInfo *info)
{
   const int p = port_of(deviceId);
   if (p < 0 || g_port_fd[p] < 0) return -1;
   struct stat st;
   if (fstat(g_port_fd[p], &st) != 0) return -1;
   memset(info, 0, sizeof *info);
   snprintf(info->label, sizeof info->label, "TEST");
   info->sectorSize  = 512;
   info->sectorCount = (uint64_t)st.st_size / 512;
   return 0;
}

int openStorage(uint64_t deviceId, int *outStorageHandle)
{
   const int p = port_of(deviceId);
   if (p < 0 || g_port_fd[p] < 0) return -1;
   *outStorageHandle = p + 1;
   return 0;
}

int closeStorage(int storageHandle) { (void)storageHandle; return 0; }

int readStorageRaw(int storageHandle, uint64_t sector, uint32_t count, void *buffer, uint32_t *outRead)
{
   const int p = storageHandle - 1;
   *outRead = 0;
   if (p < 0 || p > 7 || g_port_fail[p] || g_port_fd[p] < 0) return (int)0x8001002f;   // ENXIO: not "busy", so no retry
   if (((uintptr_t)buffer & 31) != 0) return (int)0x80010009;  // lv2 wants 32-byte alignment
   ssize_t want = (ssize_t)count * 512;
   ssize_t got = pread(g_port_fd[p], buffer, (size_t)want, (off_t)(sector * 512ULL));
   if (got != want) return (int)0x8001002f;
   g_port_reads[p]++;
   *outRead = count;
   return 0;
}

int writeStorageRaw(int storageHandle, uint64_t sector, uint32_t count, const void *buffer, uint32_t *outWritten)
{
   const int p = storageHandle - 1;
   *outWritten = 0;
   if (p < 0 || p > 7 || g_port_fail[p] || g_port_fd[p] < 0) return (int)0x8001002f;
   if (g_port_write_budget[p] == 0) { g_port_fail[p] = 1; return (int)0x8001002f; }   // pulled mid-sequence
   if (((uintptr_t)buffer & 31) != 0) return (int)0x80010009;
   struct stat st;
   if (fstat(g_port_fd[p], &st) != 0 || (sector + count) * 512ULL > (uint64_t)st.st_size)
      return (int)0x80010002 + 0x2d;   /* past the end of the device: a hard error, never a grow */
   ssize_t want = (ssize_t)count * 512;
   ssize_t put = pwrite(g_port_fd[p], buffer, (size_t)want, (off_t)(sector * 512ULL));
   if (put != want) return (int)0x8001002f;
   {   // debugging aid: MOHASI_TRACE_SECTOR=<n> prints who writes over sector n (host builds only)
      static long long watch = -2;
      if (watch == -2) { const char *e = getenv("MOHASI_TRACE_SECTOR"); watch = e ? strtoll(e, NULL, 0) : -1; }
      if (watch >= 0 && (uint64_t)watch >= sector && (uint64_t)watch < sector + count) {
         const uint8_t *at = (const uint8_t *)buffer + (watch - (long long)sector) * 512;
         fprintf(stderr, "write over sector %lld: lba %llu count %u, first bytes %02x%02x%02x%02x\n", watch,
                 (unsigned long long)sector, count, at[0], at[1], at[2], at[3]);
#ifdef HAVE_BACKTRACE
         void *bt[16]; int n = backtrace(bt, 16); backtrace_symbols_fd(bt, n, 2);
#endif
      }
   }
   if (g_port_write_budget[p] > 0) g_port_write_budget[p]--;
   g_port_writes[p]++;
   *outWritten = count;
   return 0;
}
