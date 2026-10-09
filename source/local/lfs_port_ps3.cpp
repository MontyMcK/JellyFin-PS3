// The console side of lfs: lv2 file calls for the internal disk and for FAT32 sticks, USB presence
// from the raw storage layer, two mutexes, and the thread that polls for plug and unplug.

#include "lfs.h"
#include "lfs_port.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/file.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <sys/stat.h>

extern "C" {
#include "jf_port.h"            // getStorageInfo / getUsbDeviceId (third_party/mohasi_fs)
}
#include "dl_platform.h"        // dl_plat_free_bytes: the free-space call the downloads already use
#include "timing.h"
#include "plog.h"

#define LV2_DT_UNKNOWN 0
#define LV2_DT_DIR     1

static sys_mutex_t s_mtx[2];
static bool        s_mtx_ok = false;

static void port_lock(int w)   { if (s_mtx_ok) sysMutexLock(s_mtx[w], 0); }
static void port_unlock(int w) { if (s_mtx_ok) sysMutexUnlock(s_mtx[w]); }

static bool port_usb_present(int port) {
    StorageDeviceInfo info;
    return getStorageInfo(getUsbDeviceId(port), &info) == 0;
}

static bool port_fat_mounted(int port) {
    char p[32];
    snprintf(p, sizeof p, "/dev_usb%03d", port);
    sysFSStat st;
    return sysLv2FsStat(p, &st) == 0;
}

static int port_fs_list(const char *native, void (*cb)(const char *, bool, void *), void *ctx) {
    s32 fd = -1;
    if (sysLv2FsOpenDir(native, &fd) != 0) return -1;
    sysFSDirent ent;
    u64 got = 0;
    while (sysLv2FsReadDir(fd, &ent, &got) == 0 && got > 0) {
        if (strcmp(ent.d_name, ".") == 0 || strcmp(ent.d_name, "..") == 0) continue;
        bool is_dir = ent.d_type == LV2_DT_DIR;
        if (ent.d_type == LV2_DT_UNKNOWN) {           // some FAT drivers do not say: ask
            char full[1100];
            snprintf(full, sizeof full, "%s/%s", native, ent.d_name);
            sysFSStat st;
            is_dir = sysLv2FsStat(full, &st) == 0 && (st.st_mode & S_IFDIR);
        }
        cb(ent.d_name, is_dir, ctx);
    }
    sysLv2FsCloseDir(fd);
    return 0;
}

static int port_fs_stat(const char *native, bool *is_dir, uint64_t *size, uint64_t *mtime) {
    sysFSStat st;
    if (sysLv2FsStat(native, &st) != 0) return -1;
    *is_dir = (st.st_mode & S_IFDIR) != 0;
    *size   = *is_dir ? 0 : (uint64_t)st.st_size;
    *mtime  = (uint64_t)st.st_mtime;
    return 0;
}

static int port_fs_open(const char *native) {
    s32 fd = -1;
    if (sysLv2FsOpen(native, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0 || fd < 0) return -1;
    return (int)fd;
}

static int port_fs_read(int fd, uint64_t off, void *buf, uint32_t n) {
    u64 pos = 0, got = 0;
    if (sysLv2FsLSeek64(fd, off, 0 /* SEEK_SET */, &pos) != 0) return -1;
    if (sysLv2FsRead(fd, buf, n, &got) != 0) return -1;
    return (int)got;
}

static void port_fs_close(int fd) { sysLv2FsClose(fd); }

static bool port_fs_space(const char *native, uint64_t *free_bytes, uint64_t *total_bytes) {
    const uint64_t f = dl_plat_free_bytes(native);
    *total_bytes = 0;                                   // lv2 reports free blocks only
    if (f == DL_FREE_UNKNOWN) return false;
    *free_bytes = f;
    return true;
}

static int port_fs_mkdir(const char *native) {
    if (sysLv2FsMkdir(native, 0755) == 0) return 0;
    sysFSStat st;
    return (sysLv2FsStat(native, &st) == 0 && (st.st_mode & S_IFDIR)) ? 0 : -1;
}

static uint64_t port_now_us(void) { return timing_get_us(); }
static void     port_log(const char *line) { plog(line); }

static const LfsPort PS3_PORT = {
    port_usb_present, port_fat_mounted,
    port_fs_list, port_fs_stat, port_fs_open, port_fs_read, port_fs_close, port_fs_space, port_fs_mkdir,
    port_now_us, port_lock, port_unlock, port_log,
};

// ---- the hotplug thread -----------------------------------------------------------

static volatile bool s_run = false;
static void (*volatile s_hook)(void) = NULL;

void lfs_set_poll_hook(void (*hook)(void)) { s_hook = hook; }
static sys_ppu_thread_t s_tid;
static bool s_have_tid = false;

static void poll_main(void *arg) {
    (void)arg;
    while (s_run) {
        lfs_poll_once();
        void (*hook)(void) = s_hook;
        if (hook && s_run) hook();
        for (int i = 0; i < 20 && s_run; i++) usleep(100000);     // two seconds, but quick to stop
    }
    sysThreadExit(0);
}

void lfs_start(void) {
    if (s_have_tid) return;
    // Start from the initializer: lv2 rejects zeroed pshared/adaptive fields.
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    attr.attr_protocol  = SYS_MUTEX_PROTOCOL_FIFO;
    attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    s_mtx_ok = sysMutexCreate(&s_mtx[0], &attr) == 0 && sysMutexCreate(&s_mtx[1], &attr) == 0;
    lfs_set_port(&PS3_PORT);
    s_run = true;
    static char name[] = "jf_lfs";
    if (sysThreadCreate(&s_tid, poll_main, NULL, 1400, 64 * 1024, THREAD_JOINABLE, name) != 0) {
        s_run = false;
        plog("lfs: the drive-polling thread did not start");
        return;
    }
    s_have_tid = true;
    plog("lfs: started");
}

void lfs_stop(void) {
    if (!s_have_tid) return;
    s_run = false;
    u64 ret;
    sysThreadJoin(s_tid, &ret);
    s_have_tid = false;
    lfs_unmount_all();      // anything still open for writing closed, NTFS dirty flags down
}
