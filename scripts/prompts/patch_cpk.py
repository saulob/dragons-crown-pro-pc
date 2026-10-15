#!/usr/bin/env python3
"""Applies a DCPX block patch to files inside CRI CPK archives, in place and reversibly.
Standard library only (needs cpk.py next to it).

    patch_cpk.py apply  <folder with .cpk files> <patch.dcpx>
    patch_cpk.py revert <folder with .cpk files>
    patch_cpk.py status <folder with .cpk files>

For every archive entry named in the patch: the entry is read (decompressed if needed), checked
against the patch's hash of the original, patched, appended uncompressed to the end of the archive
and its TOC row repointed. Nothing in the original data is overwritten; `revert` restores the TOC
bytes and truncates the archive back to its original length. The undo record lives next to the
archive as <name>.cpk.undo.
"""
import os
import struct
import sys
import zlib
from pathlib import Path

try:
    _here = Path(__file__).resolve().parent
except NameError:                      # run through an embedded interpreter that sets no __file__
    _here = Path(sys.argv[0]).resolve().parent if sys.argv and sys.argv[0] else Path.cwd()
sys.path[:0] = [str(_here), os.getcwd()]
import cpk as cpklib

UNDO_SUFFIX = '.undo'
UNDO_MAGIC = b'DCPUNDO1'


def read_dcpx(path):
    b = Path(path).read_bytes()
    assert b[:8] == b'DCPX1\0\0\0', 'not a DCPX patch'
    n = struct.unpack_from('<I', b, 8)[0]
    pos = 12
    targets = {}
    for _ in range(n):
        ln = struct.unpack_from('<H', b, pos)[0]; pos += 2
        name = b[pos:pos + ln].decode(); pos += ln
        size, h0, h1 = struct.unpack_from('<III', b, pos); pos += 12
        nr = struct.unpack_from('<I', b, pos)[0]; pos += 4
        runs = []
        for _ in range(nr):
            off = struct.unpack_from('<I', b, pos)[0]
            runs.append((off, b[pos + 4:pos + 20])); pos += 20
        targets[name.lower()] = dict(name=name, size=size, sha_orig=h0, sha_new=h1, runs=runs)
    return targets


def cpk_files(folder):
    folder = Path(folder)
    if folder.is_file():
        return [folder]
    return sorted(p for p in folder.iterdir() if p.suffix.lower() == '.cpk')


def undo_path(cpk_path):
    return cpk_path.with_name(cpk_path.name + UNDO_SUFFIX)


def apply_one(cpk_path, targets):
    undo = undo_path(cpk_path)
    if undo.exists():
        print(f'{cpk_path.name}: already patched (undo record present) - run revert first to re-apply')
        return
    arc = cpklib.Cpk(cpk_path)
    hits = [e for e in arc.entries if e['path'].lower() in targets]
    if not hits:
        print(f'{cpk_path.name}: nothing to patch')
        arc.f.close()
        return
    h = arc.header.rows[0]
    for flag in ('EnableTocCrc', 'EnableFileCrc'):
        if h.get(flag):
            print(f'{cpk_path.name}: header has {flag}={h[flag]}; it will be cleared so the rewritten TOC is accepted')
    toc_pos = arc.toc_offset + 16
    toc_len = len(arc.toc.raw)
    arc.f.seek(toc_pos)
    original_toc = arc.f.read(toc_len)
    arc.f.seek(16)
    original_header = arc.f.read(len(arc.header.raw))
    original_len = cpk_path.stat().st_size
    align = arc.align or 0x800

    patched = []   # (entry, data)
    for e in hits:
        t = targets[e['path'].lower()]
        print(f'{cpk_path.name}: {e["path"]} id={e["id"]} reading {e["size"]} bytes'
              f'{" (compressed)" if e["size"] < e["extract_size"] else ""} ...', flush=True)
        data = arc.read(e)
        crc = zlib.crc32(data)
        if len(data) == t['size'] and crc == t['sha_new']:
            print(f'  already carries the patch, skipping'); continue
        if len(data) != t['size'] or crc != t['sha_orig']:
            print(f'  does not match the file this patch was made for (size {len(data)}, crc32 {crc:08x}), skipping')
            continue
        buf = bytearray(data)
        for off, run in t['runs']:
            buf[off:off + 16] = run
        if zlib.crc32(buf) != t['sha_new']:
            print(f'  patched data hash mismatch, skipping'); continue
        patched.append((e, bytes(buf)))
    arc.f.close()
    if not patched:
        print(f'{cpk_path.name}: no changes written')
        return

    names = '\n'.join(e['path'] for e, _ in patched).encode()
    undo.write_bytes(UNDO_MAGIC + struct.pack('<QQI', original_len, toc_pos, len(original_toc)) + original_toc
                     + struct.pack('<QI', 16, len(original_header)) + original_header + struct.pack('<I', len(names)) + names)
    with open(cpk_path, 'r+b') as f:
        pos = original_len
        for e, data in patched:
            pos = (pos + align - 1) // align * align
            f.seek(pos); f.write(data)
            arc.toc.set_number(e['row'], 'FileOffset', pos - arc.toc_base)
            arc.toc.set_number(e['row'], 'FileSize', len(data))
            if ('ExtractSize' in arc.toc.rows[e['row']]) and ((e['row'], 'ExtractSize') in arc.toc.positions):
                arc.toc.set_number(e['row'], 'ExtractSize', len(data))
            print(f'  {e["path"]}: {len(data)} bytes stored at {pos:#x}')
            pos += len(data)
        new_toc = arc.toc.packet_bytes()
        assert len(new_toc) == toc_len
        f.seek(toc_pos); f.write(new_toc)
        for flag in ('EnableTocCrc', 'EnableFileCrc'):
            if h.get(flag) and (0, flag) in arc.header.positions:
                arc.header.set_number(0, flag, 0)
        new_header = arc.header.packet_bytes()
        assert len(new_header) == len(original_header)
        f.seek(16); f.write(new_header)
    # verify by re-reading through the normal path
    arc2 = cpklib.Cpk(cpk_path)
    for e, data in patched:
        e2 = next(x for x in arc2.entries if x['path'] == e['path'])
        got = arc2.read(e2)
        ok = zlib.crc32(got) == targets[e['path'].lower()]['sha_new'] and len(got) == len(data)
        print(f'  verify {e["path"]}: {"ok" if ok else "FAILED"}')
    arc2.f.close()
    print(f'{cpk_path.name}: patched {len(patched)} file(s); undo record: {undo.name}')


def read_undo(undo):
    b = undo.read_bytes()
    assert b[:8] == UNDO_MAGIC, f'{undo}: not an undo record'
    original_len, toc_pos, toc_len = struct.unpack_from('<QQI', b, 8); pos = 28
    toc = b[pos:pos + toc_len]; pos += toc_len
    header_pos, hdr_len = struct.unpack_from('<QI', b, pos); pos += 12
    header = b[pos:pos + hdr_len]; pos += hdr_len
    n = struct.unpack_from('<I', b, pos)[0]; pos += 4
    names = b[pos:pos + n].decode().split('\n')
    return dict(original_len=original_len, toc_pos=toc_pos, toc=toc, header_pos=header_pos, header=header, entries=names)


def revert_one(cpk_path):
    undo = undo_path(cpk_path)
    if not undo.exists():
        return
    rec = read_undo(undo)
    with open(cpk_path, 'r+b') as f:
        f.seek(rec['toc_pos']); f.write(rec['toc'])
        f.seek(rec['header_pos']); f.write(rec['header'])
        f.truncate(rec['original_len'])
    undo.unlink()
    print(f'{cpk_path.name}: reverted ({", ".join(rec["entries"])})')


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('apply', 'revert', 'status'):
        print(__doc__); sys.exit(2)
    cmd, folder = sys.argv[1], Path(sys.argv[2])
    files = cpk_files(folder)
    if not files:
        print(f'no .cpk files in {folder}'); sys.exit(1)
    if cmd == 'apply':
        targets = read_dcpx(sys.argv[3])
        print(f'patch covers: {", ".join(t["name"] for t in targets.values())}')
        for p in files:
            apply_one(p, targets)
    elif cmd == 'revert':
        for p in files:
            revert_one(p)
    else:
        for p in files:
            u = undo_path(p)
            if u.exists():
                rec = read_undo(u)
                print(f'{p.name}: PATCHED ({", ".join(rec["entries"])}; +{p.stat().st_size - rec["original_len"]} bytes)')
            else:
                print(f'{p.name}: original')


if __name__ == '__main__':
    main()
