"""Minimal XDVDFS (Xbox ISO) lister/extractor.

Usage:
    python tools/xiso.py list    <image.iso>
    python tools/xiso.py extract <image.iso> <out_dir>
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Redump-style full images carry the game partition at this offset; trimmed
# xiso images start at 0.
PARTITION_OFFSETS = (0, 0x18300000)


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise ValueError("no XDVDFS volume descriptor found")


def read_dir(f, base, sector, size):
    """Yield (name, attr, sector, size) for every entry in a directory table."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    # The table is a binary tree; walking it from the root node visits every
    # entry, while linear scanning would trip over 0xFF sector padding.
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, ent_sector, ent_size, attr, name_len = struct.unpack_from(
            "<HHIIBB", table, off
        )
        if left == 0xFFFF:  # padding
            continue
        name = table[off + 14 : off + 14 + name_len].decode("latin-1")
        yield name, attr, ent_sector, ent_size
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def walk(f, base, sector, size, prefix=""):
    for name, attr, ent_sector, ent_size in read_dir(f, base, sector, size):
        path = f"{prefix}{name}"
        if attr & 0x10:
            yield path, True, ent_sector, ent_size
            yield from walk(f, base, ent_sector, ent_size, path + "/")
        else:
            yield path, False, ent_sector, ent_size


def open_volume(path):
    f = open(path, "rb")
    base = find_partition(f)
    f.seek(base + 32 * SECTOR + len(MAGIC))
    root_sector, root_size = struct.unpack("<II", f.read(8))
    return f, base, root_sector, root_size


def cmd_list(iso):
    f, base, rs, rz = open_volume(iso)
    total = 0
    for path, is_dir, _, size in sorted(walk(f, base, rs, rz)):
        if not is_dir:
            total += size
        print(f"{'<DIR>' if is_dir else size:>12}  {path}")
    print(f"{total:>12}  bytes total")


def cmd_extract(iso, out_dir):
    f, base, rs, rz = open_volume(iso)
    for path, is_dir, sector, size in walk(f, base, rs, rz):
        dest = os.path.join(out_dir, *path.split("/"))
        if is_dir:
            os.makedirs(dest, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        f.seek(base + sector * SECTOR)
        remaining = size
        with open(dest, "wb") as o:
            while remaining:
                chunk = f.read(min(remaining, 1 << 20))
                o.write(chunk)
                remaining -= len(chunk)
        print(path)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "list":
        cmd_list(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "extract":
        cmd_extract(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(1)
