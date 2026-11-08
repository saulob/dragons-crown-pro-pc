#!/usr/bin/env python3
"""Russian game text: the fan translation by Exclusive Studio, ported to the PS4 by AlchemistLab, served by the port over
a native slot of every supported build like Portuguese (Brazil). The player needs no fan package.

The fan package is a CUSA10454 update (APP_VER 01.01) whose sheets write Cyrillic with the codes of kana whose cells its
font redraws: U+3060-U+3080 are the capitals and U+30A9-U+30C9 the small letters, in KANA_ORDER. The records the
system font draws (character messages, a few SystemMsg records) are real Unicode there. The tracked corpus is Unicode.

Tracked: localization/ru/sheets/<Sheet>.json, each record's index, the SHA-256 of the English record it translates, its
text and `system_font` where the fan package keeps Unicode; localization/ru/sources/<slot>.json, a regional slot's map as
a delta over the English one; localization/ru/font/cyrillic.glyphs.z, the 66 letters as the fan font draws them. The
package is read only into cache/localization/ru/ (git-ignored).

    russian.py extract --pkg PKG     the package's archive and executable, read-only, then the corpus under cache/
    russian.py generate              the tracked sheets and glyphs, from the extracted corpus
    russian.py validate              the tracked corpus, glyphs and maps; exit 1 on an error
    russian.py sources               rewrite sources/ from the corpus and the prepared games
    russian.py parity                each build's Russian coverage and font fit; exit 1 when stale
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'prompts'))
import fms  # noqa: E402
import ftex  # noqa: E402
from cpk import Cpk  # noqa: E402
from english import SHEETS as ENGLISH_SHEETS  # noqa: E402
from english import english_map, slot_characters  # noqa: E402
from ptbr import (CORPUS_SHEETS, PREPARED, SLOTS, TAG_KEYS, TOKEN, arguments, exclusion, formatted, sha256,  # noqa: E402
                  slot_sheets, source_hash, tag_keys, tokens)

REPO = Path(__file__).resolve().parents[2]
CACHE = REPO / 'cache' / 'localization' / 'ru'
PACKAGE = CACHE / 'pkg'
CORPUS = CACHE / 'sheets'
TRACKED = REPO / 'localization' / 'ru'
SHEETS = TRACKED / 'sheets'
SOURCES = TRACKED / 'sources'
GLYPHS = TRACKED / 'font' / 'cyrillic.glyphs.z'
CACHE_GLYPHS = CACHE / 'font' / 'cyrillic.glyphs.z'

FAN_EBOOT_SHA256 = '434b3bb43f407990f7b861ed57b8007ba459d63af25b1511ce7489be94c908c0'
FAN_SFO = {'TITLE_ID': 'CUSA10454', 'CONTENT_ID': 'UP2611-CUSA10454_00-DRAGONSCROWNPRO1', 'CATEGORY': 'gp',
           'APP_VER': '01.01', 'VERSION': '01.03'}

KANA_ORDER = 'АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЬЪЫЭЮЯ'  # the fan font's cells: the soft sign comes before the hard sign
CAPITAL_KANA, SMALL_KANA = 0x3060, 0x30A9
CAPITAL_CELL, SMALL_CELL = 300, 360  # those kana's cells in the USA font
FREE_FIRST_CELL = 58 * 60 + 28  # Japan and Asia/Hong Kong: after the PT-BR letters in the blank rows 58-59
DECODE = {chr(CAPITAL_KANA + i): c for i, c in enumerate(KANA_ORDER)}
DECODE.update({chr(SMALL_KANA + i): c.lower() for i, c in enumerate(KANA_ORDER)})
ENCODE = {cyrillic: kana for kana, cyrillic in DECODE.items()}
CYRILLIC = set(ENCODE)
JAPANESE_RUN = re.compile('[々぀-ヿ一-鿿]+')
REAL_CYRILLIC = re.compile('[Ѐ-џ]')

DEFAULT_BONE_MESSAGES = 1635  # the bone names before them are written for the fan name font, not this corpus
DEFAULT_BONE_MESSAGE_COUNT = 366
CHARACTER_MESSAGE_UNITS = 30
# The gold counter draws with the middle font, whose Latin t and v the fan package redraws as Cyrillic.
MIDDLE_FONT_RECORDS = {('SystemMsg', 746)}
EXIT_GAME_RECORDS = (387, 394)
ENTRY_KEYS = {'index', 'source_sha256', 'ru'}

GLYPH_MAGIC = b'DCPCYR1\0'
CELL_PIXELS, CELL_BLOCKS, BLOCK_BYTES = 68, 17, 16
FONT_PITCH_BLOCKS = 4096 // 4
FONT_DESCRIPTOR = bytes([0, 0, 0, 0, 0, 0x10, 0, 0x10, 1, 0x44, 0x44, 2, 2, 0x3c, 0x3c, 0])
PROBLEM_LIMIT = 20

# Every build's main font: the archive and entry, the executable's probe character, and where the letters go.
FONTS = {
    'CUSA10454 1.00 USA': ('v1.03', 'DRACRO_US.CPK', 'other/fontUTF16.ftx', CAPITAL_KANA, 'kana'),
    'CUSA10454 1.01 USA': ('CUSA10454-1.01', 'DRACRO_US.CPK', 'other/fontUTF16.ftx', CAPITAL_KANA, 'kana'),
    'CUSA07001 1.00 Japan': ('CUSA07001-1.00', 'DRACRO.CPK', 'other/fontUTF16.ftx', 0x4F53, 'free'),
    'CUSA07001 1.02 Japan': ('CUSA07001-1.02', 'DRACRO.CPK', 'other/fontUTF16.ftx', 0x4F53, 'free'),
    'CUSA07001 1.03 Japan': ('CUSA07001-1.03', 'DRACRO.CPK', 'other/fontUTF16.ftx', 0x4F53, 'free'),
    'CUSA10209 1.00 Asia/Hong Kong': ('CUSA10209-1.00', 'DRACRO_CK.CPK', 'other/CT_fontUTF16.ftx', 0x9AD4, 'free'),
    'CUSA10209 1.02 Asia/Hong Kong': ('CUSA10209-1.02', 'DRACRO_CK.CPK', 'other/CT_fontUTF16.ftx', 0x9AD4, 'free'),
    'CUSA10487 1.03 Europe': ('CUSA10487-1.03', 'DRACRO_EU.CPK', 'other/fontUTF16.ftx', CAPITAL_KANA, 'kana'),
}
# A Japanese update keeps its base package's archive.
BASE_FOLDER = {'CUSA07001-1.02': 'CUSA07001-1.00', 'CUSA07001-1.03': 'CUSA07001-1.00', 'CUSA10454-1.01': 'v1.03'}


def decode(text):
    """The fan encoding as Unicode: a run of Japanese script is Cyrillic only when every character is a redrawn kana."""
    return JAPANESE_RUN.sub(lambda m: ''.join(DECODE[c] for c in m.group()) if all(c in DECODE for c in m.group())
                            else m.group(), text)


def encode(text):
    return ''.join(ENCODE.get(c, c) for c in text)


def letter_cell(index, layout):
    if layout == 'free':
        return FREE_FIRST_CELL + index
    return (CAPITAL_CELL + index) if index < len(KANA_ORDER) else (SMALL_CELL + index - len(KANA_ORDER))


def glyph_codes():
    """(Unicode letter, fan kana code) in the order the glyph file stores them: capitals, then small letters."""
    return ([(c, CAPITAL_KANA + i) for i, c in enumerate(KANA_ORDER)] +
            [(c.lower(), SMALL_KANA + i) for i, c in enumerate(KANA_ORDER)])


# ------------------------------------------------------------------ executables

def load_image(path):
    import ps4_module
    elf, _header, ph, _, _ = ps4_module.parse_self(Path(path).read_bytes())
    loaded = [p for p in ph if p['type'] in (ps4_module.PT_LOAD, ps4_module.PT_SCE_RELRO)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loaded))
    for p in loaded:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = elf[p['offset']:p['offset'] + p['filesz']]
    return bytes(image)


def font_table(image, probe):
    """The main font's glyph table holding `probe`: {code: 8-byte entry}; None unless exactly one font has it."""
    found = []
    at = image.find(FONT_DESCRIPTOR)
    while at >= 0:
        if at % 8 == 0:
            pages = struct.unpack_from('<256Q', image, at + 16)
            entries = {}
            for high, page in enumerate(pages):
                if not page or page + 2048 > len(image):
                    continue
                for low in range(256):
                    entry = image[page + low * 8:page + low * 8 + 8]
                    if entry[0] | entry[1]:
                        entries[high << 8 | low] = entry
            if ord('a') in entries and probe in entries:
                found.append(entries)
        at = image.find(FONT_DESCRIPTOR, at + 1)
    return found[0] if len(found) == 1 else None


def cell_of(entry):
    return entry[0] | entry[1] << 8


def table_problem(table, layout):
    """Why the letters cannot be added to this glyph table; None when they can."""
    if table is None:
        return 'main font table not found'
    if any(code in table for code in (ord(c) for c in CYRILLIC)):
        return 'the table already lists Cyrillic letters'
    if layout == 'kana':
        for i, (_, kana) in enumerate(glyph_codes()):
            if kana not in table or cell_of(table[kana]) != letter_cell(i, layout):
                return f'U+{kana:04X} is not at cell {letter_cell(i, layout)}'
        return None
    used = {cell_of(entry): code for code, entry in table.items()}
    taken = [cell for cell in range(FREE_FIRST_CELL, FREE_FIRST_CELL + len(CYRILLIC))
             if cell in used and used[cell] < 0x80]
    return f'cells {taken[:4]} hold ASCII glyphs' if taken else None


# ------------------------------------------------------------------ fonts

def font_texture(blob):
    tex = ftex.textures(blob)[0]
    if (tex['width'], tex['height'], tex['dfmt'], tex['tiling'], tex['pitch']) != (4096, 4096, 41, 13, 4096):
        raise ValueError('not a 4096 x 4096 BC7 font atlas')
    return tex


def cell_blocks(blob, tex, cell):
    col, row = cell % 60, cell // 60
    out = bytearray()
    for by in range(CELL_BLOCKS):
        for bx in range(CELL_BLOCKS):
            at = tex['data'] + ftex.tiled_offset(col * CELL_BLOCKS + bx, row * CELL_BLOCKS + by, FONT_PITCH_BLOCKS)
            out += blob[at:at + BLOCK_BYTES]
    return bytes(out)


def cell_alpha_max(blob, tex, cell):
    """The cell's highest alpha, decoded through Pillow."""
    from PIL import Image
    import io
    blocks = cell_blocks(blob, tex, cell)
    dds = ftex.dds_wrap(blocks, CELL_PIXELS, CELL_PIXELS, 41)
    return Image.open(io.BytesIO(dds)).getchannel('A').getextrema()[1]


def pack_glyphs(font_blob, table):
    tex = font_texture(font_blob)
    body = bytearray(GLYPH_MAGIC + struct.pack('<HHHH', len(CYRILLIC), CELL_PIXELS, CELL_BLOCKS, 0))
    for i, (letter, kana) in enumerate(glyph_codes()):
        entry = table[kana]
        body += struct.pack('<HHBBH', ord(letter), kana, entry[2], entry[3], 0)
        body += cell_blocks(font_blob, tex, letter_cell(i, 'kana'))
    raw = bytes(body)
    return struct.pack('<I', len(raw)) + zlib.compress(raw, 9)


def unpack_glyphs(data):
    """[(code, kana, width, bearing, blocks)] from a glyph file; ValueError when it is not one."""
    if len(data) < 4:
        raise ValueError('too short')
    size = struct.unpack_from('<I', data)[0]
    raw = zlib.decompress(data[4:])
    if len(raw) != size or raw[:8] != GLYPH_MAGIC:
        raise ValueError('not a Cyrillic glyph file')
    count, pixels, blocks, _ = struct.unpack_from('<HHHH', raw, 8)
    if (count, pixels, blocks) != (len(CYRILLIC), CELL_PIXELS, CELL_BLOCKS):
        raise ValueError('unexpected glyph layout')
    record = 8 + blocks * blocks * BLOCK_BYTES
    if len(raw) != 16 + count * record:
        raise ValueError('truncated')
    out = []
    for i in range(count):
        at = 16 + i * record
        code, kana, width, bearing, _ = struct.unpack_from('<HHBBH', raw, at)
        out.append((code, kana, width, bearing, raw[at + 8:at + record]))
    return out


# ------------------------------------------------------------------ corpus

def english_corpus():
    out = {}
    for sheet in CORPUS_SHEETS:
        data = json.loads((ENGLISH_SHEETS / f'{sheet}.json').read_text('utf-8'))
        out[sheet] = {e['index']: (e['source_sha256'], e['en']) for e in data['entries']}
    return out


def fan_sheets(archive_path):
    archive = Cpk(archive_path)
    return {sheet: fms.parse(archive.read(archive.find(f'msgsheet/{sheet}_US.fms'))).records for sheet in CORPUS_SHEETS}


def build_corpus(fan, english):
    """The decoded corpus by sheet, and how many records were left out and why."""
    corpus, skipped = {}, Counter()
    for sheet in CORPUS_SHEETS:
        records = fan[sheet]
        known = english[sheet]
        entries = []
        for index, text in enumerate(records):
            if index not in known:
                skipped['no English record' if text.strip() else 'empty'] += 1
                continue
            source_sha, source = known[index]
            if text == source:
                skipped['left in English'] += 1
            elif sheet == 'DefaultBone' and index < DEFAULT_BONE_MESSAGES:
                skipped['bone name (fan name font)'] += 1
            elif (sheet, index) in MIDDLE_FONT_RECORDS:
                skipped['middle font'] += 1
            elif not text.strip():
                skipped['empty'] += 1
            elif argument_difference(source, plain_percents(source, decode(text))):
                skipped[f'{argument_difference(source, plain_percents(source, decode(text)))} differs from the English'] += 1
            else:
                entry = {'index': index, 'source_sha256': source_sha, 'ru': plain_percents(source, decode(text))}
                if REAL_CYRILLIC.search(text):
                    entry['system_font'] = True
                entries.append(entry)
        corpus[sheet] = {'sheet': sheet, 'entries': entries}
    return corpus, skipped


def dump_sheet(data):
    rows = ',\n'.join('    ' + json.dumps(e, ensure_ascii=False) for e in data['entries'])
    return ('{\n  "sheet": ' + json.dumps(data['sheet']) + ',\n  "entries": [\n' + rows + '\n  ]\n}\n').encode('utf-8')


def read_corpus(folder, problems):
    corpus = {}
    for sheet in CORPUS_SHEETS:
        path = folder / f'{sheet}.json'
        if not path.is_file():
            problems.append(f'{path.name}: missing')
            continue
        try:
            corpus[sheet] = json.loads(path.read_text('utf-8'))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            problems.append(f'{path.name}: not UTF-8 JSON: {error}')
    return corpus


def argument_difference(source, text):
    """Why the translation cannot replace the English record: 'printf', 'button' or 'tag'; None when it can. Its colours
    are its own, and it may write a tag's value out instead of the tag, as the fan text often does."""
    if not formatted(tokens(source)) and formatted(tokens(text)):
        return 'printf'
    ours, theirs = arguments(text), arguments(source)
    if ours[0] != theirs[0]:
        return 'printf'
    if ours[1] != theirs[1]:
        return 'button'
    if Counter(t for t in tokens(text) if t.startswith('##(')) - Counter(t for t in tokens(source) if t.startswith('##(')):
        return 'tag'
    return None


def plain_percents(source, text):
    """In text the game formats, a lone % becomes the full-width ％ the English writes."""
    if not formatted(tokens(source)):
        return text
    return TOKEN.sub(lambda m: '％' if m.group() == '%' else m.group(), text)


def utf16_units(text):
    return len(text.encode('utf-16-le')) // 2


def corpus_problems(corpus, english):
    """Everything wrong with the corpus, and counts worth reporting."""
    problems, notes = [], Counter()
    total = 0
    for sheet in CORPUS_SHEETS:
        data = corpus.get(sheet)
        if not isinstance(data, dict) or set(data) != {'sheet', 'entries'} or data['sheet'] != sheet:
            problems.append(f'{sheet}.json: needs "sheet": {sheet!r} and entries only')
            continue
        previous = -1
        for entry in data['entries']:
            keys = set(entry) if isinstance(entry, dict) else set()
            if keys - {'system_font'} != ENTRY_KEYS or entry.get('system_font', True) is not True:
                problems.append(f'{sheet}: an entry without index, source_sha256 and ru')
                continue
            index, text = entry['index'], entry['ru']
            where = f'{sheet} {index}'
            if not isinstance(index, int) or isinstance(index, bool) or index <= previous:
                problems.append(f'{where}: indices must increase')
                continue
            previous = index
            total += 1
            known = english[sheet].get(index)
            if known is None:
                problems.append(f'{where}: no English record')
                continue
            if entry['source_sha256'] != known[0]:
                problems.append(f'{where}: source_sha256 is not the English record\'s')
            if sheet == 'SystemMsg' and index in TAG_KEYS:
                problems.append(f'{where}: a tag key')
            if sheet == 'DefaultBone' and index < DEFAULT_BONE_MESSAGES:
                problems.append(f'{where}: a bone name')
            if not isinstance(text, str) or not text.strip():
                problems.append(f'{where}: empty text')
                continue
            if '�' in text or any(ord(c) < 0x20 and c != '\n' for c in text):
                problems.append(f'{where}: a control character')
            if decode(text) != text:
                problems.append(f'{where}: still in the fan encoding')
            if any(REAL_CYRILLIC.match(c) and c not in CYRILLIC for c in text):
                problems.append(f'{where}: Cyrillic outside the 66 letters')
            difference = argument_difference(known[1], text)
            if difference:
                problems.append(f'{where}: {difference} differs from the English')
            elif Counter(tokens(known[1])) != Counter(tokens(text)):
                notes['tags written out or colours changed'] += 1
            if plain_percents(known[1], text) != text:
                problems.append(f'{where}: a lone % in text the game formats')
            if sheet == 'DefaultBone' and utf16_units(text) > CHARACTER_MESSAGE_UNITS:
                notes[f'character messages over {CHARACTER_MESSAGE_UNITS} UTF-16 units (cut when saved)'] += 1
    messages = [e for e in corpus.get('DefaultBone', {}).get('entries', []) if e.get('index', 0) >= DEFAULT_BONE_MESSAGES]
    if len(messages) != DEFAULT_BONE_MESSAGE_COUNT:
        problems.append(f'DefaultBone: {len(messages)} character messages, expected {DEFAULT_BONE_MESSAGE_COUNT}')
    system = {e['index'] for e in corpus.get('SystemMsg', {}).get('entries', [])}
    if not all(index in system for index in EXIT_GAME_RECORDS):
        problems.append(f'SystemMsg: the Exit Game records {EXIT_GAME_RECORDS} are not in the corpus')
    return problems, notes, total


def characters_outside(corpus, allowed):
    found = Counter()
    for data in corpus.values():
        for entry in data.get('entries', []):
            for c in entry.get('ru', ''):
                if ord(c) > 0x7e and c not in CYRILLIC and c not in allowed:
                    found[c] += 1
    return found


def glyph_problems(data):
    try:
        glyphs = unpack_glyphs(data)
    except (ValueError, zlib.error) as error:
        return [f'{GLYPHS.name}: {error}'], []
    problems = []
    if [(chr(code), kana) for code, kana, *_ in glyphs] != glyph_codes():
        problems.append(f'{GLYPHS.name}: letters not in the fan font order')
    for code, _, width, _, blocks in glyphs:
        if not 8 <= width <= 68:
            problems.append(f'{GLYPHS.name}: {chr(code)} has width {width}')
        if not any(blocks):
            problems.append(f'{GLYPHS.name}: {chr(code)} has no pixels')
    return problems, glyphs


# ------------------------------------------------------------------ regional maps

def russian_exclusion(record, text, keys, characters):
    why = exclusion(record, text, keys)
    if not why and any(ord(c) > 0x7e and c not in CYRILLIC and c not in characters for c in text):
        why = 'glyph'
    return why


def slot_delta(source, corpus, raw):
    """The slot's Russian map as a delta over its English map: the hashes Russian adds and the records it cannot take."""
    english = english_map(source)
    keys = tag_keys(fms.parse(raw['SystemMsg']).records)
    characters = slot_characters(raw)
    sheets = []
    for sheet in CORPUS_SHEETS:
        records = fms.parse(raw[sheet]).records
        sheet_sha, mapped = english[sheet]
        if hashlib.sha256(raw[sheet]).hexdigest() != sheet_sha:
            sys.exit(f'{source} {sheet}: the English map was made from other sheets, run english.py sources first')
        hashes, excluded = [], []
        for entry in corpus[sheet]['entries']:
            index = entry['index']
            why = 'no record' if index >= len(records) else russian_exclusion(records[index], entry['ru'], keys, characters)
            if why:
                if index in mapped:
                    excluded.append([index, why])
            elif index not in mapped:
                hashes.append([index, source_hash(records[index])])
        rows = ',\n'.join(f'      {json.dumps(row)}' for row in sorted(hashes))
        head = {'sheet': sheet, 'records': len(records), 'sha256': sheet_sha, 'excluded': sorted(excluded)}
        sheets.append(f'    {json.dumps(head)[:-1]}, "hashes": [\n{rows}\n    ]}}' if hashes else
                      f'    {json.dumps(dict(head, hashes=[]))}')
    return '{\n  "source": ' + json.dumps(source) + ',\n  "base": "en",\n  "sheets": [\n' + ',\n'.join(sheets) + '\n  ]\n}\n'


def russian_map(source):
    """The slot's map as the launcher builds it: PT-BR's, then the English delta's, then the Russian delta's."""
    maps = english_map(source)
    delta = json.loads((SOURCES / f'{source}.json').read_text('utf-8'))
    for row in delta['sheets']:
        sheet_sha, hashes = maps[row['sheet']]
        hashes = dict(hashes)
        hashes.update(dict(row['hashes']))
        for index, _ in row['excluded']:
            hashes.pop(index, None)
        maps[row['sheet']] = (sheet_sha, hashes)
    return maps


def regional(prepared):
    seen = set()
    for label, (folder, archive, suffix, source) in SLOTS.items():
        if not source or source in seen:
            continue
        game = prepared / folder
        if (game / archive).is_file():
            seen.add(source)
            yield source, label, slot_sheets(game, archive, suffix)


# ------------------------------------------------------------------ commands

def cmd_extract(args):
    import pkg_extract
    import pkg_info
    info = pkg_info.inspect(str(args.pkg))
    sfo = info.get('param_sfo') or {}
    wrong = {k: sfo.get(k) for k, v in FAN_SFO.items() if sfo.get(k) != v}
    if wrong:
        sys.exit(f'{args.pkg.name}: not the Russian fan update ({wrong})')
    pkg_extract.extract(str(args.pkg), str(PACKAGE), only=['DRACRO_US.CPK', 'eboot.bin'])
    eboot = PACKAGE / 'eboot.bin'
    if hashlib.sha256(eboot.read_bytes()).hexdigest() != FAN_EBOOT_SHA256:
        sys.exit(f'{eboot}: not the fan executable {FAN_EBOOT_SHA256[:12]}')
    print(f'package {sfo["CONTENT_ID"]} APP_VER {sfo["APP_VER"]} VERSION {sfo["VERSION"]} ({sfo["CATEGORY"]}), '
          f'eboot {FAN_EBOOT_SHA256[:12]}')
    corpus, skipped = build_corpus(fan_sheets(PACKAGE / 'DRACRO_US.CPK'), english_corpus())
    CORPUS.mkdir(parents=True, exist_ok=True)
    for sheet, data in corpus.items():
        (CORPUS / f'{sheet}.json').write_bytes(dump_sheet(data))
        print(f'{sheet}: {len(data["entries"])} records')
    print(f'corpus {sum(len(d["entries"]) for d in corpus.values())} records; left out {dict(skipped)}')
    archive = Cpk(PACKAGE / 'DRACRO_US.CPK')
    font = archive.read(archive.find('other/fontUTF16.ftx'))
    table = font_table(load_image(eboot), CAPITAL_KANA)
    CACHE_GLYPHS.parent.mkdir(parents=True, exist_ok=True)
    CACHE_GLYPHS.write_bytes(pack_glyphs(font, table))
    print(f'glyphs {len(CYRILLIC)} -> {CACHE_GLYPHS.relative_to(REPO)} ({CACHE_GLYPHS.stat().st_size} bytes)')
    return 0


def cmd_generate(_args):
    problems = []
    corpus = read_corpus(CORPUS, problems)
    if problems:
        sys.exit('\n'.join(f'error: {p}' for p in problems) + '\nrun russian.py extract first')
    found, _, _ = corpus_problems(corpus, english_corpus())
    if found:
        sys.exit('\n'.join(f'error: {p}' for p in found[:PROBLEM_LIMIT]))
    SHEETS.mkdir(parents=True, exist_ok=True)
    for sheet in CORPUS_SHEETS:
        (SHEETS / f'{sheet}.json').write_bytes(dump_sheet(corpus[sheet]))
    GLYPHS.parent.mkdir(parents=True, exist_ok=True)
    GLYPHS.write_bytes(CACHE_GLYPHS.read_bytes())
    print(f'corpus {sum(len(d["entries"]) for d in corpus.values())} records -> {SHEETS.relative_to(REPO)}; '
          f'glyphs -> {GLYPHS.relative_to(REPO)}')
    return 0


def cmd_sources(args):
    problems = []
    corpus = read_corpus(SHEETS, problems)
    if problems:
        sys.exit('\n'.join(problems))
    SOURCES.mkdir(parents=True, exist_ok=True)
    for source, label, raw in regional(args.prepared):
        (SOURCES / f'{source}.json').write_bytes(slot_delta(source, corpus, raw).encode('utf-8'))
        print(f'{source}: from {label}')
    return 0


def source_problems(corpus):
    problems = []
    for source in ('JP', 'CT'):
        path = SOURCES / f'{source}.json'
        if not path.is_file():
            problems.append(f'{path.relative_to(REPO)}: missing, run russian.py sources')
            continue
        data = json.loads(path.read_text('utf-8'))
        if data.get('source') != source or data.get('base') != 'en' or not isinstance(data.get('sheets'), list):
            problems.append(f'{path.name}: not a delta over the English map')
            continue
        for row in data['sheets']:
            indices = {e['index'] for e in corpus.get(row.get('sheet'), {}).get('entries', [])}
            for index, _ in row.get('hashes', []) + row.get('excluded', []):
                if index not in indices:
                    problems.append(f'{path.name} {row.get("sheet")} {index}: not a corpus record')
    return problems


def print_problems(problems):
    for line in problems[:PROBLEM_LIMIT]:
        print('error:', line)
    if len(problems) > PROBLEM_LIMIT:
        print(f'error: ... and {len(problems) - PROBLEM_LIMIT} more')


def cmd_validate(args):
    problems = []
    corpus = read_corpus(SHEETS, problems)
    english = english_corpus()
    found, notes, total = corpus_problems(corpus, english)
    problems += found + source_problems(corpus)
    glyph_found, _ = glyph_problems(GLYPHS.read_bytes()) if GLYPHS.is_file() else ([f'{GLYPHS.name}: missing'], [])
    problems += glyph_found
    by_sheet = {sheet: len(corpus.get(sheet, {}).get('entries', [])) for sheet in CORPUS_SHEETS}
    system_font = sum(1 for d in corpus.values() for e in d.get('entries', []) if e.get('system_font'))
    messages = sum(1 for e in corpus.get('DefaultBone', {}).get('entries', []) if e['index'] >= DEFAULT_BONE_MESSAGES)
    english_total = sum(len(v) for v in english.values())
    print(f'corpus {total} records in {len(corpus)} sheets ({total * 100 // english_total}% of {english_total} English); '
          f'DefaultBone messages {messages}; system font {system_font}')
    print('  ' + ', '.join(f'{sheet} {count}' for sheet, count in by_sheet.items()))
    letters = Counter(c for d in corpus.values() for e in d.get('entries', []) for c in e['ru'] if c in CYRILLIC)
    print(f'Cyrillic letters used {len(letters)}/{len(CYRILLIC)}; unused: {"".join(sorted(CYRILLIC - set(letters)))}')
    other = characters_outside(corpus, set())
    print(f'other characters: {"".join(sorted(other))}')
    for note, count in sorted(notes.items()):
        print(f'note: {count} {note}')
    print_problems(problems)
    print(f'{len(problems)} errors')
    return 1 if problems else 0


def local_archive(folder, archive):
    for name in (folder, BASE_FOLDER.get(folder)):
        if name and (PREPARED / name / archive).is_file():
            return PREPARED / name / archive
    return None


def build_eboot(folder):
    if (PREPARED / folder / 'eboot.bin').is_file():
        return PREPARED / folder / 'eboot.bin'
    if folder == 'CUSA10454-1.01' and (PACKAGE / 'eboot.bin').is_file():
        return PACKAGE / 'eboot.bin'
    return None


def font_fit(label, glyphs):
    """'' when the build's executable and font take the letters, why not, or None when no local data exists."""
    folder, archive, entry, probe, layout = FONTS[label]
    eboot = build_eboot(folder)
    if folder == 'CUSA10454-1.01':
        archive_path = PACKAGE / archive if (PACKAGE / archive).is_file() else None
    else:
        archive_path = local_archive(folder, archive)
    if eboot is None or archive_path is None:
        return None
    why = table_problem(font_table(load_image(eboot), probe), layout)
    if why or layout == 'kana':
        return why or ''
    blob = Cpk(archive_path).read(Cpk(archive_path).find(entry))
    tex = font_texture(blob)
    inked = [letter_cell(i, layout) for i in range(len(glyphs)) if cell_alpha_max(blob, tex, letter_cell(i, layout)) > 8]
    return f'cells {inked[:4]} are not blank' if inked else ''


def keeps_base_sheets(label):
    """The 1.01 update is the fan package itself: the port keeps the base package's language slots under it."""
    return label == 'CUSA10454 1.01 USA'


def cmd_parity(args):
    problems = []
    corpus = read_corpus(SHEETS, problems)
    english = english_corpus()
    found, _, total = corpus_problems(corpus, english)
    problems += found + source_problems(corpus)
    glyph_found, glyphs = glyph_problems(GLYPHS.read_bytes()) if GLYPHS.is_file() else ([f'{GLYPHS.name}: missing'], [])
    problems += glyph_found
    print_problems(problems)
    if problems:
        return 1
    print(f'Russian records {total}')
    supported = stale = 0
    for label, (folder, archive, suffix, source) in SLOTS.items():
        fit = font_fit(label, glyphs)
        font_text = 'font STATIC' if fit is None else ('font fits' if fit == '' else f'font: {fit}')
        game = args.prepared / folder
        if not (game / archive).is_file() and keeps_base_sheets(label) and (args.prepared / BASE_FOLDER[folder] / archive).is_file():
            game = args.prepared / BASE_FOLDER[folder]
        local = (game / archive).is_file()
        if source is None:
            if not local:
                supported += fit in (None, '')
                print(f'{label}: Russian over the English slot, {font_text} | PASS STATIC / RUNTIME DATA UNAVAILABLE')
                continue
            raw = slot_sheets(game, archive, suffix)
            applied, refused = 0, Counter()
            for sheet in CORPUS_SHEETS:
                records = fms.parse(raw[sheet]).records
                for entry in corpus[sheet]['entries']:
                    index = entry['index']
                    if index >= len(records) or sha256(records[index]) != entry['source_sha256']:
                        refused['English changed'] += 1
                    elif argument_difference(records[index], entry["ru"]):
                        refused["printf or buttons"] += 1
                    else:
                        applied += 1
            ok = fit in (None, '') and not refused['English changed']
        else:
            maps = russian_map(source)
            mapped = sum(1 for sheet in CORPUS_SHEETS for e in corpus[sheet]['entries'] if e['index'] in maps[sheet][1])
            if not local:
                supported += fit in (None, '')
                print(f'{label}: Russian over the {source} slot, mapped {mapped}/{total}, {font_text} '
                      f'| PASS STATIC / RUNTIME DATA UNAVAILABLE')
                continue
            raw = slot_sheets(game, archive, suffix)
            applied, refused = 0, Counter()
            for sheet in CORPUS_SHEETS:
                records = fms.parse(raw[sheet]).records
                sheet_sha, hashes = maps[sheet]
                if hashlib.sha256(raw[sheet]).hexdigest() != sheet_sha:
                    refused['sheet changed'] += 1
                for entry in corpus[sheet]['entries']:
                    index = entry['index']
                    if index not in hashes:
                        refused['not mapped'] += 1
                    elif index < len(records) and hashes[index] == source_hash(records[index]):
                        applied += 1
                    else:
                        refused['record changed'] += 1
            ok = fit in (None, '') and not refused['sheet changed'] and not refused['record changed']
        exit_game = all(i in {e['index'] for e in corpus['SystemMsg']['entries']} for i in EXIT_GAME_RECORDS)
        ok = ok and exit_game
        supported += ok
        stale += not ok
        print(f'{label}: applied {applied}/{total}, kept native {total - applied} {dict(refused)}, {font_text}, '
              f'Exit Game records {"in corpus" if exit_game else "MISSING"} | {"PASS" if ok else "FAIL"}')
    print(f'Russian supported | PASS {supported}/8' if supported == 8 else f'Russian supported | FAIL {supported}/8')
    return 1 if stale or supported != 8 else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    extract = sub.add_parser('extract')
    extract.add_argument('--pkg', type=Path, required=True, help='the Russian fan update package, read-only')
    extract.set_defaults(run=cmd_extract)
    sub.add_parser('generate').set_defaults(run=cmd_generate)
    for name, run in (('validate', cmd_validate), ('sources', cmd_sources), ('parity', cmd_parity)):
        command = sub.add_parser(name)
        command.add_argument('--prepared', type=Path, default=PREPARED)
        command.set_defaults(run=run)
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    return args.run(args)


if __name__ == '__main__':
    sys.exit(main())
