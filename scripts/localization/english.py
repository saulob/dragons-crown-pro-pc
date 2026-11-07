#!/usr/bin/env python3
"""The game's English text for the builds without an English slot (Japan, Asia/Hong Kong). It is served by the port from
the tracked corpus, like Portuguese (Brazil): the player needs no other game.

Tracked: localization/en/sheets/<Sheet>.json, the English records with their indices, SHA-256 and text;
localization/en/sources/US.json, the same records' 16-hex hashes; localization/en/sources/<slot>.json, a regional slot's
map as a delta over localization/pt-BR/sources/<slot>.json (the records English adds, and the ones it cannot take). The
English sheets are read from a CUSA10454 USA folder and written only under cache/ (git-ignored).

    english.py extract [--pkg PKG | --game DIR]   the corpus under cache/localization/en/sheets/
    english.py generate                            the tracked sheets, from the extracted corpus
    english.py validate [--prepared DIR]           the tracked corpus and its maps; exit 1 on an error
    english.py sources  [--prepared DIR]           rewrite sources/ from the corpus and the prepared games
    english.py parity   [--prepared DIR]           each build's English and coverage; exit 1 when stale
    english.py map      [--prepared DIR]           the corpus against each regional slot, with why records are not mapped
"""
import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'prompts'))
import fms  # noqa: E402
from cpk import Cpk  # noqa: E402
from ptbr import (CORPUS_SHEETS, DEFAULT_GAME, PREPARED, SLOTS, TAG_KEYS, exclusion, sha256, slot_sheets,  # noqa: E402
                  source_hash, tag_keys)
from ptbr import SOURCES_DIR as PTBR_SOURCES  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
CACHE = REPO / 'cache' / 'localization' / 'en'
CORPUS = CACHE / 'sheets'
PACKAGE = CACHE / 'usa'
SOURCES = REPO / 'localization' / 'en' / 'sources'
SHEETS = REPO / 'localization' / 'en' / 'sheets'
EXIT_GAME_RECORDS = (387, 388, 394, 395)
EXPECTED_RECORDS = 11992
EXPECTED_DEFAULT_BONE = 2049
DEFAULT_BONE_MESSAGES = 1635  # the character messages follow the bone names
PROBLEM_LIMIT = 20


def us_sheets(game):
    archive = Cpk(Path(game) / 'DRACRO_US.CPK')
    return {sheet: archive.read(archive.find(f'msgsheet/{sheet}_US.fms')) for sheet in CORPUS_SHEETS}


def corpus_records(records, sheet):
    """The records the port takes: every non-empty one but the SystemMsg tag keys."""
    for index, text in enumerate(records):
        if not text.strip() or (sheet == 'SystemMsg' and index in TAG_KEYS):
            continue
        yield index, text


def sheet_data(sheet, raw):
    records = fms.parse(raw).records
    entries = [{'index': i, 'source_sha256': sha256(t), 'en': t} for i, t in corpus_records(records, sheet)]
    return {'sheet': sheet, 'records': len(records), 'sha256': hashlib.sha256(raw).hexdigest(), 'entries': entries}


def dump_sheet(data):
    return (json.dumps(data, ensure_ascii=False, indent=1) + '\n').encode('utf-8')


def game_of(args):
    if args.game:
        return Path(args.game)
    if args.pkg:
        if not (PACKAGE / 'DRACRO_US.CPK').is_file():
            import pkg_extract
            pkg_extract.extract(args.pkg, PACKAGE, only=['DRACRO_US.CPK'])
        return PACKAGE
    return DEFAULT_GAME


def cmd_extract(args):
    CORPUS.mkdir(parents=True, exist_ok=True)
    total = 0
    for sheet, raw in us_sheets(game_of(args)).items():
        data = sheet_data(sheet, raw)
        (CORPUS / f'{sheet}.json').write_bytes(dump_sheet(data))
        total += len(data['entries'])
        print(f'{sheet}: {len(data["entries"])} of {data["records"]} records')
    print(f'corpus {total} records -> {CORPUS.relative_to(REPO)}')
    return 0


def load_corpus(folder=CORPUS):
    corpus = {}
    for sheet in CORPUS_SHEETS:
        path = folder / f'{sheet}.json'
        if not path.is_file():
            sys.exit(f'{path}: missing, run english.py extract first')
        corpus[sheet] = json.loads(path.read_text('utf-8'))
    return corpus


def read_corpus(folder, problems):
    """The sheets that are readable UTF-8 JSON; an unreadable one is reported instead."""
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


def entries_of(data):
    entries = data.get('entries') if isinstance(data, dict) else None
    return entries if isinstance(entries, list) else []


def corpus_problems(corpus):
    """Everything wrong with the tracked corpus, checked without a game."""
    problems = []
    expected = set(CORPUS_SHEETS)

    def bad(sheet, index, text):
        problems.append(f'{sheet} {index}: {text}')

    total = 0
    for sheet in CORPUS_SHEETS:
        data = corpus[sheet]
        if not isinstance(data, dict) or set(data) != {'sheet', 'records', 'sha256', 'entries'}:
            problems.append(f'{sheet}.json: needs sheet, records, sha256 and entries only')
            continue
        if data['sheet'] != sheet:
            problems.append(f'{sheet}.json: "sheet" is {data["sheet"]!r}')
        record_count = data['records']
        if not isinstance(record_count, int) or record_count <= 0:
            problems.append(f'{sheet}.json: records must be a positive integer')
            continue
        if not isinstance(data['sha256'], str) or len(data['sha256']) != 64:
            problems.append(f'{sheet}.json: sha256 must be 64 hex digits')
        entries = data['entries']
        if not isinstance(entries, list):
            problems.append(f'{sheet}.json: entries must be a list')
            continue
        previous = -1
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {'index', 'source_sha256', 'en'}:
                problems.append(f'{sheet}.json: an entry without index, source_sha256 and en')
                continue
            index, text = entry['index'], entry['en']
            if not isinstance(index, int) or isinstance(index, bool) or index < 0 or index >= record_count:
                problems.append(f'{sheet}.json: index {index!r} outside 0..{record_count - 1}')
                continue
            if index <= previous:
                problems.append(f'{sheet} {index}: records are not in increasing order')
            previous = index
            if sheet == 'SystemMsg' and index in TAG_KEYS:
                bad(sheet, index, 'a tag key')
            if not isinstance(text, str) or not text.strip():
                bad(sheet, index, 'empty text')
            elif '\ufffd' in text or any(ord(c) < 0x20 and c != '\n' for c in text):
                bad(sheet, index, 'a control character')
            if not isinstance(entry['source_sha256'], str) or len(entry['source_sha256']) != 64:
                bad(sheet, index, 'source_sha256 must be 64 hex digits')
            elif isinstance(text, str) and sha256(text) != entry['source_sha256']:
                bad(sheet, index, 'the stored text is not the one its hash names')
            total += 1
        if sheet == 'SystemMsg':
            indices = {e['index'] for e in entries if isinstance(e, dict) and isinstance(e.get('index'), int)}
            if any(index not in indices for index in EXIT_GAME_RECORDS):
                problems.append(f'SystemMsg: the Exit Game records {EXIT_GAME_RECORDS} are not in the corpus')
        if sheet == 'DefaultBone':
            message_indices = [e['index'] for e in entries
                               if isinstance(e, dict) and isinstance(e.get('index'), int) and
                               e['index'] >= DEFAULT_BONE_MESSAGES]
            if len(entries) != EXPECTED_DEFAULT_BONE or message_indices != list(range(DEFAULT_BONE_MESSAGES, EXPECTED_DEFAULT_BONE)):
                problems.append(f'DefaultBone: {len(entries)} records, expected {EXPECTED_DEFAULT_BONE} with messages from {DEFAULT_BONE_MESSAGES}')
    if total != EXPECTED_RECORDS:
        problems.append(f'corpus {total} records, expected {EXPECTED_RECORDS}')
    if len(corpus) != len(expected):
        problems.append(f'{len(corpus)} sheets, expected {len(expected)}')
    return problems


def source_problems(corpus):
    """US.json holds every corpus record; each regional map covers corpus indices only."""
    problems = []
    path = SOURCES / 'US.json'
    if not path.is_file():
        return [f'{path.relative_to(REPO)}: missing, run english.py sources']
    data = json.loads(path.read_text('utf-8'))
    if data.get('source') != 'US' or not isinstance(data.get('sheets'), list):
        return [f'{path.relative_to(REPO)}: not a US source map']
    listed = {row.get('sheet'): row for row in data['sheets']}
    count = 0
    for sheet in CORPUS_SHEETS:
        data = corpus[sheet]
        if not isinstance(data, dict) or not isinstance(data.get('entries'), list):
            continue
        row = listed.get(sheet)
        expected = {e['index']: e['source_sha256'][:16] for e in data['entries']
                    if isinstance(e, dict) and isinstance(e.get('index'), int) and isinstance(e.get('source_sha256'), str)}
        if not row or row.get('records') != data.get('records') or row.get('sha256') != data.get('sha256'):
            problems.append(f'US.json {sheet}: not the tracked sheet')
            continue
        hashes = {index: value for index, value in row.get('hashes', [])}
        if hashes != expected:
            problems.append(f'US.json {sheet}: {len(hashes)} hashes, expected {len(expected)}')
        count += len(hashes)
    if count != EXPECTED_RECORDS:
        problems.append(f'US.json {count} records, expected {EXPECTED_RECORDS}')
    for name in ('JP', 'CT'):
        path = SOURCES / f'{name}.json'
        if not path.is_file():
            problems.append(f'{path.relative_to(REPO)}: missing, run english.py sources')
            continue
        data = json.loads(path.read_text('utf-8'))
        if data.get('source') != name or data.get('base') != 'pt-BR' or not isinstance(data.get('sheets'), list):
            problems.append(f'{path.relative_to(REPO)}: not a delta over the PT-BR maps')
            continue
        for row in data['sheets']:
            sheet = row.get('sheet')
            if sheet not in corpus:
                problems.append(f'{path.name}: unknown sheet {sheet!r}')
                continue
            data = corpus[sheet]
            if not isinstance(data, dict) or not isinstance(data.get('entries'), list):
                continue
            indices = {e['index'] for e in data['entries'] if isinstance(e, dict) and isinstance(e.get('index'), int)}
            for index, _ in row.get('hashes', []):
                if index not in indices:
                    problems.append(f'{path.name} {sheet} {index}: not a corpus record')
            for index, why in row.get('excluded', []):
                if index not in indices:
                    problems.append(f'{path.name} {sheet} {index}: not a corpus record ({why})')
    return problems


def regional(prepared):
    for label, (folder, archive, suffix, source) in SLOTS.items():
        game = prepared / folder
        if source and (game / archive).is_file():
            yield source, label, slot_sheets(game, archive, suffix)
        elif source:
            print(f'{label}: DATA UNAVAILABLE ({folder}/{archive})')


def slot_characters(raw):
    return set(''.join(record for sheet in CORPUS_SHEETS for record in fms.parse(raw[sheet]).records))


# The Town Map tutorial: the Japanese record names the stick with a glyph the English leaves out.
DROPPED_BUTTONS = {('SystemMsg', 1444)}


def english_exclusion(sheet, records, index, text, keys, characters):
    """Why the slot's record cannot show the English; a letter its own text never uses may be missing from its font."""
    if index >= len(records):
        return 'no record'
    why = exclusion(records[index], text, keys)
    if why == 'button' and (sheet, index) in DROPPED_BUTTONS:
        glyphless = re.sub(r'@#\(\d+\)', '', records[index])
        why = exclusion(glyphless, text, keys)
    if not why and any(ord(c) > 0x7e and c not in characters for c in text):
        why = 'glyph'
    return why


def mapping(corpus, raw):
    keys = tag_keys(fms.parse(raw['SystemMsg']).records)
    characters = slot_characters(raw)
    for sheet in CORPUS_SHEETS:
        records = fms.parse(raw[sheet]).records
        for entry in corpus[sheet]['entries']:
            index = entry['index']
            why = english_exclusion(sheet, records, index, entry['en'], keys, characters)
            yield sheet, index, entry['en'], None if index >= len(records) else records[index], why


def dump_rows(head, rows, key):
    body = ',\n'.join(f'      {json.dumps(row)}' for row in rows)
    return f'    {json.dumps(head)[:-1]}, "{key}": [\n{body}\n    ]}}'


def dump(source, sheets, extra=None):
    head = '{\n  "source": ' + json.dumps(source) + (f',\n  "base": {json.dumps(extra)}' if extra else '')
    return head + ',\n  "sheets": [\n' + ',\n'.join(sheets) + '\n  ]\n}\n'


def ptbr_map(source):
    data = json.loads((PTBR_SOURCES / f'{source}.json').read_text('utf-8'))
    return {s['sheet']: s for s in data['sheets']}


def slot_delta(source, corpus, raw):
    """The slot's English map as a delta over the PT-BR map of the same slot: the hashes it adds, the indices it drops."""
    base = ptbr_map(source)
    keys = tag_keys(fms.parse(raw['SystemMsg']).records)
    characters = slot_characters(raw)
    sheets = []
    for sheet in CORPUS_SHEETS:
        records = fms.parse(raw[sheet]).records
        row = base[sheet]
        if hashlib.sha256(raw[sheet]).hexdigest() != row['sha256']:
            sys.exit(f'{source} {sheet}: the PT-BR map was made from other sheets, run ptbr.py sources first')
        known = dict(row['hashes'])
        hashes, excluded = [], []
        for entry in corpus[sheet]['entries']:
            index = entry['index']
            why = english_exclusion(sheet, records, index, entry['en'], keys, characters)
            if why:
                if index in known or why != 'empty':
                    excluded.append([index, why])
            elif index in known:
                assert known[index] == source_hash(records[index]), (source, sheet, index)
            else:
                hashes.append([index, source_hash(records[index])])
        head = {'sheet': sheet, 'records': len(records), 'sha256': row['sha256'], 'excluded': sorted(excluded)}
        sheets.append(dump_rows(head, sorted(hashes), 'hashes'))
    return dump(source, sheets, 'pt-BR')


def cmd_sources(args):
    corpus = load_corpus(SHEETS)
    SOURCES.mkdir(parents=True, exist_ok=True)
    english = []
    for sheet in CORPUS_SHEETS:
        rows = [[e['index'], e['source_sha256'][:16]] for e in corpus[sheet]['entries']]
        head = {'sheet': sheet, 'records': corpus[sheet]['records'], 'sha256': corpus[sheet]['sha256']}
        english.append(dump_rows(head, rows, 'hashes'))
    (SOURCES / 'US.json').write_bytes(dump('US', english).encode('utf-8'))
    print('US: from the corpus')
    written = set()
    for source, label, raw in regional(args.prepared):
        if source in written:
            continue
        (SOURCES / f'{source}.json').write_bytes(slot_delta(source, corpus, raw).encode('utf-8'))
        written.add(source)
        print(f'{source}: from {label}')
    return 0


def english_map(source):
    """The slot's English map as the launcher builds it: the PT-BR hashes, plus the delta's, minus its exclusions."""
    delta = json.loads((SOURCES / f'{source}.json').read_text('utf-8'))
    base = ptbr_map(source)
    out = {}
    for row in delta['sheets']:
        hashes = dict(base[row['sheet']]['hashes'])
        hashes.update(dict(row['hashes']))
        for index, _ in row['excluded']:
            hashes.pop(index, None)
        out[row['sheet']] = (row['sha256'], hashes)
    return out


def coverage(corpus, raw):
    """Mapped records and the reason each of the others keeps the slot's text."""
    mapped, reasons, per_sheet = 0, Counter(), Counter()
    for sheet, _, _, _, why in mapping(corpus, raw):
        if why:
            reasons[(sheet, why)] += 1
            continue
        mapped += 1
        per_sheet[sheet] += 1
    return mapped, reasons, per_sheet


def print_coverage(source, label, corpus, raw):
    total = sum(len(entries_of(c)) for c in corpus.values())
    mapped, reasons, per_sheet = coverage(corpus, raw)
    print(f'{source} ({label}): mapped {mapped}/{total}, kept native {total - mapped}')
    for sheet in CORPUS_SHEETS:
        print(f'  {sheet}: {per_sheet[sheet]}/{len(entries_of(corpus[sheet]))}')
    for (sheet, why), n in sorted(reasons.items()):
        print(f'  unmapped {sheet} {why}: {n}')
    default_bone = per_sheet['DefaultBone']
    print(f'  DefaultBone: {default_bone}/{EXPECTED_DEFAULT_BONE} mapped, '
          f'messages {sum(1 for e in entries_of(corpus["DefaultBone"]) if e.get("index", -1) >= DEFAULT_BONE_MESSAGES)}/'
          f'{EXPECTED_DEFAULT_BONE - DEFAULT_BONE_MESSAGES}')
    return mapped, reasons


def cmd_validate(args):
    if not SHEETS.is_dir():
        sys.exit(f'{SHEETS}: missing, run english.py generate')
    present = {path.stem for path in SHEETS.glob('*.json')}
    problems = [f'{sheet}: not an English sheet' for sheet in sorted(present - set(CORPUS_SHEETS))]
    corpus = read_corpus(SHEETS, problems)
    if len(corpus) == len(CORPUS_SHEETS):
        problems += corpus_problems(corpus)
        problems += source_problems(corpus)
    total = sum(len(entries_of(c)) for c in corpus.values())
    print(f'corpus {total} records in {len(corpus)} sheets; '
          f'DefaultBone {len(entries_of(corpus.get("DefaultBone", {})))}')
    if not problems and args.prepared.is_dir():
        seen = set()
        for source, label, raw in regional(args.prepared):
            if source in seen:
                continue
            seen.add(source)
            print_coverage(source, label, corpus, raw)
    for line in problems[:PROBLEM_LIMIT]:
        print('error:', line)
    if len(problems) > PROBLEM_LIMIT:
        print(f'error: ... and {len(problems) - PROBLEM_LIMIT} more')
    print(f'{len(problems)} errors')
    return 1 if problems else 0


def cmd_generate(_args):
    corpus = load_corpus(CORPUS)
    problems = corpus_problems(corpus)
    if problems:
        sys.exit('\n'.join(f'error: {line}' for line in problems))
    SHEETS.mkdir(parents=True, exist_ok=True)
    for sheet in CORPUS_SHEETS:
        (SHEETS / f'{sheet}.json').write_bytes(dump_sheet(corpus[sheet]))
    total = sum(len(corpus[sheet]['entries']) for sheet in CORPUS_SHEETS)
    print(f'corpus {total} records in {len(CORPUS_SHEETS)} sheets -> {SHEETS.relative_to(REPO)}')
    return 0


def cmd_parity(args):
    problems = []
    corpus = read_corpus(SHEETS, problems)
    if len(corpus) != len(CORPUS_SHEETS):
        for line in problems:
            print('error:', line)
        return 1
    problems += corpus_problems(corpus) + source_problems(corpus)
    for line in problems:
        print('error:', line)
    total = sum(len(entries_of(c)) for c in corpus.values())
    print(f'English records {total}')
    port = available = ready = stale = 0
    for label, (folder, archive, suffix, source) in SLOTS.items():
        game = args.prepared / folder
        local = (game / archive).is_file()
        if source is None:
            available += 1
            ready += 1
            print(f'{label}: English native, ready ({archive}) | PASS' + ('' if local else ' STATIC / RUNTIME DATA UNAVAILABLE'))
            continue
        maps = english_map(source)
        mapped = sum(len(h) for _, h in maps.values())
        if not local:
            port += 1
            available += 1
            ready += 1
            print(f'{label}: English built in, mapped {mapped}/{total} | PASS STATIC / RUNTIME DATA UNAVAILABLE')
            continue
        raw = slot_sheets(game, archive, suffix)
        applied, changed = 0, Counter()
        for sheet in CORPUS_SHEETS:
            records = fms.parse(raw[sheet]).records
            sheet_sha, hashes = maps[sheet]
            if hashlib.sha256(raw[sheet]).hexdigest() != sheet_sha:
                changed[sheet] += 1
            for entry in corpus[sheet]['entries']:
                index = entry['index']
                if index in hashes and index < len(records) and hashes[index] == source_hash(records[index]):
                    applied += 1
                elif index in hashes:
                    changed[sheet] += 1
        exit_game = all(i in maps['SystemMsg'][1] for i in EXIT_GAME_RECORDS)
        ok = not changed and exit_game and applied == mapped
        stale += not ok
        port += ok
        available += 1
        ready += ok
        print(f'{label}: English built in, applied {applied}/{total}, kept native {total - applied}, '
              f'Exit Game records {"mapped" if exit_game else "NOT mapped"}'
              + (f', changed {dict(changed)}' if changed else '') + (' | PASS' if ok else ' | FAIL'))
    print(f'Port English supported | PASS {port}/5')
    print(f'English available | PASS {available}/8')
    print(f'English ready | PASS {ready}/8')
    return 1 if stale or problems else 0


def cmd_map(args):
    corpus = load_corpus(SHEETS)
    total = sum(len(entries_of(c)) for c in corpus.values())
    print(f'corpus {total} records')
    for source, label, raw in regional(args.prepared):
        print_coverage(source, label, corpus, raw)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    extract = sub.add_parser('extract')
    source = extract.add_mutually_exclusive_group()
    source.add_argument('--pkg', type=Path, help='a CUSA10454 USA package, read-only')
    source.add_argument('--game', type=Path, help='a folder holding DRACRO_US.CPK (default: game/prepared/v1.03)')
    extract.set_defaults(run=cmd_extract)
    sub.add_parser('generate').set_defaults(run=cmd_generate)
    for name, run in (('validate', cmd_validate), ('sources', cmd_sources), ('parity', cmd_parity), ('map', cmd_map)):
        command = sub.add_parser(name)
        command.add_argument('--prepared', type=Path, default=PREPARED)
        command.set_defaults(run=run)
    args = parser.parse_args()
    return args.run(args)


if __name__ == '__main__':
    sys.exit(main())
