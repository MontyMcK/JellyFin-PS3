// ntfs.c - NTFS reader for the PS3 file manager. See ntfs.h.
//
// Sibling of exfat.c: same lv2 storage I/O layer, same memory discipline (all
// scratch is static or stack, no libc, no malloc), same VFS backend wiring, the
// same single non-recursive backend mutex. Hand-written from the NTFS on-disk
// spec in docs/ntfs; the read-path parsing is cross-checked against libfsntfs.
//
// NTFS is little-endian on disk and the PPU is big-endian, so every multi-byte
// on-disk field is read through readLe16/readLe32/readLe64 - a packed on-disk
// struct is never overlaid on a C struct and read by field.
//
// Build stages (see docs/ntfs/NTFS-BUILD-LOG.md): S0 scaffold + primitives,
// S1 boot sector / probe are implemented; S2-S7 (MFT, attributes, runlists,
// file read, directory traversal, path resolution) are stubbed and wired.

#include "ntfs.h"
// jf-port: vendored from mohasi/ps3-dev (Apache-2.0); see README.md in this directory.
// Reads AND writes (the write path is upstream's, unchanged).  The VFS backend half is not
// vendored; source/local/lfs.cpp is the only caller and holds ONE lock around every call into
// this file, as the original's *Op wrappers did (the functions share static scratch buffers).
#include "jf_port.h"           // jf-port: storage (read AND write), clock, sleep and string helpers
                               // (replaces vfs/storage-device/syscall/thread/string-utilities/timer/rtc)

#define STORAGE_BUSY       0x80010002u   // lv2 "device not ready" (settling / ejected)
#define SYSIO_RETRY        8
#define SYSIO_RETRY_US     50000
#define SYSIO_SETTLE_US    62500         // settle gap after open, before the first read

#define STORAGE_ALIGN      32            // lv2 storage DMA buffer alignment
#define NTFS_MAX_SECTOR    4096          // largest sector we support
#define NTFS_MAX_RECORD    4096          // largest FILE/index record we buffer (>= mftRecordSize)
#define NTFS_READ_BOUNCE   16384         // sector-aligned bounce for non-resident file reads (16 KB/call)
#define MFT_REF_MASK       0x0000FFFFFFFFFFFFull   // low 48 bits of an 8-byte file reference = MFT index

// Partition-table constants (mirror exfat.c; MBR + GPT layout is filesystem-agnostic).
#define MBR_PART_TABLE     446
#define MBR_PART_ENTRIES   4
#define MBR_PART_SIZE      16
#define MBR_TYPE_GPT       0xEE
#define GPT_HEADER_LBA     1
#define GPT_MAX_ENTRIES    128
#define GPT_ENTRY_MIN_SIZE 128
// Finite ceiling for partition-table LBAs when the device size is unknown, so a hostile partition
// entry can't steer a scan read to an arbitrary 64-bit sector (see exfat.c for the rationale).
#define NTFS_SCAN_LBA_CAP  (1ull << 36)

// ===========================================================================
// NTFS on-disk offsets, transcribed from the spec (docs/ntfs). All fields are
// little-endian. Offsets cited so a reader can check them against the spec.
// ===========================================================================

// $Boot / BPB (volume header), 512 bytes. Spec: "The volume header".
#define BOOT_OEM_OFFSET            3    // 8 bytes, "NTFS    "
#define BOOT_BYTES_PER_SECTOR      11   // uint16
#define BOOT_SECTORS_PER_CLUSTER   13   // uint8, signed power-of-two encoding (see decodeClusterFactor)
#define BOOT_TOTAL_SECTORS         40   // uint64
#define BOOT_MFT_LCN               48   // uint64
#define BOOT_MFTMIRR_LCN           56   // uint64
#define BOOT_MFT_RECORD_FACTOR     64   // int8, signed power-of-two encoding (see decodeRecordBytes)
#define BOOT_INDEX_RECORD_FACTOR   68   // int8, same encoding
#define BOOT_VOLUME_SERIAL         72   // uint64
#define BOOT_SIGNATURE             510  // 0x55, 0xAA

// MFT FILE record header. Spec: "MFT entry header". Used from S2 on.
#define FILE_SIGNATURE_OFFSET      0    // "FILE" (0x46494C45 as bytes)
#define FILE_USA_OFFSET            4    // uint16: offset to update sequence array
#define FILE_USA_COUNT             6    // uint16: number of 16-bit words in the USA (placeholder + fixups)
#define FILE_SEQUENCE_NUMBER       16   // uint16
#define FILE_HARD_LINK_COUNT       18   // uint16
#define FILE_FIRST_ATTR_OFFSET     20   // uint16
#define FILE_FLAGS                 22   // uint16: 0x01 in-use, 0x02 directory
#define FILE_USED_SIZE             24   // uint32
#define FILE_LSN_OFFSET            8    // uint64: $LogFile sequence number (LSN) of the MFT entry (F034)
#define FILE_NEXT_ATTR_ID_OFFSET   40   // uint16: first available (next) attribute identifier (F042, v3.0)
#define FILE_HDR_UNKNOWN_42_OFFSET 42   // uint16: unknown (wfixupPattern) (F043 v3.0 / F045 v3.1)
#define FILE_HDR_UNKNOWN_44_OFFSET 44   // uint32: unknown (v3.0) / MFT entry index (v3.1) (F044)
#define FILE_HDR_V3_MIN            48   // through the @44 field (a v3.0+ MFT entry header)
#define FILE_ALLOCATED_SIZE        28   // uint32
#define FILE_BASE_REFERENCE        32   // uint64 (file reference)
#define FILE_FLAG_IN_USE           0x0001
#define FILE_FLAG_DIRECTORY        0x0002

// System file MFT record numbers (fixed by the spec).
#define MFT_RECORD_MFT             0
#define MFT_RECORD_MFTMIRR         1
#define MFT_RECORD_LOGFILE         2
#define MFT_RECORD_VOLUME          3
#define MFT_RECORD_ROOT            5
#define MFT_RECORD_BITMAP          6
#define MFT_RECORD_BOOT            7
#define MFT_RECORD_SECURE          9    // $Secure: central security descriptor store ($SDS/$SII/$SDH)
#define MFT_RECORD_ATTRDEF         4    // $AttrDef: attribute definitions (C005)
#define MFT_RECORD_BADCLUS         8    // $BadClus: bad clusters (C009)
#define MFT_RECORD_UPCASE          10
#define MFT_RECORD_EXTEND          11   // $Extend: directory of optional metadata files ($ObjId/$Quota/$Reparse/$UsnJrnl)

// Attribute record layout. Spec: "MFT attribute header". Common header is 16 bytes; resident and
// non-resident variants add their own fields after it. Used from S3 on.
#define ATTR_TYPE_OFFSET           0    // uint32: attribute type id (below)
#define ATTR_LENGTH_OFFSET         4    // uint32: length of this whole attribute (header + value)
#define ATTR_NON_RESIDENT          8    // uint8: 0 = resident, 1 = non-resident
#define ATTR_NAME_LENGTH           9    // uint8: attribute-name length in UTF-16 units (0 = unnamed)
#define ATTR_NAME_OFFSET           10   // uint16: offset to the name within the attribute
#define ATTR_FLAGS_OFFSET          12   // uint16: 0x0001 compressed, 0x4000 encrypted, 0x8000 sparse
#define ATTR_ID_OFFSET             14   // uint16: attribute id (unique within the record)
#define ATTR_RES_VALUE_LENGTH      16   // uint32 (resident): value length in bytes
#define ATTR_RES_VALUE_OFFSET      20   // uint16 (resident): offset to the value within the attribute
#define ATTR_NR_START_VCN          16   // uint64 (non-resident): first VCN this attribute maps
#define ATTR_NR_LAST_VCN           24   // uint64 (non-resident): last VCN this attribute maps
#define ATTR_NR_RUNLIST_OFFSET     32   // uint16 (non-resident): offset to the runlist (mapping pairs)
#define ATTR_NR_COMPRESSION_UNIT   34   // uint16 (non-resident): $LZNT1 compression-unit size (power of 2)
#define ATTR_NR_ALLOC_SIZE         40   // uint64 (non-resident): allocated size in bytes
#define ATTR_NR_REAL_SIZE          48   // uint64 (non-resident): real (data) size in bytes
#define ATTR_NR_VALID_SIZE         56   // uint64 (non-resident): valid/initialized size in bytes
#define ATTR_NR_HEADER_MIN         64   // smallest non-resident header (no compression-unit field)
#define ATTR_FLAG_COMPRESSED       0x0001
#define ATTR_FLAG_ENCRYPTED        0x4000
#define ATTR_FLAG_SPARSE           0x8000

// $FILE_NAME attribute (also the key inside a directory index entry). Spec: "The file name attribute".
#define FN_PARENT_REF       0     // uint64: parent directory file reference
#define FN_MODIFIED_TIME    16    // uint64 FILETIME: last data modification (used for mtime)
#define FN_ALLOC_SIZE       40    // uint64: allocated size (cluster-rounded; 0 for directories)
#define FN_REAL_SIZE        48    // uint64: real data size (accurate when read from a $I30 entry)
#define FN_FLAGS            56    // uint32: file attribute flags (0x10000000 = directory)
#define FN_NAME_LENGTH      64    // uint8: name length in UTF-16 units
#define FN_NAMESPACE        65    // uint8: 0 POSIX, 1 Win32, 2 DOS, 3 Win32+DOS
#define FN_NAME             66    // UTF-16LE name (FN_NAME_LENGTH units)
#define FN_MIN_SIZE         66    // bytes before the name
#define FN_FLAG_DIRECTORY   0x10000000u
#define FN_FLAG_REPARSE     0x00000400u   // FILE_ATTRIBUTE_REPARSE_POINT (W12b)
// DOS/Win32 FILE_ATTRIBUTE_* bits carried in the $FILE_NAME / $STANDARD_INFORMATION flags
// field (asciidoc L2624-L2650). Surfaced per-file via NtfsInfo.attributes / VfsStat.attributes.
#define FILE_ATTRIBUTE_READONLY            0x00000001u
#define FILE_ATTRIBUTE_HIDDEN              0x00000002u
#define FILE_ATTRIBUTE_SYSTEM              0x00000004u
#define FILE_ATTRIBUTE_ARCHIVE             0x00000020u
#define FILE_ATTRIBUTE_TEMPORARY           0x00000100u
#define FILE_ATTRIBUTE_SPARSE_FILE         0x00000200u
#define FILE_ATTRIBUTE_COMPRESSED          0x00000800u
#define FILE_ATTRIBUTE_OFFLINE             0x00001000u
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000u
#define FILE_ATTRIBUTE_ENCRYPTED           0x00004000u
// File-attribute flags not used by NTFS proper (FAT/legacy/view bits, asciidoc L2627-L2647).
#define FILE_ATTRIBUTE_VOLUME_LABEL        0x00000008u   // E161
#define FILE_ATTRIBUTE_DIRECTORY_FAT       0x00000010u   // E162 (FAT directory bit; NTFS uses the MFT flag)
#define FILE_ATTRIBUTE_DEVICE              0x00000040u   // E164
#define FILE_ATTRIBUTE_NORMAL              0x00000080u   // E165
#define FILE_ATTRIBUTE_UNKNOWN_8000        0x00008000u   // E173 (seen on Windows 95 FAT)
#define FILE_ATTRIBUTE_VIRTUAL             0x00010000u   // E174
#define FILE_ATTRIBUTE_DIRECTORY_I30       0x10000000u   // E175 (has $I30 index)
#define FILE_ATTRIBUTE_INDEX_VIEW          0x20000000u   // E176 (view index)
// Volume flags (asciidoc L1158-L1169), beyond the dirty bit defined below.
#define VOLUME_FLAG_RESIZE_LOGFILE         0x0002u  // E040
#define VOLUME_FLAG_UPGRADE_ON_MOUNT       0x0004u  // E041
#define VOLUME_FLAG_MOUNTED_NT4            0x0008u  // E042
#define VOLUME_FLAG_DELETE_USN_UNDERWAY    0x0010u  // E043
#define VOLUME_FLAG_REPAIR_OBJECT_IDS      0x0020u  // E044
#define VOLUME_FLAG_UNKNOWN_0080           0x0080u  // E045
#define VOLUME_FLAG_CHKDSK_UNDERWAY        0x4000u  // E046
#define VOLUME_FLAG_MODIFIED_BY_CHKDSK     0x8000u  // E047
// Collation types (asciidoc L1458-L1467).
#define COLLATION_BINARY                   0x00000000u  // E057 (first byte most significant)
#define COLLATION_UNICODE_CASE_SENSITIVE   0x00000002u  // E059
#define COLLATION_ULONG                    0x00000010u  // E060 (u32 LE)
#define COLLATION_SID                      0x00000011u  // E061
#define COLLATION_SECURITY_HASH            0x00000012u  // E062 (hash, then SID)
#define COLLATION_ULONGS                   0x00000013u  // E063 (array of u32 LE)
// MFT entry flags (beyond in-use 0x01 / directory 0x02).
#define MFT_FLAG_UNKNOWN_0004              0x0004u  // E008
#define MFT_FLAG_IS_INDEX                  0x0008u  // E009 (entry is a (view) index)
// Symbolic-link reparse flags (asciidoc L... ).
#define SYMLINK_FLAG_RELATIVE              0x00000001u  // E128 (substitute name is relative)
// Media descriptor byte bit-fields (asciidoc L422-L434): high nibble is always 0xF.
#define MEDIA_DESC_SIDES_BIT               0x01u  // E001 bit0: 0 single-sided, 1 double-sided
#define MEDIA_DESC_TRACK_SIZE_BIT          0x02u  // E002 bit1
#define MEDIA_DESC_DENSITY_BIT             0x04u  // E003 bit2
#define MEDIA_DESC_TYPE_BIT                0x08u  // E004 bit3: 0 fixed, 1 removable
#define MEDIA_DESC_HIGH_NIBBLE             0xF0u  // E005 bits4-7 always set to 1
#define FN_NAMESPACE_DOS    2     // DOS short-name twin: skipped so a long name isn't duplicated

// $INDEX_ROOT value: a 16-byte index-root header, then an index node header, then index entries.
#define IDXROOT_NODE_HEADER 16    // the index node header starts 16 bytes into the $INDEX_ROOT value
// Index node header (relative to the node header start):
#define IDXNODE_ENTRIES_OFFSET 0  // uint32: offset to the first entry (from the node header start)
#define IDXNODE_USED_SIZE      4  // uint32: bytes used (from the node header start)
#define IDXNODE_ALLOC_SIZE     8  // uint32: bytes allocated for the node (from the node header start)
#define IDXNODE_FLAGS_OFFSET   12 // uint32: node flags (bit 0 = has child nodes / large index)
// $INDEX_ALLOCATION block ("INDX" record): 24-byte header (sig + USA + LSN + VCN), then a node header.
#define INDX_NODE_HEADER       24
// Index entry:
#define IDXENTRY_FILE_REF      0  // uint64: file reference of the indexed file (0 in the end marker)
#define IDXENTRY_LENGTH        8  // uint16: length of this entry
#define IDXENTRY_KEY_LENGTH    10 // uint16: length of the key ($FILE_NAME)
#define IDXENTRY_FLAGS         12 // uint16: 0x01 has subnode, 0x02 last entry (no key)
#define IDXENTRY_FLAG_NODE     0x01 // entry has a child sub-node: last 8 bytes of the entry are its VCN
#define IDXENTRY_FLAG_LAST     0x02
#define IDXENTRY_KEY           16 // the $FILE_NAME key begins here
#define INDX_VCN_OFFSET        16 // uint64: VCN of this INDX block within $INDEX_ALLOCATION
#define NTFS_MAX_INDEX_DEPTH   24 // B-tree descent/split depth guard (a sane index is far shallower)
#define NTFS_MAX_INDEX_ENTRY   600 // ALIGN8(16 hdr + 8 child + 66 + 255*2 name): worst-case entry size
// VIEW index entry (non-$FILE_NAME, e.g. $SII/$SDH/$O): @0-7 is data offset/length/reserved instead
// of a file reference; the value (data) lives at entry+IDXENTRY_DATA_OFFSET. Common @8-13 as above.
#define IDXENTRY_DATA_OFFSET   0  // uint16: offset of the value within the entry
#define IDXENTRY_DATA_LENGTH   2  // uint16: length of the value

// $Secure security stores. $SII index value (asciidoc L2080-L2086): id@0, hash@4, id@8, $SDS offset@12, $SDS size@20.
#define SII_VAL_ID_OFFSET       0
#define SII_VAL_HASH_OFFSET     4
#define SII_VAL_ID2_OFFSET      8
#define SII_VAL_SDS_OFFSET      12   // uint64: offset into $SDS
#define SII_VAL_SDS_SIZE        20   // uint32: size in $SDS
#define SII_VAL_MIN             24
// $SDH index ($Secure:$SDH) — keyed by {hash(4), security_id(4)} (collation NTOFS_SECURITY_HASH).
#define SDH_KEY_HASH_OFFSET     0    // within the index-entry key
#define SDH_KEY_ID_OFFSET       4
#define SDH_KEY_LEN             8
// $SDH index value (asciidoc L2061-L2068): hash@0, id@4, hash@8, id@12, $SDS offset@16, size@24, padding@28.
#define SDH_VAL_HASH_OFFSET     0
#define SDH_VAL_ID_OFFSET       4
#define SDH_VAL_HASH2_OFFSET    8
#define SDH_VAL_ID2_OFFSET      12
#define SDH_VAL_SDS_OFFSET      16
#define SDH_VAL_SDS_SIZE        24
#define SDH_VAL_PAD_OFFSET      28
#define SDH_VAL_MIN             28
// $SDS data-stream entry header (L2094-L2098): hash@0, id@4, $SDS offset@12, size@20, descriptor@24.
#define SDS_HDR_HASH_OFFSET     0
#define SDS_HDR_ID_OFFSET       4
#define SDS_HDR_SDS_OFFSET      12   // uint64: this entry's own offset within $SDS
#define SDS_HDR_SIZE_OFFSET     20   // uint32: total entry size (header + descriptor)
#define SDS_HDR_DESC_OFFSET     24   // the SECURITY_DESCRIPTOR begins here
#define SDS_HDR_MIN             24

#define NTFS_FIRST_USER_RECORD 16 // records 0..15 are reserved system files ($MFT, $Volume, ...): hidden from listings

// Attribute type ids (used from S3 on).
#define ATTR_STANDARD_INFORMATION  0x10
#define ATTR_ATTRIBUTE_LIST        0x20
// $ATTRIBUTE_LIST entry layout (W8): the value is a packed list of these, sorted by (type,name,VCN).
#define AL_TYPE                    0    // uint32: attribute type of the referenced instance
#define AL_LENGTH                  4    // uint16: length of this list entry (8-aligned)
#define AL_NAME_LENGTH             6    // uint8: name length in UTF-16 units (0 = unnamed)
#define AL_NAME_OFFSET             7    // uint8: offset to the name within the entry (usually 0x1A)
#define AL_START_VCN               8    // uint64: first VCN this instance maps (0 = resident/first frag)
#define AL_MFT_REF                 0x10 // uint64: file reference of the record housing the instance
#define AL_ATTR_ID                 0x18 // uint16: the instance's attribute id within its housing record
#define AL_NAME                    0x1A // UTF-16LE name begins here
#define AL_MIN_ENTRY               0x1A // bytes before the name
#define NTFS_MAX_EXTENTS           16   // fragments of one attribute we gather across records; refuse beyond
#define JF_STREAM_BYTES (4ull << 20)   // jf-port: a file this big allocates as a stream (allocateClusters)
#define NTFS_MAX_FILE_RUNS         64   // runlist fragments we decode/re-encode for a record; refuse beyond
// W10a: a large $LZNT1 file's runlist has ~2 runs per compression unit (real clusters + a sparse hole),
// so a multi-MB file blows NTFS_MAX_FILE_RUNS. Compressed reads therefore map the runlist one compression
// unit at a time (mapVcnWindow) instead of decoding it whole — O(window) memory, any file size. A unit of
// cbClusters maps to at most cbClusters runs (each cluster its own run) plus clipping at each end.
#define NTFS_MAX_CB_CLUSTERS       64   // largest compression unit we map per read (real NTFS uses 16)
#define NTFS_CB_MAX_RUNS           (NTFS_MAX_CB_CLUSTERS + 2)
#define ATTR_FILE_NAME             0x30
#define ATTR_OBJECT_ID             0x40
#define ATTR_VOLUME_NAME           0x60
#define ATTR_VOLUME_INFORMATION    0x70
// $VOLUME_INFORMATION value: reserved(8) + major(1) + minor(1) + flags(2). Dirty bit forces chkdsk.
#define VOLINFO_MAJOR_OFFSET       8    // uint8: NTFS major version (1=NT, 2, 3=Win2000+)
#define VOLINFO_MINOR_OFFSET       9    // uint8: NTFS minor version (e.g. 3.1 -> major 3 minor 1)
#define VOLINFO_FLAGS_OFFSET       10
#define VOLUME_FLAG_DIRTY          0x0001
#define ATTR_DATA                  0x80
#define ATTR_INDEX_ROOT            0x90
#define ATTR_INDEX_ALLOCATION      0xA0
#define ATTR_BITMAP                0xB0
#define ATTR_REPARSE_POINT         0xC0
#define ATTR_EA_INFORMATION        0xD0   // (HPFS) extended attribute information (asciidoc L846)
#define ATTR_EA                    0xE0   // (HPFS) extended attribute (asciidoc L847)
#define ATTR_END                   0xFFFFFFFFu
// Attribute-type catalog stragglers (asciidoc L827-L853). Some are pre-v3.0 legacy types whose code was
// reused in v3.0 (0x40 was Volume version, now $OBJECT_ID; 0xC0 was Symbolic link, now $REPARSE_POINT).
#define ATTR_UNUSED                0x00       // E014: unused
#define ATTR_VOLUME_VERSION        0x40       // E018: volume version (removed in v3.0; code reused by $OBJECT_ID)
#define ATTR_SECURITY_DESCRIPTOR   0x50       // E020: (old per-file) security descriptor
#define ATTR_SYMBOLIC_LINK         0xC0       // E027: symbolic link (removed in v3.0; code reused by $REPARSE_POINT)
#define ATTR_PROPERTY_SET          0xF0       // E031: property set (removed in v3.0)
#define ATTR_LOGGED_UTILITY_STREAM 0x100      // E032: logged utility stream (introduced in v3.0; $TXF_DATA/$EFS)
#define ATTR_FIRST_USER_DEFINED    0x1000     // E033: first user-defined attribute type
#define ATTRIBUTE_FLAG_COMPRESSION_MASK 0x00FFu  // E011: compression-method mask in the attribute data flags
// $AttrDef record (asciidoc L1340-L1349): one 160-byte attribute-definition entry.
#define ATTRDEF_NAME_OFFSET        0    // UTF-16LE name (128 bytes, NUL-padded)
#define ATTRDEF_NAME_SIZE          128
#define ATTRDEF_TYPE_OFFSET        128  // uint32: attribute type code
#define ATTRDEF_UNKNOWN_OFFSET     132  // 8 bytes
#define ATTRDEF_FLAGS_OFFSET       140  // uint32: flags (seen 0x40/0x42/0x80)
#define ATTRDEF_MIN_SIZE_OFFSET    144  // uint64: minimum attribute size
#define ATTRDEF_MAX_SIZE_OFFSET    152  // uint64: maximum attribute size (-1 = no maximum)
#define ATTRDEF_ENTRY_SIZE         160
// One-off located fields read via ntfsReadField (offset within the relevant structure's value/record).
#define ATTR_RES_INDEXED_FLAG_OFFSET   22   // F060: resident attr indexed flag (abs; @6 relative)
#define ATTR_NR_TOTAL_ALLOC_OFFSET     64   // F070: non-resident total allocated size (compressed; abs, @48 rel)
#define FN_EXTENDED_DATA_OFFSET        60   // F103: $FILE_NAME extended data
#define VOLINFO_RESERVED_OFFSET        0    // F112: $VOLUME_INFORMATION reserved (unknown/empty)
#define UNITATTR_MODE_OFFSET           0    // F126: UNITATTR value (st_mode?)
#define IDXROOT_CLUSTER_BLOCKS_OFFSET  12   // F136: index-root header "number of cluster blocks"
#define INDX_LSN_OFFSET                8    // F140: INDX block header $LogFile LSN
#define RM_REPAIR_UNKNOWN0_OFFSET      0    // F240: resource-manager repair config unknown
#define RM_REPAIR_UNKNOWN4_OFFSET      4    // F241: resource-manager repair config unknown
// $STANDARD_INFORMATION v3.0+ extended fields (asciidoc L879-L897). The first 48 bytes (timestamps@0..31,
// flags@32, and the @36/@40/@44 fields) exist in v1.2; the @48.. fields were added in v3.0.
#define SI_MAX_VERSIONS_OFFSET     36   // uint32 (F081, meaning unknown)
#define SI_VERSION_OFFSET          40   // uint32 (F082, meaning unknown)
#define SI_CLASS_ID_OFFSET         44   // uint32 (F083, meaning unknown)
#define SI_OWNER_ID_OFFSET         48   // uint32 (F084, v3.0)
#define SI_SECURITY_ID_OFFSET      52   // uint32 ($Secure:$SII entry)
#define SI_QUOTA_CHARGED_OFFSET    56   // uint64 (F086)
#define SI_USN_OFFSET              64   // uint64 (F087)
#define SI_V3_MIN                  72   // through the USN field (a v3.0+ $STANDARD_INFORMATION)

// $REPARSE_POINT structure (asciidoc L1747-L1750): tag(4)@0, data size(2)@4, reserved(2)@6, data@8.
#define REPARSE_TAG_OFFSET         0
#define REPARSE_DATA_SIZE_OFFSET   4
#define REPARSE_RESERVED_OFFSET    6
#define REPARSE_DATA_OFFSET        8
// Reparse tag bit-fields (L1768-L1770) + flag bits (L1899-L1905): low 16 bits = type, high 4 bits = flags.
#define REPARSE_TAG_TYPE_MASK          0x0000FFFFu
#define REPARSE_TAG_FLAG_RESERVED      0x10000000u   // bit 28
#define REPARSE_TAG_FLAG_NAME_SURROGATE 0x20000000u  // bit 29 "is alias"
#define REPARSE_TAG_FLAG_HIGH_LATENCY  0x40000000u   // bit 30
#define REPARSE_TAG_FLAG_MICROSOFT     0x80000000u   // bit 31 Microsoft-defined tag
// Predefined Microsoft reparse tags we resolve a target for.
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003u   // junction / volume mount point
#define IO_REPARSE_TAG_SYMLINK     0xA000000Cu
// Predefined reparse point tag catalog (asciidoc L1780-L1871). We do not resolve a target for these (only
// MOUNT_POINT/SYMLINK above are decoded); the catalog lets a consumer classify any tag it encounters.
#define IO_REPARSE_TAG_RESERVED_ZERO     0x00000000u
#define IO_REPARSE_TAG_RESERVED_ONE      0x00000001u
#define IO_REPARSE_TAG_RESERVED_TWO      0x00000002u
#define IO_REPARSE_TAG_DRIVE_EXTENDER    0x80000005u
#define IO_REPARSE_TAG_HSM2              0x80000006u
#define IO_REPARSE_TAG_SIS               0x80000007u
#define IO_REPARSE_TAG_WIM               0x80000008u
#define IO_REPARSE_TAG_CSV               0x80000009u
#define IO_REPARSE_TAG_DFS               0x8000000Au
#define IO_REPARSE_TAG_FILTER_MANAGER    0x8000000Bu
#define IO_REPARSE_TAG_DFSR              0x80000012u
#define IO_REPARSE_TAG_DEDUP             0x80000013u
#define IO_REPARSE_TAG_NFS               0x80000014u
#define IO_REPARSE_TAG_FILE_PLACEHOLDER  0x80000015u
#define IO_REPARSE_TAG_DFM               0x80000016u
#define IO_REPARSE_TAG_WOF               0x80000017u
#define IO_REPARSE_TAG_WCI               0x80000018u
#define IO_REPARSE_TAG_APPEXECLINK       0x8000001Bu
#define IO_REPARSE_TAG_STORAGE_SYNC      0x8000001Eu
#define IO_REPARSE_TAG_UNHANDLED         0x80000020u
#define IO_REPARSE_TAG_ONEDRIVE          0x80000021u
#define IO_REPARSE_TAG_AF_UNIX           0x80000023u
#define IO_REPARSE_TAG_LX_FIFO           0x80000024u
#define IO_REPARSE_TAG_LX_CHR            0x80000025u
#define IO_REPARSE_TAG_LX_BLK            0x80000036u
#define IO_REPARSE_TAG_PROJFS            0x9000001Cu
#define IO_REPARSE_TAG_WCI_1             0x90001018u
#define IO_REPARSE_TAG_CLOUD_1           0x9000101Au
#define IO_REPARSE_TAG_CLOUD_2           0x9000201Au
#define IO_REPARSE_TAG_CLOUD_3           0x9000301Au
#define IO_REPARSE_TAG_CLOUD_4           0x9000401Au
#define IO_REPARSE_TAG_CLOUD_5           0x9000501Au
#define IO_REPARSE_TAG_CLOUD_6           0x9000601Au
#define IO_REPARSE_TAG_CLOUD_7           0x9000701Au
#define IO_REPARSE_TAG_CLOUD_8           0x9000801Au
#define IO_REPARSE_TAG_CLOUD_9           0x9000901Au
#define IO_REPARSE_TAG_CLOUD_A           0x9000A01Au
#define IO_REPARSE_TAG_CLOUD_B           0x9000B01Au
#define IO_REPARSE_TAG_CLOUD_C           0x9000C01Au
#define IO_REPARSE_TAG_CLOUD_D           0x9000D01Au
#define IO_REPARSE_TAG_CLOUD_E           0x9000E01Au
#define IO_REPARSE_TAG_CLOUD_F           0x9000F01Au
#define IO_REPARSE_TAG_IIS_CACHE         0xA0000010u
#define IO_REPARSE_TAG_GLOBAL_REPARSE    0xA0000019u
#define IO_REPARSE_TAG_CLOUD             0xA000001Au
#define IO_REPARSE_TAG_LX_SYMLINK        0xA000001Du
#define IO_REPARSE_TAG_WCI_TOMBSTONE     0xA000001Fu
#define IO_REPARSE_TAG_PROJFS_TOMBSTONE  0xA0000022u
#define IO_REPARSE_TAG_WCI_LINK          0xA0000027u
#define IO_REPARSE_TAG_WCI_LINK_1        0xA0001027u
#define IO_REPARSE_TAG_HSM               0xC0000004u
#define IO_REPARSE_TAG_APPXSTRM          0xC0000014u
// symlink/junction reparse data header (relative to REPARSE_DATA_OFFSET):
#define RP_SUBST_NAME_OFFSET   0   // uint16: substitute-name offset, relative to the name buffer
#define RP_SUBST_NAME_SIZE     2   // uint16: substitute-name size in bytes (no end-of-string)
#define RP_PRINT_NAME_OFFSET   4   // uint16: print-name offset
#define RP_PRINT_NAME_SIZE     6   // uint16: print-name size in bytes
#define RP_JUNCTION_NAME_BASE  8   // junction: name buffer begins 8 bytes into the reparse data
#define RP_SYMLINK_FLAGS_OFFSET 8  // symlink: 4-byte flags field
#define RP_SYMLINK_NAME_BASE  12   // symlink: name buffer begins after the flags
#define REPARSE_MAX_READ      4096 // bounded read for a non-resident $REPARSE_POINT (covers any path target)
// $OBJECT_ID attribute (asciidoc L1098-L1104): four 16-byte GUIDs (CDomainRelativeObjId).
#define OBJID_DROID_FILE_OFFSET    0    // the object's own object_id GUID
#define OBJID_BIRTH_VOLUME_OFFSET  16
#define OBJID_BIRTH_FILE_OFFSET    32
#define OBJID_BIRTH_DOMAIN_OFFSET  48
#define GUID_SIZE                  16
#define OBJID_MAX                  64   // all four GUIDs (the attribute may carry just the first 16)
// $EA_INFORMATION attribute (asciidoc L1250-L1253): 8-byte header.
#define EAINFO_ENTRY_SIZE_OFFSET   0    // uint16: size of (the packed) extended attribute entry
#define EAINFO_NEED_EA_COUNT_OFFSET 2   // uint16: number of EAs that have NEED_EA set
#define EAINFO_DATA_SIZE_OFFSET    4    // uint32: size of the $EA data
#define EAINFO_SIZE                8
// $EA attribute (asciidoc L1271-L1281): a chain of variable-length entries.
#define EA_NEXT_OFFSET_OFFSET      0    // uint32: offset (from start of $EA data) to the next entry; 0 = last
#define EA_FLAGS_OFFSET            4    // uint8: flags
#define EA_NAME_LENGTH_OFFSET      5    // uint8: EA name length in ASCII chars (no NUL)
#define EA_VALUE_SIZE_OFFSET       6    // uint16: value-data size
#define EA_NAME_OFFSET             8    // ASCII name begins here; value follows the NUL after the name
#define EA_FLAG_NEED_EA            0x80 // NEED_EA: the EA must be understood for correct operation
#define EA_HEADER_MIN              8    // fixed header bytes before the name
#define EA_MAX_READ                8192 // bounded prefix scanned for a non-resident $EA chain
// USN change-journal metadata ($UsnJrnl:$Max, asciidoc L2267-L2273): four uint64 fields.
#define USNMAX_MAX_SIZE_OFFSET     0    // maximum journal size in bytes
#define USNMAX_ALLOC_DELTA_OFFSET  8    // allocation delta in bytes
#define USNMAX_JOURNAL_ID_OFFSET   16   // journal identifier (a FILETIME)
#define USNMAX_UNKNOWN_OFFSET      24   // unknown (empty)
#define USN_MAX_MIN                32
// USN change-journal entry / USN_RECORD_V2 ($UsnJrnl:$J, asciidoc L2297-L2320).
#define USN_REC_LENGTH_OFFSET      0    // uint32: entry (record) size
#define USN_REC_MAJOR_OFFSET       4    // uint16: major version (2)
#define USN_REC_MINOR_OFFSET       6    // uint16: minor version (0)
#define USN_REC_FILE_REF_OFFSET    8    // uint64: file reference
#define USN_REC_PARENT_REF_OFFSET  16   // uint64: parent file reference
#define USN_REC_USN_OFFSET         24   // uint64: USN (file offset of this entry)
#define USN_REC_TIMESTAMP_OFFSET   32   // uint64: update date/time (FILETIME)
#define USN_REC_REASON_OFFSET      40   // uint32: update reason flags
#define USN_REC_SOURCE_OFFSET      44   // uint32: update source flags
#define USN_REC_SECURITY_ID_OFFSET 48   // uint32: security-descriptor id ($Secure:$SII entry)
#define USN_REC_FILE_ATTRS_OFFSET  52   // uint32: file attribute flags
#define USN_REC_NAME_SIZE_OFFSET   56   // uint16: name byte size
#define USN_REC_NAME_OFFSET_OFFSET 58   // uint16: name offset (from start of the entry)
#define USN_REC_NAME_BASE          60   // the name buffer begins here in a V2 record
#define USN_REC_MIN                60   // smallest V2 record (no name)
// Update reason flags (asciidoc L2328-L2359).
#define USN_REASON_DATA_OVERWRITE        0x00000001u
#define USN_REASON_DATA_EXTEND           0x00000002u
#define USN_REASON_DATA_TRUNCATION       0x00000004u
#define USN_REASON_NAMED_DATA_OVERWRITE  0x00000010u
#define USN_REASON_NAMED_DATA_EXTEND     0x00000020u
#define USN_REASON_NAMED_DATA_TRUNCATION 0x00000040u
#define USN_REASON_FILE_CREATE           0x00000100u
#define USN_REASON_FILE_DELETE           0x00000200u
#define USN_REASON_EA_CHANGE             0x00000400u
#define USN_REASON_SECURITY_CHANGE       0x00000800u
#define USN_REASON_RENAME_OLD_NAME       0x00001000u
#define USN_REASON_RENAME_NEW_NAME       0x00002000u
#define USN_REASON_INDEXABLE_CHANGE      0x00004000u
#define USN_REASON_BASIC_INFO_CHANGE     0x00008000u
#define USN_REASON_HARD_LINK_CHANGE      0x00010000u
#define USN_REASON_COMPRESSION_CHANGE    0x00020000u
#define USN_REASON_ENCRYPTION_CHANGE     0x00040000u
#define USN_REASON_OBJECT_ID_CHANGE      0x00080000u
#define USN_REASON_REPARSE_POINT_CHANGE  0x00100000u
#define USN_REASON_STREAM_CHANGE         0x00200000u
#define USN_REASON_TRANSACTED_CHANGE     0x00400000u
#define USN_REASON_CLOSE                 0x80000000u
// Update source flags (asciidoc L2367-L2369).
#define USN_SOURCE_DATA_MANAGEMENT       0x00000001u
#define USN_SOURCE_AUXILIARY_DATA        0x00000002u
#define USN_SOURCE_REPLICATION_MANAGEMENT 0x00000004u
// $LogFile restart page header / RESTART_PAGE_HEADER (asciidoc L2160-L2178). Begins with a
// MULTI_SECTOR_HEADER (signature + USA), like FILE/INDX records.
#define LOG_RSTR_SIG_OFFSET            0    // 4-byte signature: "RSTR" / "RCRD" / "CHKD"
#define LOG_RSTR_USA_OFFSET_OFFSET     4    // uint16: update-sequence-array offset
#define LOG_RSTR_USA_COUNT_OFFSET      6    // uint16: number of fix-up values
#define LOG_RSTR_CHKDSK_LSN_OFFSET     8    // uint64: checkdisk last LSN
#define LOG_RSTR_SYS_PAGE_SIZE_OFFSET  16   // uint32: system page size
#define LOG_RSTR_LOG_PAGE_SIZE_OFFSET  20   // uint32: log page size
#define LOG_RSTR_RESTART_OFFSET_OFFSET 24   // uint16: restart offset
#define LOG_RSTR_MINOR_VER_OFFSET      26   // uint16: minor format version
#define LOG_RSTR_MAJOR_VER_OFFSET      28   // uint16: major format version (-1 beta / 0 transition / 1 USA support)
#define LOG_RSTR_MIN                   30   // through the major-version field
// $LogFile record header / LFS_RECORD_HEADER LSN triplet (asciidoc L2191-L2195).
#define LFS_REC_THIS_LSN_OFFSET        0    // uint64: this record's LSN
#define LFS_REC_PREV_LSN_OFFSET        8    // uint64: previous LSN
#define LFS_REC_UNDO_NEXT_LSN_OFFSET   16   // uint64: undo-next LSN
#define LFS_REC_MIN                    24
// $ObjID:$O index (asciidoc L2113-L2122): key = object_id GUID@0(16); value = file ref@4(8),
// birth volume id@12(16), birth file id@28(16), birth domain id@44(16).
#define OBJO_GUID_SIZE                 16
#define OBJO_VAL_FILE_REF_OFFSET       4
#define OBJO_VAL_BIRTH_VOLUME_OFFSET   12
#define OBJO_VAL_BIRTH_FILE_OFFSET     28
#define OBJO_VAL_BIRTH_DOMAIN_OFFSET   44
#define OBJO_VAL_MIN                   60   // through the birth-domain GUID
// TxF Old Page Stream (TOPS) metadata ($Tops unnamed $DATA, asciidoc L2479-L2492): 100 bytes.
#define TOPS_UNKNOWN0_OFFSET           0    // uint16, observed 0x000a
#define TOPS_SIZE_OFFSET               2    // uint16: size of TOPS metadata (0x0064 = 100)
#define TOPS_RM_COUNT_OFFSET           4    // uint32, observed 0x0001 (resource managers/streams?)
#define TOPS_RM_GUID_OFFSET            8    // resource-manager identifier (GUID, 16)
#define TOPS_UNKNOWN24_OFFSET          24   // uint64 (empty)
#define TOPS_BASE_LSN_OFFSET           32   // uint64: base/log-start LSN of the TxFLog stream
#define TOPS_UNKNOWN40_OFFSET          40   // uint64
#define TOPS_LAST_FLUSHED_LSN_OFFSET   48   // uint64: last flushed LSN of the TxFLog stream
#define TOPS_UNKNOWN56_OFFSET          56   // uint64
#define TOPS_UNKNOWN64_OFFSET          64   // uint64 (empty)
#define TOPS_RESTART_LSN_OFFSET        72   // uint64: restart LSN?
#define TOPS_UNKNOWN80_OFFSET          80   // 20 bytes
#define TOPS_META_SIZE                 100  // 0x0064
// $TXF_DATA logged utility stream attribute (asciidoc L2597-L2609).
#define TXF_REMNANT_OFFSET             0    // 6 bytes, remnant data
#define TXF_RM_ROOT_REF_OFFSET         6    // uint64: resource-manager root file reference (an MFT ref)
#define TXF_USN_INDEX_OFFSET           14   // uint64: USN index?
#define TXF_TXID_OFFSET                22   // uint64: TxF file identifier (TxID)
#define TXF_DATA_LSN_OFFSET            30   // uint64: data LSN (CLFS LSN of file-data tx records)
#define TXF_METADATA_LSN_OFFSET        38   // uint64: metadata LSN
#define TXF_DIR_INDEX_LSN_OFFSET       46   // uint64: directory-index LSN
#define TXF_FLAGS_OFFSET               54   // uint16: flags (seen 0x0000 / 0x0002)
#define TXF_DATA_MIN                   56   // through the flags field
// Windows Overlay Filter (WOF) reparse data (asciidoc L1985-L1989): four uint32 fields, 16 bytes.
#define WOF_VERSION_OFFSET             0    // WOF version (observed 1)
#define WOF_PROVIDER_OFFSET            4    // WOF provider (observed 2)
#define WOF_FILEINFO_VERSION_OFFSET    8    // file-information version (observed 1)
#define WOF_COMPRESSION_METHOD_OFFSET  12   // compression method (enum below)
#define WOF_REPARSE_MIN                16
// WOF compression method values (asciidoc L1998-L2001).
#define WOF_COMPRESSION_LZXPRESS_4K    0    // LZXPRESS Huffman, 4k window
#define WOF_COMPRESSION_LZX_32K        1    // LZX, 32k window
#define WOF_COMPRESSION_LZXPRESS_8K    2    // LZXPRESS Huffman, 8k window
#define WOF_COMPRESSION_LZXPRESS_16K   3    // LZXPRESS Huffman, 16k window
// WOF compressed data: an array of 32- or 64-bit chunk offsets (relative to the data-chunk region), then
// the chunks themselves (asciidoc, F155-F156). The offset array begins at the start of the compressed data.
#define WOF_CHUNK_OFFSETS_OFFSET       0
// Windows Container Isolation (WCI) reparse data (asciidoc L2-series, F181-F185).
#define WCI_VERSION_OFFSET             0    // uint32: version
#define WCI_RESERVED_OFFSET            4    // uint32: reserved
#define WCI_LOOKUP_GUID_OFFSET         8    // look-up identifier (GUID, 16)
#define WCI_NAME_SIZE_OFFSET           24   // uint16: name size in bytes
#define WCI_NAME_OFFSET                26   // UTF-16LE name (no terminator)
#define WCI_REPARSE_MIN                26   // through the name-size field (name may be empty)

// jf-port: lv2's storage DMA wants a 32-byte-aligned buffer, and several callers below pass a stack
// array (growMft's record template, allocateClusters' bitmap chunk, ...).  Upstream sent those
// straight to the syscall, so whether an operation worked depended on the compiler's stack layout.
// Here an unaligned buffer goes through ioBounce instead, in pieces; aligned ones are unchanged.
#define IO_BOUNCE_BYTES 16384
static uint8_t ioBounce[IO_BOUNCE_BYTES] __attribute__((aligned(32)));

// One storage call, retried while the device reports "not ready". `aligned` must be aligned.
static int readSectorsRaw(int storageHandle, uint64_t lba, uint32_t count, void *aligned)
{
   for (int attempt = 0; attempt < SYSIO_RETRY; attempt++) {
      uint32_t got = 0;
      int rc = readStorageRaw(storageHandle, lba, count, aligned, &got);
      if (rc == 0 && got == count) return 0;
      if ((uint32_t)rc != STORAGE_BUSY) return -1;
      sys_timer_usleep(SYSIO_RETRY_US);
   }
   return -1;
}

static int writeSectorsRaw(int storageHandle, uint64_t lba, uint32_t count, const void *aligned)
{
   for (int attempt = 0; attempt < SYSIO_RETRY; attempt++) {
      uint32_t put = 0;
      int rc = writeStorageRaw(storageHandle, lba, count, aligned, &put);
      if (rc == 0 && put == count) return 0;
      if ((uint32_t)rc != STORAGE_BUSY) return -1;
      sys_timer_usleep(SYSIO_RETRY_US);
   }
   return -1;
}

// The device sector size behind each open storage handle (mountNtfs registers it right after the
// open), for splitting a bounced transfer.  Every supported size (512..4096) divides IO_BOUNCE_BYTES.
static struct { int handle; uint32_t sectorSize; } ioHandles[8];
static int ioHandleNext = 0;

static void ioRegisterHandle(int handle, uint32_t sectorSize)
{
   for (int i = 0; i < 8; i++)
      if (ioHandles[i].sectorSize && ioHandles[i].handle == handle) { ioHandles[i].sectorSize = sectorSize; return; }
   ioHandles[ioHandleNext].handle = handle;
   ioHandles[ioHandleNext].sectorSize = sectorSize;
   ioHandleNext = (ioHandleNext + 1) % 8;
}

static uint32_t ioSectorSizeOf(int handle)   // 0 = unknown: the bounce path refuses
{
   for (int i = 0; i < 8; i++)
      if (ioHandles[i].sectorSize && ioHandles[i].handle == handle) return ioHandles[i].sectorSize;
   return 0;
}

// Reads `count` sectors at `lba`, retrying while the device reports "not ready" (hotplug
// settling). Returns 0 on success, -1 on a hard error.
static int readSectors(int storageHandle, uint64_t lba, uint32_t count, void *buffer)
{
   if (((uintptr_t)buffer & 31) == 0) return readSectorsRaw(storageHandle, lba, count, buffer);
   const uint32_t ioSectorSize = ioSectorSizeOf(storageHandle);
   if (ioSectorSize == 0 || ioSectorSize > IO_BOUNCE_BYTES) return -1;
   const uint32_t per = IO_BOUNCE_BYTES / ioSectorSize;
   uint8_t *out = (uint8_t *)buffer;
   while (count) {
      uint32_t n = count < per ? count : per;
      if (readSectorsRaw(storageHandle, lba, n, ioBounce) != 0) return -1;
      memCopy(out, ioBounce, (int)(n * ioSectorSize));
      out += n * ioSectorSize; lba += n; count -= n;
   }
   return 0;
}

// Writes `count` sectors at `lba`, retrying while the device reports "not ready". Returns 0 on
// success, -1 on a hard error. Mirror of readSectors.
static int writeSectors(int storageHandle, uint64_t lba, uint32_t count, const void *buffer)
{
   if (((uintptr_t)buffer & 31) == 0) return writeSectorsRaw(storageHandle, lba, count, buffer);
   const uint32_t ioSectorSize = ioSectorSizeOf(storageHandle);
   if (ioSectorSize == 0 || ioSectorSize > IO_BOUNCE_BYTES) return -1;
   const uint32_t per = IO_BOUNCE_BYTES / ioSectorSize;
   const uint8_t *in = (const uint8_t *)buffer;
   while (count) {
      uint32_t n = count < per ? count : per;
      memCopy(ioBounce, in, (int)(n * ioSectorSize));
      if (writeSectorsRaw(storageHandle, lba, n, ioBounce) != 0) return -1;
      in += n * ioSectorSize; lba += n; count -= n;
   }
   return 0;
}

// True if `p` meets the lv2 storage DMA alignment, so whole sectors can be read/written in place
// without bouncing through an aligned scratch buffer (mirror of exfat.c).
static int isDmaAligned(const void *p) { return ((uintptr_t)p & (STORAGE_ALIGN - 1)) == 0; }

// ===========================================================================
// Little-endian readers (NTFS is LE on disk, the PPU is big-endian). Never read
// a multi-byte on-disk field any other way.
// ===========================================================================
static uint16_t readLe16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t readLe32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t readLe64(const uint8_t *p) { return (uint64_t)readLe32(p) | ((uint64_t)readLe32(p + 4) << 32); }

// little-endian writers for the write path.
static void writeLe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void writeLe32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void writeLe64(uint8_t *p, uint64_t v) { writeLe32(p, (uint32_t)v); writeLe32(p + 4, (uint32_t)(v >> 32)); }

// overlap-safe byte move (memCopy is forward-only; growing a record shifts bytes right over themselves).
static void memMove(uint8_t *dst, const uint8_t *src, int n)
{
   if (dst < src) { for (int i = 0; i < n; i++) dst[i] = src[i]; }
   else           { for (int i = n - 1; i >= 0; i--) dst[i] = src[i]; }
}

// ===========================================================================
// Shared, 32-byte-aligned scratch. Only touched while the (single) caller holds
// ntfsLock, so these are safe to share across volumes/handles. Per-mount epoch
// keys any future cache the way exfat.c does (lv2 recycles storageHandle).
// ===========================================================================
static uint32_t mountEpoch;   // last epoch handed out (first mount gets 1; wraps skip 0)

static uint8_t  bootSector[NTFS_MAX_SECTOR] __attribute__((aligned(STORAGE_ALIGN)));
// Two one-sector scratch buffers for the partition scan: scanScratch reads GPT header/entry array,
// vbrScratch reads partition candidates (kept separate so the entry array isn't clobbered mid-walk).
static uint8_t  scanScratch[NTFS_MAX_SECTOR] __attribute__((aligned(STORAGE_ALIGN)));
static uint8_t  vbrScratch[NTFS_MAX_SECTOR]  __attribute__((aligned(STORAGE_ALIGN)));
// Working buffer for one FILE record (bootstrap MFT reads, attribute walks). Shared under ntfsLock.
static uint8_t  mftRecord[NTFS_MAX_RECORD]   __attribute__((aligned(STORAGE_ALIGN)));
// Sector-aligned bounce for non-resident file data reads (read whole sectors, copy the slice out).
static uint8_t  fileBounce[NTFS_READ_BOUNCE] __attribute__((aligned(STORAGE_ALIGN)));
// Dedicated sector buffer for cluster-$Bitmap I/O, so allocation never reuses mftRecord (which would
// force re-reading the file's and $Bitmap's MFT records every block). Shared under ntfsLock.
static uint8_t  bitmapScratch[NTFS_MAX_SECTOR] __attribute__((aligned(STORAGE_ALIGN)));
// One $INDEX_ALLOCATION block ("INDX" record) buffer for directory enumeration. Shared under ntfsLock.
static uint8_t  indexBuffer[NTFS_MAX_RECORD]  __attribute__((aligned(STORAGE_ALIGN)));
// W6 (index B-tree growth) scratch: the directory FILE record kept stable across cluster/bitmap ops
// (which clobber mftRecord), plus two INDX node buffers so a parent + child + new-right sibling can be
// held during a split (the third sibling reuses indexBuffer).
static uint8_t  dirRecord[NTFS_MAX_RECORD]   __attribute__((aligned(STORAGE_ALIGN)));
static uint8_t  indexNodeA[NTFS_MAX_RECORD]  __attribute__((aligned(STORAGE_ALIGN)));
static uint8_t  indexNodeB[NTFS_MAX_RECORD]  __attribute__((aligned(STORAGE_ALIGN)));
// W8 ($ATTRIBUTE_LIST): a second FILE-record buffer so an extension record can be read while the base
// record stays in another buffer; merged runlist scratch when one attribute spans several records.
static uint8_t  extRecord[NTFS_MAX_RECORD]   __attribute__((aligned(STORAGE_ALIGN)));
// W10a ($LZNT1): one compressed sub-block (header + data) and one decompressed sub-block. Decoding a
// sub-block at a time keeps these tiny (~8 KB) vs buffering a whole 64 KB compression unit.
static uint8_t  lzComp[4096 + 8]             __attribute__((aligned(STORAGE_ALIGN)));
static uint8_t  lzPlain[4096];
// The directory index name "$I30" (UTF-16), used to select the $INDEX_ROOT / $INDEX_ALLOCATION attrs.
static const uint16_t indexNameI30[4] = { '$', 'I', '3', '0' };
static const uint16_t indexNameSII[4] = { '$', 'S', 'I', 'I' };   // $Secure security-id index
static const uint16_t indexNameSDH[4] = { '$', 'S', 'D', 'H' };   // $Secure security-hash index
// "Common used indexes" (asciidoc L1414-L1416): $O owner ids (used by $Quota), $Q quotas, $R reparse.
static const uint16_t indexNameO[2]   = { '$', 'O' };   // owner-id index (also the $ObjId object-id index)
static const uint16_t indexNameQ[2]   = { '$', 'Q' };   // $Quota quota-control index
static const uint16_t indexNameR[2]   = { '$', 'R' };   // $Reparse reparse-point backreference index

// ===========================================================================
// Boot-sector recognition and geometry validation.
// ===========================================================================

// true if buffer holds an NTFS boot sector ("NTFS    " at offset 3, sig 0xAA55). The OEM tag is
// distinct from exFAT's "EXFAT   " and FAT32's "FAT32", so this never false-positives on them.
static int hasNtfsBoot(const uint8_t *boot)
{
   static const char tag[8] = { 'N', 'T', 'F', 'S', ' ', ' ', ' ', ' ' };
   for (int i = 0; i < 8; i++) {
      if (boot[BOOT_OEM_OFFSET + i] != (uint8_t)tag[i]) return 0;
   }
   return boot[BOOT_SIGNATURE] == 0x55 && boot[BOOT_SIGNATURE + 1] == 0xAA;
}

// true if a sector begins with the GPT header signature "EFI PART".
static int hasGptHeader(const uint8_t *sector)
{
   static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
   for (int i = 0; i < 8; i++) {
      if (sector[i] != (uint8_t)sig[i]) return 0;
   }
   return 1;
}

// Decodes the spec's power-of-two factor used by SectorsPerCluster and the MFT/index record size
// fields: byte values up to `literalMax` are a literal count of the smaller unit; larger values mean
// 2^(256-value) bytes. The two fields differ only at value 128: SectorsPerCluster treats 0..128 as a
// literal sector count (literalMax 128), so a 64 KB cluster on 512-byte sectors (byte 0x80 == 128
// sectors) parses; the record-size fields treat 128..255 as the byte form (literalMax 127). Returns
// the count (caller multiplies literal values by the unit). Example: factor 0xF6 -> 2^10 == 1024;
// factor 0x04 -> 4 units; SectorsPerCluster 128 -> 128.
static uint32_t decodeSignedFactor(uint8_t factor, uint32_t unitBytes, uint8_t literalMax, uint32_t *outBytes)
{
   if (factor <= literalMax) {
      if (unitBytes == 0) { *outBytes = 0; return 0; }   // caller rejects a 0 size
      *outBytes = (uint32_t)factor * unitBytes;
      return factor;
   }
   uint32_t shift = 256u - factor;                        // 2^(256-value) bytes
   if (shift >= 32) { *outBytes = 0; return 0; }   // absurd; caller rejects a 0 size
   *outBytes = 1u << shift;
   return *outBytes / (unitBytes ? unitBytes : 1);
}

// Validates the (untrusted, removable-media) NTFS boot geometry before any of it is used to compute
// LBAs. Returns 1 if usable, 0 to reject the volume. `deviceSectors` of 0 means the device size is
// unknown, so the device-size bound is skipped. Fills the parsed geometry into *vol on success.
static int parseNtfsBoot(const uint8_t *boot, uint32_t deviceSectorSize, uint64_t deviceSectors,
                         uint64_t volStart, NtfsVolume *vol)
{
   // bytes per sector: power of two in [256, 4096], and must match the device
   uint32_t bytesPerSector = readLe16(boot + BOOT_BYTES_PER_SECTOR);
   if (bytesPerSector < 256 || bytesPerSector > NTFS_MAX_SECTOR) return 0;
   if (bytesPerSector & (bytesPerSector - 1)) return 0;            // not a power of two
   if (bytesPerSector != deviceSectorSize) return 0;

   // sectors per cluster (signed power-of-two factor; unit is 1 sector, so the decoded "count" of
   // sectors is what we want whether the field was positive or the negative 2^(-value) form)
   uint32_t clusterSectorBytes = 0;
   uint32_t sectorsPerCluster = decodeSignedFactor(boot[BOOT_SECTORS_PER_CLUSTER], 1, 128, &clusterSectorBytes);
   if (sectorsPerCluster == 0 || (sectorsPerCluster & (sectorsPerCluster - 1))) return 0;
   uint64_t bytesPerCluster = (uint64_t)bytesPerSector * sectorsPerCluster;
   if (bytesPerCluster > (32u * 1024u * 1024u)) return 0;          // cluster <= 32 MB (sanity)

   // total sectors and the MFT location
   uint64_t totalSectors = readLe64(boot + BOOT_TOTAL_SECTORS);
   uint64_t mftLcn       = readLe64(boot + BOOT_MFT_LCN);
   uint64_t mftMirrLcn   = readLe64(boot + BOOT_MFTMIRR_LCN);
   if (totalSectors == 0) return 0;
   uint64_t clusterCount = totalSectors / sectorsPerCluster;
   if (mftLcn == 0 || mftLcn >= clusterCount) return 0;            // $MFT must live inside the volume
   if (mftMirrLcn == 0 || mftMirrLcn >= clusterCount) return 0;

   // MFT record size and index record size (signed power-of-two factor, in clusters or bytes)
   uint32_t mftRecordSize = 0, indexRecordSize = 0;
   decodeSignedFactor(boot[BOOT_MFT_RECORD_FACTOR], (uint32_t)bytesPerCluster, 127, &mftRecordSize);
   decodeSignedFactor(boot[BOOT_INDEX_RECORD_FACTOR], (uint32_t)bytesPerCluster, 127, &indexRecordSize);
   if (mftRecordSize < bytesPerSector || mftRecordSize > NTFS_MAX_RECORD) return 0;
   if (mftRecordSize & (mftRecordSize - 1)) return 0;             // record size must be a power of two
   if (indexRecordSize == 0 || (indexRecordSize & (indexRecordSize - 1))) return 0;
   if (indexRecordSize < bytesPerSector || indexRecordSize > NTFS_MAX_RECORD) return 0;   // >= a sector so
   // every INDX walker's `indexRecordSize - INDX_NODE_HEADER` (and the USA/INDX header) cannot underflow;
   // must also fit indexBuffer (we read whole blocks into it).

   // the volume (heap) must fit the device when its size is known
   if (deviceSectors != 0 && volStart + totalSectors > deviceSectors) return 0;

   // commit the validated geometry
   vol->bytesPerSector    = bytesPerSector;
   vol->sectorsPerCluster = sectorsPerCluster;
   vol->bytesPerCluster   = (uint32_t)bytesPerCluster;
   vol->totalSectors      = totalSectors;
   vol->mftLcn            = mftLcn;
   vol->mftMirrLcn        = mftMirrLcn;
   vol->mftRecordSize     = mftRecordSize;
   vol->indexRecordSize   = indexRecordSize;
   vol->volumeSerial      = readLe64(boot + BOOT_VOLUME_SERIAL);
   return 1;
}

// If sector `lba` holds an NTFS boot sector, copies it into `boot`, records the start in *volStart
// and returns 1; otherwise 0. Reads through the caller's aligned `vbr` scratch. Rejects an
// out-of-range LBA (attacker-controlled partition field) before reading it.
static int tryNtfsVbr(int storageHandle, uint64_t lba, uint32_t sectorBytes, uint64_t deviceSectors,
                      uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   uint64_t sectorBound = deviceSectors ? deviceSectors : NTFS_SCAN_LBA_CAP;
   if (lba == 0 || lba >= sectorBound) return 0;
   if (readSectors(storageHandle, lba, 1, vbr) != 0 || !hasNtfsBoot(vbr)) return 0;
   memCopy(boot, vbr, (int)sectorBytes);
   *volStart = lba;
   return 1;
}

// Walks a GPT partition table and returns the first partition whose first sector is an NTFS VBR.
static int locateNtfsInGpt(int storageHandle, uint32_t sectorBytes, uint64_t deviceSectors,
                           uint8_t *scratch, uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   if (readSectors(storageHandle, GPT_HEADER_LBA, 1, scratch) != 0 || !hasGptHeader(scratch)) return 0;
   uint64_t entriesLba = readLe64(scratch + 72);   // PartitionEntryLBA
   uint32_t entryCount = readLe32(scratch + 80);   // NumberOfPartitionEntries
   uint32_t entrySize  = readLe32(scratch + 84);   // SizeOfPartitionEntry
   if (entrySize < GPT_ENTRY_MIN_SIZE || entrySize > sectorBytes) return 0;
   uint64_t sectorBound = deviceSectors ? deviceSectors : NTFS_SCAN_LBA_CAP;
   if (entriesLba >= sectorBound) return 0;
   if (entryCount > GPT_MAX_ENTRIES) entryCount = GPT_MAX_ENTRIES;
   uint32_t perSector = sectorBytes / entrySize;

   for (uint32_t i = 0; i < entryCount; i++) {
      if (i % perSector == 0) {
         uint64_t entrySectorLba = entriesLba + i / perSector;   // re-bound each read: a near-cap entriesLba could otherwise overflow into a wild LBA
         if (entrySectorLba >= sectorBound) return 0;
         if (readSectors(storageHandle, entrySectorLba, 1, scratch) != 0) return 0;
      }
      const uint8_t *entry = scratch + (i % perSector) * entrySize;
      int used = 0;
      for (int k = 0; k < 16; k++) if (entry[k]) { used = 1; break; }   // non-zero PartitionTypeGUID
      if (used && tryNtfsVbr(storageHandle, readLe64(entry + 32), sectorBytes, deviceSectors, vbr, boot, volStart))
         return 1;
   }
   return 0;
}

// Locates the NTFS volume reachable through `storageHandle`: a superfloppy at LBA 0, or a partition
// listed in an MBR or GPT table. On success `boot` holds the volume's VBR and *volStart its start LBA.
static int locateNtfsVolume(int storageHandle, uint8_t *boot, uint8_t *scratch, uint8_t *vbr,
                            uint32_t sectorBytes, uint64_t deviceSectors, uint64_t *volStart)
{
   if (hasNtfsBoot(boot)) { *volStart = 0; return 1; }              // superfloppy (volume at LBA 0)
   if (boot[BOOT_SIGNATURE] != 0x55 || boot[BOOT_SIGNATURE + 1] != 0xAA) return 0;   // neither NTFS nor partitioned

   for (int i = 0; i < MBR_PART_ENTRIES; i++) {
      const uint8_t *part = boot + MBR_PART_TABLE + i * MBR_PART_SIZE;
      uint8_t type = part[4];
      if (type == 0) continue;
      if (type == MBR_TYPE_GPT) {
         if (locateNtfsInGpt(storageHandle, sectorBytes, deviceSectors, scratch, vbr, boot, volStart)) return 1;
         continue;
      }
      if (tryNtfsVbr(storageHandle, readLe32(part + 8), sectorBytes, deviceSectors, vbr, boot, volStart)) return 1;
   }
   return 0;
}

// ===========================================================================
// MFT record reading + Update Sequence Array (fixup).
// ===========================================================================

// Applies the NTFS Update Sequence Array fixup to a multi-sector metadata record (FILE or, later,
// INDX). At write time each sector's last 2 bytes are replaced on disk by a single update sequence
// number (USN); the real values are stashed in the USA. We verify every sector still carries the
// USN - if one doesn't, the multi-sector write was torn (or the record is corrupt), which would
// otherwise be silent corruption - then restore the saved words. `recordSize` is a multiple of
// `sectorSize` (both validated at mount). Returns 0 on success, -1 on any mismatch/out-of-bounds.
static int applyUsaFixup(uint8_t *record, uint32_t recordSize, uint32_t sectorSize)
{
   uint16_t usaOffset = readLe16(record + FILE_USA_OFFSET);
   uint16_t usaCount  = readLe16(record + FILE_USA_COUNT);   // 1 USN word + one fixup word per sector

   // the USA must describe exactly one fixup per sector, and must physically fit in the record
   uint32_t blocks = recordSize / sectorSize;
   if (usaCount == 0 || (uint32_t)(usaCount - 1) != blocks) return -1;
   if (usaOffset < FILE_FIRST_ATTR_OFFSET) return -1;                      // USA can't sit in the fixed header fields
   if ((uint32_t)usaOffset + (uint32_t)usaCount * 2 > recordSize) return -1;

   // verify each sector tail still holds the USN, then restore the saved word
   const uint8_t *usa = record + usaOffset;
   uint16_t usn = readLe16(usa);
   for (uint32_t i = 0; i < blocks; i++) {
      uint8_t *tail = record + (i + 1) * sectorSize - 2;   // last 2 bytes of sector i
      if (readLe16(tail) != usn) return -1;                // torn write / corruption
      const uint8_t *saved = usa + 2 + i * 2;
      tail[0] = saved[0];
      tail[1] = saved[1];
   }
   return 0;
}

// Re-encodes the Update Sequence Array for writing a record back to disk (inverse of applyUsaFixup):
// bumps the USN, stashes each sector's real last 2 bytes into the USA, and writes the USN into each
// sector tail. After this the in-memory record is in on-disk form and ready for writeSectors. The
// USA geometry was already validated when the record was read in.
static void applyUsaWrite(uint8_t *record, uint32_t recordSize, uint32_t sectorSize)
{
   uint16_t usaOffset = readLe16(record + FILE_USA_OFFSET);
   uint16_t usaCount  = readLe16(record + FILE_USA_COUNT);
   uint32_t blocks = recordSize / sectorSize;
   if (usaCount == 0 || (uint32_t)(usaCount - 1) != blocks) return;
   uint8_t *usa = record + usaOffset;
   uint16_t usn = (uint16_t)(readLe16(usa) + 1);
   if (usn == 0) usn = 1;                                 // never use 0 as a USN
   for (uint32_t i = 0; i < blocks; i++) {
      uint8_t *tail = record + (i + 1) * sectorSize - 2;
      usa[2 + i * 2] = tail[0];                           // save the real tail into the USA
      usa[3 + i * 2] = tail[1];
      tail[0] = (uint8_t)usn;                             // write the USN into the sector tail
      tail[1] = (uint8_t)(usn >> 8);
   }
   writeLe16(usa, usn);
}

// Validates a FILE record header (after fixup): "FILE" signature, in-use, and the first-attribute
// offset and used size within the record. Returns 1 if usable, 0 otherwise.
static int isValidFileRecord(const uint8_t *record, uint32_t recordSize)
{
   if (record[0] != 'F' || record[1] != 'I' || record[2] != 'L' || record[3] != 'E') return 0;
   if (!(readLe16(record + FILE_FLAGS) & FILE_FLAG_IN_USE)) return 0;
   uint16_t firstAttr = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   uint32_t usedSize  = readLe32(record + FILE_USED_SIZE);
   if (firstAttr < FILE_FIRST_ATTR_OFFSET || firstAttr >= recordSize) return 0;
   if (usedSize < firstAttr || usedSize > recordSize) return 0;
   return 1;
}

// Reads FILE record `number` directly from the start of $MFT and fixes it up. Bootstrap path: the
// first MFT clusters are contiguous from mftLcn, so low-numbered system records ($MFT, $MFTMirr,
// root, $Bitmap, $UpCase) are reachable without the $MFT runlist - which is itself parsed out of
// record 0. Arbitrary (possibly fragmented) records need that runlist and arrive in a later stage.
// Returns 0 on success with the fixed-up, validated record in `out` (>= vol->mftRecordSize bytes).
static int readMftRecordBootstrap(const NtfsVolume *vol, uint64_t number, uint8_t *out)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint64_t byteOffset = vol->mftLcn * vol->bytesPerCluster + number * recordSize;   // within contiguous MFT head
   uint64_t lba        = vol->partitionOffset + byteOffset / vol->bytesPerSector;    // sector-aligned (record/cluster are sector multiples)
   uint32_t sectors    = recordSize / vol->bytesPerSector;
   if (readSectors(vol->storageHandle, lba, sectors, out) != 0) return -1;
   if (applyUsaFixup(out, recordSize, vol->bytesPerSector) != 0) return -1;
   if (!isValidFileRecord(out, recordSize)) return -1;
   return 0;
}

static int64_t mapVcnToLcn(const NtfsRunEntry *runs, int runCount, uint64_t vcn);   // defined in the runlist section
static int decodeRuns(const uint8_t *runlist, uint32_t length, NtfsRunEntry *runs, int max, int *count);  // runlist section

// Writes FILE record `number` back to disk via the $MFT runlist, USA-encoding it first (single
// fixed-up record write — atomic at the sector level). `record` is left in on-disk form. Mirror of
// readMftRecord's addressing. Returns 0 on success.
// $MFTMirr mirrors the first NTFS_MFTMIRR_RECORDS MFT records ($MFT, $MFTMirr, $LogFile, $Volume).
// The NTFS spec requires it be kept identical to $MFT for those records; Windows/chkdsk refuse to
// mount a volume where it diverges. So any write to record 0..3 must also be written to $MFTMirr.
#define NTFS_MFTMIRR_RECORDS 4

// jf-port: set by setVolumeDirty while it CLEARS the flag: $MFTMirr's copy is written first, so a cut
// between the two writes leaves $MFT (what every reader looks at) still saying dirty.
static int mftMirrorFirst = 0;

static int writeMftMirror(const NtfsVolume *vol, uint64_t number, const uint8_t *record)
{
   uint64_t mlba = vol->partitionOffset + vol->mftMirrLcn * vol->sectorsPerCluster
                 + (number * vol->mftRecordSize) / vol->bytesPerSector;
   return writeSectors(vol->storageHandle, mlba, vol->mftRecordSize / vol->bytesPerSector, record);
}

static int writeMftRecord(const NtfsVolume *vol, uint64_t number, uint8_t *record)
{
   applyUsaWrite(record, vol->mftRecordSize, vol->bytesPerSector);
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t byteOffset = number * recordSize;
   const int mirrored = number < NTFS_MFTMIRR_RECORDS && vol->mftMirrLcn != 0;
   if (mirrored && mftMirrorFirst && writeMftMirror(vol, number, record) != 0) return -1;   // jf-port

   if (recordSize <= bytesPerCluster) {
      int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster);
      if (lcn < 0) return -1;
      uint32_t inCluster = (uint32_t)(byteOffset % bytesPerCluster);
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + inCluster / vol->bytesPerSector;
      if (writeSectors(vol->storageHandle, lba, recordSize / vol->bytesPerSector, record) != 0) return -1;
   } else {
      // jf-port: one storage write per physically contiguous stretch of the record (nearly always the
      // whole record).  Upstream wrote cluster by cluster, so with clusters smaller than a record a
      // write cut part way (the drive pulled) left a record torn in two that nothing can read.
      uint32_t clusters = recordSize / bytesPerCluster;
      for (uint32_t c = 0; c < clusters; ) {
         int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster + c);
         if (lcn < 0) return -1;
         uint32_t run = 1;
         while (c + run < clusters &&
                mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster + c + run) == lcn + (int64_t)run)
            run++;
         uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster;
         if (writeSectors(vol->storageHandle, lba, run * (bytesPerCluster / vol->bytesPerSector), record + c * bytesPerCluster) != 0)
            return -1;
         c += run;
      }
   }

   // Mirror records 0..3 into $MFTMirr (spec requirement; the buffer is already USA-encoded and the
   // mirror copy is byte-identical). Assumes $MFTMirr's clusters are contiguous from mftMirrLcn —
   // true for every volume mkntfs/Windows produce (it is 4 records allocated as one small unit). A
   // (pathological) fragmented $MFTMirr would need its runlist; not handled, accepted limitation.
   if (mirrored && !mftMirrorFirst && writeMftMirror(vol, number, record) != 0) return -1;
   // Restore the caller's buffer to its pre-write logical content. applyUsaWrite mangled the last two
   // bytes of every sector in place with the USN; without undoing that, any re-find/re-read of `record`
   // after the write (e.g. W6/W7's "re-find $INDEX_ALLOCATION after the root split", W8b) reads garbage
   // wherever an attribute field lands on a sector-end fixup offset (510, 1022, ...). applyUsaFixup is
   // the exact inverse and leaves `record` byte-identical to before the call.
   applyUsaFixup(record, vol->mftRecordSize, vol->bytesPerSector);
   return 0;
}

static int setVolumeDirty(NtfsVolume *vol, int dirty);   // defined with the write path below

// ===========================================================================
// Attribute parsing. Walks the attribute chain inside one FILE record, parsing
// the common header and the resident vs non-resident variants. ($ATTRIBUTE_LIST
// spanning multiple records is followed in S4, once arbitrary records are
// readable via the $MFT runlist.)
// ===========================================================================

// One attribute's parsed location within a FILE record buffer. For resident attributes `value`
// points at the data inside the record; for non-resident ones `attr` points at the attribute
// header so the runlist (at runlistOffset) can be decoded later.
typedef struct {
   uint32_t type;
   uint16_t attributeId;
   int      resident;
   int      compressed;        // ATTR_FLAG_COMPRESSED or ATTR_FLAG_SPARSE set
   uint32_t compUnitClusters;  // $LZNT1 compression-unit size in clusters (1<<compression_unit), 0 if none
   const uint8_t *value;       // resident: pointer to the value within the record
   uint32_t valueLength;       // resident: value length in bytes
   const uint8_t *attr;        // non-resident: pointer to the attribute header
   uint32_t attrLength;        // non-resident: whole-attribute length
   uint16_t runlistOffset;     // non-resident: offset of the runlist within the attribute
   uint64_t startVcn;          // non-resident: first VCN mapped
   uint64_t lastVcn;           // non-resident: last VCN mapped
   uint64_t allocatedSize;     // non-resident: allocated bytes
   uint64_t realSize;          // non-resident: real (data) bytes
   uint64_t validSize;         // non-resident: valid/initialized bytes (reads past it are zero)
} NtfsAttr;

// W8b-S1 ($DATA spill to an extension record). Forward decls; defined after the MFT-alloc helpers.
static int allocExtensionRecord(NtfsVolume *vol, uint64_t baseRefFull, uint64_t *outRef);
static int spillDataToExtension(NtfsVolume *vol, uint64_t baseRef, const NtfsRunEntry *runs, int runCount,
                                uint64_t realSize, uint64_t validSize, uint64_t allocSize, uint64_t lastVcn);
static int findDataAnywhere(NtfsVolume *vol, uint64_t baseRef, uint8_t *buf, NtfsAttr *out, uint64_t *housingRef);
static int countNtfsFreeClusters(NtfsVolume *vol, uint64_t *out);   // seeds vol->freeClusters at mount

// Parses one attribute (header already located at `attr`, total length `attrLength`) into *out,
// bounds-checking the resident value / non-resident fields against the attribute. Returns 1 on
// success, 0 if a field doesn't fit the attribute.
static int parseAttribute(const uint8_t *attr, uint32_t attrLength, NtfsAttr *out)
{
   memSet(out, 0, (int)sizeof(*out));
   out->type        = readLe32(attr + ATTR_TYPE_OFFSET);
   out->attributeId = readLe16(attr + ATTR_ID_OFFSET);
   out->resident    = (attr[ATTR_NON_RESIDENT] == 0);
   out->attr        = attr;          // record location of the attribute header (both forms)
   out->attrLength  = attrLength;
   out->compressed  = (readLe16(attr + ATTR_FLAGS_OFFSET) & (ATTR_FLAG_COMPRESSED | ATTR_FLAG_SPARSE)) != 0;

   if (out->resident) {
      if (attrLength < 24) return 0;   // resident header is 24 bytes: common 16 + value length/offset/flags
      uint32_t valueLength = readLe32(attr + ATTR_RES_VALUE_LENGTH);
      uint16_t valueOffset = readLe16(attr + ATTR_RES_VALUE_OFFSET);
      if ((uint32_t)valueOffset + valueLength > attrLength) return 0;   // value must fit the attribute
      out->value       = attr + valueOffset;
      out->valueLength = valueLength;
      return 1;
   }

   // non-resident
   if (attrLength < ATTR_NR_HEADER_MIN) return 0;
   out->runlistOffset = readLe16(attr + ATTR_NR_RUNLIST_OFFSET);
   if (out->runlistOffset < ATTR_NR_HEADER_MIN || out->runlistOffset > attrLength) return 0;
   out->startVcn      = readLe64(attr + ATTR_NR_START_VCN);
   out->lastVcn       = readLe64(attr + ATTR_NR_LAST_VCN);
   out->allocatedSize = readLe64(attr + ATTR_NR_ALLOC_SIZE);
   out->realSize      = readLe64(attr + ATTR_NR_REAL_SIZE);
   out->validSize     = readLe64(attr + ATTR_NR_VALID_SIZE);
   { uint16_t cu = readLe16(attr + ATTR_NR_COMPRESSION_UNIT);   // W10a: $LZNT1 compression-unit (power of 2)
     // jf-port: the compression unit applies only to a COMPRESSED attribute.  A sparse-only one
     // (flag 0x8000, which Windows and ntfs-3g also give a unit of 16 clusters) stores its allocated
     // clusters as plain data, so it must read through the normal runlist path, holes as zeros.
     // Upstream decoded those clusters as LZNT1 and failed any sparse file with a written extent.
     out->compUnitClusters = ((readLe16(attr + ATTR_FLAGS_OFFSET) & ATTR_FLAG_COMPRESSED) && cu > 0 && cu < 24)
                                ? (1u << cu) : 0; }
   return 1;
}

// Finds the first attribute of `type` (matching `name` of `nameLen` UTF-16 units when nameLen > 0;
// pass NULL/0 for the common unnamed case, e.g. the unnamed $DATA) by walking the attribute chain
// with bounds + a forward-progress/iteration guard against a cyclic or overlong chain. Returns 1
// found (fills *out), 0 not present, -1 malformed.
static int findAttribute(const uint8_t *record, uint32_t recordSize, uint32_t type,
                         const uint16_t *name, uint8_t nameLen, NtfsAttr *out)
{
   uint32_t usedSize = readLe32(record + FILE_USED_SIZE);
   uint32_t limit    = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset   = readLe16(record + FILE_FIRST_ATTR_OFFSET);

   for (int guard = 0; guard < 256; guard++) {   // a record can't hold anywhere near 256 attributes
      // read the type (the end marker is just this 4-byte word), then validate a full common header
      if (offset + 4 > limit) return -1;
      uint32_t attrType = readLe32(record + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) return 0;
      if (offset + 16 > limit) return -1;
      uint32_t attrLength = readLe32(record + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) return -1;   // offset <= limit, so no overflow

      // on a type hit, optionally match the UTF-16 attribute name
      if (attrType == type) {
         int nameOk = (record[offset + ATTR_NAME_LENGTH] == nameLen);
         if (nameOk && nameLen > 0) {
            uint16_t nameOffset = readLe16(record + offset + ATTR_NAME_OFFSET);
            if ((uint32_t)nameOffset + (uint32_t)nameLen * 2 > attrLength) return -1;
            for (uint8_t i = 0; i < nameLen && nameOk; i++)
               if (readLe16(record + offset + nameOffset + i * 2) != name[i]) nameOk = 0;
         }
         if (nameOk) return parseAttribute(record + offset, attrLength, out) ? 1 : -1;
      }
      offset += attrLength;
   }
   return -1;   // no end marker within the bound -> malformed
}

// ===========================================================================
// Runlist (data run) decoding. A non-resident attribute's content is a list of
// runs: each run's header byte packs the byte-width of the run length (low
// nibble) and of the LCN offset (high nibble). The LCN offset is a *signed*
// delta from the previous run's LCN (sign-extended from its width); a zero
// offset width is a sparse run (a hole that reads as zeros). A 0x00 header byte
// terminates the list.
// ===========================================================================

// A cursor over an encoded runlist. Tracks the accumulated absolute LCN (updated only by real
// runs, so a sparse run does not perturb the next run's delta base) and the running VCN.
typedef struct {
   const uint8_t *runlist;
   uint32_t length;       // bytes available (bound)
   uint32_t offset;       // current parse position
   int64_t  currentLcn;   // accumulated LCN of the last real run
   uint64_t currentVcn;   // VCN at the start of the next run
   int      done;         // hit the terminator
} NtfsRunlist;

static void openRunlist(NtfsRunlist *cursor, const uint8_t *runlist, uint32_t length)
{
   cursor->runlist = runlist;
   cursor->length  = length;
   cursor->offset  = 0;
   cursor->currentLcn = 0;
   cursor->currentVcn = 0;
   cursor->done = 0;
}

// Decodes the next run: sets *vcn/*lcn/*count and returns 1 (lcn < 0 for a sparse hole), returns
// 0 at the terminator, or -1 on a malformed runlist.
static int nextRun(NtfsRunlist *cursor, uint64_t *vcn, int64_t *lcn, uint64_t *count)
{
   if (cursor->done) return 0;
   if (cursor->offset >= cursor->length) return -1;        // ran off the end with no terminator

   // header: low nibble = length byte-width, high nibble = offset byte-width
   uint8_t header = cursor->runlist[cursor->offset++];
   if (header == 0) { cursor->done = 1; return 0; }        // terminator
   uint32_t lengthBytes = header & 0x0F;
   uint32_t offsetBytes = (header >> 4) & 0x0F;
   if (lengthBytes == 0 || lengthBytes > 8 || offsetBytes > 8) return -1;
   if (cursor->offset + lengthBytes + offsetBytes > cursor->length) return -1;

   // run length (unsigned, little-endian)
   uint64_t runLength = 0;
   for (uint32_t i = 0; i < lengthBytes; i++)
      runLength |= (uint64_t)cursor->runlist[cursor->offset + i] << (8 * i);
   cursor->offset += lengthBytes;
   if (runLength == 0) return -1;                           // a zero-length run is malformed

   *vcn = cursor->currentVcn;
   *count = runLength;

   if (offsetBytes == 0) {
      *lcn = -1;                                            // sparse: hole, currentLcn unchanged
   } else {
      // signed LCN delta, little-endian, sign-extended from its top byte
      uint64_t raw = 0;
      for (uint32_t i = 0; i < offsetBytes; i++)
         raw |= (uint64_t)cursor->runlist[cursor->offset + i] << (8 * i);
      uint32_t shift = 64 - 8 * offsetBytes;
      int64_t delta = (int64_t)(raw << shift) >> shift;     // arithmetic shift sign-extends the delta
      cursor->offset += offsetBytes;
      cursor->currentLcn += delta;
      if (cursor->currentLcn < 0) return -1;                // a real run can't map below LCN 0
      *lcn = cursor->currentLcn;
   }
   cursor->currentVcn += runLength;
   return 1;
}

// Decodes $MFT's own $DATA runlist into vol->mftRuns so any record can be located. Returns 0 on a
// clean terminator, -1 if malformed, sparse (the MFT never is), or too fragmented for the cache.
static int decodeMftRunlist(NtfsVolume *vol, const NtfsAttr *data)
{
   NtfsRunlist cursor;
   openRunlist(&cursor, data->attr + data->runlistOffset, data->attrLength - data->runlistOffset);
   vol->mftRunCount = 0;
   uint64_t vcn, count;
   int64_t  lcn;
   int rc;
   while ((rc = nextRun(&cursor, &vcn, &lcn, &count)) == 1) {
      if (lcn < 0) return -1;                               // $MFT is never sparse
      if (vol->mftRunCount >= NTFS_MFT_RUNS_MAX) return -1; // too fragmented for the bounded cache
      vol->mftRuns[vol->mftRunCount].vcn   = vcn;
      vol->mftRuns[vol->mftRunCount].lcn   = lcn;
      vol->mftRuns[vol->mftRunCount].count = count;
      vol->mftRunCount++;
   }
   return rc;   // 0 clean, -1 malformed
}

// Maps a VCN to its absolute LCN via a decoded runlist. Returns the LCN, or -1 if the VCN is in a
// sparse hole or maps outside the runlist.
static int64_t mapVcnToLcn(const NtfsRunEntry *runs, int runCount, uint64_t vcn)
{
   for (int i = 0; i < runCount; i++) {
      if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].count) {
         if (runs[i].lcn < 0) return -1;                    // sparse
         return runs[i].lcn + (int64_t)(vcn - runs[i].vcn);
      }
   }
   return -1;
}

// Like mapVcnToLcn but also reports, in *contig, how many physically-contiguous clusters map from
// `vcn` onward (consecutive VCNs -> consecutive LCNs, absorbing adjacent runs) so a multi-cluster
// read/write can issue ONE storage call. *contig spans the sparse run on a hole. Returns -1 sparse,
// -2 if vcn is unmapped.
static int64_t mapRunsSpan(const NtfsRunEntry *runs, int runCount, uint64_t vcn, uint64_t *contig)
{
   for (int i = 0; i < runCount; i++) {
      if (vcn < runs[i].vcn || vcn >= runs[i].vcn + runs[i].count) continue;
      uint64_t offsetInRun = vcn - runs[i].vcn;
      if (runs[i].lcn < 0) { *contig = runs[i].count - offsetInRun; return -1; }   // sparse hole
      int64_t  startLcn = runs[i].lcn + (int64_t)offsetInRun;
      uint64_t span     = runs[i].count - offsetInRun;
      int64_t  nextLcn  = runs[i].lcn + (int64_t)runs[i].count;
      for (int j = i + 1; j < runCount && runs[j].lcn == nextLcn; j++) {           // fold physically-adjacent runs
         span    += runs[j].count;
         nextLcn += (int64_t)runs[j].count;
      }
      *contig = span;
      return startLcn;
   }
   return -2;
}

// Reads FILE record `number` via the decoded $MFT runlist (so it works for any record, fragmented
// or not), fixes it up and validates it. Handles records smaller than a cluster (several per
// cluster) and records spanning multiple clusters (read per cluster, so a run boundary mid-record
// is handled). Returns 0 on success with the record in `out`.
// Reads record `number` off disk via the $MFT runlist and applies the USA fixup, with no validity
// check. A not-in-use (freed or freshly grown) record reads back fine - the in-use/structural checks
// are layered on top by readMftRecord. Returns 0 on a clean read.
static int readMftRecordBytes(const NtfsVolume *vol, uint64_t number, uint8_t *out)
{
   uint32_t recordSize     = vol->mftRecordSize;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t byteOffset     = number * recordSize;

   if (recordSize <= bytesPerCluster) {
      // one cluster holds the whole record (possibly several records per cluster)
      int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster);
      if (lcn < 0) return -1;
      uint32_t inCluster = (uint32_t)(byteOffset % bytesPerCluster);
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + inCluster / vol->bytesPerSector;
      if (readSectors(vol->storageHandle, lba, recordSize / vol->bytesPerSector, out) != 0) return -1;
   } else {
      // record spans several clusters: read each one (handles a run boundary inside the record)
      uint32_t clusters = recordSize / bytesPerCluster;
      for (uint32_t c = 0; c < clusters; c++) {
         int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster + c);
         if (lcn < 0) return -1;
         uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster;
         if (readSectors(vol->storageHandle, lba, bytesPerCluster / vol->bytesPerSector, out + c * bytesPerCluster) != 0)
            return -1;
      }
   }
   if (applyUsaFixup(out, recordSize, vol->bytesPerSector) != 0) return -1;
   return 0;
}

static int readMftRecord(const NtfsVolume *vol, uint64_t number, uint8_t *out)
{
   if (readMftRecordBytes(vol, number, out) != 0) return -1;
   if (!isValidFileRecord(out, vol->mftRecordSize)) return -1;
   return 0;
}

// Reads an MFT record named by an 8-byte FILE REFERENCE: low 48 bits = entry index, high 16
// = sequence number. Validates the sequence (F048): when an MFT record is deleted and reused
// its sequence is bumped (see freeMftRecord), so a stale reference (right index, old sequence)
// must be refused rather than silently resolved to the reused record. The threat case is
// references read from untrusted on-disk metadata ($ATTRIBUTE_LIST, directory index entries,
// parent pointers); route every reference-based record read through this helper.
//
// wantSeq == 0 is the DELIBERATE NTFS CONVENTION for "unchecked": a bare record NUMBER (a
// system file such as $MFT/$Volume/$Bitmap, or an internally-generated/just-allocated record)
// has no sequence, so callers that legitimately read by index pass it with the high 16 bits
// zero and the sequence check is skipped. Real on-disk file references always carry a nonzero
// sequence, so this never weakens validation of attacker-controlled refs.
static int readMftRecordByRef(const NtfsVolume *vol, uint64_t fullRef, uint8_t *out)
{
   if (readMftRecord(vol, fullRef & MFT_REF_MASK, out) != 0) return -1;
   uint16_t wantSeq = (uint16_t)(fullRef >> 48);
   if (wantSeq != 0 && readLe16(out + FILE_SEQUENCE_NUMBER) != wantSeq) return -1;
   return 0;
}

// ===========================================================================
// File data read (resident, non-resident via runlist, sparse holes, ValidDataLength).
// ===========================================================================

// Maps a VCN to its LCN by decoding an attribute's runlist on the fly (no per-file run cache).
// Returns the LCN, -1 for a sparse hole, or -2 if the runlist is malformed or doesn't cover the VCN.
static int64_t mapRunlistVcn(const uint8_t *runlist, uint32_t length, uint64_t vcn)
{
   NtfsRunlist cursor;
   openRunlist(&cursor, runlist, length);
   uint64_t runVcn, runCount;
   int64_t  runLcn;
   int rc;
   while ((rc = nextRun(&cursor, &runVcn, &runLcn, &runCount)) == 1)
      if (vcn >= runVcn && vcn < runVcn + runCount) {
         if (runLcn < 0) return -1;                         // sparse hole
         return runLcn + (int64_t)(vcn - runVcn);
      }
   return -2;   // malformed, or VCN beyond the runlist while still inside the file size
}

// Like mapRunlistVcn but also reports, in *contig, how many physically-contiguous clusters map from
// `vcn` onward (absorbing adjacent runs) so a multi-cluster read/write can issue ONE storage call.
// *contig spans the sparse run on a hole. Returns -1 sparse, -2 malformed / vcn beyond the runlist.
static int64_t mapRunlistSpan(const uint8_t *runlist, uint32_t length, uint64_t vcn, uint64_t *contig)
{
   NtfsRunlist cursor;
   openRunlist(&cursor, runlist, length);
   uint64_t runVcn, runCount;
   int64_t  runLcn;
   int64_t  startLcn = -2, nextLcn = 0;
   uint64_t span = 0;
   int rc;
   while ((rc = nextRun(&cursor, &runVcn, &runLcn, &runCount)) == 1) {
      if (startLcn == -2) {                                 // still locating the run that holds vcn
         if (vcn < runVcn || vcn >= runVcn + runCount) continue;
         uint64_t offsetInRun = vcn - runVcn;
         if (runLcn < 0) { *contig = runCount - offsetInRun; return -1; }   // sparse hole
         startLcn = runLcn + (int64_t)offsetInRun;
         span     = runCount - offsetInRun;
         nextLcn  = runLcn + (int64_t)runCount;
      } else if (runLcn == nextLcn) {                       // physically-adjacent following run: extend the span
         span    += runCount;
         nextLcn += (int64_t)runCount;
      } else break;
   }
   if (startLcn == -2) return -2;
   *contig = span;
   return startLcn;
}

// ===========================================================================
// W10a: $LZNT1 decompression (read). A compressed $DATA stores each compression unit (cb) as a few
// real "compressed" clusters followed by sparse padding; each cb decompresses to cb_size bytes made of
// 4 KiB sub-blocks. We decode ONE sub-block at a time (LZNT1 back-references never cross a sub-block
// boundary) to keep the static footprint tiny instead of buffering a whole 64 KiB cb. Faithful port of
// ntfs-3g compress.c ntfs_decompress; spec is gold standard.
// ===========================================================================
#define NTFS_SB_SIZE        4096      // $LZNT1 sub-block plaintext size
#define NTFS_SB_SIZE_MASK   0x0fff
#define NTFS_SB_IS_COMPRESSED 0x8000

// Decompresses one $LZNT1 sub-block (`sb` = its 2-byte header + data, total `sbTotal` bytes) into
// plain[NTFS_SB_SIZE]. Returns the plaintext length (always NTFS_SB_SIZE; trailing bytes zero-filled),
// or -1 on malformed input. All compressed-stream accesses are bounds-checked (refuse, never over-read).
static int lznt1DecompressSb(const uint8_t *sb, uint32_t sbTotal, uint8_t *plain)
{
   if (sbTotal < 2) return -1;
   uint16_t hdr = readLe16(sb);
   const uint8_t *comp = sb + 2;
   const uint8_t *compEnd = sb + sbTotal;
   if (!(hdr & NTFS_SB_IS_COMPRESSED)) {                       // verbatim sub-block: exactly 4096 bytes
      if (sbTotal - 2 != NTFS_SB_SIZE) return -1;
      memCopy(plain, comp, NTFS_SB_SIZE);
      return NTFS_SB_SIZE;
   }
   uint32_t dpos = 0;                                          // position within the plaintext sub-block
   while (comp < compEnd && dpos < NTFS_SB_SIZE) {
      uint8_t tag = *comp++;
      for (int token = 0; token < 8 && comp < compEnd && dpos < NTFS_SB_SIZE; token++, tag >>= 1) {
         if ((tag & 1) == 0) { plain[dpos++] = *comp++; continue; }   // literal byte
         if (dpos == 0) return -1;                             // a phrase can't be the first token
         if (comp + 2 > compEnd) return -1;
         uint16_t pt = readLe16(comp); comp += 2;
         uint32_t lg = 0;
         for (uint32_t i = dpos - 1; i >= 0x10; i >>= 1) lg++; // log2 split point for this position
         uint32_t back   = (uint32_t)(pt >> (12 - lg)) + 1;    // bytes to go back
         uint32_t length = (uint32_t)(pt & (0xfff >> lg)) + 3; // bytes to copy
         if (back > dpos || dpos + length > NTFS_SB_SIZE) return -1;
         uint32_t src = dpos - back;
         for (uint32_t k = 0; k < length; k++) plain[dpos + k] = plain[src + k];   // byte-wise (overlap-safe)
         dpos += length;
      }
   }
   if (dpos < NTFS_SB_SIZE) memSet(plain + dpos, 0, NTFS_SB_SIZE - dpos);   // pad an incomplete sub-block
   return NTFS_SB_SIZE;
}

// Reads `chunk` real (non-sparse) bytes, already clamped to the contiguous span and ValidDataLength,
// starting at byte offset `inCluster` within cluster `lcn` into `out`. Whole aligned sectors go straight
// to the caller's buffer in one storage call (spanning clusters); a partial/unaligned edge bounces
// through the scratch. `spanBytes` is the contiguous real bytes available from here. Returns bytes
// read (may be < chunk when only whole sectors were taken), or -1. Shared by both non-resident readers.
static int64_t readSpanData(const NtfsVolume *vol, int64_t lcn, uint32_t inCluster, uint64_t spanBytes,
                            uint8_t *out, uint64_t chunk)
{
   uint32_t sectorSize   = vol->bytesPerSector;
   uint32_t offsetInSec  = inCluster % sectorSize;
   uint32_t alignedStart = inCluster - offsetInSec;
   uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + alignedStart / sectorSize;

   // fast path: sector-aligned start + aligned destination -> one storage call into the caller's buffer
   if (offsetInSec == 0 && chunk >= sectorSize && isDmaAligned(out)) {
      uint32_t sectors = (uint32_t)(chunk / sectorSize);
      if (readSectors(vol->storageHandle, lba, sectors, out) != 0) return -1;
      return (int64_t)sectors * sectorSize;            // partial trailing sector handled on the next iteration
   }

   // bounce path: partial leading sector or unaligned destination. Read whole sectors from the aligned
   // start (capped at the bounce) through the scratch, then copy out the requested slice.
   uint64_t windowBytes = spanBytes + offsetInSec;     // whole-sector bytes from alignedStart
   if (windowBytes > NTFS_READ_BOUNCE) windowBytes = NTFS_READ_BOUNCE;
   if (readSectors(vol->storageHandle, lba, (uint32_t)windowBytes / sectorSize, fileBounce) != 0) return -1;
   uint64_t avail = windowBytes - offsetInSec;
   if (chunk > avail) chunk = avail;
   memCopy(out, fileBounce + offsetInSec, (int)chunk);
   return (int64_t)chunk;
}

// Reads `want` bytes at file offset `pos` from a non-resident $DATA: maps each contiguous span via the
// runlist, zero-fills sparse holes and anything at/after ValidDataLength, and reads real data via
// readSpanData. Returns bytes read (== want on success) or -1 on error.
static int64_t readNonResident(const NtfsVolume *vol, const NtfsAttr *data, uint64_t validSize,
                               uint64_t pos, uint8_t *out, uint64_t want)
{
   const uint8_t *runlist = data->attr + data->runlistOffset;
   uint32_t runlistLength = data->attrLength - data->runlistOffset;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t done = 0;

   while (done < want) {
      uint64_t fileOffset = pos + done;
      uint64_t vcn        = fileOffset / bytesPerCluster;
      uint32_t inCluster  = (uint32_t)(fileOffset % bytesPerCluster);

      // bytes past ValidDataLength read as zeros (initialized-size semantics); clamp each window to VDL
      uint64_t chunk = want - done;
      if (fileOffset >= validSize) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }
      if (fileOffset + chunk > validSize) chunk = validSize - fileOffset;

      uint64_t contig = 0;
      int64_t  lcn = mapRunlistSpan(runlist, runlistLength, vcn, &contig);
      if (lcn == -2) return -1;
      uint64_t spanBytes = contig * bytesPerCluster - inCluster;       // contiguous real bytes from here
      if (chunk > spanBytes) chunk = spanBytes;
      if (lcn == -1) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }   // sparse hole

      int64_t n = readSpanData(vol, lcn, inCluster, spanBytes, out + done, chunk);
      if (n < 0) return -1;
      done += (uint64_t)n;
   }
   return (int64_t)done;
}

// ===========================================================================
// W8a read path: $ATTRIBUTE_LIST. When a file/dir spreads across several MFT records, the base
// record carries an $ATTRIBUTE_LIST mapping each attribute instance (type+name+startVCN) to the
// record that houses it. The helpers below follow that list and merge a non-resident attribute's
// runlist fragments (held in different records) into one decoded runlist, so the existing readers
// work unchanged. A single-record file (no $ATTRIBUTE_LIST) takes the fast path untouched.
// ===========================================================================

// Compares an attribute name (UTF-16LE) in a record against `name`/`nameLen`. Returns 1 on match.
static int attrNameMatches(const uint8_t *attr, uint32_t attrLength, const uint16_t *name, uint8_t nameLen)
{
   if (attr[ATTR_NAME_LENGTH] != nameLen) return 0;
   if (nameLen == 0) return 1;
   uint16_t nameOff = readLe16(attr + ATTR_NAME_OFFSET);
   if ((uint32_t)nameOff + (uint32_t)nameLen * 2 > attrLength) return 0;
   for (uint8_t i = 0; i < nameLen; i++)
      if (readLe16(attr + nameOff + i * 2) != name[i]) return 0;
   return 1;
}

// Finds the attribute instance in `record` matching (type, name, attribute-id). Like findAttribute
// but also matches the id, so a specific fragment named by an $ATTRIBUTE_LIST entry is selected even
// when a record holds several instances of the same type. Returns 1 (fills *out), 0 absent, -1 bad.
static int findAttributeInstance(const uint8_t *record, uint32_t recordSize, uint32_t type,
                                 const uint16_t *name, uint8_t nameLen, uint16_t attrId, NtfsAttr *out)
{
   uint32_t usedSize = readLe32(record + FILE_USED_SIZE);
   uint32_t limit    = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset   = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   for (int guard = 0; guard < 256; guard++) {
      if (offset + 4 > limit) return -1;
      uint32_t attrType = readLe32(record + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) return 0;
      if (offset + 16 > limit) return -1;
      uint32_t attrLength = readLe32(record + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) return -1;
      if (attrType == type && readLe16(record + offset + ATTR_ID_OFFSET) == attrId &&
          attrNameMatches(record + offset, attrLength, name, nameLen))
         return parseAttribute(record + offset, attrLength, out) ? 1 : -1;
      offset += attrLength;
   }
   return -1;
}

// Reads `want` bytes at `pos` from a decoded (possibly multi-fragment) runlist — the merged-runlist
// twin of readNonResident. Zero-fills sparse holes and bytes at/after validSize. Returns bytes read
// or -1. (Mirrors readNonResident exactly but maps VCN via the decoded runs[] instead of raw pairs.)
static int64_t readRuns(const NtfsVolume *vol, const NtfsRunEntry *runs, int runCount, uint64_t validSize,
                        uint64_t pos, uint8_t *out, uint64_t want)
{
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t done = 0;
   while (done < want) {
      uint64_t fileOffset = pos + done;
      uint64_t vcn        = fileOffset / bytesPerCluster;
      uint32_t inCluster  = (uint32_t)(fileOffset % bytesPerCluster);

      uint64_t chunk = want - done;
      if (fileOffset >= validSize) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }
      if (fileOffset + chunk > validSize) chunk = validSize - fileOffset;

      uint64_t contig = 0;
      int64_t  lcn = mapRunsSpan(runs, runCount, vcn, &contig);
      if (lcn == -2) return -1;
      uint64_t spanBytes = contig * bytesPerCluster - inCluster;
      if (chunk > spanBytes) chunk = spanBytes;
      if (lcn == -1) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }   // sparse / hole

      int64_t n = readSpanData(vol, lcn, inCluster, spanBytes, out + done, chunk);
      if (n < 0) return -1;
      done += (uint64_t)n;
   }
   return (int64_t)done;
}

// Merges every fragment of attribute (type,name) in a file into one decoded runlist, following the
// base record's $ATTRIBUTE_LIST. `baseBuf` holds the base record (number `baseRef`); extension
// records load into extRecord. Returns 0 (fills runs/count + sizes from the startVCN==0 fragment),
// 1 if the attribute is resident in a single record (caller reads its value directly), or -1 on
// error/refuse (non-resident $ATTRIBUTE_LIST, too many fragments/runs, malformed, unreadable). The
// fast path (no $ATTRIBUTE_LIST) decodes the base instance directly.
static int gatherRuns(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                      uint32_t type, const uint16_t *name, uint8_t nameLen,
                      NtfsRunEntry *runs, int maxRuns, int *runCount,
                      uint64_t *realSize, uint64_t *validSize, uint64_t *allocSize)
{
   uint32_t recSize = vol->mftRecordSize;
   *runCount = 0; *realSize = *validSize = *allocSize = 0;

   NtfsAttr listAttr;
   if (findAttribute(baseBuf, recSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1) {
      NtfsAttr a;                                            // no list: the whole attribute is in the base
      if (findAttribute(baseBuf, recSize, type, name, nameLen, &a) != 1) return -1;
      if (a.resident) return 1;
      if (a.startVcn != 0) return -1;
      *realSize = a.realSize; *validSize = a.validSize; *allocSize = a.allocatedSize;
      return decodeRuns(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, runs, maxRuns, runCount) == 0 ? 0 : -1;
   }
   if (!listAttr.resident) return -1;                        // non-resident $ATTRIBUTE_LIST: deferred (very rare)

   const uint8_t *L = listAttr.value;
   uint32_t Llen = listAttr.valueLength, off = 0;
   int frags = 0, sawResident = 0, sawFirst = 0;
   uint64_t expectVcn = 0;                                   // next fragment must start exactly here (tile, in order)
   for (int guard = 0; guard < 8192; guard++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;   // end / malformed: stop
      // does this list entry name our (type, name)?
      int match = (readLe32(L + off + AL_TYPE) == type && L[off + AL_NAME_LENGTH] == nameLen);
      if (match && nameLen > 0) {
         uint8_t no = L[off + AL_NAME_OFFSET];
         if ((uint32_t)no + (uint32_t)nameLen * 2 > elen) match = 0;
         for (uint8_t i = 0; i < nameLen && match; i++)
            if (readLe16(L + off + no + i * 2) != name[i]) match = 0;
      }
      if (match) {
         {
            uint64_t evcn = readLe64(L + off + AL_START_VCN);
            uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
            uint64_t eref = erefFull & MFT_REF_MASK;
            uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
            const uint8_t *rec = baseBuf;
            if (eref != baseRef) { if (readMftRecordByRef(vol, erefFull, extRecord) != 0) return -1; rec = extRecord; }
            NtfsAttr a;
            if (findAttributeInstance(rec, recSize, type, name, nameLen, eid, &a) != 1) return -1;
            if (a.resident) { sawResident = 1; break; }       // resident single instance: caller handles
            // Fragments must tile the VCN space contiguously and in order: the first names VCN 0 (and
            // carries the authoritative sizes), each next starts exactly where the previous ended. A
            // malformed list with a gap, overlap or out-of-order fragment is refused, never silently
            // zero-filled (gap) or shadowed (overlap).
            if (evcn != expectVcn) return -1;
            if (frags >= NTFS_MAX_EXTENTS) return -1;         // refuse before appending past the fragment cap
            frags++;
            if (evcn == 0) { sawFirst = 1; *realSize = a.realSize; *validSize = a.validSize; *allocSize = a.allocatedSize; }
            NtfsRunEntry frag[NTFS_MAX_FILE_RUNS]; int fc = 0;
            if (decodeRuns(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, frag, NTFS_MAX_FILE_RUNS, &fc) != 0) return -1;
            for (int i = 0; i < fc; i++) {
               if (*runCount >= maxRuns) return -1;
               runs[*runCount] = frag[i];
               runs[*runCount].vcn += evcn;                   // decoded VCNs are fragment-relative; make absolute
               expectVcn += frag[i].count;                    // advance the expected next-fragment VCN
               (*runCount)++;
            }
         }
      }
      off += elen;
   }
   if (sawResident) return 1;
   return (sawFirst && *runCount > 0) ? 0 : -1;               // require the startVCN==0 fragment (sizes set)
}

// Streams one fragment's mapping pairs (VCNs relative to fragBaseVcn) and appends to out[] the runs
// overlapping [winStart, winEnd), each clipped to the window (LCN advanced into the clip). Never stores
// more than the window's worth of runs, so a fragment with thousands of runs costs O(1) memory. Returns
// 0, or -1 on a malformed runlist or if the (bounded) window output overflows.
static int streamFragWindow(const uint8_t *rl, uint32_t rlLen, uint64_t fragBaseVcn,
                            uint64_t winStart, uint64_t winEnd, NtfsRunEntry *out, int maxOut, int *outCount)
{
   NtfsRunlist cur; openRunlist(&cur, rl, rlLen);
   uint64_t rvcn, rcount; int64_t rlcn; int rc;
   while ((rc = nextRun(&cur, &rvcn, &rlcn, &rcount)) == 1) {
      uint64_t a = fragBaseVcn + rvcn, b = a + rcount;
      if (b <= winStart) continue;                             // entirely before the window
      if (a >= winEnd) break;                                  // runs are VCN-ascending: nothing later overlaps
      uint64_t lo = a > winStart ? a : winStart;
      uint64_t hi = b < winEnd ? b : winEnd;
      if (*outCount >= maxOut) return -1;
      out[*outCount].vcn   = lo;
      out[*outCount].count = hi - lo;
      out[*outCount].lcn   = (rlcn < 0) ? -1 : (rlcn + (int64_t)(lo - a));   // advance the LCN into the clipped start
      (*outCount)++;
   }
   return (rc < 0) ? -1 : 0;
}

// Maps [vcnStart, vcnStart+vcnCount) of a (possibly $ATTRIBUTE_LIST-spanned) non-resident attribute to
// runs, WITHOUT materializing the whole runlist: it streams the mapping pairs of each covering fragment
// and keeps only the runs intersecting the window. This is what lets the driver read a large $LZNT1 file
// whose full runlist (~2 runs per compression unit) far exceeds NTFS_MAX_FILE_RUNS — each read maps one
// compression unit (<= cbClusters clusters) at a time. Housing extension records are read into extRecord;
// baseBuf must remain valid (the $ATTRIBUTE_LIST is read from it). Returns 0 (sets *outCount) or -1.
static int mapVcnWindow(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                        uint32_t type, const uint16_t *name, uint8_t nameLen,
                        uint64_t vcnStart, uint64_t vcnCount, NtfsRunEntry *out, int maxOut, int *outCount)
{
   uint32_t recSize = vol->mftRecordSize;
   uint64_t winEnd = vcnStart + vcnCount;
   *outCount = 0;

   NtfsAttr listAttr;
   if (findAttribute(baseBuf, recSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1) {
      NtfsAttr a;                                              // no list: one runlist in the base
      if (findAttribute(baseBuf, recSize, type, name, nameLen, &a) != 1 || a.resident) return -1;
      return streamFragWindow(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, 0,
                              vcnStart, winEnd, out, maxOut, outCount);
   }
   if (!listAttr.resident) return -1;                          // non-resident $ATTRIBUTE_LIST: unsupported

   const uint8_t *L = listAttr.value; uint32_t Llen = listAttr.valueLength, off = 0;
   for (int guard = 0; guard < 8192; guard++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;
      int match = (readLe32(L + off + AL_TYPE) == type && L[off + AL_NAME_LENGTH] == nameLen);
      if (match && nameLen > 0) {
         uint8_t no = L[off + AL_NAME_OFFSET];
         if ((uint32_t)no + (uint32_t)nameLen * 2 > elen) match = 0;
         for (uint8_t i = 0; i < nameLen && match; i++)
            if (readLe16(L + off + no + i * 2) != name[i]) match = 0;
      }
      if (match) {
         uint64_t evcn = readLe64(L + off + AL_START_VCN);
         if (evcn < winEnd) {                                  // this fragment may overlap the window
            uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
            uint64_t eref = erefFull & MFT_REF_MASK;
            uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
            const uint8_t *rec = baseBuf;
            if (eref != baseRef) { if (readMftRecordByRef(vol, erefFull, extRecord) != 0) return -1; rec = extRecord; }
            NtfsAttr a;
            if (findAttributeInstance(rec, recSize, type, name, nameLen, eid, &a) != 1) return -1;
            if (!a.resident &&
                streamFragWindow(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, evcn,
                                 vcnStart, winEnd, out, maxOut, outCount) != 0) return -1;
         }
      }
      off += elen;
   }
   return 0;
}

// W10a: reads `want` bytes at `pos` from an $LZNT1-compressed non-resident $DATA. The runlist is mapped
// one compression unit at a time via mapVcnWindow (baseBuf + baseRef locate it, following $ATTRIBUTE_LIST),
// so this works for files of any size/fragmentation. Each compression unit (cbClusters clusters) is sparse
// (leading hole -> zeros), uncompressed (no holes -> raw), or compressed (leading real clusters + trailing
// holes -> decode its 4 KiB sub-blocks). Reads past validSize are zeros. Returns bytes read (== want) / -1.
static int64_t readCompressed(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                              const uint16_t *name, uint8_t nameLen,
                              uint32_t cbClusters, uint64_t validSize, uint64_t pos, uint8_t *out, uint64_t want)
{
   uint32_t clusterBytes = vol->bytesPerCluster;
   uint64_t cbSize = (uint64_t)cbClusters * clusterBytes;
   if (cbClusters == 0 || cbSize == 0 || cbClusters > NTFS_MAX_CB_CLUSTERS) return -1;
   if (cbSize > 0x40000000u) return -1;                     // keep cbSize (and the uint32 offsets below) 31-bit-safe
   uint64_t done = 0;
   while (done < want) {
      uint64_t fileOff = pos + done;
      if (fileOff >= validSize) { memSet(out + done, 0, (int)(want - done)); done = want; break; }
      uint64_t unitVcn = (fileOff / cbSize) * cbClusters;       // first VCN of this compression unit
      uint32_t offInUnit = (uint32_t)(fileOff % cbSize);
      uint64_t chunk = cbSize - offInUnit;
      if (chunk > want - done) chunk = want - done;
      if (fileOff + chunk > validSize) chunk = validSize - fileOff;

      // map just this unit's clusters (bounded), then classify by counting leading real clusters
      NtfsRunEntry runs[NTFS_CB_MAX_RUNS]; int runCount = 0;
      if (mapVcnWindow(vol, baseBuf, baseRef, ATTR_DATA, name, nameLen, unitVcn, cbClusters, runs, NTFS_CB_MAX_RUNS, &runCount) != 0) return -1;

      int hasHole = 0; uint32_t realClusters = 0;
      for (uint32_t c = 0; c < cbClusters; c++) {
         int64_t l = mapVcnToLcn(runs, runCount, unitVcn + c);
         if (l < 0) { hasHole = 1; break; }
         realClusters++;
      }
      if (realClusters == 0) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }   // sparse unit

      if (!hasHole) {                                           // uncompressed unit: read raw
         if (readRuns(vol, runs, runCount, validSize, fileOff, out + done, chunk) != (int64_t)chunk) return -1;
         done += chunk; continue;
      }

      // compressed unit: walk its sub-blocks from the unit start; decode those overlapping the request.
      uint64_t unitByte = unitVcn * clusterBytes;
      uint64_t realBytes = (uint64_t)realClusters * clusterBytes;
      uint32_t compOff = 0;                                     // byte offset of the current sub-block in the unit
      uint32_t outUnitPos = 0;                                  // plaintext byte offset within the unit
      for (int sbGuard = 0; sbGuard < (int)(cbSize / NTFS_SB_SIZE) + 1 && outUnitPos < cbSize; sbGuard++) {
         if (compOff + 2 > realBytes) break;                    // no more sub-blocks
         uint32_t avail = (uint32_t)(realBytes - compOff);
         uint32_t rd = avail < (uint32_t)sizeof lzComp ? avail : (uint32_t)sizeof lzComp;
         if (readRuns(vol, runs, runCount, unitByte + realBytes, unitByte + compOff, lzComp, rd) != (int64_t)rd) return -1;
         uint16_t sbHdr = readLe16(lzComp);
         if (sbHdr == 0) break;                                 // end of compressed data
         uint32_t sbTotal = (uint32_t)(sbHdr & NTFS_SB_SIZE_MASK) + 3;
         if (sbTotal > rd) return -1;                           // sub-block runs past the real data
         if (lznt1DecompressSb(lzComp, sbTotal, lzPlain) != NTFS_SB_SIZE) return -1;
         // copy the part of this sub-block (plaintext [outUnitPos, outUnitPos+NTFS_SB_SIZE)) that
         // overlaps the requested [offInUnit, offInUnit+chunk)
         uint32_t sbStart = outUnitPos, sbEnd = outUnitPos + NTFS_SB_SIZE;
         uint32_t reqStart = offInUnit, reqEnd = offInUnit + (uint32_t)chunk;
         uint32_t lo = sbStart > reqStart ? sbStart : reqStart;
         uint32_t hi = sbEnd < reqEnd ? sbEnd : reqEnd;
         if (lo < hi) memCopy(out + done + (lo - reqStart), lzPlain + (lo - sbStart), (int)(hi - lo));
         compOff += sbTotal;
         outUnitPos += NTFS_SB_SIZE;
      }
      // any plaintext the unit didn't produce within the request reads as zero
      if (outUnitPos < offInUnit + (uint32_t)chunk) {
         uint32_t z0 = outUnitPos > offInUnit ? outUnitPos : offInUnit;
         uint32_t z1 = offInUnit + (uint32_t)chunk;
         if (z0 < z1) memSet(out + done + (z0 - offInUnit), 0, (int)(z1 - z0));
      }
      done += chunk;
   }
   return (int64_t)done;
}

// Parses the authoritative (lowest_vcn == 0) fragment of a (possibly $ATTRIBUTE_LIST-spanned) attribute
// into *out, WITHOUT decoding its runlist — so openFileByRef can learn a file's size and whether it is
// $LZNT1-compressed even when the runlist is far too large to decode whole. out->attr points into baseBuf
// or extRecord (valid until the next record read). Returns 1 resident, 0 non-resident, -1 not found/error.
static int firstFragmentInfo(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                             uint32_t type, const uint16_t *name, uint8_t nameLen, NtfsAttr *out)
{
   uint32_t recSize = vol->mftRecordSize;
   NtfsAttr listAttr;
   if (findAttribute(baseBuf, recSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1) {
      if (findAttribute(baseBuf, recSize, type, name, nameLen, out) != 1) return -1;
      return out->resident ? 1 : 0;
   }
   if (!listAttr.resident) return -1;
   const uint8_t *L = listAttr.value; uint32_t Llen = listAttr.valueLength, off = 0;
   for (int guard = 0; guard < 8192; guard++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;
      int match = (readLe32(L + off + AL_TYPE) == type && L[off + AL_NAME_LENGTH] == nameLen);
      if (match && nameLen > 0) {
         uint8_t no = L[off + AL_NAME_OFFSET];
         if ((uint32_t)no + (uint32_t)nameLen * 2 > elen) match = 0;
         for (uint8_t i = 0; i < nameLen && match; i++)
            if (readLe16(L + off + no + i * 2) != name[i]) match = 0;
      }
      // Only the lowest_vcn==0 extent's header carries the authoritative real/valid sizes, COMPRESSED/SPARSE
      // flags and compression_unit (later extents have compression_unit==0); that's the one we parse here.
      if (match && readLe64(L + off + AL_START_VCN) == 0) {
         uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
         uint64_t eref = erefFull & MFT_REF_MASK;
         uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
         const uint8_t *rec = baseBuf;
         if (eref != baseRef) { if (readMftRecordByRef(vol, erefFull, extRecord) != 0) return -1; rec = extRecord; }
         if (findAttributeInstance(rec, recSize, type, name, nameLen, eid, out) != 1) return -1;
         return out->resident ? 1 : 0;
      }
      off += elen;
   }
   return -1;
}

// Sets up an open file from its MFT reference and a $DATA stream name (W12a). `name`/`nameLen` select a
// named stream (file:stream); nameLen == 0 selects the unnamed main stream (the common case). Reads the
// record, finds that $DATA, and fills size / ValidDataLength / resident flag. Returns 0, -1 on error /
// no such stream (e.g. a directory, or a missing named stream), or -2 if the stream is encrypted.
static int openStreamByRef(NtfsFile *file, NtfsVolume *vol, uint64_t mftReference,
                           const uint16_t *name, uint8_t nameLen)
{
   memSet(file, 0, (int)sizeof(*file));
   file->vol          = vol;
   file->mftReference = mftReference;
   if (nameLen > 0) {                                         // remember the stream name so readNtfs re-finds it
      if (nameLen > 32) return -1;
      for (uint8_t i = 0; i < nameLen; i++) file->dataName[i] = name[i];
      file->dataNameLen = nameLen;
   }
   if (readMftRecordByRef(vol, mftReference, mftRecord) != 0) return -1;   // F048: masks + validates sequence

   NtfsAttr data, listAttr;
   int found = findAttribute(mftRecord, vol->mftRecordSize, ATTR_DATA, name, nameLen, &data);
   int haveList = (findAttribute(mftRecord, vol->mftRecordSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) == 1);
   // Self-contained base $DATA (the overwhelming common case): the base record holds the unnamed
   // $DATA, its first fragment (startVcn 0), and no $ATTRIBUTE_LIST relocating it. Fast path unchanged.
   if (found == 1 && !haveList) {
      file->resident   = data.resident;
      file->dataAttrId = data.attributeId;
      if (data.resident) {                                   // resident $DATA is never compressed/sparse
         file->size = data.valueLength; file->validSize = data.valueLength; file->compressed = 0;
         return 0;
      }
      if (data.startVcn != 0) return -1;
      file->size = data.realSize; file->validSize = data.validSize;
      if (readLe16(data.attr + ATTR_FLAGS_OFFSET) & ATTR_FLAG_ENCRYPTED) {   // EFS: opened-but-unsupported
         file->compressed = 1; return -2;
      }
      // W10a: an $LZNT1-compressed $DATA is readable via readCompressed; a sparse-only $DATA reads
      // through the normal reader (holes -> zeros). Both stay write-refused (file->compressed) until
      // W10b/W12. A plain file is fully writable.
      file->compUnitClusters = data.compUnitClusters;
      file->compressed = (data.compUnitClusters > 0 || data.compressed) ? 1 : 0;
      return 0;
   }

   // $DATA is relocated by an $ATTRIBUTE_LIST (resident-in-a-list-file, or spread across records). Peek the
   // authoritative (lowest_vcn 0) fragment first — without decoding the runlist — so a large $LZNT1 file
   // (whose runlist dwarfs NTFS_MAX_FILE_RUNS) is recognized as compressed and read by streaming.
   NtfsAttr first;
   int ff = firstFragmentInfo(vol, mftRecord, mftReference & MFT_REF_MASK, ATTR_DATA, name, nameLen, &first);
   if (ff < 0) return -1;                                   // no $DATA (directory) or malformed
   if (ff == 1) {                                           // resident $DATA (small; value lives in the base)
      if (found != 1) return -1;
      file->resident = 1; file->compressed = data.compressed; file->dataAttrId = data.attributeId;
      file->size = data.valueLength; file->validSize = data.valueLength;
      return file->compressed ? -2 : 0;
   }
   // non-resident, spanned. `first` holds the authoritative sizes + flags (attr ptr still valid).
   file->size = first.realSize; file->validSize = first.validSize; file->spanned = 1;
   if (readLe16(first.attr + ATTR_FLAGS_OFFSET) & ATTR_FLAG_ENCRYPTED) { file->compressed = 1; return -2; }
   if (first.compUnitClusters > 0) {                        // spanned + $LZNT1: read via readCompressed/mapVcnWindow
      if (first.compUnitClusters > NTFS_MAX_CB_CLUSTERS) { file->compressed = 1; return -2; }   // absurd unit: refuse
      file->compUnitClusters = first.compUnitClusters; file->compressed = 1;
      return 0;
   }
   // spanned, uncompressed: validate the merged runlist now (gatherRuns; reads use it too). A sparse-but-
   // not-LZNT1 $DATA also lands here (holes -> zeros via readRuns).
   NtfsRunEntry r[NTFS_MAX_FILE_RUNS]; int rc = 0; uint64_t rs, vs, as;
   int g = gatherRuns(vol, mftRecord, mftReference & MFT_REF_MASK, ATTR_DATA, name, nameLen, r, NTFS_MAX_FILE_RUNS, &rc, &rs, &vs, &as);
   if (g != 0) return -1;                                   // refused (e.g. too fragmented) or malformed
   file->resident = 0; file->compressed = first.compressed; file->size = rs; file->validSize = vs;
   return 0;
}

// Unnamed (main) $DATA stream — the overwhelming common case and every internal caller. Wraps
// openStreamByRef with no stream name.
static int openFileByRef(NtfsFile *file, NtfsVolume *vol, uint64_t mftReference)
{
   return openStreamByRef(file, vol, mftReference, 0, 0);
}

// ===========================================================================
// W9a — $LogFile clean/dirty detection.
//
// The on-disk spec documents only the restart PAGE header ("RSTR"/"CHKD" + USA + page sizes +
// restart_area_offset); the journal's redo/undo records are marked "TODO" and are undocumented, so
// replay is impossible to spec (and ntfs-3g, the production reference, doesn't replay either — it only
// detects clean/dirty). We parse the two restart pages, USA-validate them (the spec's multi-sector
// protection), and read the restart AREA flags (ntfs-3g/MSDN layout) to decide whether Windows left the
// journal clean. Used only to TIGHTEN the writable decision at mount; never writes the volume.
// ===========================================================================
#define NTFS_LOG_CLEAN    0
#define NTFS_LOG_DIRTY    1
#define NTFS_LOG_UNKNOWN  2

// Restart-page-header field offsets are shared with logParseRestartPage (LOG_RSTR_* constants); the
// restart-AREA structure below is distinct.
#define RA_CURRENT_LSN           0    // restart area: current LSN / client-in-use list / flags
#define RA_CLIENT_IN_USE_LIST    12
#define RA_FLAGS                 14
#define RA_FILE_SIZE_OFS         24   // spec: restart_area_offset + 24 must be <= 510 or the page is corrupt
#define RESTART_VOLUME_IS_CLEAN  0x0002
#define LOGFILE_NO_CLIENT        0xFFFF

// Evaluates one already-read restart page (`page`, `pageLen` bytes). Returns NTFS_LOG_* and, when the
// page is a valid restart page, sets *currentLsn to its restart-area current LSN (to pick the newer of
// the two pages). Pure parsing + bounds checks; no I/O.
static int evalRestartPage(uint8_t *page, uint32_t pageLen, uint32_t sectorSize, uint64_t *currentLsn)
{
   *currentLsn = 0;
   // All-0xFF ⇒ an emptied $LogFile ⇒ was clean before it was emptied.
   int allFF = 1;
   for (uint32_t i = 0; i < pageLen; i++) if (page[i] != 0xFF) { allFF = 0; break; }
   if (allFF) return NTFS_LOG_CLEAN;

   // Parse + validate the restart-page header (signature + fields) through the single audited parser.
   NtfsLogRestartPage hdr;
   if (logParseRestartPage(page, pageLen, &hdr) != 0) return NTFS_LOG_UNKNOWN;   // short or unknown signature
   if (page[0] == 'R' && page[1] == 'C') return NTFS_LOG_UNKNOWN;                // "RCRD" is a record page, not a restart page
   int isChkd = (page[0] == 'C');                            // "CHKD" vs "RSTR"
   // USA fix-up (the spec's multi-sector transfer protection) over the whole restart page. The header
   // fields read above sit below the first fix-up point (byte 510), so they are identical pre/post-fixup;
   // the restart-area fields read below require the unscrambled page.
   if (applyUsaFixup(page, pageLen, sectorSize) != 0) return NTFS_LOG_UNKNOWN;

   uint32_t raOff = hdr.restartOffset;                       // restart-area offset (page-header @24)
   // Spec: if restart_area_offset + (offset of file_size, 24) > 510 the page is corrupt (the USA would
   // have clobbered the field). Also keep the whole restart area we read within the page.
   if (raOff + RA_FILE_SIZE_OFS > 510) return NTFS_LOG_UNKNOWN;
   if ((uint64_t)raOff + RA_FLAGS + 2 > pageLen) return NTFS_LOG_UNKNOWN;

   // Selection LSN: for a CHKD (chkdsk-modified) page the authoritative sequence number is the page
   // header's chkdsk_lsn (+8); only an RSTR page uses the restart area's current_lsn (ntfs-3g logfile.c).
   *currentLsn = isChkd ? hdr.chkdskLsn : readLe64(page + raOff + RA_CURRENT_LSN);
   uint16_t inUse = readLe16(page + raOff + RA_CLIENT_IN_USE_LIST);
   uint16_t flags = readLe16(page + raOff + RA_FLAGS);
   // ntfs-3g ntfs_is_logfile_clean: an open journal (a client in use) without the CLEAN bit ⇒ unclean.
   if (inUse != LOGFILE_NO_CLIENT && !(flags & RESTART_VOLUME_IS_CLEAN)) return NTFS_LOG_DIRTY;
   return NTFS_LOG_CLEAN;
}

// Reads $LogFile's two restart pages and returns NTFS_LOG_CLEAN / _DIRTY / _UNKNOWN. Uses the shared
// mftRecord + dirRecord scratch (caller is mountNtfs, single-threaded under the backend lock at mount).
static int checkLogFileClean(NtfsVolume *vol)
{
   if (readMftRecord(vol, MFT_RECORD_LOGFILE, mftRecord) != 0) return NTFS_LOG_UNKNOWN;   // F048 ok: $LogFile by fixed system record number (seq-0 convention)
   NtfsAttr data;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_DATA, 0, 0, &data) != 1 || data.resident)
      return NTFS_LOG_UNKNOWN;                               // $LogFile $DATA is always non-resident

   // Page size: the header's log_page_size (default 4096). Read page 0 first to learn it, bounded to our
   // scratch (NTFS_MAX_RECORD). USA validation needs the whole page, so we require it to fit.
   uint32_t pageSize = 4096;
   if (pageSize > NTFS_MAX_RECORD) return NTFS_LOG_UNKNOWN;

   int verdict = NTFS_LOG_UNKNOWN; uint64_t bestLsn = 0; int haveVerdict = 0;
   for (int pg = 0; pg < 2; pg++) {
      uint8_t *page = (pg == 0) ? dirRecord : extRecord;     // two distinct scratch buffers
      if (readNonResident(vol, &data, data.validSize, (uint64_t)pg * pageSize, page, pageSize) != (int64_t)pageSize)
         continue;                                           // page absent/short — skip
      uint64_t lsn = 0;
      int v = evalRestartPage(page, pageSize, vol->bytesPerSector, &lsn);
      if (v == NTFS_LOG_UNKNOWN) continue;                   // unparseable page — ignore, try the other
      // Prefer the more recent (strictly larger LSN) valid page, matching ntfs-3g. On an exact LSN tie
      // the safety bias is toward DIRTY: a false "clean" verdict is the dangerous one (it would let us
      // write over a volume mid-recovery), so an ambiguous pair never downgrades a DIRTY to CLEAN.
      if (!haveVerdict || lsn > bestLsn) { verdict = v; bestLsn = lsn; haveVerdict = 1; }
      else if (lsn == bestLsn && v == NTFS_LOG_DIRTY) verdict = NTFS_LOG_DIRTY;
   }
   return haveVerdict ? verdict : NTFS_LOG_UNKNOWN;
}

// ===========================================================================
// Mount / unmount.
// ===========================================================================
int mountNtfs(NtfsVolume *vol, int drive)
{
   memSet(vol, 0, (int)sizeof(*vol));

   // device geometry
   uint64_t deviceId = getUsbDeviceId(drive);
   StorageDeviceInfo info;
   int      haveInfo         = (getStorageInfo(deviceId, &info) == 0);
   uint32_t deviceSectorSize = haveInfo ? info.sectorSize  : 512;
   uint64_t deviceSectors    = haveInfo ? info.sectorCount : 0;   // 0 = unknown (skip the device-size bound)
   if (deviceSectorSize == 0 || deviceSectorSize > NTFS_MAX_SECTOR) return NTFS_MOUNT_NOT_READY;

   // open + settle (lv2 faults if the first read lands too soon after open)
   int storageHandle;
   if (openStorage(deviceId, &storageHandle) < 0) return NTFS_MOUNT_NOT_READY;
   ioRegisterHandle(storageHandle, deviceSectorSize);   // jf-port: for bounced (unaligned) transfers
   sys_timer_usleep(SYSIO_SETTLE_US);

   // read LBA 0 and locate the NTFS volume (superfloppy, MBR or GPT partition)
   if (readSectors(storageHandle, 0, 1, bootSector) != 0) {
      closeStorage(storageHandle);
      return NTFS_MOUNT_NOT_READY;
   }
   uint64_t volStart = 0;
   if (!locateNtfsVolume(storageHandle, bootSector, scanScratch, vbrScratch,
                         deviceSectorSize, deviceSectors, &volStart)) {
      closeStorage(storageHandle);
      return NTFS_MOUNT_NOT_NTFS;
   }

   // validate + parse the (untrusted) geometry
   if (!parseNtfsBoot(bootSector, deviceSectorSize, deviceSectors, volStart, vol)) {
      closeStorage(storageHandle);
      return NTFS_MOUNT_NOT_NTFS;
   }

   // commit volume state
   vol->storageHandle    = storageHandle;
   vol->cacheEpoch       = (mountEpoch + 1) ? ++mountEpoch : (mountEpoch = 1);   // fresh cache key; skip 0
   vol->drive            = (uint8_t)drive;
   vol->deviceSectorSize = deviceSectorSize;
   vol->partitionOffset  = volStart;
   vol->allocHint        = 0;
   vol->mounted          = 1;

   // bootstrap check: $MFT's own record (0) must read, fix up and validate as a FILE record. A
   // volume whose $MFT entry is unreadable or garbage is not usable NTFS; rejecting here also
   // exercises the S2 record-read + USA-fixup path at every real mount.
   if (readMftRecordBootstrap(vol, MFT_RECORD_MFT, mftRecord) != 0) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }

   // S3: $MFT's own unnamed $DATA must be present and non-resident (the MFT data always is). This
   // exercises the attribute walk on the real volume and records the MFT data size (record count)
   // for the S4 runlist decode.
   NtfsAttr mftData;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_DATA, 0, 0, &mftData) != 1 || mftData.resident) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   vol->mftDataSize = mftData.realSize;

   // S4: decode $MFT's runlist so any record is reachable, then prove the pipeline by reading the
   // root directory record (#5) via that runlist and confirming it's an in-use directory FILE
   // record. (decode reads from mftRecord, so it must run before readMftRecord overwrites it.)
   if (decodeMftRunlist(vol, &mftData) != 0) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   if (readMftRecord(vol, MFT_RECORD_ROOT, mftRecord) != 0 ||   // F048 ok: root dir by fixed system record number (seq-0 convention)
       !(readLe16(mftRecord + FILE_FLAGS) & FILE_FLAG_DIRECTORY)) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }

   // S5: read the $Boot file's non-resident $DATA and confirm its first sector equals the VBR we
   // already loaded. This byte-diffs the file-read path against a known reference on the real
   // volume (so a broken read engine fails the mount rather than silently returning bad bytes).
   NtfsFile bootFile;
   uint8_t firstSector[512];
   if (openFileByRef(&bootFile, vol, MFT_RECORD_BOOT) != 0) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   if (readNtfs(&bootFile, firstSector, 512) != 512) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   for (int i = 0; i < 512; i++) {
      if (firstSector[i] != bootSector[i]) {
         closeStorage(storageHandle);
         vol->mounted = 0;
         return NTFS_MOUNT_NOT_NTFS;
      }
   }

   // W1 + W9a writability gate. A dirty volume (unclean Windows shutdown or a prior interrupted write)
   // mounts read-only so we never write over a possibly-inconsistent volume; a clean volume is writable
   // and we'll re-arm the flag on the first write. The signal is the union of two sources:
   //   (1) $Volume's VOLUME_IS_DIRTY flag — Windows' primary "needs chkdsk" bit (the W1 gate), and
   //   (2) $LogFile's restart area — W9a: if the journal shows an open transaction with no clean bit,
   //       Windows left pending recovery only it can perform, so we must not write. An unparseable or
   //       empty journal adds no signal (we defer to the $Volume flag); only a clearly DIRTY journal
   //       forces read-only on its own.
   vol->writable = 1;
   vol->volumeDirty = 0;
   vol->versionMajor = 0;
   vol->versionMinor = 0;
   vol->label[0] = '\0';                                       // $VOLUME_NAME label (empty -> chooseSegment falls back)
   if (readMftRecord(vol, MFT_RECORD_VOLUME, mftRecord) == 0) {   // F048 ok: $Volume by fixed system record number (seq-0 convention)
      NtfsAttr volumeInfo;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_VOLUME_INFORMATION, 0, 0, &volumeInfo) == 1 &&
          volumeInfo.resident && volumeInfo.valueLength >= VOLINFO_FLAGS_OFFSET + 2) {
         vol->versionMajor = volumeInfo.value[VOLINFO_MAJOR_OFFSET];   // NTFS version, e.g. 3.1
         vol->versionMinor = volumeInfo.value[VOLINFO_MINOR_OFFSET];
         if (readLe16(volumeInfo.value + VOLINFO_FLAGS_OFFSET) & VOLUME_FLAG_DIRTY)
            vol->writable = 0;
      } else {
         vol->writable = 0;   // jf-port: no readable $VOLUME_INFORMATION: the dirty flag is unknown -> read only
      }

      // $VOLUME_NAME (type 0x60, resident UTF-16LE) -> UTF-8 display label, used as the mount segment
      // (parity with exFAT). Absent/empty leaves vol->label empty so chooseSegment uses "ntfs<port>".
      NtfsAttr volumeName;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_VOLUME_NAME, 0, 0, &volumeName) == 1 && volumeName.resident) {
         uint16_t units[64];
         uint32_t count = volumeName.valueLength / 2;
         if (count > 63) count = 63;
         for (uint32_t i = 0; i < count; i++) units[i] = readLe16(volumeName.value + i * 2);
         units[count] = 0;
         utf16ToUtf8(units, vol->label, (int)sizeof(vol->label));
      }
   } else {
      vol->writable = 0;   // jf-port: $Volume unreadable (a torn record): its dirty flag is unknown -> read only
   }
   if (vol->writable && checkLogFileClean(vol) == NTFS_LOG_DIRTY)   // W9a: honor a dirty journal
      vol->writable = 0;

   // Cache $Bitmap's runlist so the free-cluster scan and cluster (de)allocation reach bitmap sectors
   // directly, instead of re-opening and re-reading $Bitmap's MFT record on every block. Cached on
   // read-only mounts too: countNtfsFreeClusters below uses it, and gating this on `writable` meant
   // any read-only volume paid the slow path. If it can't be cached (a pathologically fragmented
   // bitmap) writes are refused and the scan falls back to the file engine.
   vol->bitmapRunCount = 0;
   vol->bitmapDataSize = 0;
   if (readMftRecord(vol, MFT_RECORD_BITMAP, mftRecord) == 0) {
      NtfsRunEntry runs[NTFS_MFT_RUNS_MAX]; int runCount = 0; uint64_t realSize, validSize, allocSize;
      if (gatherRuns(vol, mftRecord, MFT_RECORD_BITMAP, ATTR_DATA, 0, 0, runs, NTFS_MFT_RUNS_MAX, &runCount,
                     &realSize, &validSize, &allocSize) == 0) {
         for (int i = 0; i < runCount; i++) vol->bitmapRuns[i] = runs[i];
         vol->bitmapRunCount = runCount;
         vol->bitmapDataSize = realSize;   // bounds the scan: never read past $Bitmap's own data
      }
   }
   if (vol->bitmapRunCount == 0) vol->writable = 0;   // can't cache $Bitmap -> refuse writes

   if (countNtfsFreeClusters(vol, &vol->freeClusters) != 0)   // seed once; maintained on alloc/free thereafter
      vol->freeClusters = 0;

   return NTFS_MOUNT_OK;
}

void unmountNtfs(NtfsVolume *vol)
{
   if (!vol->mounted) return;
   if (vol->volumeDirty && vol->writable && setVolumeDirty(vol, 0) == 0)   // clean unmount clears the flag
      vol->volumeDirty = 0;
   closeStorage(vol->storageHandle);
   vol->mounted = 0;
}

// ===========================================================================
// Directory traversal ($INDEX_ROOT + $INDEX_ALLOCATION, the $I30 index). Entries
// are enumerated iteratively across the root node and every index block (no
// recursive B-tree descent): each file's key appears exactly once in the tree, so
// visiting every node and emitting every real entry lists each file once.
// ===========================================================================

// 100ns ticks between 1601-01-01 and 1970-01-01; FILETIME (UTC) -> unix seconds.
static uint64_t filetimeToUnix(uint64_t filetime)
{
   uint64_t epoch = 116444736000000000ull;
   return filetime > epoch ? (filetime - epoch) / 10000000ull : 0;
}

// W11: civil UTC date -> days since 1970-01-01 (proleptic Gregorian; Howard Hinnant's algorithm).
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
   y -= (m <= 2);
   int64_t era = (y >= 0 ? y : y - 399) / 400;
   unsigned yoe = (unsigned)(y - era * 400);                       // [0, 399]
   unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;   // [0, 365]
   unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        // [0, 146096]
   return era * 146097 + (int64_t)doe - 719468;
}

// W11: civil UTC datetime -> NTFS FILETIME (100 ns ticks since 1601-01-01 UTC). Pure; host unit-tested
// against known anchors (1601-01-01 -> 0, 1970-01-01 -> 116444736000000000).
// jf-port: unused since nowFiletime reads lv2's clock as unix seconds; kept as upstream wrote it.
static __attribute__((unused)) uint64_t civilToFiletime(int y, int mo, int d, int h, int mi, int s, int us)
{
   int64_t days = daysFromCivil(y, (unsigned)mo, (unsigned)d);
   int64_t secs = days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
   return (uint64_t)(secs * 10000000ll + 116444736000000000ll) + (uint64_t)us * 10ull;
}

// Fills *info and the UTF-8 name from a $FILE_NAME key (caller bounds-checked the key length).
static void fillFileNameInfo(const uint8_t *key, uint64_t fileRef, char *name, int nameCap, NtfsInfo *info)
{
   uint32_t fnFlags   = readLe32(key + FN_FLAGS);
   info->isDir        = (fnFlags & FN_FLAG_DIRECTORY) != 0;
   info->isReparse    = (fnFlags & FN_FLAG_REPARSE) != 0;   // W12b: symlink/junction/placeholder marker
   info->reparseTag   = 0;                                  // the tag lives in $REPARSE_POINT; statNtfs fills it
   info->attributes   = fnFlags;                            // DOS/Win32 FILE_ATTRIBUTE_* flags (read-only/hidden/system/...)
   info->size         = readLe64(key + FN_REAL_SIZE);
   info->validSize    = info->size;
   info->mtime        = filetimeToUnix(readLe64(key + FN_MODIFIED_TIME));
   info->mftReference = fileRef;

   uint8_t nameLength = key[FN_NAME_LENGTH];
   uint16_t units[256];
   for (uint8_t i = 0; i < nameLength; i++) units[i] = readLe16(key + FN_NAME + (uint32_t)i * 2);   // LE -> host
   units[nameLength] = 0;
   utf16ToUtf8(units, name, nameCap);
}

// Scans one index node for the next listable entry, resuming at *offset (a byte offset from the
// node header start). Skips the DOS short-name twin and the reserved system files, stops at the
// node's last-entry marker. Returns 1 (fills name/info, advances *offset), 0 at end of node.
static int scanIndexNode(const uint8_t *node, const uint8_t *hardEnd, uint32_t *offset,
                         char *name, int nameCap, NtfsInfo *info)
{
   uint32_t entriesOffset = readLe32(node + IDXNODE_ENTRIES_OFFSET);
   uint32_t usedSize      = readLe32(node + IDXNODE_USED_SIZE);
   uint32_t bufCap        = (uint32_t)(hardEnd - node);   // usedSize is attacker-controlled: never trust it past
   if (usedSize > bufCap) usedSize = bufCap;              // the real buffer, or an entry's key reads out of bounds
   uint32_t pos = *offset ? *offset : entriesOffset;

   for (int guard = 0; guard < 4096; guard++) {
      const uint8_t *entry = node + pos;
      if (pos + 16 > usedSize || entry + 16 > hardEnd) return 0;             // no room for an entry header
      uint16_t entryLength = readLe16(entry + IDXENTRY_LENGTH);
      uint16_t entryFlags  = readLe16(entry + IDXENTRY_FLAGS);
      if (entryFlags & IDXENTRY_FLAG_LAST) return 0;                         // last entry: end of node
      if (entryLength < 16 || pos + entryLength > usedSize) return 0;        // malformed -> stop
      uint16_t keyLength = readLe16(entry + IDXENTRY_KEY_LENGTH);
      uint32_t nextPos = pos + entryLength;

      // emit only a real, listable $FILE_NAME (skip DOS twins and reserved system files)
      const uint8_t *key = entry + IDXENTRY_KEY;
      uint64_t fileRef = readLe64(entry + IDXENTRY_FILE_REF);
      if (keyLength >= FN_MIN_SIZE && (uint32_t)16 + keyLength <= entryLength) {
         uint8_t nameLength = key[FN_NAME_LENGTH];
         if ((uint32_t)FN_NAME + (uint32_t)nameLength * 2 <= keyLength &&
             key[FN_NAMESPACE] != FN_NAMESPACE_DOS &&
             (fileRef & MFT_REF_MASK) >= NTFS_FIRST_USER_RECORD) {
            fillFileNameInfo(key, fileRef, name, nameCap, info);
            *offset = nextPos;
            return 1;
         }
      }
      pos = nextPos;
   }
   return 0;
}

// ===========================================================================
// Public read API - stubbed until S2-S7 land. Each returns a clean error so the
// VFS surface is wired and a probed NTFS volume mounts without breaking the
// router; browsing/reading becomes functional as the parsing stages land.
// ===========================================================================
// Number of $INDEX_ALLOCATION blocks worth scanning. Blocks past the real allocation read back as
// zeros (not errors), so a corrupt realSize/allocatedSize would otherwise spin the scan loop ~2^52
// times under the backend lock. Clamp to the real allocation and hard-cap by the volume size, so any
// header corruption still terminates. On a healthy volume allocatedSize >= realSize, so this is realSize.
static uint64_t indexBlockCount(const NtfsVolume *vol, const NtfsAttr *alloc, uint32_t blockSize)
{
   if (!blockSize) return 0;
   uint64_t bytes  = alloc->realSize < alloc->allocatedSize ? alloc->realSize : alloc->allocatedSize;
   uint64_t blocks = bytes / blockSize;
   uint64_t cap    = (vol->totalSectors * vol->bytesPerSector) / blockSize;   // can't exceed the volume
   return blocks < cap ? blocks : cap;
}

void openNtfsDir(NtfsDir *dir, const NtfsVolume *vol, uint64_t dirReference)
{
   memSet(dir, 0, (int)sizeof(*dir));
   dir->vol = vol;
   dir->dirReference = dirReference;
   dir->inRoot = 1;   // start in the resident $INDEX_ROOT node, then move to $INDEX_ALLOCATION blocks
}

// Returns one directory entry per call. Re-reads the directory's record and index block each call
// (the shared buffers can be reused between calls), resuming from the iterator position. The walk
// is: the $INDEX_ROOT node first, then every $INDEX_ALLOCATION block in turn.
int readNtfsDir(NtfsDir *dir, char *name, int nameCap, NtfsInfo *info)
{
   NtfsVolume *vol = (NtfsVolume *)dir->vol;
   if (!vol || !vol->mounted) { dir->ioError = 1; return 0; }

   for (;;) {
      // re-read the directory's FILE record and locate its $I30 index attributes
      if (readMftRecordByRef(vol, dir->dirReference, mftRecord) != 0) { dir->ioError = 1; return 0; }

      // phase 1: the resident $INDEX_ROOT node
      if (dir->inRoot) {
         NtfsAttr root;
         if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident)
            return 0;   // a directory without a readable $INDEX_ROOT: nothing to list
         const uint8_t *node = root.value + IDXROOT_NODE_HEADER;
         const uint8_t *hardEnd = root.value + root.valueLength;
         if (scanIndexNode(node, hardEnd, &dir->entryOffset, name, nameCap, info)) return 1;
         dir->inRoot = 0; dir->indexVcn = 0; dir->entryOffset = 0;   // root exhausted -> index blocks
         continue;
      }

      // phase 2: the $INDEX_ALLOCATION blocks (absent for a small, root-only directory)
      NtfsAttr alloc;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &alloc) != 1 ||
          alloc.resident)
         return 0;   // no large index -> done
      uint32_t blockSize = vol->indexRecordSize;
      uint64_t totalBlocks = indexBlockCount(vol, &alloc, blockSize);

      while (dir->indexVcn < totalBlocks) {
         int64_t got = readNonResident(vol, &alloc, alloc.validSize, dir->indexVcn * blockSize, indexBuffer, blockSize);
         if (got != (int64_t)blockSize) { dir->ioError = 1; return 0; }
         // a valid, in-use block is an "INDX" record that fixes up; skip anything else
         if (indexBuffer[0] == 'I' && indexBuffer[1] == 'N' && indexBuffer[2] == 'D' && indexBuffer[3] == 'X' &&
             applyUsaFixup(indexBuffer, blockSize, vol->bytesPerSector) == 0) {
            const uint8_t *node = indexBuffer + INDX_NODE_HEADER;
            const uint8_t *hardEnd = indexBuffer + blockSize;
            if (scanIndexNode(node, hardEnd, &dir->entryOffset, name, nameCap, info)) return 1;
         }
         dir->indexVcn++; dir->entryOffset = 0;
      }
      return 0;   // all blocks consumed
   }
}

void closeNtfsDir(NtfsDir *dir) { (void)dir; }

// Resolves an in-volume path ("/", "/a", "/a/b/c") to its entry metadata by walking it component
// by component from the root, matching each name case-insensitively (ASCII) against the parent's
// directory index. Iterative and bounded (no recursion). Returns 0 with *info filled, -1 if any
// component is missing or a non-directory is descended into. This lookup scan is ASCII case-fold only
// (a non-ASCII name differing only by case may not match - a benign miss, never corruption); the
// write-path B-tree collation in compareFileNameKeys does full $UpCase folding so inserts stay ordered.
static int resolvePath(const NtfsVolume *vol, const char *path, NtfsInfo *info)
{
   memSet(info, 0, (int)sizeof(*info));
   info->isDir = 1;
   info->mftReference = MFT_RECORD_ROOT;

   const char *cursor = path;
   while (*cursor) {
      while (*cursor == '/') cursor++;                  // skip separators
      if (!*cursor) break;
      char component[256];
      int length = 0;
      while (*cursor && *cursor != '/' && length < 255) component[length++] = *cursor++;
      component[length] = '\0';
      if (!info->isDir) return -1;                      // a path component under a non-directory

      // scan the current directory for a case-insensitive name match
      NtfsDir dir;
      openNtfsDir(&dir, vol, info->mftReference);
      char name[256];
      NtfsInfo entry;
      int found = 0;
      while (readNtfsDir(&dir, name, (int)sizeof(name), &entry) == 1) {
         if (strCmpICase(name, component) == 0) { *info = entry; found = 1; break; }
      }
      if (dir.ioError || !found) return -1;
   }
   return 0;
}

void seekNtfs(NtfsFile *file, uint64_t position);   // defined later; used by readNtfsSecurityDescriptor below

int statNtfs(const NtfsVolume *vol, const char *path, NtfsInfo *info)
{
   if (resolvePath(vol, path, info) != 0) return -1;
   // Read the target record once to refine `info` from the record's own attributes (resolvePath
   // only has the $FILE_NAME / $I30 index copy, which can be stale):
   //  - $STANDARD_INFORMATION @0x20 holds the AUTHORITATIVE DOS attribute flags. Windows updates SI
   //    on SetFileAttributes but does not eagerly push the change into the index key, so SI wins for
   //    read-only/hidden/system reporting. (Also fixes the synthetic root entry, which has no FN key.)
   //  - W12b: a reparse point's tag (first 4 bytes of $REPARSE_POINT) distinguishes symlink/junction/etc.
   // Best-effort: on any read/parse miss, info keeps the index-copy attributes and reparseTag stays 0.
   if (readMftRecordByRef(vol, info->mftReference, mftRecord) == 0) {
      NtfsAttr si;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_STANDARD_INFORMATION, 0, 0, &si) == 1 &&
          si.resident && si.valueLength >= 0x24)
         info->attributes = readLe32(si.value + 0x20);   // canonical FILE_ATTRIBUTE_* flags
      if (info->isReparse) {
         NtfsAttr rp;
         if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_REPARSE_POINT, 0, 0, &rp) == 1) {
            if (rp.resident && rp.valueLength >= 4) {
               info->reparseTag = readLe32(rp.value);
            } else if (!rp.resident) {                      // non-resident $REPARSE_POINT: read the 4-byte tag
               uint8_t tagbuf[8];
               if (readNonResident(vol, &rp, rp.validSize, 0, tagbuf, 4) == 4)
                  info->reparseTag = readLe32(tagbuf);
            }
         }
      }
   }

   // The index $FILE_NAME size copy lags the live $DATA (it is only resynced at close, syncFileNameSizes),
   // so a written-but-not-yet-resynced file lists with a stale/small size. Report the authoritative current
   // $DATA size instead. Directories have no $DATA -> keep the index value. Clobbers mftRecord.
   if (!info->isDir) {
      NtfsAttr data;
      uint64_t housingRef;
      if (findDataAnywhere((NtfsVolume *)vol, info->mftReference & MFT_REF_MASK, mftRecord, &data, &housingRef) == 0) {
         info->size      = data.resident ? data.valueLength : data.realSize;
         info->validSize = info->size;
      }
   }
   return 0;
}

// Validate a $LogFile restart-page signature ("RSTR"/"RCRD"/"CHKD", asciidoc L2164) and parse the
// RESTART_PAGE_HEADER fields (L2167-L2178). Returns 0 on a recognized page, -1 if short or the
// signature is unknown. The USA itself is referenced by usaOffset/usaCount but not applied here.
int logParseRestartPage(const uint8_t *data, uint32_t len, NtfsLogRestartPage *out)
{
   if (len < LOG_RSTR_MIN) return -1;
   const uint8_t *s = data + LOG_RSTR_SIG_OFFSET;
   int sigOk = (s[0]=='R' && s[1]=='S' && s[2]=='T' && s[3]=='R') ||   // RSTR
               (s[0]=='R' && s[1]=='C' && s[2]=='R' && s[3]=='D') ||   // RCRD
               (s[0]=='C' && s[1]=='H' && s[2]=='K' && s[3]=='D');     // CHKD
   if (!sigOk) return -1;
   if (out) {
      out->usaOffset      = readLe16(data + LOG_RSTR_USA_OFFSET_OFFSET);
      out->usaCount       = readLe16(data + LOG_RSTR_USA_COUNT_OFFSET);
      out->chkdskLsn      = readLe64(data + LOG_RSTR_CHKDSK_LSN_OFFSET);
      out->systemPageSize = readLe32(data + LOG_RSTR_SYS_PAGE_SIZE_OFFSET);
      out->logPageSize    = readLe32(data + LOG_RSTR_LOG_PAGE_SIZE_OFFSET);
      out->restartOffset  = readLe16(data + LOG_RSTR_RESTART_OFFSET_OFFSET);
      out->minorVersion   = readLe16(data + LOG_RSTR_MINOR_VER_OFFSET);
      out->majorVersion   = readLe16(data + LOG_RSTR_MAJOR_VER_OFFSET);
   }
   return 0;
}

int openNtfs(NtfsFile *file, NtfsVolume *vol, const char *path)
{
   // W12a: an optional "file:stream" suffix in the leaf selects a named $DATA stream. ':' is illegal in
   // NTFS names, so the first ':' after the last '/' is unambiguously the stream separator. A trailing
   // ":$DATA" attribute-type suffix is stripped; "file::$DATA" (empty stream) means the unnamed stream.
   char buf[512];
   int n = 0; while (path[n] && n < (int)sizeof(buf) - 1) { buf[n] = path[n]; n++; }
   buf[n] = '\0';
   int lastSlash = -1; for (int i = 0; i < n; i++) if (buf[i] == '/') lastSlash = i;
   int colon = -1; for (int i = lastSlash + 1; i < n; i++) if (buf[i] == ':') { colon = i; break; }

   uint16_t streamName[32]; uint8_t streamLen = 0;
   if (colon >= 0) {
      buf[colon] = '\0';                                  // terminate the file path at the stream separator
      const char *s = buf + colon + 1;
      int sl = 0; while (s[sl]) sl++;
      if (sl >= 6 && strCmpICase(s + sl - 6, ":$DATA") == 0) sl -= 6;   // strip the ":$DATA" type suffix
      if (sl > 32) return -1;                             // stream name too long for our bounded buffer
      for (int i = 0; i < sl; i++) streamName[i] = (uint8_t)s[i];        // ASCII -> UTF-16 (ADS names are ASCII)
      streamLen = (uint8_t)sl;
   }

   NtfsInfo info;
   if (resolvePath(vol, buf, &info) != 0) return -1;
   if (info.isReparse) return -1;                         // W12b: a reparse point is not an ordinary file
   if (info.isDir) return -1;                             // can't open a directory as a file
   return openStreamByRef(file, vol, info.mftReference, streamLen ? streamName : 0, streamLen);
}

int readNtfs(NtfsFile *file, void *buffer, int length)
{
   if (!file->vol || !file->vol->mounted) return -1;
   if (length <= 0) return 0;
   if (file->position >= file->size) return 0;  // at/after EOF

   uint64_t want = file->size - file->position;
   if (want > (uint64_t)length) want = (uint64_t)length;

   // re-read this file's record and locate its $DATA stream (the unnamed main stream, or a W12a named
   // stream if this handle was opened on one). The record buffer is shared under the lock.
   if (readMftRecordByRef(file->vol, file->mftReference, mftRecord) != 0) return -1;   // F048
   const uint16_t *sName = file->dataNameLen ? file->dataName : 0;
   uint8_t sNameLen = file->dataNameLen;
   NtfsAttr data;
   int haveData = (findAttribute(mftRecord, file->vol->mftRecordSize, ATTR_DATA, sName, sNameLen, &data) == 1);
   if (!haveData && !file->spanned) return -1;             // base must hold $DATA unless it spilled (W8)

   int64_t done;
   if (file->compUnitClusters > 0) {
      // W10a: $LZNT1-compressed $DATA. readCompressed maps the runlist one compression unit at a time
      // (mapVcnWindow, following $ATTRIBUTE_LIST), so it reads a file of any size/fragmentation in
      // O(unit) memory — the full runlist of a multi-MB compressed file far exceeds NTFS_MAX_FILE_RUNS.
      done = readCompressed(file->vol, mftRecord, file->mftReference & MFT_REF_MASK, sName, sNameLen,
                            file->compUnitClusters, file->validSize, file->position, (uint8_t *)buffer, want);
      if (done < 0) return -1;
   } else if (file->spanned) {
      // W8a: $DATA fragments live in several records — merge the runlist, then read from it.
      NtfsRunEntry r[NTFS_MAX_FILE_RUNS]; int rc; uint64_t rs, vs, as;
      if (gatherRuns(file->vol, mftRecord, file->mftReference & MFT_REF_MASK, ATTR_DATA, sName, sNameLen,
                     r, NTFS_MAX_FILE_RUNS, &rc, &rs, &vs, &as) != 0) return -1;
      done = readRuns(file->vol, r, rc, file->validSize, file->position, (uint8_t *)buffer, want);
      if (done < 0) return -1;
   } else if (data.resident) {
      memCopy((uint8_t *)buffer, data.value + file->position, (int)want);   // pos+want <= valueLength == size
      done = (int64_t)want;
   } else {
      done = readNonResident(file->vol, &data, file->validSize, file->position, (uint8_t *)buffer, want);
      if (done < 0) return -1;
   }
   file->position += (uint64_t)done;
   return (int)done;
}

// Sets or clears the on-disk volume-dirty flag in $Volume's $VOLUME_INFORMATION (resident). Set
// before the first write of a session so Windows runs chkdsk if we're interrupted; cleared on a
// clean unmount. Returns 0 on success.
static int setVolumeDirty(NtfsVolume *vol, int dirty)
{
   if (readMftRecord(vol, MFT_RECORD_VOLUME, mftRecord) != 0) return -1;   // F048 ok: $Volume by fixed system record number (seq-0 convention)
   NtfsAttr info;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_VOLUME_INFORMATION, 0, 0, &info) != 1 || !info.resident)
      return -1;
   if (info.valueLength < VOLINFO_FLAGS_OFFSET + 2) return -1;
   uint8_t *flagsField = (uint8_t *)info.value + VOLINFO_FLAGS_OFFSET;   // points into mftRecord (writable)
   uint16_t flags = readLe16(flagsField);
   uint16_t updated = dirty ? (uint16_t)(flags | VOLUME_FLAG_DIRTY) : (uint16_t)(flags & ~VOLUME_FLAG_DIRTY);
   if (updated == flags) return 0;                        // already in the desired state
   writeLe16(flagsField, updated);
   mftMirrorFirst = !dirty;                                // jf-port: clearing -> mirror first
   int rc = writeMftRecord(vol, MFT_RECORD_VOLUME, mftRecord);
   mftMirrorFirst = 0;
   return rc;
}

// Overwrites `want` bytes at `pos` of a non-resident attribute in place; the clusters are already
// allocated. Whole sectors are written straight from the caller's buffer; only a partial leading/
// trailing sector is read-modify-written (to preserve the surrounding on-disk bytes). Caller
// guarantees the range is within the allocation and not sparse. Returns bytes written, or -1.
static int64_t writeNonResident(const NtfsVolume *vol, const NtfsAttr *data, uint64_t pos,
                                const uint8_t *in, uint64_t want)
{
   const uint8_t *runlist = data->attr + data->runlistOffset;
   uint32_t runlistLength = data->attrLength - data->runlistOffset;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint32_t sectorSize = vol->bytesPerSector;
   uint64_t done = 0;

   while (done < want) {
      uint64_t fileOffset = pos + done;
      uint64_t vcn        = fileOffset / bytesPerCluster;
      uint32_t inCluster  = (uint32_t)(fileOffset % bytesPerCluster);

      uint64_t contig = 0;
      int64_t  lcn = mapRunlistSpan(runlist, runlistLength, vcn, &contig);
      if (lcn < 0) return -1;                             // sparse / unmapped: overwriting needs allocation (W2)
      uint64_t spanBytes = contig * bytesPerCluster - inCluster;
      uint64_t chunk = want - done;
      if (chunk > spanBytes) chunk = spanBytes;

      uint32_t offsetInSec  = inCluster % sectorSize;
      uint32_t alignedStart = inCluster - offsetInSec;
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + alignedStart / sectorSize;

      // fast path: sector-aligned start + aligned source -> write the whole contiguous span straight
      // from the caller's buffer in one storage call (no read-modify-write, spans clusters).
      if (offsetInSec == 0 && chunk >= sectorSize && isDmaAligned(in + done)) {
         uint32_t sectors = (uint32_t)(chunk / sectorSize);
         if (writeSectors(vol->storageHandle, lba, sectors, in + done) != 0) return -1;
         done += (uint64_t)sectors * sectorSize;
         continue;
      }

      // read-modify-write path: a partial leading/trailing sector or unaligned source. Read the affected
      // whole sectors, overlay the bytes, write back so the surrounding on-disk bytes are preserved.
      uint64_t windowBytes = spanBytes + offsetInSec;
      if (windowBytes > NTFS_READ_BOUNCE) windowBytes = NTFS_READ_BOUNCE;
      uint32_t sectors = (uint32_t)windowBytes / sectorSize;
      if (readSectors(vol->storageHandle, lba, sectors, fileBounce) != 0) return -1;
      uint64_t avail = windowBytes - offsetInSec;
      if (chunk > avail) chunk = avail;
      memCopy(fileBounce + offsetInSec, in + done, (int)chunk);
      if (writeSectors(vol->storageHandle, lba, sectors, fileBounce) != 0) return -1;
      done += chunk;
   }
   return (int64_t)done;
}

// ===========================================================================
// Write path W2: cluster allocation, runlist (re)encoding, truncate, grow.
// Order rule (crash safety): set $Bitmap bits BEFORE a runlist references them;
// write data BEFORE publishing a larger size; free clusters AFTER unreferencing.
// ===========================================================================


// minimal byte width for a runlist mapping-pair field. NTFS decodes both the length and the LCN
// delta as SIGNED (ntfs-3g runlist.c sign-extends the top byte; Windows does the same), so a value
// whose minimal unsigned encoding has the top bit set needs an extra 0x00 byte to stay positive -
// e.g. a 196-cluster run length is 0xc4 0x00 (two bytes), not 0xc4 (one byte, reads as -60).
static uint32_t signedRunBytes(int64_t v)
{
   uint32_t n = 1;
   while (n < 8) { int64_t lo = -(1LL << (8 * n - 1)), hi = (1LL << (8 * n - 1)) - 1; if (v >= lo && v <= hi) break; n++; }
   return n;
}

// Encodes runs[] into a runlist (mapping pairs + 0x00 terminator). Returns bytes written, or -1 if
// it doesn't fit `cap`. lcn < 0 entries are sparse (offset width 0).
static int encodeRuns(const NtfsRunEntry *runs, int count, uint8_t *out, int cap)
{
   int offset = 0;
   int64_t prevLcn = 0;
   for (int i = 0; i < count; i++) {
      uint32_t lengthBytes = signedRunBytes((int64_t)runs[i].count);
      int sparse = runs[i].lcn < 0;
      int64_t delta = sparse ? 0 : runs[i].lcn - prevLcn;
      uint32_t offsetBytes = sparse ? 0 : signedRunBytes(delta);
      if (offset + 1 + (int)lengthBytes + (int)offsetBytes + 1 > cap) return -1;
      out[offset++] = (uint8_t)(lengthBytes | (offsetBytes << 4));
      for (uint32_t b = 0; b < lengthBytes; b++) out[offset++] = (uint8_t)(runs[i].count >> (8 * b));
      for (uint32_t b = 0; b < offsetBytes; b++) out[offset++] = (uint8_t)((uint64_t)delta >> (8 * b));
      if (!sparse) prevLcn = runs[i].lcn;
   }
   out[offset++] = 0;   // terminator
   return offset;
}

// Decodes a runlist into runs[] (bounded). Returns 0 with *count set, or -1 (malformed / too many).
static int decodeRuns(const uint8_t *runlist, uint32_t length, NtfsRunEntry *runs, int max, int *count)
{
   NtfsRunlist cursor;
   openRunlist(&cursor, runlist, length);
   *count = 0;
   uint64_t vcn, runCount;
   int64_t lcn;
   int rc;
   while ((rc = nextRun(&cursor, &vcn, &lcn, &runCount)) == 1) {
      if (*count >= max) return -1;
      runs[*count].vcn = vcn; runs[*count].lcn = lcn; runs[*count].count = runCount;
      (*count)++;
   }
   return rc;   // 0 clean, -1 malformed
}

// Reads `count` bytes of the cluster $Bitmap at byte offset `pos` into `out` via the runlist cached at
// mount (vol->bitmapRuns) - so cluster (de)allocation never opens or re-reads $Bitmap's MFT record. A
// sub-sector slice is served by reading its covering sector into bitmapScratch. Returns 0 / -1.
static int readBitmapBytes(NtfsVolume *vol, uint64_t pos, uint8_t *out, uint32_t count)
{
   uint32_t sectorSize = vol->bytesPerSector, bytesPerCluster = vol->bytesPerCluster;
   uint32_t copied = 0;
   while (copied < count) {
      uint64_t at = pos + copied;
      uint64_t contiguous = 0;
      int64_t  lcn = mapRunsSpan(vol->bitmapRuns, vol->bitmapRunCount, at / bytesPerCluster, &contiguous);
      if (lcn < 0) return -1;
      uint32_t inCluster = (uint32_t)(at % bytesPerCluster), offsetInSec = inCluster % sectorSize;
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + (inCluster - offsetInSec) / sectorSize;
      uint32_t remaining = count - copied;

      // Whole-sector fast path: take as many contiguous sectors as the run and the request allow in a
      // single read, straight into the caller's buffer. The free-cluster scan moves megabytes through
      // here and on the PS3 every read is a USB round-trip (~1.2ms measured), so reads-per-byte is what
      // decides mount time - one sector at a time cost 17s on a 120GB volume.
      uint64_t runBytes = contiguous * bytesPerCluster - inCluster;
      if (offsetInSec == 0 && remaining >= sectorSize && runBytes >= sectorSize && isDmaAligned(out + copied)) {
         uint32_t take = remaining < runBytes ? remaining : (uint32_t)runBytes;
         take -= take % sectorSize;
         if (readSectors(vol->storageHandle, lba, take / sectorSize, out + copied) != 0) return -1;
         copied += take;
         continue;
      }

      if (readSectors(vol->storageHandle, lba, 1, bitmapScratch) != 0) return -1;
      uint32_t take = sectorSize - offsetInSec;
      if (take > remaining) take = remaining;
      memCopy(out + copied, bitmapScratch + offsetInSec, (int)take);
      copied += take;
   }
   return 0;
}

// Writes `count` bytes to the cluster $Bitmap at byte offset `pos` from `in`, read-modify-writing each
// covering sector through bitmapScratch (cached runlist; no $Bitmap MFT-record read). Returns 0 / -1.
static int writeBitmapBytes(NtfsVolume *vol, uint64_t pos, const uint8_t *in, uint32_t count)
{
   uint32_t sectorSize = vol->bytesPerSector, bytesPerCluster = vol->bytesPerCluster;
   uint32_t done = 0;
   while (done < count) {
      uint64_t at = pos + done;
      uint64_t contiguous = 0;
      int64_t  lcn = mapRunsSpan(vol->bitmapRuns, vol->bitmapRunCount, at / bytesPerCluster, &contiguous);
      if (lcn < 0) return -1;
      uint32_t inCluster = (uint32_t)(at % bytesPerCluster), offsetInSec = inCluster % sectorSize;
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + (inCluster - offsetInSec) / sectorSize;
      if (readSectors(vol->storageHandle, lba, 1, bitmapScratch) != 0) return -1;
      uint32_t put = sectorSize - offsetInSec;
      if (put > count - done) put = count - done;
      memCopy(bitmapScratch + offsetInSec, in + done, (int)put);
      if (writeSectors(vol->storageHandle, lba, 1, bitmapScratch) != 0) return -1;
      done += put;
   }
   return 0;
}

// Sets/clears `count` bitmap bits from `startLcn` in the cluster $Bitmap (read-modify-write of the
// affected bytes via the cached runlist). Returns 0 / -1.
static int setClusterBits(NtfsVolume *vol, uint64_t startLcn, uint64_t count, int value)
{
   uint64_t lastByte   = (startLcn + count - 1) / 8;
   uint64_t byteOffset = startLcn / 8;
   uint64_t flips = 0;                                       // bits actually changed, to keep freeClusters exact
   while (byteOffset <= lastByte) {
      uint8_t chunk[512];
      uint64_t span = lastByte - byteOffset + 1;
      uint32_t got  = span < sizeof(chunk) ? (uint32_t)span : (uint32_t)sizeof(chunk);
      if (readBitmapBytes(vol, byteOffset, chunk, got) != 0) return -1;
      int modified = 0;
      for (uint32_t i = 0; i < got; i++) {
         uint64_t base = (byteOffset + i) * 8;
         for (int bit = 0; bit < 8; bit++) {
            uint64_t lcn = base + bit;
            if (lcn >= startLcn && lcn < startLcn + count) {
               uint8_t mask = (uint8_t)(1 << bit), had = (chunk[i] >> bit) & 1;
               if (value && !had)      { chunk[i] |= mask;  flips++; modified = 1; }
               else if (!value && had) { chunk[i] &= (uint8_t)~mask; flips++; modified = 1; }
            }
         }
      }
      if (modified && writeBitmapBytes(vol, byteOffset, chunk, got) != 0) return -1;
      byteOffset += got;
   }
   if (value) vol->freeClusters -= flips; else vol->freeClusters += flips;
   return 0;
}

// Frees the clusters described by runs[] (clears their $Bitmap bits). Returns 0 / -1.
static int freeClusterRuns(NtfsVolume *vol, const NtfsRunEntry *runs, int count)
{
   int rc = 0;
   for (int i = 0; i < count; i++)
      if (runs[i].lcn >= 0 && setClusterBits(vol, (uint64_t)runs[i].lcn, runs[i].count, 0) != 0) rc = -1;
   return rc;
}

// Frees the last `count` clusters from the tail of runs[], trimming runs[]/*runCount. Used to roll back
// a just-appended allocation regardless of how it coalesced into the existing tail run. Returns 0 / -1.
static int freeTailClusters(NtfsVolume *vol, NtfsRunEntry *runs, int *runCount, uint64_t count)
{
   while (count > 0 && *runCount > 0) {
      NtfsRunEntry *last = &runs[*runCount - 1];
      uint64_t take = last->count < count ? last->count : count;
      if (last->lcn >= 0) {
         NtfsRunEntry tail;
         tail.vcn = 0; tail.lcn = last->lcn + (int64_t)(last->count - take); tail.count = take;
         if (freeClusterRuns(vol, &tail, 1) != 0) return -1;
      }
      last->count -= take;
      if (last->count == 0) (*runCount)--;
      count -= take;
   }
   return count == 0 ? 0 : -1;
}

// Folds physically adjacent runs into one (a run whose lcn continues the previous run's lcn+count).
// Lets a tail-hinted allocation collapse back to a single fragment instead of one run per call.
static void coalesceRuns(NtfsRunEntry *runs, int *count)
{
   int merged = 0;
   for (int r = 1; r < *count; r++) {
      if (runs[merged].lcn >= 0 && runs[r].lcn == runs[merged].lcn + (int64_t)runs[merged].count)
         runs[merged].count += runs[r].count;
      else runs[++merged] = runs[r];
   }
   if (*count > 0) *count = merged + 1;
}

// Allocates `needed` free clusters, appending coalesced runs to runs[]/*count. Scans $Bitmap from
// `hintLcn` and wraps to the start, so a growing file's new clusters land right after its current tail;
// the caller then merges that adjacent run into the existing tail so a sequential write stays one
// fragment instead of one-run-per-call (which would hit `max`). Coalesces only among this call's own
// runs, so a caller that rolls back by run index stays valid. Sets the bits on disk. On any failure
// frees exactly what it allocated and returns -1.
static int allocateClusters(NtfsVolume *vol, uint64_t needed, NtfsRunEntry *runs, int *count, int max, uint64_t hintLcn)
{
   int startCount = *count;
   uint64_t clusterCount = vol->totalSectors / vol->sectorsPerCluster;

   // P5: with no caller tail-hint, resume from where the last allocation left off instead of rescanning
   // the filled prefix from 0 every time. It's only a scan-start hint - the wrap below still reaches any
   // freed clusters before it, so correctness is unaffected by a stale hint.
   // jf-port: two cursors.  A file growing past JF_STREAM_BYTES (a download) is a STREAM: with no
   // tail hint it starts in the stream region (from an eighth of the way into the volume) and then
   // extends from its own tail; everything else -- records, artwork, index blocks, $MFT bitmap --
   // takes the small cursor from the front.  Upstream had one cursor, so the record a download saves
   // at every checkpoint was allocated right after the download's tail and broke it into a new
   // fragment each time, until it hit NTFS_MAX_FILE_RUNS and the download could not grow.
   const int stream = vol->allocStream;
   vol->allocStream = 0;
   uint64_t effectiveHint = hintLcn ? hintLcn
                          : stream  ? (vol->allocHint ? vol->allocHint : clusterCount / 8)
                          :           vol->smallHint;
   uint64_t bitmapBytes = (clusterCount + 7) / 8;
   uint64_t startByte = effectiveHint / 8;
   if (startByte >= bitmapBytes) startByte = 0;
   startByte -= startByte % 512;                              // align to the chunk read size

   uint64_t allocated = 0;
   uint64_t committed = 0;   // jf-port: bits set on disk and not yet taken off vol->freeClusters
   // two ranges so the scan wraps once past the hint: [startByte, end), then [0, startByte)
   for (int pass = 0; pass < 2 && allocated < needed; pass++) {
      uint64_t rangeEnd   = pass == 0 ? bitmapBytes : startByte;
      uint64_t byteOffset = pass == 0 ? startByte : 0;
      while (allocated < needed && byteOffset < rangeEnd) {
         uint8_t chunk[512];
         uint32_t want = (rangeEnd - byteOffset) < sizeof(chunk) ? (uint32_t)(rangeEnd - byteOffset) : (uint32_t)sizeof(chunk);
         if (readBitmapBytes(vol, byteOffset, chunk, want) != 0) goto fail;
         int modified = 0;
         const uint64_t allocatedBefore = allocated;
         for (uint32_t i = 0; i < want && allocated < needed; i++) {
            for (int bit = 0; bit < 8 && allocated < needed; bit++) {
               uint64_t lcn = (byteOffset + i) * 8 + bit;
               if (lcn >= clusterCount) break;
               if ((chunk[i] >> bit) & 1) continue;          // in use
               int contiguous = (*count > startCount && runs[*count - 1].lcn >= 0 &&
                                 (uint64_t)(runs[*count - 1].lcn) + runs[*count - 1].count == lcn);
               if (!contiguous && *count >= max) goto fail;  // a new fragment but runs[] is full
               chunk[i] |= (uint8_t)(1 << bit); modified = 1; allocated++;
               if (contiguous) runs[*count - 1].count++;
               else { runs[*count].vcn = 0; runs[*count].lcn = (int64_t)lcn; runs[*count].count = 1; (*count)++; }
            }
         }
         if (modified && writeBitmapBytes(vol, byteOffset, chunk, want) != 0) goto fail;
         committed += allocated - allocatedBefore;
         byteOffset += want;
      }
   }
   vol->freeClusters -= committed;   // each set bit was a free->used flip (fail path re-adds via setClusterBits)
   committed = 0;
   if (allocated < needed) goto fail;
   // advance the cursor past this allocation's last run so the next call resumes here
   if (*count > startCount && runs[*count - 1].lcn >= 0) {
      const uint64_t end = (uint64_t)runs[*count - 1].lcn + runs[*count - 1].count;
      if (stream) vol->allocHint = end;            // jf-port: each cursor moves on its own
      else if (!hintLcn) vol->smallHint = end;
   }
   return 0;
fail:
   vol->freeClusters -= committed;   // jf-port: what reached the disk, which freeTailClusters gives back
   freeTailClusters(vol, runs, count, allocated);
   return -1;
}

// Resizes the attribute at `attrOffset` in a FILE record from oldLen to newLen, shifting the trailing
// attributes/end-marker and updating the record's used size. Returns 0, or -1 if it won't fit.
static int resizeAttribute(uint8_t *record, uint32_t recordSize, uint32_t attrOffset, uint32_t oldLen, uint32_t newLen)
{
   uint32_t used = readLe32(record + FILE_USED_SIZE);
   uint32_t tailStart = attrOffset + oldLen;
   if (tailStart > used) return -1;
   uint32_t tailLen = used - tailStart;
   if (newLen > oldLen && used + (newLen - oldLen) > recordSize) return -1;
   memMove(record + attrOffset + newLen, record + tailStart, (int)tailLen);
   uint32_t newUsed = used + newLen - oldLen;
   if (newUsed < used) memSet(record + newUsed, 0, (int)(used - newUsed));   // clear vacated bytes on shrink
   writeLe32(record + FILE_USED_SIZE, newUsed);
   writeLe32(record + attrOffset + ATTR_LENGTH_OFFSET, newLen);
   return 0;
}

// Writes the resident $DATA value (resident form) of `length` bytes into the record, resizing the
// attribute. Used for small files and truncate-to-empty. Returns 0 / -1.
static int setResidentData(uint8_t *record, uint32_t recordSize, uint32_t attrOffset, const uint8_t *value, uint32_t length)
{
   uint32_t oldLen = readLe32(record + attrOffset + ATTR_LENGTH_OFFSET);
   uint32_t newLen = (24 + length + 7) & ~7u;                // resident header is 24 bytes, 8-aligned
   if (resizeAttribute(record, recordSize, attrOffset, oldLen, newLen) != 0) return -1;
   uint8_t *attr = record + attrOffset;
   attr[ATTR_NON_RESIDENT] = 0;
   writeLe16(attr + ATTR_FLAGS_OFFSET, 0);
   writeLe32(attr + ATTR_RES_VALUE_LENGTH, length);
   writeLe16(attr + ATTR_RES_VALUE_OFFSET, 24);
   attr[22] = 0; attr[23] = 0;                              // indexed flag + padding
   if (length) memCopy(attr + 24, value, (int)length);
   return 0;
}

// Writes a non-resident $DATA header + runlist into the record (attribute at attrOffset), resizing
// the attribute to fit. runlistOffset is fixed at 64 (no attribute name). Returns 0 / -1.
static int setNonResidentData(uint8_t *record, uint32_t recordSize, uint32_t attrOffset,
                              const NtfsRunEntry *runs, int runCount, uint64_t realSize, uint64_t validSize,
                              uint64_t allocatedSize, uint64_t lastVcn)
{
   uint8_t encoded[1024];                                     // a dedicated extent record can hold a bigger
   int encodedLen = encodeRuns(runs, runCount, encoded, (int)sizeof(encoded));   // runlist than a crowded base
   if (encodedLen < 0) return -1;
   uint32_t oldLen = readLe32(record + attrOffset + ATTR_LENGTH_OFFSET);
   uint32_t newLen = (ATTR_NR_HEADER_MIN + (uint32_t)encodedLen + 7) & ~7u;
   if (resizeAttribute(record, recordSize, attrOffset, oldLen, newLen) != 0) return -1;
   uint8_t *attr = record + attrOffset;
   attr[ATTR_NON_RESIDENT] = 1;
   writeLe16(attr + ATTR_FLAGS_OFFSET, 0);
   writeLe64(attr + ATTR_NR_START_VCN, 0);
   writeLe64(attr + ATTR_NR_LAST_VCN, lastVcn);
   writeLe16(attr + ATTR_NR_RUNLIST_OFFSET, ATTR_NR_HEADER_MIN);
   writeLe64(attr + ATTR_NR_ALLOC_SIZE, allocatedSize);
   writeLe64(attr + ATTR_NR_REAL_SIZE, realSize);
   writeLe64(attr + ATTR_NR_VALID_SIZE, validSize);
   memCopy(attr + ATTR_NR_HEADER_MIN, encoded, encodedLen);
   memSet(attr + ATTR_NR_HEADER_MIN + encodedLen, 0, (int)(newLen - ATTR_NR_HEADER_MIN - encodedLen));
   return 0;
}

// The resident-value capacity for the $DATA attribute at attrOffset, given the rest of the record.
static uint32_t residentCapacity(const uint8_t *record, uint32_t recordSize, uint32_t attrOffset)
{
   uint32_t used = readLe32(record + FILE_USED_SIZE);
   uint32_t attrLen = readLe32(record + attrOffset + ATTR_LENGTH_OFFSET);
   uint32_t otherBytes = used - attrLen;                    // everything except this attribute
   if (recordSize <= otherBytes + 24 + 8) return 0;
   return recordSize - otherBytes - 24 - 8;                 // 24 resident header + 8 end-marker slack
}

// Overwrites `want` bytes at `pos` within the file's already-initialized data (caller guarantees
// pos+want <= validSize). Returns bytes written, or -1. Re-reads the record each call.
static int overwriteInPlace(NtfsFile *file, uint64_t pos, const uint8_t *in, uint64_t want)
{
   NtfsVolume *vol = file->vol;
   uint64_t reference = file->mftReference & MFT_REF_MASK;
   NtfsAttr data;
   uint64_t housingRef;
   if (findDataAnywhere(vol, reference, mftRecord, &data, &housingRef) != 0) return -1;   // base or extent
   if (data.resident) {
      memCopy((uint8_t *)data.value + pos, in, (int)want);
      if (writeMftRecord(vol, housingRef, mftRecord) != 0) return -1;
      return (int)want;
   }
   int64_t done = writeNonResident(vol, &data, pos, in, want);
   return done < 0 ? -1 : (int)done;
}

// Appends `len` bytes at end-of-file, growing the file: stays resident while it fits the record,
// otherwise allocates clusters and (promoting from resident if needed) writes non-resident. Order:
// allocate bitmap -> publish runlist (new region uninitialized) -> write data -> bump ValidDataLength.
// Returns bytes appended, or -1.
static int appendData(NtfsFile *file, const uint8_t *in, int len)
{
   NtfsVolume *vol = file->vol;
   uint64_t reference = file->mftReference & MFT_REF_MASK;
   uint32_t recordSize = vol->mftRecordSize;
   uint64_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t oldSize = file->size;
   uint64_t newSize = oldSize + (uint64_t)len;

   NtfsAttr data;
   uint64_t housingRef;
   if (findDataAnywhere(vol, reference, mftRecord, &data, &housingRef) != 0) return -1;   // base or extent
   uint32_t attrOffset = (uint32_t)(data.attr - mftRecord);

   // resident and still fits the record -> grow the value in place (resident $DATA is always in base)
   if (data.resident && newSize <= residentCapacity(mftRecord, recordSize, attrOffset)) {
      memCopy(indexBuffer, data.value, (int)oldSize);
      memCopy(indexBuffer + oldSize, in, len);
      if (setResidentData(mftRecord, recordSize, attrOffset, indexBuffer, (uint32_t)newSize) != 0) return -1;
      if (writeMftRecord(vol, housingRef, mftRecord) != 0) return -1;
      file->size = newSize; file->validSize = newSize; file->resident = 1;
      return len;
   }

   // otherwise the file becomes / stays non-resident; gather the existing runlist (and resident bytes)
   NtfsRunEntry runs[NTFS_MAX_FILE_RUNS];
   int runCount = 0;
   uint64_t haveClusters = 0;
   int promoting = data.resident;
   if (promoting) {
      memCopy(indexBuffer, data.value, (int)oldSize);                 // stash existing bytes to re-write
   } else {
      if (decodeRuns(data.attr + data.runlistOffset, data.attrLength - data.runlistOffset,
                     runs, NTFS_MAX_FILE_RUNS, &runCount) != 0) return -1;
      haveClusters = data.allocatedSize / bytesPerCluster;
   }

   // Allocate any additional clusters. allocateClusters works the cached $Bitmap runlist now, so it does
   // NOT clobber the shared record buffer - the file record read at the top stays valid (no re-read). W2
   // D (reserve-ahead): when the file must grow, over-allocate a ramped, capped reserve of contiguous
   // clusters so the next ~1 MB of 32 KB appends find them already mapped and skip the $Bitmap entirely;
   // the unused slack is reclaimed on close (trimOverAllocation). Hint the allocator at the current tail
   // so the reserve extends one run instead of fragmenting.
   uint64_t needClusters = (newSize + bytesPerCluster - 1) / bytesPerCluster;
   uint64_t addClusters = 0;
   if (needClusters > haveClusters) {
      // jf-port: 16 MB, not 1 MB: a download grows by 16 MB at a time (trimmed on close as before)
      uint64_t reserveCap = (newSize >= JF_STREAM_BYTES ? (16u << 20) : (1u << 20)) / bytesPerCluster;
      if (reserveCap == 0) reserveCap = 1;
      uint64_t reserve = needClusters < reserveCap ? needClusters : reserveCap;   // ramps up to the cap
      addClusters = (needClusters + reserve) - haveClusters;
   }
   uint64_t hintLcn = (runCount > 0 && runs[runCount - 1].lcn >= 0)
                      ? (uint64_t)runs[runCount - 1].lcn + runs[runCount - 1].count : 0;
   // jf-port: a stream (see allocateClusters) whose tail is still in the small region moves to the
   // stream region once, at JF_STREAM_BYTES; after that it extends from its tail
   if (newSize >= JF_STREAM_BYTES && addClusters) {
      vol->allocStream = 1;                        // read and cleared by the allocateClusters just below
      if (hintLcn < (vol->totalSectors / vol->sectorsPerCluster) / 8) hintLcn = 0;
   }
   if (addClusters && allocateClusters(vol, addClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, hintLcn) != 0) {
      // jf-port: no room for the reserve-ahead: take exactly what this write needs, so a volume
      // fills to its last cluster instead of failing with the reserve's worth still free
      if (needClusters <= haveClusters || addClusters == needClusters - haveClusters) return -1;
      addClusters = needClusters - haveClusters;
      if (newSize >= JF_STREAM_BYTES) vol->allocStream = 1;
      if (allocateClusters(vol, addClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, hintLcn) != 0) return -1;
   }
   if (addClusters) coalesceRuns(runs, &runCount);             // fold adjacent runs so a sequential file stays one fragment
   uint64_t totalClusters = haveClusters + addClusters;        // clusters now mapped (data + reserve slack)
   uint64_t allocatedSize = totalClusters * bytesPerCluster;

   // lay the runlist + new real size into the in-memory record (validSize stays at the old length). The
   // record from the top of appendData is still in mftRecord, so reuse data/attrOffset/housingRef directly.
   int spilled = 0;   // declared before any `goto rollback` so the label can read it
   uint64_t oldValid = promoting ? 0 : file->validSize;
   if (setNonResidentData(mftRecord, recordSize, attrOffset, runs, runCount, newSize, oldValid,
                          allocatedSize, totalClusters - 1) != 0) {
      // The runlist won't fit the current housing record. If it's the base, spill $DATA to a dedicated
      // extension record (W8b-S1) - which commits metadata to disk now. If it's already an extension, the
      // runlist outgrew even a dedicated record (needs multi-fragment $DATA = S1b) -> refuse cleanly.
      if (housingRef != reference) goto rollback;
      if (spillDataToExtension(vol, reference, runs, runCount, newSize, oldValid, allocatedSize, needClusters - 1) != 0)
         goto rollback;
      spilled = 1;
   }

   // Write the data to the freshly mapped clusters BEFORE committing the size. On the non-spill path the
   // runlist is still only in memory, so a write failure rolls the cluster allocation back with the
   // on-disk file untouched (atomic append). The spill path already published its metadata above, so it
   // re-reads $DATA from its new extension record and a later failure leaves a zero-tailed grown file.
   if (spilled) {
      if (findDataAnywhere(vol, reference, mftRecord, &data, &housingRef) != 0) goto rollback;
   } else {
      if (findAttribute(mftRecord, recordSize, ATTR_DATA, 0, 0, &data) != 1) goto rollback;   // in-memory new runlist
      housingRef = reference;
   }
   if (promoting && oldSize && writeNonResident(vol, &data, 0, indexBuffer, oldSize) < 0) goto rollback;
   if (writeNonResident(vol, &data, oldSize, in, (uint64_t)len) < 0) goto rollback;
   writeLe64((uint8_t *)data.attr + ATTR_NR_VALID_SIZE, newSize);
   if (writeMftRecord(vol, housingRef, mftRecord) != 0) goto rollback;
   file->size = newSize; file->validSize = newSize; file->resident = 0;
   return len;

rollback:
   // only free the allocation while it is still unpublished; once spilled, the clusters are referenced
   // by the committed extension record and freeing them would cross-link the volume.
   if (!spilled) freeTailClusters(vol, runs, &runCount, addClusters);
   return -1;
}

int writeNtfs(NtfsFile *file, const void *buffer, int length)
{
   NtfsVolume *vol = file->vol;
   if (!vol || !vol->mounted || !vol->writable || !file->writable) return -1;   // read-only mount/handle
   if (file->compressed) return -1;                      // $LZNT1 / WOF: not writable here
   if (file->dataNameLen) return -1;                     // named ADS is read-only: the write path only
                                                         // tracks the unnamed $DATA, so refuse rather than
                                                         // silently overwrite the main stream
   if (length <= 0) return 0;

   // arm the dirty flag on the first write of the session (cleared on clean close/unmount)
   if (!vol->volumeDirty) {
      if (setVolumeDirty(vol, 1) != 0) return -1;
      vol->volumeDirty = 1;
   }
   file->dirty = 1;                                       // sync the $FILE_NAME size copies on close

   const uint8_t *in = (const uint8_t *)buffer;
   uint64_t want = (uint64_t)length;
   int total = 0;

   // overwrite the portion that lands within the already-initialized data (W1 in-place path)
   if (file->position < file->validSize) {
      uint64_t overwrite = file->validSize - file->position;
      if (overwrite > want) overwrite = want;
      int written = overwriteInPlace(file, file->position, in, overwrite);
      if (written < 0) return total > 0 ? total : -1;
      total += written; file->position += (uint64_t)written; in += written; want -= (uint64_t)written;
      if ((uint64_t)written < overwrite) return total;   // short write
   }

   // append the remainder (W2 grow). Only a contiguous write at end-of-file is supported; a write
   // that would leave an uninitialized gap (position past the end) is refused rather than guessed.
   if (want > 0) {
      if (file->position != file->size) return total > 0 ? total : -1;
      int chunk = want > 0x7FFFFFFF ? 0x7FFFFFFF : (int)want;
      int appended = appendData(file, in, chunk);
      if (appended < 0) return total > 0 ? total : -1;
      total += appended; file->position += (uint64_t)appended;
   }
   return total;
}

// Shrinks a file to `newSize` (W2 supports shrink only): frees the clusters past newSize and
// updates the runlist/sizes (truncate-to-zero leaves a resident, empty $DATA). Order: publish the
// shrunk runlist (unreference the tail) before freeing the bits, so a crash leaks rather than
// cross-links. Returns 0 / -1.
// Splits decoded runs[] at cluster `keepClusters`: the head (clusters [0,keepClusters)) lands in
// keepRuns[]/*keepCount, the tail in freeRuns[]/*freeCount. keepRuns may alias runs (in-place: each
// kept entry index <= its source index). A run straddling the cut is split across the two outputs.
static void splitRunsAt(const NtfsRunEntry *runs, int runCount, uint64_t keepClusters,
                        NtfsRunEntry *keepRuns, int *keepCount, NtfsRunEntry *freeRuns, int *freeCount)
{
   *keepCount = 0; *freeCount = 0;
   uint64_t vcn = 0;
   for (int i = 0; i < runCount; i++) {
      uint64_t count = runs[i].count;
      if (vcn >= keepClusters) {
         freeRuns[(*freeCount)++] = runs[i];
      } else if (vcn + count <= keepClusters) {
         keepRuns[(*keepCount)++] = runs[i];
      } else {                                            // this run straddles the cut
         uint64_t keepHere = keepClusters - vcn;
         freeRuns[*freeCount].vcn = 0;
         freeRuns[*freeCount].lcn = runs[i].lcn < 0 ? -1 : runs[i].lcn + (int64_t)keepHere;
         freeRuns[*freeCount].count = count - keepHere; (*freeCount)++;
         NtfsRunEntry head = runs[i]; head.count = keepHere; keepRuns[(*keepCount)++] = head;
      }
      vcn += count;
   }
}

// Frees clusters allocated past the file's real data - the slack left by reserve-ahead appends (W2 D) -
// so a closed file's allocatedSize == ceil(realSize). realSize/validSize are unchanged. Non-resident,
// base-record $DATA only; a no-op when there's no slack. Returns 0 / -1. Clobbers mftRecord.
static int trimOverAllocation(NtfsFile *file)
{
   NtfsVolume *vol = file->vol;
   if (file->resident || file->spanned || file->compressed || file->dataNameLen) return 0;
   uint32_t recordSize = vol->mftRecordSize;
   uint64_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t reference = file->mftReference & MFT_REF_MASK;
   if (readMftRecordByRef(vol, file->mftReference, mftRecord) != 0) return -1;
   NtfsAttr data;
   if (findAttribute(mftRecord, recordSize, ATTR_DATA, 0, 0, &data) != 1 || data.resident) return 0;
   uint64_t keepClusters = (file->size + bytesPerCluster - 1) / bytesPerCluster;
   if (data.allocatedSize / bytesPerCluster <= keepClusters) return 0;   // no slack to reclaim

   NtfsRunEntry runs[NTFS_MAX_FILE_RUNS], freeRuns[NTFS_MAX_FILE_RUNS];
   int runCount = 0, keepCount = 0, freeCount = 0;
   if (decodeRuns(data.attr + data.runlistOffset, data.attrLength - data.runlistOffset,
                  runs, NTFS_MAX_FILE_RUNS, &runCount) != 0) return -1;
   splitRunsAt(runs, runCount, keepClusters, runs, &keepCount, freeRuns, &freeCount);
   uint32_t attrOffset = (uint32_t)(data.attr - mftRecord);
   if (setNonResidentData(mftRecord, recordSize, attrOffset, runs, keepCount, file->size, file->validSize,
                          keepClusters * bytesPerCluster, keepClusters - 1) != 0) return -1;
   if (writeMftRecord(vol, reference, mftRecord) != 0) return -1;   // unreference the tail first
   freeClusterRuns(vol, freeRuns, freeCount);                       // then free the bits
   return 0;
}

int truncateNtfs(NtfsFile *file, uint64_t newSize)
{
   NtfsVolume *vol = file->vol;
   if (!vol || !vol->mounted || !vol->writable || !file->writable) return -1;
   if (file->dataNameLen) return -1;                     // named ADS is read-only (see writeNtfs)
   if (file->compressed) return -1;                      // $LZNT1 / sparse: runlist isn't plain, can't truncate it
   if (file->spanned) return -1;                         // $DATA spans $ATTRIBUTE_LIST extents: only the base
                                                         // fragment is reachable here, so refuse rather than
                                                         // mis-truncate / leak the extension records
   if (newSize >= file->size) return 0;
   file->dirty = 1;                                       // sync the $FILE_NAME size copies on close
   uint64_t reference = file->mftReference & MFT_REF_MASK;
   uint32_t recordSize = vol->mftRecordSize;
   uint64_t bytesPerCluster = vol->bytesPerCluster;

   if (!vol->volumeDirty) { if (setVolumeDirty(vol, 1) != 0) return -1; vol->volumeDirty = 1; }
   if (readMftRecordByRef(vol, file->mftReference, mftRecord) != 0) return -1;   // F048: validate the open handle's sequence
   NtfsAttr data;
   if (findAttribute(mftRecord, recordSize, ATTR_DATA, 0, 0, &data) != 1) return -1;
   uint32_t attrOffset = (uint32_t)(data.attr - mftRecord);

   // resident: just shrink the value (truncate-to-zero is the common case)
   if (data.resident) {
      memCopy(indexBuffer, data.value, (int)newSize);
      if (setResidentData(mftRecord, recordSize, attrOffset, indexBuffer, (uint32_t)newSize) != 0) return -1;
      if (writeMftRecord(vol, reference, mftRecord) != 0) return -1;
      file->size = newSize; file->validSize = newSize; file->resident = 1;
      return 0;
   }

   // non-resident: split the runlist at the kept cluster count; the tail is freed after publishing
   uint64_t keepClusters = (newSize + bytesPerCluster - 1) / bytesPerCluster;
   NtfsRunEntry runs[NTFS_MAX_FILE_RUNS], freeRuns[NTFS_MAX_FILE_RUNS];
   int runCount = 0, freeCount = 0, keepCount = 0;
   if (decodeRuns(data.attr + data.runlistOffset, data.attrLength - data.runlistOffset,
                  runs, NTFS_MAX_FILE_RUNS, &runCount) != 0) return -1;
   splitRunsAt(runs, runCount, keepClusters, runs, &keepCount, freeRuns, &freeCount);

   uint64_t newValid = file->validSize < newSize ? file->validSize : newSize;
   if (newSize == 0) {
      if (setResidentData(mftRecord, recordSize, attrOffset, 0, 0) != 0) return -1;   // empty resident $DATA
   } else if (setNonResidentData(mftRecord, recordSize, attrOffset, runs, keepCount, newSize, newValid,
                                 keepClusters * bytesPerCluster, keepClusters - 1) != 0) {
      return -1;
   }
   if (writeMftRecord(vol, reference, mftRecord) != 0) return -1;   // unreference the tail first
   freeClusterRuns(vol, freeRuns, freeCount);                       // then free the bits
   file->size = newSize; file->validSize = newValid;
   file->resident = (newSize == 0);
   return 0;
}

// ===========================================================================
// Write path W3: create file / mkdir. Allocates a free MFT record from $MFT's
// $BITMAP, builds a fresh FILE record, and inserts a $FILE_NAME entry into the
// parent's $I30 index in collation order. Refuses (rather than risk corruption)
// when it would need to split a B-tree node or extend the MFT.
// ===========================================================================

#define ALIGN8(x) (((x) + 7u) & ~7u)
#define NTFS_NEW_FILETIME 133444736000000000ull   // fallback create timestamp (~2023 UTC) when the RTC isn't loaded
#define FN_NAMESPACE_POSIX 0                       // single standalone name (no DOS twin); what ntfs-3g and Windows write for new files

// W11: current time as an NTFS FILETIME from the hardware RTC (UTC). Falls back to the fixed constant
// when the RTC module isn't loaded (cellRtcGetCurrentClock fails or returns a pre-NTFS year), so a
// create still writes a valid, in-range timestamp rather than 0.
static uint64_t nowFiletime(void)
{
   // jf-port: lv2's UTC clock (jf_port_utc_seconds) instead of cellRtcGetCurrentClock; the same
   // fallback to the fixed constant when the clock is unset or out of range.
   uint64_t unixSeconds = jf_port_utc_seconds();
   if (unixSeconds < 946684800ULL || unixSeconds > 253402300799ULL) return NTFS_NEW_FILETIME;   // before 2000 / after 9999
   return unixSeconds * 10000000ULL + 116444736000000000ULL;   // 100 ns ticks since 1601-01-01
}

// Builds a $FILE_NAME key into `out` from UTF-16 `units[nameLen]` with an explicit namespace and
// timestamp. Returns the key length in bytes, or -1 if the name length is out of range.
static int buildFileNameKeyU(uint8_t *out, uint64_t parentRef, const uint16_t *units, int nameLen,
                             int isDir, uint8_t namespace, uint64_t ft)
{
   if (nameLen < 1 || nameLen > 255) return -1;
   memSet(out, 0, FN_NAME + nameLen * 2);
   writeLe64(out + FN_PARENT_REF, parentRef);
   for (int i = 8; i <= 32; i += 8) writeLe64(out + i, ft);  // creation/modified/mft/access (one instant)
   writeLe64(out + FN_ALLOC_SIZE, 0);                         // allocated size (set on write via syncFileNameSizes)
   writeLe64(out + FN_REAL_SIZE, 0);                          // real size (set on write via syncFileNameSizes)
   writeLe32(out + 56, isDir ? FN_FLAG_DIRECTORY : 0);        // file attribute flags
   out[FN_NAME_LENGTH] = (uint8_t)nameLen;
   out[FN_NAMESPACE]   = namespace;
   for (int i = 0; i < nameLen; i++) writeLe16(out + FN_NAME + i * 2, units[i]);
   return FN_NAME + nameLen * 2;
}

// Emits a resident attribute (header + value) at `off` in the record; returns the next offset.
static uint32_t emitResidentAttr(uint8_t *record, uint32_t off, uint32_t type, uint16_t id, int indexed,
                                 const uint16_t *name, uint8_t nameLen, const uint8_t *value, uint32_t valueLen)
{
   uint16_t nameOffset = nameLen ? 24 : 0;
   uint16_t valueOffset = (uint16_t)ALIGN8(24 + (uint32_t)nameLen * 2);
   uint32_t length = ALIGN8(valueOffset + valueLen);
   uint8_t *attr = record + off;
   memSet(attr, 0, length);
   writeLe32(attr + ATTR_TYPE_OFFSET, type);
   writeLe32(attr + ATTR_LENGTH_OFFSET, length);
   attr[ATTR_NON_RESIDENT] = 0;
   attr[ATTR_NAME_LENGTH] = nameLen;
   writeLe16(attr + ATTR_NAME_OFFSET, nameOffset);
   writeLe16(attr + ATTR_ID_OFFSET, id);
   writeLe32(attr + ATTR_RES_VALUE_LENGTH, valueLen);
   writeLe16(attr + ATTR_RES_VALUE_OFFSET, valueOffset);
   attr[22] = (uint8_t)(indexed ? 1 : 0);
   for (uint8_t i = 0; i < nameLen; i++) writeLe16(attr + nameOffset + i * 2, name[i]);
   if (valueLen) memCopy(attr + valueOffset, value, (int)valueLen);
   return off + length;
}

// Builds a fresh FILE record for a new file/dir into `record`. Returns the used size. `securityId` is
// the parent's inherited $STANDARD_INFORMATION security_id; when it is 0 the parent instead carries an
// inline `$SECURITY_DESCRIPTOR` (some volumes, incl. this volume's root) — `secDesc[secDescLen]` is then
// a copy of it to embed inline, so the new object inherits the parent's ACL instead of having no
// security at all (which Windows treats as access-denied). With neither, the legacy no-security form.
static uint32_t buildNewRecord(uint8_t *record, uint32_t recordSize, uint32_t sectorSize, uint32_t indexRecordSize,
                               uint64_t recordNumber, uint16_t sequence, int isDir, const uint8_t *nameKey, int keyLen,
                               const uint8_t *dosKey, int dosKeyLen, uint32_t securityId,
                               const uint8_t *secDesc, uint32_t secDescLen)
{
   memSet(record, 0, recordSize);
   record[0] = 'F'; record[1] = 'I'; record[2] = 'L'; record[3] = 'E';
   uint32_t blocks = recordSize / sectorSize;
   uint16_t usaOffset = 48, usaCount = (uint16_t)(1 + blocks);
   writeLe16(record + FILE_USA_OFFSET, usaOffset);
   writeLe16(record + FILE_USA_COUNT, usaCount);
   writeLe16(record + FILE_SEQUENCE_NUMBER, sequence);        // continue the freed record's sequence (stale-ref detection)
   writeLe16(record + FILE_HARD_LINK_COUNT, 1);
   uint16_t firstAttr = (uint16_t)ALIGN8(usaOffset + (uint32_t)usaCount * 2);
   writeLe16(record + FILE_FIRST_ATTR_OFFSET, firstAttr);
   writeLe16(record + FILE_FLAGS, (uint16_t)(FILE_FLAG_IN_USE | (isDir ? FILE_FLAG_DIRECTORY : 0)));
   writeLe64(record + FILE_BASE_REFERENCE, 0);
   writeLe32(record + 44, (uint32_t)recordNumber);            // MFT record index (v3.1)
   writeLe16(record + usaOffset, 1);                          // initial USN (applyUsaWrite bumps it)

   // $STANDARD_INFORMATION. Timestamps are taken from the $FILE_NAME key so the record's authoritative
   // copy and the name copy are the same instant. With a security_id to inherit we emit the 72-byte
   // NTFS-3.x form (adds owner/security_id/quota/USN) so the record carries a valid central-security
   // reference like Windows'; without one we keep the 48-byte legacy form.
   uint8_t sinfo[72];
   memSet(sinfo, 0, sizeof sinfo);
   writeLe64(sinfo + 0x00, readLe64(nameKey + 8));            // creation  (FN +0x08)
   writeLe64(sinfo + 0x08, readLe64(nameKey + 16));           // modified  (FN +0x10)
   writeLe64(sinfo + 0x10, readLe64(nameKey + 24));           // mft-change(FN +0x18)
   writeLe64(sinfo + 0x18, readLe64(nameKey + 32));           // access    (FN +0x20)
   writeLe32(sinfo + 0x20, isDir ? FN_FLAG_DIRECTORY : 0);    // DOS file-attribute flags
   uint32_t siLen = 48;
   if (securityId != 0) { writeLe32(sinfo + 0x34, securityId); siLen = 72; }   // 3.x form: inherited security_id
   uint32_t off = emitResidentAttr(record, firstAttr, ATTR_STANDARD_INFORMATION, 0, 0, 0, 0, sinfo, siLen);

   // $FILE_NAME (the Win32/primary key), marked indexed (id 1). For a non-8.3 name a second DOS-twin
   // $FILE_NAME (id 2) follows so the record carries both names Windows would (W11). The hard-link
   // count stays 1: a DOS twin shares its Win32 partner's link and is not counted.
   off = emitResidentAttr(record, off, ATTR_FILE_NAME, 1, 1, 0, 0, nameKey, (uint32_t)keyLen);
   uint16_t nextId = 2;
   if (dosKey && dosKeyLen > 0) {
      off = emitResidentAttr(record, off, ATTR_FILE_NAME, 2, 1, 0, 0, dosKey, (uint32_t)dosKeyLen);
      nextId = 3;
   }
   // inline $SECURITY_DESCRIPTOR (id next), only when the parent had one to inherit (no $Secure id).
   // Kept in attribute-type order (0x50, after $FILE_NAME 0x30, before $DATA/$INDEX_ROOT).
   if (securityId == 0 && secDesc && secDescLen > 0)
      off = emitResidentAttr(record, off, ATTR_SECURITY_DESCRIPTOR, nextId++, 0, 0, 0, secDesc, secDescLen);

   uint16_t bodyId = nextId++;                                // id for the $INDEX_ROOT / $DATA below

   if (isDir) {
      // empty $INDEX_ROOT ($I30): index-root header + node header + lone end-marker entry
      uint8_t root[48];
      memSet(root, 0, sizeof root);
      writeLe32(root + 0, ATTR_FILE_NAME);                    // indexed attribute type
      writeLe32(root + 4, 1);                                 // collation: filename
      writeLe32(root + 8, indexRecordSize);                   // index block byte size
      root[12] = 1;                                           // clusters per index block (nominal)
      writeLe32(root + 16 + 0, 16);                           // node: entries offset
      writeLe32(root + 16 + 4, 32);                           // node: used size (header + end entry)
      writeLe32(root + 16 + 8, 32);                           // node: allocated size
      writeLe32(root + 16 + 12, 0);                           // node flags: small index (no children)
      writeLe16(root + 32 + 8, 16);                           // end entry: length 16
      writeLe16(root + 32 + 12, IDXENTRY_FLAG_LAST);          // end entry: last
      uint16_t i30[4] = { '$', 'I', '3', '0' };
      off = emitResidentAttr(record, off, ATTR_INDEX_ROOT, bodyId, 0, i30, 4, root, 48);
   } else {
      off = emitResidentAttr(record, off, ATTR_DATA, bodyId, 0, 0, 0, 0, 0);   // empty resident $DATA
   }

   writeLe32(record + off, ATTR_END);                         // end marker
   off = ALIGN8(off + 4);
   writeLe32(record + FILE_USED_SIZE, off);
   writeLe32(record + FILE_ALLOCATED_SIZE, recordSize);
   writeLe16(record + 40, (uint16_t)(bodyId + 1));            // next attribute id
   return off;
}

static int setMftRecordBit(NtfsVolume *vol, uint64_t number, int value);   // defined just below findFreeMftRecord

// W8b: lays out an empty, not-in-use FILE record in `rec` (matches ntfs-3g ntfs_mft_record_layout:
// "FILE" magic, USA, flags=0, a lone end marker). New MFT extents are formatted this way — like
// Windows/ntfs-3g — rather than left zeroed, so the grown MFT looks identical to a native one.
static void formatEmptyMftRecord(uint8_t *rec, uint32_t recordSize, uint32_t sectorSize, uint64_t number)
{
   memSet(rec, 0, recordSize);
   rec[0] = 'F'; rec[1] = 'I'; rec[2] = 'L'; rec[3] = 'E';
   uint32_t blocks = recordSize / sectorSize;
   uint16_t usaOffset = 48, usaCount = (uint16_t)(1 + blocks);
   writeLe16(rec + FILE_USA_OFFSET, usaOffset);
   writeLe16(rec + FILE_USA_COUNT, usaCount);
   writeLe16(rec + 16, 0);                                    // sequence number (0 = never used)
   writeLe16(rec + FILE_HARD_LINK_COUNT, 0);
   uint16_t firstAttr = (uint16_t)ALIGN8(usaOffset + (uint32_t)usaCount * 2);
   writeLe16(rec + FILE_FIRST_ATTR_OFFSET, firstAttr);
   writeLe16(rec + FILE_FLAGS, 0);                            // not in use, not a directory
   writeLe64(rec + FILE_BASE_REFERENCE, 0);
   writeLe32(rec + 44, (uint32_t)number);                    // MFT record index (v3.1)
   writeLe16(rec + usaOffset, 1);                            // initial USN (applyUsaWrite bumps it)
   writeLe32(rec + firstAttr, ATTR_END);                     // lone end marker
   writeLe32(rec + FILE_USED_SIZE, firstAttr + 8);
   writeLe32(rec + FILE_ALLOCATED_SIZE, recordSize);
   writeLe16(rec + 40, 0);                                    // next attribute id
}

// W8b: writes `count` zeroed clusters at `lcn` (so newly allocated bitmap extents are clean, not
// stale on-disk garbage). Returns 0/-1.
static int zeroClusters(NtfsVolume *vol, int64_t lcn, uint64_t count)
{
   if (lcn < 0 || count == 0) return 0;
   memSet(fileBounce, 0, NTFS_READ_BOUNCE);
   uint32_t spc = vol->sectorsPerCluster, ss = vol->bytesPerSector;
   uint64_t sectors = count * spc;
   uint64_t lba = vol->partitionOffset + (uint64_t)lcn * spc;
   uint32_t chunkSecs = NTFS_READ_BOUNCE / ss;
   while (sectors > 0) {
      uint32_t n = sectors < chunkSecs ? (uint32_t)sectors : chunkSecs;
      if (writeSectors(vol->storageHandle, lba, n, fileBounce) != 0) return -1;
      lba += n; sectors -= n;
   }
   return 0;
}

// W8b: grows $MFT (record 0) by at least `addRecords` records when its $BITMAP has no free slot.
// Allocates clusters, extends $MFT's unnamed $DATA runlist + sizes, zeroes the new records, grows the
// $MFT $BITMAP (resident in-record, or non-resident by extending its own runlist) to cover them, and
// marks the new records free. Writing record 0 mirrors $MFTMirr (W6). The $MFT is the most dangerous
// structure to touch, so ordering is strict allocate-before-reference and the in-memory $MFT runlist
// cache (vol->mftRuns) is updated only after the commit. Returns 0/-1; refuses (never corrupts) the
// rare cases it can't handle ($MFT's own runlist outgrowing its record / spanning records). Uses
// dirRecord for record 0 (allocateClusters/setMftRecordBit clobber the shared mftRecord buffer).
static int growMft(NtfsVolume *vol, uint64_t addRecords)
{
   uint32_t rs = vol->mftRecordSize, cb = vol->bytesPerCluster;
   if (readMftRecord(vol, MFT_RECORD_MFT, dirRecord) != 0) return -1;   // F048 ok: $MFT by fixed system record number (seq-0 convention)
   NtfsAttr data;
   if (findAttribute(dirRecord, rs, ATTR_DATA, 0, 0, &data) != 1 || data.resident || data.startVcn != 0) return -1;
   uint32_t dataOff = (uint32_t)(data.attr - dirRecord);
   uint64_t oldRecs = data.realSize / rs;
   // jf-port: grow by a quarter of the $MFT (at least 256 records), not by the 8 the callers ask for.
   // The $MFT may have at most NTFS_MFT_RUNS_MAX fragments, and each growth lands wherever the free
   // clusters after its tail are -- usually not adjacent once files have been written there -- so
   // 8-record steps ran out of fragments after ~250 files and every create failed from then on.
   // Capped at 1/16 of the free space, so a nearly full volume still grows by what it can.
   { uint64_t step = oldRecs / 4 < 256 ? 256 : oldRecs / 4;
     uint64_t capRecs = (vol->freeClusters / 16) * (uint64_t)cb / rs;
     if (step > capRecs) step = capRecs;
     if (addRecords < step) addRecords = step; }
   if (addRecords == 0) addRecords = 1;
   uint64_t addClusters = (addRecords * rs + cb - 1) / cb;
   if (addClusters == 0) addClusters = 1;
   uint64_t newAlloc = data.allocatedSize + addClusters * cb;
   uint64_t newRecs  = newAlloc / rs;

   // --- extend the $MFT $DATA runlist (allocate the new record clusters) ---
   NtfsRunEntry runs[NTFS_MFT_RUNS_MAX]; int rc = 0;
   if (decodeRuns(data.attr + data.runlistOffset, data.attrLength - data.runlistOffset, runs, NTFS_MFT_RUNS_MAX, &rc) != 0)
      return -1;
   // hint the allocator at the $MFT's own tail so new records land in the MFT zone contiguous with the
   // existing runs, then fold the adjacent run in -- keeps the $MFT one fragment instead of one-per-grow,
   // which otherwise reaches NTFS_MFT_RUNS_MAX and permanently stalls all record allocation (mirrors appendData).
   uint64_t hintLcn = (rc > 0 && runs[rc - 1].lcn >= 0) ? (uint64_t)runs[rc - 1].lcn + runs[rc - 1].count : 0;
   if (allocateClusters(vol, addClusters, runs, &rc, NTFS_MFT_RUNS_MAX, hintLcn) != 0) return -1;
   coalesceRuns(runs, &rc);
   uint8_t enc[512];
   int encLen = encodeRuns(runs, rc, enc, (int)sizeof enc);
   if (encLen < 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
   // Format every newly-addressable record as an empty FILE record (like ntfs-3g / Windows), not just
   // zeroed. Done pre-commit via the canonical runlist's real VCNs: if we crash now the new clusters
   // are an unreferenced leak (chkdsk reclaims) since $MFT $DATA size isn't grown until record 0 lands.
   { NtfsRunEntry nr[NTFS_MFT_RUNS_MAX]; int nrc = 0;
     if (decodeRuns(enc, (uint32_t)encLen, nr, NTFS_MFT_RUNS_MAX, &nrc) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
     uint8_t tmpl[NTFS_MAX_RECORD];
     for (uint64_t n = oldRecs; n < newRecs; n++) {
        formatEmptyMftRecord(tmpl, rs, vol->bytesPerSector, n);
        applyUsaWrite(tmpl, rs, vol->bytesPerSector);
        uint64_t bo = n * rs;
        // jf-port: map each cluster of the record.  Upstream mapped only the first and wrote the whole
        // record from there, so with clusters smaller than a record (512-byte clusters, 1 KB records)
        // a record straddling two runs wrote its second half over whatever cluster came next on the
        // disk -- another file's data.  (writeMftRecord already maps cluster by cluster.)
        for (uint32_t done = 0; done < rs; ) {
           const uint64_t b = bo + done;
           int64_t lcn = mapVcnToLcn(nr, nrc, b / cb);
           if (lcn < 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
           const uint32_t inCluster = (uint32_t)(b % cb);
           uint32_t take = cb - inCluster;
           while (done + take < rs && mapVcnToLcn(nr, nrc, (b + take) / cb) == lcn + (int64_t)((inCluster + take) / cb))
              take += cb;                                   // and the clusters right after it, in one write
           if (take > rs - done) take = rs - done;
           uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + inCluster / vol->bytesPerSector;
           if (writeSectors(vol->storageHandle, lba, take / vol->bytesPerSector, tmpl + done) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
           done += take;
        }
     }
   }
   uint16_t runOff = readLe16(dirRecord + dataOff + ATTR_NR_RUNLIST_OFFSET);
   uint32_t oldAttrLen = readLe32(dirRecord + dataOff + ATTR_LENGTH_OFFSET);
   uint32_t newAttrLen = (uint32_t)ALIGN8((uint32_t)runOff + (uint32_t)encLen);
   if (newAttrLen != oldAttrLen &&
       resizeAttribute(dirRecord, rs, dataOff, oldAttrLen, newAttrLen) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
   { uint8_t *a = dirRecord + dataOff;
     writeLe64(a + ATTR_NR_LAST_VCN, newAlloc / cb - 1);
     writeLe64(a + ATTR_NR_ALLOC_SIZE, newAlloc);
     writeLe64(a + ATTR_NR_REAL_SIZE,  newAlloc);
     writeLe64(a + ATTR_NR_VALID_SIZE, newAlloc);
     memSet(a + runOff, 0, newAttrLen - runOff);
     memCopy(a + runOff, enc, encLen); }

   // --- grow the $MFT $BITMAP to cover newRecs bits (re-find: the $DATA resize shifted the record) ---
   NtfsAttr bm;
   if (findAttribute(dirRecord, rs, ATTR_BITMAP, 0, 0, &bm) != 1) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
   uint64_t neededBytes = (newRecs + 7) / 8;
   uint32_t bmOff = (uint32_t)(bm.attr - dirRecord);
   if (bm.resident) {
      uint32_t vOff = readLe16(dirRecord + bmOff + ATTR_RES_VALUE_OFFSET);
      uint32_t curBytes = bm.valueLength;
      if (neededBytes > curBytes) {
         uint32_t oldA = readLe32(dirRecord + bmOff + ATTR_LENGTH_OFFSET);
         uint32_t newA = (uint32_t)ALIGN8(vOff + neededBytes);
         if (newA > rs || resizeAttribute(dirRecord, rs, bmOff, oldA, newA) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
         writeLe32(dirRecord + bmOff + ATTR_RES_VALUE_LENGTH, (uint32_t)neededBytes);
         memSet(dirRecord + bmOff + vOff + curBytes, 0, (uint32_t)(neededBytes - curBytes));
      }
   } else {
      if (neededBytes > bm.allocatedSize) {                   // extend the non-resident bitmap's clusters
         uint64_t addBm = (neededBytes - bm.allocatedSize + cb - 1) / cb;
         NtfsRunEntry br[NTFS_MFT_RUNS_MAX]; int brc = 0;
         if (decodeRuns(bm.attr + bm.runlistOffset, bm.attrLength - bm.runlistOffset, br, NTFS_MFT_RUNS_MAX, &brc) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
         int bpre = brc;
         if (allocateClusters(vol, addBm, br, &brc, NTFS_MFT_RUNS_MAX, 0) != 0) { freeTailClusters(vol, runs, &rc, addClusters); return -1; }
         for (int i = bpre; i < brc; i++)
            if (zeroClusters(vol, br[i].lcn, br[i].count) != 0) { freeClusterRuns(vol, br + bpre, brc - bpre); freeTailClusters(vol, runs, &rc, addClusters); return -1; }
         uint8_t benc[512]; int bencLen = encodeRuns(br, brc, benc, (int)sizeof benc);
         if (bencLen < 0) { freeClusterRuns(vol, br + bpre, brc - bpre); freeTailClusters(vol, runs, &rc, addClusters); return -1; }
         uint16_t bRunOff = readLe16(dirRecord + bmOff + ATTR_NR_RUNLIST_OFFSET);
         uint32_t bOld = readLe32(dirRecord + bmOff + ATTR_LENGTH_OFFSET);
         uint32_t bNew = (uint32_t)ALIGN8((uint32_t)bRunOff + (uint32_t)bencLen);
         uint64_t bNewAlloc = bm.allocatedSize + addBm * cb;
         if (bNew != bOld && resizeAttribute(dirRecord, rs, bmOff, bOld, bNew) != 0) { freeClusterRuns(vol, br + bpre, brc - bpre); freeTailClusters(vol, runs, &rc, addClusters); return -1; }
         uint8_t *ba = dirRecord + bmOff;
         writeLe64(ba + ATTR_NR_LAST_VCN, bNewAlloc / cb - 1);
         writeLe64(ba + ATTR_NR_ALLOC_SIZE, bNewAlloc);
         writeLe64(ba + ATTR_NR_REAL_SIZE,  neededBytes);
         writeLe64(ba + ATTR_NR_VALID_SIZE, neededBytes);
         memSet(ba + bRunOff, 0, bNew - bRunOff);
         memCopy(ba + bRunOff, benc, bencLen);
      } else if (neededBytes > bm.realSize) {                 // already-allocated clusters cover it: bump sizes
         writeLe64(dirRecord + bmOff + ATTR_NR_REAL_SIZE,  neededBytes);
         writeLe64(dirRecord + bmOff + ATTR_NR_VALID_SIZE, neededBytes);
      }
   }

   // --- commit record 0 (mirrors $MFTMirr), then publish the new runlist cache + free the new bits ---
   if (writeMftRecord(vol, MFT_RECORD_MFT, dirRecord) != 0) return -1;   // on fail: leaked clusters (chkdsk reclaims)
   // Refresh the cached $MFT runlist by DECODING the canonical encoded runlist (allocateClusters leaves
   // appended runs with vcn=0; encodeRuns is positional so the on-disk runlist is correct, but the
   // decoded cache needs the real sequential VCNs or readMftRecord/writeMftRecord mis-map new records).
   int nrc = 0;
   if (decodeRuns(enc, (uint32_t)encLen, vol->mftRuns, NTFS_MFT_RUNS_MAX, &nrc) != 0) return -1;
   vol->mftRunCount = nrc;
   vol->mftDataSize = newAlloc;
   // mark the new records free (also scrubs any stale bits in a partially-covered bitmap byte). A
   // failure here only leaves a record marked in-use-but-unused = a chkdsk-repairable bit, never data loss.
   for (uint64_t n = oldRecs; n < newRecs; n++) setMftRecordBit(vol, n, 0);
   return 0;
}

// Finds the first free MFT record number (>= 24, reserved records skipped) in $MFT's $BITMAP, or
// ~0 if none free within the existing MFT. Does not modify the bitmap. Caller grows the MFT (growMft)
// when this returns ~0.
static uint64_t findFreeMftRecord(NtfsVolume *vol)
{
   if (readMftRecord(vol, MFT_RECORD_MFT, mftRecord) != 0) return ~0ull;   // F048 ok: $MFT by fixed system record number (seq-0 convention)
   NtfsAttr bitmap;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_BITMAP, 0, 0, &bitmap) != 1) return ~0ull;
   uint64_t recordCount = vol->mftDataSize / vol->mftRecordSize;

   if (bitmap.resident) {
      for (uint64_t n = 24; n < recordCount && n / 8 < bitmap.valueLength; n++)
         if (!((bitmap.value[n / 8] >> (n % 8)) & 1)) return n;
      return ~0ull;
   }
   const uint8_t *runlist = bitmap.attr + bitmap.runlistOffset;
   uint32_t runlistLength = bitmap.attrLength - bitmap.runlistOffset;
   uint32_t sectorSize = vol->bytesPerSector, bytesPerCluster = vol->bytesPerCluster;
   uint64_t bitmapBytes = bitmap.realSize;                 // don't scan past the bitmap's valid data: a crafted
   uint64_t loadedLba = ~0ull;                             // $MFT whose $BITMAP is shorter than recordCount/8
   for (uint64_t n = 24; n < recordCount; n++) {           // would otherwise read whatever clusters trail it
      uint64_t byteIndex = n / 8;
      if (byteIndex >= bitmapBytes) break;
      int64_t lcn = mapRunlistVcn(runlist, runlistLength, byteIndex / bytesPerCluster);
      if (lcn < 0) return ~0ull;
      uint32_t inCluster = (uint32_t)(byteIndex % bytesPerCluster);
      uint32_t alignedStart = inCluster - (inCluster % sectorSize);
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + alignedStart / sectorSize;
      if (lba != loadedLba) { if (readSectors(vol->storageHandle, lba, 1, fileBounce) != 0) return ~0ull; loadedLba = lba; }
      if (!((fileBounce[inCluster - alignedStart] >> (n % 8)) & 1)) return n;
   }
   return ~0ull;
}

// Sets (value!=0) or clears the $MFT $BITMAP bit for record `number`. Returns 0 / -1.
static int setMftRecordBit(NtfsVolume *vol, uint64_t number, int value)
{
   if (readMftRecord(vol, MFT_RECORD_MFT, mftRecord) != 0) return -1;   // F048 ok: $MFT by fixed system record number (seq-0 convention)
   NtfsAttr bitmap;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_BITMAP, 0, 0, &bitmap) != 1) return -1;
   uint64_t byteIndex = number / 8;
   uint8_t mask = (uint8_t)(1 << (number % 8));

   if (bitmap.resident) {
      if (byteIndex >= bitmap.valueLength) return -1;
      uint8_t *bits = (uint8_t *)bitmap.value;
      if (value) bits[byteIndex] |= mask; else bits[byteIndex] &= (uint8_t)~mask;
      return writeMftRecord(vol, MFT_RECORD_MFT, mftRecord);
   }
   const uint8_t *runlist = bitmap.attr + bitmap.runlistOffset;
   uint32_t runlistLength = bitmap.attrLength - bitmap.runlistOffset;
   uint32_t sectorSize = vol->bytesPerSector, bytesPerCluster = vol->bytesPerCluster;
   int64_t lcn = mapRunlistVcn(runlist, runlistLength, byteIndex / bytesPerCluster);
   if (lcn < 0) return -1;
   uint32_t inCluster = (uint32_t)(byteIndex % bytesPerCluster);
   uint32_t alignedStart = inCluster - (inCluster % sectorSize);
   uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + alignedStart / sectorSize;
   if (readSectors(vol->storageHandle, lba, 1, fileBounce) != 0) return -1;
   uint32_t byteInSector = inCluster - alignedStart;
   if (value) fileBounce[byteInSector] |= mask; else fileBounce[byteInSector] &= (uint8_t)~mask;
   return writeSectors(vol->storageHandle, lba, 1, fileBounce);
}

// Standard NTFS $UpCase simple-uppercase fold (Windows XP table, transcribed from the ntfs-3g
// uc_run / uc_dup / uc_byte run tables in docs/ntfs/ntfs-3g). Applied on the fly so the memory-tight
// PRX holds no 128 KiB $UpCase table. Covers the realistic filename charset - Latin-1, Latin
// Extended-A, Greek, Cyrillic, Armenian - so a directory B-tree that Windows ordered by $UpCase
// collates identically here. Codepoints outside these ranges fold to identity, exactly as XP's table
// does for them; the post-XP-only foldings of rare scripts (Latin Extended-B, Coptic, Glagolitic, ...)
// are not reproduced, which only matters for those exotic letters inside a large directory B-tree.
static const int upcaseRunTable[][3] = {   // [start, end), add
   {0x0061,0x007B,-32},{0x0451,0x045D,-80},{0x1F70,0x1F72, 74},
   {0x00E0,0x00F7,-32},{0x045E,0x0460,-80},{0x1F72,0x1F76, 86},
   {0x00F8,0x00FF,-32},{0x0561,0x0587,-48},{0x1F76,0x1F78,100},
   {0x0256,0x0258,-205},{0x1F00,0x1F08, 8},{0x1F78,0x1F7A,128},
   {0x028A,0x028C,-217},{0x1F10,0x1F16, 8},{0x1F7A,0x1F7C,112},
   {0x03AC,0x03AD,-38},{0x1F20,0x1F28, 8},{0x1F7C,0x1F7E,126},
   {0x03AD,0x03B0,-37},{0x1F30,0x1F38, 8},{0x1FB0,0x1FB2,  8},
   {0x03B1,0x03C2,-32},{0x1F40,0x1F46, 8},{0x1FD0,0x1FD2,  8},
   {0x03C2,0x03C3,-31},{0x1F51,0x1F52, 8},{0x1FE0,0x1FE2,  8},
   {0x03C3,0x03CC,-32},{0x1F53,0x1F54, 8},{0x1FE5,0x1FE6,  7},
   {0x03CC,0x03CD,-64},{0x1F55,0x1F56, 8},{0x2170,0x2180,-16},
   {0x03CD,0x03CF,-63},{0x1F57,0x1F58, 8},{0x24D0,0x24EA,-26},
   {0x0430,0x0450,-32},{0x1F60,0x1F68, 8},{0xFF41,0xFF5B,-32},
   {0,0,0}
};
static const int upcaseDupTable[][2] = {   // [start, end): even codepoint is upper, odd folds to even
   {0x0100,0x012F},{0x01A0,0x01A6},{0x03E2,0x03EF},{0x04CB,0x04CC},
   {0x0132,0x0137},{0x01B3,0x01B7},{0x0460,0x0481},{0x04D0,0x04EB},
   {0x0139,0x0149},{0x01CD,0x01DD},{0x0490,0x04BF},{0x04EE,0x04F5},
   {0x014A,0x0178},{0x01DE,0x01EF},{0x04BF,0x04BF},{0x04F8,0x04F9},
   {0x0179,0x017E},{0x01F4,0x01F5},{0x04C1,0x04C4},{0x1E00,0x1E95},
   {0x018B,0x018B},{0x01FA,0x0218},{0x04C7,0x04C8},{0x1EA0,0x1EF9},
   {0,0}
};
static const int upcaseByteTable[][2] = {  // single codepoint -> uppercase
   {0x00FF,0x0178},{0x01AD,0x01AC},{0x01F3,0x01F1},{0x0269,0x0196},
   {0x0183,0x0182},{0x01B0,0x01AF},{0x0253,0x0181},{0x026F,0x019C},
   {0x0185,0x0184},{0x01B9,0x01B8},{0x0254,0x0186},{0x0272,0x019D},
   {0x0188,0x0187},{0x01BD,0x01BC},{0x0259,0x018F},{0x0275,0x019F},
   {0x018C,0x018B},{0x01C6,0x01C4},{0x025B,0x0190},{0x0283,0x01A9},
   {0x0192,0x0191},{0x01C9,0x01C7},{0x0260,0x0193},{0x0288,0x01AE},
   {0x0199,0x0198},{0x01CC,0x01CA},{0x0263,0x0194},{0x0292,0x01B7},
   {0x01A8,0x01A7},{0x01DD,0x018E},{0x0268,0x0197},
   {0,0}
};

static int upcaseUnit(uint16_t u)
{
   int c = u;
   for (int r = 0; upcaseRunTable[r][0]; r++)
      if (u >= upcaseRunTable[r][0] && u < upcaseRunTable[r][1]) { c = u + upcaseRunTable[r][2]; break; }
   for (int r = 0; upcaseDupTable[r][0]; r++) {
      int start = upcaseDupTable[r][0], end = upcaseDupTable[r][1];
      if ((int)u - 1 >= start && (int)u - 1 < end && (((int)u - 1 - start) & 1) == 0) { c = u - 1; break; }
   }
   for (int r = 0; upcaseByteTable[r][0]; r++)
      if (u == upcaseByteTable[r][0]) { c = upcaseByteTable[r][1]; break; }
   return c;
}

// Compares two $FILE_NAME keys by their names ($UpCase case-insensitive collation). <0, 0, >0.
static int compareFileNameKeys(const uint8_t *a, const uint8_t *b)
{
   uint8_t la = a[FN_NAME_LENGTH], lb = b[FN_NAME_LENGTH];
   uint8_t n = la < lb ? la : lb;
   for (uint8_t i = 0; i < n; i++) {
      int ca = upcaseUnit(readLe16(a + FN_NAME + i * 2)), cb = upcaseUnit(readLe16(b + FN_NAME + i * 2));
      if (ca != cb) return ca - cb;
   }
   return (int)la - (int)lb;
}

// Validates one index entry at node[pos] before we trust any of its on-disk lengths. Every field is
// attacker-controlled (a corrupt or crafted directory record), so we confirm the entry header fits
// within usedSize, its length is 8-aligned and in-bounds, and (for a keyed entry) the $FILE_NAME key
// is self-consistent and in-bounds so compareFileNameKeys cannot over-read. usedSize must already be
// bounded against the resident value. Returns the entry length (>0) for a keyed entry, 0 for the
// last-entry marker, -1 if malformed (caller refuses rather than corrupts).
static int validateIndexEntry(const uint8_t *node, uint32_t pos, uint32_t usedSize)
{
   if (pos + 16 > usedSize) return -1;                                  // entry header must fit
   const uint8_t *entry = node + pos;
   uint16_t length = readLe16(entry + IDXENTRY_LENGTH);
   if (length < 16 || (length & 7) || pos + length > usedSize) return -1;   // 8-aligned, fully in-bounds
   if (readLe16(entry + IDXENTRY_FLAGS) & IDXENTRY_FLAG_LAST) return 0;     // last marker carries no key
   uint16_t keyLength = readLe16(entry + IDXENTRY_KEY_LENGTH);
   if ((uint32_t)IDXENTRY_KEY + keyLength > length) return -1;          // key fits within the entry
   if (keyLength < FN_NAME) return -1;                                  // room for the $FILE_NAME header
   uint8_t nameLen = entry[IDXENTRY_KEY + FN_NAME_LENGTH];
   if ((uint32_t)FN_NAME + (uint32_t)nameLen * 2 > keyLength) return -1;    // name fits within the key
   return (int)length;
}

// ===========================================================================
// Write path W6: index B-tree growth ($INDEX_ALLOCATION + node split). Lets
// create/mkdir/rename insert into a directory of any size. See NTFS-W6-DESIGN.md.
// A node lives either in the resident $INDEX_ROOT (inside the dir FILE record,
// node header at value+16) or in an INDX block (node header at block+24). The
// $I30 index is reconstructible from each file's $FILE_NAME, so a torn multi-
// sector INDX write is chkdsk-repairable (W9/$LogFile makes it truly atomic).
// ===========================================================================

// node header accessors (nd points at the 16-byte index node header)
static uint32_t getNodeUsedSize(const uint8_t *nd)     { return readLe32(nd + IDXNODE_USED_SIZE); }
static uint32_t getNodeEntriesOffset(const uint8_t *nd)  { return readLe32(nd + IDXNODE_ENTRIES_OFFSET); }
static int      isNodeLeaf(const uint8_t *nd)   { return (readLe32(nd + IDXNODE_FLAGS_OFFSET) & 1) == 0; }

// Returns a pointer to a node's last-entry marker, or 0 if the node is malformed.
static uint8_t *getNodeEndMarker(uint8_t *nd)
{
   uint32_t used = getNodeUsedSize(nd);
   uint32_t pos = getNodeEntriesOffset(nd);
   for (int guard = 0; guard < 8192; guard++) {
      int ev = validateIndexEntry(nd, pos, used);
      if (ev < 0) return 0;
      if (ev == 0) return nd + pos;
      pos += (uint32_t)ev;
   }
   return 0;
}

// VCN <-> index-block helpers. An INDX block's VCN (in parent sub-node pointers and the INDX header) is
// its byte offset in $INDEX_ALLOCATION divided by the index VCN unit: the cluster size when a cluster is
// no larger than an index record, otherwise 512 bytes - the sub-cluster case (e.g. 4 KB index records on
// a 64 KB cluster), where several INDX blocks pack into one cluster. Spec/ntfs-3g:
// vcn_size = (cluster_size <= index_block_size) ? cluster_size : sector_size. For cluster <= block these
// reduce to the old cluster math (VCN = blockIndex * indexRecordSize / bytesPerCluster).
static uint64_t getIndexVcnSize(const NtfsVolume *vol)
{
   return vol->bytesPerCluster <= vol->indexRecordSize ? vol->bytesPerCluster : vol->bytesPerSector;
}
static uint64_t blockToVcn(const NtfsVolume *vol, uint64_t blockIndex)
{
   return (blockIndex * vol->indexRecordSize) / getIndexVcnSize(vol);
}

// Builds an empty INDX block (one last-entry marker) into buf. isLeaf selects the node flags; an
// internal marker additionally carries a child VCN the caller fills in. Returns 0.
static int buildEmptyIndxBlock(uint8_t *buf, uint32_t blockSize, uint32_t sectorSize, uint64_t vcn, int isLeaf)
{
   memSet(buf, 0, (int)blockSize);
   buf[0] = 'I'; buf[1] = 'N'; buf[2] = 'D'; buf[3] = 'X';
   uint16_t usaCount = (uint16_t)(1 + blockSize / sectorSize);
   writeLe16(buf + FILE_USA_OFFSET, 40);                      // USA at 0x28, clear of the node header
   writeLe16(buf + FILE_USA_COUNT, usaCount);
   writeLe64(buf + INDX_VCN_OFFSET, vcn);
   uint8_t *nd = buf + INDX_NODE_HEADER;
   uint32_t entriesOff = (uint32_t)ALIGN8(16 + (uint32_t)usaCount * 2);   // node-relative, past the USA
   uint32_t markerLen = isLeaf ? 16 : 24;
   writeLe32(nd + IDXNODE_ENTRIES_OFFSET, entriesOff);
   writeLe32(nd + IDXNODE_USED_SIZE, entriesOff + markerLen);
   writeLe32(nd + IDXNODE_ALLOC_SIZE, blockSize - INDX_NODE_HEADER);
   writeLe32(nd + IDXNODE_FLAGS_OFFSET, isLeaf ? 0 : 1);
   uint8_t *marker = nd + entriesOff;
   writeLe16(marker + IDXENTRY_LENGTH, markerLen);
   writeLe16(marker + IDXENTRY_FLAGS, isLeaf ? IDXENTRY_FLAG_LAST : (IDXENTRY_FLAG_LAST | IDXENTRY_FLAG_NODE));
   writeLe16(buf + 40, 1);                                    // initial USN (applyUsaWrite bumps it)
   return 0;
}

// Reads INDX block at cluster `vcn` into buf, applies the USA fixup, validates the magic. Returns 0/-1.
static int readIndxBlock(const NtfsVolume *vol, const NtfsAttr *alloc, uint64_t vcn, uint8_t *buf)
{
   uint32_t blockSize = vol->indexRecordSize;
   uint64_t byteOffset = vcn * getIndexVcnSize(vol);
   if (readNonResident(vol, alloc, alloc->validSize, byteOffset, buf, blockSize) != (int64_t)blockSize) return -1;
   if (!(buf[0] == 'I' && buf[1] == 'N' && buf[2] == 'D' && buf[3] == 'X')) return -1;
   if (applyUsaFixup(buf, blockSize, vol->bytesPerSector) != 0) return -1;
   if (readLe32(buf + INDX_NODE_HEADER + IDXNODE_USED_SIZE) > blockSize - INDX_NODE_HEADER) return -1;   // node lies about its size
   return 0;
}

// Writes buf back to INDX block at cluster `vcn` (USA write-encode + non-resident write). Returns 0/-1.
static int writeIndxBlock(const NtfsVolume *vol, const NtfsAttr *alloc, uint64_t vcn, uint8_t *buf)
{
   uint32_t blockSize = vol->indexRecordSize;
   applyUsaWrite(buf, blockSize, vol->bytesPerSector);
   uint64_t byteOffset = vcn * getIndexVcnSize(vol);
   int64_t done = writeNonResident(vol, alloc, byteOffset, buf, blockSize);
   applyUsaFixup(buf, blockSize, vol->bytesPerSector);       // restore in-memory copy for further edits
   return done == (int64_t)blockSize ? 0 : -1;
}

// Inserts a keyed entry (optionally carrying a child VCN) into a node in collation order. Returns 0,
// -2 if the name already exists, or -1 if it won't fit `cap` (node-relative byte budget) / malformed.
static int insertNodeEntry(uint8_t *nd, uint32_t cap, uint64_t fileRef,
                        const uint8_t *key, int keyLen, int hasChild, uint64_t childVcn)
{
   uint32_t used = getNodeUsedSize(nd);
   uint32_t entryLen = (uint32_t)ALIGN8(IDXENTRY_KEY + (uint32_t)keyLen + (hasChild ? 8u : 0u));
   if (used + entryLen > cap) return -1;                      // caller must split first

   uint32_t pos = getNodeEntriesOffset(nd);
   for (int guard = 0; guard < 8192; guard++) {
      int ev = validateIndexEntry(nd, pos, used);
      if (ev < 0) return -1;
      if (ev == 0) break;                                    // last marker: insert before it
      int cmp = compareFileNameKeys(key, nd + pos + IDXENTRY_KEY);
      if (cmp == 0) return -2;
      if (cmp < 0) break;
      pos += (uint32_t)ev;
   }
   memMove(nd + pos + entryLen, nd + pos, (int)(used - pos));  // open a gap before nd[pos]
   uint8_t *e = nd + pos;
   memSet(e, 0, (int)entryLen);
   writeLe64(e + IDXENTRY_FILE_REF, fileRef);
   writeLe16(e + IDXENTRY_LENGTH, (uint16_t)entryLen);
   writeLe16(e + IDXENTRY_KEY_LENGTH, (uint16_t)keyLen);
   writeLe16(e + IDXENTRY_FLAGS, hasChild ? IDXENTRY_FLAG_NODE : 0);
   memCopy(e + IDXENTRY_KEY, key, keyLen);
   if (hasChild) writeLe64(e + entryLen - 8, childVcn);
   writeLe32(nd + IDXNODE_USED_SIZE, used + entryLen);
   return 0;
}

// Repoints the child VCN of whichever entry (incl. the marker) currently points at oldVcn. Returns 0/-1.
static int replaceChildVcn(uint8_t *nd, uint64_t oldVcn, uint64_t newVcn)
{
   uint32_t used = getNodeUsedSize(nd);
   uint32_t pos = getNodeEntriesOffset(nd);
   for (int guard = 0; guard < 8192; guard++) {
      if (pos + 16 > used) return -1;
      uint8_t *e = nd + pos;
      uint16_t len = readLe16(e + IDXENTRY_LENGTH);
      uint16_t fl  = readLe16(e + IDXENTRY_FLAGS);
      if (len < 16 || pos + len > used) return -1;
      if (fl & IDXENTRY_FLAG_NODE) {
         if (readLe64(e + len - 8) == oldVcn) { writeLe64(e + len - 8, newVcn); return 0; }
      }
      if (fl & IDXENTRY_FLAG_LAST) return -1;
      pos += len;
   }
   return -1;
}

// Removes the entry at node-relative offset `pos` from node `nd`, compacting the following entries
// and shrinking usedSize. Works on any node buffer (INDX block or the resident $INDEX_ROOT node).
// Returns the removed entry's child VCN (or 0 for a leaf entry), or ~0 on a malformed node.
static uint64_t removeNodeEntryAt(uint8_t *nd, uint32_t pos)
{
   uint32_t used = getNodeUsedSize(nd);
   if (pos + 16 > used) return ~0ull;
   uint16_t len = readLe16(nd + pos + IDXENTRY_LENGTH);
   if (len < 16 || pos + len > used) return ~0ull;
   uint64_t child = (readLe16(nd + pos + IDXENTRY_FLAGS) & IDXENTRY_FLAG_NODE) ? readLe64(nd + pos + len - 8) : 0;
   memMove(nd + pos, nd + pos + len, (int)(used - pos - len));   // compact the tail over the hole
   writeLe32(nd + IDXNODE_USED_SIZE, used - len);
   return child;
}

// Splits a full INDX node `nd` (leaf or internal): the lower half stays, the upper half moves to the
// freshly-built block `rightNd`, and the median entry is returned (key + fileRef) for the caller to
// promote into the parent. For an internal node the median's own child becomes the left node's new
// rightmost child. `leftVcn` is nd's own VCN (so the marker copy is consistent). Returns 0/-1.
static int splitNode(uint8_t *nd, uint8_t *rightNd, uint32_t rightCap,
                       uint8_t *medKey, int *medKeyLen, uint64_t *medFileRef, uint64_t *medChildOut)
{
   int isLeaf = isNodeLeaf(nd);
   uint32_t used = getNodeUsedSize(nd);
   uint32_t offs[512];
   int n = 0;
   uint32_t pos = getNodeEntriesOffset(nd);
   for (int guard = 0; guard < 8192 && n < 512; guard++) {
      int ev = validateIndexEntry(nd, pos, used);
      if (ev < 0) return -1;
      if (ev == 0) break;
      offs[n++] = pos; pos += (uint32_t)ev;
   }
   uint32_t markerPos = pos;                                  // original last marker (belongs to the right half)
   if (n < 2) return -1;                                      // nothing to split
   int m = n / 2;
   uint8_t *med = nd + offs[m];
   uint16_t medLen = readLe16(med + IDXENTRY_LENGTH);
   uint16_t medKL  = readLe16(med + IDXENTRY_KEY_LENGTH);
   if (medKL > FN_NAME + 255 * 2 || (uint32_t)IDXENTRY_KEY + medKL > medLen) return -1;  // 576 = max $FILE_NAME key
   *medFileRef = readLe64(med + IDXENTRY_FILE_REF);
   *medKeyLen  = medKL;
   memCopy(medKey, med + IDXENTRY_KEY, medKL);
   uint64_t medChild = isLeaf ? 0 : readLe64(med + medLen - 8);   // becomes left node's new rightmost child
   if (medChildOut) *medChildOut = medChild;

   // right node: insert the upper entries e[m+1..n-1], then make its marker = the original marker
   uint8_t *rnd = rightNd + INDX_NODE_HEADER;
   for (int i = m + 1; i < n; i++) {
      uint8_t *e = nd + offs[i];
      uint16_t len = readLe16(e + IDXENTRY_LENGTH);
      uint16_t kl  = readLe16(e + IDXENTRY_KEY_LENGTH);
      int hasChild = (readLe16(e + IDXENTRY_FLAGS) & IDXENTRY_FLAG_NODE) != 0;
      uint64_t ch  = hasChild ? readLe64(e + len - 8) : 0;
      if (insertNodeEntry(rnd, rightCap, readLe64(e + IDXENTRY_FILE_REF), e + IDXENTRY_KEY, kl, hasChild, ch) != 0)
         return -1;
   }
   if (!isLeaf) {                                             // right node's rightmost child = original marker's
      uint8_t *rmarker = getNodeEndMarker(rnd);
      uint8_t *omarker = nd + markerPos;
      if (!rmarker) return -1;
      writeLe64(rmarker + readLe16(rmarker + IDXENTRY_LENGTH) - 8, readLe64(omarker + readLe16(omarker + IDXENTRY_LENGTH) - 8));
   }

   // left node (nd): truncate to e[0..m-1] then append a fresh marker (leaf: bare; internal: child=medChild)
   uint32_t leftEnd = offs[m];                                // drop median and everything after
   uint32_t markerLen = isLeaf ? 16 : 24;
   uint8_t *lm = nd + leftEnd;
   memSet(lm, 0, (int)markerLen);
   writeLe16(lm + IDXENTRY_LENGTH, (uint16_t)markerLen);
   writeLe16(lm + IDXENTRY_FLAGS, isLeaf ? IDXENTRY_FLAG_LAST : (IDXENTRY_FLAG_LAST | IDXENTRY_FLAG_NODE));
   if (!isLeaf) writeLe64(lm + markerLen - 8, medChild);
   writeLe32(nd + IDXNODE_USED_SIZE, leftEnd + markerLen);
   return 0;
}

// Offset of the ATTR_END marker in a FILE record, or 0 if not found/malformed.
static uint32_t getEndMarkerOffset(const uint8_t *record, uint32_t recordSize)
{
   uint32_t used = readLe32(record + FILE_USED_SIZE);
   uint32_t limit = used <= recordSize ? used : recordSize;
   uint32_t off = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   for (int guard = 0; guard < 256; guard++) {
      if (off + 4 > limit) return 0;
      if (readLe32(record + off + ATTR_TYPE_OFFSET) == ATTR_END) return off;
      if (off + 16 > limit) return 0;
      uint32_t len = readLe32(record + off + ATTR_LENGTH_OFFSET);
      if (len < 16 || len > limit - off) return 0;
      off += len;
   }
   return 0;
}

// Creates or replaces the directory's $INDEX_ALLOCATION attribute (non-resident, name $I30) in
// dirRecord with the given runlist covering `blocks` index blocks. Returns 0/-1.
static int putIndexAlloc(NtfsVolume *vol, const NtfsRunEntry *runs, int runCount, uint64_t blocks)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint64_t bytes = blocks * vol->indexRecordSize;
   uint64_t allocClusters = (bytes + vol->bytesPerCluster - 1) / vol->bytesPerCluster;   // round blocks up to whole clusters
   uint64_t allocBytes = allocClusters * vol->bytesPerCluster;
   uint8_t enc[256];
   int encLen = encodeRuns(runs, runCount, enc, (int)sizeof enc);
   if (encLen < 0) return -1;
   uint16_t nameOff = ATTR_NR_HEADER_MIN;                     // 64: name follows the NR header
   uint16_t runOff  = (uint16_t)ALIGN8((uint32_t)nameOff + 8);// $I30 = 4 units = 8 bytes -> runlist at 72
   uint32_t newLen  = (uint32_t)ALIGN8((uint32_t)runOff + (uint32_t)encLen);

   NtfsAttr ia;
   int have = findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia) == 1;
   uint32_t off;
   uint16_t id;
   if (have) {
      off = (uint32_t)(ia.attr - dirRecord);
      id  = readLe16(dirRecord + off + ATTR_ID_OFFSET);
      if (resizeAttribute(dirRecord, recordSize, off, ia.attrLength, newLen) != 0) return -1;
   } else {
      // Insert $INDEX_ALLOCATION (0xA0) BEFORE the index $BITMAP (0xB0) to keep the attribute chain in
      // ascending type order. allocIndexBlock always creates $BITMAP first, so appending here would
      // leave 0xB0 before 0xA0 -- an out-of-order chain Windows/chkdsk reject as a corrupt MFT record.
      NtfsAttr bmI30;
      uint32_t bitmapOff = (findAttribute(dirRecord, recordSize, ATTR_BITMAP, indexNameI30, 4, &bmI30) == 1)
                         ? (uint32_t)(bmI30.attr - dirRecord) : 0;
      id = readLe16(dirRecord + 40);
      writeLe16(dirRecord + 40, (uint16_t)(id + 1));
      if (bitmapOff != 0) {                                    // open a gap at $BITMAP's offset (it + end marker shift right)
         uint32_t used = readLe32(dirRecord + FILE_USED_SIZE);
         if (used + newLen > recordSize) return -1;
         off = bitmapOff;
         memMove(dirRecord + off + newLen, dirRecord + off, (int)(used - off));
         writeLe32(dirRecord + FILE_USED_SIZE, used + newLen);
      } else {                                                 // no $BITMAP yet: append at the end marker
         off = getEndMarkerOffset(dirRecord, recordSize);
         if (off == 0 || off + newLen + 4 > recordSize) return -1;
         writeLe32(dirRecord + off + newLen, ATTR_END);
         writeLe32(dirRecord + FILE_USED_SIZE, ALIGN8(off + newLen + 4));   // +8 convention: marker (4B) + 4B slack, like buildNewRecord
      }
   }
   uint8_t *a = dirRecord + off;
   memSet(a, 0, (int)newLen);
   writeLe32(a + ATTR_TYPE_OFFSET, ATTR_INDEX_ALLOCATION);
   writeLe32(a + ATTR_LENGTH_OFFSET, newLen);
   a[ATTR_NON_RESIDENT] = 1;
   a[ATTR_NAME_LENGTH]  = 4;
   writeLe16(a + ATTR_NAME_OFFSET, nameOff);
   writeLe16(a + ATTR_ID_OFFSET, id);
   writeLe64(a + ATTR_NR_START_VCN, 0);
   writeLe64(a + ATTR_NR_LAST_VCN, allocClusters - 1);          // attribute runlist highest VCN is cluster-based
   writeLe16(a + ATTR_NR_RUNLIST_OFFSET, runOff);
   writeLe64(a + ATTR_NR_ALLOC_SIZE, allocBytes);
   writeLe64(a + ATTR_NR_REAL_SIZE, bytes);
   writeLe64(a + ATTR_NR_VALID_SIZE, bytes);
   for (int i = 0; i < 4; i++) writeLe16(a + nameOff + i * 2, indexNameI30[i]);
   memCopy(a + runOff, enc, encLen);
   return 0;
}

// Allocates a new index block for the directory: finds/creates the $BITMAP:$I30, claims the first
// free block bit, and extends $INDEX_ALLOCATION to cover it. Edits dirRecord IN MEMORY only (the
// global $Bitmap clusters are written by allocateClusters); the CALLER persists dirRecord after it
// has written the new block's contents, so a pointer is never published before its block. Returns the
// new block's VCN (clusters) or ~0 on failure. dirRef is unused now (kept for signature stability).
static uint64_t allocIndexBlock(NtfsVolume *vol, uint64_t dirRef)
{
   (void)dirRef;
   uint32_t recordSize = vol->mftRecordSize;

   // --- $BITMAP:$I30: find or create (resident), claim first free bit b ---
   NtfsAttr bm;
   int haveBm = findAttribute(dirRecord, recordSize, ATTR_BITMAP, indexNameI30, 4, &bm) == 1;
   if (haveBm && !bm.resident) return ~0ull;                 // non-resident index bitmap: deferred (W8)
   uint32_t bmOff;
   uint32_t bmValueLen;
   if (haveBm) { bmOff = (uint32_t)(bm.attr - dirRecord); bmValueLen = bm.valueLength; }
   else {
      uint8_t zero[8] = {0,0,0,0,0,0,0,0};
      uint32_t off = getEndMarkerOffset(dirRecord, recordSize);
      if (off == 0) return ~0ull;
      uint16_t id = readLe16(dirRecord + 40);
      uint32_t end = emitResidentAttr(dirRecord, off, ATTR_BITMAP, id, 0, indexNameI30, 4, zero, 8);
      if (end + 4 > recordSize) return ~0ull;
      writeLe16(dirRecord + 40, (uint16_t)(id + 1));
      writeLe32(dirRecord + end, ATTR_END);
      writeLe32(dirRecord + FILE_USED_SIZE, ALIGN8(end + 4));   // +8 convention: marker (4B) + 4B slack, like buildNewRecord
      if (findAttribute(dirRecord, recordSize, ATTR_BITMAP, indexNameI30, 4, &bm) != 1) return ~0ull;
      bmOff = (uint32_t)(bm.attr - dirRecord); bmValueLen = bm.valueLength;
   }
   // find the first 0 bit
   uint8_t *bits = dirRecord + bmOff + readLe16(dirRecord + bmOff + ATTR_RES_VALUE_OFFSET);
   uint64_t b = ~0ull;
   for (uint32_t byte = 0; byte < bmValueLen && b == ~0ull; byte++)
      for (int bit = 0; bit < 8; bit++)
         if (!((bits[byte] >> bit) & 1)) { b = byte * 8u + bit; break; }
   if (b == ~0ull) {                                         // bitmap full: grow it by one byte
      uint32_t vOff = readLe16(dirRecord + bmOff + ATTR_RES_VALUE_OFFSET);
      uint32_t oldAttr = readLe32(dirRecord + bmOff + ATTR_LENGTH_OFFSET);
      uint32_t newAttr = (uint32_t)ALIGN8(vOff + bmValueLen + 1);
      if (resizeAttribute(dirRecord, recordSize, bmOff, oldAttr, newAttr) != 0) return ~0ull;
      writeLe32(dirRecord + bmOff + ATTR_RES_VALUE_LENGTH, bmValueLen + 1);
      bits = dirRecord + bmOff + vOff;
      bits[bmValueLen] = 0;
      b = (uint64_t)bmValueLen * 8u;
      bmValueLen += 1;
   }
   bits[b / 8] |= (uint8_t)(1 << (b % 8));                   // claim the bit

   // --- $INDEX_ALLOCATION: ensure it covers block b, physically (clusters) and logically (real size) ---
   // Index blocks pack indexRecordSize at a time into cluster-backed space; on a sub-cluster volume
   // several blocks share a cluster, so a new block may need only a real-size bump, not a new cluster.
   NtfsAttr ia;
   int haveIa = findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia) == 1 && !ia.resident;
   uint64_t needBlocks = b + 1;
   NtfsRunEntry runs[NTFS_MAX_FILE_RUNS];
   int runCount = 0;
   uint64_t haveClusters = 0, realBlocks = 0;
   if (haveIa) {
      if (decodeRuns(ia.attr + ia.runlistOffset, ia.attrLength - ia.runlistOffset, runs, NTFS_MAX_FILE_RUNS, &runCount) != 0)
         return ~0ull;
      haveClusters = ia.allocatedSize / vol->bytesPerCluster;
      realBlocks   = ia.realSize / vol->indexRecordSize;
   }
   uint64_t needClusters = ((uint64_t)needBlocks * vol->indexRecordSize + vol->bytesPerCluster - 1) / vol->bytesPerCluster;
   if (needClusters > haveClusters) {                        // grow the backing clusters, then republish runlist + sizes
      if (allocateClusters(vol, needClusters - haveClusters, runs, &runCount, NTFS_MAX_FILE_RUNS, 0) != 0) return ~0ull;
      if (putIndexAlloc(vol, runs, runCount, needBlocks) != 0) {
         freeClusterRuns(vol, runs, runCount);               // best-effort; bit left set is a leak chkdsk reclaims
         return ~0ull;
      }
   } else if (needBlocks > realBlocks) {                     // fits the existing clusters; just grow the real/valid size
      if (putIndexAlloc(vol, runs, runCount, needBlocks) != 0) return ~0ull;
   }
   return blockToVcn(vol, b);                              // VCN of the new block (dirRecord NOT yet written)
}

// Root capacity: the max node-relative usedSize the resident $INDEX_ROOT node may reach in dirRecord.
static uint32_t getRootCapacity(const NtfsVolume *vol, const NtfsAttr *root)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t used = readLe32(dirRecord + FILE_USED_SIZE);
   uint32_t attrLen = readLe32(root->attr + ATTR_LENGTH_OFFSET);
   uint32_t valueOff = (uint32_t)(root->value - root->attr);
   uint32_t other = used - attrLen;                          // bytes used by everything except $INDEX_ROOT
   if (recordSize <= other + valueOff + IDXROOT_NODE_HEADER + 8) return 0;
   return recordSize - other - valueOff - IDXROOT_NODE_HEADER - 8;   // 8 = end-marker slack
}

// Demotes the resident $INDEX_ROOT into a single child INDX block (height += 1). Order matters for
// both record space and crash safety: stash the root's entries, SHRINK the resident root to a tiny
// one-pointer internal node FIRST (freeing record space for the new $BITMAP/$INDEX_ALLOCATION
// attributes), allocate the block, write the child block to disk, then commit the dir record last
// (so the published root pointer always references an already-written block). Returns 0/-1.
static int demoteRoot(NtfsVolume *vol, uint64_t dirRef)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t blockSize  = vol->indexRecordSize;
   NtfsAttr root;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident) return -1;
   uint8_t *rootNd = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
   int rootLeaf = isNodeLeaf(rootNd);
   uint32_t rEntries = getNodeEntriesOffset(rootNd);
   uint32_t rootUsed = getNodeUsedSize(rootNd);
   uint32_t copyBytes = rootUsed - rEntries;                 // entries + marker of the root node
   if (copyBytes > blockSize - INDX_NODE_HEADER) return -1;  // root content must fit one block (always true)

   // 1. stash the root node's entries (incl. marker) before we shrink the root
   memCopy(indexNodeA, rootNd + rEntries, (int)copyBytes);

   // 2. shrink $INDEX_ROOT to a tiny internal node (one LAST|NODE marker, child VCN patched later) so
   //    the record has room for the $BITMAP / $INDEX_ALLOCATION attributes the alloc step adds.
   uint32_t rootAttrOff = (uint32_t)(root.attr - dirRecord);
   uint32_t valueOff = (uint32_t)(root.value - root.attr);
   uint32_t newNodeUsed = IDXROOT_NODE_HEADER + 24;
   uint32_t newValueLen = IDXROOT_NODE_HEADER + newNodeUsed;
   uint32_t newAttrLen = (uint32_t)ALIGN8(valueOff + newValueLen);
   uint32_t oldAttrLen = readLe32(dirRecord + rootAttrOff + ATTR_LENGTH_OFFSET);
   if (resizeAttribute(dirRecord, recordSize, rootAttrOff, oldAttrLen, newAttrLen) != 0) return -1;
   uint8_t *ra = dirRecord + rootAttrOff;
   writeLe32(ra + ATTR_RES_VALUE_LENGTH, newValueLen);
   rootNd = ra + valueOff + IDXROOT_NODE_HEADER;
   writeLe32(rootNd + IDXNODE_ENTRIES_OFFSET, IDXROOT_NODE_HEADER);
   writeLe32(rootNd + IDXNODE_USED_SIZE, newNodeUsed);
   writeLe32(rootNd + IDXNODE_ALLOC_SIZE, newNodeUsed);
   writeLe32(rootNd + IDXNODE_FLAGS_OFFSET, 1);              // internal

   // 3. allocate the child block (edits dirRecord in memory: $BITMAP bit + $INDEX_ALLOCATION)
   uint64_t vcn = allocIndexBlock(vol, dirRef);
   if (vcn == ~0ull) return -1;
   NtfsAttr ia;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia) != 1) return -1;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1) return -1;   // re-find after resize
   rootNd = (uint8_t *)root.value + IDXROOT_NODE_HEADER;

   // 4. build the child block from the stash and write it to disk BEFORE publishing the root pointer
   buildEmptyIndxBlock(indexBuffer, blockSize, vol->bytesPerSector, vcn, rootLeaf);
   uint8_t *cnd = indexBuffer + INDX_NODE_HEADER;
   uint32_t cEntries = getNodeEntriesOffset(cnd);
   if (cEntries + copyBytes > blockSize - INDX_NODE_HEADER) return -1;
   memCopy(cnd + cEntries, indexNodeA, (int)copyBytes);
   writeLe32(cnd + IDXNODE_USED_SIZE, cEntries + copyBytes);
   writeLe32(cnd + IDXNODE_FLAGS_OFFSET, rootLeaf ? 0 : 1);
   if (writeIndxBlock(vol, &ia, vcn, indexBuffer) != 0) return -1;

   // 5. point the root marker at the now-written child block and commit the dir record (the commit)
   uint8_t *m = rootNd + IDXROOT_NODE_HEADER;
   memSet(m, 0, 24);
   writeLe16(m + IDXENTRY_LENGTH, 24);
   writeLe16(m + IDXENTRY_FLAGS, IDXENTRY_FLAG_LAST | IDXENTRY_FLAG_NODE);
   writeLe64(m + 24 - 8, vcn);
   return writeMftRecord(vol, dirRef, dirRecord);
}

// Inserts (fileRef,key) into a large directory index (root has, or is being given, children): descend
// to the target leaf, splitting any full node top-down so the parent always has room for a promoted
// median. dirRecord must hold the directory FILE record. Returns 0, -2 if the name exists, -1 on
// error / refusal. Used by insertIndexEntry when the small resident path can't take the entry.

// Inserts an entry (optionally carrying a child VCN) into the *resident* $INDEX_ROOT node in
// dirRecord, GROWING the attribute first (resizeAttribute shifts the following attributes right) so
// the write never spills into $INDEX_ALLOCATION/$BITMAP. Unlike an INDX block (whose node owns a full
// fixed-size block), the resident root's node is bounded by its attribute allocation, so it must be
// resized to grow. Caller must re-find any attribute that follows $INDEX_ROOT afterwards. Returns
// 0, -2 (duplicate), or -1.
static int insertRootEntry(NtfsVolume *vol, uint64_t fileRef, const uint8_t *key, int keyLen,
                             int hasChild, uint64_t childVcn)
{
   uint32_t recordSize = vol->mftRecordSize;
   NtfsAttr root;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident) return -1;
   uint32_t attrOff  = (uint32_t)(root.attr - dirRecord);
   uint32_t valueOff = (uint32_t)(root.value - root.attr);
   uint32_t entryLen = (uint32_t)ALIGN8(IDXENTRY_KEY + (uint32_t)keyLen + (hasChild ? 8u : 0u));
   uint8_t *nd = dirRecord + attrOff + valueOff + IDXROOT_NODE_HEADER;
   // Size the $INDEX_ROOT attribute to the node's ACTUAL post-insert used size, growing or shrinking as
   // needed. NOT a blind +entryLen: after a separator removal (W7 case B) the node carries reusable
   // slack, so an unconditional grow would balloon the attribute on every root delete until the MFT
   // record overflowed. Tracking usedSize keeps allocSize == usedSize (the resident-root convention).
   uint32_t finalUsed  = getNodeUsedSize(nd) + entryLen;                       // node bytes (incl. node header) after insert
   uint32_t newValLen  = IDXROOT_NODE_HEADER + finalUsed;             // value = 16B index-root header + node
   uint32_t oldAttrLen = readLe32(dirRecord + attrOff + ATTR_LENGTH_OFFSET);
   uint32_t newAttrLen = (uint32_t)ALIGN8((uint64_t)valueOff + newValLen);
   if (newAttrLen != oldAttrLen &&
       resizeAttribute(dirRecord, recordSize, attrOff, oldAttrLen, newAttrLen) != 0) return -1;
   nd = dirRecord + attrOff + valueOff + IDXROOT_NODE_HEADER;         // re-derive after a possible resize
   writeLe32(dirRecord + attrOff + ATTR_RES_VALUE_LENGTH, newAttrLen - valueOff);
   writeLe32(nd + IDXNODE_ALLOC_SIZE, finalUsed);
   return insertNodeEntry(nd, finalUsed, fileRef, key, keyLen, hasChild, childVcn);
}

static int insertIntoLargeIndex(NtfsVolume *vol, uint64_t dirRef, uint64_t fileRef, const uint8_t *key, int keyLen)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t blockSize  = vol->indexRecordSize;
   if (blockSize > NTFS_MAX_RECORD) return -1;   // W6c: sub-cluster blocks (block < cluster) now supported
   uint32_t indxCap = blockSize - INDX_NODE_HEADER;
   // Reserve exactly this entry's worst-case size (an internal copy carries an 8-byte child VCN) when
   // deciding whether a node is "full" enough to split/demote. Using the real entry size (not the
   // 255-char worst case) is essential: the resident root shares the FILE record with other
   // attributes, so its capacity can be well under the absolute max entry size.
   uint32_t reserve = (uint32_t)ALIGN8(IDXENTRY_KEY + (uint32_t)keyLen + 8);

   // load the directory FILE record into the dedicated buffer (the caller used mftRecord; bitmap /
   // cluster ops below clobber mftRecord, so the dir record needs its own stable buffer)
   if (readMftRecord(vol, dirRef, dirRecord) != 0) return -1;   // F048 ok: dirRef already validated by insertNameKey/removeNameKey before this delegated index I/O (same record, same lock)

   // ensure the resident root is an internal node with room for a promoted median (demote if needed)
   NtfsAttr root;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident) return -1;
   uint8_t *rootNd = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
   // SEC corruption guard: the node must fit its PHYSICAL container (the $INDEX_ROOT value minus its
   // 16-byte header), not getRootCapacity. getRootCapacity subtracts an insertion-slack reserve that a validly
   // packed-full leaf legitimately reaches just before the demote below frees the space - bounding the
   // guard by it wrongly refuses a full directory (e.g. a long-named dir whose DOS twin fills the record).
   uint32_t rootContainer = (root.valueLength >= IDXROOT_NODE_HEADER) ? root.valueLength - IDXROOT_NODE_HEADER : 0;
   if (getNodeUsedSize(rootNd) > rootContainer) return -1;
   if (isNodeLeaf(rootNd) || getNodeUsedSize(rootNd) + reserve > getRootCapacity(vol, &root)) {
      if (demoteRoot(vol, dirRef) != 0) return -1;
      if (readMftRecord(vol, dirRef, dirRecord) != 0) return -1;   // F048 ok: dirRef already validated by insertNameKey/removeNameKey before this delegated index I/O (same record, same lock)
      if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1) return -1;
      rootNd = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
   }
   NtfsAttr ia;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia) != 1 || ia.resident) return -1;

   // descend from the root, splitting a full child before entering it (top-down preemptive split)
   int curIsRoot = 1;
   uint64_t curVcn = 0;
   for (int depth = 0; depth < NTFS_MAX_INDEX_DEPTH; depth++) {
      uint8_t *curBuf, *curNd;
      uint32_t curCap;
      if (curIsRoot) { curNd = rootNd; curCap = getRootCapacity(vol, &root); curBuf = 0; }
      else {
         if (readIndxBlock(vol, &ia, curVcn, indexNodeA) != 0) return -1;
         curBuf = indexNodeA; curNd = indexNodeA + INDX_NODE_HEADER; curCap = indxCap;
      }

      if (isNodeLeaf(curNd)) {                                 // leaf: insert here (room guaranteed top-down)
         int rc = insertNodeEntry(curNd, curCap, fileRef, key, keyLen, 0, 0);
         if (rc != 0) return rc;
         if (curIsRoot) return writeMftRecord(vol, dirRef, dirRecord);
         return writeIndxBlock(vol, &ia, curVcn, curBuf);
      }

      // internal: pick the child to descend into, and detect an exact duplicate
      uint32_t used = getNodeUsedSize(curNd), pos = getNodeEntriesOffset(curNd);
      uint64_t childVcn = 0; int found = 0;
      for (int g = 0; g < 8192; g++) {
         int ev = validateIndexEntry(curNd, pos, used);
         if (ev < 0) return -1;
         uint8_t *e = curNd + pos;
         if (ev == 0) { childVcn = readLe64(e + readLe16(e + IDXENTRY_LENGTH) - 8); found = 1; break; }
         int cmp = compareFileNameKeys(key, e + IDXENTRY_KEY);
         if (cmp == 0) return -2;                            // already exists
         if (cmp < 0) { childVcn = readLe64(e + readLe16(e + IDXENTRY_LENGTH) - 8); found = 1; break; }
         pos += (uint32_t)ev;
      }
      if (!found) return -1;

      // read the child; if it is full, split it and promote the median into cur (which has room)
      if (readIndxBlock(vol, &ia, childVcn, indexNodeB) != 0) return -1;
      uint8_t *childNd = indexNodeB + INDX_NODE_HEADER;
      if (getNodeUsedSize(childNd) + reserve > indxCap) {
         uint64_t rightVcn = allocIndexBlock(vol, dirRef);   // in-memory dirRecord edits ($BITMAP + $IA)
         if (rightVcn == ~0ull) return -1;
         // re-find root + $IA (alloc's resize shifted attribute offsets); cur/child buffers are separate
         if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1) return -1;
         rootNd = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
         if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia) != 1) return -1;
         if (curIsRoot) { curNd = rootNd; curCap = getRootCapacity(vol, &root); }

         buildEmptyIndxBlock(indexBuffer, blockSize, vol->bytesPerSector, rightVcn, isNodeLeaf(childNd));
         uint8_t medKey[600]; int medKeyLen; uint64_t medRef, medChild;
         if (splitNode(childNd, indexBuffer, indxCap, medKey, &medKeyLen, &medRef, &medChild) != 0) return -1;
         if (writeIndxBlock(vol, &ia, rightVcn, indexBuffer) != 0) return -1;   // right (new) block
         if (writeIndxBlock(vol, &ia, childVcn, indexNodeB) != 0) return -1;    // left (shrunk child)
         if (replaceChildVcn(curNd, childVcn, rightVcn) != 0) return -1;        // old pointer -> right
         // promote the median into cur. The resident root must be GROWN (resizeAttribute) so the new
         // entry doesn't overwrite the following $INDEX_ALLOCATION/$BITMAP; an INDX block has room.
         int mi = curIsRoot ? insertRootEntry(vol, medRef, medKey, medKeyLen, 1, childVcn)
                            : insertNodeEntry(curNd, curCap, medRef, medKey, medKeyLen, 1, childVcn);
         if (mi != 0) return -1;
         if (!curIsRoot && writeIndxBlock(vol, &ia, curVcn, indexNodeA) != 0) return -1;  // commit cur INDX node
         if (writeMftRecord(vol, dirRef, dirRecord) != 0) return -1;              // commit $BITMAP + $IA (+root)
         // A root insert (insertRootEntry) ran resizeAttribute, which shifts $INDEX_ALLOCATION within
         // the record: re-find ia so the continued descent reads blocks through the current runlist.
         if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia) != 1 || ia.resident) return -1;
         int medCmp = compareFileNameKeys(key, medKey);
         if (medCmp == 0) return -2;                          // key equals the just-promoted median -> duplicate
         childVcn = (medCmp < 0) ? childVcn : rightVcn;       // descend into the chosen half
      }
      curIsRoot = 0; curVcn = childVcn;                      // descend
   }
   return -1;                                                // depth guard: refuse rather than loop
}

// Inserts a $FILE_NAME index entry into a directory record's $INDEX_ROOT, in collation order.
// Returns 0, -2 if the name already exists, or -1 if it won't fit the record (a B-tree node split
// would be needed: refused). Operates on the parent record already in `record`.
static int insertIndexEntry(uint8_t *record, uint32_t recordSize, uint64_t fileRef, const uint8_t *key, int keyLen)
{
   NtfsAttr root;
   if (findAttribute(record, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident) return -1;
   if (root.valueLength < IDXROOT_NODE_HEADER + 16) return -1;   // value must hold the $INDEX_ROOT + node headers
   uint32_t attrOffset = (uint32_t)(root.attr - record);
   uint32_t valueOffset = (uint32_t)(root.value - root.attr);
   uint8_t *node = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
   uint32_t nodeMax = root.valueLength - IDXROOT_NODE_HEADER;    // bytes the resident value gives the node

   // A "large" index (already spilled into $INDEX_ALLOCATION / the root node has children) is handled
   // by the W6 B-tree path: return -3 so the caller delegates to insertIntoLargeIndex.
   NtfsAttr allocation;
   if (findAttribute(record, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &allocation) == 1) return -3;
   if (readLe32(node + IDXNODE_FLAGS_OFFSET) & 1) return -3;

   uint32_t entriesOffset = readLe32(node + IDXNODE_ENTRIES_OFFSET);
   uint32_t usedSize = readLe32(node + IDXNODE_USED_SIZE);
   if (usedSize > nodeMax || entriesOffset < IDXNODE_FLAGS_OFFSET + 4 || entriesOffset > usedSize) return -1;

   // find the collation-ordered insertion point (and reject a duplicate name); a valid node always
   // ends with a last-entry marker, so if we never reach one the node is malformed -> refuse.
   uint32_t pos = entriesOffset;
   int located = 0;
   for (int guard = 0; guard < 4096; guard++) {
      int ev = validateIndexEntry(node, pos, usedSize);
      if (ev < 0) return -1;                                // malformed entry -> refuse, don't corrupt
      if (ev == 0) { located = 1; break; }                 // last marker: insert before it
      int cmp = compareFileNameKeys(key, node + pos + IDXENTRY_KEY);
      if (cmp == 0) return -2;                              // already exists
      if (cmp < 0) { located = 1; break; }                 // insert before this entry
      pos += (uint32_t)ev;
   }
   if (!located) return -1;

   uint32_t entryLength = ALIGN8(IDXENTRY_KEY + (uint32_t)keyLen);
   uint32_t oldAttrLen = readLe32(record + attrOffset + ATTR_LENGTH_OFFSET);
   if (resizeAttribute(record, recordSize, attrOffset, oldAttrLen, oldAttrLen + entryLength) != 0)
      return -3;   // won't fit the resident root: promote to $INDEX_ALLOCATION (W6 large-index path)

   // open a gap at the insertion point and write the new entry
   uint8_t *insertAt = node + pos;
   uint32_t insertOffset = (uint32_t)(insertAt - record);
   memMove(record + insertOffset + entryLength, record + insertOffset, (int)((attrOffset + oldAttrLen) - insertOffset));
   writeLe64(insertAt + IDXENTRY_FILE_REF, fileRef);
   writeLe16(insertAt + IDXENTRY_LENGTH, (uint16_t)entryLength);
   writeLe16(insertAt + IDXENTRY_KEY_LENGTH, (uint16_t)keyLen);
   writeLe16(insertAt + IDXENTRY_FLAGS, 0);
   writeLe16(insertAt + 14, 0);
   memCopy(insertAt + IDXENTRY_KEY, key, keyLen);

   // grow the node + resident value to match
   writeLe32(node + IDXNODE_USED_SIZE, usedSize + entryLength);
   writeLe32(node + IDXNODE_ALLOC_SIZE, (oldAttrLen + entryLength) - valueOffset - IDXROOT_NODE_HEADER);
   writeLe32(record + attrOffset + ATTR_RES_VALUE_LENGTH, (oldAttrLen + entryLength) - valueOffset);
   return 0;
}

// Forward decls (defined in the W4/W7 delete sections below) for the W11 name-key helpers.
static int removeIndexEntry(uint8_t *record, uint32_t recordSize, const uint8_t *key);
static int removeFromLargeIndex(NtfsVolume *vol, uint64_t dirRef, const uint8_t *key);

// Returns the node-relative offset of the keyed entry collation-equal to `matchKey`, -1 if absent,
// -2 if the node is malformed. `used` must already be bounded against the node container.
static int findNodeKey(const uint8_t *node, uint32_t used, const uint8_t *matchKey)
{
   uint32_t pos = readLe32(node + IDXNODE_ENTRIES_OFFSET);
   for (int g = 0; g < 8192; g++) {
      int ev = validateIndexEntry(node, pos, used);
      if (ev < 0) return -2;
      if (ev == 0) return -1;                                 // last marker: not found
      if (compareFileNameKeys(matchKey, node + pos + IDXENTRY_KEY) == 0) return (int)pos;
      pos += (uint32_t)ev;
   }
   return -2;
}

// Writes the allocated/real data size into the parent $I30 index entry whose key collates equal to
// `matchKey`. NTFS keeps a size copy in every $FILE_NAME, and listings/stat read the index copy, so it
// must track $DATA. Searches the resident root, then every INDX block. Returns 0 patched, -1 on error /
// not found. Clobbers mftRecord + indexBuffer.
static int setIndexEntrySize(NtfsVolume *vol, uint64_t parentRef, const uint8_t *matchKey,
                             uint64_t allocSize, uint64_t realSize, uint64_t modifiedTime)
{
   uint32_t recordSize = vol->mftRecordSize;
   if (readMftRecordByRef(vol, parentRef, mftRecord) != 0) return -1;
   NtfsAttr root;
   if (findAttribute(mftRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident) return -1;
   if (root.valueLength < IDXROOT_NODE_HEADER + 16) return -1;

   // resident root
   uint8_t *rnode = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
   uint32_t rmax  = root.valueLength - IDXROOT_NODE_HEADER;
   uint32_t rused = readLe32(rnode + IDXNODE_USED_SIZE);
   if (rused > rmax) rused = rmax;
   int pos = findNodeKey(rnode, rused, matchKey);
   if (pos == -2) return -1;
   if (pos >= 0) {
      uint8_t *key = rnode + (uint32_t)pos + IDXENTRY_KEY;
      writeLe64(key + FN_ALLOC_SIZE, allocSize);
      writeLe64(key + FN_REAL_SIZE, realSize);
      writeLe64(key + FN_MODIFIED_TIME, modifiedTime);
      return writeMftRecord(vol, parentRef & MFT_REF_MASK, mftRecord);
   }

   // large index: scan every INDX block (the name lives in exactly one node)
   NtfsAttr alloc;
   if (findAttribute(mftRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &alloc) != 1 || alloc.resident) return -1;
   uint32_t blockSize = vol->indexRecordSize;
   uint64_t totalBlocks = indexBlockCount(vol, &alloc, blockSize);
   for (uint64_t block = 0; block < totalBlocks; block++) {
      uint64_t vcn = blockToVcn(vol, block);
      if (readIndxBlock(vol, &alloc, vcn, indexBuffer) != 0) continue;   // not an in-use INDX block
      uint8_t *node = indexBuffer + INDX_NODE_HEADER;
      uint32_t used = readLe32(node + IDXNODE_USED_SIZE);
      uint32_t bmax = blockSize - INDX_NODE_HEADER;
      if (used > bmax) used = bmax;
      int p = findNodeKey(node, used, matchKey);
      if (p == -2) return -1;
      if (p >= 0) {
         uint8_t *key = node + (uint32_t)p + IDXENTRY_KEY;
         writeLe64(key + FN_ALLOC_SIZE, allocSize);
         writeLe64(key + FN_REAL_SIZE, realSize);
         writeLe64(key + FN_MODIFIED_TIME, modifiedTime);
         return writeIndxBlock(vol, &alloc, vcn, indexBuffer);
      }
   }
   return -1;                                                 // entry not found: surface rather than hide
}

// Propagates the file's current $DATA size into every $FILE_NAME copy: the file's own record AND the
// matching parent $I30 index entries. Without this a written file reports 0 bytes (listings read the
// index copy) and chkdsk rejects the stale link. No-op for system records. Returns 0/-1. Clobbers
// mftRecord + indexBuffer + dirRecord.
static int syncFileNameSizes(NtfsVolume *vol, uint64_t fileRef)
{
   uint64_t number = fileRef & MFT_REF_MASK;
   if (number < NTFS_FIRST_USER_RECORD) return 0;             // never touch system files
   uint32_t recordSize = vol->mftRecordSize;
   uint64_t modifiedTime = nowFiletime();                     // C5: stamp the data-modification time of this write/truncate

   // current $DATA sizes (the stream may have spilled to an extension record)
   NtfsAttr data;
   uint64_t housingRef;
   if (findDataAnywhere(vol, number, mftRecord, &data, &housingRef) != 0) return -1;
   uint64_t realSize  = data.resident ? data.valueLength : data.realSize;
   uint64_t allocSize = data.resident ? 0 : data.allocatedSize;   // resident data occupies no clusters

   // update each $FILE_NAME's own size copy in place, collecting the keys + parent refs so the parent
   // index entries can be patched after the record buffers are reused below.
   if (readMftRecordByRef(vol, fileRef, dirRecord) != 0) return -1;

   // C5: bump $STANDARD_INFORMATION last-modified + MFT-changed times (the canonical timestamps)
   NtfsAttr si;
   if (findAttribute(dirRecord, recordSize, ATTR_STANDARD_INFORMATION, 0, 0, &si) == 1 && si.resident && si.valueLength >= 0x18) {
      uint8_t *siValue = (uint8_t *)si.value;
      writeLe64(siValue + 0x08, modifiedTime);               // last data modification
      writeLe64(siValue + 0x10, modifiedTime);               // MFT record change
   }

   uint32_t used  = readLe32(dirRecord + FILE_USED_SIZE);
   uint32_t limit = used <= recordSize ? used : recordSize;
   uint8_t  keys[2][FN_NAME + 255 * 2];
   uint64_t parents[2];
   int nkeys = 0;
   uint32_t off = readLe16(dirRecord + FILE_FIRST_ATTR_OFFSET);
   for (int g = 0; g < 256; g++) {
      if (off + 4 > limit) break;
      uint32_t type = readLe32(dirRecord + off + ATTR_TYPE_OFFSET);
      if (type == ATTR_END) break;
      if (off + 16 > limit) break;
      uint32_t attrLength = readLe32(dirRecord + off + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - off) break;
      if (type == ATTR_FILE_NAME && dirRecord[off + ATTR_NON_RESIDENT] == 0) {
         uint32_t valueOffset = readLe16(dirRecord + off + ATTR_RES_VALUE_OFFSET);
         uint32_t valueLength = readLe32(dirRecord + off + ATTR_RES_VALUE_LENGTH);
         if (valueOffset + FN_MIN_SIZE <= attrLength && valueLength >= FN_MIN_SIZE && valueLength <= sizeof(keys[0])) {
            uint8_t *fn = dirRecord + off + valueOffset;
            writeLe64(fn + FN_ALLOC_SIZE, allocSize);
            writeLe64(fn + FN_REAL_SIZE, realSize);
            writeLe64(fn + FN_MODIFIED_TIME, modifiedTime);   // C5: keep the $FILE_NAME copy in step with $SI
            if (nkeys < 2) {
               memCopy(keys[nkeys], fn, (int)valueLength);
               parents[nkeys] = readLe64(fn + FN_PARENT_REF);
               nkeys++;
            }
         }
      }
      off += attrLength;
   }
   if (writeMftRecord(vol, number, dirRecord) != 0) return -1;

   for (int i = 0; i < nkeys; i++)
      if (setIndexEntrySize(vol, parents[i], keys[i], allocSize, realSize, modifiedTime) != 0) return -1;
   return 0;
}

// W11: the inherited $STANDARD_INFORMATION security_id from a parent directory (3.x volumes). Returns
// 0 if the parent has only the 48-byte legacy $STANDARD_INFORMATION (an NTFS-1.2 volume) or on error.
// Clobbers mftRecord.
static uint32_t parentSecurityId(NtfsVolume *vol, uint64_t parentRef)
{
   if (readMftRecordByRef(vol, parentRef, mftRecord) != 0) return 0;   // F048: parentRef is on-disk-derived; validate sequence
   NtfsAttr si;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_STANDARD_INFORMATION, 0, 0, &si) != 1 || !si.resident) return 0;
   if (si.valueLength < 0x38) return 0;                      // 48-byte legacy form: no security_id
   return readLe32(si.value + 0x34);
}

// Copies the parent's inline $SECURITY_DESCRIPTOR value into out[cap]; returns its length, or 0 if the
// parent has none (it uses a $Secure security_id instead) or it won't fit. Used so a new object inherits
// the parent's ACL on volumes whose directories carry inline descriptors. Clobbers mftRecord.
static uint32_t getParentSecurityDescriptor(NtfsVolume *vol, uint64_t parentRef, uint8_t *out, uint32_t cap)
{
   if (readMftRecordByRef(vol, parentRef, mftRecord) != 0) return 0;
   NtfsAttr sd;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_SECURITY_DESCRIPTOR, 0, 0, &sd) != 1 || !sd.resident) return 0;
   if (sd.valueLength == 0 || sd.valueLength > cap) return 0;
   memCopy(out, sd.value, (int)sd.valueLength);
   return sd.valueLength;
}

// W11: insert one $FILE_NAME key into a parent index (small resident root or large B-tree). 0/-2/-1.
static int insertNameKey(NtfsVolume *vol, uint64_t parentRef, uint64_t fileRef, const uint8_t *key, int keyLen)
{
   uint64_t num = parentRef & MFT_REF_MASK;
   if (readMftRecordByRef(vol, parentRef, mftRecord) != 0) return -1;   // F048: validate the on-disk parent ref's sequence
   int r = insertIndexEntry(mftRecord, vol->mftRecordSize, fileRef, key, keyLen);
   if (r == -3) return insertIntoLargeIndex(vol, num, fileRef, key, keyLen);   // delegate by bare number (parent already validated here)
   if (r != 0) return r;
   return writeMftRecord(vol, num, mftRecord);
}

// W11: remove one $FILE_NAME key from a parent index (small or large). 0/-1 (idempotent on absence
// only via the caller). Used for twin upkeep and rollback.
static int removeNameKey(NtfsVolume *vol, uint64_t parentRef, const uint8_t *key)
{
   uint64_t num = parentRef & MFT_REF_MASK;
   if (readMftRecordByRef(vol, parentRef, mftRecord) != 0) return -1;   // F048: validate the on-disk parent ref's sequence
   int r = removeIndexEntry(mftRecord, vol->mftRecordSize, key);
   if (r == -3) return removeFromLargeIndex(vol, num, key);   // delegate by bare number (parent already validated here)
   if (r != 0) return r;
   return writeMftRecord(vol, num, mftRecord);
}

// Returns `recordNumber`'s full file reference (record number + real on-disk sequence). A child's
// $FILE_NAME parent back-reference must carry the parent's real sequence, but resolvePath hands back the
// root with sequence 0 (the seq-0 read convention); storing that 0 makes chkdsk reject the link and
// orphan the file. For non-root parents the ref already carries the sequence, so this is a no-op-equivalent
// readback. Uses the shared mftRecord buffer as scratch (callers re-read it afterwards). ~0ull on error.
static uint64_t getRecordFullRef(NtfsVolume *vol, uint64_t recordNumber)
{
   uint64_t number = recordNumber & MFT_REF_MASK;
   if (readMftRecord(vol, number, mftRecord) != 0) return ~0ull;
   return number | ((uint64_t)readLe16(mftRecord + FILE_SEQUENCE_NUMBER) << 48);
}

// Creates an empty file (isDir=0) or directory (isDir=1) at an in-volume path. Returns 0, -2 if it
// already exists, or -1 on error / refusal (parent missing, no free MFT record, index full). The name
// is written as a single POSIX $FILE_NAME with no DOS 8.3 twin, matching what ntfs-3g and modern Windows
// (8.3 creation disabled) write for new files.
static int freeMftRecord(NtfsVolume *vol, uint64_t reference);   // clean rollback of an already-written record

static int createPath(NtfsVolume *vol, const char *path, int isDir)
{
   if (!vol->writable) return -1;

   // split into parent directory + leaf name
   int lastSlash = -1;
   for (int i = 0; path[i]; i++) if (path[i] == '/') lastSlash = i;
   if (lastSlash < 0) return -1;
   const char *leaf = path + lastSlash + 1;
   if (!*leaf) return -1;
   char parentPath[MAX_PATH_LEN];
   int parentLen = lastSlash == 0 ? 1 : lastSlash;          // "/" stays "/"
   if (parentLen >= (int)sizeof(parentPath)) return -1;
   memCopy(parentPath, path, parentLen); parentPath[parentLen] = '\0';

   NtfsInfo parent, existing;
   if (resolvePath(vol, parentPath, &parent) != 0 || !parent.isDir) return -1;
   if (resolvePath(vol, path, &existing) == 0) return -2;   // already exists

   // leaf -> UTF-16
   uint16_t units[257];
   utf8ToUtf16(leaf, units, 256);
   int nlen = 0; while (nlen < 255 && units[nlen]) nlen++;
   if (nlen == 0 || units[nlen]) return -1;

   // the parent back-reference needs the parent's real sequence (see getRecordFullRef)
   uint64_t parentFullRef = getRecordFullRef(vol, parent.mftReference);
   if (parentFullRef == ~0ull) return -1;
   uint64_t ft = nowFiletime();
   uint8_t key[600];
   int keyLen = buildFileNameKeyU(key, parentFullRef, units, nlen, isDir, FN_NAMESPACE_POSIX, ft);
   if (keyLen < 0) return -1;

   // Past here we may mutate the volume, so arm the dirty flag. `armedDirty` lets a *refused* op (which
   // rolls everything back, leaving the volume unchanged) clear it again at `done:` rather than forcing
   // a needless chkdsk on the next mount.
   int rc = -1, armedDirty = 0;
   if (!vol->volumeDirty) { if (setVolumeDirty(vol, 1) != 0) return -1; vol->volumeDirty = 1; armedDirty = 1; }

   uint32_t secId = parentSecurityId(vol, parentFullRef);   // F048: pass full ref so the parent's sequence is validated
   // When the parent has no $Secure id, inherit its inline $SECURITY_DESCRIPTOR instead (else the new
   // object would carry no security at all, which Windows treats as access-denied).
   uint8_t secDesc[512]; uint32_t secDescLen = 0;
   if (secId == 0) secDescLen = getParentSecurityDescriptor(vol, parentFullRef, secDesc, sizeof secDesc);

   uint64_t number = findFreeMftRecord(vol);
   if (number == ~0ull) {                                   // W8b: MFT full -> grow it, then retry
      if (growMft(vol, 8) != 0) goto done;
      number = findFreeMftRecord(vol);
   }
   if (number == ~0ull || setMftRecordBit(vol, number, 1) != 0) goto done;

   // C6: reuse the record's existing on-disk sequence (freeMftRecord bumped it on the last free; a
   // never-used grown record reads back 0). Resetting to 1 would let a new record collide with a stale
   // reference from an earlier generation and pass the sequence check. Read raw: a freed record is not
   // in use so readMftRecord would reject it. 0 -> 1 (sequence is never 0).
   uint16_t sequence = 1;
   if (readMftRecordBytes(vol, number, mftRecord) == 0 &&
       mftRecord[0] == 'F' && mftRecord[1] == 'I' && mftRecord[2] == 'L' && mftRecord[3] == 'E') {
      uint16_t onDisk = readLe16(mftRecord + FILE_SEQUENCE_NUMBER);
      if (onDisk) sequence = onDisk;
   }

   // build and write the new record (single POSIX $FILE_NAME)
   buildNewRecord(mftRecord, vol->mftRecordSize, vol->bytesPerSector, vol->indexRecordSize, number, sequence, isDir,
                  key, keyLen, 0, 0, secId, secDesc, secDescLen);
   if (writeMftRecord(vol, number, mftRecord) != 0) { setMftRecordBit(vol, number, 0); goto done; }

   // link the name into the parent index with the matching sequence
   {
      uint64_t fileRef = number | ((uint64_t)sequence << 48);
      int r1 = insertNameKey(vol, parentFullRef, fileRef, key, keyLen);
      // the record is already on disk and in-use; a bare bitmap clear would leave it referenced by the
      // bitmap as free yet readable as in-use (chkdsk "corrupt MFT record"). freeMftRecord undoes both.
      if (r1 != 0) { freeMftRecord(vol, fileRef); rc = (r1 == -2) ? -2 : -1; goto done; }
   }
   rc = 0;
done:
   if (rc != 0 && armedDirty) { setVolumeDirty(vol, 0); vol->volumeDirty = 0; }   // refused op: undo the dirty arming
   return rc;
}

int createNtfsPath(NtfsVolume *vol, const char *path) { return createPath(vol, path, 0); }
int mkdirNtfsPath(NtfsVolume *vol, const char *path)  { return createPath(vol, path, 1); }

// ===========================================================================
// Write path W4: delete (unlink file / rmdir empty directory). Removes the
// $FILE_NAME entry from the parent's $I30 (inverse of insertIndexEntry), then
// frees the data clusters and the MFT record. Order: unreference first, free
// after, so a crash leaks rather than dangles. Refuses large-index parents.
// ===========================================================================

// Removes the $FILE_NAME entry whose name matches `key` from a directory record's $INDEX_ROOT.
// Returns 0, or -1 if not found / the index is large (B-tree removal deferred) / malformed.
static int removeIndexEntry(uint8_t *record, uint32_t recordSize, const uint8_t *key)
{
   NtfsAttr root;
   if (findAttribute(record, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident) return -1;
   if (root.valueLength < IDXROOT_NODE_HEADER + 16) return -1;   // value must hold the $INDEX_ROOT + node headers
   uint32_t attrOffset = (uint32_t)(root.attr - record);
   uint32_t valueOffset = (uint32_t)(root.value - root.attr);
   uint8_t *node = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
   uint32_t nodeMax = root.valueLength - IDXROOT_NODE_HEADER;    // bytes the resident value gives the node
   // A large index (spilled into $INDEX_ALLOCATION / root has children) is handled by the W7 B-tree
   // delete path: return -3 so the caller delegates to removeFromLargeIndex.
   NtfsAttr allocation;
   if (findAttribute(record, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &allocation) == 1) return -3;
   if (readLe32(node + IDXNODE_FLAGS_OFFSET) & 1) return -3;
   uint32_t usedSize = readLe32(node + IDXNODE_USED_SIZE);
   uint32_t entriesOffset = readLe32(node + IDXNODE_ENTRIES_OFFSET);
   if (usedSize > nodeMax || entriesOffset < IDXNODE_FLAGS_OFFSET + 4 || entriesOffset > usedSize) return -1;

   uint32_t pos = entriesOffset;
   uint32_t entryLength = 0;
   int found = 0;
   for (int guard = 0; guard < 4096; guard++) {
      int ev = validateIndexEntry(node, pos, usedSize);
      if (ev < 0) return -1;                                // malformed entry -> refuse, don't corrupt
      if (ev == 0) break;                                   // last marker reached: not found
      if (compareFileNameKeys(key, node + pos + IDXENTRY_KEY) == 0) { entryLength = (uint32_t)ev; found = 1; break; }
      pos += (uint32_t)ev;
   }
   if (!found) return -1;

   // compact the entries within the node, then shrink the attribute / record. The validated bounds
   // above guarantee removeOffset + entryLength <= attrOffset + oldAttrLen, so the length is never
   // negative (no unsigned underflow into a giant memMove); re-checked explicitly for safety.
   uint32_t removeOffset = (uint32_t)((node + pos) - record);
   uint32_t oldAttrLen = readLe32(record + attrOffset + ATTR_LENGTH_OFFSET);
   if (removeOffset + entryLength > attrOffset + oldAttrLen) return -1;
   if (oldAttrLen < entryLength + valueOffset + IDXROOT_NODE_HEADER) return -1;
   uint32_t tailWithinAttr = (attrOffset + oldAttrLen) - (removeOffset + entryLength);
   memMove(record + removeOffset, record + removeOffset + entryLength, (int)tailWithinAttr);
   if (resizeAttribute(record, recordSize, attrOffset, oldAttrLen, oldAttrLen - entryLength) != 0) return -1;

   writeLe32(node + IDXNODE_USED_SIZE, usedSize - entryLength);
   writeLe32(node + IDXNODE_ALLOC_SIZE, (oldAttrLen - entryLength) - valueOffset - IDXROOT_NODE_HEADER);
   writeLe32(record + attrOffset + ATTR_RES_VALUE_LENGTH, (oldAttrLen - entryLength) - valueOffset);
   return 0;
}

// W7: descends to the leftmost leaf of the subtree rooted at INDX block `startVcn`, leaving it in
// `buf` and its VCN in *outVcn. The leftmost leaf's first entry is the in-order minimum of the
// subtree (the successor used by an internal-node delete). Returns 0/-1.
static int findLeftmostLeaf(NtfsVolume *vol, const NtfsAttr *ia, uint64_t startVcn, uint8_t *buf, uint64_t *outVcn)
{
   uint64_t vcn = startVcn;
   for (int d = 0; d < NTFS_MAX_INDEX_DEPTH; d++) {
      if (readIndxBlock(vol, ia, vcn, buf) != 0) return -1;
      uint8_t *nd = buf + INDX_NODE_HEADER;
      if (isNodeLeaf(nd)) { *outVcn = vcn; return 0; }
      uint8_t *e = nd + getNodeEntriesOffset(nd);                       // first entry (or marker) of an internal node
      if (validateIndexEntry(nd, getNodeEntriesOffset(nd), getNodeUsedSize(nd)) < 0) return -1;
      if (!(readLe16(e + IDXENTRY_FLAGS) & IDXENTRY_FLAG_NODE)) return -1;   // internal entries carry a child
      vcn = readLe64(e + readLe16(e + IDXENTRY_LENGTH) - 8); // leftmost child = keys below the first key
   }
   return -1;
}

// W7: descends to the rightmost leaf of the subtree rooted at INDX block `startVcn`. The rightmost
// leaf's last real entry is the in-order maximum (the predecessor used when no successor exists).
static int findRightmostLeaf(NtfsVolume *vol, const NtfsAttr *ia, uint64_t startVcn, uint8_t *buf, uint64_t *outVcn)
{
   uint64_t vcn = startVcn;
   for (int d = 0; d < NTFS_MAX_INDEX_DEPTH; d++) {
      if (readIndxBlock(vol, ia, vcn, buf) != 0) return -1;
      uint8_t *nd = buf + INDX_NODE_HEADER;
      if (isNodeLeaf(nd)) { *outVcn = vcn; return 0; }
      uint8_t *m = getNodeEndMarker(nd);                          // marker carries the rightmost child
      if (!m || !(readLe16(m + IDXENTRY_FLAGS) & IDXENTRY_FLAG_NODE)) return -1;
      vcn = readLe64(m + readLe16(m + IDXENTRY_LENGTH) - 8);
   }
   return -1;
}

// W7: removes the entry matching `key` from a large ($INDEX_ALLOCATION) directory's B-tree. Descends
// from the root; a leaf match is removed in place, an internal (separator) match is replaced by its
// in-order successor (which is then removed from its leaf). NTFS allows the resulting underfull/empty
// nodes, so no rebalancing is done. dirRecord holds the directory record. Returns 0, -1 on error.
// Used by unlinkPath when removeIndexEntry reports -3 (large index).
static int removeFromLargeIndex(NtfsVolume *vol, uint64_t dirRef, const uint8_t *key)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t blockSize  = vol->indexRecordSize;
   if (blockSize > NTFS_MAX_RECORD) return -1;   // W6c: sub-cluster blocks now supported
   uint32_t indxCap = blockSize - INDX_NODE_HEADER;
   if (readMftRecord(vol, dirRef, dirRecord) != 0) return -1;   // F048 ok: dirRef already validated by insertNameKey/removeNameKey before this delegated index I/O (same record, same lock)
   NtfsAttr root, ia;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident) return -1;
   if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia) != 1 || ia.resident) return -1;
   // SEC corruption guard: bound the root node by its PHYSICAL container (value minus the 16-byte
   // $INDEX_ROOT header), not getRootCapacity's insertion-slack-reduced value (which a validly full root reaches).
   if (root.valueLength < IDXROOT_NODE_HEADER ||
       getNodeUsedSize((uint8_t *)root.value + IDXROOT_NODE_HEADER) > root.valueLength - IDXROOT_NODE_HEADER) return -1;

   int curIsRoot = 1;
   uint64_t curVcn = 0;
   for (int depth = 0; depth < NTFS_MAX_INDEX_DEPTH; depth++) {
      uint8_t *curNd;
      if (curIsRoot) curNd = (uint8_t *)root.value + IDXROOT_NODE_HEADER;
      else { if (readIndxBlock(vol, &ia, curVcn, indexNodeA) != 0) return -1; curNd = indexNodeA + INDX_NODE_HEADER; }
      uint32_t used = getNodeUsedSize(curNd), pos = getNodeEntriesOffset(curNd);
      int leaf = isNodeLeaf(curNd);
      uint64_t descend = ~0ull;
      int foundPos = -1;
      for (int g = 0; g < 8192; g++) {
         int ev = validateIndexEntry(curNd, pos, used);
         if (ev < 0) return -1;
         uint8_t *e = curNd + pos;
         if (ev == 0) {                                       // last marker
            if (leaf) return -1;                              // key not present in this leaf
            descend = readLe64(e + readLe16(e + IDXENTRY_LENGTH) - 8);   // keys greater than all separators
            break;
         }
         int cmp = compareFileNameKeys(key, e + IDXENTRY_KEY);
         if (cmp == 0) { foundPos = (int)pos; break; }
         if (cmp < 0) {
            if (leaf) return -1;                              // would sort here but absent
            descend = readLe64(e + readLe16(e + IDXENTRY_LENGTH) - 8);   // child holds keys below e.key
            break;
         }
         pos += (uint32_t)ev;
      }

      if (foundPos >= 0) {
         if (leaf) {                                          // case A: remove from the leaf
            if (curIsRoot) return -1;                         // a large index never has a resident-root leaf
            if (removeNodeEntryAt(curNd, (uint32_t)foundPos) == ~0ull) return -1;
            return writeIndxBlock(vol, &ia, curVcn, indexNodeA);
         }
         // case B: separator in an internal node. Replace it with an adjacent key so the two child
         // ranges stay separated: prefer the in-order SUCCESSOR (leftmost entry of the right subtree);
         // if that leaf is empty (NTFS allows empty leaves, so a successor may not exist), fall back to
         // the PREDECESSOR (rightmost entry of the left subtree). If BOTH neighbour leaves are empty,
         // the separator sits between two empty ranges, so it can just be removed.
         uint8_t *e = curNd + foundPos;
         uint16_t elen = readLe16(e + IDXENTRY_LENGTH);
         uint64_t cleft = readLe64(e + elen - 8);             // separator's own child (keys below key)
         uint8_t *nxt = e + elen;                             // the entry/marker after e covers keys above key
         // validate the next entry before trusting its length to locate its child VCN (it follows the
         // found separator, so the descent loop never validated it). validateIndexEntry guarantees
         // nxtPos + length <= used, so the readLe64 at nxt+length-8 stays in-bounds even if malformed.
         uint32_t nxtPos = (uint32_t)foundPos + elen;
         if (validateIndexEntry(curNd, nxtPos, used) < 0) return -1;
         uint64_t cright = readLe64(nxt + readLe16(nxt + IDXENTRY_LENGTH) - 8);

         uint8_t  rKey[600]; int rKeyLen = 0; uint64_t rFileRef = 0; uint64_t replLeafVcn = 0;
         uint8_t *lnd = 0; uint32_t replPos = 0; int haveRepl = 0;

         if (findLeftmostLeaf(vol, &ia, cright, indexNodeB, &replLeafVcn) == 0) {   // try successor
            lnd = indexNodeB + INDX_NODE_HEADER;
            uint32_t p = getNodeEntriesOffset(lnd);
            if (validateIndexEntry(lnd, p, getNodeUsedSize(lnd)) > 0) { replPos = p; haveRepl = 1; }
         }
         if (!haveRepl && findRightmostLeaf(vol, &ia, cleft, indexNodeB, &replLeafVcn) == 0) {   // try predecessor
            lnd = indexNodeB + INDX_NODE_HEADER;
            uint32_t p = getNodeEntriesOffset(lnd), u = getNodeUsedSize(lnd), last = ~0u;
            for (int g = 0; g < 8192; g++) { int ev = validateIndexEntry(lnd, p, u); if (ev < 0) return -1; if (ev == 0) break; last = p; p += (uint32_t)ev; }
            if (last != ~0u) { replPos = last; haveRepl = 1; }
         }

         if (haveRepl) {
            uint8_t *le = lnd + replPos;
            rKeyLen = readLe16(le + IDXENTRY_KEY_LENGTH);
            rFileRef = readLe64(le + IDXENTRY_FILE_REF);
            if (rKeyLen < FN_NAME || rKeyLen > FN_NAME + 255 * 2) return -1;
            memCopy(rKey, le + IDXENTRY_KEY, rKeyLen);

            // Edit the separator's node IN MEMORY first (remove the old separator, insert the
            // replacement keeping the separator's own left child). Nothing is on disk yet, so a failure
            // here (e.g. a full record) refuses cleanly without corrupting or losing anything.
            if (removeNodeEntryAt(curNd, (uint32_t)foundPos) == ~0ull) return -1;
            if (curIsRoot) {
               if (insertRootEntry(vol, rFileRef, rKey, rKeyLen, 1, cleft) != 0) return -1;
            } else if (insertNodeEntry(curNd, indxCap, rFileRef, rKey, rKeyLen, 1, cleft) != 0) {
               return -1;
            }
            // Remove the replacement from its donor leaf (in memory).
            if (removeNodeEntryAt(lnd, replPos) == ~0ull) return -1;

            // Commit the PARENT first, then the donor leaf. A crash between the two writes leaves the
            // replacement key in BOTH nodes (a duplicate chkdsk reconciles from each file's $FILE_NAME)
            // rather than losing it — the leaf-first order would lose it. Tightened to atomic by W9.
            if (curIsRoot) {
               if (writeMftRecord(vol, dirRef, dirRecord) != 0) return -1;
               NtfsAttr ia2;   // $INDEX_ALLOCATION may have shifted if insertRootEntry resized the record
               if (findAttribute(dirRecord, recordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &ia2) != 1 || ia2.resident) return -1;
               return writeIndxBlock(vol, &ia2, replLeafVcn, indexNodeB);
            }
            if (writeIndxBlock(vol, &ia, curVcn, indexNodeA) != 0) return -1;
            return writeIndxBlock(vol, &ia, replLeafVcn, indexNodeB);
         }

         // both neighbour leaves empty: just remove the separator (its left child becomes unreferenced
         // but stays marked allocated in $BITMAP:$I30 / $INDEX_ALLOCATION = a valid free index block).
         if (removeNodeEntryAt(curNd, (uint32_t)foundPos) == ~0ull) return -1;
         if (curIsRoot) return writeMftRecord(vol, dirRef, dirRecord);
         return writeIndxBlock(vol, &ia, curVcn, indexNodeA);
      }

      if (descend == ~0ull) return -1;
      curIsRoot = 0; curVcn = descend;
   }
   return -1;                                                // depth guard: refuse rather than loop
}

// Frees a file's $DATA clusters (non-resident only; resident data lives in the record).
// Frees the clusters of EVERY non-resident attribute in a record: $DATA for a file, and
// $INDEX_ALLOCATION + $BITMAP:$I30 for a directory that grew a B-tree (W6). One walk covers files,
// directories and any future non-resident attribute ($ATTRIBUTE_LIST, named streams) instead of a
// per-type special case. Best-effort: an undecodable run is skipped (leaks, which chkdsk reclaims)
// rather than aborting the unlink.
static __attribute__((unused)) void freeAllNonResident(NtfsVolume *vol, uint64_t reference)
{
   if (readMftRecordByRef(vol, reference, mftRecord) != 0) return;   // F048: ref is on-disk-derived (AL ext / unlink target); validate sequence
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t usedSize   = readLe32(mftRecord + FILE_USED_SIZE);
   uint32_t limit      = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset     = readLe16(mftRecord + FILE_FIRST_ATTR_OFFSET);
   for (int guard = 0; guard < 256; guard++) {
      if (offset + 4 > limit) return;
      uint32_t attrType = readLe32(mftRecord + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) return;
      if (offset + 16 > limit) return;
      uint32_t attrLength = readLe32(mftRecord + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) return;
      NtfsAttr a;
      if (parseAttribute(mftRecord + offset, attrLength, &a) && !a.resident) {
         NtfsRunEntry runs[NTFS_MAX_FILE_RUNS];
         int count = 0;
         if (decodeRuns(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, runs, NTFS_MAX_FILE_RUNS, &count) == 0)
            freeClusterRuns(vol, runs, count);
      }
      offset += attrLength;
   }
}

// Frees the MFT record: clears the in-use flag, bumps the sequence number (so stale references are
// detectable), writes it back, then clears its $MFT $BITMAP bit. Best-effort.
static int freeMftRecord(NtfsVolume *vol, uint64_t reference)
{
   // F048: `reference` is on-disk-derived (an $ATTRIBUTE_LIST extension ref, or the unlink
   // target). A stale sequence means the record was already reused, so refuse to free it —
   // otherwise we would clear the in-use flag and $MFT bitmap bit of someone else's record.
   // Only proceed (free clusters' owner record + bitmap bit) after the sequence validates.
   uint64_t number = reference & MFT_REF_MASK;
   if (readMftRecordByRef(vol, reference, mftRecord) != 0) return -1;
   writeLe16(mftRecord + FILE_FLAGS, (uint16_t)(readLe16(mftRecord + FILE_FLAGS) & ~FILE_FLAG_IN_USE));
   uint16_t seq = (uint16_t)(readLe16(mftRecord + 16) + 1);
   writeLe16(mftRecord + 16, seq ? seq : 1);            // sequence never 0
   // jf-port: the result is reported (upstream ignored it), and the bitmap bit is only cleared once the
   // record itself says "not in use" -- a free bit over an in-use record is a record the next create reuses
   if (writeMftRecord(vol, number, mftRecord) != 0) return -1;
   return setMftRecordBit(vol, number, 0);
}

// jf-port: frees one record and THEN the clusters it referenced.  Upstream freed the clusters first
// and the record after, ignoring both results, so a write cut in between (the drive pulled) left an
// in-use record pointing at clusters marked free -- the next file would be given them -- and the
// delete still returned success.  The runs are read out first, since freeing the record is what
// makes them unreachable.  Returns 0, or -1 when a write failed (the caller reports it).
#define JF_FREE_RUNS_MAX 512
static NtfsRunEntry jfFreeRuns[JF_FREE_RUNS_MAX];
static int freeRecordThenClusters(NtfsVolume *vol, uint64_t reference)
{
   if (readMftRecordByRef(vol, reference, mftRecord) != 0) return -1;
   int total = 0;
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t usedSize   = readLe32(mftRecord + FILE_USED_SIZE);
   uint32_t limit      = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset     = readLe16(mftRecord + FILE_FIRST_ATTR_OFFSET);
   for (int guard = 0; guard < 256; guard++) {
      if (offset + 16 > limit) break;
      uint32_t attrType = readLe32(mftRecord + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) break;
      uint32_t attrLength = readLe32(mftRecord + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) break;
      NtfsAttr a;
      if (parseAttribute(mftRecord + offset, attrLength, &a) && !a.resident) {
         int count = 0;
         if (decodeRuns(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, jfFreeRuns + total,
                        JF_FREE_RUNS_MAX - total, &count) == 0)
            total += count;                               // (more than fit: the rest leak, chkdsk reclaims)
      }
      offset += attrLength;
   }
   if (freeMftRecord(vol, reference) != 0) return -1;   // the clusters stay marked: a leak at worst
   return freeClusterRuns(vol, jfFreeRuns, total) == 0 ? 0 : -1;
}

// W8b-S1: frees every extension MFT record a file spilled to (their non-resident clusters + the
// records themselves), by walking the base record's $ATTRIBUTE_LIST. Distinct extension refs are
// collected first (freeing clobbers the shared buffer). Best-effort: > NTFS_MAX_EXTENTS distinct
// extents leaves a leak chkdsk reclaims. No-op for a single-record file.
static int freeExtensionRecords(NtfsVolume *vol, uint64_t baseRef)
{
   uint32_t rs = vol->mftRecordSize;
   uint64_t baseMasked = baseRef & MFT_REF_MASK;
   if (readMftRecordByRef(vol, baseRef, mftRecord) != 0) return -1;   // F048: validate the base ref
   NtfsAttr listAttr;
   if (findAttribute(mftRecord, rs, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1 || !listAttr.resident)
      return freeRecordThenClusters(vol, baseRef);          // jf-port: no extensions: just the base
   uint64_t refs[NTFS_MAX_EXTENTS]; int nrefs = 0;   // FULL extension refs (index + sequence) for F048 validation
   const uint8_t *L = listAttr.value; uint32_t Llen = listAttr.valueLength, off = 0;
   for (int g = 0; g < 8192; g++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;
      uint64_t rFull = readLe64(L + off + AL_MFT_REF);            // keep the sequence
      uint64_t r = rFull & MFT_REF_MASK;
      if (r != baseMasked && r >= NTFS_FIRST_USER_RECORD) {
         int seen = 0; for (int i = 0; i < nrefs; i++) if ((refs[i] & MFT_REF_MASK) == r) seen = 1;
         if (!seen && nrefs < NTFS_MAX_EXTENTS) refs[nrefs++] = rFull;
      }
      off += elen;
   }
   // freeAllNonResident/freeMftRecord validate each ext ref's sequence (a stale AL ext ref
   // to a reused record is refused, so we never free a stranger's clusters/record).
   // jf-port: the base first (it is what references the extensions), each record before its clusters;
   // a cut anywhere leaves orphans and leaked clusters, which chkdsk reclaims, never a dangling reference
   int rc = freeRecordThenClusters(vol, baseRef);
   for (int i = 0; i < nrefs; i++) if (freeRecordThenClusters(vol, refs[i]) != 0) rc = -1;
   return rc;
}

// Deletes a file (wantDir=0) or empty directory (wantDir=1) at an in-volume path. Returns 0, -2 if
// already absent (idempotent), or -1 on error / refusal (type mismatch, non-empty dir, large index).
// W11: removes every one of a record's $FILE_NAME index entries from its parent directory (the Win32
// name and any DOS short-name twin — both must go or the survivor is orphaned). The record is in
// `record`; its name keys are copied out first because removal clobbers mftRecord. Refuses a record
// with more than two names (true hard links across directories are W12). Returns 0/-1.
static int removeAllNameKeys(NtfsVolume *vol, const uint8_t *record)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint32_t usedSize = readLe32(record + FILE_USED_SIZE);
   uint32_t limit    = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset   = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   uint8_t  keys[2][600];
   uint64_t parents[2];
   int      keyLens[2], nkeys = 0;
   for (int guard = 0; guard < 256; guard++) {
      if (offset + 4 > limit) break;
      uint32_t t = readLe32(record + offset + ATTR_TYPE_OFFSET);
      if (t == ATTR_END) break;
      if (offset + 16 > limit) break;
      uint32_t alen = readLe32(record + offset + ATTR_LENGTH_OFFSET);
      if (alen < 16 || alen > limit - offset) break;
      if (t == ATTR_FILE_NAME) {
         NtfsAttr a;
         if (!parseAttribute(record + offset, alen, &a) || !a.resident) return -1;
         if (a.valueLength < FN_MIN_SIZE || a.valueLength > 600) return -1;
         if (nkeys >= 2) return -1;                          // >2 names = a cross-directory hard link (W12)
         memCopy(keys[nkeys], a.value, (int)a.valueLength);  // a $FILE_NAME value IS a valid index key
         keyLens[nkeys] = (int)a.valueLength;
         parents[nkeys] = readLe64(a.value + FN_PARENT_REF);   // F048: keep the full parent ref (index + sequence) so removeNameKey validates it
         nkeys++;
      }
      offset += alen;
   }
   if (nkeys == 0) return -1;
   for (int i = 0; i < nkeys; i++)
      if (removeNameKey(vol, parents[i], keys[i]) != 0) return -1;
   return 0;
}

static int unlinkPath(NtfsVolume *vol, const char *path, int wantDir)
{
   if (!vol->writable) return -1;
   NtfsInfo info;
   if (resolvePath(vol, path, &info) != 0) return -2;       // already absent
   if (info.isDir != wantDir) return -1;
   uint64_t refFull   = info.mftReference;                 // full ref (index + sequence from the parent index entry)
   uint64_t reference = info.mftReference & MFT_REF_MASK;
   if (reference < NTFS_FIRST_USER_RECORD) return -1;       // never unlink a reserved/system record (incl. root = 5)

   // a directory must be empty to remove. Emptiness is decided by walking the index (handles both a
   // small resident $INDEX_ROOT and a large $INDEX_ALLOCATION B-tree that W7 has emptied); a dir that
   // once grew a B-tree but holds no entries is still removable — its index clusters are freed below.
   if (wantDir) {
      NtfsDir dir;
      char name[256];
      NtfsInfo child;
      openNtfsDir(&dir, vol, reference);
      int has = readNtfsDir(&dir, name, (int)sizeof(name), &child);
      if (dir.ioError) return -1;
      if (has == 1) return -1;                              // not empty
   }

   if (!vol->volumeDirty) { if (setVolumeDirty(vol, 1) != 0) return -1; vol->volumeDirty = 1; }

   // 1. unlink ALL of the record's names from their parent index (Win32 + any DOS twin), unreferencing
   //    before freeing. Removing only the path's name would orphan a twin's index entry (a chkdsk error).
   if (readMftRecordByRef(vol, refFull, mftRecord) != 0) return -1;   // F048: validate the unlink target's sequence
   if (removeAllNameKeys(vol, mftRecord) != 0) return -1;

   // 2. free the record's non-resident clusters ($DATA for a file; $INDEX_ALLOCATION + $BITMAP:$I30
   //    for an emptied large dir) plus any extension records it spilled to (W8b), then 3. free the base
   //    MFT record. A crash here only leaks (chkdsk reclaims).
   // jf-port: base record (with any extensions) freed before its clusters, and a failure reported:
   // the index entry is already gone, so the caller must know the rest did not finish
   return freeExtensionRecords(vol, refFull) == 0 ? 0 : -1;
}

int unlinkNtfsPath(NtfsVolume *vol, const char *path) { return unlinkPath(vol, path, 0); }
int rmdirNtfsPath(NtfsVolume *vol, const char *path)  { return unlinkPath(vol, path, 1); }

// ===========================================================================
// Write path W5: rename / move (same volume). Moves the $FILE_NAME index entry
// from the source parent's $I30 to the destination parent's, and rewrites the
// file's own $FILE_NAME attribute (parent ref + name), preserving its timestamps
// and sizes. The data and the MFT record stay put; a moved directory's children
// are unaffected (each child's parent ref is the moved dir's MFT number, which
// does not change). Publishes via the index first, so a crash leaves the file in
// at least one directory (never orphaned mid-move) - chkdsk resyncs the rest.
// ===========================================================================

// Counts the $FILE_NAME attributes in a record and returns the first in *out. A record with more
// than one (a DOS short-name twin) is refused by rename, since we would otherwise orphan the twin's
// index entry. Returns the count; *out is the first $FILE_NAME (valid only when the count >= 1).
static int countFileNames(const uint8_t *record, uint32_t recordSize, NtfsAttr *out)
{
   uint32_t usedSize = readLe32(record + FILE_USED_SIZE);
   uint32_t limit    = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset   = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   int count = 0;
   for (int guard = 0; guard < 256; guard++) {
      if (offset + 4 > limit) break;
      uint32_t attrType = readLe32(record + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) break;
      if (offset + 16 > limit) break;
      uint32_t attrLength = readLe32(record + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) break;
      if (attrType == ATTR_FILE_NAME) {
         if (count == 0) { if (!parseAttribute(record + offset, attrLength, out)) return -1; }
         count++;
      }
      offset += attrLength;
   }
   return count;
}

// Splits an in-volume path into parent ("/" preserved) + leaf. Returns 0, or -1 if malformed / the
// leaf is empty (can't rename the volume root).
static int splitPathParent(const char *path, char *parentOut, int parentCap, const char **leafOut)
{
   int lastSlash = -1;
   for (int i = 0; path[i]; i++) if (path[i] == '/') lastSlash = i;
   if (lastSlash < 0) return -1;
   const char *leaf = path + lastSlash + 1;
   if (!*leaf) return -1;
   int parentLen = lastSlash == 0 ? 1 : lastSlash;
   if (parentLen >= parentCap) return -1;
   memCopy(parentOut, path, parentLen); parentOut[parentLen] = '\0';
   *leafOut = leaf;
   return 0;
}

// W11/W8b: removes the whole attribute at `attrOffset` from a record (shifts the tail down, shrinks
// used size, clears the freed bytes). Returns 0/-1.
static int removeAttrAt(uint8_t *record, uint32_t attrOffset)
{
   uint32_t used = readLe32(record + FILE_USED_SIZE);
   if (attrOffset + 16 > used) return -1;
   uint32_t alen = readLe32(record + attrOffset + ATTR_LENGTH_OFFSET);
   if (alen < 16 || attrOffset + alen > used) return -1;
   uint32_t tailLen = used - (attrOffset + alen);
   memMove(record + attrOffset, record + attrOffset + alen, (int)tailLen);
   writeLe32(record + FILE_USED_SIZE, used - alen);
   memSet(record + (used - alen), 0, (int)alen);
   return 0;
}

// W11/W8b: inserts a resident, unnamed attribute (type/id/value) into a record in ascending type
// order (NTFS requires attributes sorted by type code). Returns 0, or -1 if it won't fit the record
// (the caller then spills via $ATTRIBUTE_LIST — W8b). The end marker must already be present.
static int insertResidentAttrSorted(uint8_t *record, uint32_t recordSize, uint32_t type, uint16_t id,
                                    int indexed, const uint8_t *value, uint32_t valueLen)
{
   uint32_t length = ALIGN8(24 + valueLen);                   // resident header (24) + value, 8-aligned
   uint32_t used = readLe32(record + FILE_USED_SIZE);
   if (used + length > recordSize) return -1;
   uint32_t limit = used <= recordSize ? used : recordSize;
   uint32_t off = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   for (int g = 0; g < 256; g++) {                            // find the first attr with a greater type / the end
      if (off + 4 > limit) return -1;
      uint32_t t = readLe32(record + off + ATTR_TYPE_OFFSET);
      if (t == ATTR_END || t > type) break;
      if (off + 16 > limit) return -1;
      uint32_t al = readLe32(record + off + ATTR_LENGTH_OFFSET);
      if (al < 16 || off + al > limit) return -1;
      off += al;
   }
   uint32_t tailLen = used - off;                             // open a gap of `length` at off
   memMove(record + off + length, record + off, (int)tailLen);
   uint8_t *a = record + off;
   memSet(a, 0, length);
   writeLe32(a + ATTR_TYPE_OFFSET, type);
   writeLe32(a + ATTR_LENGTH_OFFSET, length);
   a[ATTR_NON_RESIDENT] = 0; a[ATTR_NAME_LENGTH] = 0;
   writeLe16(a + ATTR_NAME_OFFSET, 0);
   writeLe16(a + ATTR_ID_OFFSET, id);
   writeLe32(a + ATTR_RES_VALUE_LENGTH, valueLen);
   writeLe16(a + ATTR_RES_VALUE_OFFSET, 24);
   a[22] = (uint8_t)(indexed ? 1 : 0);
   if (valueLen) memCopy(a + 24, value, (int)valueLen);
   writeLe32(record + FILE_USED_SIZE, used + length);
   return 0;
}

// W11: rewrites a file record's $FILE_NAME attribute(s) to `win32` (+ a `dos` twin when dosLen>0),
// going from the existing 1 or 2 names to the new 1 or 2. Removes the old $FILE_NAME attrs and inserts
// the new one(s) in type order, then sets the hard-link count to the non-DOS name count (1). 0/-1.
static int rewriteRecordNames(uint8_t *record, uint32_t recordSize,
                              const uint8_t *win32, int win32Len, const uint8_t *dos, int dosLen)
{
   uint32_t offs[2]; int nfn = 0;
   uint32_t used = readLe32(record + FILE_USED_SIZE);
   uint32_t limit = used <= recordSize ? used : recordSize;
   uint32_t off = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   for (int g = 0; g < 256; g++) {
      if (off + 4 > limit) break;
      uint32_t t = readLe32(record + off + ATTR_TYPE_OFFSET);
      if (t == ATTR_END) break;
      if (off + 16 > limit) break;
      uint32_t al = readLe32(record + off + ATTR_LENGTH_OFFSET);
      if (al < 16 || al > limit - off) break;
      if (t == ATTR_FILE_NAME) { if (nfn < 2) offs[nfn] = off; nfn++; }
      off += al;
   }
   if (nfn < 1 || nfn > 2) return -1;
   if (nfn == 2 && removeAttrAt(record, offs[1]) != 0) return -1;   // remove higher offset first
   if (removeAttrAt(record, offs[0]) != 0) return -1;
   uint16_t id = readLe16(record + 40);
   if (insertResidentAttrSorted(record, recordSize, ATTR_FILE_NAME, id, 1, win32, (uint32_t)win32Len) != 0) return -1;
   writeLe16(record + 40, (uint16_t)(id + 1));
   if (dos && dosLen > 0) {
      id = readLe16(record + 40);
      if (insertResidentAttrSorted(record, recordSize, ATTR_FILE_NAME, id, 1, dos, (uint32_t)dosLen) != 0) return -1;
      writeLe16(record + 40, (uint16_t)(id + 1));
   }
   writeLe16(record + FILE_HARD_LINK_COUNT, 1);               // Win32(+DOS twin) => one hard link (DOS not counted)
   return 0;
}

// Renames/moves `from` to `to` on the same volume. Returns 0, -2 if the destination already exists (a
// different file), or -1 on error / refusal. Preserves timestamps/sizes (from the source's first
// $FILE_NAME); writes the destination as a single POSIX name (matching create). Removes any old DOS twin
// the source still carried, and handles a case-only rename.
int renameNtfsPath(NtfsVolume *vol, const char *from, const char *to)
{
   if (!vol->writable) return -1;

   NtfsInfo src;
   if (resolvePath(vol, from, &src) != 0) return -1;             // source must exist
   uint64_t fileRefMasked = src.mftReference & MFT_REF_MASK;
   if (fileRefMasked < NTFS_FIRST_USER_RECORD) return -1;        // never relink a reserved/system record

   char srcParent[MAX_PATH_LEN], dstParent[MAX_PATH_LEN];
   const char *srcLeaf, *dstLeaf;
   if (splitPathParent(from, srcParent, (int)sizeof srcParent, &srcLeaf) != 0) return -1;
   if (splitPathParent(to,   dstParent, (int)sizeof dstParent, &dstLeaf) != 0) return -1;

   NtfsInfo sp, dp;
   if (resolvePath(vol, srcParent, &sp) != 0 || !sp.isDir) return -1;
   if (resolvePath(vol, dstParent, &dp) != 0 || !dp.isDir) return -1;
   uint64_t srcParentRef = sp.mftReference & MFT_REF_MASK;
   uint64_t dstParentRef = dp.mftReference & MFT_REF_MASK;
   uint64_t srcParentFull = sp.mftReference, dstParentFull = dp.mftReference;   // F048: full refs (with sequence) for validated reads
   uint64_t fileRefFull   = src.mftReference;

   // destination must not already be a DIFFERENT file. A case-only rename (or to==from) resolves to
   // the same record and is allowed.
   { NtfsInfo dst; if (resolvePath(vol, to, &dst) == 0 && (dst.mftReference & MFT_REF_MASK) != fileRefMasked) return -2; }

   // destination leaf -> UTF-16
   uint16_t units[257];
   utf8ToUtf16(dstLeaf, units, 256);
   int nlen = 0; while (nlen < 255 && units[nlen]) nlen++;
   if (nlen == 0 || units[nlen]) return -1;

   // read the source record; capture the timestamp/size/flags template and the existing name keys
   if (readMftRecordByRef(vol, fileRefFull, mftRecord) != 0) return -1;   // F048: validate the rename source's sequence
   NtfsAttr fn0;
   int oldCount = countFileNames(mftRecord, vol->mftRecordSize, &fn0);
   if (oldCount < 1 || oldCount > 2 || !fn0.resident || fn0.valueLength < FN_MIN_SIZE || fn0.valueLength > 600) return -1;
   uint8_t tmpl[600]; memCopy(tmpl, fn0.value, (int)fn0.valueLength);

   uint8_t oldKeys[2][600]; int nold = 0;
   {  uint32_t used = readLe32(mftRecord + FILE_USED_SIZE), lim = used <= vol->mftRecordSize ? used : vol->mftRecordSize;
      uint32_t off = readLe16(mftRecord + FILE_FIRST_ATTR_OFFSET);
      for (int g = 0; g < 256; g++) {
         if (off + 4 > lim) break; uint32_t t = readLe32(mftRecord + off + ATTR_TYPE_OFFSET);
         if (t == ATTR_END) break; if (off + 16 > lim) break;
         uint32_t al = readLe32(mftRecord + off + ATTR_LENGTH_OFFSET); if (al < 16 || al > lim - off) break;
         if (t == ATTR_FILE_NAME) { NtfsAttr a; if (parseAttribute(mftRecord + off, al, &a) && a.resident &&
             a.valueLength <= 600 && nold < 2) { memCopy(oldKeys[nold], a.value, (int)a.valueLength); nold++; } }
         off += al;
      }
   }
   if (nold < 1) return -1;

   if (!vol->volumeDirty) { if (setVolumeDirty(vol, 1) != 0) return -1; vol->volumeDirty = 1; }

   // the parent back-reference needs the destination parent's real sequence (see getRecordFullRef);
   // mftRecord is free here (dirty-flag write clobbered it) and is re-read for the source record below
   uint64_t dstParentFullRef = getRecordFullRef(vol, dstParentRef);
   if (dstParentFullRef == ~0ull) return -1;

   // build the new POSIX $FILE_NAME from the template (keep timestamps/sizes, change parent/name)
   uint8_t newName[600];
   memCopy(newName, tmpl, FN_NAME);
   writeLe64(newName + FN_PARENT_REF, dstParentFullRef);
   newName[FN_NAME_LENGTH] = (uint8_t)nlen;
   newName[FN_NAMESPACE]   = FN_NAMESPACE_POSIX;
   for (int i = 0; i < nlen; i++) writeLe16(newName + FN_NAME + i * 2, units[i]);
   int newNameLen = FN_NAME + nlen * 2;

   // 1. rewrite the file record's own $FILE_NAME attribute(s) to the single new name (authoritative copy).
   //    Re-read it first: setVolumeDirty above reused the shared mftRecord buffer, so it no longer holds
   //    this file's record. (tmpl/oldKeys/newName were already copied out, so they stay valid.)
   if (readMftRecordByRef(vol, fileRefFull, mftRecord) != 0) return -1;   // F048: validate the source record's sequence
   if (rewriteRecordNames(mftRecord, vol->mftRecordSize, newName, newNameLen, 0, 0) != 0) return -1;
   if (writeMftRecord(vol, fileRefMasked, mftRecord) != 0) return -1;

   // 2. update the parent index/indexes. Same dir: remove old key(s) first (avoids a self-duplicate on
   //    a case-only rename), then insert the new one(s). Cross dir: publish into the destination first,
   //    then unpublish from the source. Either way the record's $FILE_NAME is authoritative, so any
   //    crash mid-update is a chkdsk-repairable index/record mismatch, never a lost or cross-linked file.
   uint64_t fileRef = src.mftReference;
   int rc = 0;
   if (srcParentRef == dstParentRef) {
      for (int i = 0; i < nold; i++) if (removeNameKey(vol, srcParentFull, oldKeys[i]) != 0) rc = -1;
      if (insertNameKey(vol, dstParentFull, fileRef, newName, newNameLen) != 0) rc = -1;
   } else {
      if (insertNameKey(vol, dstParentFull, fileRef, newName, newNameLen) != 0) return -1;
      for (int i = 0; i < nold; i++) if (removeNameKey(vol, srcParentFull, oldKeys[i]) != 0) rc = -1;
   }
   // The record's $FILE_NAME is already the authoritative new name; if any index update failed the
   // result is a record/index mismatch that chkdsk reconciles from the record (never a lost file).
   // Surface the error rather than reporting success.
   return rc;
}

// ===========================================================================
// W8b-S1: $DATA runlist spill to an extension MFT record. When a file's non-resident $DATA won't fit
// the base record (alongside $STD_INFO + $FILE_NAME), move the whole $DATA attribute to a dedicated
// extension record and give the base an $ATTRIBUTE_LIST. The W8a read side (gatherRuns) already
// follows the list and merges, so reads need no change; the $DATA write path is routed through
// findDataAnywhere so it locates $DATA wherever it lives. Modelled on ntfs-3g attrlist.c/attrib.c.
// ===========================================================================

// Allocates + formats an in-use extension MFT record whose base reference is `baseRefFull` (record# +
// sequence). Grows the MFT if full. Returns 0 with *outRef = ref (record# + seq 1), or -1.
static int allocExtensionRecord(NtfsVolume *vol, uint64_t baseRefFull, uint64_t *outRef)
{
   uint64_t n = findFreeMftRecord(vol);
   if (n == ~0ull) { if (growMft(vol, 8) != 0) return -1; n = findFreeMftRecord(vol); }
   if (n == ~0ull || setMftRecordBit(vol, n, 1) != 0) return -1;
   uint8_t er[NTFS_MAX_RECORD];
   formatEmptyMftRecord(er, vol->mftRecordSize, vol->bytesPerSector, n);
   writeLe16(er + FILE_FLAGS, FILE_FLAG_IN_USE);              // in use, not a directory
   writeLe16(er + 16, 1);                                     // sequence number 1
   writeLe16(er + FILE_HARD_LINK_COUNT, 0);                   // extents have no names / link count
   writeLe64(er + FILE_BASE_REFERENCE, baseRefFull);          // points home (marks this as an extent)
   if (writeMftRecord(vol, n, er) != 0) { setMftRecordBit(vol, n, 0); return -1; }
   *outRef = n | ((uint64_t)1 << 48);
   return 0;
}

// Builds an $ATTRIBUTE_LIST value enumerating every attribute currently in `base` (all housed in
// baseRefFull). Returns the value length, or -1 on overflow of `outCap`.
static int buildAttrListValue(const uint8_t *base, uint32_t recordSize, uint64_t baseRefFull, uint8_t *out, int outCap)
{
   uint32_t used = readLe32(base + FILE_USED_SIZE), lim = used < recordSize ? used : recordSize;
   uint32_t off = readLe16(base + FILE_FIRST_ATTR_OFFSET);
   int o = 0;
   for (int g = 0; g < 256; g++) {
      if (off + 4 > lim) break;
      uint32_t t = readLe32(base + off + ATTR_TYPE_OFFSET);
      if (t == ATTR_END) break;
      if (off + 16 > lim) break;
      uint32_t al = readLe32(base + off + ATTR_LENGTH_OFFSET);
      if (al < 16 || al > lim - off) break;
      uint8_t  nameLen = base[off + ATTR_NAME_LENGTH];
      uint16_t nameOff = readLe16(base + off + ATTR_NAME_OFFSET);
      if (nameLen && (uint32_t)off + nameOff + (uint32_t)nameLen * 2 > lim) return -1;   // name in-bounds
      uint32_t elen = (uint32_t)ALIGN8(AL_MIN_ENTRY + (uint32_t)nameLen * 2);
      if (o + (int)elen > outCap) return -1;
      uint8_t *e = out + o;
      memSet(e, 0, (int)elen);
      writeLe32(e + AL_TYPE, t);
      writeLe16(e + AL_LENGTH, (uint16_t)elen);
      e[AL_NAME_LENGTH] = nameLen;
      e[AL_NAME_OFFSET] = AL_MIN_ENTRY;
      writeLe64(e + AL_START_VCN, base[off + ATTR_NON_RESIDENT] ? readLe64(base + off + ATTR_NR_START_VCN) : 0);
      writeLe64(e + AL_MFT_REF, baseRefFull);
      writeLe16(e + AL_ATTR_ID, readLe16(base + off + ATTR_ID_OFFSET));
      for (uint8_t i = 0; i < nameLen; i++) writeLe16(e + AL_NAME + i * 2, readLe16(base + off + nameOff + i * 2));
      o += (int)elen;
      off += al;
   }
   return o;
}

// Moves the base record's unnamed non-resident $DATA into a fresh extension record and gives the base
// an $ATTRIBUTE_LIST, so a runlist too large for the base record is stored in a dedicated extent.
// dirRecord holds the base on entry/exit. The runlist (runs/runCount + sizes) is written into the
// extension $DATA. Returns 0/-1. (S1: single $DATA fragment per extent; multi-fragment is S1b.)
static int spillDataToExtension(NtfsVolume *vol, uint64_t baseRef, const NtfsRunEntry *runs, int runCount,
                                uint64_t realSize, uint64_t validSize, uint64_t allocSize, uint64_t lastVcn)
{
   uint32_t rs = vol->mftRecordSize;
   if (readMftRecord(vol, baseRef, dirRecord) != 0) return -1;   // F048 ok: baseRef is the file's own record from the open write handle (validated at open)
   uint64_t baseRefFull = baseRef | ((uint64_t)readLe16(dirRecord + 16) << 48);

   // refuse if the runlist won't even fit a dedicated extension record (multi-fragment = S1b)
   uint8_t enc[1024];
   int encLen = encodeRuns(runs, runCount, enc, (int)sizeof enc);
   if (encLen < 0) return -1;
   if (ATTR_NR_HEADER_MIN + (uint32_t)encLen + 8 > rs) return -1;

   // 1. allocate the extension record and write the $DATA into it (allocate-before-reference).
   uint64_t extRef;
   if (allocExtensionRecord(vol, baseRefFull, &extRef) != 0) return -1;
   uint8_t er[NTFS_MAX_RECORD];
   if (readMftRecord(vol, extRef & MFT_REF_MASK, er) != 0) return -1;   // F048 ok: extRef was just returned by allocExtensionRecord (internally generated, no on-disk sequence to trust)
   uint16_t dataId = readLe16(er + 40);
   // create a tiny resident placeholder $DATA, then convert it to the non-resident runlist
   if (insertResidentAttrSorted(er, rs, ATTR_DATA, dataId, 0, 0, 0) != 0) { freeMftRecord(vol, extRef); return -1; }
   writeLe16(er + 40, (uint16_t)(dataId + 1));
   NtfsAttr da;
   if (findAttribute(er, rs, ATTR_DATA, 0, 0, &da) != 1) { freeMftRecord(vol, extRef); return -1; }
   uint32_t daOff = (uint32_t)(da.attr - er);
   if (setNonResidentData(er, rs, daOff, runs, runCount, realSize, validSize, allocSize, lastVcn) != 0) { freeMftRecord(vol, extRef); return -1; }
   if (writeMftRecord(vol, extRef & MFT_REF_MASK, er) != 0) { freeMftRecord(vol, extRef); return -1; }

   // 2. rebuild the base: remove its old $DATA, add an $ATTRIBUTE_LIST listing everything (base attrs +
   //    the relocated $DATA in the extent). Commit the base last (the atomic publish point). Re-read
   //    the base: allocExtensionRecord may have grown the MFT, which reuses dirRecord.
   if (readMftRecord(vol, baseRef, dirRecord) != 0) return -1;   // F048 ok: baseRef is the file's own record from the open write handle (validated at open)
   NtfsAttr bd;
   if (findAttribute(dirRecord, rs, ATTR_DATA, 0, 0, &bd) == 1) {
      if (removeAttrAt(dirRecord, (uint32_t)(bd.attr - dirRecord)) != 0) return -1;
   }
   uint8_t al[1024];
   int alLen = buildAttrListValue(dirRecord, rs, baseRefFull, al, (int)sizeof al);
   if (alLen < 0) return -1;
   // append the relocated $DATA's entry (type 0x80, unnamed, vcn 0, housed in the extent)
   { uint32_t elen = (uint32_t)ALIGN8(AL_MIN_ENTRY);
     if (alLen + (int)elen > (int)sizeof al) return -1;
     uint8_t *e = al + alLen; memSet(e, 0, (int)elen);
     writeLe32(e + AL_TYPE, ATTR_DATA); writeLe16(e + AL_LENGTH, (uint16_t)elen);
     e[AL_NAME_OFFSET] = AL_MIN_ENTRY;
     writeLe64(e + AL_START_VCN, 0); writeLe64(e + AL_MFT_REF, extRef); writeLe16(e + AL_ATTR_ID, dataId);
     alLen += (int)elen; }
   uint16_t alId = readLe16(dirRecord + 40);
   if (insertResidentAttrSorted(dirRecord, rs, ATTR_ATTRIBUTE_LIST, alId, 0, al, (uint32_t)alLen) != 0) return -1;
   writeLe16(dirRecord + 40, (uint16_t)(alId + 1));
   if (writeMftRecord(vol, baseRef, dirRecord) != 0) return -1;
   return 0;
}

// Locates a file's unnamed $DATA wherever it lives. Reads the base into `buf`; if $DATA is there,
// returns the base ref. Otherwise follows the base $ATTRIBUTE_LIST to the housing extent, reads it
// into `buf`, and returns the extent ref. Fills *out (pointing into buf) and *housingRef. The caller
// writes *housingRef's record back after any modification. Returns 0/-1.
static int findDataAnywhere(NtfsVolume *vol, uint64_t baseRef, uint8_t *buf, NtfsAttr *out, uint64_t *housingRef)
{
   uint32_t rs = vol->mftRecordSize;
   if (readMftRecord(vol, baseRef, buf) != 0) return -1;   // F048 ok: baseRef is the file's own record from the open handle (validated at open)
   if (findAttribute(buf, rs, ATTR_DATA, 0, 0, out) == 1) { *housingRef = baseRef; return 0; }
   NtfsAttr listAttr;
   if (findAttribute(buf, rs, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1 || !listAttr.resident) return -1;
   const uint8_t *L = listAttr.value; uint32_t Llen = listAttr.valueLength, off = 0;
   for (int g = 0; g < 8192; g++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;
      if (readLe32(L + off + AL_TYPE) == ATTR_DATA && L[off + AL_NAME_LENGTH] == 0 &&
          readLe64(L + off + AL_START_VCN) == 0) {
         uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
         uint64_t eref = erefFull & MFT_REF_MASK;
         uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
         if (eref < NTFS_FIRST_USER_RECORD) return -1;         // a list must not point $DATA at a system record
         if (readMftRecordByRef(vol, erefFull, buf) != 0) return -1;
         if (findAttributeInstance(buf, rs, ATTR_DATA, 0, 0, eid, out) != 1) return -1;
         *housingRef = eref;
         return 0;
      }
      off += elen;
   }
   return -1;
}

void seekNtfs(NtfsFile *file, uint64_t position) { if (file) file->position = position; }

int closeNtfs(NtfsFile *file)
{
   if (!file || !file->dirty || !file->writable || !file->vol || !file->vol->writable) return 0;
   int rc = trimOverAllocation(file);                       // reclaim reserve-ahead slack (W2 D)
   if (rc == 0) rc = syncFileNameSizes(file->vol, file->mftReference);
   file->dirty = 0;
   return rc;
}

// Free (zero) bits per nibble value, so the scan costs one lookup per nibble instead of eight
// shift-and-test steps over hundreds of millions of clusters.
static const uint8_t zeroBitsPerNibble[16] = { 4, 3, 3, 2, 3, 2, 2, 1, 3, 2, 2, 1, 2, 1, 1, 0 };

// Walks $Bitmap through the file engine. Correct but costs two device reads per 512 bytes (readNtfs
// re-reads $Bitmap's MFT record every call), so it is only the fallback for a $Bitmap too fragmented
// to cache a runlist for - a volume that is refused writes anyway.
static int countFreeClustersViaFileEngine(NtfsVolume *vol, uint64_t clusterCount, uint64_t *out)
{
   NtfsFile bitmap;
   if (openFileByRef(&bitmap, vol, MFT_RECORD_BITMAP) != 0) return -1;

   uint64_t freeClusters = 0;
   uint64_t bitIndex = 0;
   uint8_t chunk[512];
   int got = 0;
   while (bitIndex < clusterCount && (got = readNtfs(&bitmap, chunk, (int)sizeof(chunk))) > 0) {
      for (int i = 0; i < got && bitIndex < clusterCount; i++) {
         int bits = (clusterCount - bitIndex) >= 8 ? 8 : (int)(clusterCount - bitIndex);
         for (int bit = 0; bit < bits; bit++)
            if (!((chunk[i] >> bit) & 1)) freeClusters++;
         bitIndex += bits;
      }
   }
   if (got < 0) return -1;
   *out = freeClusters;
   return 0;
}

// Counts the zero (free) bits in $Bitmap's $DATA, exactly clusterCount bits (bit 0 of byte 0 = cluster
// 0, LSB first). Run once at mount to seed vol->freeClusters, then maintained incrementally by
// allocateClusters / setClusterBits. Returns 0 with *out set, or -1.
//
// Reads through the cached $Bitmap runlist in fileBounce-sized blocks. Mount blocks on this, and the
// cost grows with the volume, so reads-per-byte is what decides how long a large drive takes to appear.
static int countNtfsFreeClusters(NtfsVolume *vol, uint64_t *out)
{
   uint64_t clusterCount = vol->totalSectors / vol->sectorsPerCluster;
   if (vol->bitmapRunCount == 0) return countFreeClustersViaFileEngine(vol, clusterCount, out);

   uint64_t wholeBytes = clusterCount / 8;
   uint32_t tailBits   = (uint32_t)(clusterCount % 8);
   if (vol->bitmapDataSize && wholeBytes > vol->bitmapDataSize) {   // $Bitmap shorter than the volume it
      wholeBytes = vol->bitmapDataSize;                             // describes: count what exists, no more
      tailBits   = 0;
   }

   // whole bytes, in the largest blocks the bounce buffer allows
   uint64_t freeClusters = 0;
   for (uint64_t at = 0; at < wholeBytes; ) {
      uint32_t want = NTFS_READ_BOUNCE;
      if (wholeBytes - at < want) want = (uint32_t)(wholeBytes - at);
      if (readBitmapBytes(vol, at, fileBounce, want) != 0) return -1;
      for (uint32_t i = 0; i < want; i++)
         freeClusters += zeroBitsPerNibble[fileBounce[i] & 0xF] + zeroBitsPerNibble[fileBounce[i] >> 4];
      at += want;
   }

   // trailing partial byte: bits past clusterCount are padding and must not be counted
   if (tailBits) {
      uint8_t last;
      if (readBitmapBytes(vol, wholeBytes, &last, 1) != 0) return -1;
      for (uint32_t bit = 0; bit < tailBits; bit++)
         if (!((last >> bit) & 1)) freeClusters++;
   }

   *out = freeClusters;
   return 0;
}

// O(1): the bitmap is counted once at mount and the count is kept current on every alloc/free, so the
// file manager can refresh free space on the UI thread without rescanning the whole $Bitmap (parity
// with exFAT). totalBytes is pure geometry.
int getNtfsFree(const NtfsVolume *vol, uint64_t *freeBytes, uint64_t *totalBytes)
{
   uint64_t clusterCount = vol->totalSectors / vol->sectorsPerCluster;
   if (totalBytes) *totalBytes = clusterCount * vol->bytesPerCluster;   // callers may pass NULL (free-space widget)
   if (freeBytes)  *freeBytes  = vol->freeClusters * vol->bytesPerCluster;
   return 0;
}

// ===========================================================================
// jf-port: the VFS backend half of the original (volume and handle pools, the backend mutex, the
// open/close/dirty-flag wrappers and VFS registration) is not vendored.  source/local/lfs.cpp owns
// volumes, handles and the one lock, and does what the wrappers did around a write:
//   * a write open needs vol->writable (a clean mount);
//   * after a writer closes, or a create/mkdir/unlink/rmdir/rename finishes, and no writer is
//     still open, the on-disk dirty flag is cleared with ntfsClearDirty().
// ===========================================================================

// Clears the on-disk volume-dirty flag this session set (no-op when it never set it).  The caller
// holds the lock and has checked that no writer is open on the volume.  Returns 0 or -1.
int ntfsClearDirty(NtfsVolume *vol)
{
   if (!vol || !vol->mounted || !vol->volumeDirty || !vol->writable) return 0;
   if (setVolumeDirty(vol, 0) != 0) return -1;
   vol->volumeDirty = 0;
   return 0;
}
