#!/usr/bin/env python3
"""Self-test for fwcap_analyze.py on a synthetic capture.

Hand-encoded PPC64 big-endian: a fake "Configure2" that reads its second
argument as a struct (directly and through a copy in r31), calls a firmware
import through a Sony-style trampoline, calls a local helper and issues
syscall 367.  If the analyser cannot recover all of that here, it cannot be
trusted on the real capture.

    python tools/test_fwcap_analyze.py
"""
import io
import os
import struct
import sys
import tempfile
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fwcap_analyze as fa  # noqa: E402


def li(d, v):       return (14 << 26) | (d << 21) | (v & 0xFFFF)
def oris(a, s, v):  return (25 << 26) | (s << 21) | (a << 16) | (v & 0xFFFF)
def lwz(d, off, a): return (32 << 26) | (d << 21) | (a << 16) | (off & 0xFFFF)
def lbz(d, off, a): return (34 << 26) | (d << 21) | (a << 16) | (off & 0xFFFF)
def lhz(d, off, a): return (40 << 26) | (d << 21) | (a << 16) | (off & 0xFFFF)
def stw(s, off, a): return (36 << 26) | (s << 21) | (a << 16) | (off & 0xFFFF)
def std(s, off, a): return (62 << 26) | (s << 21) | (a << 16) | (off & 0xFFFC)
def mr(a, s):       return (31 << 26) | (s << 21) | (a << 16) | (s << 11) | (444 << 1)
def mtctr(s):       return 0x7C0903A6 | (s << 21)
def bl(pc, t):      return (18 << 26) | ((t - pc) & 0x03FFFFFC) | 1
def beq(pc, t):     return (16 << 26) | (12 << 21) | (2 << 16) | ((t - pc) & 0xFFFC)
BCTR, BLR, SC, NOP = 0x4E800420, 0x4E800020, 0x44000002, 0x60000000

TEXT, DATA = 0x10000, 0x10200
F_CFG2, F_SUB, F_TRAMP, SLOT = 0x10000, 0x10080, 0x10100, 0x10200
HDR, NAME, FNIDS = 0x10300, 0x10400, 0x10410


def build_text():
    t = bytearray(0x200)

    def put(addr, words):
        for i, w in enumerate(words):
            struct.pack_into(">I", t, addr - TEXT + 4 * i, w)

    put(F_CFG2, [
        mr(31, 4),                 # keep arg2 in a non-volatile
        lbz(9, 0, 4),              # arg2 + 0
        lbz(10, 1, 31),            # arg2 + 1, through the copy
        lhz(0, 6, 4),              # arg2 + 6
        bl(F_CFG2 + 16, F_TRAMP),  # -> import
        lwz(9, 8, 31),             # arg2 + 8 still visible after the call (r31)
        lbz(9, 2, 4),              # r4 is volatile: must NOT count as arg2
        bl(F_CFG2 + 28, F_SUB),    # -> local helper
        li(11, 367),
        SC,
        BLR,
    ])
    # An early return that a forward branch jumps past, then the NEXT
    # function, which must not be read as part of this one.
    put(F_SUB, [lwz(0, 12, 3), stw(0, 0, 5), beq(F_SUB + 8, F_SUB + 16), BLR,
                lwz(0, 16, 3), BLR,
                lbz(9, 0x77, 3)])
    # Sony import trampoline: li r12,0 / oris r12,r12,hi / lwz r12,lo(r12) ...
    put(F_TRAMP, [li(12, 0), oris(12, 12, SLOT >> 16), lwz(12, SLOT & 0xFFFF, 12),
                  std(2, 40, 1), lwz(0, 0, 12), lwz(2, 4, 12), mtctr(0), BCTR])
    return bytes(t)


def build_data():
    d = bytearray(0x300)
    struct.pack_into(">I", d, SLOT - DATA, 0x00020000)            # resolved OPD
    struct.pack_into(">IHHHHIIII", d, HDR - DATA, 0x2C000001, 0x0009, 1, 0, 0, 0,
                     NAME, FNIDS, SLOT)
    d[NAME - DATA:NAME - DATA + 12] = b"cellSysutil\0"
    struct.pack_into(">I", d, FNIDS - DATA, fa.fnid("cellVideoOutConfigure"))
    return bytes(d)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        text, data = build_text(), build_data()
        open(os.path.join(tmp, "jf_fwcap_avconfext_seg0_00010000.bin"), "wb").write(text)
        open(os.path.join(tmp, "jf_fwcap_avconfext_seg1_00010200.bin"), "wb").write(data)
        open(os.path.join(tmp, "jf_fwcap_index.txt"), "w").write(
            "jf_fwcap v1\n"
            "slot[0] cellVideoOutConfigure2 opd=0x00020000 code=0x00010000 toc=0x00000000\n"
            "slot[3] cellVideoOutGetScreenSize UNRESOLVED\n"
            "module[avconfext] code=0x00010000 id=0x00000001\n"
            f"    wrote /dev_hdd0/tmp/jf_fwcap_avconfext_seg0_00010000.bin ({len(text)} bytes)\n"
            f"    wrote /dev_hdd0/tmp/jf_fwcap_avconfext_seg1_00010200.bin ({len(data)} bytes)\n")
        buf = io.StringIO()
        with redirect_stdout(buf):
            fa.main([tmp])
        out = buf.getvalue()

    expect = [
        "IMPORT cellSysutil:cellVideoOutConfigure",
        "SYSCALL 367 sys_uart_initialize",
        "local sub_00010080",
        "==== sub_00010080",
        "r4+0x0 lbz", "r4+0x1 lbz", "r4+0x6 lhz", "r4+0x8 lwz",
        "r3+0xc lwz", "r5+0x0 stw", "r3+0x10 lwz",
        "UNRESOLVED (firmware does not export these FNIDs): cellVideoOutGetScreenSize",
        "cellSysutil  (1 functions)",
    ]
    reject = ["r4+0x2 lbz",   # read through r4 AFTER a call: not arg2 any more
              "r3+0x77"]      # belongs to the next function
    bad = [e for e in expect if e not in out] + [r for r in reject if r in out]
    if bad:
        print(out)
        print("test_fwcap_analyze: FAILED on", bad)
        return 1
    print("test_fwcap_analyze: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
