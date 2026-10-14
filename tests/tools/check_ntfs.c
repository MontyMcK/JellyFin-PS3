// check_ntfs: a read-only structural check of an NTFS image, built on libntfs-3g (the Linux
// driver), so the writer in third_party/mohasi_fs is judged by code that is not its own.
//
//   check_ntfs [-l] [-q] image [offset_bytes]
//
// What it checks (what chkdsk's first three stages look at, as far as libntfs-3g exposes it):
//   * every in-use base record opens, and every attribute (through $ATTRIBUTE_LIST too) walks;
//   * every non-resident attribute's runlist decodes, covers exactly [lowest_vcn, highest_vcn],
//     and the last extent ends at allocated_size; sizes are ordered (init <= data <= alloc);
//   * no cluster belongs to two attributes, and every cluster in a runlist is set in $Bitmap;
//     a set bit no runlist uses is a LEAK (an error unless -l: after a cut write it is expected);
//   * every directory index entry points at an in-use record with the same sequence number,
//     and every in-use user record is named by as many index entries as its link count says;
//   * the volume is not marked dirty (reported, not an error, with -l).
// -c (after a write sequence was cut off): when the volume is marked dirty, what chkdsk repairs
// without losing anything that was there before -- leaked clusters, orphan records, link counts --
// is reported but allowed; cross-links, used-but-free clusters, broken runlists and entries that
// point at nothing are still errors.  When it is NOT marked dirty, everything must be clean.
// Exit 0 when clean, 1 when anything is wrong, 2 when the image cannot be mounted at all.

#define _FILE_OFFSET_BITS 64
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#define HAVE_STDARG_H 1
#include <ntfs-3g/types.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/bitmap.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/mft.h>
#include <ntfs-3g/runlist.h>
#include <ntfs-3g/volume.h>


static int  g_errors = 0, g_quiet = 0, g_allow_leaks = 0, g_cut = 0, g_soft = 0;
static long g_leaks = 0;

static void fail(const char *fmt, ...)
{
   va_list ap;
   g_errors++;
   if (g_errors > 50) return;
   va_start(ap, fmt);
   fputs("check_ntfs: ", stdout);
   vprintf(fmt, ap);
   fputc('\n', stdout);
   va_end(ap);
}

static long g_soft_count = 0;
// a problem chkdsk repairs without loss: an error, except in -c mode on a dirty volume
static void soft(const char *fmt, ...)
{
   va_list ap;
   if (!g_soft) { char b[200]; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); fail("%s", b); return; }
   if (g_soft_count++ < 5 && !g_quiet) { va_start(ap, fmt); fputs("check_ntfs: (repairable) ", stdout); vprintf(fmt, ap); fputc('\n', stdout); va_end(ap); }
}

static uint8_t  *g_owned;      // one bit per cluster: claimed by a runlist
static uint64_t *g_owner;      // the record that claimed it (sparse use: only on conflict)
static int64_t   g_nclusters;
static uint32_t *g_links;      // index entries naming each record
static uint8_t  *g_mftbmp;
static int64_t   g_nrecords;

static int bit(const uint8_t *b, int64_t i) { return (b[i >> 3] >> (i & 7)) & 1; }

static void claim(ntfs_volume *vol, int64_t rec, int64_t lcn, int64_t len, const char *what)
{
   (void)vol;
   if (lcn < 0 || len <= 0) return;
   if (lcn + len > g_nclusters) { fail("record %lld %s: run %lld+%lld past the volume end", (long long)rec, what, (long long)lcn, (long long)len); return; }
   for (int64_t c = lcn; c < lcn + len; c++) {
      if (bit(g_owned, c)) {
         fail("cluster %lld claimed by record %lld (%s) and by record %llu", (long long)c, (long long)rec, what,
              (unsigned long long)g_owner[c]);
         return;
      }
      g_owned[c >> 3] |= (uint8_t)(1 << (c & 7));
      g_owner[c] = (uint64_t)rec;
   }
}

static void check_attrs(ntfs_volume *vol, ntfs_inode *ni)
{
   const int64_t rec = (int64_t)ni->mft_no;
   ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
   if (!ctx) { fail("record %lld: no search context", (long long)rec); return; }
   // per (type,name) the next vcn the following extent must start at
   struct { uint32_t type; int64_t next_vcn; int64_t alloc; } open[32];
   int nopen = 0;
   while (!ntfs_attrs_walk(ctx)) {
      ATTR_RECORD *a = ctx->attr;
      if (a->type == AT_END) break;
      if (!a->non_resident) continue;
      char what[48];
      snprintf(what, sizeof what, "attr 0x%x", (unsigned)le32_to_cpu(a->type));
      const int64_t lowest  = sle64_to_cpu(a->lowest_vcn);
      const int64_t highest = sle64_to_cpu(a->highest_vcn);
      if (lowest == 0) {
         const int64_t alloc = sle64_to_cpu(a->allocated_size), data = sle64_to_cpu(a->data_size),
                       init = sle64_to_cpu(a->initialized_size);
         if (!(init <= data && data <= alloc)) fail("record %lld %s: sizes out of order (init %lld data %lld alloc %lld)",
                                                    (long long)rec, what, (long long)init, (long long)data, (long long)alloc);
         if (alloc % vol->cluster_size) fail("record %lld %s: allocated size %lld not whole clusters", (long long)rec, what, (long long)alloc);
         if (nopen < 32) { open[nopen].type = le32_to_cpu(a->type); open[nopen].next_vcn = 0; open[nopen].alloc = alloc; nopen++; }
      }
      int slot = -1;
      for (int i = nopen - 1; i >= 0; i--) if (open[i].type == le32_to_cpu(a->type)) { slot = i; break; }
      if (slot < 0) { fail("record %lld %s: extent at vcn %lld with no first extent", (long long)rec, what, (long long)lowest); continue; }
      if (lowest != open[slot].next_vcn) fail("record %lld %s: extent starts at vcn %lld, expected %lld", (long long)rec, what,
                                              (long long)lowest, (long long)open[slot].next_vcn);
      runlist_element *rl = ntfs_mapping_pairs_decompress(vol, a, NULL);
      if (!rl) { fail("record %lld %s: runlist does not decode", (long long)rec, what); continue; }
      int64_t vcn_end = lowest;
      for (runlist_element *r = rl; r->length; r++) {
         if (r->vcn < lowest) continue;                       // the NOT_MAPPED lead-in before this extent
         if (r->vcn != vcn_end) fail("record %lld %s: runlist gap at vcn %lld", (long long)rec, what, (long long)r->vcn);
         if (r->lcn >= 0) claim(vol, rec, r->lcn, r->length, what);
         else if (r->lcn != LCN_HOLE) fail("record %lld %s: bad lcn %lld at vcn %lld", (long long)rec, what, (long long)r->lcn, (long long)r->vcn);
         vcn_end = r->vcn + r->length;
      }
      free(rl);
      if (vcn_end != highest + 1) fail("record %lld %s: runlist ends at vcn %lld, header says %lld", (long long)rec, what,
                                       (long long)vcn_end, (long long)(highest + 1));
      open[slot].next_vcn = highest + 1;
   }
   for (int i = 0; i < nopen; i++)
      if (open[i].next_vcn * (int64_t)vol->cluster_size != open[i].alloc)
         fail("record %lld attr 0x%x: extents cover %lld clusters, allocated size says %lld bytes", (long long)rec,
              open[i].type, (long long)open[i].next_vcn, (long long)open[i].alloc);
   ntfs_attr_put_search_ctx(ctx);
}

struct dirctx { ntfs_volume *vol; int64_t dir; };

static int on_entry(void *dirent, const ntfschar *name, const int name_len, const int name_type,
                    const s64 pos, const MFT_REF mref, const unsigned dt_type)
{
   (void)name; (void)dt_type;
   struct dirctx *d = (struct dirctx *)dirent;
   if (pos < 2) return 0;                                       // the "." and ".." ntfs_readdir makes up
   if (name_len == 1 && le16_to_cpu(name[0]) == '.') return 0;  // the root's own "." entry
   const int64_t r = (int64_t)MREF(mref);
   if (r >= g_nrecords || !bit(g_mftbmp, r)) { fail("dir %lld: entry -> record %lld, not in use", (long long)d->dir, (long long)r); return 0; }
   MFT_RECORD *m = NULL;
   if (ntfs_file_record_read(d->vol, r, &m, NULL)) { fail("dir %lld: entry -> record %lld unreadable", (long long)d->dir, (long long)r); return 0; }
   if (!(m->flags & MFT_RECORD_IN_USE)) fail("dir %lld: entry -> record %lld, record not in use", (long long)d->dir, (long long)r);
   if (MSEQNO(mref) && MSEQNO(mref) != le16_to_cpu(m->sequence_number))
      fail("dir %lld: entry -> record %lld seq %u, record seq %u", (long long)d->dir, (long long)r,
           (unsigned)MSEQNO(mref), (unsigned)le16_to_cpu(m->sequence_number));
   free(m);
   if (name_type != FILE_NAME_DOS) g_links[r]++;
   return 0;
}

int main(int argc, char **argv)
{
   int a = 1;
   for (; a < argc && argv[a][0] == '-'; a++) {
      if (!strcmp(argv[a], "-l")) g_allow_leaks = 1;
      else if (!strcmp(argv[a], "-q")) g_quiet = 1;
      else if (!strcmp(argv[a], "-c")) g_cut = 1;
   }
   if (a >= argc) { fprintf(stderr, "usage: check_ntfs [-l] [-q] image\n"); return 2; }
   ntfs_volume *vol = ntfs_mount(argv[a], NTFS_MNT_RDONLY | NTFS_MNT_RECOVER);
   if (!vol) vol = ntfs_mount(argv[a], NTFS_MNT_RDONLY);
   if (!vol) { printf("check_ntfs: %s does not mount\n", argv[a]); return 2; }

   g_nclusters = vol->nr_clusters;
   g_owned = calloc((size_t)(g_nclusters + 8) / 8, 1);
   g_owner = calloc((size_t)g_nclusters, sizeof *g_owner);
   g_nrecords = vol->mft_na->initialized_size >> vol->mft_record_size_bits;
   g_links = calloc((size_t)g_nrecords, sizeof *g_links);
   const int64_t mftbmp_len = vol->mftbmp_na->initialized_size;
   g_mftbmp = calloc((size_t)mftbmp_len + 8, 1);
   if (ntfs_attr_pread(vol->mftbmp_na, 0, mftbmp_len, g_mftbmp) != mftbmp_len) fail("$MFT bitmap unreadable");
   if (mftbmp_len * 8 < g_nrecords) g_nrecords = mftbmp_len * 8;

   if (g_cut && (vol->flags & VOLUME_IS_DIRTY)) { g_soft = 1; g_allow_leaks = 1; }
   if (vol->flags & VOLUME_IS_DIRTY) {
      if (g_allow_leaks) { if (!g_quiet) printf("check_ntfs: volume marked dirty (allowed)\n"); }
      else fail("volume marked dirty");
   }

   // pass 1: every in-use base record, its attributes and runlists; directories' entries
   long files = 0, dirs = 0;
   for (int64_t r = 0; r < g_nrecords; r++) {
      if (!bit(g_mftbmp, r)) continue;
      MFT_RECORD *m = NULL;
      if (ntfs_file_record_read(vol, r, &m, NULL)) { fail("record %lld (in use in $MFT bitmap) unreadable", (long long)r); continue; }
      const int in_use = (m->flags & MFT_RECORD_IN_USE) != 0;
      const int is_ext = MREF_LE(m->base_mft_record) != 0;
      const int is_dir = (m->flags & MFT_RECORD_IS_DIRECTORY) != 0;
      free(m);
      if (!in_use) { if (r >= 24) soft("record %lld: set in $MFT bitmap, record not in use", (long long)r); continue; }
      if (is_ext) continue;                                  // walked through its base's attribute list
      ntfs_inode *ni = ntfs_inode_open(vol, (MFT_REF)r);
      if (!ni) { fail("record %lld does not open", (long long)r); continue; }
      check_attrs(vol, ni);
      if (is_dir) {
         dirs++;
         struct dirctx d = { vol, r };
         s64 pos = 0;
         if (ntfs_readdir(ni, &pos, &d, on_entry)) fail("dir %lld: index walk failed", (long long)r);
      } else files++;
      ntfs_inode_close(ni);
   }
   // pass 2: link counts (user records only; the system files are linked by mkntfs' own rules)
   for (int64_t r = 24; r < g_nrecords; r++) {
      if (!bit(g_mftbmp, r)) continue;
      MFT_RECORD *m = NULL;
      if (ntfs_file_record_read(vol, r, &m, NULL)) continue;
      if ((m->flags & MFT_RECORD_IN_USE) && MREF_LE(m->base_mft_record) == 0) {
         const unsigned lc = le16_to_cpu(m->link_count);
         if (g_links[r] == 0) soft("record %lld: in use, named by no directory (orphan)", (long long)r);
         else if (g_links[r] != lc) soft("record %lld: link count %u, %u index entries", (long long)r, lc, g_links[r]);
      }
      free(m);
   }
   // pass 3: the cluster bitmap against what the runlists claimed
   const int64_t bmp_len = (g_nclusters + 7) / 8;
   uint8_t *lcnbmp = calloc((size_t)bmp_len + 8, 1);
   if (ntfs_attr_pread(vol->lcnbmp_na, 0, bmp_len, lcnbmp) != bmp_len) fail("$Bitmap unreadable");
   long missing = 0;
   for (int64_t c = 0; c < g_nclusters; c++) {
      const int used = bit(g_owned, c), marked = bit(lcnbmp, c);
      if (used && !marked) { if (missing++ < 5) fail("cluster %lld used by record %llu but free in $Bitmap", (long long)c, (unsigned long long)g_owner[c]); }
      else if (!used && marked) g_leaks++;
   }
   if (missing > 5) fail("... %ld clusters used but free in $Bitmap in all", missing);
   if (g_leaks) {
      if (g_allow_leaks) { if (!g_quiet) printf("check_ntfs: %ld clusters marked in $Bitmap but unused (allowed)\n", g_leaks); }
      else fail("%ld clusters marked in $Bitmap but used by nothing (leaked)", g_leaks);
   }
   if (!g_quiet || g_errors) printf("check_ntfs: %s: %ld files, %ld dirs, %lld clusters -- %s\n", argv[a], files, dirs,
                                    (long long)g_nclusters, g_errors ? "PROBLEMS" : "clean");
   ntfs_umount(vol, FALSE);
   return g_errors ? 1 : 0;
}
