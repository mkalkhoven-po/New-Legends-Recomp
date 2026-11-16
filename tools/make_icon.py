"""Draw the program icon (our own design, no game artwork) and write res/newlegends.ico.

A gold ring with a two-tier pagoda roof on a dark red rounded square.
Pure standard library: shapes are tested per pixel with 4x4 supersampling and
every icon size is stored as a PNG inside the .ico.

Usage: python tools/make_icon.py
"""
import math
import os
import struct
import zlib

BG_TOP = (122, 28, 28)
BG_BOTTOM = (54, 10, 14)
GOLD = (232, 182, 72)
GOLD_DARK = (176, 120, 36)


def rounded_square(x, y, r=0.18):
    # Unit square [0,1]^2 with rounded corners of radius r.
    cx = min(max(x, r), 1 - r)
    cy = min(max(y, r), 1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and 0 <= x <= 1 and 0 <= y <= 1


def ring(x, y):
    d = math.hypot(x - 0.5, y - 0.5)
    return 0.34 <= d <= 0.42


def roof(x, y, top, width, height):
    # A roof tier: highest in the middle, sloping down, eaves flicking up.
    dx = abs(x - 0.5)
    if dx > width / 2:
        return False
    t = dx / (width / 2)
    edge = top + height * (0.8 * t * t - 6 * max(0.0, t - 0.75) ** 2)
    return edge <= y <= edge + height * (0.8 - 0.3 * t)


def pagoda(x, y):
    if roof(x, y, 0.33, 0.46, 0.13):
        return True
    if roof(x, y, 0.47, 0.58, 0.14):
        return True
    # Spire and the walls between the tiers.
    if abs(x - 0.5) <= 0.022 and 0.25 <= y <= 0.36:
        return True
    if abs(x - 0.5) <= 0.10 and 0.42 <= y <= 0.50:
        return True
    if abs(x - 0.5) <= 0.13 and 0.57 <= y <= 0.70:
        # A doorway keeps the base from being a plain block.
        return not (abs(x - 0.5) <= 0.035 and y >= 0.61)
    return False


def pixel(x, y):
    """Colour (r, g, b, a) at unit coordinates."""
    if not rounded_square(x, y):
        return None
    if ring(x, y) or pagoda(x, y):
        t = min(max((y - 0.15) / 0.7, 0.0), 1.0)
        return tuple(int(GOLD[i] * (1 - t) + GOLD_DARK[i] * t) for i in range(3)) + (255,)
    t = y
    return tuple(int(BG_TOP[i] * (1 - t) + BG_BOTTOM[i] * t) for i in range(3)) + (255,)


def render(size, ss=4):
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    c = pixel((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    if c:
                        for i in range(3):
                            acc[i] += c[i] * c[3]
                        acc[3] += c[3]
            a = acc[3] / (ss * ss)
            if acc[3]:
                row += bytes(int(acc[i] / acc[3]) for i in range(3)) + bytes([int(a)])
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "res")
    os.makedirs(out_dir, exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 256]
    images = [png(s, render(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(os.path.join(out_dir, "newlegends.ico"), "wb") as f:
        f.write(header + entries + b"".join(images))
    with open(os.path.join(out_dir, "icon_preview.png"), "wb") as f:
        f.write(images[-1])
    print("wrote res/newlegends.ico and res/icon_preview.png")


if __name__ == "__main__":
    main()
