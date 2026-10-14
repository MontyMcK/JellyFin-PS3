// Host implementation of LfsPort for tests/test_lfs.cpp.
//
//  * "/dev_hdd0/..." and "/dev_usb00N/..." are directories on the host (g_hdd_root, g_fat_root[N]).
//  * A USB port is "present" when the test says so; whether it holds NTFS or exFAT is whatever image
//    the storage stub (mohasi_host_storage.c) has attached to that port.
//  * The clock is the test's.

#include "lfs.h"
#include "lfs_port.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

std::string g_hdd_root;                 // stands for /dev_hdd0
std::string g_fat_root[8];              // stands for /dev_usb00N when the port is a FAT stick
bool        g_present[8]   = { false };
bool        g_fat_up[8]    = { false };
uint64_t    g_now_us       = 1000000;
std::vector<std::string> g_log;

static pthread_mutex_t s_mu[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

static bool map(const char *native, std::string *out) {
    if (strncmp(native, "/dev_hdd0", 9) == 0) { *out = g_hdd_root + (native + 9); return true; }
    if (strncmp(native, "/dev_usb00", 10) == 0 && native[10] >= '0' && native[10] <= '7') {
        const int p = native[10] - '0';
        if (g_fat_root[p].empty() || !g_fat_up[p]) return false;
        *out = g_fat_root[p] + (native + 11);
        return true;
    }
    return false;
}

static bool h_usb_present(int port) { return g_present[port]; }
static bool h_fat_mounted(int port) { return g_fat_up[port] && g_present[port]; }

static int h_fs_list(const char *native, void (*cb)(const char *, bool, void *), void *ctx) {
    std::string path;
    if (!map(native, &path)) return -1;
    DIR *d = opendir(path.c_str());
    if (!d) return -1;
    while (dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        const std::string full = path + "/" + e->d_name;
        const bool is_dir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        cb(e->d_name, is_dir, ctx);
    }
    closedir(d);
    return 0;
}

static int h_fs_stat(const char *native, bool *is_dir, uint64_t *size, uint64_t *mtime) {
    std::string path;
    struct stat st;
    if (!map(native, &path) || stat(path.c_str(), &st) != 0) return -1;
    *is_dir = S_ISDIR(st.st_mode);
    *size   = *is_dir ? 0 : (uint64_t)st.st_size;
    *mtime  = (uint64_t)st.st_mtime;
    return 0;
}

static int h_fs_open(const char *native) {
    std::string path;
    if (!map(native, &path)) return -1;
    return open(path.c_str(), O_RDONLY);
}

static int h_fs_read(int fd, uint64_t off, void *buf, uint32_t n) {
    const ssize_t r = pread(fd, buf, n, (off_t)off);
    return r < 0 ? -1 : (int)r;
}

static void h_fs_close(int fd) { close(fd); }

static bool h_fs_space(const char *native, uint64_t *free_bytes, uint64_t *total_bytes) {
    std::string path;
    if (!map(native, &path)) return false;
    *free_bytes = 7ull << 30; *total_bytes = 0;
    return true;
}

static int h_fs_mkdir(const char *native) {
    std::string path;
    if (!map(native, &path)) return -1;
    return (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static uint64_t h_now(void) { return g_now_us; }
static void h_lock(int w)   { pthread_mutex_lock(&s_mu[w]); }
static void h_unlock(int w) { pthread_mutex_unlock(&s_mu[w]); }
static void h_log(const char *l) { g_log.push_back(l); }

extern const LfsPort HOST_PORT;
const LfsPort HOST_PORT = {
    h_usb_present, h_fat_mounted,
    h_fs_list, h_fs_stat, h_fs_open, h_fs_read, h_fs_close, h_fs_space, h_fs_mkdir,
    h_now, h_lock, h_unlock, h_log,
};

extern "C" void lfs_start(void) {}
extern "C" void lfs_stop(void) {}
