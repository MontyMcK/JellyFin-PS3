#pragma once
// jf-port: the platform layer for the vendored mohasi NTFS (read/write) and exFAT (read) drivers.
//
// Replaces mohasi's storage-device.h, syscall.h, thread.h, string-utilities.h,
// <sys/timer.h> and <cell/rtc.h> with the little they used.  The storage calls
// are implemented by jf_port_ps3.c on the console (lv2 storage syscalls
// 600/601/602/609) and by tests/mohasi_host_storage.c on the host, where they
// read an image file.  The helpers below are copied unchanged from mohasi's
// string-utilities.h (Apache-2.0).

#include <stdint.h>

#define USB_STORAGE_MAX_PORTS  8     // lv2 exposes USB mass-storage on ports 0..7
#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 512                // in-volume path buffers in the write path (upstream path.h)
#endif

// USB mass-storage device id for a port (0-5 and 6+ use different bases).
static inline uint64_t getUsbDeviceId(int port)
{
   if (port < 0) port = 0;
   if (port >= USB_STORAGE_MAX_PORTS) port = USB_STORAGE_MAX_PORTS - 1;
   return port < 6 ? 0x10300000000000AULL + (uint64_t)port
                   : 0x10300000000001FULL + (uint64_t)(port - 6);
}

// lv2 storage device info; layout mirrors sys_device_info_t.
typedef struct {
   char     label[32];
   uint32_t reserved1;
   uint32_t reserved2;
   uint64_t sectorCount;
   uint32_t sectorSize;
   uint32_t reserved3;
   uint8_t  reserved4[8];
} __attribute__((packed)) StorageDeviceInfo;

// 0 = a device is present on that id (and info is filled).  Does no DMA: safe to poll.
int getStorageInfo(uint64_t deviceId, StorageDeviceInfo *info);
int openStorage(uint64_t deviceId, int *outStorageHandle);
int closeStorage(int storageHandle);
// One raw read of `count` sectors into a 32-byte-aligned buffer; outRead receives
// the sectors transferred.  0 on success, an lv2 error otherwise.
int readStorageRaw(int storageHandle, uint64_t sector, uint32_t count, void *buffer, uint32_t *outRead);
// One raw write (lv2 syscall 603), the same shape.  Only ntfs.c's write path calls it; exfat.c
// stays read-only (tests/Makefile.host check-mohasi-readonly).
int writeStorageRaw(int storageHandle, uint64_t sector, uint32_t count, const void *buffer, uint32_t *outWritten);

// The wall clock, UTC, in unix seconds (0 when unknown): timestamps of files the writer creates.
uint64_t jf_port_utc_seconds(void);

// Sleep, in microseconds (sys_timer_usleep in the original).
void jf_port_usleep(unsigned usec);
#define sys_timer_usleep jf_port_usleep

// jf-port: the helpers below are upstream's, unchanged; its formatting trips GCC's indentation warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

// Returns length of a string.
static inline int getStrLen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

// Byte-wise copy. Use instead of libc memcpy in code that links against a
// stripped libc (e.g. vsh prx imports — memcpy is not exported there).
static inline void memCopy(void *dst, const void *src, int n)
{
   unsigned char *d = (unsigned char *)dst;
   const unsigned char *s = (const unsigned char *)src;
   for (int i = 0; i < n; i++) d[i] = s[i];
}

// Byte-wise fill. Use instead of libc memset in prx-sensitive code.
static inline void memSet(void *dst, unsigned char value, int n)
{
   unsigned char *d = (unsigned char *)dst;
   for (int i = 0; i < n; i++) d[i] = value;
}

// Strict string equality. Same reason as memCopy: avoids libc strcmp.
static inline int strEq(const char *a, const char *b)
{
   while (*a && *b && *a == *b) { a++; b++; }
   return *a == *b;
}

// ASCII-only lowercase for a single character.
static inline char toLowerChar(char c)
{
   return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Case-insensitive string comparison.
static inline int strCmpICase(const char *a, const char *b)
{
   while (*a && *b) {
     char ca = *a, cb = *b;
     ca = toLowerChar(ca);
     cb = toLowerChar(cb);
     if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
     a++; b++;
   }
   return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// Converts a UTF-8 string to UTF-16, emitting surrogate pairs for astral code
// points and skipping malformed bytes. Writes at most maxUnits code units plus a
// NUL terminator. The PS3 system APIs (e.g. cellOskDialog) speak UTF-16.
// `in` MUST be NUL-terminated: continuation-byte validation relies on the NUL to
// stop at a truncated trailing lead byte. A non-terminated slice would let a
// dangling multi-byte lead read one byte past the buffer -- wrap such input in a
// NUL-terminated copy first (urlDecode handles its own length-delimited input).
static inline void utf8ToUtf16(const char *in, uint16_t *out, int maxUnits)
{
   int n = 0;
   const unsigned char *s = (const unsigned char *)in;
   if (!s) { out[0] = 0; return; }

   while (*s && n < maxUnits) {
     uint32_t cp;
     if (*s < 0x80) {
       cp = *s++;
     } else if ((*s & 0xE0) == 0xC0) {
       cp = *s++ & 0x1F;
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
     } else if ((*s & 0xF0) == 0xE0) {
       cp = *s++ & 0x0F;
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
     } else if ((*s & 0xF8) == 0xF0) {
       cp = *s++ & 0x07;
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
     } else {
       s++;  continue;
     }

     if (cp <= 0xFFFF) {
       out[n++] = (uint16_t)cp;
     } else if (n + 1 < maxUnits) {
       cp -= 0x10000;
       out[n++] = (uint16_t)(0xD800 | (cp >> 10));
       out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
     }
   }
   out[n] = 0;
}

// Converts a NUL-terminated UTF-16 string to UTF-8, decoding surrogate pairs.
// Writes a NUL-terminated result bounded by cap bytes. Inverse of utf8ToUtf16.
static inline void utf16ToUtf8(const uint16_t *in, char *out, int cap)
{
   int o = 0;
   if (!in) { out[0] = 0; return; }

   for (int i = 0; in[i] && o + 4 < cap; i++) {
     uint32_t cp = in[i];
     if (cp >= 0xD800 && cp <= 0xDBFF && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF)
       cp = 0x10000 + ((cp - 0xD800) << 10) + (in[++i] - 0xDC00);
     else if (cp >= 0xD800 && cp <= 0xDFFF)
       cp = 0xFFFD;   // unpaired surrogate: emit U+FFFD, not an ill-formed WTF-8 3-byte sequence

     if (cp < 0x80) {
       out[o++] = (char)cp;
     } else if (cp < 0x800) {
       out[o++] = (char)(0xC0 | (cp >> 6));
       out[o++] = (char)(0x80 | (cp & 0x3F));
     } else if (cp < 0x10000) {
       out[o++] = (char)(0xE0 | (cp >> 12));
       out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
       out[o++] = (char)(0x80 | (cp & 0x3F));
     } else {
       out[o++] = (char)(0xF0 | (cp >> 18));
       out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
       out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
       out[o++] = (char)(0x80 | (cp & 0x3F));
     }
   }
   out[o] = 0;
}

#pragma GCC diagnostic pop
