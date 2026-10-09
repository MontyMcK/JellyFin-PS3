// Offline downloads -- the console side: dl_platform.h over libnet, lv2 fs
// and an lv2 mutex, plus the worker thread (dl_service.h).
//
// Everything testable lives in dl_manager/dl_store/dl_http/dl_model and is
// covered on the host by tests/test_offline.cpp.  This file is the thin rest:
// each function is a direct call into the system, with no decisions of its
// own beyond mapping return codes.

#include "dl_platform.h"
#include "dl_service.h"
#include "dl_manager.h"
#include "dl_store.h"
#include "dl_request.h"
#include "stream_request.h"   // stream_prefs_current
#include "thumbnail_cache.h"   // thumb_set_local_source
#include "lfs.h"               // the store on a USB drive ("usb0:/...")
#include "i18n.h"              // TR

#include "../build_config.h"   // relative: source/ is not on the -I path
#include "http.h"              // http_open_socket
#include "jellyfin_api.h"      // g_token, jf_device_id
#include "jf_paths.h"
#include "plog.h"
#include "timing.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>   // S_IFDIR: lv2's st_mode uses the POSIX bits
#include <unistd.h>

#include <ppu-types.h>
#include <net/net.h>
#include <net/socket.h>
#include <sys/file.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <lv2/sysfs.h>

extern u32 running;   // main.cpp

// -------------------------------------------------------------------------
// Network
// -------------------------------------------------------------------------

// Receive timeout on the download socket.  Short on purpose: it is how often
// the transfer loop gets to look at pause/cancel/suspend.  The idle and
// header deadlines are counted in dl_manager.cpp on top of it.
#define DL_RECV_TIMEOUT_MS 1000

// 512 KB -- the size stream.cpp measured as a clear win over 128 KB on this
// console (31 vs 25 Mbps median).  A download wants the same thing a stream
// does: a wide TCP window.
#define DL_RCVBUF_BYTES (512 * 1024)

int dl_plat_connect(const char *host, int port) {
    int sock = http_open_socket(host, port, DL_RCVBUF_BYTES);
    if (sock < 0) return -1;
    struct timeval tv;
    tv.tv_sec  = DL_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (DL_RECV_TIMEOUT_MS % 1000) * 1000;
    netSetSockOpt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

int dl_plat_send(int h, const void *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = netSend(h, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return len;
}

int dl_plat_recv(int h, void *buf, int cap) {
    int n = netRecv(h, buf, cap, 0);
    if (n > 0) return n;
    if (n == 0) return DL_RECV_CLOSED;
    // SO_RCVTIMEO expiring reports EAGAIN/EWOULDBLOCK (ETIMEDOUT on some
    // firmware paths); anything else is the connection going away.
    int e = net_errno;
    if (e == NET_EAGAIN || e == NET_ETIMEDOUT || e == NET_EINTR)
        return DL_RECV_TIMEOUT;
    return DL_RECV_ERROR;
}

void dl_plat_close(int h) { if (h >= 0) netClose(h); }

// -------------------------------------------------------------------------
// Filesystem -- lv2 syscalls, so 64-bit sizes and no PRX dependency, except
// free space, which only libsysfs reports.  A store on a USB drive has lfs
// paths ("usb0:/jellyfin_offline/..."): every call below sends those to lfs,
// which writes NTFS (source/local/lfs.h).  Media handles from lfs are offset
// by LFS_FH so they never collide with an lv2 descriptor.
// -------------------------------------------------------------------------

static bool on_usb(const char *path) { return path && path[0] && path[0] != '/'; }
#define LFS_FH 0x40000000
static bool lfs_fh(int fh) { return fh >= LFS_FH; }

bool dl_plat_mkdir(const char *path) {
    if (on_usb(path)) return lfs_mkdir(path) == 0;
    if (sysLv2FsMkdir(path, 0755) == 0) return true;
    sysFSStat st;
    return sysLv2FsStat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

bool dl_plat_rmdir(const char *path) {
    if (on_usb(path)) return lfs_rmdir(path) == 0;
    return sysLv2FsRmdir(path) == 0;
}

bool dl_plat_remove(const char *path) {
    if (on_usb(path)) return lfs_remove(path) == 0;
    if (sysLv2FsUnlink(path) == 0) return true;
    sysFSStat st;
    return sysLv2FsStat(path, &st) != 0;
}

bool dl_plat_rename(const char *from, const char *to) {
    if (on_usb(from) || on_usb(to)) return on_usb(from) && on_usb(to) && lfs_rename(from, to) == 0;
    return sysLv2FsRename(from, to) == 0;
}

int64_t dl_plat_file_size(const char *path) {
    if (on_usb(path)) {
        lfs_entry e;
        return (lfs_stat(path, &e) && !e.is_dir) ? (int64_t)e.size : -1;
    }
    sysFSStat st;
    if (sysLv2FsStat(path, &st) != 0 || (st.st_mode & S_IFDIR)) return -1;
    return (int64_t)st.st_size;
}

bool dl_plat_exists(const char *path) {
    if (on_usb(path)) { lfs_entry e; return lfs_stat(path, &e); }
    sysFSStat st;
    return sysLv2FsStat(path, &st) == 0;
}

bool dl_plat_truncate(const char *path) {
    if (on_usb(path)) {
        // NTFS: delete and create, not truncate -- the writer only truncates files whose data
        // fits one record, and a part file being restarted can be any size
        if (lfs_remove(path) != 0) return false;
        const int h = lfs_open_append(path);
        if (h < 0) return false;
        lfs_close(h);
        return true;
    }
    s32 fd = -1;
    if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd,
                     0644, NULL, 0) != 0)
        return false;
    sysLv2FsClose(fd);
    return true;
}

// lv2 dirent types (CELL_FS_TYPE_*); PSL1GHT has no names for them.
#define LV2_DT_UNKNOWN 0
#define LV2_DT_DIR     1

struct DirsCtx { void (*cb)(const char *, void *); void *ctx; int n; };
static void dirs_only(const char *name, bool is_dir, void *c) {
    DirsCtx *d = (DirsCtx *)c;
    if (is_dir) { d->cb(name, d->ctx); d->n++; }
}

int dl_plat_list_dirs(const char *path, void (*cb)(const char *, void *),
                      void *ctx) {
    if (on_usb(path)) {
        DirsCtx d = { cb, ctx, 0 };
        return lfs_list_names(path, dirs_only, &d) < 0 ? -1 : d.n;
    }
    s32 fd = -1;
    if (sysLv2FsOpenDir(path, &fd) != 0) return -1;
    int n = 0;
    sysFSDirent ent;
    u64 got = 0;
    while (sysLv2FsReadDir(fd, &ent, &got) == 0 && got > 0) {
        if (strcmp(ent.d_name, ".") == 0 || strcmp(ent.d_name, "..") == 0)
            continue;
        if (ent.d_type != LV2_DT_DIR) {
            if (ent.d_type != LV2_DT_UNKNOWN) continue;
            // Type not reported: ask the filesystem.
            char full[DL_PATH_MAX];
            int fl = snprintf(full, sizeof(full), "%s/%s", path, ent.d_name);
            sysFSStat st;
            if (fl < 0 || fl >= (int)sizeof(full) ||
                sysLv2FsStat(full, &st) != 0 || !(st.st_mode & S_IFDIR))
                continue;
        }
        cb(ent.d_name, ctx);
        n++;
    }
    sysLv2FsCloseDir(fd);
    return n;
}

static bool s_fs_module = false;   // libsysfs loaded (for free space)

// The mount point holding a path ("/dev_hdd0/" for anything under it): the
// free-size call is documented for mount points, and every path under
// /dev_hdd0 is the same disk.
static void mount_point(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    if (strncmp(path, "/dev_", 5) == 0) {
        const char *slash = strchr(path + 1, '/');
        const size_t n = slash ? (size_t)(slash - path) : strlen(path);
        if (n + 2 <= cap) {
            memcpy(out, path, n);
            out[n] = '/';
            out[n + 1] = '\0';
        }
    }
}

uint64_t dl_plat_free_bytes(const char *path) {
    if (on_usb(path)) {
        uint64_t f = 0, t = 0;
        return lfs_space(path, &f, &t) ? f : DL_FREE_UNKNOWN;
    }
    if (!s_fs_module) return DL_FREE_UNKNOWN;
    char mp[32];
    mount_point(path, mp, sizeof(mp));
    u32 block = 0;
    u64 blocks = 0;
    const bool ok = (mp[0] && sysFsGetFreeSize(mp, &block, &blocks) == 0 && block != 0) ||
                    (sysFsGetFreeSize(path, &block, &blocks) == 0 && block != 0);
    if (!ok) return DL_FREE_UNKNOWN;
    return (uint64_t)block * blocks;
}

// jellyfin_dlreserve.txt: whole GB the downloads must leave free.  It can
// only raise the floor (dl_manager_init clamps it), so a stray value is safe.
int dl_plat_reserve_gb(void) {
    FILE *f = fopen(jf_data_path("jellyfin_dlreserve.txt"), "r");
    if (!f) return 0;
    int gb = 0;
    if (fscanf(f, "%d", &gb) != 1 || gb < 0) gb = 0;
    fclose(f);
    return gb;
}

int dl_plat_file_open_append(const char *path) {
    if (on_usb(path)) {
        const int h = lfs_open_append(path);
        return h < 0 ? -1 : LFS_FH + h;
    }
    s32 fd = -1;
    if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd,
                     0644, NULL, 0) != 0)
        return -1;
    return fd;
}

int dl_plat_file_write(int fh, const void *buf, int len) {
    if (lfs_fh(fh)) return lfs_write(fh - LFS_FH, buf, (uint32_t)len) == len ? len : -1;
    u64 written = 0;
    if (sysLv2FsWrite(fh, buf, (u64)len, &written) != 0 || written != (u64)len)
        return -1;
    return len;
}

// (the NTFS writer writes through: nothing waits in a cache to be synced)
bool dl_plat_file_sync(int fh) { return lfs_fh(fh) ? true : sysLv2FsFsync(fh) == 0; }
void dl_plat_file_close(int fh) {
    if (lfs_fh(fh)) lfs_close(fh - LFS_FH);
    else if (fh >= 0) sysLv2FsClose(fh);
}

int dl_plat_read_file(const char *path, void *buf, int cap) {
    if (on_usb(path)) {
        const int h = lfs_open(path);
        if (h < 0) return -1;
        const int n = lfs_read(h, 0, buf, (uint32_t)cap);
        lfs_close(h);
        return n < 0 ? -1 : n;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    const int n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

bool dl_plat_write_file(const char *path, const void *data, int len) {
    if (on_usb(path)) {
        if (!dl_plat_truncate(path)) return false;
        const int h = lfs_open_append(path);
        if (h < 0) return false;
        const bool ok = lfs_write(h, data, (uint32_t)len) == len;
        lfs_close(h);
        return ok;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

// -------------------------------------------------------------------------
// Time, lock, log
// -------------------------------------------------------------------------

static sys_mutex_t s_mtx;
static bool        s_mtx_ok = false;

uint64_t dl_plat_now_ms(void)          { return timing_get_us() / 1000ULL; }
void     dl_plat_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
void     dl_plat_lock(void)   { if (s_mtx_ok) sysMutexLock(s_mtx, 0); }
void     dl_plat_unlock(void) { if (s_mtx_ok) sysMutexUnlock(s_mtx); }
void     dl_plat_log(const char *line) { plog(line); }

// -------------------------------------------------------------------------
// Worker thread
// -------------------------------------------------------------------------

static sys_ppu_thread_t s_thread = 0;
static void (*s_thread_fn)(void) = NULL;

static void thread_entry(void *arg) {
    (void)arg;
    s_thread_fn();
    sysThreadExit(0);
}

bool dl_plat_thread_start(void (*fn)(void)) {
    s_thread_fn = fn;
    static char name[] = "dl_worker";   // sysThreadCreate takes char*
    // Priority 1500: below every playback thread (700-1100), so a download
    // can never take the PPU from the player.  Same as pkgi-ps3's worker.
    // JOINABLE: with flags 0 lv2 makes the thread detached and sysThreadJoin returns at once --
    // dl_svc_stop must really wait, or a store switch would start a second worker beside the first.
    s32 rc = sysThreadCreate(&s_thread, thread_entry, NULL, 1500, 65536, THREAD_JOINABLE, name);
    if (rc != 0) {
        char b[48];
        snprintf(b, sizeof(b), "dl: worker create failed 0x%08x", (u32)rc);
        plog(b);
        s_thread = 0;
        return false;
    }
    return true;
}

void dl_plat_thread_join(void) {
    if (!s_thread) return;
    u64 tret;
    sysThreadJoin(s_thread, &tret);
    s_thread = 0;
}

bool dl_plat_app_running(void) { return running != 0; }

// -------------------------------------------------------------------------
// Service glue (dl_service.h)
// -------------------------------------------------------------------------

// Thumbnail keys for a download's own artwork: "offp:<id>" is its poster and
// "offb:<id>" its backdrop.  The cache's fetch thread asks for the path; the
// file is only offered when it exists.
static bool thumb_resolve(const char *key, char *path, int cap) {
    const char *leaf = NULL;
    if (strncmp(key, "offp:", 5) == 0)      leaf = DL_FILE_POSTER;
    else if (strncmp(key, "offb:", 5) == 0) leaf = DL_FILE_BACKDROP;
    if (!leaf || !dl_id_valid(key + 5)) return false;
    if (!dl_store_item_file(path, cap, key + 5, leaf)) return false;
    return dl_plat_file_size(path) > 0;
}

// Where the store lives.  Tried in order on the worker; the first that can
// be created AND written (dl_store_init writes a marker to prove it) wins.
//
//  Hardware: a directory of its own at the HDD root first.  Not /dev_hdd0/tmp
//  -- that is scratch space (jf_paths.cpp) and a film is not scratch -- and
//  not the game USRDIR, which jf_paths.cpp found is not reliably writable
//  while the title runs.  /dev_hdd0/tmp is the fallback only because it is
//  the one place this app has PROVEN writable on hardware.
//  RPCS3: the USRDIR, where the emulator keeps everything else of ours.
static char s_fallback_root[DL_PATH_MAX] = "";
static const char *s_roots[2] = {
#if BUILD_FOR_RPCS3
    "/dev_hdd0/game/JFPS30000/USRDIR/jellyfin_offline",
#else
    "/dev_hdd0/jellyfin_offline",
#endif
    s_fallback_root,
};

// ---- where the store is ---------------------------------------------------
//
// jellyfin_dlloc.txt: "usb" puts the store on an NTFS USB drive, anything else (or no file) on the
// internal disk.  With "usb" the store follows the drive: the hook below runs on lfs's polling
// thread after every poll, and when the drive to use changes -- plugged in, pulled out, turned
// read-only by a failed write -- it stops the service and starts it again on the other store.
// While no drive takes writes the HDD store is shown (its downloads still play) and new downloads
// are refused with the reason (dl_location_note), rather than quietly going to the HDD.
//
// The drive: the NTFS one that already holds a store (jellyfin_offline/layout.txt), else the
// first writable NTFS drive.  The store is "usbN:/jellyfin_offline".

static const char *k_loc_file = "jellyfin_dlloc.txt";
static int  s_loc_usb = -1;                    // the setting; -1 = not read yet
static char s_usb_root[DL_PATH_MAX] = "";      // the store's root while it is on a drive
static const char *s_usb_roots[1] = { s_usb_root };
static char s_note[192] = "";                  // why new downloads cannot go to the drive ("" = they can)
static char s_drive_label[48] = "";            // the drive the store is on ("" = the HDD)
static volatile bool s_exiting = false;
static volatile bool s_recheck = true;
static uint32_t s_last_gen = 0;
static sys_mutex_t s_switch_mtx;               // a switch and dl_service_stop never overlap
static bool s_switch_ok = false;

bool dl_location_usb(void) {
    if (s_loc_usb < 0) {
        char b[8] = "";
        FILE *f = fopen(jf_data_path(k_loc_file), "r");
        if (f) { if (!fgets(b, sizeof(b), f)) b[0] = '\0'; fclose(f); }
        s_loc_usb = strncmp(b, "usb", 3) == 0;
    }
    return s_loc_usb == 1;
}

void dl_location_set_usb(bool usb) {
    s_loc_usb = usb ? 1 : 0;
    FILE *f = fopen(jf_data_path(k_loc_file), "w");
    if (f) { fputs(usb ? "usb\n" : "hdd\n", f); fclose(f); }
    s_recheck = true;                          // the hook acts on it within two seconds
}

// Copies under the manager's lock: written on the polling thread, read on the UI thread.
void dl_location_note(char *out, int cap) {
    dl_plat_lock(); snprintf(out, (size_t)cap, "%s", s_note); dl_plat_unlock();
}
void dl_location_drive(char *out, int cap) {
    dl_plat_lock(); snprintf(out, (size_t)cap, "%s", s_drive_label); dl_plat_unlock();
}

static void set_note(const char *note, const char *label) {
    dl_plat_lock();
    snprintf(s_note, sizeof(s_note), "%s", note);
    snprintf(s_drive_label, sizeof(s_drive_label), "%s", label);
    dl_plat_unlock();
}

// The store root to use now ("" = the internal disk), and the note/label to show with it.
static void choose(char *root, int cap, char *note, int ncap, char *label, int lcap) {
    root[0] = note[0] = label[0] = '\0';
    if (!dl_location_usb()) return;
    lfs_drive d[9];
    const int n = lfs_drives(d, 9);
    int pick = -1;
    for (int pass = 0; pass < 2 && pick < 0; pass++) {
        for (int i = 0; i < n; i++) {
            if (d[i].kind != LFS_USB_NTFS) continue;
            char p[64];
            snprintf(p, sizeof(p), "%s:/jellyfin_offline/layout.txt", d[i].id);
            lfs_entry e;
            if (pass == 0 ? lfs_stat(p, &e) : true) { pick = i; break; }   // a drive with a store first
        }
    }
    if (pick < 0) {
        snprintf(note, (size_t)ncap, "%s", TR("Download location is USB: plug in an NTFS drive"));
        return;
    }
    char top[24];
    snprintf(top, sizeof(top), "%s:/", d[pick].id);
    if (!lfs_drive_writable(top)) {
        snprintf(note, (size_t)ncap, "%s: %s", d[pick].label, lfs_drive_why_readonly(top));
        return;
    }
    snprintf(root, (size_t)cap, "%s:/jellyfin_offline", d[pick].id);
    snprintf(label, (size_t)lcap, "%s", d[pick].label);
}

static void location_hook(void) {
    if (s_exiting || !s_switch_ok) return;
    const uint32_t gen = lfs_generation();
    if (gen == s_last_gen && !s_recheck) return;
    s_last_gen = gen;
    s_recheck = false;
    char want[DL_PATH_MAX], note[192], label[48];
    choose(want, sizeof(want), note, sizeof(note), label, sizeof(label));
    sysMutexLock(s_switch_mtx, 0);
    if (!s_exiting && strcmp(want, s_usb_root) != 0) {
        char b[DL_PATH_MAX + 48];
        snprintf(b, sizeof(b), "dl: store moves to %s", want[0] ? want : "the internal disk");
        plog(b);
        dl_svc_stop();                          // the active item goes back to the queue, data kept
        snprintf(s_usb_root, sizeof(s_usb_root), "%s", want);
        if (want[0]) dl_svc_start(s_usb_roots, 1);
        else         dl_svc_start(s_roots, 2);
    }
    set_note(note, s_usb_root[0] ? label : "");
    sysMutexUnlock(s_switch_mtx);
}

bool dl_service_start(void) {
    if (dl_svc_started()) return true;
    if (!s_mtx_ok) {
        // Start from the initializer: lv2 rejects zeroed pshared/adaptive fields.
        sys_mutex_attr_t attr;
        sysMutexAttrInitialize(attr);
        attr.attr_protocol  = SYS_MUTEX_PROTOCOL_FIFO;
        attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
        s_mtx_ok = (sysMutexCreate(&s_mtx, &attr) == 0);
        if (!s_mtx_ok) { plog("dl: mutex create failed"); return false; }
        s_switch_ok = (sysMutexCreate(&s_switch_mtx, &attr) == 0);
    }
    if (!s_fs_module) s_fs_module = (sysModuleLoad(SYSMODULE_FS) == 0);
    thumb_set_local_source(thumb_resolve);
    snprintf(s_fallback_root, sizeof(s_fallback_root), "%s",
             jf_data_path("jellyfin_offline"));
    // Whatever session exists already (a saved login) -- or none, which
    // keeps the queue held until login.
    dl_service_refresh_auth();
    // The internal disk first; with the location on USB the hook moves the store to the drive once
    // lfs has mounted it (a few seconds after launch).
    const bool ok = dl_svc_start(s_roots, 2);
    lfs_set_poll_hook(location_hook);
    return ok;
}

void dl_service_stop(void) {
    s_exiting = true;                           // the hook does nothing from here on
    lfs_set_poll_hook(NULL);
    if (s_switch_ok) sysMutexLock(s_switch_mtx, 0);
    dl_svc_stop();
    if (s_switch_ok) sysMutexUnlock(s_switch_mtx);
}

void dl_service_refresh_auth(void) { dl_svc_set_session(g_token, jf_device_id()); }

const char *dl_service_root(void) { return dl_svc_root(); }

bool dl_service_wait_restored(unsigned timeout_ms) {
    if (!dl_svc_started()) return false;
    for (unsigned waited = 0; !dl_svc_restored() && waited < timeout_ms; waited += 10)
        usleep(10000);
    return dl_svc_restored() && dl_manager_ready();
}

int dl_download_item(const JFItem *item, const XMBItemDetail *detail,
                     const JFMediaSource *source) {
    StreamPrefs prefs;
    stream_prefs_current(&prefs);
    DlRequestInput in = { g_server, jf_device_id(), item, detail, source, &prefs };
    // ~5 KB, on the heap: the item page and a season being queued on its own
    // thread both come through here.
    DlRequest *rq = (DlRequest *)malloc(sizeof(DlRequest));
    if (!rq) return DL_E_IO;
    DlResult r;
    char note[8];
    dl_location_note(note, sizeof(note));
    if (dl_location_usb() && note[0]) {
        r = DL_E_NO_DRIVE;                      // the reason: dl_location_note
    } else if (!dl_request_build(&in, rq)) {
        r = DL_E_INVALID;
    } else if (!rq->size_known) {
        // Space has to be checkable: with neither a size nor a runtime there
        // is nothing to check against, and nothing is queued.
        r = DL_E_SIZE_UNKNOWN;
    } else {
        r = dl_enqueue(&rq->meta, rq->url, 0, &rq->extras);
    }
    free(rq);
    char b[96];
    snprintf(b, sizeof(b), "dl: request %.8s -> %d", item ? item->id : "?", (int)r);
    plog(b);
    return r;
}
