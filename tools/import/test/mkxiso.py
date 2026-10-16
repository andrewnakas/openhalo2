#!/usr/bin/env python3
"""TEST-ONLY XISO writer for exercising h2import (never ships, never needed by users).

Packs a folder into an XDVDFS image laid out the way Xbox images are:
  sector 32 of the game partition = volume descriptor ("MICROSOFT*XBOX*MEDIA"
  at 0 and 0x7EC, root dir sector/size), directory tables as balanced binary
  trees (left/right offsets in dwords, entries 4-byte aligned, never straddling
  a sector, 0xFF padding), file data sector-aligned.

usage:
  mkxiso.py SRC OUT.iso [--redump] [--only GLOB ...] [--extra DIR NAME SRCFILE ...]
    --redump   put the game partition at 0x18300000 (Redump full-disc layout)
               with junk in front of it, like a video partition
    --only     include only files whose relative path matches a glob
    --extra    add a file whose raw on-disc NAME is used verbatim (for
               path-traversal tests); DIR is "" for the root
"""
import argparse
import fnmatch
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
REDUMP_OFFSET = 0x18300000


class Node:
    def __init__(self, name, is_dir, src=None, size=0):
        self.name = name          # bytes
        self.is_dir = is_dir
        self.src = src
        self.size = size
        self.children = []
        self.sector = 0
        self.table = b""


def build_tree(root, only):
    def walk(path, rel):
        node = Node(b"", True)
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            r = f"{rel}/{name}" if rel else name
            if os.path.isdir(full):
                child = walk(full, r)
                if only and not child.children:
                    continue
                child.name = name.encode()
                node.children.append(child)
            else:
                if only and not any(fnmatch.fnmatch(r, g) for g in only):
                    continue
                node.children.append(Node(name.encode(), False, full, os.path.getsize(full)))
        return node
    return walk(root, "")


def find_dir(root, rel):
    node = root
    for part in [p for p in rel.split("/") if p]:
        node = next(c for c in node.children if c.is_dir and c.name.decode() == part)
    return node


def layout_table(entries):
    """entries sorted; returns bytes of the directory table (tree)."""
    order = []                       # preorder of (entry, left_idx, right_idx)

    def place(lo, hi):
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        idx = len(order)
        order.append([entries[mid], None, None])
        order[idx][1] = place(lo, mid)
        order[idx][2] = place(mid + 1, hi)
        return idx

    place(0, len(entries))
    offs, pos = [], 0
    for e, _, _ in order:
        sz = (14 + len(e.name) + 3) & ~3
        if pos // SECTOR != (pos + sz - 1) // SECTOR:
            pos = (pos // SECTOR + 1) * SECTOR
        offs.append(pos)
        pos += sz
    total = (pos + SECTOR - 1) // SECTOR * SECTOR
    buf = bytearray(b"\xff" * total)
    for i, (e, li, ri) in enumerate(order):
        left = offs[li] // 4 if li is not None else 0
        right = offs[ri] // 4 if ri is not None else 0
        size = len(e.table) if e.is_dir else e.size
        rec = struct.pack("<HHIIBB", left, right, e.sector, size, 0x10 if e.is_dir else 0x20,
                          len(e.name)) + e.name
        buf[offs[i]:offs[i] + len(rec)] = rec
        pad = ((len(rec) + 3) & ~3) - len(rec)
        buf[offs[i] + len(rec):offs[i] + len(rec) + pad] = b"\xff" * pad
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--redump", action="store_true")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--extra", nargs=3, action="append", default=[], metavar=("DIR", "NAME", "SRC"))
    a = ap.parse_args()

    root = build_tree(a.src, a.only)
    for d, name, src in a.extra:
        find_dir(root, d).children.append(Node(name.encode("latin-1"), False, src, os.path.getsize(src)))

    dirs, files = [], []

    def collect(n):
        if n.is_dir:
            dirs.append(n)
            n.children.sort(key=lambda c: c.name.upper())
            for c in n.children:
                collect(c)
        else:
            files.append(n)
    collect(root)

    # pass 1: table sizes (sector numbers do not change sizes)
    for d in reversed(dirs):
        d.table = layout_table(d.children) if d.children else b""
    sector = 33
    for d in dirs:
        if d.table:
            d.sector = sector
            sector += len(d.table) // SECTOR
    for f in files:
        f.sector = sector if f.size else 0
        sector += (f.size + SECTOR - 1) // SECTOR
    # pass 2: tables with final sectors
    for d in reversed(dirs):
        d.table = layout_table(d.children) if d.children else b""

    base = REDUMP_OFFSET if a.redump else 0
    total = base + sector * SECTOR
    with open(a.out, "wb") as o:
        if a.redump:
            o.write(os.urandom(64 * 1024))     # stand-in "video partition" bytes
        o.seek(base + 32 * SECTOR)
        vd = bytearray(SECTOR)
        vd[0:20] = MAGIC
        struct.pack_into("<II", vd, 20, root.sector, len(root.table))
        vd[0x7EC:0x800] = MAGIC
        o.write(vd)
        for d in dirs:
            if d.table:
                o.seek(base + d.sector * SECTOR)
                o.write(d.table)
        for f in files:
            if not f.size:
                continue
            o.seek(base + f.sector * SECTOR)
            with open(f.src, "rb") as i:
                while True:
                    chunk = i.read(4 << 20)
                    if not chunk:
                        break
                    o.write(chunk)
        o.truncate(total)
    print(f"{a.out}: {len(files)} files, {len(dirs)} dirs, {total} bytes, partition at 0x{base:X}")


if __name__ == "__main__":
    sys.exit(main())
