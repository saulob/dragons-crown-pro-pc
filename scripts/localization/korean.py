#!/usr/bin/env python3
"""Korean (localization/ko): the game's official Korean translation, whose message sheets are index-aligned with the
USA ones, and the Hangul syllables it uses drawn for the USA font atlas. See localization/ko/README.md.

    korean.py map    --source DIR --game game/prepared/v1.03   # sheets/
    korean.py glyphs --game game/prepared/v1.03                 # font/hangul.glyphs.z

DIR holds DRACRO_PATCH_CK.CPK (the Korean sheets, msgsheet/*_KR.fms) and DRACRO_PATCH.CPK (the Japanese sheets they
are aligned with). A record takes the Korean at its index when the Japanese record there is the installed game's
Japanese record (DRACRO.CPK), which the USA record is aligned with.

The sheets and font/hangul.glyphs.z are tracked build inputs: a normal build never runs this script. Only regeneration
needs Pillow, numpy, bc7 and the source Korean archives.
"""
import argparse
import collections
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'scripts/prompts'), str(ROOT / 'scripts/localization')]
from cpk import Cpk  # noqa: E402
import ptbr  # noqa: E402

TRACKED = ROOT / 'localization/ko'
SKIP_SHEETS = {'StaffRollMsg'}  # credits stay in the build's language, as in the other packs
JAPANESE_NAME = {'ObjectParam': 'ObjectParamMsg'}
# Records whose Korean shows a button or icon the USA record does not (or the other way round): the edit that makes
# their printf conversions and buttons the USA record's.
FIXES = {
    ('AbilityList', 46): ('(@#(26) 연타)', '(연타)'),          # a rapid-press button the USA line does not show
    ('CustomItem', 3996): ('위력 %d의 ', ''),                 # the PS4 line names no power
    ('CustomItem', 3997): ('위력 %d의 ', ''),                 # the PS4 lines name no power
    ('CustomItem', 3998): ('위력 %d의 ', ''),                 # the PS4 lines name no power
    ('SystemMsg', 504): ('@#(53)', '@#(34)'),                 # the right stick is glyph 34 on PS4
    ('SystemMsg', 510): ('@#(53)', '@#(34)'),                 # the right stick is glyph 34 on PS4
    ('SystemMsg', 534): ('@#(53)', '@#(34)'),                 # the right stick is glyph 34 on PS4
    ('SystemMsg', 750): ('@#(36) ', ''),                      # a floor icon the USA line does not show
    ('SystemMsg', 1257): ('@#(33) ', ''),                     # a button the USA line does not show
    ('SystemMsg', 1444): ('@#(11)으로 ', ''),                 # directional buttons the USA line does not show
    ('SystemMsg', 1473): ('네트워크를 통해', 'PlayStation@#(52)Network를 통해'),  # the PS4 line's trademark glyph
}


def parse(data, pointer_bytes):
    """An FMSB sheet's records. The pointer table filled at load is 8 bytes a record in the installed game's sheets and
    4 in the Korean source archives."""
    header = struct.unpack_from('<I', data, 8)[0]
    count = struct.unpack_from('<I', data, 0x14)[0]
    trailer = struct.unpack_from('<I', data, 4)[0] + 0x20
    parts = data[header + count * pointer_bytes:trailer].split(b'\0')
    return [p.decode('utf-8') for p in parts[:count]]


def sheet(archive, path, pointer_bytes):
    try:
        return parse(archive.read(archive.find(path)), pointer_bytes)
    except KeyError:
        return None


def arguments(text):
    """The printf conversions in order and the button glyphs in any order: what the runtime makes a slot's own text
    share with a translation taken from another release (SameArguments in launcher/game_translation.cpp)."""
    tokens = ptbr.tokens(text)
    return ([t for t in tokens if ptbr.is_printf(t) and t[1] != ' '], sorted(t for t in tokens if t.startswith('@#(')))


def fit_tokens(english, korean):
    """The Korean line with its colour codes and full-width percent signs made the USA record's; None when its printf
    conversions or buttons still differ (the runtime would refuse it)."""
    if '%d%' in english or '%d %' in english:
        korean = korean.replace('％', '%')
    colour = re.compile(r'@c\(\d+\)')
    ec, kc = colour.findall(english), colour.findall(korean)
    if len(ec) == len(kc) and ec != kc:
        it = iter(ec)
        korean = colour.sub(lambda _: next(it), korean)
    return korean if arguments(english) == arguments(korean) else None


def write_sheet(name, entries):
    lines = ['{', f'  "sheet": "{name}",', '  "entries": [']
    lines += ['    ' + json.dumps(e, ensure_ascii=False) + (',' if k < len(entries) - 1 else '') for k, e in enumerate(entries)]
    (TRACKED / 'sheets' / f'{name}.json').write_text('\n'.join(lines + ['  ]', '}', '']), encoding='utf-8')


def cmd_map(args):
    game, source = Path(args.game), Path(args.source)
    usa, jpn = Cpk(game / 'DRACRO_US.CPK'), Cpk(game / 'DRACRO.CPK')
    source_jp, source_kr = Cpk(source / 'DRACRO_PATCH.CPK'), Cpk(source / 'DRACRO_PATCH_CK.CPK')
    names = sorted({m.group(1) for e in source_kr.entries if (m := re.match(r'msgsheet/([A-Za-z]+)_KR\.fms$', e['path']))}
                   - SKIP_SHEETS)
    (TRACKED / 'sheets').mkdir(parents=True, exist_ok=True)
    total = refused = moved = 0
    for name in names:
        jp_name = JAPANESE_NAME.get(name, name)
        us = sheet(usa, f'msgsheet/{name}_US.fms', 8)
        j4 = sheet(jpn, f'msgsheet/{jp_name}.fms', 8)
        j3 = sheet(source_jp, f'msgsheet/{jp_name}.fms', 4)
        k3 = sheet(source_kr, f'msgsheet/{name}_KR.fms', 4)
        if None in (us, j4, j3, k3) or len(us) != len(j4):
            print(f'{name}: skipped (a sheet is missing or the USA and Japanese sheets differ in length)')
            continue
        entries = []
        for i, english in enumerate(us):
            if i >= len(k3) or i >= len(j3) or not k3[i].strip() or not english.strip():
                continue
            if j3[i] != j4[i]:  # the installed record is not the source's one at this index
                moved += 1
                continue
            if name == 'SystemMsg' and i in ptbr.TAG_KEYS:  # tag keys are never translated
                continue
            text = k3[i]
            if (name, i) in FIXES:
                old, new = FIXES[(name, i)]
                text = text.replace(old, new)
            korean = fit_tokens(english, text)
            if korean is None:
                refused += 1
                continue
            entries.append({'index': i, 'source_sha256': hashlib.sha256(english.encode('utf-8')).hexdigest(), 'ko': korean})
        write_sheet(name, entries)
        total += len(entries)
        print(f'{name:15} {len(entries):5} of {sum(1 for t in us if t.strip()):5}')
    print(f'{total} records; {refused} left in English (control sequences differ); {moved} not at the same index')


# ------------------------------------------------------------------ glyphs

GLYPH_MAGIC = b'DCPKOR1\0'
CELL, BLOCKS = 68, 17
FONT_FILE = '/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc'
FONT_FACE_KR, FONT_WEIGHT, FONT_SIZE, BASELINE, WIDTH, SPACING = 1, 700, 56, 60, 60, 2


UI_TEXT_SOURCES = ('comms_text.c', 'boss_battle_text.c', 'exit_game_text.c')


def kanji_cells(table):
    """The atlas cells the USA glyph table lists for kanji only, in order."""
    by_cell = collections.defaultdict(list)
    for code, entry in table.items():
        by_cell[entry[0] | entry[1] << 8].append(code)
    return sorted(c for c, codes in by_cell.items()
                  if c and all(0x3400 <= k <= 0x9FFF or 0xF900 <= k <= 0xFAFF for k in codes))


def cmd_glyphs(args):
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont
    import bc7
    import russian
    texts = [e['ko'] for f in (TRACKED / 'sheets').glob('*.json') for e in json.loads(f.read_text(encoding='utf-8'))['entries']]
    # The port's own Korean lines the atlas draws too (quick chat, Boss Battle, the title menu).
    texts += [(ROOT / 'src' / name).read_text(encoding='utf-8') for name in UI_TEXT_SOURCES]
    syllables = sorted({ch for text in texts for ch in text if 0xAC00 <= ord(ch) <= 0xD7A3})
    table = russian.font_table(russian.load_image(Path(args.game) / 'eboot.bin'), 0x4E9C)
    cells = kanji_cells(table)
    if len(syllables) > len(cells):
        sys.exit(f'{len(syllables)} syllables, {len(cells)} kanji cells')
    font = ImageFont.truetype(args.font or FONT_FILE, FONT_SIZE, index=FONT_FACE_KR)
    font.set_variation_by_axes([FONT_WEIGHT])
    body = bytearray(GLYPH_MAGIC + struct.pack('<HHHH', len(syllables), CELL, BLOCKS, 0))
    for k, ch in enumerate(syllables):
        im = Image.new('L', (CELL, CELL), 0)
        draw = ImageDraw.Draw(im)
        box = draw.textbbox((0, 0), ch, font=font)
        draw.text(((CELL - (box[2] - box[0])) // 2 - box[0], BASELINE - box[3]), ch, font=font, fill=255)
        alpha = np.asarray(im)
        rgba = np.zeros((CELL, CELL, 4), np.uint8)
        rgba[..., 3] = alpha
        rgba[..., :3] = np.where(alpha[..., None] > 0, 255, 0)
        body += struct.pack('<HHBBH', ord(ch), cells[k], WIDTH, SPACING, 0) + bc7.encode(rgba)
    out = TRACKED / 'font/hangul.glyphs.z'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(struct.pack('<I', len(body)) + zlib.compress(bytes(body), 9))
    print(f'{len(syllables)} syllables in cells {cells[0]}..{cells[len(syllables) - 1]}; {out.stat().st_size} bytes')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('map')
    p.add_argument('--source', required=True, help='the folder holding DRACRO_PATCH_CK.CPK and DRACRO_PATCH.CPK')
    p.add_argument('--game', required=True, help='the prepared USA game folder (DRACRO.CPK, DRACRO_US.CPK)')
    p.set_defaults(func=cmd_map)
    p = sub.add_parser('glyphs')
    p.add_argument('--game', required=True, help='the prepared USA game folder (eboot.bin)')
    p.add_argument('--font', help='Noto Sans CJK (a .ttc with the KR face at index 1)')
    p.set_defaults(func=cmd_glyphs)
    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
