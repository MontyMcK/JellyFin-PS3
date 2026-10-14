# mohasi_fs: NTFS (read and write) and exFAT (read only)

Vendored from **[mohasi/ps3-dev](https://github.com/mohasi/ps3-dev)**,
`libs/simple-lib-core/` (`src/ntfs.c`, `src/exfat.c`, `include/ntfs.h`,
`include/exfat.h`), commit recorded in `VERSION`. Licence **Apache-2.0** (the
`LICENSE` file here is upstream's). This project is GPL-3.0; the Apache-2.0
files keep their own licence and are included under its terms.

Hand-written drivers that work on the volume through the lv2 raw storage
syscalls: partition table (MBR and GPT), NTFS (MFT, runlists, `$I30` index,
`$ATTRIBUTE_LIST`, sparse and LZNT1 data; and upstream's write path: cluster
allocation, `$MFT` growth, index B-tree insert/remove/split, create, delete,
rename) and exFAT (entry sets, FAT chains, up-case table). Used by
`source/local/lfs.cpp`: to play files from USB drives, and to keep the offline
downloads on an NTFS drive.

## How ntfs.c is made

`ntfs.c` and `ntfs.h` are generated, not edited by hand:

    python3 port_ntfs.py <upstream ntfs.c> <upstream ntfs.h> third_party/mohasi_fs

applies every change below to upstream's files at the `VERSION` commit, and
fails if any of them no longer matches. Every change is marked `jf-port` in
the result. `exfat.c` was edited by hand (read only, see below).

## What was changed

* **exFAT is read only.** Its write path (about 1,700 lines) was deleted, not
  disabled. `make -f tests/Makefile.host check-mohasi-readonly` fails if a write
  call, `scCall` or `lv2syscall` appears in `exfat.c`, or a raw syscall in
  `ntfs.c` (which writes only through `jf_port.h`).
* **No VFS half.** Both files ended with a VFS backend (volume and handle
  pools, a mutex, registration). It is gone; `source/local/lfs.cpp` owns
  volumes, handles and the one lock these drivers need (they share static
  scratch buffers, so every call is serialised), and does what the VFS wrappers
  did around a write: a write open needs a clean mount, and the on-disk dirty
  flag comes down (`ntfsClearDirty`) once no writer is open.
* **Platform layer.** `jf_port.h` replaces `storage-device.h`, `syscall.h`,
  `thread.h`, `string-utilities.h`, `<sys/timer.h>` and `<cell/rtc.h>`: five
  storage functions (lv2 syscalls 600, 601, 602, 603 and 609 in
  `jf_port_ps3.c`; an image file in the host tests), a UTC clock, a sleep, and
  the string helpers copied unchanged from upstream. exFAT's scratch block is an
  ordinary `memalign` allocation instead of `sysMemAllocate`.

Fixes to upstream, each found by the tests below:

* **Unaligned storage I/O.** lv2's storage DMA wants 32-byte-aligned buffers;
  `growMft`'s record template and `allocateClusters`' bitmap chunk (among
  others) were stack arrays, so whether creating a file worked depended on the
  compiler's stack layout. Unaligned transfers now go through an aligned bounce
  buffer.
* **`$MFT` growth in 8-record steps** ran out of `$MFT` fragments (32 at most)
  after a few hundred files, and every create failed from then on. It grows by a
  quarter (at least 256 records).
* **One allocation cursor.** The record a download rewrites at every checkpoint
  landed right after the download's tail and broke it into a new fragment each
  time, until it hit the 64-fragment limit. Files growing past 4 MB now allocate
  as streams in their own region (from an eighth into the volume), reserving
  16 MB ahead (trimmed on close); everything else takes a cursor from the front.
  When the reserve does not fit, a write takes exactly what it needs.
* **Free-space count** went too high when an allocation failed part way.
* **`growMft` with clusters smaller than a record** wrote the second half of a
  new record over whatever cluster followed the first on the disk -- another
  file's data. It maps cluster by cluster.
* **Records bigger than a cluster** were written one cluster per call; now one
  call per physically contiguous stretch.
* **Delete order.** A file's clusters were freed before its record, ignoring
  both results: a drive pulled in between left an in-use record pointing at free
  clusters (which the next file would be given), and the delete returned
  success. Now: the base record, then its extension records, each before its
  clusters, and a failure is reported.
* **Clearing the dirty flag** writes `$MFTMirr`'s copy first, so a cut between
  the two copies leaves `$MFT` still saying dirty.
* **Mount gate.** A `$Volume` record that cannot be read, or has no volume
  information, mounts read only (the dirty flag is unknown).
* **Sparse data** (flag `0x8000`, compression unit 16 clusters) was decoded as
  LZNT1; the unit applies only to compressed attributes.

`lfs.cpp` adds its own rules on top: writes only on volumes whose clusters hold
a whole MFT record and index block (4 KB and up, Windows' default), never on a
volume Windows hibernated (`hiberfil.sys` starting `hibr`), and a write that
fails leaves the dirty flag up and the drive read only until it is replugged.

## Tests

* `tests/test_mohasi_fs.c`: images made with the Linux drivers
  (`tests/fixtures/fs/make_images.sh`) bare, in MBR and GPT partitions; listings,
  every file's content, a 5 GB sparse file, the drive cut mid-read. Host and
  big-endian under `qemu-ppc64`.
* `tests/fixtures/fs/ntfs_write_check.sh` (`quick`, `full`, `cut512`; `be` for
  big-endian): the write path driven the way the downloads drive it
  (`tests/test_ntfs_write.c`) on images made by `mkntfs`, judged by code that is
  not this one: `tests/tools/check_ntfs` (libntfs-3g: cross-linked clusters,
  used-but-free clusters, runlists, index entries, link counts) and `ntfscat`.
  Workloads on 512 B to 64 KB clusters and in an MBR partition, a 4.5 GiB file,
  filling the volume, and the drive pulled after N writes at 400 points across
  the whole workload: a volume left marked clean must check clean; one left
  dirty may only have what chkdsk repairs without loss.
* `tests/test_lfs.cpp`: the write rules above, through lfs.
