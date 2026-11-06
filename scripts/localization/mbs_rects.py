#!/usr/bin/env python3
"""Texture rectangles an FMBS layout cuts out of a 2048-wide atlas.

    mbs_rects.py FILE.mbs [x0 y0 x1 y1]    rectangles inside the box (all when no box)

The layout keeps its texture coordinates as float (u, v) pairs from 0x2070 up to the section that follows (the
offset is in the header); a sprite quad appears there as a centre point followed by its four corners.
"""
import struct
import sys
from pathlib import Path

ATLAS = 2048
UV_TABLE_HEADER_FIELD = 0xB0 + 4 * 2  # second section offset in the header


def uv_points(data):
    start = struct.unpack_from('<I', data, UV_TABLE_HEADER_FIELD)[0]
    end = struct.unpack_from('<I', data, UV_TABLE_HEADER_FIELD + 8)[0]
    count = (end - start) // 8
    return [tuple(round(v * ATLAS, 1) for v in struct.unpack_from('<2f', data, start + 8 * i)) for i in range(count)]


def rectangles(data):
    """{(x0, y0, x1, y1): [first uv index, ...]} for every quad of at least 40 x 30 texels."""
    points = uv_points(data)
    found = {}
    for i in range(len(points) - 5):
        unique = set(points[i:i + 6])
        xs = [p[0] for p in unique]
        ys = [p[1] for p in unique]
        box = (min(xs), min(ys), max(xs), max(ys))
        if box[2] - box[0] < 40 or box[3] - box[1] < 30 or len(unique) > 5:
            continue
        if {(box[0], box[1]), (box[0], box[3]), (box[2], box[1]), (box[2], box[3])} <= unique:
            found.setdefault(box, []).append(i)
    return found


def position_start(data):
    return struct.unpack_from('<I', data, UV_TABLE_HEADER_FIELD + 8)[0]


def position_at(data, index):
    return struct.unpack_from('<2f', data, position_start(data) + 8 * index)


def position_quads(data, size_hint=None):
    """{(x0, y0, x1, y1): first position index} for every quad of the position table (centre point, four corners)."""
    start = position_start(data)
    end = struct.unpack_from('<I', data, UV_TABLE_HEADER_FIELD + 16)[0]
    points = [struct.unpack_from('<2f', data, start + 8 * i) for i in range(min((end - start) // 8, 15552))]
    found = {}
    for i in range(len(points) - 5):
        unique = set(points[i + 1:i + 6])
        xs = [p[0] for p in unique]
        ys = [p[1] for p in unique]
        box = (min(xs), min(ys), max(xs), max(ys))
        if len(unique) == 4 and {(box[0], box[1]), (box[0], box[3]), (box[2], box[1]), (box[2], box[3])} == unique:
            found.setdefault(box, i)
    return found


def main():
    data = Path(sys.argv[1]).read_bytes()
    if data[:4] != b'FMBS':
        raise SystemExit('not an FMBS layout')
    box = tuple(float(v) for v in sys.argv[2:6]) if len(sys.argv) >= 6 else None
    for rect, indices in sorted(rectangles(data).items(), key=lambda kv: (kv[0][1], kv[0][0])):
        if box and not (rect[0] >= box[0] and rect[1] >= box[1] and rect[2] <= box[2] and rect[3] <= box[3]):
            continue
        print(rect, indices[:4])


if __name__ == '__main__':
    main()
