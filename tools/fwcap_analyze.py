#!/usr/bin/env python3
"""Read the firmware capture written by source/video/avconf_capture.cpp.

The app, when /dev_hdd0/tmp/jellyfin_fwcapture.txt exists, dumps the loaded
(already decrypted) segments of cellSysutilAvconfExt and libsysutil plus an
index of where cellVideoOutConfigure2 & co. live.  This turns that into
something a person can read:

  * disassembly of each captured entry point (capstone, PPC64 big-endian);
  * every `bl` resolved -- to a local routine, or through the module's import
    trampoline to the firmware function it calls (FNID reversed by hashing
    candidate names, the same way the app binds them);
  * every `sc` resolved to its lv2 syscall number/name;
  * a summary of how each argument register is used -- loads/stores off r3..r6
    are the struct layout the whole 24p question hinges on.

Usage:
    python tools/fwcap_analyze.py <dir-with-jf_fwcap-files> [--depth 2] [-n 160]

Fetch the files first, e.g. over FTP from /dev_hdd0/tmp/jf_fwcap_* .
Needs: pip install capstone.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

FNID_SUFFIX = bytes([0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                     0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A])


def fnid(name):
    return struct.unpack("<I", hashlib.sha1(name.encode() + FNID_SUFFIX).digest()[:4])[0]


# Control: these are PSL1GHT's own table values.  If the hash ever disagrees,
# every name below is wrong, so refuse to run.
assert fnid("cellVideoOutConfigure") == 0x0BAE8772
assert fnid("cellVideoOutGetState") == 0x887572D5


def load_names():
    """FNID -> name, from tools/fwcap_names.txt ("<module> <name>" lines,
    collected from RPCS3's REG_FUNC registrations) plus a few extras."""
    names = {}
    path = os.path.join(HERE, "fwcap_names.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            parts = line.split()
            if len(parts) == 2:
                names[fnid(parts[1])] = parts[1]
    return names


def load_syscalls():
    path = os.path.join(HERE, "fwcap_syscalls.txt")
    out = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            parts = line.split()
            if len(parts) >= 2 and parts[0].isdigit():
                out[int(parts[0])] = " ".join(parts[1:])
    return out


# ---------------------------------------------------------------- the capture

class Memory:
    def __init__(self):
        self.segs = []          # (base, bytes, tag)

    def add(self, base, data, tag):
        self.segs.append((base, data, tag))

    def find(self, addr):
        for base, data, tag in self.segs:
            if base <= addr < base + len(data):
                return base, data, tag
        return None

    def u32(self, addr):
        s = self.find(addr)
        if not s or addr + 4 > s[0] + len(s[1]):
            return None
        return struct.unpack(">I", s[1][addr - s[0]:addr - s[0] + 4])[0]

    def cstr(self, addr, maxlen=64):
        s = self.find(addr)
        if not s:
            return None
        off = addr - s[0]
        raw = s[1][off:off + maxlen]
        end = raw.find(b"\0")
        if end <= 0:
            return None
        raw = raw[:end]
        if not all(0x20 <= c < 0x7F for c in raw):
            return None
        return raw.decode()

    def bytes_at(self, addr, n):
        s = self.find(addr)
        if not s:
            return b""
        off = addr - s[0]
        return s[1][off:off + n]


def parse_index(capdir):
    idx_path = os.path.join(capdir, "jf_fwcap_index.txt")
    text = open(idx_path, encoding="utf-8", errors="replace").read()
    entries = []   # (name, code)
    for m in re.finditer(r"slot\[\d+\] (\S+) opd=0x([0-9a-f]+) code=0x([0-9a-f]+)", text):
        entries.append((m.group(1), int(m.group(3), 16)))
    for m in re.finditer(r"^(cellVideoOut\w+) opd=0x([0-9a-f]+) code=0x([0-9a-f]+)", text, re.M):
        if int(m.group(3), 16):
            entries.append((m.group(1), int(m.group(3), 16)))
    unresolved = re.findall(r"slot\[\d+\] (\S+) UNRESOLVED", text)
    mem = Memory()
    for m in re.finditer(r"wrote /dev_hdd0/tmp/(jf_fwcap_(\w+?)_seg(\d+)_([0-9a-f]+)\.bin)", text):
        fname, tag, seg, base = m.group(1), m.group(2), int(m.group(3)), int(m.group(4), 16)
        local = os.path.join(capdir, fname)
        if not os.path.exists(local):
            print(f"warning: {fname} listed in the index but not present locally")
            continue
        mem.add(base, open(local, "rb").read(), f"{tag}:seg{seg}")
    return text, entries, unresolved, mem


def find_import_stubs(mem):
    """Scan every captured segment for 44-byte import headers (0x2c000001 ...)
    and map each stub SLOT address to (library, fnid)."""
    slot_map = {}
    libs = []
    for base, data, tag in mem.segs:
        for off in range(0, len(data) - 44, 4):
            if data[off:off + 4] != b"\x2c\x00\x00\x01":
                continue
            (h1, attr, nfunc, nvar, ntls, _hash, name_p, fnid_p,
             stub_p) = struct.unpack(">IHHHHIIII", data[off:off + 28])
            if not (0 < nfunc < 1024):
                continue
            lib = mem.cstr(name_p)
            if not lib:
                continue
            ok = True
            for i in range(nfunc):
                f = mem.u32(fnid_p + 4 * i)
                if f is None:
                    ok = False
                    break
                slot_map[stub_p + 4 * i] = (lib, f)
            if ok:
                libs.append((tag, base + off, lib, nfunc))
    return slot_map, libs


# ------------------------------------------------------------- disassembly

def make_disassembler():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_64 | capstone.CS_MODE_BIG_ENDIAN)
    # Without this capstone silently STOPS at the first word it cannot decode
    # (a jump table, a literal), which truncates any linear scan.
    md.skipdata = True
    return md


def disasm(md, mem, addr, count):
    code = mem.bytes_at(addr, count * 4)
    return list(md.disasm(code, addr))


def trampoline_slot(md, mem, target):
    """If `target` is an import trampoline (lis/addis r12 + lwz r12,off(r12) +
    ... mtctr + bctr), return the slot address it loads."""
    hi = None
    for ins in disasm(md, mem, target, 10):
        ops = ins.op_str.replace(" ", "")
        m = re.match(r"r12,(-?0x[0-9a-f]+|-?\d+)$", ops)
        if ins.mnemonic == "lis" and m:
            hi = int(m.group(1), 0) & 0xFFFF
            continue
        if ins.mnemonic == "li" and m:
            hi = int(m.group(1), 0) & 0xFFFF   # Sony's form: li r12,0 then oris
            continue
        m2 = re.match(r"r12,r12,(0x[0-9a-f]+|\d+)$", ops)
        if ins.mnemonic == "oris" and m2:
            hi = int(m2.group(1), 0) & 0xFFFF
            continue
        m = re.match(r"r12,(-?0x[0-9a-f]+|-?\d+)\(r12\)$", ops)
        if ins.mnemonic == "lwz" and m and hi is not None:
            lo = int(m.group(1), 0)
            return ((hi << 16) + lo) & 0xFFFFFFFF
        if ins.mnemonic in ("bctr", "blr"):
            break
    return None


def describe_call(md, mem, target, slot_map, names):
    slot = trampoline_slot(md, mem, target)
    if slot is not None and slot in slot_map:
        lib, f = slot_map[slot]
        return f"IMPORT {lib}:{names.get(f, '0x%08x' % f)}"
    if mem.find(target):
        return f"local sub_{target:08x}"
    return f"outside capture 0x{target:08x}"


ARG_REGS = ("r3", "r4", "r5", "r6", "r7", "r8")


def analyse_function(md, mem, name, addr, n, slot_map, names, syscalls, depth, seen, out):
    if addr in seen or depth < 0:
        return
    seen.add(addr)
    out.append(f"\n==== {name} @ 0x{addr:08x} ====")
    alias = {r: r for r in ARG_REGS}      # current reg -> which arg it holds
    accesses = []
    last_r11 = None
    callees = []
    reach = addr
    insns = disasm(md, mem, addr, n)
    if not insns:
        out.append("  (not inside any captured segment)")
        return
    for ins in insns:
        note = ""
        ops = ins.op_str.replace(" ", "")
        if ins.mnemonic == "bl":
            tgt = int(ops, 0)
            note = describe_call(md, mem, tgt, slot_map, names)
            if note.startswith("local"):
                callees.append(tgt)
        elif ins.mnemonic == "li" and ops.startswith("r11,"):
            last_r11 = int(ops.split(",")[1], 0)
        elif ins.mnemonic == "sc":
            note = f"SYSCALL {last_r11} {syscalls.get(last_r11, '?')}"
        elif ins.mnemonic == "mr":
            d, s = ops.split(",")[:2]
            if s in alias and alias[s] in ARG_REGS:
                alias[d] = alias[s]
            elif d in alias:
                del alias[d]
        m = re.match(r"(r\d+),(-?0x[0-9a-f]+|-?\d+)\((r\d+)\)$", ops)
        if m and ins.mnemonic[0] in "ls" and m.group(3) in alias:
            arg = alias[m.group(3)]
            off = int(m.group(2), 0)
            accesses.append(f"{arg}+0x{off:x} {ins.mnemonic}")
            note = (note + " " if note else "") + f"[{arg} + 0x{off:x}]"
        # Any other write to an aliased register ends the alias (the access was
        # recorded above first, so `lwz r3,0(r3)` still counts against r3).
        dest = ops.split(",")[0] if "," in ops else None
        writes = not (ins.mnemonic.startswith(("st", "cmp", "b")) or ins.mnemonic == "mr")
        if writes and dest in alias:
            del alias[dest]
        if ins.mnemonic == "bl":
            # r3..r12 are volatile across a call; only copies in r14+ survive.
            for r in [r for r in alias if int(r[1:]) < 14]:
                del alias[r]
        out.append(f"  {ins.address:08x}: {ins.bytes.hex()}  {ins.mnemonic:8s} {ins.op_str:28s} {note}")
        # End of function: a return/tail-jump that no earlier forward branch
        # jumps past.  Track the furthest local branch target seen so far.
        if ins.mnemonic.startswith("b") and ins.mnemonic not in ("bl", "blr", "bctr", "bctrl"):
            tm = re.search(r"(0x[0-9a-f]+)$", ops)
            if tm:
                reach = max(reach, int(tm.group(1), 16))
        if ins.mnemonic in ("blr", "bctr") and ins.address >= reach:
            break
    if accesses:
        out.append("  -- argument accesses (first use order): " + ", ".join(dict.fromkeys(accesses)))
    for c in callees:
        analyse_function(md, mem, f"sub_{c:08x} (from {name})", c, n, slot_map, names,
                         syscalls, depth - 1, seen, out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capdir")
    ap.add_argument("--depth", type=int, default=2, help="local call depth to follow")
    ap.add_argument("-n", type=int, default=200, help="max instructions per function")
    a = ap.parse_args(argv)

    try:
        import capstone
    except ImportError:
        sys.exit("pip install capstone")
    md = make_disassembler()

    text, entries, unresolved, mem = parse_index(a.capdir)
    names = load_names()
    syscalls = load_syscalls()
    slot_map, libs = find_import_stubs(mem)

    out = ["# fwcap analysis", "", "## index", text.strip(), "", "## import tables found"]
    for tag, at, lib, n in libs:
        out.append(f"  {tag} @0x{at:08x}  {lib}  ({n} functions)")
    if unresolved:
        out.append("")
        out.append("UNRESOLVED (firmware does not export these FNIDs): " + ", ".join(unresolved))
    seen = set()
    for name, code in entries:
        analyse_function(md, mem, name, code, a.n, slot_map, names, syscalls, a.depth, seen, out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
