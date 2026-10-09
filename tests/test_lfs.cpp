// Host test for source/local/lfs.cpp: drives, hotplug, paths, listings and reads, over directory
// trees (the internal disk, a FAT32 stick) and the NTFS / exFAT images of fixtures/fs/ (read
// through the vendored mohasi readers on the fake storage layer).
//
//   make -f Makefile.host test_lfs && ./test_lfs

#define _FILE_OFFSET_BITS 64
#include "lfs.h"
#include "lfs_port.h"
#include "lfs_path.h"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern std::string g_hdd_root, g_fat_root[8];
extern bool g_present[8], g_fat_up[8];
extern uint64_t g_now_us;
extern std::vector<std::string> g_log;
extern const LfsPort HOST_PORT;
extern "C" {
extern int g_port_fd[8], g_port_fail[8];
extern unsigned long g_port_reads[8];
}

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define MB (1024 * 1024)
#define SEC 1000000ULL

// tree.py's generator (see fixtures/fs/tree.py)
static void gen(int seed, uint64_t start, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const uint64_t idx = start + i, block = idx >> 6;
        const unsigned off = (unsigned)(idx & 63);
        out[i] = off == 0 ? (uint8_t)(block & 0xFF) : off == 1 ? (uint8_t)((block >> 8) & 0xFF)
                                                              : (uint8_t)((block * 7 + (unsigned)seed) & 0xFF);
    }
}

static void write_file(const std::string &path, const std::string &data) {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        std::string d = path.substr(0, slash), cur;
        for (size_t i = 1; i <= d.size(); i++)
            if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0755);
    }
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static std::string gen_str(int seed, size_t n) {
    std::string s(n, '\0');
    gen(seed, 0, (uint8_t *)&s[0], n);
    return s;
}

static void advance(uint64_t us) { g_now_us += us; }
static void poll() { lfs_poll_once(); }
static void settle_and_poll() { poll(); advance(3 * SEC); poll(); }   // seen, then past the settle time

static bool has_drive(const char *id, lfs_drive *out = nullptr) {
    lfs_drive d[16];
    const int n = lfs_drives(d, 16);
    for (int i = 0; i < n; i++) if (!strcmp(d[i].id, id)) { if (out) *out = d[i]; return true; }
    return false;
}

static std::vector<lfs_entry> list_all(const char *path, int *total = nullptr) {
    std::vector<lfs_entry> v(4200);
    const int n = lfs_list(path, v.data(), (int)v.size(), 0);
    if (total) *total = n;
    v.resize(n > 0 ? n : 0);
    return v;
}

static bool names_are(const std::vector<lfs_entry> &v, const std::vector<std::string> &want) {
    if (v.size() != want.size()) return false;
    for (size_t i = 0; i < v.size(); i++) if (want[i] != v[i].name) return false;
    return true;
}

static void attach(int port, const char *image) {
    char path[128], cmd[300];
    snprintf(path, sizeof path, "/tmp/jf_lfs_%d.img", port);
    snprintf(cmd, sizeof cmd, "gzip -dc fixtures/fs/%s.img.gz > %s", image, path);
    if (system(cmd) != 0) { printf("FAIL cannot unpack %s\n", image); s_failed++; return; }
    if (g_port_fd[port] >= 0) close(g_port_fd[port]);
    g_port_fd[port] = open(path, O_RDONLY);
    g_port_fail[port] = 0;
    g_present[port] = true;
}

static void detach(int port) {
    g_present[port] = false;
    if (g_port_fd[port] >= 0) close(g_port_fd[port]);
    g_port_fd[port] = -1;
    g_port_fail[port] = 0;
}

static std::vector<uint8_t> read_whole(int h, uint64_t size, uint32_t chunk) {
    std::vector<uint8_t> out(size);
    uint64_t pos = 0;
    while (pos < size) {
        const uint32_t want = (size - pos) < chunk ? (uint32_t)(size - pos) : chunk;
        const int r = lfs_read(h, pos, out.data() + pos, want);
        if (r <= 0) { out.resize(pos); break; }
        pos += (uint64_t)r;
    }
    return out;
}

// ---- the tests --------------------------------------------------------------------

static void internal_disk() {
    printf("- internal disk\n");
    lfs_drive d;
    CHECK(has_drive("hdd", &d) && d.kind == LFS_HDD && !strcmp(d.label, "Internal disk"));
    CHECK(d.free == (7ull << 30));

    // hdd:/ offers three folders, not the whole disk
    int total = 0;
    auto root = list_all("hdd:/", &total);
    CHECK(total == 3 && names_are(root, { "jellyfin_local", "music", "video" }));
    CHECK(root.size() == 3 && root[0].is_dir && root[1].is_dir && root[2].is_dir);
    struct stat st;
    CHECK(stat((g_hdd_root + "/jellyfin_local").c_str(), &st) == 0);          // created when missing

    // video/: folders first, natural order, media only, nothing hidden
    auto v = list_all("hdd:/video");
    CHECK(names_are(v, { "Season", "ep2.mkv", "ep10.mkv", "movie.m2ts" }));
    CHECK(v.size() == 4 && v[0].is_dir && !v[1].is_dir && v[1].size == 3000);
    CHECK(v.size() == 4 && v[3].size == 1111);

    // a stat and a read
    lfs_entry e;
    CHECK(lfs_stat("hdd:/video/ep2.mkv", &e) && !e.is_dir && e.size == 3000 && !strcmp(e.name, "ep2.mkv"));
    CHECK(lfs_stat("hdd:/video/Season", &e) && e.is_dir);
    CHECK(lfs_stat("hdd:/", &e) && e.is_dir);
    CHECK(!lfs_stat("hdd:/video/missing.mkv", &e));
    int h = lfs_open("hdd:/video/ep2.mkv");
    CHECK(h >= 0 && lfs_size(h) == 3000);
    auto data = read_whole(h, 3000, 700);
    CHECK(data.size() == 3000 && memcmp(data.data(), gen_str(21, 3000).data(), 3000) == 0);
    uint8_t buf[100];
    CHECK(lfs_read(h, 2950, buf, 100) == 50);                         // short at the end
    CHECK(lfs_read(h, 3000, buf, 100) == 0);                          // nothing past it
    CHECK(lfs_read(h, 99999, buf, 100) == 0);
    CHECK(lfs_read(h, 0, buf, 0) == 0);
    lfs_close(h);
    CHECK(lfs_read(h, 0, buf, 10) < 0);                               // a closed handle fails

    // internal absolute paths (where downloads live) open too
    h = lfs_open("/dev_hdd0/video/ep10.mkv");
    CHECK(h >= 0 && lfs_size(h) == 3000);
    lfs_close(h);

    // refusals
    CHECK(lfs_open("hdd:/other/x.mkv") == LFS_E_NOTFOUND);            // only the three folders
    CHECK(lfs_open("hdd:/video/../music/x.mp3") == LFS_E_BADPATH);
    CHECK(lfs_open("/dev_hdd0/../etc/passwd") == LFS_E_BADPATH);
    CHECK(lfs_open("/dev_hdd0/video/Season") == LFS_E_BADPATH);       // a folder is not a file
    CHECK(lfs_open("hdd:/") == LFS_E_BADPATH);
    CHECK(lfs_open("hdd:/video/missing.mkv") == LFS_E_NOTFOUND);
    CHECK(lfs_open("usb0:/a.mkv") == LFS_E_NOTFOUND);                 // no drive there yet
    CHECK(lfs_open(nullptr) == LFS_E_BADPATH);
    CHECK(lfs_list("nonsense", nullptr, 0, 0) == LFS_E_BADPATH);
    CHECK(lfs_list("hdd:/other", nullptr, 0, 0) == LFS_E_NOTFOUND);
    CHECK(lfs_list("hdd:/video/ep2.mkv", nullptr, 0, 0) == LFS_E_BADPATH);   // a file is not a folder
    CHECK(lfs_list("hdd:/video/nope", nullptr, 0, 0) == LFS_E_NOTFOUND);
}

static void too_many_handles() {
    printf("- handles\n");
    std::vector<int> hs;
    for (int i = 0; i < 8; i++) { int h = lfs_open("hdd:/video/ep2.mkv"); CHECK(h >= 0); hs.push_back(h); }
    CHECK(lfs_open("hdd:/video/ep2.mkv") == LFS_E_TOOMANY);
    lfs_close(hs[3]);
    int again = lfs_open("hdd:/video/ep2.mkv");
    CHECK(again == hs[3]);                                            // the freed slot is reused
    hs[3] = again;
    for (int h : hs) lfs_close(h);
    lfs_close(-1); lfs_close(99); lfs_close(hs[0]);                   // harmless
}

static void paging() {
    printf("- paging a big folder\n");
    for (int i = 1; i <= 300; i++) {
        char n[64];
        snprintf(n, sizeof n, "%s/video/big/clip %d.ts", g_hdd_root.c_str(), i);
        write_file(n, "x");
    }
    write_file(g_hdd_root + "/video/big/notes.txt", "n");
    int total = 0;
    std::vector<lfs_entry> page(50);
    CHECK((total = lfs_list("hdd:/video/big", page.data(), 50, 0)) == 300);
    CHECK(!strcmp(page[0].name, "clip 1.ts") && !strcmp(page[1].name, "clip 2.ts") && !strcmp(page[9].name, "clip 10.ts"));
    CHECK(lfs_list("hdd:/video/big", page.data(), 50, 275) == 300);
    CHECK(!strcmp(page[0].name, "clip 276.ts") && !strcmp(page[24].name, "clip 300.ts"));
    CHECK(lfs_list("hdd:/video/big", page.data(), 50, 300) == 300);   // past the end: nothing copied, count stays
    CHECK(lfs_list("hdd:/video/big", page.data(), 0, 0) == 300);

    // the listing is kept for a while, then read again
    write_file(g_hdd_root + "/video/big/clip 301.ts", "x");
    CHECK(lfs_list("hdd:/video/big", page.data(), 1, 0) == 300);      // cached
    advance(11 * SEC);
    CHECK(lfs_list("hdd:/video/big", page.data(), 1, 0) == 301);      // read again
}

static void fat_stick() {
    printf("- FAT32 stick (lv2)\n");
    char tmpl[] = "/tmp/jf_lfs_fat_XXXXXX";
    g_fat_root[2] = mkdtemp(tmpl);
    write_file(g_fat_root[2] + "/Films/One.mkv", gen_str(31, 5000));
    write_file(g_fat_root[2] + "/Films/readme.txt", "r");
    write_file(g_fat_root[2] + "/Music/a.flac", gen_str(32, 400));
    write_file(g_fat_root[2] + "/.Trashes/x.mkv", "x");
    write_file(g_fat_root[2] + "/System Volume Information/y.mkv", "y");

    g_present[2] = true; g_fat_up[2] = true;
    const uint32_t gen0 = lfs_generation();
    poll();
    CHECK(!has_drive("usb2"));                                        // not before the settle time
    CHECK(lfs_generation() == gen0);
    advance(3 * SEC);
    poll();
    lfs_drive d;
    CHECK(has_drive("usb2", &d) && d.kind == LFS_USB_FAT && !strcmp(d.label, "USB drive 3"));
    CHECK(d.free == (7ull << 30));
    CHECK(lfs_generation() != gen0);

    CHECK(names_are(list_all("usb2:/"), { "Films", "Music" }));       // hidden names and trash left out
    CHECK(names_are(list_all("usb2:/Films"), { "One.mkv" }));
    auto f = list_all("usb2:/Films");
    CHECK(f.size() == 1 && f[0].size == 5000);
    int h = lfs_open("usb2:/Films/One.mkv");
    CHECK(h >= 0 && lfs_size(h) == 5000);
    auto data = read_whole(h, 5000, 4096);
    CHECK(data.size() == 5000 && memcmp(data.data(), gen_str(31, 5000).data(), 5000) == 0);

    // the stick is pulled with a file open: reads say so, nothing hangs
    g_present[2] = false;
    uint8_t buf[16];
    poll();
    CHECK(!has_drive("usb2"));
    CHECK(lfs_read(h, 0, buf, 16) == LFS_E_REMOVED);
    CHECK(lfs_open("usb2:/Films/One.mkv") == LFS_E_NOTFOUND);
    CHECK(lfs_list("usb2:/", nullptr, 0, 0) == LFS_E_NOTFOUND);

    // plugged in again: a new drive, and the old handle stays dead (it is a different volume now)
    g_present[2] = true;
    settle_and_poll();
    CHECK(has_drive("usb2"));
    CHECK(lfs_read(h, 0, buf, 16) == LFS_E_REMOVED);
    int fresh = lfs_open("usb2:/Films/One.mkv");
    CHECK(fresh >= 0 && fresh != h);
    CHECK(lfs_read(fresh, 0, buf, 16) == 16);
    CHECK(lfs_read(h, 0, buf, 16) == LFS_E_REMOVED);
    lfs_close(h);
    lfs_close(fresh);
    g_present[2] = false; g_fat_up[2] = false;
    poll();
    CHECK(!has_drive("usb2"));
}

// Contents of the NTFS / exFAT images (fixtures/fs/tree.py)
static void check_image_drive(const char *id, lfs_kind kind, const char *label) {
    char path[64];
    lfs_drive d;
    CHECK(has_drive(id, &d) && d.kind == kind && !strcmp(d.label, label));
    CHECK(d.total > 40 * MB && d.free > 0 && d.free < d.total);

    snprintf(path, sizeof path, "%s:/", id);
    const bool ntfs = kind == LFS_USB_NTFS;
    // media files and folders only: hello.txt, many/'s text files and the spacers' .bin files do not show
    std::vector<std::string> root = { "Movies", "Music", "Unicode", "many", "spacers" };
    if (ntfs) root.insert(root.begin() + 3, "big");
    // folders with nothing playable still list (the browser decides what to do with an empty one)
    std::vector<std::string> want = root;
    std::sort(want.begin(), want.end(), [](const std::string &a, const std::string &b) { return lfs_natural_cmp(a.c_str(), b.c_str()) < 0; });
    CHECK(names_are(list_all(path), want));

    snprintf(path, sizeof path, "%s:/Movies", id);
    auto mv = list_all(path);
    CHECK(names_are(mv, { "sub", "Film One (2019).mkv" }));
    CHECK(mv.size() == 2 && mv[0].is_dir && mv[1].size == 3 * MB);

    snprintf(path, sizeof path, "%s:/Unicode", id);
    auto un = list_all(path);
    CHECK(un.size() == 2);                                            // the folder and the emoji .mkv; the .txt files are not media
    bool jp = false, emoji = false;
    for (auto &e : un) {
        if (e.is_dir && !strcmp(e.name, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E")) jp = true;
        if (!e.is_dir && !strcmp(e.name, "emoji \xF0\x9F\x8E\xAC clip.mkv") && e.size == 70000) emoji = true;
    }
    CHECK(jp && emoji);

    snprintf(path, sizeof path, "%s:/many", id);
    CHECK(lfs_list(path, nullptr, 0, 0) == 0);                        // 400 text files: none is media

    snprintf(path, sizeof path, "%s:/Movies/Film One (2019).mkv", id);
    lfs_entry e;
    CHECK(lfs_stat(path, &e) && e.size == 3 * MB && !e.is_dir);
    int h = lfs_open(path);
    CHECK(h >= 0 && lfs_size(h) == 3 * MB);
    const std::string want_bytes = gen_str(11, 3 * MB);
    // one call for the whole file (several lock holds inside), then odd chunks, then random positions
    {
        std::vector<uint8_t> all(3 * MB);
        CHECK(lfs_read(h, 0, all.data(), 3 * MB) == 3 * MB && memcmp(all.data(), want_bytes.data(), 3 * MB) == 0);
    }
    auto odd = read_whole(h, 3 * MB, 100003);
    CHECK(odd.size() == 3 * MB && memcmp(odd.data(), want_bytes.data(), 3 * MB) == 0);
    static const uint64_t offs[] = { 1, 511, 513, 4095, 4097, 262143, 262144, 262145, 1000003, 3 * MB - 100 };
    for (uint64_t o : offs) {
        uint8_t buf[300];
        const int want_n = (int)std::min<uint64_t>(300, 3 * MB - o);
        CHECK(lfs_read(h, o, buf, 300) == want_n && memcmp(buf, want_bytes.data() + o, want_n) == 0);
    }
    uint8_t one[4];
    CHECK(lfs_read(h, 3 * MB, one, 4) == 0);
    lfs_close(h);

    if (ntfs) {
        snprintf(path, sizeof path, "%s:/big", id);
        auto b = list_all(path);
        CHECK(b.size() == 0);                                         // sparse5g.bin is not a media file
    }
}

static void image_drives() {
    printf("- NTFS in an MBR partition on usb0, exFAT in GPT on usb1\n");
    attach(0, "ntfs_mbr");
    attach(1, "exfat_gpt");
    g_log.clear();
    settle_and_poll();
    check_image_drive("usb0", LFS_USB_NTFS, "JFNTFS");
    check_image_drive("usb1", LFS_USB_EXFAT, "JFEXFAT");
    bool logged = false;
    for (auto &l : g_log) if (l.find("usb0 NTFS \"JFNTFS\" mounted") != std::string::npos) logged = true;
    CHECK(logged);
    bool bench = false;
    for (auto &l : g_log) if (l.find("read 64 MB") != std::string::npos) bench = true;
    CHECK(!bench);                                                    // 6 MB read so far: not yet

    // drives are listed internal first, then in port order
    lfs_drive d[16];
    const int n = lfs_drives(d, 16);
    CHECK(n == 3 && !strcmp(d[0].id, "hdd") && !strcmp(d[1].id, "usb0") && !strcmp(d[2].id, "usb1"));
    CHECK(lfs_drives(d, 2) == 2);                                     // never past max
    CHECK(lfs_drives(d, 0) == 0);

    // a bare filesystem (no partition table) and an exFAT one in MBR work the same
    attach(3, "ntfs_flat");
    attach(4, "exfat_mbr");
    settle_and_poll();
    check_image_drive("usb3", LFS_USB_NTFS, "JFNTFS");
    check_image_drive("usb4", LFS_USB_EXFAT, "JFEXFAT");
    detach(3); detach(4);
    poll();
    CHECK(!has_drive("usb3") && !has_drive("usb4"));
}

static void unplug_mid_read() {
    printf("- unplug mid-read\n");
    int h = lfs_open("usb0:/Movies/Film One (2019).mkv");
    CHECK(h >= 0);
    std::vector<uint8_t> buf(100000);
    CHECK(lfs_read(h, 0, buf.data(), 4096) == 4096);

    // the device stops answering and is gone from the bus, but the poll has not run yet
    g_port_fail[0] = 1; g_present[0] = false;
    CHECK(lfs_read(h, 2 * MB, buf.data(), 4096) == LFS_E_REMOVED);
    // the device stops answering but is still on the bus: an I/O error, not a removal
    g_present[0] = true;
    CHECK(lfs_read(h, 2 * MB, buf.data(), 4096) == LFS_E_IO);
    g_port_fail[0] = 0;
    CHECK(lfs_read(h, 2 * MB, buf.data(), 4096) == 4096);             // and it recovers
    uint8_t want[4096];
    gen(11, 2 * MB, want, 4096);
    CHECK(memcmp(buf.data(), want, 4096) == 0);

    // listings fail the same way
    g_port_fail[0] = 1;
    advance(11 * SEC);                                                // past the cached listing
    CHECK(lfs_list("usb0:/many", nullptr, 0, 0) == LFS_E_IO);
    g_present[0] = false;
    CHECK(lfs_list("usb0:/many", nullptr, 0, 0) == LFS_E_REMOVED);

    // now the poll sees it gone
    const uint32_t gen0 = lfs_generation();
    poll();
    CHECK(!has_drive("usb0") && lfs_generation() != gen0);
    CHECK(lfs_read(h, 0, buf.data(), 16) == LFS_E_REMOVED);
    CHECK(lfs_open("usb0:/Movies/Film One (2019).mkv") == LFS_E_NOTFOUND);
    lfs_close(h);

    // replugged: the old handle does not wake up
    g_port_fail[0] = 0;
    g_port_fd[0] = open("/tmp/jf_lfs_0.img", O_RDONLY);
    g_present[0] = true;
    int h2 = -1;
    settle_and_poll();
    CHECK(has_drive("usb0"));
    h2 = lfs_open("usb0:/Movies/Film One (2019).mkv");
    CHECK(h2 >= 0 && lfs_read(h2, 0, buf.data(), 4096) == 4096);
    CHECK(h2 != h || lfs_read(h, 0, buf.data(), 16) >= 0);            // (the slot was reused: same number is fine)
    lfs_close(h2);
}

static void unsupported_and_probing() {
    printf("- a drive nobody can read\n");
    // 8 MB of zeros: no partition table, no file system
    if (system("dd if=/dev/zero of=/tmp/jf_lfs_blank.img bs=1M count=8 status=none") != 0) printf("FAIL dd\n");
    g_port_fd[5] = open("/tmp/jf_lfs_blank.img", O_RDONLY);
    g_present[5] = true;
    const uint32_t gen0 = lfs_generation();
    poll();
    CHECK(!has_drive("usb5"));
    advance(3 * SEC);
    poll();
    lfs_drive d;
    CHECK(has_drive("usb5", &d) && d.kind == LFS_USB_UNSUPPORTED && !strcmp(d.label, "USB drive 6"));
    CHECK(lfs_generation() != gen0);
    CHECK(lfs_open("usb5:/a.mkv") == LFS_E_NOTFOUND);
    // it is probed once, not on every poll (that would blink the drive's light for ever)
    const unsigned long reads = g_port_reads[5];
    for (int i = 0; i < 10; i++) { advance(2 * SEC); poll(); }
    CHECK(g_port_reads[5] == reads);

    // lv2 mounts it as FAT32 after all (late): it becomes a FAT drive
    char tmpl[] = "/tmp/jf_lfs_late_XXXXXX";
    g_fat_root[5] = mkdtemp(tmpl);
    write_file(g_fat_root[5] + "/a.mkv", "abc");
    g_fat_up[5] = true;
    poll();
    CHECK(has_drive("usb5", &d) && d.kind == LFS_USB_FAT);
    int h = lfs_open("usb5:/a.mkv");
    CHECK(h >= 0 && lfs_size(h) == 3);
    lfs_close(h);

    g_present[5] = false; g_fat_up[5] = false;
    close(g_port_fd[5]); g_port_fd[5] = -1;
    poll();
    CHECK(!has_drive("usb5"));
}

static void benchmark_log() {
    printf("- throughput log\n");
    attach(6, "ntfs_flat");
    settle_and_poll();
    g_log.clear();
    int h = lfs_open("usb6:/Movies/Film One (2019).mkv");
    CHECK(h >= 0);
    std::vector<uint8_t> buf(3 * MB);
    for (int i = 0; i < 25; i++) lfs_read(h, 0, buf.data(), 3 * MB);  // 75 MB
    lfs_close(h);
    int count = 0;
    for (auto &l : g_log) if (l.find("usb6 NTFS read 64 MB") != std::string::npos) count++;
    CHECK(count == 1);                                                // once per mount
    detach(6);
    poll();
}

// ---- writing to an NTFS drive -------------------------------------------------------

static void fresh_ntfs(int port, const char *path) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -f %s && truncate -s 128M %s && mkntfs -F -f -q -L JFWRITE %s >/dev/null 2>&1", path, path, path);
    if (system(cmd) != 0) { printf("FAIL mkntfs %s\n", path); s_failed++; return; }
    if (g_port_fd[port] >= 0) close(g_port_fd[port]);
    g_port_fd[port] = open(path, O_RDWR);
    g_port_fail[port] = 0;
    g_present[port] = true;
}

static void replug(int port, const char *path) {         // pulled, then plugged in again
    g_present[port] = false;
    poll();
    if (g_port_fd[port] >= 0) close(g_port_fd[port]);
    g_port_fd[port] = open(path, O_RDWR);
    g_port_fail[port] = 0;
    g_present[port] = true;
    settle_and_poll();
}

static std::vector<std::string> g_names;
static void collect_name(const char *name, bool is_dir, void *) { g_names.push_back(std::string(is_dir ? "/" : "") + name); }

static void ntfs_writes() {
    printf("- writing to an NTFS drive\n");
    const char *img = "/tmp/jf_lfs_w.img";
    fresh_ntfs(3, img);
    settle_and_poll();
    lfs_drive d;
    CHECK(has_drive("usb3", &d) && d.kind == LFS_USB_NTFS);
    CHECK(lfs_drive_writable("usb3:/"));
    CHECK(strcmp(lfs_drive_why_readonly("usb3:/"), "") == 0);
    CHECK(!lfs_drive_writable("hdd:/video"));                               // not an lfs write target
    CHECK(lfs_mkdir("hdd:/video/x") == LFS_E_READONLY);
    CHECK(lfs_drive_why_readonly("usb6:/")[0] != '\0');                     // nothing there

    uint64_t free0 = 0, total = 0;
    CHECK(lfs_space("usb3:/", &free0, &total) && total > 100ull * MB && free0 > 0);
    CHECK(lfs_mkdir("usb3:/jellyfin_offline") == 0);
    CHECK(lfs_mkdir("usb3:/jellyfin_offline") == 0);                        // already there: fine
    CHECK(lfs_mkdir("usb3:/jellyfin_offline/items") == 0);
    CHECK(lfs_mkdir("usb3:/no/such/parent") < 0);

    // a download: appended, read back while it grows, closed, resumed
    std::vector<uint8_t> chunk(512 * 1024), want(3 * MB + 777);
    gen(31, 0, want.data(), want.size());
    int h = lfs_open_append("usb3:/jellyfin_offline/items/media.ts.part");
    CHECK(h >= 0);
    uint64_t at = 0;
    while (at < 2 * MB) {
        const uint32_t n = (uint32_t)std::min<uint64_t>(chunk.size(), 2 * MB - at);
        CHECK(lfs_write(h, want.data() + at, n) == (int)n);
        at += n;
    }
    CHECK(lfs_size(h) == 2 * MB);
    uint8_t back[4096];
    CHECK(lfs_read(h, MB, back, sizeof back) == (int)sizeof back && memcmp(back, want.data() + MB, sizeof back) == 0);
    CHECK(lfs_write(h, want.data() + at, 100) == 100);                     // a read in between does not move the append point
    at += 100;
    lfs_close(h);
    lfs_entry e;
    CHECK(lfs_stat("usb3:/jellyfin_offline/items/media.ts.part", &e) && e.size == at);
    h = lfs_open_append("usb3:/jellyfin_offline/items/media.ts.part");      // the app restarts: resume
    CHECK(h >= 0 && lfs_size(h) == at);
    CHECK(lfs_write(h, want.data() + at, (uint32_t)(want.size() - at)) == (int)(want.size() - at));
    lfs_close(h);
    CHECK(lfs_rename("usb3:/jellyfin_offline/items/media.ts.part", "usb3:/jellyfin_offline/items/media.ts") == 0);
    h = lfs_open("usb3:/jellyfin_offline/items/media.ts");
    CHECK(h >= 0 && lfs_size(h) == want.size());
    if (h >= 0) { CHECK(read_whole(h, want.size(), 65536) == want); lfs_close(h); }

    // records: written whole, swapped in
    h = lfs_open_append("usb3:/jellyfin_offline/items/state.txt");
    CHECK(h >= 0 && lfs_write(h, "state 1\n", 8) == 8);
    lfs_close(h);
    h = lfs_open_append("usb3:/jellyfin_offline/items/state.txt.tmp");
    CHECK(h >= 0 && lfs_write(h, "state 2\n", 8) == 8);
    lfs_close(h);
    CHECK(lfs_rename("usb3:/jellyfin_offline/items/state.txt.tmp", "usb3:/jellyfin_offline/items/state.txt") == LFS_E_EXISTS);
    CHECK(lfs_remove("usb3:/jellyfin_offline/items/state.txt") == 0);
    CHECK(lfs_rename("usb3:/jellyfin_offline/items/state.txt.tmp", "usb3:/jellyfin_offline/items/state.txt") == 0);
    CHECK(lfs_remove("usb3:/jellyfin_offline/items/nothing.txt") == 0);     // absent: fine
    CHECK(lfs_remove("usb3:/jellyfin_offline/items") == LFS_E_BADPATH);     // a folder is not a file
    CHECK(lfs_rmdir("usb3:/jellyfin_offline/items") < 0);                   // not empty

    g_names.clear();
    CHECK(lfs_list_names("usb3:/jellyfin_offline/items", collect_name, nullptr) == 2);
    std::sort(g_names.begin(), g_names.end());
    CHECK(g_names == (std::vector<std::string>{ "media.ts", "state.txt" }));
    uint64_t free1 = 0;
    CHECK(lfs_space("usb3:/", &free1, &total) && free1 + want.size() <= free0);

    // pulled with a file open for writing: it comes back read-only (dirty flag up), and says why
    h = lfs_open_append("usb3:/jellyfin_offline/items/second.ts.part");
    CHECK(h >= 0 && lfs_write(h, want.data(), 65536) == 65536);
    g_port_fail[3] = 1; g_present[3] = false;
    CHECK(lfs_write(h, want.data(), 65536) == LFS_E_REMOVED);
    replug(3, img);
    lfs_close(h);                                                           // stale: only freed
    CHECK(has_drive("usb3"));
    CHECK(!lfs_drive_writable("usb3:/"));
    CHECK(strstr(lfs_drive_why_readonly("usb3:/"), "chkdsk") != nullptr);
    CHECK(lfs_mkdir("usb3:/after") == LFS_E_READONLY);
    h = lfs_open("usb3:/jellyfin_offline/items/media.ts");                 // still reads
    CHECK(h >= 0 && lfs_read(h, 0, back, 16) == 16 && memcmp(back, want.data(), 16) == 0);
    lfs_close(h);

    // chkdsk, on a PC (ntfsfix -d here clears the flag): writable again
    lfs_unmount_all();
    CHECK(system("ntfsfix -d /tmp/jf_lfs_w.img >/dev/null 2>&1") == 0);
    replug(3, img);
    CHECK(lfs_drive_writable("usb3:/"));
    CHECK(lfs_remove("usb3:/jellyfin_offline/items/second.ts.part") == 0);

    // a write that fails with the drive still there: read-only from then on, with the reason
    h = lfs_open_append("usb3:/jellyfin_offline/items/third.ts.part");
    CHECK(h >= 0 && lfs_write(h, want.data(), 65536) == 65536);
    g_port_fail[3] = 1;
    CHECK(lfs_write(h, want.data(), 65536) == LFS_E_IO);
    g_port_fail[3] = 0;
    CHECK(!lfs_drive_writable("usb3:/"));
    CHECK(strstr(lfs_drive_why_readonly("usb3:/"), "failed") != nullptr);
    CHECK(lfs_write(h, want.data(), 16) == LFS_E_READONLY);
    lfs_close(h);
    CHECK(lfs_mkdir("usb3:/later") == LFS_E_READONLY);
    // ...and its dirty flag stayed up: after a replug it is still read-only
    replug(3, img);
    CHECK(!lfs_drive_writable("usb3:/"));
    lfs_unmount_all();
    CHECK(system("ntfsfix -d /tmp/jf_lfs_w.img >/dev/null 2>&1") == 0);
    replug(3, img);
    CHECK(lfs_drive_writable("usb3:/"));

    // the app exits with a download open: the clean unmount closes it and puts the flag down
    h = lfs_open_append("usb3:/jellyfin_offline/items/third.ts.part");
    CHECK(h >= 0 && lfs_write(h, want.data(), 65536) == 65536);
    lfs_unmount_all();
    CHECK(!has_drive("usb3"));
    settle_and_poll();
    CHECK(lfs_drive_writable("usb3:/"));
    CHECK(lfs_stat("usb3:/jellyfin_offline/items/third.ts.part", &e) && e.size >= 65536);

    // Windows hibernated with the drive attached: never written
    h = lfs_open_append("usb3:/hiberfil.sys");
    CHECK(h >= 0 && lfs_write(h, "hibr", 4) == 4);
    lfs_close(h);
    replug(3, img);
    CHECK(has_drive("usb3") && !lfs_drive_writable("usb3:/"));
    CHECK(strstr(lfs_drive_why_readonly("usb3:/"), "hibernated") != nullptr);
    lfs_unmount_all();

    // clusters smaller than an MFT record: read, never written
    CHECK(system("rm -f /tmp/jf_lfs_w.img && truncate -s 128M /tmp/jf_lfs_w.img && "
                 "mkntfs -F -f -q -c 512 -L SMALL /tmp/jf_lfs_w.img >/dev/null 2>&1") == 0);
    replug(3, img);
    CHECK(has_drive("usb3") && !lfs_drive_writable("usb3:/"));
    CHECK(strstr(lfs_drive_why_readonly("usb3:/"), "4 KB") != nullptr);
    CHECK(lfs_mkdir("usb3:/x") == LFS_E_READONLY);
    lfs_unmount_all();
    detach(3);
}

int main() {
    char tmpl[] = "/tmp/jf_lfs_hdd_XXXXXX";
    g_hdd_root = mkdtemp(tmpl);
    write_file(g_hdd_root + "/video/ep2.mkv", gen_str(21, 3000));
    write_file(g_hdd_root + "/video/ep10.mkv", gen_str(22, 3000));
    write_file(g_hdd_root + "/video/movie.m2ts", gen_str(23, 1111));
    write_file(g_hdd_root + "/video/Season/s1.mkv", "x");
    write_file(g_hdd_root + "/video/cover.jpg", "jpg");
    write_file(g_hdd_root + "/video/.hidden.mkv", "h");
    write_file(g_hdd_root + "/video/notes.txt", "t");
    mkdir((g_hdd_root + "/music").c_str(), 0755);

    lfs_set_port(&HOST_PORT);
    poll();
    internal_disk();
    too_many_handles();
    paging();
    fat_stick();
    image_drives();
    unplug_mid_read();
    unsupported_and_probing();
    benchmark_log();
    ntfs_writes();
    printf("lfs: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
