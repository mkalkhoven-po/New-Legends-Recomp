"""Tile the smoke test screenshots into contact sheets.

Usage: python tools/contact_sheet.py [--columns N] [--per-sheet N] [png ...]

Reads the unfiltered 24-bit PNGs bmp2png.py writes (default smoke/*.png) and
writes smoke/sheets/sheet_NN.png at half size, in order, left to right.
"""
import argparse
import glob
import os
import struct
import zlib


def read_png(path):
    data = open(path, "rb").read()
    pos, idat = 8, b""
    w = h = 0
    while pos < len(data):
        n, tag = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8 : pos + 8 + n]
        if tag == b"IHDR":
            w, h = struct.unpack_from(">II", body)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    return w, h, [raw[y * stride + 1 : (y + 1) * stride] for y in range(h)]


def half(rows):
    out = []
    for y in range(0, len(rows) - 1, 2):
        row = rows[y]
        px = bytearray()
        for x in range(0, len(row) - 3, 6):
            px += row[x : x + 3]
        out.append(bytes(px))
    return out


def write_png(path, w, rows):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, len(rows), 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(b"".join(b"\0" + r for r in rows), 6)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--columns", type=int, default=3)
    ap.add_argument("--per-sheet", type=int, default=9)
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()
    files = args.files or sorted(glob.glob("smoke/*.png"))
    os.makedirs("smoke/sheets", exist_ok=True)
    for sheet in range(0, len(files), args.per_sheet):
        tiles = [half(read_png(p)[2]) for p in files[sheet : sheet + args.per_sheet]]
        tile_w, tile_h = len(tiles[0][0]) // 3, len(tiles[0])
        rows = []
        for r in range(0, len(tiles), args.columns):
            line = tiles[r : r + args.columns]
            line += [[bytes(tile_w * 3)] * tile_h] * (args.columns - len(line))
            for y in range(tile_h):
                rows.append(b"".join(t[y] for t in line))
        out = f"smoke/sheets/sheet_{sheet // args.per_sheet:02d}.png"
        write_png(out, tile_w * args.columns, rows)
        print(out, [os.path.basename(p)[:-4] for p in files[sheet : sheet + args.per_sheet]])


if __name__ == "__main__":
    main()
