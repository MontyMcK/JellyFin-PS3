#!/usr/bin/env python3
"""The file tree on the test images, and its content generator.

    tree.py populate MOUNTPOINT sparse|nosparse   write the tree (make_images.sh)
    tree.py manifest                              print "path size seed" per file

The C test (tests/test_mohasi_fs.c) has the same generator, so the expected bytes of
every file are known without storing hashes:

    block = index // 64                            (64-byte blocks)
    byte  = block & 0xFF, (block >> 8) & 0xFF, then (block * 7 + seed) & 0xFF
            for the 62 remaining bytes of the block

A cluster read from the wrong place, or two clusters swapped, shows up as a wrong block number.
The blocks are long runs, so the images compress to a few hundred KB.
"""
import os
import sys

# (path, size, seed)
FILES = [
    ("hello.txt", None, None),                       # literal text
    ("Movies/Film One (2019).mkv", 3 * 1024 * 1024, 11),   # the fragmented one
    ("Movies/sub/Deep.ts", 100 * 1024 + 17, 12),
    ("Music/Album/01 Track.flac", 200 * 1024, 13),
    ("Unicode/Café Ünïcode.txt", None, None),
    ("Unicode/日本語/ファイル.txt", None, None),
    ("Unicode/emoji \U0001F3AC clip.mkv", 70000, 14),      # outside the BMP: a surrogate pair
]
LITERAL = {
    "hello.txt": b"hello world\n",
    "Unicode/Café Ünïcode.txt": b"unicode\n",
    "Unicode/日本語/ファイル.txt": b"nihongo\n",
}
MANY = 400                      # /many/f0000.txt .. : forces a multi-block directory
BIG_SIZE = 5 * 1024 ** 3        # NTFS only: sparse, > 4 GB
BIG_TAIL_AT = BIG_SIZE - 4096   # a real, written block at the very end
BIG_TAIL_SEED = 99


def gen(seed, start, n):
    out = bytearray(n)
    for i in range(n):
        idx = start + i
        block, off = idx >> 6, idx & 63
        if off == 0:
            out[i] = block & 0xFF
        elif off == 1:
            out[i] = (block >> 8) & 0xFF
        else:
            out[i] = (block * 7 + seed) & 0xFF
    return bytes(out)


def populate(root, sparse):
    def put(rel, data):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    # fragment the volume: fill it with spacers, free every other one, then write the big
    # file into the gaps
    for i in range(24):
        put("spacers/s%02d.bin" % i, b"\xAA" * (160 * 1024))
    for i in range(1, 24, 2):
        os.remove(os.path.join(root, "spacers/s%02d.bin" % i))
    for rel, size, seed in FILES:
        if rel in LITERAL:
            put(rel, LITERAL[rel])
        else:
            put(rel, gen(seed, 0, size))
    for i in range(MANY):
        put("many/f%04d.txt" % i, ("file %d\n" % i).encode())
    if sparse == "sparse":
        path = os.path.join(root, "big/sparse5g.bin")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.truncate(BIG_SIZE)
            f.seek(BIG_TAIL_AT)
            f.write(gen(BIG_TAIL_SEED, 0, 4096))


def manifest():
    for rel, size, seed in FILES:
        n = len(LITERAL[rel]) if rel in LITERAL else size
        print("%s\t%d\t%s" % (rel, n, "literal" if rel in LITERAL else seed))


if __name__ == "__main__":
    if sys.argv[1] == "populate":
        populate(sys.argv[2], sys.argv[3])
    else:
        manifest()
