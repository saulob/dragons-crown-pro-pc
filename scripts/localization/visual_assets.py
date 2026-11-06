#!/usr/bin/env python3
"""Build and validate the PT-BR visual replacements from Marcelo's final game files.

    visual_assets.py build     writes localization/pt-BR/visual/*.dcpx and the ignored report
    visual_assets.py validate  checks the patches, the reference files and the USA source entries
    visual_assets.py inventory lists the reference asset families
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts' / 'prompts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cpk import Cpk, UtfTable, crilayla_decompress  # noqa: E402

ARCHIVE = REPO / 'game' / 'prepared' / 'v1.03' / 'DRACRO_US.CPK'
UNDO = Path(str(ARCHIVE) + '.undo')
REFERENCE = REPO / 'cache' / 'localization' / 'pt-BR' / 'reference' / 'marcelo' / 'completo'
FINAL = REFERENCE / '2_arquivos_finais_do_jogo'
IMAGES = REFERENCE / '3_imagens'
VISUAL = REPO / 'localization' / 'pt-BR' / 'visual'
REPORT = REPO / 'cache' / 'localization' / 'pt-BR' / 'work' / 'visual-assets-report.json'

INTERFACE_FR = FINAL / 'stage' / 'Interface_FR.ftx'
INTERFACE_FR_MBS = FINAL / 'stage' / 'Interface_FR.mbs'
TITLE_FR = FINAL / 'stage' / 'title_FR.ftx'
TITLE_FR_MBS = FINAL / 'stage' / 'title_FR.mbs'
MANUALS = [(FINAL / 'other' / f'FR_manual_{i:03d}.ftx', f'other/US_manual_{i:03d}.ftx') for i in range(1, 12)]

# The French layouts are the layouts Marcelo selected for the Portuguese labels. They are read from the user's own
# archive at runtime; only the embedded file name is changed, so no original layout data is distributed.
MBS_MIRRORS = [
    {'source': 'stage/Interface_FR.mbs', 'target': 'stage/Interface_US.mbs', 'name': 'Interface_FR.mbs',
     'sha256': 'f8e74c3d05587048439a0e62679281df83659a6cb55af0ec71bc9a4d95623458'},
    {'source': 'stage/title_FR.mbs', 'target': 'stage/title_US.mbs', 'name': 'title_FR.mbs',
     'sha256': 'f9728947b789454b2d096a6a0cb607c29d763d8a8f85e0063c482f220bf70505'},
]
INTERFACE_SHA256 = 'f1ad6a3c8b6210d94d87434f981b113c1b771d4c09fdd97b5eb6fa0328b536db'
INTERFACE_CRC32 = 0x278b74fb


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


def read_archive(cpk, path):
    entry = cpk.find(path)
    if entry is None:
        raise SystemExit(f'{path}: not in the USA archive')
    return cpk.read(entry)


def original_entries(cpk):
    if not UNDO.is_file():
        return {}
    raw = UNDO.read_bytes()
    if raw[:8] != b'DCPUNDO1':
        raise SystemExit(f'{UNDO}: not an undo record')
    toc_len = struct.unpack_from('<I', raw, 24)[0]
    return {row['DirName'] + '/' + row['FileName']: row for row in UtfTable(raw[28:28 + toc_len]).rows}


def original_data(cpk, rows, path):
    row = rows.get(path)
    if row is None:
        return read_archive(cpk, path)
    base = min(cpk.toc_offset, 0x800)
    if cpk.content_offset:
        base = min(cpk.content_offset, base)
    raw = ARCHIVE.read_bytes()[base + row['FileOffset']:base + row['FileOffset'] + row['FileSize']]
    if row['FileSize'] < row['ExtractSize'] or raw[:8] == b'CRILAYLA':
        raw = crilayla_decompress(raw)
    return raw


def ftx_structure(data):
    if data[:4] != b'FTEX':
        raise ValueError('not an FTX file')
    count = struct.unpack_from('<I', data, 12)[0]
    names = []
    for i in range(count):
        raw = data[32 + i * 48:32 + (i + 1) * 48].split(b'\0', 1)[0]
        names.append(raw.decode('ascii'))
    positions = []
    start = 0
    while True:
        at = data.find(b'GNF ', start)
        if at < 0:
            break
        positions.append(at)
        start = at + 4
    blocks = [(a, b - a) for a, b in zip(positions, positions[1:] + [len(data)])]
    if len(blocks) != count:
        raise ValueError(f'{count} names but {len(blocks)} GNF blocks')
    return {'count': count, 'names': names, 'blocks': blocks}


def portuguese_interface():
    import treasure_rank
    return treasure_rank.apply(INTERFACE_FR.read_bytes())


def portuguese_layout(mirror, data):
    import treasure_rank
    if mirror['target'] != 'stage/Interface_US.mbs':
        return data
    edited = bytearray(data)
    for offset, old, new in treasure_rank.layout_edits(data, treasure_rank.layout_shift(INTERFACE_FR.read_bytes())):
        if data[offset:offset + 4] != old:
            raise SystemExit(f'{mirror["source"]}: the Treasure Rank layout edit does not match its source')
        edited[offset:offset + 4] = new
    return bytes(edited)


def dcpx_bytes(name, source, target):
    if len(source) != len(target):
        raise ValueError(f'{name}: {len(source)} -> {len(target)} bytes')
    runs = [(i, target[i:i + 16]) for i in range(0, len(source), 16) if source[i:i + 16] != target[i:i + 16]]
    out = bytearray(b'DCPX1\0\0\0' + struct.pack('<I', 1))
    encoded = name.encode('ascii')
    out += struct.pack('<H', len(encoded)) + encoded
    out += struct.pack('<III', len(source), crc32(source), crc32(target))
    out += struct.pack('<I', len(runs))
    for offset, data in runs:
        out += struct.pack('<I', offset) + data
    return bytes(out), len(runs)


def parse_dcpx(data):
    if data[:8] != b'DCPX1\0\0\0':
        raise ValueError('not a DCPX patch')
    count = struct.unpack_from('<I', data, 8)[0]
    pos = 12
    targets = []
    for _ in range(count):
        length = struct.unpack_from('<H', data, pos)[0]
        pos += 2
        name = data[pos:pos + length].decode('ascii')
        pos += length
        size, before, after, runs = struct.unpack_from('<IIII', data, pos)
        pos += 16
        entries = []
        for _ in range(runs):
            offset = struct.unpack_from('<I', data, pos)[0]
            entries.append((offset, data[pos + 4:pos + 20]))
            pos += 20
        targets.append({'name': name, 'size': size, 'before': before, 'after': after, 'runs': entries})
    return targets


def apply_patch(source, target):
    if len(source) != target['size'] or crc32(source) != target['before']:
        return None
    out = bytearray(source)
    for offset, data in target['runs']:
        if offset + 16 > len(out):
            return None
        out[offset:offset + 16] = data
    return bytes(out) if crc32(out) == target['after'] else None


def pack_patch(data):
    return struct.pack('<I', len(data)) + zlib.compress(data, 9)


def unpack_patch(data):
    if len(data) < 4:
        raise ValueError('visual patch is truncated')
    size = struct.unpack_from('<I', data, 0)[0]
    out = zlib.decompress(data[4:])
    if len(out) != size:
        raise ValueError('visual patch has the wrong uncompressed size')
    return out


def png_size(path):
    raw = path.read_bytes()
    if raw[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    return struct.unpack('>II', raw[16:24])


def inventory():
    files = [p for p in REFERENCE.rglob('*') if p.is_file()]
    images = [p for p in REFERENCE.rglob('*') if p.suffix.lower() in ('.png', '.jpg', '.jpeg')]
    ftx = [p for p in FINAL.rglob('*') if p.suffix.lower() == '.ftx']
    mbs = [p for p in FINAL.rglob('*') if p.suffix.lower() == '.mbs']
    manuals = [p for p in FINAL.rglob('*.ftx') if p.name.startswith('FR_manual_')]
    return {
        'files': len(files),
        'images': len(images),
        'ftx': len(ftx),
        'mbs': len(mbs),
        'manual_ftx': len(manuals),
        'manual_png': len(list((IMAGES / 'manual_paginas').glob('*.png'))),
        'fonts': len(list((REFERENCE / '4_fontes_usadas_nas_imagens').glob('*.ttf'))),
    }


def xbox_interface():
    patch = REPO / 'launcher' / 'prompts' / 'xbox-prompts.dcpx'
    if not patch.is_file():
        return None
    for target in parse_dcpx(patch.read_bytes()):
        if target['name'] == 'stage/Interface_US.ftx':
            return target
    return None


def xbox_patched(original, target):
    data = bytearray(original)
    for offset, block in target['runs']:
        data[offset:offset + 16] = block
    if crc32(data) != target['after']:
        raise SystemExit('the Xbox prompt patch does not reconstruct from the original Interface')
    return bytes(data)


def build():
    cpk = Cpk(ARCHIVE)
    rows = original_entries(cpk)
    original_interface = original_data(cpk, rows, 'stage/Interface_US.ftx')
    current_interface = read_archive(cpk, 'stage/Interface_US.ftx')
    if sha256(original_interface) != INTERFACE_SHA256 or crc32(original_interface) != INTERFACE_CRC32:
        raise SystemExit('the archived USA Interface_US.ftx is not the expected source')
    pt_interface = portuguese_interface()
    title_source = read_archive(cpk, 'stage/title_US.ftx')
    pt_title = TITLE_FR.read_bytes()

    for name, source, target in (('Interface', original_interface, pt_interface), ('title', title_source, pt_title)):
        if len(source) != len(target):
            raise SystemExit(f'{name}: source and replacement sizes differ')
        if ftx_structure(source) != ftx_structure(target):
            raise SystemExit(f'{name}: the replacement changed the FTX/GNF structure')

    xbox = xbox_interface()
    interface_runs = [(i, pt_interface[i:i + 16]) for i in range(0, len(original_interface), 16)
                      if original_interface[i:i + 16] != pt_interface[i:i + 16]]
    xbox_offsets = {offset for offset, _ in xbox['runs']} if xbox else set()
    if xbox and any(offset in xbox_offsets for offset, _ in interface_runs):
        raise SystemExit('the Portuguese Interface texture runs overlap the Xbox prompt runs')
    xbox_base = xbox_patched(original_interface, xbox) if xbox else None
    if crc32(current_interface) not in {INTERFACE_CRC32, xbox['after'] if xbox else -1}:
        raise SystemExit('the current archive is neither the original nor the expected Xbox-patched Interface')

    combined_interface = bytearray(xbox_base if xbox else original_interface)
    for offset, data in interface_runs:
        combined_interface[offset:offset + 16] = data
    combined_interface = bytes(combined_interface)
    if xbox and any(combined_interface[offset:offset + 16] != xbox_base[offset:offset + 16]
                    for offset, _ in xbox['runs']):
        raise SystemExit('the combined Interface does not preserve the Xbox prompt blocks')

    VISUAL.mkdir(parents=True, exist_ok=True)
    assets = []

    def add(family, patch_name, source_label, target_label, source_bytes, target_bytes, dimensions=''):
        data, runs = dcpx_bytes(target_label, source_bytes, target_bytes)
        path = VISUAL / (patch_name + '.z')
        path.write_bytes(pack_patch(data))
        assets.append({
            'family': family, 'asset_type': 'FTX/GNF texture pack', 'format': 'FTX container, GNF subresources',
            'patch': str(path.relative_to(REPO)), 'patch_size': path.stat().st_size,
            'destination': target_label, 'source': source_label, 'source_size': len(source_bytes),
            'source_crc32': f'{crc32(source_bytes):08x}', 'source_sha256': sha256(source_bytes),
            'replacement_size': len(target_bytes), 'replacement_crc32': f'{crc32(target_bytes):08x}',
            'replacement_sha256': sha256(target_bytes), 'runs': runs, 'dimensions': dimensions,
            'compression': 'DCPX 16-byte deltas, zlib-compressed on disk',
            'status': 'integrated'})
        return path

    for stale in VISUAL.glob('*.dcpx'):
        stale.unlink()

    add('interface', 'interface-us.dcpx', 'archive original', 'stage/Interface_US.ftx',
        original_interface, pt_interface, '2048x2048 source textures')
    if xbox:
        add('interface', 'interface-us-xbox.dcpx', 'archive with Xbox prompts', 'stage/Interface_US.ftx',
            xbox_base, combined_interface, '2048x2048 source textures')
    add('title', 'title-us.dcpx', 'archive', 'stage/title_US.ftx', title_source, pt_title, '4096x2048 source texture')

    manual_png = {p.stem: png_size(p) for p in (IMAGES / 'manual_paginas').glob('*.png')}
    for number, (source_path, target_label) in enumerate(MANUALS, 1):
        source = read_archive(cpk, target_label)
        target = source_path.read_bytes()
        if len(source) != len(target) or ftx_structure(source) != ftx_structure(target):
            raise SystemExit(f'{target_label}: replacement does not match the USA manual structure')
        dims = manual_png.get(f'manual_pt_{number:03d}')
        add('manual', f'manual-us-{number:03d}.dcpx', target_label, target_label, source, target,
            f'{dims[0]}x{dims[1]}' if dims else '')

    mbs = []
    for mirror in MBS_MIRRORS:
        data = read_archive(cpk, mirror['source'])
        if sha256(data) != mirror['sha256']:
            raise SystemExit(f'{mirror["source"]}: unexpected source hash')
        old = mirror['name'].encode('ascii')
        new = Path(mirror['target']).name.encode('ascii')
        if len(old) != len(new) or data.count(old) != 1:
            raise SystemExit(f'{mirror["source"]}: cannot rename the embedded layout')
        mbs.append({**mirror, 'asset_type': 'MBS layout', 'format': 'FMBS', 'dimensions': 'n/a',
                    'source_size': len(data), 'target_size': len(data), 'status': 'runtime mirror',
                    'result_sha256': sha256(portuguese_layout(mirror, data))})

    report = {
        'reference_root': str(REFERENCE.relative_to(REPO)),
        'archive': str(ARCHIVE.relative_to(REPO)),
        'inventory': inventory(),
        'assets': assets,
        'mbs': mbs,
        'skipped': [
            {'path': 'stage/Interface.ftx',
             'reason': 'base texture pack; Marcelo supplied the language-slot Interface_FR.ftx, mapped to Interface_US.ftx'},
            {'path': 'stage/title.ftx',
             'reason': 'base texture pack; Marcelo supplied the language-slot title_FR.ftx, mapped to title_US.ftx'},
            {'path': 'stage/Menu_US.mbs',
             'reason': 'no translated Menu layout was supplied'},
            {'path': '3_imagens/texturas_interface/*.png',
             'reason': 'source images for the FTX patches; not separate runtime assets'},
            {'path': '3_imagens/manual_paginas/*.png',
             'reason': 'source manual pages for the FTX patches; used for dimension checks only'},
            {'path': '3_imagens/previas/*',
             'reason': 'preview screenshots, not game assets'},
            {'path': '4_fontes_usadas_nas_imagens/*.ttf',
             'reason': 'source fonts used to draw the images, not game assets'},
            {'path': 'textos/* and 1_textos/*',
             'reason': 'game text handled by the separate text import; not visual assets'},
        ],
    }
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_bytes((json.dumps(report, ensure_ascii=False, indent=1) + '\n').encode('utf-8'))
    print(f'{len(assets)} patches in {VISUAL}')
    print(f'{len(mbs)} MBS mirrors; report {REPORT}')
    return 0


def validate():
    cpk = Cpk(ARCHIVE)
    rows = original_entries(cpk)
    original_interface = original_data(cpk, rows, 'stage/Interface_US.ftx')
    current_interface = read_archive(cpk, 'stage/Interface_US.ftx')
    pt_interface = portuguese_interface()
    title_source = read_archive(cpk, 'stage/title_US.ftx')
    pt_title = TITLE_FR.read_bytes()
    interface_runs = [(i, pt_interface[i:i + 16]) for i in range(0, len(original_interface), 16)
                      if original_interface[i:i + 16] != pt_interface[i:i + 16]]
    xbox = xbox_interface()
    xbox_base = xbox_patched(original_interface, xbox) if xbox else original_interface
    combined_interface = bytearray(xbox_base)
    for offset, data in interface_runs:
        combined_interface[offset:offset + 16] = data
    expected = {
        'interface-us.dcpx': (original_interface, pt_interface),
        'interface-us-xbox.dcpx': (xbox_base, bytes(combined_interface)),
        'title-us.dcpx': (title_source, pt_title),
    }
    for source_path, target_label in MANUALS:
        expected[f'manual-us-{source_path.stem[-3:]}.dcpx'] = (read_archive(cpk, target_label), source_path.read_bytes())

    errors = []
    seen = {}
    patches = sorted(VISUAL.glob('*.dcpx.z'))
    for path in patches:
        name = path.name[:-2]
        try:
            targets = parse_dcpx(unpack_patch(path.read_bytes()))
        except (ValueError, zlib.error) as e:
            errors.append(f'{path.name}: {e}')
            continue
        if len(targets) != 1:
            errors.append(f'{path.name}: {len(targets)} targets, expected one')
            continue
        target = targets[0]
        variant = target['name'] == 'stage/Interface_US.ftx' and name in ('interface-us.dcpx',
                                                                          'interface-us-xbox.dcpx')
        if target['name'] in seen and not variant:
            errors.append(f'{path.name}: duplicate destination {target["name"]}')
        seen[target['name']] = path.name
        source, wanted = expected.get(name, (None, None))
        if source is None:
            errors.append(f'{path.name}: no expected source mapping')
            continue
        result = apply_patch(source, target)
        if result is None:
            errors.append(f'{path.name}: does not apply to its source')
            continue
        if result != wanted:
            errors.append(f'{path.name}: reconstructed bytes differ from the reference')
        try:
            if ftx_structure(source) != ftx_structure(result):
                errors.append(f'{path.name}: reconstructed FTX structure differs')
        except ValueError as e:
            errors.append(f'{path.name}: {e}')
        print(f'{path.name}: {target["name"]}, source {target["before"]:08x}, result {target["after"]:08x}, '
              f'{"ok" if result is not None else "FAILED"}')

    current = {
        'interface-us.dcpx': current_interface,
        'interface-us-xbox.dcpx': current_interface,
        'title-us.dcpx': title_source,
    }
    for source_path, target_label in MANUALS:
        current[f'manual-us-{source_path.stem[-3:]}.dcpx'] = read_archive(cpk, target_label)
    applied = {}
    for path in patches:
        target = parse_dcpx(unpack_patch(path.read_bytes()))[0]
        if target['name'] in applied:
            continue
        source = current.get(path.name[:-2])
        if source is None:
            continue
        result = apply_patch(source, target)
        if result is not None:
            applied[target['name']] = result
    for mirror in MBS_MIRRORS:
        data = read_archive(cpk, mirror['source'])
        if sha256(data) != mirror['sha256']:
            errors.append(f'{mirror["source"]}: unexpected source hash')
            continue
        old = mirror['name'].encode('ascii')
        if data.count(old) != 1:
            errors.append(f'{mirror["source"]}: embedded name not found once')
            continue
        data = portuguese_layout(mirror, data.replace(old, Path(mirror['target']).name.encode('ascii')))
        applied[mirror['target']] = data
    for name in applied:
        if cpk.find(name) is None:
            errors.append(f'{name}: destination is not in the USA archive')
    print(f'runtime overlay for this archive: {len(applied)} assets')

    if not patches:
        errors.append('no visual patches')
    for e in errors:
        print('error:', e)
    print(f'{len(patches)} patches, {len(errors)} errors')
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('build', 'validate', 'inventory'))
    args = parser.parse_args()
    if args.command == 'build':
        return build()
    if args.command == 'validate':
        return validate()
    print(json.dumps(inventory(), indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
