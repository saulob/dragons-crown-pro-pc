#!/usr/bin/env python3
"""CRI CPK archive reader: list the table of contents, extract files. Standard library only.

    cpk.py list <file.cpk | folder>            [--grep REGEX]      one line per entry
    cpk.py extract <file.cpk> <name> <outdir>                      one file (name = path in the archive)
    cpk.py extract-matching <file.cpk> REGEX <outdir> [--max-bytes N]   every entry whose path matches
    cpk.py header <file.cpk>                                       every header field (CRC flags etc.)
    cpk.py selftest

CPK = a header @UTF table, a TOC @UTF table (DirName, FileName, FileSize, ExtractSize, FileOffset)
and the file data. Entries whose FileSize < ExtractSize are CRILAYLA-compressed.
"""
import argparse
import io
import os
import re
import struct
import sys
from pathlib import Path


# ------------------------------------------------------------------ @UTF tables

STORAGE_MASK, STORAGE_ZERO, STORAGE_CONSTANT, STORAGE_PERROW = 0xF0, 0x10, 0x30, 0x50
TYPE_MASK = 0x0F
TYPE_SIZES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4}
TYPE_FMT = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i', 6: '>Q', 7: '>q', 8: '>f'}
TYPE_STRING, TYPE_DATA = 0x0A, 0x0B


def decrypt_utf(packet):
    out = bytearray(len(packet))
    m, t = 0x5F, 0x15
    for i, b in enumerate(packet):
        out[i] = b ^ m
        m = (m * t) & 0xFF
    return bytes(out)


class UtfTable:
    """A parsed @UTF table: .name, .columns [(name, flags)], .rows [dict]."""

    def __init__(self, packet):
        self.encrypted = packet[:4] != b'@UTF'
        if self.encrypted:
            packet = decrypt_utf(packet)
            if packet[:4] != b'@UTF':
                raise ValueError('not a @UTF table (even after decryption)')
        self.raw = packet
        self.positions = {}        # (row index, column name) -> (byte offset in packet, type) for per-row fields
        (size, rows_off, strings_off, data_off, name_off, ncols, row_len, nrows) = struct.unpack_from(
            '>IIIIIHHI', packet, 4)
        base = 8
        self.rows_off, self.strings_off, self.data_off = base + rows_off, base + strings_off, base + data_off
        self.row_len = row_len

        def string_at(off):
            s = packet.index(b'\0', self.strings_off + off)
            return packet[self.strings_off + off:s].decode('utf-8', 'replace')
        self.name = string_at(name_off)
        pos = 4 + 28  # magic + size + 4 offsets + ncols + row_len + nrows
        self.columns = []
        for _ in range(ncols):
            flags = packet[pos]; pos += 1
            cname = string_at(struct.unpack_from('>I', packet, pos)[0]); pos += 4
            const = None
            storage, typ = flags & STORAGE_MASK, flags & TYPE_MASK
            if storage == STORAGE_CONSTANT:
                const, pos = self._read_value(packet, pos, typ, string_at)
            self.columns.append((cname, flags, const))
        self.rows = []
        for r in range(nrows):
            pos = self.rows_off + r * row_len
            row = {}
            for cname, flags, const in self.columns:
                storage, typ = flags & STORAGE_MASK, flags & TYPE_MASK
                if storage == STORAGE_PERROW:
                    self.positions[(r, cname)] = (pos, typ)
                    row[cname], pos = self._read_value(packet, pos, typ, string_at)
                elif storage == STORAGE_CONSTANT:
                    row[cname] = const
                else:
                    row[cname] = 0
            self.rows.append(row)

    def set_number(self, row, cname, value):
        """Overwrites a per-row numeric field in place (the packet keeps its size)."""
        pos, typ = self.positions[(row, cname)]
        if typ in (TYPE_STRING, TYPE_DATA):
            raise TypeError(f'{cname} is not numeric')
        raw = bytearray(self.raw)
        struct.pack_into(TYPE_FMT[typ], raw, pos, value)
        self.raw = bytes(raw)
        self.rows[row][cname] = value

    def packet_bytes(self):
        """The packet as it must be stored in the file (re-encrypted if it was encrypted)."""
        return decrypt_utf(self.raw) if self.encrypted else self.raw

    def _read_value(self, packet, pos, typ, string_at):
        if typ == TYPE_STRING:
            return string_at(struct.unpack_from('>I', packet, pos)[0]), pos + 4
        if typ == TYPE_DATA:
            off, ln = struct.unpack_from('>II', packet, pos)
            return packet[self.data_off + off:self.data_off + off + ln], pos + 8
        return struct.unpack_from(TYPE_FMT[typ], packet, pos)[0], pos + TYPE_SIZES[typ]


def read_packet(f, offset, magic):
    """The @UTF packet behind a 16-byte CPK section header (magic, u32, u64 size)."""
    f.seek(offset)
    head = f.read(16)
    if head[:4] != magic:
        raise ValueError(f'expected {magic!r} at {offset:#x}, found {head[:4]!r}')
    size = struct.unpack_from('<Q', head, 8)[0]
    return UtfTable(f.read(size))


# ------------------------------------------------------------------ CRILAYLA

def crilayla_decompress(data):
    """Decompresses one CRILAYLA block (as stored in a CPK entry)."""
    if data[:8] != b'CRILAYLA':
        raise ValueError('not CRILAYLA')
    out_size, header_off = struct.unpack_from('<II', data, 8)
    out = bytearray(out_size + 0x100)
    # the file's first 0x100 bytes are stored raw after the compressed stream
    out[:0x100] = data[0x10 + header_off:0x10 + header_off + 0x100]
    src = data[0x10:0x10 + header_off]
    in_pos = len(src) - 1
    bits_left = 0
    bit_buf = 0
    out_pos = out_size + 0x100 - 1
    written = 0
    levels = (2, 3, 5, 8)

    # bit reader inlined for speed: reads `n` bits MSB-first from the stream walking backwards
    def get_bits(n):
        nonlocal in_pos, bits_left, bit_buf
        value = 0
        while n:
            if bits_left == 0:
                bit_buf = src[in_pos]
                in_pos -= 1
                bits_left = 8
            take = bits_left if bits_left < n else n
            value = (value << take) | ((bit_buf >> (bits_left - take)) & ((1 << take) - 1))
            bits_left -= take
            n -= take
        return value

    while written < out_size:
        if get_bits(1):
            offset = get_bits(13) + 3
            length = 3
            level = 0
            while True:
                nbits = levels[level] if level < 4 else 8
                this = get_bits(nbits)
                length += this
                if this != (1 << nbits) - 1:
                    break
                if level < 4:
                    level += 1
            # back-reference: byte-by-byte because the source may overlap the destination
            for _ in range(length):
                out[out_pos] = out[out_pos + offset]
                out_pos -= 1
            written += length
        else:
            out[out_pos] = get_bits(8)
            out_pos -= 1
            written += 1
    return bytes(out)


# ------------------------------------------------------------------ CPK

class Cpk:
    def __init__(self, path):
        self.path = Path(path)
        self.f = open(path, 'rb')
        self.header = read_packet(self.f, 0, b'CPK ')
        h = self.header.rows[0]
        self.toc_offset = h.get('TocOffset') or 0
        self.content_offset = h.get('ContentOffset') or 0
        self.align = h.get('Align') or 0x800
        self.file_count = h.get('Files')
        self.entries = []
        self.toc = None
        self.toc_base = 0
        if self.toc_offset:
            toc = self.toc = read_packet(self.f, self.toc_offset, b'TOC ')
            # FileOffset is relative to the lower of ContentOffset and TocOffset (TocOffset counted as at
            # most 0x800), as CriPakTools resolves it.
            toc_base = min(self.toc_offset, 0x800)
            base = self.toc_base = toc_base if not self.content_offset else min(self.content_offset, toc_base)
            for row_index, r in enumerate(toc.rows):
                d, n = r.get('DirName', ''), r.get('FileName', '')
                self.entries.append(dict(path=(d + '/' + n) if d else n, size=r['FileSize'],
                                         extract_size=r.get('ExtractSize', r['FileSize']),
                                         offset=base + r['FileOffset'], id=r.get('ID', 0),
                                         user=r.get('UserString', ''), row=row_index))

    def read(self, entry):
        self.f.seek(entry['offset'])
        data = self.f.read(entry['size'])
        if entry['size'] < entry['extract_size'] or data[:8] == b'CRILAYLA':
            data = crilayla_decompress(data)
        return data

    def find(self, name):
        name = name.replace('\\', '/').lower()
        for e in self.entries:
            if e['path'].lower() == name:
                return e
        hits = [e for e in self.entries if e['path'].lower().endswith('/' + name) or e['path'].lower() == name]
        if len(hits) == 1:
            return hits[0]
        raise KeyError(f'{name}: {"not found" if not hits else "ambiguous: " + ", ".join(h["path"] for h in hits)}')


def cmd_list(args):
    paths = [Path(args.target)]
    if paths[0].is_dir():
        paths = sorted(p for p in paths[0].iterdir() if p.suffix.lower() == '.cpk')
    rx = re.compile(args.grep, re.I) if args.grep else None
    for p in paths:
        cpk = Cpk(p)
        h = cpk.header.rows[0]
        print(f'=== {p}  files={cpk.file_count} toc@{cpk.toc_offset:#x} content@{cpk.content_offset:#x} align={cpk.align} '
              f'{"itoc" if h.get("ItocOffset") else ""} {"etoc" if h.get("EtocOffset") else ""}')
        print(f'    columns: {", ".join(c for c, _, _ in cpk.header.columns)}')
        total = comp = 0
        for e in cpk.entries:
            if rx and not rx.search(e['path']):
                continue
            z = 'z' if e['size'] < e['extract_size'] else ' '
            comp += z == 'z'; total += 1
            print(f'{e["id"]:6d} {z} {e["size"]:>10d} {e["extract_size"]:>10d} {e["offset"]:#12x}  {e["path"]}')
        print(f'    {total} entries listed, {comp} compressed')


def cmd_header(args):
    cpk = Cpk(args.cpk)
    h = cpk.header
    print(f'{args.cpk}: header packet {"encrypted" if h.encrypted else "plain"}, {len(h.raw)} bytes; '
          f'TOC packet {"encrypted" if cpk.toc.encrypted else "plain"}, {len(cpk.toc.raw)} bytes at {cpk.toc_offset:#x}')
    for cname, flags, const in h.columns:
        v = h.rows[0][cname]
        if isinstance(v, bytes):
            v = v.hex() if len(v) <= 32 else f'<{len(v)} bytes>'
        print(f'  {cname:20s} = {v!r}   (flags {flags:#04x})')


def cmd_extract(args):
    cpk = Cpk(args.cpk)
    e = cpk.find(args.name)
    out = Path(args.outdir) / Path(e['path']).name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(cpk.read(e))
    print(f'{e["path"]} -> {out} ({out.stat().st_size} bytes)')


def cmd_extract_matching(args):
    cpk = Cpk(args.cpk)
    rx = re.compile(args.regex, re.I)
    n = 0
    for e in cpk.entries:
        if rx.search(e['path']) and e['extract_size'] <= args.max_bytes:
            out = Path(args.outdir) / e['path']
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(cpk.read(e))
            n += 1
            print(f'{e["path"]} ({e["extract_size"]} bytes)')
    print(f'{n} files -> {args.outdir}')


# ------------------------------------------------------------------ self-test: a synthetic CPK

def _utf(name, columns, rows):
    """Builds a @UTF packet. columns: [(name, type, storage)], rows: [dict]."""
    strings = bytearray(b'<NULL>\0')
    data = bytearray()

    def s(text):
        off = len(strings); strings.extend(text.encode() + b'\0'); return off
    name_off = s(name)
    cols = bytearray()
    for cname, typ, storage in columns:
        cols.append(storage | typ); cols += struct.pack('>I', s(cname))
        if storage == STORAGE_CONSTANT:
            cols += struct.pack(TYPE_FMT[typ], rows[0][cname])
    rowbuf = bytearray()
    row_len = 0
    for r in rows:
        start = len(rowbuf)
        for cname, typ, storage in columns:
            if storage != STORAGE_PERROW:
                continue
            v = r[cname]
            if typ == TYPE_STRING:
                rowbuf += struct.pack('>I', s(v))
            elif typ == TYPE_DATA:
                rowbuf += struct.pack('>II', len(data), len(v)); data += v
            else:
                rowbuf += struct.pack(TYPE_FMT[typ], v)
        row_len = len(rowbuf) - start
    body_head = 4 + 28
    rows_off = body_head + len(cols) - 8
    strings_off = rows_off + len(rowbuf)
    data_off = strings_off + len(strings)
    hdr = struct.pack('>IIIIIHHI', 0, rows_off, strings_off, data_off, name_off, len(columns), row_len, len(rows))
    pkt = bytearray(b'@UTF') + hdr + cols + rowbuf + strings + data
    struct.pack_into('>I', pkt, 4, len(pkt) - 8)
    return bytes(pkt)


def _section(magic, pkt):
    return magic + struct.pack('<I', 0xFF) + struct.pack('<Q', len(pkt)) + pkt


def _crilayla_literals(payload):
    """A CRILAYLA block that stores `payload` with literal bytes only (tests the bit reader).
    The file's first 0x100 bytes are stored raw after the stream; the rest is the stream."""
    head, body = payload[:0x100], payload[0x100:]
    bits = []
    for b in reversed(body):   # the stream is read backwards and writes the output backwards
        bits.append(0)
        bits += [(b >> i) & 1 for i in range(7, -1, -1)]
    while len(bits) % 8:
        bits.append(0)
    stream = bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
    stream = stream[::-1]
    return b'CRILAYLA' + struct.pack('<II', len(body), len(stream)) + stream + head


def selftest():
    files = [('ui/prompt.gnf', b'GNF ' + bytes(range(256)) * 4), ('ui/text.bin', b'hello cpk\0' * 40 + b'x' * 0x100)]
    content = bytearray()
    rows = []
    for i, (p, d) in enumerate(files):
        stored = d if i == 0 else _crilayla_literals(d)
        while len(content) % 0x10:
            content.append(0)
        rows.append(dict(DirName=p.rsplit('/', 1)[0], FileName=p.rsplit('/', 1)[1], FileSize=len(stored),
                         ExtractSize=len(d), FileOffset=0x1000 - 0x800 + len(content), ID=i, UserString='<NULL>'))
        content += stored
    toc = _section(b'TOC ', _utf('CpkTocInfo', [('DirName', TYPE_STRING, STORAGE_PERROW), ('FileName', TYPE_STRING, STORAGE_PERROW),
                                               ('FileSize', 4, STORAGE_PERROW), ('ExtractSize', 4, STORAGE_PERROW),
                                               ('FileOffset', 6, STORAGE_PERROW), ('ID', 4, STORAGE_PERROW),
                                               ('UserString', TYPE_STRING, STORAGE_PERROW)], rows))
    toc_offset, content_offset = 0x800, 0x1000
    header_pkt = _utf('CpkHeader', [('TocOffset', 6, STORAGE_PERROW), ('ContentOffset', 6, STORAGE_PERROW),
                                    ('Files', 4, STORAGE_PERROW), ('Align', 2, STORAGE_PERROW), ('Version', 2, STORAGE_CONSTANT)],
                      [dict(TocOffset=toc_offset, ContentOffset=content_offset, Files=len(files), Align=16, Version=7)])
    blob = bytearray(_section(b'CPK ', decrypt_utf(header_pkt)))   # encrypted header, as real CPKs have
    blob += b'\0' * (toc_offset - len(blob)); blob += toc
    blob += b'\0' * (content_offset - len(blob)); blob += content
    tmp = Path(os.environ.get('TEMP', '/tmp')) / 'cpk_selftest.cpk'
    tmp.write_bytes(blob)
    cpk = Cpk(tmp)
    assert [e['path'] for e in cpk.entries] == [p for p, _ in files], cpk.entries
    assert cpk.header.rows[0]['Version'] == 7 and cpk.align == 16
    for (p, d), e in zip(files, cpk.entries):
        got = cpk.read(e)
        assert got == d, (p, len(got), len(d))
    print(f'selftest ok: {len(files)} entries, encrypted header, CRILAYLA literal stream')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list'); p.add_argument('target'); p.add_argument('--grep')
    p = sub.add_parser('header'); p.add_argument('cpk')
    p = sub.add_parser('extract'); p.add_argument('cpk'); p.add_argument('name'); p.add_argument('outdir')
    p = sub.add_parser('extract-matching'); p.add_argument('cpk'); p.add_argument('regex'); p.add_argument('outdir')
    p.add_argument('--max-bytes', type=int, default=8 << 20)
    sub.add_parser('selftest')
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(errors='replace')
    except (AttributeError, ValueError):
        pass
    if args.cmd == 'selftest':
        selftest()
    elif args.cmd == 'list':
        cmd_list(args)
    elif args.cmd == 'header':
        cmd_header(args)
    elif args.cmd == 'extract':
        cmd_extract(args)
    else:
        cmd_extract_matching(args)


if __name__ == '__main__':
    main()
