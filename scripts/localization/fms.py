#!/usr/bin/env python3
"""Vanillaware FMSB message sheets (msgsheet/*.fms), the layout launcher/fms_sheet.cpp reads and writes: a header, a
pointer table the game fills in at load (zeros on disk), the records as NUL-terminated UTF-8 padded to 16 bytes, then
the FEOC trailer. A record's position is its ID.

    fms.py roundtrip <archive.CPK>...   rebuilds every sheet of the archives and compares it with the original
"""
import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'prompts'))
from cpk import Cpk  # noqa: E402

HEADER_SIZE = 0x20


@dataclass
class Sheet:
    header: bytes
    records: list
    trailer: bytes


def parse(data):
    """The sheet in `data`; ValueError when it is not one of this layout."""
    if len(data) < HEADER_SIZE or data[:4] != b'FMSB':
        raise ValueError('not an FMSB sheet')
    header, count = struct.unpack_from('<I', data, 8)[0], struct.unpack_from('<I', data, 0x14)[0]
    trailer = struct.unpack_from('<I', data, 4)[0] + HEADER_SIZE
    start = header + count * 8
    if header < HEADER_SIZE or start > trailer or trailer + 4 > len(data) or data[trailer:trailer + 4] != b'FEOC':
        raise ValueError('header offsets out of range')
    if any(data[header:start]):
        raise ValueError('pointer table not empty')
    parts = data[start:trailer].split(b'\0')
    if len(parts) <= count or any(parts[count:]):
        raise ValueError('records do not end in padding')
    return Sheet(data[:header], [p.decode('utf-8') for p in parts[:count]], data[trailer:])


def build(sheet):
    out = bytearray(sheet.header)
    out += bytes(len(sheet.records) * 8)
    for record in sheet.records:
        out += record.encode('utf-8') + b'\0'
    out += bytes(-len(out) % 16)
    struct.pack_into('<I', out, 4, len(out) - HEADER_SIZE)
    struct.pack_into('<I', out, 0x14, len(sheet.records))
    return bytes(out + sheet.trailer)


def records_of(archive, path):
    """The records of the sheet at `path` in an open Cpk; KeyError when the archive has none."""
    return parse(archive.read(archive.find(path))).records


def cmd_roundtrip(args):
    failed = total = 0
    for path in args.archives:
        archive = Cpk(path)
        for entry in archive.entries:
            if not entry['path'].lower().endswith('.fms'):
                continue
            data = archive.read(entry)
            total += 1
            try:
                same = build(parse(data)) == data
            except ValueError as e:
                same = False
                print(f'{path.name} {entry["path"]}: {e}')
            if not same:
                failed += 1
                print(f'{path.name} {entry["path"]}: rebuild differs')
    print(f'{total} sheets, {total - failed} rebuilt byte-identical')
    return 1 if failed or not total else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('roundtrip')
    p.add_argument('archives', type=Path, nargs='+')
    args = parser.parse_args()
    return cmd_roundtrip(args)


if __name__ == '__main__':
    sys.exit(main())
