"""Read-only report on the text languages of a prepared game folder: per language suffix of each CPK archive, how many
files it has and what letters its SystemMsg sheet is written in. Prints counts only, never game text.

Usage: python scripts/text_languages.py <game folder (app0)>
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'prompts'))
from cpk import Cpk  # noqa: E402

SUFFIX = re.compile(r'^(.*/)?([^/]+)_([A-Z]{2})(\.[^./]+)$')
RUSSIAN = set(range(0x410, 0x450)) | {0x401, 0x451}


def letters(sheet):
    text = sheet.decode('utf-8', errors='ignore')
    latin = sum(c.isascii() and c.isalpha() for c in text)
    cyrillic = [ord(c) for c in text if 0x400 <= ord(c) <= 0x4ff]
    other = sum(c not in RUSSIAN for c in cyrillic)
    yery = sum(c in (0x42b, 0x44b) for c in cyrillic)
    cjk = sum(0x3040 <= ord(c) <= 0x9fff or 0xac00 <= ord(c) <= 0xd7af for c in text)
    russian = len(cyrillic) > latin and yery > 0 and other * 100 < len(cyrillic)
    return (f'{latin} Latin, {len(cyrillic)} Cyrillic ({other} outside Russian, {yery} yery), {cjk} CJK/Hangul'
            f'{" -> Russian" if russian else ""}')


def report(archive):
    cpk = Cpk(archive)
    print(f'{archive.name}: {archive.stat().st_size:,} bytes, {len(cpk.entries)} entries')
    files = {}
    for entry in cpk.entries:
        m = SUFFIX.match(entry['path'])
        if m:
            files.setdefault(m.group(3), set()).add((m.group(1) or '') + m.group(2) + m.group(4))
    every = set().union(*files.values()) if files else set()
    required = {f for f in every if 2 * sum(f in names for names in files.values()) >= len(files)}
    for suffix in sorted(files):
        names = files[suffix]
        line = f'  {suffix}: {len(names)} files, {len(required & names)} of the {len(required)} most languages have'
        missing = sorted(required - names)
        if missing:
            line += f', missing {len(missing)} (first: {missing[0]})'
        try:
            line += '; SystemMsg: ' + letters(cpk.read(cpk.find(f'msgsheet/SystemMsg_{suffix}.fms')))
        except KeyError:
            line += '; no SystemMsg sheet'
        print(line)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    args = parser.parse_args()
    archives = sorted(p for p in args.game.iterdir() if p.suffix.lower() == '.cpk' and p.name.upper() != 'DRACRO.CPK')
    if not archives:
        print(f'{args.game}: no regional archive')
        return 1
    for archive in archives:
        report(archive)
    return 0


if __name__ == '__main__':
    sys.exit(main())
