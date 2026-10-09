// jf-port: the console side of jf_port.h.  The five storage calls are the lv2 syscalls the
// original reached through its scCall trampolines (600 open, 601 close, 602 read, 603 write,
// 609 get_device_info); the return values are passed through unchanged.  Only ntfs.c's write
// path calls the write.
//
// They need the same firmware support (HEN/CFW) the app already requires.
//
// PSL1GHT's lv2syscallN are statement macros that leave the result in a register, finished
// by return_to_user_prog(), so each call is a function of its own.

#include "jf_port.h"

#include <ppu-lv2.h>
#include <unistd.h>
#include <sys/systime.h>

#define STORAGE_OPEN      600
#define STORAGE_CLOSE     601
#define STORAGE_READ      602
#define STORAGE_WRITE     603
#define STORAGE_GET_INFO  609

int getStorageInfo(uint64_t deviceId, StorageDeviceInfo *info)
{
   lv2syscall2(STORAGE_GET_INFO, deviceId, (uint64_t)(uintptr_t)info);
   return_to_user_prog(int);
}

int openStorage(uint64_t deviceId, int *outStorageHandle)
{
   lv2syscall4(STORAGE_OPEN, deviceId, 0, (uint64_t)(uintptr_t)outStorageHandle, 0);
   return_to_user_prog(int);
}

int closeStorage(int storageHandle)
{
   lv2syscall1(STORAGE_CLOSE, (uint64_t)storageHandle);
   return_to_user_prog(int);
}

int readStorageRaw(int storageHandle, uint64_t sector, uint32_t count, void *buffer, uint32_t *outRead)
{
   lv2syscall7(STORAGE_READ, (uint64_t)storageHandle, 0, sector, count,
               (uint64_t)(uintptr_t)buffer, (uint64_t)(uintptr_t)outRead, 0);
   return_to_user_prog(int);
}

int writeStorageRaw(int storageHandle, uint64_t sector, uint32_t count, const void *buffer, uint32_t *outWritten)
{
   lv2syscall7(STORAGE_WRITE, (uint64_t)storageHandle, 0, sector, count,
               (uint64_t)(uintptr_t)buffer, (uint64_t)(uintptr_t)outWritten, 0);
   return_to_user_prog(int);
}

uint64_t jf_port_utc_seconds(void)
{
   u64 sec = 0, nsec = 0;
   return sysGetCurrentTime(&sec, &nsec) == 0 ? (uint64_t)sec : 0;
}

void jf_port_usleep(unsigned usec) { usleep(usec); }
