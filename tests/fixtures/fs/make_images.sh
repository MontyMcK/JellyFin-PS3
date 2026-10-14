#!/bin/bash
# Builds the 64 MB NTFS and exFAT test images in this directory (run as root, in WSL or Linux).
#
#   sudo bash make_images.sh
#
# Needs: ntfs-3g (mkntfs, ntfs-3g), exfatprogs (mkfs.exfat), kernel exfat + loop, sfdisk, sgdisk, python3, gzip.
#
# Six images, one tree each (see tree.py): a volume at LBA 0, and a volume inside an MBR
# partition and a GPT partition (both starting at sector 2048).  They are written compressed
# (<name>.img.gz); the host test unpacks them.  File contents come from tree.py's generator, so
# the test regenerates the expected bytes instead of storing hashes.
#
# The test images are made with real Linux drivers, not with the code under test.
set -euo pipefail
cd "$(dirname "$0")"
HERE=$(pwd)
WORK=$(mktemp -d)
trap 'umount "$WORK/mnt" 2>/dev/null || true; losetup -D 2>/dev/null || true; rm -rf "$WORK"' EXIT
mkdir -p "$WORK/mnt"

MB=$((1024 * 1024))
FS_BYTES=$((60 * MB))          # the filesystem
DISK_BYTES=$((64 * MB))        # the disk it sits on, with a partition table
PART_START=2048                # sectors

populate() {   # $1 = mount point, $2 = "sparse" to add the >4 GB sparse file
    python3 "$HERE/tree.py" populate "$1" "$2"
}

make_ntfs() {   # $1 = output file (a bare filesystem)
    truncate -s $FS_BYTES "$1"
    mkntfs --fast --force --quiet --label "JFNTFS" "$1" >/dev/null
    ntfs-3g "$1" "$WORK/mnt" -o rw,big_writes,windows_names
    populate "$WORK/mnt" sparse
    sync
    umount "$WORK/mnt"
}

make_exfat() {
    truncate -s $FS_BYTES "$1"
    mkfs.exfat -L JFEXFAT "$1" >/dev/null
    mount -o loop -t exfat "$1" "$WORK/mnt"
    populate "$WORK/mnt" nosparse
    sync
    umount "$WORK/mnt"
}

wrap() {   # $1 = fs image, $2 = out, $3 = mbr|gpt : put the filesystem inside a partitioned disk
    truncate -s $DISK_BYTES "$2"
    if [ "$3" = mbr ]; then
        printf 'label: dos\nstart=%s, type=7\n' "$PART_START" | sfdisk --quiet "$2"
    else
        sgdisk --clear --new=1:$PART_START:0 --typecode=1:0700 "$2" >/dev/null
    fi
    dd if="$1" of="$2" bs=512 seek=$PART_START conv=notrunc status=none
}

pack() { gzip -9 -n -c "$1" > "$2.img.gz"; ls -l "$2.img.gz" | awk '{print $5, $9}'; }

make_ntfs "$WORK/ntfs.fs"
make_exfat "$WORK/exfat.fs"
ntfsinfo -F /big/sparse5g.bin "$WORK/ntfs.fs" 2>/dev/null | grep -i -E 'allocated|data size|sparse|runlist' | head -5 || true

cp "$WORK/ntfs.fs" "$WORK/ntfs_flat.img";  pack "$WORK/ntfs_flat.img" ntfs_flat
cp "$WORK/exfat.fs" "$WORK/exfat_flat.img"; pack "$WORK/exfat_flat.img" exfat_flat
wrap "$WORK/ntfs.fs"  "$WORK/ntfs_mbr.img"  mbr; pack "$WORK/ntfs_mbr.img"  ntfs_mbr
wrap "$WORK/ntfs.fs"  "$WORK/ntfs_gpt.img"  gpt; pack "$WORK/ntfs_gpt.img"  ntfs_gpt
wrap "$WORK/exfat.fs" "$WORK/exfat_mbr.img" mbr; pack "$WORK/exfat_mbr.img" exfat_mbr
wrap "$WORK/exfat.fs" "$WORK/exfat_gpt.img" gpt; pack "$WORK/exfat_gpt.img" exfat_gpt
