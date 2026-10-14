#!/bin/bash
# The NTFS write path, end to end: images made by mkntfs, written by test_ntfs_write (the code under
# test), judged by tools/check_ntfs (libntfs-3g) and ntfscat (ntfs-3g's own reader).
#
#   bash fixtures/fs/ntfs_write_check.sh [quick|full] [be]
#
# Run from tests/.  Needs mkntfs, ntfscat, sfdisk and the ntfs-3g headers (ntfs-3g-dev); "be" runs
# the writer built big-endian under qemu-ppc64 (the PS3's byte order).  No root needed.
set -euo pipefail
MODE=${1:-quick}
ENDIAN=${2:-le}
cd "$(dirname "$0")/../.."
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

MOHASI=../third_party/mohasi_fs
SRC="test_ntfs_write.c mohasi_host_storage.c $MOHASI/ntfs.c"
if [ "$ENDIAN" = be ]; then
    powerpc64-linux-gnu-gcc -static -O2 -Wall -Wno-misleading-indentation -Wno-format-truncation -I$MOHASI -o "$WORK/w" $SRC
    RUN="qemu-ppc64 $WORK/w"
else
    gcc -O2 -g -Wall -Wno-misleading-indentation -Wno-format-truncation -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -I$MOHASI -o "$WORK/w" $SRC
    RUN="$WORK/w"
fi
gcc -O2 -Wall -o "$WORK/check" tools/check_ntfs.c -lntfs-3g
gcc -O2 -w -I$MOHASI -o "$WORK/fnv" $SRC          # (its fnv mode hashes ntfscat's output)

fails=0
note() { echo "== $*"; }
bad()  { echo "FAILED: $*"; fails=$((fails + 1)); }

mkfs() {   # $1 file  $2 size  $3 cluster bytes  [$4 "mbr" -> filesystem inside an MBR partition at 2048]
    rm -f "$1"
    if [ "${4:-}" = mbr ]; then
        truncate -s "$2" "$1"
        printf 'label: dos\nstart=2048, type=7\n' | sfdisk --quiet "$1"
        local fs="$1.fs" sectors
        sectors=$(( $(stat -c %s "$1") / 512 - 2048 ))
        truncate -s $((sectors * 512)) "$fs"
        mkntfs -F -f -q -c "$3" -p 2048 -L JFTEST "$fs" >/dev/null 2>&1
        dd if="$fs" of="$1" bs=512 seek=2048 conv=notrunc status=none
        rm -f "$fs"
    else
        truncate -s "$2" "$1"
        mkntfs -F -f -q -c "$3" -L JFTEST "$1" >/dev/null 2>&1
    fi
}

# an image inside an MBR is checked through a copy of its partition
judge() {   # $1 image  $2 "mbr"|""  $3 extra check flags
    local img="$1"
    if [ "${2:-}" = mbr ]; then
        dd if="$1" of="$1.part" bs=512 skip=2048 status=none
        img="$1.part"
    fi
    if ! "$WORK/check" ${3:-} "$img"; then bad "check_ntfs $1"; fi
    echo "$img"
}

workload() {   # $1 label  $2 size  $3 cluster  $4 scale  [$5 mbr]
    local img="$WORK/$1.img"
    note "workload $1 ($2, ${3}-byte clusters, scale $4${5:+, $5})"
    mkfs "$img" "$2" "$3" "${5:-}"
    if ! $RUN workload "$img" "$4" > "$WORK/$1.out"; then cat "$WORK/$1.out"; bad "writer $1"; return; fi
    grep -E '^(HASH|FREE|ntfs write)' "$WORK/$1.out"
    local judged
    judged=$(judge "$img" "${5:-}" | tail -1)
    # ntfscat, an independent reader, must produce the same bytes
    local path hash
    read -r _ path hash < <(grep '^HASH' "$WORK/$1.out")
    local h2
    h2=$(ntfscat "$judged" "$path" 2>/dev/null | "$WORK/fnv" fnv)
    [ "$h2" = "$hash" ] && echo "ntfscat agrees: $path" || bad "ntfscat hash $h2 != writer's $hash for $path"
}

if [ "$MODE" = full ]; then
    workload w4k   1536M 4096  1
    workload w512  1G    512   2
    workload w1k   1G    1024  2
    workload w2k   1G    2048  2
    workload w64k  2G    65536 1
    workload wmbr  1536M 4096  2 mbr
else
    workload w4k   768M  4096  4
    workload wmbr  768M  4096  8 mbr
fi
[ "$ENDIAN" = be ] && SCALE_CUT=64 || SCALE_CUT=16

if [ "$MODE" = full ]; then
    # one file past 4 GiB: needs ~5 GB of real disk, so not under /tmp (tmpfs)
    BIG=$(mktemp -d -p "$HOME")
    note "big: a 4.5 GiB file in 512 KB pieces (in $BIG)"
    mkfs "$BIG/big.img" 6G 4096
    if $RUN big "$BIG/big.img" 4608 > "$BIG/out"; then
        cat "$BIG/out"
        "$WORK/check" "$BIG/big.img" || bad "check_ntfs big"
        hash=$(sed -n 's/^HASH [^ ]* //p' "$BIG/out")
        h2=$(ntfscat "$BIG/big.img" /big/film.ts 2>/dev/null | "$WORK/fnv" fnv)
        [ "$h2" = "$hash" ] && echo "ntfscat agrees: /big/film.ts" || bad "ntfscat hash $h2 != $hash for the big file"
    else cat "$BIG/out"; bad "big"; fi
    rm -rf "$BIG"
fi

note "fill"
mkfs "$WORK/fill.img" 256M 4096
if ! $RUN fill "$WORK/fill.img"; then bad "fill"; fi
"$WORK/check" "$WORK/fill.img" || bad "check_ntfs after fill"

cuts() {   # $1 cluster bytes  $2 number of spread-out cut points
    note "cut ($1-byte clusters): the drive pulled after N storage writes"
    mkfs "$WORK/cut0.img" 256M "$1"
    cp "$WORK/cut0.img" "$WORK/count.img"
    # how many writes the whole workload takes, so the cut points cover all of it (folders, records,
    # both media files, the rename, the deletes) -- plus the first writes one by one
    total=$($RUN cut "$WORK/count.img" 100000000 $SCALE_CUT | sed -n 's/.*writes=\([0-9]*\).*/\1/p')
    echo "the workload takes $total storage writes"
    NPTS=$2
    POINTS="1 2 3 4 5 6 7 8 13 21 34 55 89 $(python3 -c "
import random
random.seed(7)
t, n = $total, $NPTS
print(' '.join(str(max(90, t * i // n + random.randint(0, max(1, t // n)))) for i in range(1, n)))")"
    for n in $POINTS; do
        cp "$WORK/cut0.img" "$WORK/cut.img"
        out=$($RUN cut "$WORK/cut.img" "$n" $SCALE_CUT) || { echo "$out"; bad "cut $n: writer checks"; }
        line=$(echo "$out" | grep '^CUT')
        res=$("$WORK/check" -c -q "$WORK/cut.img") && rc=0 || rc=$?
        if [ $rc = 2 ]; then
            # A cut between the two copies of an $MFT/$MFTMirr record write: Windows' chkdsk puts the
            # mirror right; on Linux it is ntfsfix, which is what a user there would run.  Then judge.
            ntfsfix "$WORK/cut.img" >/dev/null 2>&1 || true
            res=$("$WORK/check" -c -q "$WORK/cut.img") && rc=0 || rc=$?
            line="$line (mirror repaired by ntfsfix first)"
        fi
        if [ $rc != 0 ]; then echo "$line"; echo "$res"; bad "cut $n: check_ntfs"; else echo "$line ok"; fi
        echo "$line" | grep -q 'finished=1' && break
    done

}
# Cuts on 4 KB clusters only: with clusters smaller than an MFT record (512 bytes) a record can
# straddle two runs and take two writes, so a cut between them tears it -- which is why lfs.cpp
# writes only to volumes whose clusters hold a whole record and index block.  "cut512" still runs
# that series, to see it.
if [ "$MODE" = full ]; then cuts 4096 400; elif [ "$MODE" = cut512 ]; then cuts 512 100; else cuts 4096 40; fi

echo
[ $fails -eq 0 ] && echo "ntfs write check ($MODE, $ENDIAN): all passed" || { echo "ntfs write check ($MODE, $ENDIAN): $fails FAILED"; exit 1; }
