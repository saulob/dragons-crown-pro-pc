"""Prepare a runnable game tree from the user's packages.

  game/*.pkg                       the user's packages (read-only)
  game/extracted/<folder>          what one package contains, named after the build it finishes (resumable)
  game/prepared/<folder>           the /app0 tree the loader uses: the build's layers laid over each other (hard links)

A build is its base package and then its updates (scripts/game_profiles.py). <folder> is the build's folder: v1.03 for
CUSA10454, CUSA07001-1.00, CUSA07001-1.02 or CUSA07001-1.03 for CUSA07001, CUSA10209-1.00 or CUSA10209-1.02 for CUSA10209. A package that
several builds share (the base) is extracted once.

Usage: python scripts/prepare_game.py [--title CUSA07001] [--version 1.02] [--scope core|full] [--game-root DIR]
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_profiles  # noqa: E402
import game_view  # noqa: E402
import pkg_info  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def fail(message):
    print(f'error: {message}', file=sys.stderr)
    sys.exit(1)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def find_build_packages(game_root, title, version):
    """The build to prepare and its packages, one per layer: (title, version, profile, [(path, info), ...]).

    Packages are classified by their metadata, not by name. A build is complete when each of its layers has one package.
    """
    by_identity = {}
    for path in sorted(game_root.glob('*.pkg')):
        try:
            info = pkg_info.inspect(str(path))
        except (OSError, ValueError) as error:
            fail(f'{path.name}: {error}')
        identity = game_profiles.package_identity(info)
        if identity is None:
            print(f'skipped: {path.name} ({pkg_info.title_id(info)} {info["param_sfo"].get("APP_VER")}, '
                  f'{info["param_sfo"].get("CATEGORY")}) is not a package of a supported build')
            continue
        by_identity.setdefault(identity, []).append((path, info))

    builds = []
    for found_title, profile in game_profiles.PROFILES.items():
        if title and found_title != title:
            continue
        for found_version, build in profile['versions'].items():
            if version and found_version != version:
                continue
            found = [by_identity.get(game_profiles.layer_identity(found_title, layer), []) for layer in build['layers']]
            if all(len(candidates) == 1 for candidates in found):
                builds.append((found_title, found_version, profile, [candidates[0] for candidates in found]))
    if not builds:
        wanted = ' '.join(x for x in (title, version) if x)
        fail(f'no complete supported build{" for " + wanted if wanted else ""} in {game_root}')
    if len(builds) > 1:
        names = ', '.join(f'{t} {v}' for t, v, _, _ in builds)
        fail(f'the packages complete several builds ({names}); pass --version to choose one')
    return builds[0]


def progress_printer(label):
    last = [-1]

    def report(done, total, rate):
        percent = done * 100 // max(total, 1)
        if percent >= last[0] + 5 or percent == 100 and last[0] != 100:
            last[0] = percent
            print(f'  {label}: {percent:3d}%  {done >> 20} of {total >> 20} MB  {rate:.0f} MB/s', flush=True)
    return report


def extract(pkg_path, target, only, label):
    import pkg_extract
    if pkg_extract.is_extracted(str(pkg_path), str(target), only):
        print(f'  {label}: already extracted ({target})')
        return
    scope = 'everything' if not only else 'executables, modules, metadata'
    print(f'  {label}: extracting {scope} -> {target}', flush=True)
    started = time.time()
    try:
        pkg_extract.extract(str(pkg_path), str(target), progress_printer(label), None, only)
    except (OSError, ValueError) as error:
        fail(f'{label}: extraction failed: {error}')
    print(f'  {label}: done in {time.time() - started:.0f}s')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game-root', type=Path, default=ROOT / 'game')
    parser.add_argument('--title', help='title to prepare: CUSA10454, CUSA07001 or CUSA10209')
    parser.add_argument('--version', help='profile version to prepare (1.00, 1.02 or 1.03 for CUSA07001)')
    parser.add_argument('--scope', choices=('core', 'full'), default='core',
                        help='core: executables, modules and metadata only; full: all game data')
    args = parser.parse_args()
    sys.stdout.reconfigure(errors='replace')

    if args.title and args.title not in game_profiles.PROFILES:
        fail(f'unsupported title {args.title}; supported: {", ".join(game_profiles.PROFILES)}')
    versions = {v for p in game_profiles.PROFILES.values() for v in p['versions']}
    if args.version and args.version not in versions:
        fail(f'unsupported version {args.version}; supported: {", ".join(sorted(versions))}')
    root = args.game_root.resolve()
    title, version, profile, packages = find_build_packages(root, args.title, args.version)
    build = profile['versions'][version]
    full = args.scope == 'full'
    only = None if full else list(profile['core_patterns'])

    for index, (package, info) in enumerate(packages):
        sfo = info['param_sfo']
        print(f'layer {index}: {package.name}  ({title} APP_VER {sfo.get("APP_VER")}, VERSION {sfo.get("VERSION")}, '
              f'{sfo.get("CATEGORY")})')
        if info['content_id'] != profile['content_id']:
            fail(f'{package.name}: content id {info["content_id"]} does not match the profile')
        if not info['fake_pkg']:
            fail(f'{package.name}: not a fake PKG: it needs console keys and cannot be unpacked here')

    layers = []
    for index, (package, info) in enumerate(packages):
        owner = game_profiles.finishing_version(title, game_profiles.package_identity(info))
        layers.append(root / 'extracted' / profile['versions'][owner]['folder'])
        extract(package, layers[-1], only, f'layer {index}')

    view = root / 'prepared' / build['folder']
    stats = game_view.build_view(layers, view, f'{title} {version}')
    print(f'view   : {view}  ({stats["link"]} links, {stats["copy"]} copies)')

    problems = []
    if game_profiles.version_of(view) != version:
        problems.append(f'sce_sys/param.sfo is not the finished {version} game')
    eboot = view / profile['entry']
    if not eboot.is_file():
        problems.append(f'{profile["entry"]} missing')
    elif sha256(eboot) != build['eboot_sha256']:
        problems.append(f'{profile["entry"]} sha256 does not match the known {version} build')
    for name in profile['modules']:
        if not (view / 'sce_module' / name).is_file():
            problems.append(f'sce_module/{name} missing')
    for name in build['extra_files']:
        if not (view / name).is_file():
            problems.append(f'{name} missing')
    if full and build['regional_archive'] and not (view / build['regional_archive']).is_file():
        problems.append(f'{build["regional_archive"]} missing')
    if problems:
        fail('prepared tree is not valid:\n  ' + '\n  '.join(problems))
    scope = 'full' if all(json.loads((layer / '.pkg_extracted').read_text())['only'] is None for layer in layers) else 'core'
    (view / game_view.MARKER).write_text(json.dumps(
        {**json.loads((view / game_view.MARKER).read_text()), 'version': version, 'scope': scope}, indent=2))
    print(f'ready  : {view}  ({title} {version}, extracted scope {scope})')


if __name__ == '__main__':
    main()
