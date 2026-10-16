#!/usr/bin/env python3
"""Builds a DCPX block patch from pairs of (original, patched) files: the 16-byte runs that differ,
with CRC32s of both versions so the patcher can tell untouched from already-patched data.

    make_dcpx.py out.dcpx  archive/path=orig.bin:patched.bin  [...]
"""
import zlib
import struct
import sys
from pathlib import Path


def main():
    out = Path(sys.argv[1])
    targets = []
    for spec in sys.argv[2:]:
        name, files = spec.split('=', 1)
        orig_p, new_p = files.split(':', 1)
        orig, new = Path(orig_p).read_bytes(), Path(new_p).read_bytes()
        assert len(orig) == len(new), name
        runs = [(i, new[i:i + 16]) for i in range(0, len(orig), 16) if orig[i:i + 16] != new[i:i + 16]]
        targets.append((name, len(orig), zlib.crc32(orig), zlib.crc32(new), runs))
        print(f'{name}: {len(runs)} changed 16-byte runs of {len(orig) // 16}')
    buf = bytearray(b'DCPX1\0\0\0' + struct.pack('<I', len(targets)))
    for name, size, h0, h1, runs in targets:
        nb = name.encode()
        buf += struct.pack('<H', len(nb)) + nb + struct.pack('<III', size, h0, h1) + struct.pack('<I', len(runs))
        for off, data in runs:
            buf += struct.pack('<I', off) + data
    out.write_bytes(buf)
    print(f'-> {out} ({len(buf)} bytes)')


if __name__ == '__main__':
    main()
