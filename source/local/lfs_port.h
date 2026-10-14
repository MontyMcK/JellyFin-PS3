#pragma once
#include <stdbool.h>
#include <stdint.h>

// The platform seam under lfs.cpp: what the console provides through lv2 (lfs_port_ps3.cpp) and
// the host tests provide over a directory tree (tests/lfs_port_host.cpp).  The raw NTFS/exFAT
// readers have their own seam (third_party/mohasi_fs/jf_port.h: four storage calls).

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // --- the USB ports ---------------------------------------------------------
    // A mass-storage device is present on the port.  Must not do DMA (it is polled).
    bool (*usb_present)(int port);
    // lv2 has mounted a file system for the port (FAT32): "/dev_usb00N" exists.
    bool (*fat_mounted)(int port);

    // --- native file system: lv2's mounts (FAT32 USB, the internal disk) ----------
    // Paths are lv2 paths ("/dev_usb000/Movies", "/dev_hdd0/video").
    // Calls cb for every entry but "." and "..".  Returns 0, or -1 when the folder cannot be read.
    int  (*fs_list)(const char *native, void (*cb)(const char *name, bool is_dir, void *ctx), void *ctx);
    // 0 and the fields, or -1.  Folders report size 0.
    int  (*fs_stat)(const char *native, bool *is_dir, uint64_t *size, uint64_t *mtime);
    int  (*fs_open)(const char *native);                         // a descriptor >= 0, or -1
    int  (*fs_read)(int fd, uint64_t off, void *buf, uint32_t n); // bytes (0 at the end), or -1
    void (*fs_close)(int fd);
    // Free and total bytes of the volume holding `native`.  False when unknown.
    bool (*fs_space)(const char *native, uint64_t *free_bytes, uint64_t *total_bytes);
    int  (*fs_mkdir)(const char *native);                        // 0 or already there; -1 failed

    // --- the rest --------------------------------------------------------------
    uint64_t (*now_us)(void);                                    // a clock that only goes forward
    void (*lock)(int which);                                     // 0 = table lock, 1 = i/o lock
    void (*unlock)(int which);
    void (*log)(const char *line);                               // may be NULL
} LfsPort;

// Installs the platform.  lfs_start() on the console installs its own; tests call this first.
void lfs_set_port(const LfsPort *port);

#ifdef __cplusplus
}
#endif
