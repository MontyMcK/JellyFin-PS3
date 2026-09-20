#pragma once
// Host stand-in for PSL1GHT's <sys/file.h>, so theme.cpp can be compiled and
// exercised on the build machine.  Only the three directory syscalls
// theme_scan() uses are provided, mapped onto POSIX opendir/readdir, and the
// sysFSDirent layout matches lv2/sysfs.h (d_type, d_namlen, d_name).
//
// Behaviour parity is the point: sysLv2FsReadDir returns 0 with *read == 0 at
// end of directory rather than a distinct EOF code, so the stub does the same.

#include <ppu-types.h>
#include <dirent.h>
#include <string.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

typedef struct _sys_fs_dirent {
    u8   d_type;
    u8   d_namlen;
    char d_name[MAXPATHLEN + 1];
} sysFSDirent;

#define HOSTSTUB_MAX_DIRS 8
static DIR *hoststub_dirs[HOSTSTUB_MAX_DIRS];

static inline s32 sysLv2FsOpenDir(const char *path, s32 *fd) {
    for (int i = 0; i < HOSTSTUB_MAX_DIRS; i++) {
        if (hoststub_dirs[i]) continue;
        DIR *d = opendir(path);
        if (!d) return -1;
        hoststub_dirs[i] = d;
        *fd = i;
        return 0;
    }
    return -1;
}

static inline s32 sysLv2FsReadDir(s32 fd, sysFSDirent *entry, u64 *read) {
    if (fd < 0 || fd >= HOSTSTUB_MAX_DIRS || !hoststub_dirs[fd]) return -1;
    struct dirent *e = readdir(hoststub_dirs[fd]);
    if (!e) { *read = 0; return 0; }          // end of directory, not an error
    snprintf(entry->d_name, sizeof(entry->d_name), "%s", e->d_name);
    entry->d_namlen = (u8)strlen(entry->d_name);
    entry->d_type = 0;
    *read = 1;
    return 0;
}

static inline s32 sysLv2FsCloseDir(s32 fd) {
    if (fd < 0 || fd >= HOSTSTUB_MAX_DIRS || !hoststub_dirs[fd]) return -1;
    closedir(hoststub_dirs[fd]);
    hoststub_dirs[fd] = NULL;
    return 0;
}
