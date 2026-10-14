// Host test for the vendored NTFS / exFAT readers (third_party/mohasi_fs).
//
// The images under fixtures/fs/ were made with the Linux drivers (make_images.sh), as a bare
// filesystem and inside an MBR and a GPT partition.  The readers run here on a fake storage
// device that reads the image file, and every file is compared with the bytes tree.py wrote
// (the same generator is below).  Built with a big-endian cross compiler and run under
// qemu-ppc64 it proves the readers on the console's byte order too.
//
//   make -f Makefile.host test_mohasi_fs && ./test_mohasi_fs
//   make -f Makefile.host test_mohasi_fs_be     # needs powerpc64-linux-gnu-gcc + qemu-ppc64

#define _FILE_OFFSET_BITS 64
#include "jf_port.h"
#include "ntfs.h"
#include "exfat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

extern int g_port_fd[8], g_port_fail[8];
#define g_dev_fd   g_port_fd[0]
#define g_dev_fail g_port_fail[0]

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define MB (1024 * 1024)

// ---- tree.py's generator -------------------------------------------------------
static void gen(int seed, uint64_t start, uint8_t *out, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      uint64_t idx = start + i, block = idx >> 6;
      unsigned off = (unsigned)(idx & 63);
      out[i] = off == 0 ? (uint8_t)(block & 0xFF)
             : off == 1 ? (uint8_t)((block >> 8) & 0xFF)
                        : (uint8_t)((block * 7 + (unsigned)seed) & 0xFF);
   }
}

typedef struct { const char *path; uint64_t size; int seed; const char *literal; } Expect;
static const Expect FILES[] = {
   { "/hello.txt", 12, 0, "hello world\n" },
   { "/Movies/Film One (2019).mkv", 3 * MB, 11, 0 },
   { "/Movies/sub/Deep.ts", 100 * 1024 + 17, 12, 0 },
   { "/Music/Album/01 Track.flac", 200 * 1024, 13, 0 },
   { "/Unicode/Caf\xC3\xA9 \xC3\x9Cn\xC3\xAF" "code.txt", 8, 0, "unicode\n" },
   { "/Unicode/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E/\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB.txt", 8, 0, "nihongo\n" },
   { "/Unicode/emoji \xF0\x9F\x8E\xAC clip.mkv", 70000, 14, 0 },     // U+1F3AC: a surrogate pair on disk
};
#define NFILES ((int)(sizeof FILES / sizeof FILES[0]))
#define BIG_SIZE      (5ULL * 1024 * 1024 * 1024)
#define BIG_TAIL_AT   (BIG_SIZE - 4096)
#define BIG_TAIL_SEED 99

// ---- one filesystem behind one interface -----------------------------------------
typedef struct {
   const char *name;
   int is_ntfs;
   NtfsVolume nv; NtfsFile nf; NtfsDir nd;
   ExfatVolume ev; ExfatFile ef; ExfatDir ed;
} Fs;

static int fs_mount(Fs *f) {
   return f->is_ntfs ? mountNtfs(&f->nv, 0) : mountExfat(&f->ev, 0);
}
static void fs_unmount(Fs *f) { if (f->is_ntfs) unmountNtfs(&f->nv); else unmountExfat(&f->ev); }

typedef struct { uint64_t size; int is_dir; } Stat;
static int fs_stat(Fs *f, const char *path, Stat *s) {
   if (f->is_ntfs) {
      NtfsInfo i;
      if (statNtfs(&f->nv, path, &i) != 0) return -1;
      s->size = i.size; s->is_dir = i.isDir; return 0;
   }
   ExfatInfo i;
   if (statExfat(&f->ev, path, &i) != 0) return -1;
   s->size = i.size; s->is_dir = i.isDir; return 0;
}
static int fs_open(Fs *f, const char *path) {
   return f->is_ntfs ? openNtfs(&f->nf, &f->nv, path) : openExfat(&f->ef, &f->ev, path);
}
static int fs_read(Fs *f, void *buf, int n) {
   return f->is_ntfs ? readNtfs(&f->nf, buf, n) : readExfat(&f->ef, buf, n);
}
static void fs_seek(Fs *f, uint64_t pos) {
   if (f->is_ntfs) seekNtfs(&f->nf, pos); else seekExfat(&f->ef, pos);
}
static void fs_close(Fs *f) { if (f->is_ntfs) closeNtfs(&f->nf); else closeExfat(&f->ef); }

// Lists a directory; calls cb(name, is_dir, ctx) per entry.  Returns the count, -1 on error.
static int fs_list(Fs *f, const char *path, void (*cb)(const char *, int, void *), void *ctx) {
   int n = 0, rc;
   char name[300];
   if (f->is_ntfs) {
      NtfsInfo i;
      if (statNtfs(&f->nv, path, &i) != 0 || !i.isDir) return -1;
      openNtfsDir(&f->nd, &f->nv, i.mftReference);
      while ((rc = readNtfsDir(&f->nd, name, sizeof name, &i)) == 1) {
         if (name[0] == '$' && name[1] != 0 && strcmp(path, "/") == 0) continue;   // $MFT, $Bitmap, ...
         cb(name, i.isDir, ctx); n++;
      }
      closeNtfsDir(&f->nd);
      return rc < 0 ? -1 : n;
   }
   ExfatInfo i;
   if (statExfat(&f->ev, path, &i) != 0 || !i.isDir) return -1;
   openExfatDir(&f->ed, &f->ev, i.firstCluster, i.noFatChain, i.size);
   while ((rc = readExfatDir(&f->ed, name, sizeof name, &i)) == 1) { cb(name, i.isDir, ctx); n++; }
   closeExfatDir(&f->ed);
   return n;
}

// ---- directory checks ---------------------------------------------------------
typedef struct { const char *want; int found; int dir; } Want;
static void find_cb(const char *name, int is_dir, void *ctx) {
   Want *w = (Want *)ctx;
   if (strcmp(name, w->want) == 0) { w->found++; w->dir = is_dir; }
}
static int has(Fs *f, const char *dirpath, const char *name, int want_dir) {
   Want w = { name, 0, -1 };
   if (fs_list(f, dirpath, find_cb, &w) < 0) return 0;
   return w.found == 1 && w.dir == want_dir;
}

static unsigned char s_seen[400];
static int s_many_bad;
static void many_cb(const char *name, int is_dir, void *ctx) {
   (void)ctx; (void)is_dir;
   int k;
   char tail;
   if (sscanf(name, "f%04d.tx%c", &k, &tail) == 2 && tail == 't' && k >= 0 && k < 400) s_seen[k]++;
   else s_many_bad++;
}

// ---- content checks -------------------------------------------------------------
static int read_all(Fs *f, uint64_t off, uint8_t *buf, int n) {
   fs_seek(f, off);
   int got = 0;
   while (got < n) {
      int r = fs_read(f, buf + got, n - got);
      if (r <= 0) return got ? got : r;
      got += r;
   }
   return got;
}

static int check_file(Fs *f, const Expect *e) {
   Stat st;
   if (fs_stat(f, e->path, &st) != 0 || st.is_dir || st.size != e->size) { printf("  stat %s\n", e->path); return 0; }
   if (fs_open(f, e->path) != 0) { printf("  open %s\n", e->path); return 0; }
   int ok = 1;
   uint8_t *want = malloc(e->size + 1), *got = malloc(e->size + 1);
   if (e->literal) memcpy(want, e->literal, e->size); else gen(e->seed, 0, want, e->size);

   // whole file, in 4097-byte reads (crosses sector and cluster edges at every phase)
   fs_seek(f, 0);
   uint64_t pos = 0;
   while (pos < e->size) {
      int want_n = e->size - pos < 4097 ? (int)(e->size - pos) : 4097;
      int r = fs_read(f, got + pos, want_n);
      if (r != want_n) { printf("  short read %s at %llu: %d\n", e->path, (unsigned long long)pos, r); ok = 0; break; }
      pos += (uint64_t)r;
   }
   if (ok && memcmp(got, want, e->size) != 0) { printf("  content differs %s\n", e->path); ok = 0; }
   // reading at the end gives nothing
   if (ok && fs_read(f, got, 10) != 0) { printf("  read past end %s\n", e->path); ok = 0; }

   // seeks: a spread of offsets, small and large reads, one byte at a time near a boundary
   static const uint64_t probes[] = { 0, 1, 511, 512, 513, 4095, 4096, 4097, 32767, 65536, 99999, 1048575 };
   for (size_t p = 0; ok && p < sizeof probes / sizeof probes[0]; p++) {
      if (probes[p] >= e->size) continue;
      int n = 7000;
      if (probes[p] + (uint64_t)n > e->size) n = (int)(e->size - probes[p]);
      if (read_all(f, probes[p], got, n) != n || memcmp(got, want + probes[p], n) != 0) {
         printf("  seek %llu %s\n", (unsigned long long)probes[p], e->path); ok = 0;
      }
   }
   for (int i = 0; ok && i < 3 && e->size > 8192; i++) {      // single bytes
      uint8_t b;
      if (read_all(f, 4090 + i * 3, &b, 1) != 1 || b != want[4090 + i * 3]) ok = 0;
   }
   fs_close(f);
   free(want); free(got);
   return ok;
}

static void test_fs(const char *image, int is_ntfs, uint64_t part_offset, const char *label) {
   printf("- %s\n", image);
   char path[256], cmd[600];
   snprintf(path, sizeof path, "/tmp/jf_%s.img", image);
   snprintf(cmd, sizeof cmd, "gzip -dc fixtures/fs/%s.img.gz > %s", image, path);
   if (system(cmd) != 0) { printf("FAIL cannot unpack %s\n", image); s_failed++; return; }
   g_dev_fd = open(path, O_RDONLY);
   g_dev_fail = 0;
   CHECK(g_dev_fd >= 0);
   static Fs fs;
   memset(&fs, 0, sizeof fs);
   fs.is_ntfs = is_ntfs;

   // the other format must refuse the volume without damaging anything
   {
      static NtfsVolume nv; static ExfatVolume ev;
      if (is_ntfs) CHECK(mountExfat(&ev, 0) == EXFAT_MOUNT_NOT_EXFAT);
      else         CHECK(mountNtfs(&nv, 0) == NTFS_MOUNT_NOT_NTFS);
   }

   CHECK(fs_mount(&fs) == 0);
   if (is_ntfs) {
      CHECK(fs.nv.mounted && fs.nv.partitionOffset == part_offset);
      CHECK(strcmp(fs.nv.label, label) == 0);
      CHECK(fs.nv.bytesPerSector == 512);
   } else {
      CHECK(fs.ev.mounted && fs.ev.partitionOffset == part_offset);
      CHECK(strcmp(fs.ev.label, label) == 0);
   }

   // free space is sane
   {
      uint64_t fr = 0, tot = 0;
      int rc = is_ntfs ? getNtfsFree(&fs.nv, &fr, &tot) : getExfatFree(&fs.ev, &fr, &tot);
      CHECK(rc == 0 && tot > 40 * MB && tot <= 64 * MB && fr > 0 && fr < tot);
   }

   // directories
   CHECK(has(&fs, "/", "hello.txt", 0));
   CHECK(has(&fs, "/", "Movies", 1));
   CHECK(has(&fs, "/", "Music", 1));
   CHECK(has(&fs, "/", "Unicode", 1));
   CHECK(has(&fs, "/", "many", 1));
   CHECK(!has(&fs, "/", "nothing", 0));
   CHECK(has(&fs, "/Movies", "Film One (2019).mkv", 0));
   CHECK(has(&fs, "/Movies", "sub", 1));
   CHECK(has(&fs, "/Movies/sub", "Deep.ts", 0));
   CHECK(has(&fs, "/Music/Album", "01 Track.flac", 0));
   CHECK(has(&fs, "/Unicode", "Caf\xC3\xA9 \xC3\x9Cn\xC3\xAF" "code.txt", 0));
   CHECK(has(&fs, "/Unicode", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 1));
   CHECK(has(&fs, "/Unicode", "emoji \xF0\x9F\x8E\xAC clip.mkv", 0));
   CHECK(fs_list(&fs, "/hello.txt", find_cb, 0) < 0);           // a file is not a directory
   CHECK(fs_list(&fs, "/no/such/dir", find_cb, 0) < 0);
   memset(s_seen, 0, sizeof s_seen); s_many_bad = 0;
   CHECK(fs_list(&fs, "/many", many_cb, 0) == 400);             // a directory across several blocks
   int all = 1;
   for (int i = 0; i < 400; i++) if (s_seen[i] != 1) all = 0;
   CHECK(all && s_many_bad == 0);
   CHECK(has(&fs, "/spacers", "s00.bin", 0));
   CHECK(!has(&fs, "/spacers", "s01.bin", 0));                  // freed in the fragmentation step

   // paths are matched the way the volume matches them (case-insensitively)
   {
      Stat st;
      CHECK(fs_stat(&fs, "/MOVIES/film one (2019).MKV", &st) == 0 && st.size == 3 * MB);
      CHECK(fs_stat(&fs, "/Movies/missing.mkv", &st) != 0);
      CHECK(fs_open(&fs, "/Movies") != 0);                      // a directory does not open as a file
      CHECK(fs_open(&fs, "/Movies/missing.mkv") != 0);
   }

   // contents
   for (int i = 0; i < NFILES; i++) CHECK(check_file(&fs, &FILES[i]));

   // a file bigger than 4 GB (NTFS keeps it sparse): size, a hole, and a written block at the end
   if (is_ntfs) {
      Stat st;
      CHECK(fs_stat(&fs, "/big/sparse5g.bin", &st) == 0 && st.size == BIG_SIZE);
      CHECK(fs_open(&fs, "/big/sparse5g.bin") == 0);
      static uint8_t buf[8192], want[8192], zero[8192];
      memset(zero, 0, sizeof zero);
      CHECK(read_all(&fs, 0, buf, 8192) == 8192 && memcmp(buf, zero, 8192) == 0);
      CHECK(read_all(&fs, 4ULL * 1024 * 1024 * 1024 + 123, buf, 8192) == 8192 && memcmp(buf, zero, 8192) == 0);
      gen(BIG_TAIL_SEED, 0, want, 4096);
      CHECK(read_all(&fs, BIG_TAIL_AT, buf, 4096) == 4096 && memcmp(buf, want, 4096) == 0);
      CHECK(read_all(&fs, BIG_SIZE - 10, buf, 100) == 10);      // reads stop at the end
      fs_close(&fs);
   }

   // the drive is pulled mid-use: reads fail, nothing hangs or crashes
   {
      CHECK(fs_open(&fs, "/Movies/Film One (2019).mkv") == 0);
      static uint8_t buf[4096];
      CHECK(read_all(&fs, 0, buf, 4096) == 4096);
      g_dev_fail = 1;
      CHECK(read_all(&fs, 2 * MB, buf, 4096) < 4096);
      Stat st;
      (void)fs_stat(&fs, "/hello.txt", &st);                    // may fail, must return
      g_dev_fail = 0;
      fs_close(&fs);
   }

   fs_unmount(&fs);
   close(g_dev_fd);
   g_dev_fd = -1;
   unlink(path);
}

int main(void) {
   test_fs("ntfs_flat",  1, 0,    "JFNTFS");
   test_fs("ntfs_mbr",   1, 2048, "JFNTFS");
   test_fs("ntfs_gpt",   1, 2048, "JFNTFS");
   test_fs("exfat_flat", 0, 0,    "JFEXFAT");
   test_fs("exfat_mbr",  0, 2048, "JFEXFAT");
   test_fs("exfat_gpt",  0, 2048, "JFEXFAT");
   printf("mohasi fs: %d checks, %d failed\n", s_checks, s_failed);
   return s_failed ? 1 : 0;
}
