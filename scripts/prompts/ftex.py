#!/usr/bin/env python3
"""Vanillaware FTEX texture packs (Dragon's Crown Pro, PS4): list, decode, and re-encode regions.

FTEX = header, an entry table (32-byte name, u32 0, u32 section size, 8 flag bytes), then FTX0
sections (0x40 header) each holding a GNF file: 0x100 header with the texture descriptor (T#),
then the pixel data. Every texture in the game is BC7, tile mode 13 (Thin_1dThin), no mips.

    ftex.py list  pack.ftx
    ftex.py dump  pack.ftx outdir              every texture as PNG (BC7 decoded through Pillow)
"""
import io
import struct
import sys
from pathlib import Path

DFMT = {35: 'BC1', 36: 'BC2', 37: 'BC3', 38: 'BC4', 39: 'BC5', 40: 'BC6', 41: 'BC7'}
DXGI = {35: 71, 37: 77, 41: 98}  # DXGI_FORMAT_BC1_UNORM, BC3_UNORM, BC7_UNORM


def textures(blob):
    """[(name, section offset, data offset, width, height, dfmt, tiling, pitch)] of a pack."""
    count = struct.unpack_from('<I', blob, 12)[0]
    first = struct.unpack_from('<I', blob, 8)[0]
    names = [blob[0x20 + i * 0x30:0x20 + i * 0x30 + 32].split(b'\0')[0].decode() for i in range(count)]
    out = []
    pos = first
    for name in names:
        assert blob[pos:pos + 4] == b'FTX0', f'{name}: no FTX0 at {pos:#x}'
        size, hdr = struct.unpack_from('<II', blob, pos + 4)
        g = pos + hdr
        assert blob[g:g + 4] == b'GNF '
        T = struct.unpack_from('<8I', blob, g + 16)
        dfmt = (T[1] >> 20) & 0x3F
        w, h = (T[2] & 0x3FFF) + 1, ((T[2] >> 14) & 0x3FFF) + 1
        tiling = (T[3] >> 20) & 0x1F
        pitch = ((T[4] >> 13) & 0x3FFF) + 1
        out.append(dict(name=name, section=pos, data=g + 0x100, size=size - 0x100, width=w, height=h,
                        dfmt=dfmt, tiling=tiling, pitch=pitch))
        pos += hdr + size
    return out


def micro_index(x, y):
    """Element index inside an 8x8 micro tile, THIN micro-tiling (x0 y0 x1 y1 x2 y2)."""
    return ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))


def tiled_offset(bx, by, pitch_blocks, elem_bytes=16):
    """Byte offset of block (bx, by) in a Thin_1dThin surface of 16-byte elements."""
    tiles_per_row = pitch_blocks // 8
    tile = (by // 8) * tiles_per_row + (bx // 8)
    return tile * 64 * elem_bytes + micro_index(bx & 7, by & 7) * elem_bytes


def detile(data, width, height, pitch, elem_bytes=16):
    """Linear block rows from 1D-thin tiled BC data."""
    bw, bh, pb = width // 4, height // 4, pitch // 4
    out = bytearray(bw * bh * elem_bytes)
    for by in range(bh):
        row = by * bw * elem_bytes
        for bx in range(bw):
            o = tiled_offset(bx, by, pb, elem_bytes)
            out[row + bx * elem_bytes:row + (bx + 1) * elem_bytes] = data[o:o + elem_bytes]
    return bytes(out)


def dds_wrap(linear, width, height, dfmt):
    """A DDS (DX10 header) around linear BC data, for Pillow."""
    h = bytearray(b'DDS ' + struct.pack('<7I', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, height, width,
                                       len(linear), 1, 1) + b'\0' * 44)
    h += struct.pack('<II4sIIIII', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    h += struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    h += struct.pack('<5I', DXGI[dfmt], 3, 0, 1, 0)  # dxgi, D3D10_RESOURCE_DIMENSION_TEXTURE2D, misc, arraysize, misc2
    return bytes(h) + linear


def decode(blob, tex):
    from PIL import Image
    data = blob[tex['data']:tex['data'] + tex['size']]
    linear = detile(data, tex['width'], tex['height'], tex['pitch'])
    return Image.open(io.BytesIO(dds_wrap(linear, tex['width'], tex['height'], tex['dfmt']))).convert('RGBA')


def main():
    cmd, path = sys.argv[1], Path(sys.argv[2])
    blob = path.read_bytes()
    texs = textures(blob)
    if cmd == 'list':
        for t in texs:
            print(f"{t['name']:36s} {t['width']}x{t['height']} pitch={t['pitch']} {DFMT.get(t['dfmt'], t['dfmt'])} "
                  f"tiling={t['tiling']} data@{t['data']:#x} {t['size']} bytes")
    elif cmd == 'dump':
        out = Path(sys.argv[3]); out.mkdir(parents=True, exist_ok=True)
        for t in texs:
            im = decode(blob, t)
            im.save(out / (t['name'].replace('.gnf', '') + '.png'))
            print(f"{t['name']} -> {im.size}")


if __name__ == '__main__':
    main()
