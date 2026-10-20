"""Import inventory of a prepared image, for planning only.

Sources: build/image/<ver>/image.json (written by prepare_image.py) and the output of
`dragons_crown_pro --check-imports` (the runtime's own answer to "which imports can you bind").

Usage: python scripts/import_inventory.py [--version 1.03] [--check-log FILE]
           [--reference NAME=import_names.inc ...] [--source NAME=DIR ...]
Without --check-log the loader found under build/cmake is run with --check-imports; no game code executes.
Writes build/image/<ver>/imports.json.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
MISSING = re.compile(r'^MISSING: (\S+) \((.*)\)$', re.M)
INC_ENTRY = re.compile(r'\{"([^"]+)","([^"]*)"\}')


def library(scoped):
    return scoped.split('#', 1)[1]


def run_check(version):
    loader = next(iter(sorted((ROOT / 'build' / 'cmake').glob('*/bin/dragons_crown_pro.exe'), key=lambda p: p.stat().st_mtime, reverse=True)), None)
    image = ROOT / 'build' / 'image' / f'v{version}'
    if not loader or not (image / 'boot.bin').is_file():
        sys.exit('No loader or boot image: run tools/build.ps1 and tools/prepare-game.ps1, or pass --check-log')
    result = subprocess.run([str(loader), str(image / 'boot.bin'), '--content-profile', str(image / 'content.bin'),
                             '--app0', str(ROOT / 'game' / 'prepared' / f'v{version}'), '--user', str(ROOT / 'user'),
                             '--check-imports'], capture_output=True, text=True, errors='replace', stdin=subprocess.DEVNULL)
    return result.stdout


def reference_imports(path):
    return {m.group(1) for m in INC_ENTRY.finditer(Path(path).read_text(errors='replace'))}


def source_symbols(directories):
    """Every identifier in the C/C++ sources under `directories` (a symbol named there is implemented or declared there)."""
    tokens = set()
    for directory in directories:
        for path in Path(directory).rglob('*'):
            if path.suffix in ('.c', '.cpp', '.h', '.hpp', '.inl') and path.name != 'import_names.inc':
                tokens.update(re.findall(r'[A-Za-z_][A-Za-z0-9_]*', path.read_text(errors='replace')))
    return tokens


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', default='1.03')
    parser.add_argument('--check-log', type=Path)
    parser.add_argument('--reference', action='append', default=[], metavar='NAME=FILE',
                        help="a reference project's src/import_names.inc (the imports of the game it was made for)")
    parser.add_argument('--source', action='append', default=[], metavar='NAME=DIR[,DIR]',
                        help='source folders of a reference port: which missing symbols its code names')
    args = parser.parse_args()
    sys.stdout.reconfigure(errors='replace')

    out = ROOT / 'build' / 'image' / f'v{args.version}'
    report = json.loads((out / 'image.json').read_text())
    imports = report['import_list']
    unresolved = set(report['unresolved_imports'])
    check = args.check_log.read_text(errors='replace') if args.check_log else run_check(args.version)
    missing = {m.group(1) for m in MISSING.finditer(check)}
    if not missing and 'Imports:' not in check:
        sys.exit('check-imports output has no summary line; the loader did not finish')
    game_provided = [n for n in imports if n not in unresolved]
    runtime_bound = [n for n in imports if n in unresolved and n not in missing]

    table = collections.OrderedDict()
    for n in imports:
        row = table.setdefault(library(n), dict(total=0, game_provided=0, runtime=0, missing=0))
        row['total'] += 1
        row['game_provided' if n not in unresolved else 'missing' if n in missing else 'runtime'] += 1
    result = dict(total=len(imports), game_provided=len(game_provided), runtime_implemented=len(runtime_bound),
                  missing=sorted(missing), libraries=table, references={})
    for spec in args.reference:
        name, _, path = spec.partition('=')
        known = reference_imports(path)
        need = [n for n in imports if n in unresolved]
        result['references'][name] = dict(
            reference_imports=len(known), shared_with_runtime_needed=sum(n in known for n in need),
            shared_missing=sorted(n for n in missing if n in known),
            not_in_reference=sorted(n for n in missing if n not in known),
            reference_libraries=len({library(n) for n in known}))
    symbol = {m.group(1): m.group(2) for m in MISSING.finditer(check)}
    for spec in args.source:
        name, _, dirs = spec.partition('=')
        tokens = source_symbols(dirs.split(','))
        named = sorted(n for n in missing if symbol[n] in tokens)
        result.setdefault('sources', {})[name] = dict(named_missing=len(named), absent_missing=sorted(symbol[n] for n in missing if n not in named))
    (out / 'imports.json').write_text(json.dumps(result, indent=2) + '\n')

    print(f'imports {len(imports)}: {len(game_provided)} provided by the game\'s own modules, '
          f'{len(runtime_bound)} implemented by the runtime, {len(missing)} missing')
    print(f'{"library":32}{"total":>7}{"game":>7}{"runtime":>9}{"missing":>9}')
    for lib, row in sorted(table.items(), key=lambda kv: (-kv[1]['missing'], -kv[1]['total'], kv[0])):
        print(f'{lib:32}{row["total"]:>7}{row["game_provided"]:>7}{row["runtime"]:>9}{row["missing"]:>9}')
    for name, ref in result['references'].items():
        print(f'vs {name}: {ref["reference_imports"]} imports in {ref["reference_libraries"]} libraries; '
              f'{ref["shared_with_runtime_needed"]} of our {len(unresolved)} runtime imports appear there; '
              f'{len(ref["shared_missing"])} of our {len(missing)} missing imports appear there')
    for name, src in result.get('sources', {}).items():
        print(f'source {name}: names {src["named_missing"]} of our {len(missing)} missing imports; absent: {len(src["absent_missing"])}')
    print(f'Output: {out / "imports.json"}')


if __name__ == '__main__':
    main()
