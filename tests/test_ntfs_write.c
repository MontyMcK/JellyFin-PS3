// The NTFS write path (third_party/mohasi_fs/ntfs.c) driven the way the offline downloads drive it,
// on an image file standing in for the USB drive.  tests/fixtures/fs/ntfs_write_check.sh makes the
// images with mkntfs, runs this, and then judges the result with tests/tools/check_ntfs (libntfs-3g,
// not this code) and ntfscat.
//
//   test_ntfs_write workload <image> [scale]    the download store: ~400 item folders, records
//                                               saved .tmp-then-swap, two big media files appended
//                                               in 512 KB pieces side by side (so they fragment),
//                                               resume, rename, cancel, delete; then a remount
//                                               reads everything back.  Prints each media file's
//                                               path and FNV-1a hash for the ntfscat cross-check.
//   test_ntfs_write fill <image>                write until the disk is full: the write must fail
//                                               cleanly, and deleting the file must give the space back.
//   test_ntfs_write cut <image> <writes> [scale]  the workload with the drive pulled after <writes>
//                                               storage writes.  Then: a remount must come up
//                                               read-only (the dirty flag was set) and still read.
//
// The driver functions are called exactly as source/local/lfs.cpp calls them, with the same rules
// around them (a write open marks the handle writable; the dirty flag is cleared once no writer is
// open).  Single-threaded here, so no lock.

#define _FILE_OFFSET_BITS 64
#include "jf_port.h"
#include "ntfs.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int g_port_fd[8];
extern int g_port_fail[8];
extern long g_port_write_budget[8];
extern unsigned long g_port_writes[8];

static int g_fail = 0, g_checks = 0;
static int g_cut = 0;          // a cut run: failures after the cut are the point, not test failures

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); printf("\n"); } } while (0)

static NtfsVolume vol;

// ---- deterministic content --------------------------------------------------------------------

static uint64_t mix(uint64_t x)
{
   x += 0x9E3779B97F4A7C15ULL;
   x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
   x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
   return x ^ (x >> 31);
}

static void fill(uint8_t *buf, uint32_t n, uint64_t key, uint64_t off)
{
   for (uint32_t i = 0; i < n; i++) {
      const uint64_t o = off + i;
      buf[i] = (uint8_t)(mix(key ^ (o >> 3)) >> ((o & 7) * 8));
   }
}

static uint64_t fnv(uint64_t h, const uint8_t *p, uint32_t n)
{
   for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ULL; }
   return h;
}
#define FNV_INIT 0xCBF29CE484222325ULL

// ---- the calls, as lfs makes them --------------------------------------------------------------

static NtfsFile s_open[4];
static int      s_used[4];

static int any_writer(void)
{
   for (int i = 0; i < 4; i++) if (s_used[i] && s_open[i].writable) return 1;
   return 0;
}
static void settle(void) { if (!any_writer()) ntfsClearDirty(&vol); }

// opens for append (creating it), like dl_plat_file_open_append; returns a slot or -1
static int open_append(const char *path)
{
   int s = -1;
   for (int i = 0; i < 4; i++) if (!s_used[i]) { s = i; break; }
   if (s < 0 || !vol.writable) return -1;
   int rc = openNtfs(&s_open[s], &vol, path);
   if (rc != 0 && createNtfsPath(&vol, path) == 0) rc = openNtfs(&s_open[s], &vol, path);
   if (rc != 0) return -1;
   s_open[s].writable = 1;
   seekNtfs(&s_open[s], s_open[s].size);
   s_used[s] = 1;
   return s;
}

static int close_file(int s)
{
   int rc = closeNtfs(&s_open[s]);
   s_used[s] = 0;
   if (rc == 0) settle();
   return rc;
}

static int write_all(int s, const uint8_t *buf, uint32_t n)
{
   uint32_t done = 0;
   while (done < n) {
      int w = writeNtfs(&s_open[s], buf + done, (int)(n - done));
      if (w <= 0) return -1;
      done += (uint32_t)w;
   }
   return 0;
}

static int mkdir_p(const char *p) { int r = mkdirNtfsPath(&vol, p); settle(); return (r == 0 || r == -2) ? 0 : -1; }
static int unlink_p(const char *p) { int r = unlinkNtfsPath(&vol, p); settle(); return (r == 0 || r == -2) ? 0 : -1; }
static int rmdir_p(const char *p) { int r = rmdirNtfsPath(&vol, p); settle(); return (r == 0 || r == -2) ? 0 : -1; }
static int rename_p(const char *a, const char *b) { int r = renameNtfsPath(&vol, a, b); settle(); return r; }

// a small record, the dl_store way: <path>.tmp written whole, the old file removed, the .tmp renamed in
static int save_text(const char *path, uint64_t key, uint32_t len)
{
   char tmp[300];
   snprintf(tmp, sizeof tmp, "%s.tmp", path);
   uint8_t buf[4096] __attribute__((aligned(32)));
   if (len > sizeof buf) len = sizeof buf;
   fill(buf, len, key, 0);
   unlink_p(tmp);
   int s = open_append(tmp);
   if (s < 0) { if (!g_cut) printf("  save_text: open %s failed\n", tmp); return -1; }
   int ok = write_all(s, buf, len) == 0;
   ok = close_file(s) == 0 && ok;
   if (!ok) { if (!g_cut) printf("  save_text: write/close %s failed\n", tmp); return -1; }
   if (unlink_p(path) != 0) { if (!g_cut) printf("  save_text: unlink %s failed\n", path); return -1; }
   const int rc = rename_p(tmp, path);
   if (rc != 0 && !g_cut) printf("  save_text: rename %s -> %s = %d\n", tmp, path, rc);
   return rc == 0 ? 0 : -1;
}

static int stat_size(const char *path, uint64_t *size)
{
   NtfsInfo i;
   if (statNtfs(&vol, path, &i) != 0) return -1;
   *size = i.size;
   return 0;
}

// reads a whole file back and compares it with the generator; returns its FNV hash (0 on mismatch)
static uint64_t verify_file(const char *path, uint64_t key, uint64_t want_size)
{
   static uint8_t got[1 << 20] __attribute__((aligned(32)));
   static uint8_t exp[1 << 20];
   NtfsFile f;
   if (openNtfs(&f, &vol, path) != 0) { CHECK(0, "%s does not open", path); return 0; }
   CHECK(f.size == want_size, "%s: size %llu, want %llu", path, (unsigned long long)f.size, (unsigned long long)want_size);
   uint64_t h = FNV_INIT, off = 0;
   while (off < f.size) {
      uint32_t n = (f.size - off) < sizeof got ? (uint32_t)(f.size - off) : (uint32_t)sizeof got;
      int r = readNtfs(&f, got, (int)n);
      if (r != (int)n) { CHECK(0, "%s: read at %llu gave %d", path, (unsigned long long)off, r); closeNtfs(&f); return 0; }
      fill(exp, n, key, off);
      if (memcmp(got, exp, n) != 0) {
         uint32_t i = 0; while (got[i] == exp[i]) i++;
         CHECK(0, "%s: content differs at byte %llu", path, (unsigned long long)(off + i));
         closeNtfs(&f); return 0;
      }
      h = fnv(h, got, n);
      off += n;
   }
   closeNtfs(&f);
   return h;
}

static void open_image(const char *img)
{
   g_port_fd[0] = open(img, O_RDWR);
   if (g_port_fd[0] < 0) { perror(img); exit(2); }
}

static int mount_it(void)
{
   memset(&vol, 0, sizeof vol);
   memset(s_used, 0, sizeof s_used);
   return mountNtfs(&vol, 0);
}

// ---- the download-store workload ----------------------------------------------------------------

#define ROOT "/jellyfin_offline"

static void item_id(char *out, int i) { snprintf(out, 33, "%08x%08x%08x%08x", 0x51ab0000u + (unsigned)i, 0xdeadbeefu ^ (unsigned)i, 7u * (unsigned)i, 0x1234u); }

// returns 0 when it ran to the end; -1 when a call failed (expected only in a cut run)
static int workload(int scale)
{
   const int n_items = 400 / scale < 8 ? 8 : 400 / scale;
   const uint64_t media_bytes = (300ull << 20) / (uint64_t)scale;
   char p[300], q[300], id[40];

   if (mkdir_p(ROOT) || mkdir_p(ROOT "/items")) { CHECK(g_cut, "mkdir root"); return -1; }
   if (save_text(ROOT "/layout.txt", 1, 14)) { CHECK(g_cut, "layout.txt"); return -1; }

   for (int i = 0; i < n_items; i++) {
      item_id(id, i);
      snprintf(p, sizeof p, ROOT "/items/%s", id);
      if (mkdir_p(p)) { CHECK(g_cut, "mkdir item %d", i); return -1; }
      for (int round = 0; round < 3; round++) {
         snprintf(q, sizeof q, "%s/state.txt", p);
         if (save_text(q, 1000 + (uint64_t)i * 4 + (uint64_t)round, 300 + (uint32_t)(i % 7) * 50)) { CHECK(g_cut, "state %d/%d", i, round); return -1; }
      }
      snprintf(q, sizeof q, "%s/meta.txt", p);
      if (save_text(q, 50000 + (uint64_t)i, 1500 + (uint32_t)(i % 13) * 40)) { CHECK(g_cut, "meta %d", i); return -1; }
      if (i % 10 == 0) {     // artwork: a few hundred KB, non-resident
         snprintf(q, sizeof q, "%s/poster.jpg", p);
         int s = open_append(q);
         if (s < 0) { CHECK(g_cut, "poster %d", i); return -1; }
         static uint8_t img[200000] __attribute__((aligned(32)));
         fill(img, sizeof img, 90000 + (uint64_t)i, 0);
         int bad = write_all(s, img, sizeof img);
         if (close_file(s) || bad) { CHECK(g_cut, "poster %d write", i); return -1; }
      }
   }

   // two downloads side by side, 512 KB pieces (odd sizes now and then), a resume in the middle
   static uint8_t buf[512 * 1024] __attribute__((aligned(32)));
   char part[2][300];
   uint64_t have[2] = { 0, 0 };
   for (int k = 0; k < 2; k++) { item_id(id, k); snprintf(part[k], sizeof part[k], ROOT "/items/%s/media.ts.part", id); }
   int fh[2] = { open_append(part[0]), open_append(part[1]) };
   if (fh[0] < 0 || fh[1] < 0) { CHECK(g_cut, "open parts"); return -1; }
   int step = 0;
   while (have[0] < media_bytes || have[1] < media_bytes) {
      for (int k = 0; k < 2; k++) {
         if (have[k] >= media_bytes) continue;
         uint32_t n = sizeof buf - (uint32_t)((step % 5) * 188 * 7);          // not always sector-sized
         if (have[k] + n > media_bytes) n = (uint32_t)(media_bytes - have[k]);
         fill(buf, n, 777 + (uint64_t)k, have[k]);
         if (write_all(fh[k], buf, n)) { CHECK(g_cut, "append %d at %llu", k, (unsigned long long)have[k]); return -1; }
         have[k] += n;
      }
      if (++step == 40) {               // the app restarts: both closed, reopened at their ends
         for (int k = 0; k < 2; k++) {
            if (close_file(fh[k])) { CHECK(g_cut, "close for resume"); return -1; }
            uint64_t sz = 0;
            if (stat_size(part[k], &sz) || sz != have[k]) { CHECK(g_cut, "resume size %d: %llu vs %llu", k, (unsigned long long)sz, (unsigned long long)have[k]); return -1; }
            fh[k] = open_append(part[k]);
            if (fh[k] < 0) { CHECK(g_cut, "reopen %d", k); return -1; }
         }
      }
   }
   for (int k = 0; k < 2; k++) if (close_file(fh[k])) { CHECK(g_cut, "close part %d", k); return -1; }

   // item 0 completes: media.ts.part -> media.ts.  item 1 is cancelled: its part is deleted.
   item_id(id, 0);
   snprintf(q, sizeof q, ROOT "/items/%s/media.ts", id);
   if (rename_p(part[0], q)) { CHECK(g_cut, "rename media"); return -1; }
   if (unlink_p(part[1])) { CHECK(g_cut, "cancel part"); return -1; }

   // a third of the items deleted, the dl_store way: known names, then the folder
   for (int i = 2; i < n_items; i += 3) {
      item_id(id, i);
      static const char *const leaves[] = { "state.txt", "state.txt.tmp", "meta.txt", "meta.txt.tmp", "poster.jpg", "media.ts", "media.ts.part" };
      for (unsigned l = 0; l < sizeof leaves / sizeof leaves[0]; l++) {
         snprintf(q, sizeof q, ROOT "/items/%s/%s", id, leaves[l]);
         if (unlink_p(q)) { CHECK(g_cut, "delete %s", q); return -1; }
      }
      snprintf(p, sizeof p, ROOT "/items/%s", id);
      if (rmdir_p(p)) { CHECK(g_cut, "rmdir item %d", i); return -1; }
   }
   return 0;
}

static void verify_workload(int scale)
{
   const int n_items = 400 / scale < 8 ? 8 : 400 / scale;
   const uint64_t media_bytes = (300ull << 20) / (uint64_t)scale;
   char p[300], id[40];
   int listed = 0;
   NtfsInfo di;
   CHECK(statNtfs(&vol, ROOT "/items", &di) == 0 && di.isDir, "items/ missing after remount");
   NtfsDir d; char name[300]; NtfsInfo ei;
   openNtfsDir(&d, &vol, di.mftReference);
   while (readNtfsDir(&d, name, sizeof name, &ei) == 1) listed++;
   closeNtfsDir(&d);
   int want = 0;
   for (int i = 0; i < n_items; i++) if (!(i >= 2 && (i - 2) % 3 == 0)) want++;
   CHECK(listed == want, "items/ lists %d folders, want %d", listed, want);
   for (int i = 0; i < n_items; i++) {
      item_id(id, i);
      const int deleted = i >= 2 && (i - 2) % 3 == 0;
      snprintf(p, sizeof p, ROOT "/items/%s", id);
      NtfsInfo x;
      if (deleted) { CHECK(statNtfs(&vol, p, &x) != 0, "deleted item %d still there", i); continue; }
      char q[300];
      snprintf(q, sizeof q, "%s/state.txt", p);
      CHECK(verify_file(q, 1000 + (uint64_t)i * 4 + 2, 300 + (uint32_t)(i % 7) * 50) != 0, "state %d", i);
      snprintf(q, sizeof q, "%s/state.txt.tmp", p);
      CHECK(statNtfs(&vol, q, &x) != 0, "state.txt.tmp %d left behind", i);
      snprintf(q, sizeof q, "%s/meta.txt", p);
      CHECK(verify_file(q, 50000 + (uint64_t)i, 1500 + (uint32_t)(i % 13) * 40) != 0, "meta %d", i);
      if (i % 10 == 0) { snprintf(q, sizeof q, "%s/poster.jpg", p); CHECK(verify_file(q, 90000 + (uint64_t)i, 200000) != 0, "poster %d", i); }
   }
   item_id(id, 0);
   snprintf(p, sizeof p, ROOT "/items/%s/media.ts", id);
   uint64_t h = verify_file(p, 777, media_bytes);
   CHECK(h != 0, "media.ts");
   printf("HASH %s %016llx\n", p, (unsigned long long)h);
   item_id(id, 1);
   snprintf(p, sizeof p, ROOT "/items/%s/media.ts.part", id);
   NtfsInfo x;
   CHECK(statNtfs(&vol, p, &x) != 0, "cancelled part still there");
}

int main(int argc, char **argv)
{
   if (argc == 2 && !strcmp(argv[1], "fnv")) {     // the hash of stdin, for comparing with ntfscat's output
      static uint8_t b[1 << 20];
      uint64_t h = FNV_INIT; size_t n;
      while ((n = fread(b, 1, sizeof b, stdin)) > 0) h = fnv(h, b, (uint32_t)n);
      printf("%016llx\n", (unsigned long long)h);
      return 0;
   }
   if (argc < 3) { fprintf(stderr, "usage: test_ntfs_write workload|fill|cut|big <image> [n] [scale] | fnv\n"); return 2; }
   const char *mode = argv[1];
   open_image(argv[2]);

   if (!strcmp(mode, "workload")) {
      const int scale = argc > 3 ? atoi(argv[3]) : 1;
      CHECK(mount_it() == NTFS_MOUNT_OK, "mount");
      CHECK(vol.writable, "a fresh mkntfs volume mounts writable");
      uint64_t free0 = 0, total = 0;
      getNtfsFree(&vol, &free0, &total);
      CHECK(workload(scale) == 0, "workload ran to the end");
      CHECK(!vol.volumeDirty, "dirty flag cleared once no writer is open");
      uint64_t free1 = 0;
      getNtfsFree(&vol, &free1, &total);
      unmountNtfs(&vol);
      CHECK(mount_it() == NTFS_MOUNT_OK && vol.writable, "remount writable (the volume is clean)");
      uint64_t free2 = 0;
      getNtfsFree(&vol, &free2, &total);
      CHECK(free2 == free1, "free space after remount %llu == as tracked %llu", (unsigned long long)free2, (unsigned long long)free1);
      verify_workload(scale);
      printf("FREE %llu %llu %llu\n", (unsigned long long)free0, (unsigned long long)free1, (unsigned long long)total);
      unmountNtfs(&vol);
   } else if (!strcmp(mode, "fill")) {
      CHECK(mount_it() == NTFS_MOUNT_OK && vol.writable, "mount");
      uint64_t free0 = 0, total = 0;
      getNtfsFree(&vol, &free0, &total);
      CHECK(mkdir_p("/fill") == 0, "mkdir");
      int s = open_append("/fill/big.bin");
      CHECK(s >= 0, "open");
      static uint8_t buf[512 * 1024] __attribute__((aligned(32)));
      uint64_t have = 0;
      for (;;) {
         fill(buf, sizeof buf, 4242, have);
         int w = writeNtfs(&s_open[s], buf, (int)sizeof buf);
         if (w <= 0) break;
         have += (uint64_t)w;
         if (have > total) { CHECK(0, "wrote more than the volume holds"); break; }
      }
      uint64_t freeFull = 0;
      getNtfsFree(&vol, &freeFull, &total);
      CHECK(freeFull < (1u << 20) * 2, "disk full: %llu bytes free after the write failed", (unsigned long long)freeFull);
      CHECK(close_file(s) == 0, "close after disk full");
      CHECK(!vol.volumeDirty, "clean after the failed write");
      uint64_t sz = 0;
      CHECK(stat_size("/fill/big.bin", &sz) == 0 && sz == have, "size %llu == written %llu", (unsigned long long)sz, (unsigned long long)have);
      printf("FILLED %llu of %llu free\n", (unsigned long long)have, (unsigned long long)free0);
      unmountNtfs(&vol);
      CHECK(mount_it() == NTFS_MOUNT_OK && vol.writable, "remount");
      CHECK(verify_file("/fill/big.bin", 4242, have) != 0, "content after disk full");
      CHECK(unlink_p("/fill/big.bin") == 0 && rmdir_p("/fill") == 0, "delete");
      uint64_t freeEnd = 0;
      getNtfsFree(&vol, &freeEnd, &total);
      // all of it but what $MFT grew by for the two new records ($MFT never shrinks, and grows by
      // 256 records at least); check_ntfs proves separately that nothing leaked
      CHECK(freeEnd <= free0 && free0 - freeEnd <= 1024 * 1024, "space back: %llu, was %llu", (unsigned long long)freeEnd, (unsigned long long)free0);
      unmountNtfs(&vol);
   } else if (!strcmp(mode, "big")) {
      // one file past 4 GiB, written the way a download is (512 KB pieces, a resume), read back after a remount
      const uint64_t want = (argc > 3 ? (uint64_t)atoll(argv[3]) : 4608) << 20;
      CHECK(mount_it() == NTFS_MOUNT_OK && vol.writable, "mount");
      CHECK(mkdir_p("/big") == 0, "mkdir");
      static uint8_t buf[512 * 1024] __attribute__((aligned(32)));
      uint64_t have = 0;
      int s = open_append("/big/film.ts.part");
      CHECK(s >= 0, "open");
      while (s >= 0 && have < want) {
         uint32_t n = want - have < sizeof buf ? (uint32_t)(want - have) : (uint32_t)sizeof buf;
         fill(buf, n, 5150, have);
         if (write_all(s, buf, n)) { CHECK(0, "append at %llu", (unsigned long long)have); break; }
         have += n;
         if (have == (want / 2 & ~(uint64_t)0xFFFFF)) {      // the app restarts half way
            CHECK(close_file(s) == 0, "close half way");
            s = open_append("/big/film.ts.part");
            CHECK(s >= 0 && s_open[s].size == have, "resume at %llu", (unsigned long long)have);
         }
      }
      if (s >= 0) CHECK(close_file(s) == 0, "close");
      CHECK(rename_p("/big/film.ts.part", "/big/film.ts") == 0, "rename");
      unmountNtfs(&vol);
      CHECK(mount_it() == NTFS_MOUNT_OK && vol.writable, "remount");
      uint64_t h = verify_file("/big/film.ts", 5150, want);
      CHECK(h != 0, "content");
      printf("HASH /big/film.ts %016llx\n", (unsigned long long)h);
      unmountNtfs(&vol);
   } else if (!strcmp(mode, "cut")) {
      const long budget = argc > 3 ? atol(argv[3]) : 100;
      const int scale = argc > 4 ? atoi(argv[4]) : 16;
      g_cut = 1;
      CHECK(mount_it() == NTFS_MOUNT_OK && vol.writable, "mount");
      g_port_write_budget[0] = budget;
      const int done = workload(scale) == 0;
      // the drive is gone: what the app does next is unmount; nothing more may reach it
      unmountNtfs(&vol);
      g_port_fail[0] = 0; g_port_write_budget[0] = -1;
      const unsigned long writes_before = g_port_writes[0];
      const int rc = mount_it();
      CHECK(rc == NTFS_MOUNT_OK, "remount after the cut");
      CHECK(g_port_writes[0] == writes_before, "mounting wrote to the volume");
      // What the DISK says decides it, not the driver's memory (a cut inside the two-part
      // $MFT/$MFTMirr write of the flag splits them): dirty on disk -> the remount is read-only and
      // check_ntfs -c allows only repairable damage; clean on disk -> check_ntfs -c demands clean.
      NtfsInfo x;
      CHECK(statNtfs(&vol, "/", &x) == 0, "root still reads");
      printf("CUT budget=%ld finished=%d writable_after=%d writes=%lu\n", budget, done, vol.writable, writes_before);
      unmountNtfs(&vol);
   } else {
      fprintf(stderr, "unknown mode %s\n", mode);
      return 2;
   }
   printf("ntfs write %s: %d checks, %d failed\n", mode, g_checks, g_fail);
   return g_fail ? 1 : 0;
}
