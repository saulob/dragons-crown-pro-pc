#!/usr/bin/env python3
"""Brazilian Portuguese game text: a port-side language played in a native slot of every supported build. The corpus was
written against the English sheets of CUSA10454 1.00.

Tracked: localization/pt-BR/sheets/<Sheet>.json, the PT-BR text with each entry's record indices and the SHA-256 of
the English record it was written from; localization/pt-BR/sources/<source>.json, the hash of each mapped record of a slot
in another language; and localization/pt-BR/glossary.tsv. Game text is read from the installed game and written only under
cache/ (git-ignored); it never enters the repository.

    ptbr.py batches  [--game DIR]                the translation batches, with progress
    ptbr.py extract  [--game DIR] [--batch ID]   working files: cache/localization/pt-BR/work/<batch>.json
    ptbr.py merge FILE... [--game DIR]           a filled working file's pt_br into the tracked sheets
    ptbr.py validate [--game DIR]                the tracked sheets and the glossary; exit 1 on an error
    ptbr.py generate [--game DIR]                the sheets the game will read: cache/localization/pt-BR/msgsheet/
    ptbr.py reference [--game DIR] [--folder DIR]  audit Marcelo's PT-BR reference against the installed English
    ptbr.py import-reference [--game DIR]         import the safe Marcelo baseline into the tracked sheets
    ptbr.py format                               rewrites the tracked sheets in their canonical layout
    ptbr.py sources [--prepared DIR]             the slot maps of the builds not in English, from their prepared games
    ptbr.py parity [--prepared DIR]              each build's sheets against the corpus; exit 1 when a map is stale
    ptbr.py selftest                             the checks themselves, on synthetic text; no game needed
"""
import argparse
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'prompts'))
import fms  # noqa: E402
from cpk import Cpk  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
TRACKED = REPO / 'localization' / 'pt-BR'
SHEETS_DIR = TRACKED / 'sheets'
GLOSSARY = TRACKED / 'glossary.tsv'
SOURCES_DIR = TRACKED / 'sources'
CACHE = REPO / 'cache' / 'localization' / 'pt-BR'
DEFAULT_GAME = REPO / 'game' / 'prepared' / 'v1.03'
PREPARED = REPO / 'game' / 'prepared'

# The US slot's sheets; that slot reads StaffRollMsg_PS4_US, not StaffRollMsg_US.
SHEETS = ('AbilityList', 'BonusScore', 'ChallengeQuest', 'CustomItem', 'DefaultBone', 'EventMsg', 'ItemList',
          'ObjectParam', 'RoundContinue', 'StaffRollMsg_PS4', 'SystemMsg', 'Temple', 'TreasureArt', 'Tutorial')
CONTEXT_LANGUAGES = ('es', 'fr', 'de', 'it')
SLOT = {'en': 'US', 'es': 'ES', 'fr': 'FR', 'de': 'DE', 'it': 'IT'}
JAPANESE_SHEET = {'ObjectParam': 'ObjectParamMsg'}  # DRACRO.CPK's unsuffixed names
TAG_KEYS = range(109, 226)  # SystemMsg: the ##(NAME) keys; 226-342 hold their values
ENTRY_KEYS = ('index', 'source_sha256', 'pt_br')
PT_LETTERS = set('áàâãéêíóôõúüçÁÀÂÃÉÊÍÓÔÕÚÜÇ')
SPANISH_WORDS = {'y', 'el', 'los', 'las', 'del', 'pero', 'muy', 'hay', 'usted', 'ustedes', 'con', 'una', 'es', 'puede',
                 'puedes', 'tiene', 'tienes', 'juego', 'ahora', 'también', 'cuando', 'donde', 'aquí', 'nuevo', 'hacer'}
# Sheets of names and property lines: identical English is the same item or property wherever it repeats.
NAME_SHEETS = {'CustomItem', 'ItemList', 'ObjectParam', 'TreasureArt'}
GLOSSARY_COLUMNS = ['category', 'source', 'pt_br', 'status', 'notes']
GLOSSARY_STATUSES = {'approved', 'review'}
GLOSSARY_CATEGORIES = {'tag', 'difficulty', 'place', 'region', 'class', 'character', 'monster', 'stat', 'status_effect',
                       'skill', 'item', 'equipment', 'game_term', 'ui', 'online', 'proper_noun'}
TAG_SOURCE = re.compile(r'##\([A-Z0-9_]+\)')  # a glossary source that is a tag key stands for that tag's value
BATCH_UNITS, BATCH_WORDS = 120, 1500
LINE_LENGTH_SLACK = 1.10

# The same split as ControlTokens in launcher/game_translation.cpp.
TOKEN = re.compile(r'''
    \#\#\([^()\n]*\)                                      # ##(NAME)
  | @[A-Za-z\#]\([^()\n]*\)                               # @#(34) @c(10) @c(r,g,b) @x(4) @s(0.9) @w(60) @y(24) @t(3)
  | @/[A-Za-z]                                            # @/c @/t
  | @[^\n] | @                                            # @@ @C, any other pair, a lone @
  | %% | %[-+\ \#0]*\d*(?:\.\d+)?(?:hh|ll|h|l|z)?[diouxXeEfFgGcsp] | %
  | \$
''', re.X)


def tokens(text):
    return TOKEN.findall(text)


def is_printf(token):
    return len(token) > 1 and token[0] == '%' and token != '%%'


def formatted(token_list):
    """Text with a conversion the game fills in; elsewhere a % is prose ("50% of"), even where printf would read "% o"."""
    return any((is_printf(t) and t[1] != ' ') or t == '%%' for t in token_list)


def visible(text):
    return TOKEN.sub('', text)


def sha256(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def needs_translation(text):
    return re.search(r'[A-Za-z]{2,}', visible(text)) is not None


def word_count(text):
    return len(re.findall(r"[A-Za-z][A-Za-z'\-]*", visible(text)))


# ------------------------------------------------------------------ the installed game

class Game:
    def __init__(self, folder):
        folder = Path(folder)
        regional = folder / 'DRACRO_US.CPK'
        if not regional.is_file():
            sys.exit(f'{regional}: not found (an installed CUSA10454 1.00 game folder is needed, --game)')
        self.archive = Cpk(regional)
        shared = folder / 'DRACRO.CPK'
        self.shared = Cpk(shared) if shared.is_file() else None
        self._sheets = {}

    def records(self, sheet, language='en'):
        """The sheet's records in a language (en, es, fr, de, it, ja); None when the game has no such sheet or its
        records do not line up with the English ones."""
        key = (sheet, language)
        if key not in self._sheets:
            try:
                if language == 'ja':
                    path = f'msgsheet/{JAPANESE_SHEET.get(sheet, sheet)}.fms'
                    records = fms.records_of(self.shared, path) if self.shared else None
                else:
                    records = fms.records_of(self.archive, f'msgsheet/{sheet}_{SLOT[language]}.fms')
            except KeyError:
                records = None
            if records is not None and language != 'en' and len(records) != len(self.records(sheet)):
                records = None
            self._sheets[key] = records
        return self._sheets[key]

    def raw(self, sheet):
        return self.archive.read(self.archive.find(f'msgsheet/{sheet}_US.fms'))


# ------------------------------------------------------------------ context groups, units and batches

@dataclass
class Group:
    id: str
    label: str
    indices: list
    locked: str = ''  # why the group is never translated


@dataclass
class Unit:
    id: str
    group: Group
    english: str
    indices: list = field(default_factory=list)


@dataclass
class Batch:
    id: str
    sheet: str
    groups: list
    units: list


def windows(sheet, indices, size, label):
    return [Group(f'{sheet}-{indices[i]:04d}', label, indices[i:i + size]) for i in range(0, len(indices), size)]


def strided(sheet, count, parts, label, name_first=False):
    """Records that describe one thing across `parts` parallel tables: i, i + n, i + 2n..."""
    stride = count // parts
    groups = []
    for i in range(stride):
        indices = [i + k * stride for k in range(parts)]
        groups.append(Group(f'{sheet}-{i:04d}', label, indices[::-1] if name_first else indices))
    return groups, list(range(stride * parts, count))


def context_groups(sheet, en):
    n = len(en)
    if sheet == 'SystemMsg':
        return (windows(sheet, list(range(0, TAG_KEYS.start)), 30, 'online and network messages') +
                [Group('SystemMsg-tagkeys', 'tag keys', list(TAG_KEYS), 'tag keys the other records look up')] +
                windows(sheet, list(range(TAG_KEYS.stop, 343)), 40, 'tag values: places, regions, classes, menu terms') +
                windows(sheet, list(range(343, n)), 30, 'menus and system messages'))
    if sheet == 'AbilityList':
        # Records 0-85 describe the skills that 86-171 name.
        pairs, _ = strided(sheet, 172, 2, 'skill name and description', name_first=True)
        return pairs + windows(sheet, list(range(172, n)), 30, 'skill levels and effects')
    if sheet == 'ChallengeQuest':
        groups, rest = strided(sheet, n, 4, 'quest title, story, goal and completion')
        return groups + windows(sheet, rest, 30, 'quests')
    if sheet == 'TreasureArt':
        groups, rest = strided(sheet, n, 3, 'art title, story and artist credit')
        return groups + windows(sheet, rest, 30, 'treasure art')
    if sheet == 'Temple':
        groups, rest = strided(sheet, n, 2, 'blessing name and effect')
        return groups + windows(sheet, rest, 30, 'temple')
    if sheet == 'ItemList':
        groups, start, label = [], 0, 'items'
        for i in range(n + 1):
            marker = re.match(r'Begin (.+)$', en[i]) if i < n else None
            if marker or i == n:
                groups += windows(sheet, list(range(start, i)), 40, label)
                start = i
                if marker:
                    label = marker.group(1).lower()
        return groups
    if sheet == 'DefaultBone':
        names = [i for i in range(n) if re.fullmatch(r"[A-Z][A-Za-z'\-]*|　\S*", en[i])]
        name_set = set(names)
        return ([Group('DefaultBone-names', 'proper names', names, 'proper names stay unchanged')] +
                windows(sheet, [i for i in range(n) if i not in name_set], 30, 'bone messages'))
    sizes = {'EventMsg': (20, 'story text, consecutive scenes'), 'CustomItem': (40, 'equipment properties'),
             'Tutorial': (25, 'tutorial sequence'), 'StaffRollMsg_PS4': (30, 'staff credits'),
             'ObjectParam': (40, 'enemy and object names'), 'BonusScore': (40, 'round bonuses'),
             'RoundContinue': (40, 'continue bonuses')}
    size, label = sizes.get(sheet, (30, sheet))
    return windows(sheet, list(range(n)), size, label)


def shared_across_sheet(sheet, text):
    """Identical English in two places of a sheet takes one translation when it is a name or property line or a
    phrase long enough to carry its own context; short UI words are shared only inside their group."""
    return sheet in NAME_SHEETS or word_count(text) >= 3


def make_units(sheet, en, groups):
    units, found = [], {}
    for group in groups:
        if group.locked:
            continue
        for i in group.indices:
            if not needs_translation(en[i]):
                continue
            key = ('', en[i]) if shared_across_sheet(sheet, en[i]) else (group.id, en[i])
            if key not in found:
                found[key] = Unit(f'{sheet}:{i}', group, en[i])
                units.append(found[key])
            found[key].indices.append(i)
    return units


def make_batches(sheet, units):
    batches, current, words = [], [], 0
    for unit in units:
        new_group = current and unit.group is not current[-1].group
        full = len(current) >= BATCH_UNITS or words >= BATCH_WORDS
        if new_group and (full or unit.group.label != current[-1].group.label):
            batches.append(current)
            current, words = [], 0
        current.append(unit)
        words += word_count(unit.english)
    if current:
        batches.append(current)
    out = []
    for n, units_of in enumerate(batches, 1):
        groups = list(dict.fromkeys(u.group.id for u in units_of))
        out.append(Batch(f'{sheet}-{n:02d}', sheet, groups, units_of))
    return out


def plan(game):
    """Every sheet's groups, units and batches."""
    result = {}
    for sheet in SHEETS:
        en = game.records(sheet)
        if en is None:
            continue
        groups = context_groups(sheet, en)
        units = make_units(sheet, en, groups)
        result[sheet] = (en, groups, units, make_batches(sheet, units))
    return result


# ------------------------------------------------------------------ tracked files

def entry_indices(entry):
    index = entry.get('index')
    return index if isinstance(index, list) else [index]


def canonical_entry(indices, source_sha256, text):
    indices = sorted(indices)
    return {'index': indices[0] if len(indices) == 1 else indices, 'source_sha256': source_sha256, 'pt_br': text}


def dump_sheet(sheet, entries):
    entries = sorted(entries, key=lambda e: min(entry_indices(e)))
    lines = [json.dumps(canonical_entry(entry_indices(e), e['source_sha256'], e['pt_br']), ensure_ascii=False)
             for e in entries]
    body = ',\n'.join('    ' + line for line in lines)
    return '{\n  "sheet": ' + json.dumps(sheet) + ',\n  "entries": [\n' + (body + '\n' if body else '') + '  ]\n}\n'


def tracked_path(sheet):
    return SHEETS_DIR / f'{sheet}.json'


def load_tracked(path):
    """(data, text, error) for a tracked sheet file."""
    try:
        text = path.read_bytes().decode('utf-8')
        return json.loads(text), text, ''
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        return None, '', f'not UTF-8 JSON: {e}'


def write_tracked(sheet, entries):
    SHEETS_DIR.mkdir(parents=True, exist_ok=True)
    tracked_path(sheet).write_bytes(dump_sheet(sheet, entries).encode('utf-8'))


def load_glossary(path=GLOSSARY):
    """(rows, problems)."""
    if not path.is_file():
        return [], [f'{path.name}: missing']
    lines = path.read_bytes().decode('utf-8').split('\n')
    problems, rows, seen = [], [], set()
    if lines[0].split('\t') != GLOSSARY_COLUMNS:
        return [], [f'{path.name}: the header must be ' + '\\t'.join(GLOSSARY_COLUMNS)]
    for number, line in enumerate(lines[1:], 2):
        if not line:
            continue
        cells = line.split('\t')
        if len(cells) not in (len(GLOSSARY_COLUMNS) - 1, len(GLOSSARY_COLUMNS)) or cells[-1] == '':
            problems.append(f'{path.name}:{number}: {len(cells)} columns (notes may be left out, not left empty)')
            continue
        row = dict(zip(GLOSSARY_COLUMNS, cells + ['']))
        if row['category'] not in GLOSSARY_CATEGORIES:
            problems.append(f'{path.name}:{number}: unknown category {row["category"]}')
        if row['status'] not in GLOSSARY_STATUSES:
            problems.append(f'{path.name}:{number}: status must be approved or review')
        if not row['source'] or not row['pt_br']:
            problems.append(f'{path.name}:{number}: empty source or pt_br')
        if (row['category'], row['source']) in seen:
            problems.append(f'{path.name}:{number}: {row["category"]} {row["source"]} listed twice')
        if (row['category'] == 'tag' or row['source'].startswith('##(')) and not TAG_SOURCE.fullmatch(row['source']):
            problems.append(f'{path.name}:{number}: a tag source is its key, ##(NAME)')
        bad = disallowed_characters(row['pt_br'], row['source'])
        if bad:
            problems.append(f'{path.name}:{number}: characters outside the PT-BR set: {bad}')
        seen.add((row['category'], row['source']))
        rows.append(row)
    if '\r' in path.read_bytes().decode('utf-8'):
        problems.append(f'{path.name}: CR line endings')
    return rows, problems


def kept_words(glossary):
    """Words the glossary keeps as they are in English (names, abbreviations)."""
    words = set()
    for row in glossary:
        if not TAG_SOURCE.fullmatch(row['source']) and row['pt_br'] == row['source']:
            words.update(re.findall(r"[A-Za-z][A-Za-z'\-]*", row['source']))
    return words


def glossary_hits(glossary, english):
    hits = []
    for row in glossary:
        source = row['source']
        if TAG_SOURCE.fullmatch(source):
            found = source in english
        else:
            found = re.search(r'(?<![A-Za-z])' + re.escape(source) + r'(?![A-Za-z])', english, re.I) is not None
        if found:
            hits.append({'source': source, 'pt_br': row['pt_br'], 'status': row['status']})
    return hits


# ------------------------------------------------------------------ checks

CHARACTER_MESSAGE_UNITS = 30


def disallowed_characters(text, source):
    allowed = PT_LETTERS | set(source)
    bad = sorted({c for c in text if not (' ' <= c <= '~' or c == '\n' or c in allowed)})
    return ''.join(bad)


def record_errors(sheet, english, index, text):
    """What stops `text` replacing English record `index`; the runtime refuses the same (game_translation.cpp)."""
    errors = []
    if index >= len(english):
        return ['no such record']
    if sheet == 'SystemMsg' and index in TAG_KEYS:
        return ['a tag key, never translated']
    source = english[index]
    if not text and source:
        errors.append('empty translation')
    if any(ord(c) < 0x20 and c != '\n' for c in text):
        errors.append('a control character (only \\n line breaks)')
    a, b = tokens(source), tokens(text)
    if not formatted(a):
        if formatted(b):
            errors.append('a printf conversion in text the game does not format')
        a, b = [t for t in a if t[0] != '%'], [t for t in b if t[0] != '%']
    if [t for t in a if is_printf(t)] != [t for t in b if is_printf(t)]:
        errors.append(f'printf conversions differ: {[t for t in a if is_printf(t)]} != {[t for t in b if is_printf(t)]}')
    elif Counter(a) != Counter(b):
        missing, extra = Counter(a) - Counter(b), Counter(b) - Counter(a)
        errors.append(f'control sequences differ: missing {sorted(missing.elements())}, extra {sorted(extra.elements())}')
    bad = disallowed_characters(text, source)
    if bad:
        errors.append(f'characters outside the PT-BR set: {bad!r}')
    if sheet == 'DefaultBone' and len(text.encode('utf-16-le')) // 2 > CHARACTER_MESSAGE_UNITS:
        errors.append(f'longer than the {CHARACTER_MESSAGE_UNITS} units a character message keeps')
    return errors


def balanced(token_list):
    depth = Counter()
    for t in token_list:
        if t.startswith('@c(') or t.startswith('@t('):
            depth[t[1]] += 1
        elif t in ('@/c', '@/t'):
            depth[t[2]] -= 1
            if depth[t[2]] < 0:
                return False
    return not any(depth.values())


def languages_of(context, index):
    return [context[lang][index] for lang in ('en',) + CONTEXT_LANGUAGES if context.get(lang)]


def line_count_error(context, index, text):
    """More lines than any language uses may not fit the box the game draws the text in."""
    budget = max(o.count('\n') + 1 for o in languages_of(context, index))
    lines = text.count('\n') + 1
    return f'{lines} lines, the other languages use at most {budget}' if lines > budget else ''


def record_warnings(sheet, context, index, text, kept):
    """Things a reviewer should look at; none stops the record from being used."""
    english = context['en'][index]
    warnings = []
    a, b = tokens(english), tokens(text)
    if Counter(a) == Counter(b) and a != b:
        warnings.append('control sequences reordered')
    if balanced(a) and not balanced(b):
        warnings.append('color or timing spans no longer close in order')
    others = languages_of(context, index)
    longest = max((len(line) for line in visible(text).split('\n')), default=0)
    limit = max(len(line) for o in others for line in visible(o).split('\n'))
    if longest > limit * LINE_LENGTH_SLACK + 2:
        warnings.append(f'a line of {longest} characters, the other languages use at most {limit}')
    if text == english and needs_translation(english):
        words = set(re.findall(r"[A-Za-z][A-Za-z'\-]*", visible(english)))
        if words - kept:
            warnings.append('still the English text')
    spanish = context.get('es')
    if spanish and text == spanish[index] and spanish[index] != english:
        warnings.append('identical to the Spanish text')
    words = {w.lower() for w in re.findall(r'[^\W\d_]+', visible(text))}
    spanish_words = sorted(w for w in words if w in SPANISH_WORDS or w.endswith('ción') or w.endswith('ciones'))
    if spanish_words:
        warnings.append(f'Spanish words: {", ".join(spanish_words)}')
    return warnings


@dataclass
class Report:
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    notes: list = field(default_factory=list)


def check_sheet_file(sheet, data, text, context, kept, report):
    """Validates a tracked sheet's data against the installed English; returns {index: text} of the usable entries."""
    english = context['en']
    where = f'{sheet}.json'
    if not isinstance(data, dict) or set(data) != {'sheet', 'entries'} or not isinstance(data.get('entries'), list):
        report.errors.append(f'{where}: must be an object with "sheet" and "entries" only')
        return {}
    if data['sheet'] != sheet:
        report.errors.append(f'{where}: "sheet" is {data["sheet"]!r}')
    usable, seen, translations = {}, set(), defaultdict(set)
    for number, entry in enumerate(data['entries']):
        label = f'{where} entry {number}'
        if not isinstance(entry, dict) or set(entry) != set(ENTRY_KEYS):
            report.errors.append(f'{label}: needs exactly {", ".join(ENTRY_KEYS)}')
            continue
        indices, digest, pt = entry_indices(entry), entry['source_sha256'], entry['pt_br']
        if not indices or not all(isinstance(i, int) and not isinstance(i, bool) and i >= 0 for i in indices):
            report.errors.append(f'{label}: index must be a record number or a list of them')
            continue
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            report.errors.append(f'{label}: source_sha256 must be 64 lower-case hex digits')
            continue
        if not isinstance(pt, str):
            report.errors.append(f'{label}: pt_br must be text')
            continue
        for index in indices:
            at = f'{sheet} {index}'
            if index in seen:
                report.errors.append(f'{at}: translated twice')
                continue
            seen.add(index)
            if index < len(english) and sha256(english[index]) != digest:
                report.errors.append(f'{at}: the installed English record is not the one translated (stale entry)')
                continue
            errors = record_errors(sheet, english, index, pt)
            if not errors and line_count_error(context, index, pt):
                errors.append(line_count_error(context, index, pt))
            report.errors += [f'{at}: {e}' for e in errors]
            if errors:
                continue
            report.warnings += [f'{at}: {w}' for w in record_warnings(sheet, context, index, pt, kept)]
            usable[index] = pt
            translations[english[index]].add(pt)
    split = sum(1 for texts in translations.values() if len(texts) > 1)
    if split:
        report.notes.append(f'{sheet}: {split} English texts have more than one translation (fine when the context '
                            'differs)')
    if text and text != dump_sheet(sheet, data['entries']):
        report.warnings.append(f'{where}: not in the canonical layout (ptbr.py format)')
    return usable


def generate_sheet(raw, usable):
    """The PT-BR sheet bytes: the installed English sheet with the usable records replaced."""
    sheet = fms.parse(raw)
    if fms.build(sheet) != raw:
        raise ValueError('the English sheet does not rebuild byte-identical')
    for index, text in usable.items():
        sheet.records[index] = text
    return fms.build(sheet)


def check_generated(raw, data, usable):
    """Problems with a generated sheet: it must parse again, keep the record count and the header and trailer, and
    hold English wherever nothing was translated."""
    original, generated = fms.parse(raw), fms.parse(data)
    problems = []
    if len(generated.records) != len(original.records):
        problems.append('record count changed')
    if generated.header[:4] + generated.header[8:] != original.header[:4] + original.header[8:]:
        problems.append('header changed beyond its size field')
    if generated.trailer != original.trailer:
        problems.append('trailer changed')
    for i, (a, b) in enumerate(zip(original.records, generated.records)):
        if b != usable.get(i, a):
            problems.append(f'record {i} is neither its English nor its translation')
            break
    return problems


# ------------------------------------------------------------------ the human PT-BR reference

# Marcelo's translation, kept under cache/ and never tracked. One JSON per sheet: {"strings": [{"i", "en", "pt"}]}.
REFERENCE = CACHE / 'reference' / 'marcelo' / 'textos' / '1_textos' / 'json'


@dataclass
class Reference:
    sheets: dict
    encodings: dict
    duplicates: dict


def load_reference(folder=REFERENCE):
    """The reference sheets, or None when the folder is absent."""
    folder = Path(folder)
    if not folder.is_dir():
        return None
    sheets, encodings, duplicates = {}, {}, {}
    for path in sorted(folder.glob('*.json')):
        raw = path.read_bytes()
        text, encoding = None, ''
        for candidate in ('utf-8', 'cp1252'):
            try:
                text, encoding = raw.decode(candidate), candidate
                break
            except UnicodeDecodeError:
                continue
        if text is None:
            raise ValueError(f'{path.name}: neither UTF-8 nor cp1252')
        data = json.loads(text)
        records, seen, repeated = {}, set(), []
        for entry in data.get('strings', []):
            if not isinstance(entry.get('i'), int) or not isinstance(entry.get('en'), str) \
                    or not isinstance(entry.get('pt'), str):
                raise ValueError(f'{path.name}: an entry without an integer i and en/pt strings')
            if entry['i'] in seen:
                repeated.append(entry['i'])
            seen.add(entry['i'])
            records[entry['i']] = entry
        sheets[path.stem] = records
        encodings[path.stem] = encoding
        duplicates[path.stem] = repeated
    return Reference(sheets, encodings, duplicates)


def reference_tokens(source, target):
    """How target's control sequences differ from source's, by kind."""
    problems = []
    a, b = tokens(source), tokens(target)
    if not formatted(a):
        if formatted(b):
            problems.append('a printf conversion the source does not have')
        a = [t for t in a if t[0] != '%']
        b = [t for t in b if t[0] != '%']
    conversions = [t for t in a if is_printf(t)], [t for t in b if is_printf(t)]
    if conversions[0] != conversions[1]:
        problems.append(f'printf conversions {conversions[0]} != {conversions[1]}')
    tags = [t for t in a if t.startswith('##(')], [t for t in b if t.startswith('##(')]
    if Counter(tags[0]) != Counter(tags[1]):
        problems.append(f'tags {tags[0]} != {tags[1]}')
    codes = ([t for t in a if not t.startswith('##(') and not is_printf(t)],
             [t for t in b if not t.startswith('##(') and not is_printf(t)])
    if Counter(codes[0]) != Counter(codes[1]):
        problems.append(f'codes {codes[0]} != {codes[1]}')
    elif a != b:
        problems.append('control sequences reordered')
    if balanced(a) and not balanced(b):
        problems.append('color or timing spans no longer close in order')
    return problems


def reference_audit(context, sheets, duplicates):
    """{sheet: report} of Marcelo's records against the installed English."""
    english = context['en']
    result = {}
    for sheet, entries in sheets.items():
        installed = english.get(sheet)
        info = {'records': len(entries), 'installed': len(installed) if installed else 0, 'translated': 0,
                'source_matches': 0, 'source_mismatches': [], 'missing': [], 'out_of_range': [],
                'duplicates': duplicates.get(sheet, []), 'empty': [], 'encoding': [], 'characters': [],
                'tokens': defaultdict(list), 'spanish_copies': [], 'line_counts': [], 'split_texts': []}
        if installed is None:
            info['unknown_sheet'] = True
            result[sheet] = info
            continue
        info['missing'] = sorted(set(range(len(installed))) - set(entries))
        translations = defaultdict(set)
        for index, entry in sorted(entries.items()):
            if index >= len(installed):
                info['out_of_range'].append(index)
                continue
            source, target = installed[index], entry['pt']
            if source == entry['en']:
                info['source_matches'] += 1
            else:
                info['source_mismatches'].append(index)
            if not target.strip():
                info['empty'].append(index)
                continue
            info['translated'] += 1
            if '\ufffd' in target or any(ord(c) < 0x20 and c != '\n' for c in target):
                info['encoding'].append(index)
            bad = disallowed_characters(target, source)
            if bad:
                info['characters'].append((index, bad))
            for problem in reference_tokens(source, target):
                info['tokens'][problem].append(index)
            spanish = (context.get('es') or {}).get(sheet)
            if spanish and index < len(spanish) and spanish[index] == target != source:
                info['spanish_copies'].append(index)
            lines = [context[lang][sheet][index].count('\n')
                     for lang in ('en', 'es', 'fr', 'de', 'it')
                     if context.get(lang) and context[lang].get(sheet)]
            if target.count('\n') > max(lines, default=0):
                info['line_counts'].append(index)
            translations[source].add(target)
        info['split_texts'] = [sorted(seen) for seen in translations.values() if len(seen) > 1]
        result[sheet] = info
    return result


# ------------------------------------------------------------------ importing the human reference

REFERENCE_CHAR_FIXES = (
    ('\u2014', '-'), ('\u2013', '-'), ('\u201c', '"'), ('\u201d', '"'), ('\u2018', "'"), ('\u2019', "'"),
    ('N\u00ba', 'Nro.'), ('\u00ba', 'o'), ('\u00aa', 'a'),
)
REFERENCE_TEXT_FIXES = {
    ('SystemMsg', 719): ('token', [('Aprender a ##(ABILITY) "%s"?', 'Aprender a "%s" ##(ABILITY)?')]),
    ('SystemMsg', 1257): ('token', [('Entrar na sala do Andar %d em ##(R_RD)', 'Entrar na sala ##(R_RD) do Andar %d')]),
}
REFERENCE_LINE_REFLOW = {('ChallengeQuest', 96): 54, ('TreasureArt', 72): 67}
REFERENCE_GLOSSARY_FIXES = (
    ('Dano sofrido', 'Dano recebido'), ('dano sofrido', 'dano recebido'), ('DANO sofrido', 'DANO recebido'),
    ('Resistência a repulsão', 'Resistência a recuo'),
    ('Ataque Poderoso', 'Golpe Poderoso'),
)


def normalize_reference_text(text):
    """(text, the replacements made) for the typography the PT-BR character set does not carry."""
    changes = []
    for old, new in REFERENCE_CHAR_FIXES:
        if old in text:
            changes.append(f'{old} -> {new}')
            text = text.replace(old, new)
    return text, changes


def reflow_last_paragraph(text, width):
    """The text with its last paragraph wrapped at spaces to `width` columns."""
    head, separator, paragraph = text.rpartition('\n\n')
    if not separator:
        head, paragraph = '', text
    lines, current = [], ''
    for word in paragraph.split():
        if current and len(current) + 1 + len(word) > width:
            lines.append(current)
            current = word
        else:
            current = word if not current else current + ' ' + word
    if current:
        lines.append(current)
    return head + ('\n\n' if separator else '') + '\n'.join(lines)


def corrected_reference_text(sheet, index, text):
    """(text, character replacements, token/line fix kind, glossary replacements) for one reference record."""
    text, changes = normalize_reference_text(text)
    glossary_changes = []
    for old, new in REFERENCE_GLOSSARY_FIXES:
        if old in text:
            glossary_changes.append(f'{old} -> {new}')
            text = text.replace(old, new)
    fix = REFERENCE_TEXT_FIXES.get((sheet, index))
    kind = ''
    if fix:
        kind, replacements = fix
        applied = False
        for old, new in replacements:
            if old in text:
                text = text.replace(old, new)
                applied = True
        if not applied:
            kind = ''
    if (width := REFERENCE_LINE_REFLOW.get((sheet, index))) is not None:
        text = reflow_last_paragraph(text, width)
        kind = 'line'
    return text, changes, kind, glossary_changes


def import_reference(game, glossary, reference):
    """The tracked entries after importing the safe reference records; also the counts and review queue."""
    report = {'sheets': {}, 'corrections': {'token': [], 'character': [], 'line': [], 'glossary': []},
              'rejections': [], 'review': {'missing': defaultdict(list), 'proper_noun': defaultdict(list),
                                           'spanish_identical': defaultdict(list),
                                           'terminology_review': defaultdict(list),
                                           'line_length': defaultdict(list),
                                           'reviewed_existing_kept': []}}
    kept = kept_words(glossary)
    for sheet in sorted(reference.sheets):
        if sheet not in SHEETS:
            continue
        english = game.records(sheet)
        if english is None:
            continue
        context = contexts(game, sheet)
        existing, existing_pt, covered = [], {}, set()
        if tracked_path(sheet).is_file():
            data, _, error = load_tracked(tracked_path(sheet))
            if error:
                raise ValueError(f'{sheet}.json: {error}')
            for entry in data.get('entries', []):
                indices = entry_indices(entry)
                existing.append(canonical_entry(indices, entry['source_sha256'], entry['pt_br']))
                for index in indices:
                    covered.add(index)
                    existing_pt[index] = entry['pt_br']
        added = []
        counts = Counter(source=len(english), marcelo=len(reference.sheets[sheet]), existing=len(covered))
        for index, source in enumerate(english):
            if index in covered:
                entry = reference.sheets[sheet].get(index)
                if entry and entry['en'] == source and entry['pt'] != existing_pt[index]:
                    report['review']['reviewed_existing_kept'].append(
                        {'sheet': sheet, 'index': index, 'existing': existing_pt[index], 'marcelo': entry['pt']})
                continue
            if sheet == 'SystemMsg' and index in TAG_KEYS:
                counts['tag_keys'] += 1
                continue
            if not needs_translation(source):
                counts['non_text'] += 1
                continue
            entry = reference.sheets[sheet].get(index)
            if entry is None:
                counts['missing'] += 1
                report['review']['missing'][sheet].append(index)
                continue
            if entry['en'] != source:
                counts['rejected'] += 1
                report['rejections'].append({'sheet': sheet, 'index': index, 'reasons': ['source mismatch']})
                continue
            original = entry['pt']
            if not original.strip():
                counts['rejected'] += 1
                report['rejections'].append({'sheet': sheet, 'index': index, 'reasons': ['empty translation']})
                continue
            text, changes, fix_kind, glossary_changes = corrected_reference_text(sheet, index, original)
            if text == source:
                counts['unchanged'] += 1
                report['review']['proper_noun'][sheet].append(index)
                continue
            problems = record_errors(sheet, english, index, text)
            line_problem = line_count_error(context, index, text)
            if line_problem:
                problems.append(line_problem)
            problems.extend(reference_tokens(source, text))
            problems = list(dict.fromkeys(p for p in problems if p))
            if problems:
                counts['rejected'] += 1
                report['rejections'].append({'sheet': sheet, 'index': index, 'reasons': problems,
                                             'english': source, 'marcelo': original, 'corrected': text})
                continue
            added.append(canonical_entry([index], sha256(source), text))
            counts['imported_new'] += 1
            if changes:
                report['corrections']['character'].append(
                    {'sheet': sheet, 'index': index, 'changes': changes, 'before': original, 'after': text})
            if fix_kind:
                report['corrections'][fix_kind].append(
                    {'sheet': sheet, 'index': index, 'before': original, 'after': text})
            if glossary_changes:
                report['corrections']['glossary'].append(
                    {'sheet': sheet, 'index': index, 'changes': glossary_changes, 'before': original, 'after': text})
            spanish = (context.get('es') or [None] * len(english))[index]
            if spanish and text == spanish and spanish != source:
                report['review']['spanish_identical'][sheet].append(index)
            if any(hit['status'] == 'review' for hit in glossary_hits(glossary, source)):
                report['review']['terminology_review'][sheet].append(index)
            if any(w.startswith('a line of ') for w in record_warnings(sheet, context, index, text, kept)):
                report['review']['line_length'][sheet].append(index)
        entries = existing + added
        if entries:
            write_tracked(sheet, entries)
        translated = {index for entry in entries for index in entry_indices(entry)}
        counts['translated'] = len(translated)
        counts['fallback'] = len(english) - len(translated)
        counts['rejected'] = counts.get('rejected', 0)
        report['sheets'][sheet] = dict(counts)
    return report


def import_review_text(report):
    """The Markdown review queue for the imported baseline."""
    lines = ['# PT-BR Marcelo baseline review queue', '']
    lines.append('This file is ignored by git. It records what the import kept, corrected, rejected or left in English.')
    lines.append('')
    lines.append('## Per-sheet summary')
    lines.append('')
    lines.append('| Sheet | Source records | Marcelo records | PT-BR records | New imports | Kept reviewed | Rejected | Missing | Unchanged | Non-text | English fallback |')
    lines.append('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
    for sheet, c in report['sheets'].items():
        lines.append(f'| {sheet} | {c["source"]} | {c["marcelo"]} | {c["translated"]} | {c.get("imported_new", 0)} | '
                     f'{c.get("existing", 0)} | {c.get("rejected", 0)} | {c.get("missing", 0)} | '
                     f'{c.get("unchanged", 0)} | {c.get("non_text", 0)} | {c["fallback"]} |')
    lines.append('')
    for title, key in (('Token-order corrections', 'token'), ('Character normalizations', 'character'),
                       ('Line-limit corrections', 'line'), ('Glossary normalizations', 'glossary')):
        rows = report['corrections'][key]
        lines.append(f'## {title}: {len(rows)}')
        lines.append('')
        for row in rows:
            lines.append(f'- {row["sheet"]} {row["index"]}')
        lines.append('')
    lines.append(f'## Rejections: {len(report["rejections"])}')
    lines.append('')
    for row in report['rejections']:
        lines.append(f'- {row["sheet"]} {row["index"]}: {"; ".join(row["reasons"])}')
    lines.append('')
    lines.append('## Review categories')
    for title, key in (('Untranslated or missing', 'missing'),
                       ('Identical to English (proper noun or untranslated)', 'proper_noun'),
                       ('Spanish-identical', 'spanish_identical'), ('Terminology review', 'terminology_review'),
                       ('Line-length warning', 'line_length')):
        lines.append('')
        lines.append(f'### {title}')
        for sheet, indices in report['review'][key].items():
            if indices:
                lines.append(f'- {sheet}: {indices}')
    lines.append('')
    lines.append('### Reviewed entries kept over Marcelo')
    for row in report['review']['reviewed_existing_kept']:
        lines.append(f'- {row["sheet"]} {row["index"]}')
    lines.append('')
    return '\n'.join(lines)


# ------------------------------------------------------------------ commands

def contexts(game, sheet):
    context = {'en': game.records(sheet)}
    for language in CONTEXT_LANGUAGES + ('ja',):
        context[language] = game.records(sheet, language)
    return context


def tracked_usable(game, glossary, report):
    """{sheet: {index: text}} of the tracked sheets that pass, with every problem in `report`."""
    kept = kept_words(glossary)
    result = {}
    for path in sorted(SHEETS_DIR.glob('*.json')) if SHEETS_DIR.is_dir() else []:
        sheet = path.stem
        if sheet not in SHEETS:
            report.errors.append(f'{path.name}: not one of the US slot sheets')
            continue
        data, text, error = load_tracked(path)
        if error:
            report.errors.append(f'{path.name}: {error}')
            continue
        result[sheet] = check_sheet_file(sheet, data, text, contexts(game, sheet), kept, report)
    return result


def cmd_batches(args):
    game = Game(args.game)
    done = {}
    for sheet in SHEETS:
        path = tracked_path(sheet)
        data = load_tracked(path)[0] if path.is_file() else None
        done[sheet] = {i for e in (data or {}).get('entries', []) for i in entry_indices(e)}
    total_units = total_done = 0
    for sheet, (en, groups, units, batches) in plan(game).items():
        locked = sum(len(g.indices) for g in groups if g.locked)
        print(f'{sheet}: {len(en)} records, {len(units)} to translate'
              f'{f", {locked} kept as they are" if locked else ""}, {len(batches)} batches')
        for batch in batches:
            finished = sum(all(i in done[sheet] for i in u.indices) for u in batch.units)
            records = sum(len(u.indices) for u in batch.units)
            words = sum(word_count(u.english) for u in batch.units)
            first, last = min(u.indices[0] for u in batch.units), max(u.indices[0] for u in batch.units)
            label = batch.units[0].group.label
            print(f'  {batch.id}: records {first}-{last}, {len(batch.units)} texts for {records} records, {words} words,'
                  f' {finished} done; {label}')
            total_units += len(batch.units)
            total_done += finished
    print(f'{total_done} of {total_units} texts translated')
    return 0


def cmd_extract(args):
    game = Game(args.game)
    glossary, _ = load_glossary()
    reference = load_reference(args.reference)
    out = CACHE / 'work'
    out.mkdir(parents=True, exist_ok=True)
    written = 0
    for sheet, (en, groups, units, batches) in plan(game).items():
        context = contexts(game, sheet)
        existing = {}
        if tracked_path(sheet).is_file():
            data = load_tracked(tracked_path(sheet))[0] or {}
            for entry in data.get('entries', []):
                for i in entry_indices(entry):
                    existing[i] = entry.get('pt_br', '')
        elsewhere = defaultdict(list)
        for unit in units:
            elsewhere[en[unit.indices[0]]].append(unit.id)
        for batch in batches:
            if args.batch and batch.id != args.batch:
                continue
            items = []
            for unit in batch.units:
                first = unit.indices[0]
                text = en[first]
                item = {'id': unit.id, 'group': unit.group.id, 'context': unit.group.label, 'indices': unit.indices,
                        'source_sha256': sha256(text), 'en': text}
                for language in CONTEXT_LANGUAGES + ('ja',):
                    if context[language]:
                        item[language] = context[language][first]
                others = [context[lang][first] for lang in ('en',) + CONTEXT_LANGUAGES if context[lang]]
                item['tokens'] = tokens(text)
                item['max_lines'] = max(o.count('\n') + 1 for o in others)
                item['max_line_length'] = max(len(line) for o in others for line in visible(o).split('\n'))
                group_indices = unit.group.indices
                position = group_indices.index(first)
                if position:
                    item['before'] = en[group_indices[position - 1]]
                if position + 1 < len(group_indices):
                    item['after'] = en[group_indices[position + 1]]
                same = [u for u in elsewhere[text] if u != unit.id]
                if same:
                    item['same_english'] = same
                hits = glossary_hits(glossary, text)
                if hits:
                    item['glossary'] = hits
                if reference:
                    entry = reference.sheets.get(sheet, {}).get(first)
                    if entry and entry['en'] == text and entry['pt'].strip():
                        item['marcelo_pt_br'] = entry['pt']
                        problems = reference_tokens(text, entry['pt'])
                        if problems:
                            item['marcelo_note'] = '; '.join(problems)
                item['pt_br'] = existing.get(first, '')
                items.append(item)
            work = {'batch': batch.id, 'sheet': sheet,
                    'note': 'English is the source; es/fr/de/it/ja are context only. Fill pt_br and keep every token.',
                    'units': items}
            (out / f'{batch.id}.json').write_bytes((json.dumps(work, ensure_ascii=False, indent=1) + '\n').encode())
            written += 1
    if args.batch and not written:
        sys.exit(f'no batch {args.batch} (ptbr.py batches lists them)')
    print(f'{written} working files in {out}')
    return 0


def cmd_merge(args):
    game = Game(args.game)
    merged = refused = 0
    for path in args.files:
        work = json.loads(Path(path).read_bytes().decode('utf-8'))
        sheet = work['sheet']
        english = game.records(sheet)
        if english is None:
            sys.exit(f'{path}: no sheet {sheet} in the game')
        context = contexts(game, sheet)
        data = load_tracked(tracked_path(sheet))[0] if tracked_path(sheet).is_file() else None
        entries = (data or {}).get('entries', [])
        changed = False
        for unit in work['units']:
            text = unit.get('pt_br', '')
            if not text:
                continue
            indices = unit['indices']
            stale = [i for i in indices if i >= len(english) or sha256(english[i]) != unit['source_sha256']]
            errors = ['the installed English is not the extracted one'] if stale else []
            errors += list(dict.fromkeys(e for i in indices for e in record_errors(sheet, english, i, text)))
            if not errors and line_count_error(context, indices[0], text):
                errors.append(line_count_error(context, indices[0], text))
            if errors:
                refused += 1
                print(f'{unit["id"]}: not merged, {"; ".join(errors)}')
                continue
            taken = set(indices)
            kept = []
            for entry in entries:
                rest = [i for i in entry_indices(entry) if i not in taken]
                if rest:
                    kept.append(canonical_entry(rest, entry['source_sha256'], entry['pt_br']))
            entries = kept + [canonical_entry(indices, unit['source_sha256'], text)]
            merged += 1
            changed = True
        if changed:
            write_tracked(sheet, entries)
    print(f'{merged} texts merged, {refused} refused')
    return 1 if refused else 0


def cmd_validate(args):
    game = Game(args.game)
    glossary, problems = load_glossary()
    report = Report(errors=list(problems))
    usable = tracked_usable(game, glossary, report)
    for sheet, records in usable.items():
        try:
            data = generate_sheet(game.raw(sheet), records)
            report.errors += [f'{sheet} generated: {p}' for p in check_generated(game.raw(sheet), data, records)]
        except ValueError as e:
            report.errors.append(f'{sheet}: {e}')
    for line in report.errors:
        print('error:', line)
    for line in report.warnings:
        print('warning:', line)
    for line in report.notes:
        print('note:', line)
    count = sum(len(r) for r in usable.values())
    print(f'{count} records translated in {len(usable)} sheets; glossary {len(glossary)} terms; '
          f'{len(report.errors)} errors, {len(report.warnings)} warnings')
    return 1 if report.errors else 0


def cmd_generate(args):
    game = Game(args.game)
    glossary, _ = load_glossary()
    report = Report()
    usable = tracked_usable(game, glossary, report)
    out = CACHE / 'msgsheet'
    out.mkdir(parents=True, exist_ok=True)
    for sheet, records in usable.items():
        raw = game.raw(sheet)
        data = generate_sheet(raw, records)
        problems = check_generated(raw, data, records)
        if problems:
            sys.exit(f'{sheet}: {"; ".join(problems)}')
        (out / f'{sheet}_US.fms').write_bytes(data)
        print(f'{sheet}_US.fms: {len(records)} of {len(fms.parse(raw).records)} records translated')
    for line in report.errors:
        print('left in English:', line)
    print(f'{len(usable)} sheets in {out}')
    return 0


def cmd_reference(args):
    reference = load_reference(args.reference)
    if reference is None:
        sys.exit(f'{args.reference}: not found')
    game = Game(args.game)
    context = {lang: {} for lang in ('en',) + CONTEXT_LANGUAGES + ('ja',)}
    for sheet in reference.sheets:
        if sheet in SHEETS:
            for language, records in contexts(game, sheet).items():
                context[language][sheet] = records
    reports = reference_audit(context, reference.sheets, reference.duplicates)
    totals = Counter()
    for sheet, info in reports.items():
        if info.get('unknown_sheet'):
            print(f'{sheet}: not a US slot sheet, skipped')
            continue
        print(f'{sheet}: {info["records"]} records, {info["translated"]} translated of {info["installed"]} installed; '
              f'{info["source_matches"]} English matches, {len(info["source_mismatches"])} mismatches, '
              f'{len(info["missing"])} missing, {len(info["out_of_range"])} out of range, '
              f'{len(info["duplicates"])} duplicate indices')
        for key, label in (('source_mismatches', 'English differs at'), ('missing', 'no record for'),
                           ('out_of_range', 'beyond the sheet at'), ('duplicates', 'repeated index'),
                           ('empty', 'empty translation at'), ('encoding', 'bad encoding at'),
                           ('characters', 'characters outside PT-BR at'), ('spanish_copies', 'Spanish copy at'),
                           ('line_counts', 'more lines than the other languages at')):
            if info[key]:
                print(f'  {label}: {info[key]}')
        for problem, indices in info['tokens'].items():
            print(f'  tokens: {problem} at {indices}')
        if info['split_texts']:
            print(f'  same English, different PT-BR: {len(info["split_texts"])} texts')
        if reference.encodings.get(sheet) != 'utf-8':
            print(f'  file encoding: {reference.encodings.get(sheet)}')
        for key in ('records', 'translated', 'source_matches'):
            totals[key] += info[key]
        for key in ('source_mismatches', 'missing', 'out_of_range', 'duplicates', 'empty', 'encoding',
                    'characters', 'spanish_copies', 'line_counts', 'split_texts'):
            totals[key] += len(info[key])
        totals['token_records'] += sum(len(v) for v in info['tokens'].values())
    print(f'{totals["translated"]} of {totals["records"]} reference records translated; '
          f'{totals["source_matches"]} English matches, {totals["source_mismatches"]} mismatches, '
          f'{totals["missing"]} missing, {totals["out_of_range"]} out of range, {totals["duplicates"]} duplicate indices, '
          f'{totals["empty"]} empty, {totals["token_records"]} with token problems, '
          f'{totals["spanish_copies"]} Spanish copies, {totals["line_counts"]} line-count problems')
    return 0


def cmd_import_reference(args):
    game = Game(args.game)
    glossary, _ = load_glossary()
    reference = load_reference(args.reference)
    if reference is None:
        sys.exit(f'{args.reference}: not found')
    report = import_reference(game, glossary, reference)
    out = CACHE / 'work'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'import-report.json').write_bytes(
        (json.dumps(report, ensure_ascii=False, indent=1) + '\n').encode('utf-8'))
    (out / 'review-queue.md').write_bytes(import_review_text(report).encode('utf-8'))
    totals = Counter()
    for sheet, counts in report['sheets'].items():
        print(f'{sheet}: {counts["source"]} records, {counts["marcelo"]} Marcelo, '
              f'{counts["translated"]} PT-BR ({counts.get("imported_new", 0)} new, {counts.get("existing", 0)} kept), '
              f'{counts.get("rejected", 0)} rejected, {counts.get("missing", 0)} missing, '
              f'{counts.get("unchanged", 0)} unchanged, {counts.get("non_text", 0)} non-text')
        for key, value in counts.items():
            totals[key] += value
    print(f'{totals["translated"]} records translated in {len(report["sheets"])} sheets; '
          f'{totals.get("imported_new", 0)} imported, {totals.get("existing", 0)} kept, '
          f'{totals.get("rejected", 0)} rejected, {totals.get("missing", 0)} missing; '
          f'{len(report["corrections"]["token"])} token, {len(report["corrections"]["character"])} character, '
          f'{len(report["corrections"]["line"])} line and {len(report["corrections"]["glossary"])} glossary corrections')
    print(f'review queue: {out / "review-queue.md"}')
    return 0


# ------------------------------------------------------------------ the builds not in English

CORPUS_SHEETS = ('AbilityList', 'BonusScore', 'ChallengeQuest', 'CustomItem', 'DefaultBone', 'EventMsg', 'ItemList', 'ObjectParam',
                 'RoundContinue', 'SystemMsg', 'Temple', 'TreasureArt', 'Tutorial')
SOURCE_HASH_LENGTH = 16
# The slot each build plays PT-BR in. A folder missing from the prepared games is a build with no local data.
SLOTS = {
    'CUSA10454 1.00 USA': ('v1.03', 'DRACRO_US.CPK', 'US', None),
    'CUSA10454 1.01 USA': ('CUSA10454-1.01', 'DRACRO_US.CPK', 'US', None),
    'CUSA07001 1.00 Japan': ('CUSA07001-1.00', 'DRACRO.CPK', '', 'JP'),
    'CUSA07001 1.02 Japan': ('CUSA07001-1.02', 'DRACRO.CPK', '', 'JP'),
    'CUSA07001 1.03 Japan': ('CUSA07001-1.03', 'DRACRO.CPK', '', 'JP'),
    'CUSA10209 1.00 Asia/Hong Kong': ('CUSA10209-1.00', 'DRACRO_CK.CPK', 'CT', 'CT'),
    'CUSA10209 1.02 Asia/Hong Kong': ('CUSA10209-1.02', 'DRACRO_CK.CPK', 'CT', 'CT'),
    'CUSA10487 1.03 Europe': ('CUSA10487-1.03', 'DRACRO_EU.CPK', 'US', None),
}


def corpus_entries(sheet):
    return load_tracked(tracked_path(sheet))[0]['entries']


def slot_path(sheet, suffix):
    if suffix:
        return f'msgsheet/{sheet}_{suffix}.fms'
    return f'msgsheet/{JAPANESE_SHEET.get(sheet, sheet)}.fms'


def source_hash(text):
    return sha256(text)[:SOURCE_HASH_LENGTH]


def arguments(text):
    """The printf conversions in order and the button glyphs, what a slot must share with the translation."""
    found = tokens(text)
    return [t for t in found if is_printf(t) and t[1] != ' '], sorted(t for t in found if t.startswith('@#('))


def tag_keys(records):
    return {record for record in records[TAG_KEYS.start:TAG_KEYS.stop]}


def exclusion(record, text, keys):
    """Why this slot's record cannot safely show `text`; None when it can."""
    if not record.strip():
        return 'empty'
    ours, theirs = arguments(text), arguments(record)
    if ours[0] != theirs[0]:
        return 'printf'
    if ours[1] != theirs[1]:
        return 'button'
    if any(t not in keys for t in tokens(text) if t.startswith('##(')):
        return 'tag'
    return None


def slot_sheets(folder, archive_name, suffix):
    archive = Cpk(folder / archive_name)
    return {sheet: archive.read(archive.find(slot_path(sheet, suffix))) for sheet in CORPUS_SHEETS}


def source_map(name, raw):
    keys = tag_keys(fms.parse(raw['SystemMsg']).records)
    sheets = []
    for sheet in CORPUS_SHEETS:
        records = fms.parse(raw[sheet]).records
        hashes, excluded = [], []
        for entry in corpus_entries(sheet):
            for index in entry_indices(entry):
                why = 'no record' if index >= len(records) else exclusion(records[index], entry['pt_br'], keys)
                if why:
                    excluded.append([index, why])
                else:
                    hashes.append([index, source_hash(records[index])])
        sheets.append({'sheet': sheet, 'records': len(records), 'sha256': hashlib.sha256(raw[sheet]).hexdigest(),
                       'hashes': sorted(hashes), 'excluded': sorted(excluded)})
    return {'source': name, 'sheets': sheets}


def dump_source(data):
    rows = []
    for sheet in data['sheets']:
        head = {k: sheet[k] for k in ('sheet', 'records', 'sha256')}
        body = ',\n'.join(f'      {json.dumps(row, ensure_ascii=False)}' for row in sheet['hashes'])
        excluded = json.dumps(sheet['excluded'])
        rows.append(f'    {json.dumps(head)[:-1]}, "excluded": {excluded}, "hashes": [\n{body}\n    ]}}')
    return '{\n  "source": ' + json.dumps(data['source']) + ',\n  "sheets": [\n' + ',\n'.join(rows) + '\n  ]\n}\n'


def cmd_sources(args):
    SOURCES_DIR.mkdir(parents=True, exist_ok=True)
    written = {}
    for label, (folder, archive, suffix, source) in SLOTS.items():
        game = args.prepared / folder
        if source is None or source in written or not (game / archive).is_file():
            continue
        data = source_map(source, slot_sheets(game, archive, suffix))
        (SOURCES_DIR / f'{source}.json').write_bytes(dump_source(data).encode('utf-8'))
        written[source] = label
        print(f'{source}: from {label}')
    return 0 if written else 1


def cmd_parity(args):
    corpus = {sheet: corpus_entries(sheet) for sheet in CORPUS_SHEETS}
    total = sum(len(entry_indices(entry)) for entries in corpus.values() for entry in entries)
    print(f'corpus {total} records')
    stale = 0
    for label, (folder, archive, suffix, source) in SLOTS.items():
        game = args.prepared / folder
        if not (game / archive).is_file():
            print(f'{label}: DATA UNAVAILABLE ({folder}/{archive})')
            continue
        raw = slot_sheets(game, archive, suffix)
        applied, reasons, changed = 0, Counter(), []
        mapped = json.loads((SOURCES_DIR / f'{source}.json').read_text('utf-8')) if source else None
        for sheet in CORPUS_SHEETS:
            records = fms.parse(raw[sheet]).records
            if mapped:
                row = next(s for s in mapped['sheets'] if s['sheet'] == sheet)
                if hashlib.sha256(raw[sheet]).hexdigest() != row['sha256']:
                    changed.append(sheet)
                hashes = dict(row['hashes'])
                reasons.update(why for _, why in row['excluded'])
            for entry in corpus[sheet]:
                for index in entry_indices(entry):
                    if index >= len(records):
                        reasons['no record'] += 1
                    elif mapped:
                        if index in hashes:
                            if hashes[index] == source_hash(records[index]):
                                applied += 1
                            else:
                                reasons['source changed'] += 1
                    elif entry['source_sha256'] == sha256(records[index]):
                        applied += 1
                    else:
                        reasons['source changed'] += 1
        stale += reasons['source changed']
        print(f'{label}: applied {applied}/{total}, kept native {dict(reasons)}'
              + (f', sheets differing from the map: {changed}' if changed else ''))
    return 1 if stale else 0


def cmd_format(_args):
    for path in sorted(SHEETS_DIR.glob('*.json')) if SHEETS_DIR.is_dir() else []:
        data, _, error = load_tracked(path)
        if error:
            sys.exit(f'{path.name}: {error}')
        write_tracked(path.stem, data['entries'])
    return 0


def cmd_selftest(_args):
    check = []

    def expect(condition, what):
        check.append(what)
        if not condition:
            sys.exit(f'selftest failed: {what}')

    expect(tokens('@#(34)Go ##(TAG) %d%% %3d @c(1,2,3)x@/c @@ @C 100$ %.2f %08lx 5%@/c') ==
           ['@#(34)', '##(TAG)', '%d', '%%', '%3d', '@c(1,2,3)', '@/c', '@@', '@C', '$', '%.2f', '%08lx', '%', '@/c'],
           'token split')
    expect(tokens('@ end @') == ['@ ', '@'] and tokens('%!') == ['%'] and tokens('##(open') == [], 'lone markers')
    expect(tokens('50% chance') == ['% c'], 'a percent sign before a letter reads as a conversion, as printf does')
    english = ['Open @c(10)##(P_INN)@/c', 'Floor %d of %d', 'Tag', 'Plain text here']
    expect(record_errors('X', english, 0, 'Abrir @c(10)##(P_INN)@/c') == [], 'kept tokens pass')
    expect(record_errors('X', english, 1, 'Andar %d de %d') == [], 'printf kept')
    expect(record_errors('X', english, 1, 'Andar %d')[0].startswith('printf'), 'printf dropped')
    expect(record_errors('X', english, 0, 'Abrir ##(P_INN)')[0].startswith('control'), 'color dropped')
    expect(record_errors('X', ['About 50% of eggs'], 0, 'Cerca de 50% dos ovos') == [], 'a prose percent sign')
    expect(record_errors('X', ['About 50% of eggs'], 0, 'Cerca de %d ovos') != [], 'no conversion added to prose')
    expect(record_errors('X', ['Chance %d%'], 0, 'Chance %d') != [], 'percent signs kept in formatted text')
    expect(record_errors('X', english, 3, 'Texto ¿simples?')[0].startswith('characters'), 'Spanish mark refused')
    expect(record_errors('X', english, 3, 'Ação não é pão') == [], 'PT-BR letters pass')
    expect(record_errors('X', english, 3, 'Tab\there') != [], 'control character refused')
    expect(record_errors('SystemMsg', ['x'] * 300, 120, 'y') == ['a tag key, never translated'], 'tag key refused')
    context = {'en': english, 'es': ['Abrir @c(10)##(P_INN)@/c', 'Piso %d de %d', 'Etiqueta', 'Texto simple aquí'],
               'fr': None, 'de': None, 'it': None}
    expect(record_warnings('X', context, 3, 'Plain text here', set()) == ['still the English text'], 'untouched English')
    expect(record_warnings('X', context, 0, 'Open @c(10)##(P_INN)@/c', {'Open'}) == [], 'kept names pass')
    expect('identical to the Spanish text' in record_warnings('X', context, 3, 'Texto simple aquí', set()), 'Spanish copy')
    expect(any(w.startswith('Spanish words') for w in record_warnings('X', context, 3, 'Texto y más', set())),
           'Spanish words')
    expect(line_count_error(context, 3, 'Texto\nem\nlinhas') != '' and not line_count_error(context, 3, 'Texto'),
           'line budget')
    expect('control sequences reordered' in record_warnings('X', {'en': ['%s @#(1) @#(2)']}, 0, '@#(2) %s @#(1)', set()),
           'reorder noticed')
    expect(reference_tokens('A ##(X) @c(1)B@/c %d', 'C ##(X) @c(1)D@/c %d') == [], 'reference tokens kept')
    expect(reference_tokens('A ##(X) %d', 'B ##(Y) %d') != [], 'reference tag change')
    expect(reference_tokens('A %s %d', 'B %d %s') != [], 'reference printf reorder')
    sheets = {'X': {0: {'i': 0, 'en': 'Hello', 'pt': 'Ola'}, 1: {'i': 1, 'en': 'Old', 'pt': 'Velho'}}}
    audit = reference_audit({'en': {'X': ['Hello', 'New']}, 'es': {'X': ['Hola', 'Nuevo']}}, sheets, {'X': []})
    expect(audit['X']['source_matches'] == 1 and audit['X']['source_mismatches'] == [1], 'reference alignment')
    expect(audit['X']['translated'] == 2, 'reference translated count')

    header = b'FMSB' + bytes(4) + (0x20).to_bytes(4, 'little') + bytes(8) + (3).to_bytes(4, 'little') + b'\x03' + bytes(7)
    sheet = fms.Sheet(header, ['One', 'Two %d', 'Three'], b'FEOC' + bytes(12))
    raw = fms.build(sheet)
    expect(fms.build(fms.parse(raw)) == raw and fms.parse(raw).records == sheet.records, 'FMS round trip')
    usable = {1: 'Dois %d'}
    generated = generate_sheet(raw, usable)
    expect(fms.parse(generated).records == ['One', 'Dois %d', 'Three'], 'generated sheet falls back to English')
    expect(check_generated(raw, generated, usable) == [], 'generated sheet checks')
    expect(check_generated(raw, generated, {}) != [], 'generated sheet check catches a change')

    entries = [canonical_entry([5, 2], sha256('x'), 'y'), canonical_entry([1], sha256('z'), 'w')]
    text = dump_sheet('X', entries)
    expect(json.loads(text)['entries'][0]['index'] == 1 and json.loads(text)['entries'][1]['index'] == [2, 5],
           'canonical layout')
    report = Report()
    english = ['Hello there', 'x', 'Floor %d']
    data = {'sheet': 'X', 'entries': [canonical_entry([0], sha256('Hello there'), 'Olá'),
                                      canonical_entry([2], sha256('Floor 9'), 'Andar %d'),
                                      canonical_entry([0], sha256('Hello there'), 'Oi')]}
    usable = check_sheet_file('X', data, '', {'en': english}, set(), report)
    expect(usable == {0: 'Olá'}, 'stale and repeated entries left out')
    expect(any('stale' in e for e in report.errors) and any('twice' in e for e in report.errors), 'their errors')
    print(f'selftest: {len(check)} checks passed')
    return 0


def main():
    global SHEETS_DIR
    sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('sources', 'parity'):
        sub.add_parser(name).add_argument('--prepared', type=Path, default=PREPARED)
    for name in ('batches', 'extract', 'merge', 'validate', 'generate', 'reference', 'import-reference', 'format'):
        p = sub.add_parser(name)
        p.add_argument('--sheets', type=Path, default=SHEETS_DIR, help='the tracked sheets folder (for trying changes)')
        if name == 'format':
            continue
        p.add_argument('--game', type=Path, default=DEFAULT_GAME)
        if name in ('extract', 'reference', 'import-reference'):
            p.add_argument('--reference', type=Path, default=REFERENCE)
        if name == 'extract':
            p.add_argument('--batch')
        if name == 'merge':
            p.add_argument('files', type=Path, nargs='+')
    sub.add_parser('selftest')
    args = parser.parse_args()
    SHEETS_DIR = getattr(args, 'sheets', SHEETS_DIR)
    return globals()['cmd_' + args.command.replace('-', '_')](args)


if __name__ == '__main__':
    sys.exit(main())
