"""Builds assets/comms/icons.png: the eight Comms Wheel icons (assets/comms/*.svg) as one white-on-transparent atlas,
4 columns by 2 rows of 128 px cells in message id order. Needs rsvg-convert on PATH or given with --rsvg."""
import argparse
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ICONS = ["hello", "thanks", "good-job", "follow-me", "group-up", "need-help", "ready", "lets-go"]
CELL, COLUMNS, MARGIN = 128, 4, 10


def read_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, header = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    width, height, depth, color, _, _, interlace = header
    assert depth == 8 and color == 6 and interlace == 0, "expected 8-bit RGBA"
    raw = zlib.decompress(idat)
    stride = width * 4
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = previous[i]
            c = previous[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        previous = line
    return width, height, rows


def write_png(path, width, height, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rsvg", default="rsvg-convert")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent / "assets" / "comms"
    rows_total = (len(ICONS) + COLUMNS - 1) // COLUMNS
    atlas = [bytearray(CELL * COLUMNS * 4) for _ in range(CELL * rows_total)]
    inner = CELL - 2 * MARGIN
    with tempfile.TemporaryDirectory() as tmp:
        for index, name in enumerate(ICONS):
            out = Path(tmp) / (name + ".png")
            subprocess.run([args.rsvg, "-w", str(inner), "-h", str(inner), "--keep-aspect-ratio", "-o", str(out),
                            str(root / (name + ".svg"))], check=True)
            width, height, rows = read_png(out.read_bytes())
            ox = (index % COLUMNS) * CELL + (CELL - width) // 2
            oy = (index // COLUMNS) * CELL + (CELL - height) // 2
            for y in range(height):
                for x in range(width):
                    alpha = rows[y][x * 4 + 3]
                    pixel = atlas[oy + y]
                    base = (ox + x) * 4
                    pixel[base:base + 4] = bytes((255, 255, 255, alpha))
    write_png(root / "icons.png", CELL * COLUMNS, CELL * rows_total, atlas)


if __name__ == "__main__":
    sys.exit(main())
