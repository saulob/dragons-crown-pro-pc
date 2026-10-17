"""Per-title facts for the games this port knows, keyed by TITLE_ID in sce_sys/param.sfo.

A version's layers are its packages, the base
first and then the updates applied over it; the last layer's param.sfo is the finished game's. launcher/game_profile.cpp
mirrors these values, and tests/launcher_tests.cpp checks that the two agree.
"""
from pathlib import Path

from ps4_module import parse_sfo

PROFILES = {
    'CUSA10454': {
        'name': "Dragon's Crown Pro",
        'region': 'USA',
        'content_id': 'UP2611-CUSA10454_00-DRAGONSCROWNPRO1',
        'versions': {
            '1.00': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.03', 'update': False},),
                'eboot_sha256': 'f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a',
                'folder': 'v1.03', 'regional_archive': 'DRACRO_US.CPK', 'extra_files': (),
            },
        },
        'entry': 'eboot.bin',
        'extra_executables': (),
        # Linked after the eboot in this order (libc.prx needs libSceFios2.prx, as in the other ports).
        'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
        # What the loader needs; the game data (DRACRO*.CPK) is only unpacked with --scope full.
        'core_patterns': ('eboot.bin', 'sce_module/*', 'sce_sys/*', '*.plt'),
    },
    'CUSA07001': {
        'name': "Dragon's Crown Pro",
        'region': 'Japan',
        'content_id': 'JP0005-CUSA07001_00-DRAGONSCROWN0001',
        'versions': {
            '1.00': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.00', 'update': False},),
                'eboot_sha256': 'e838e5c551f4ea2d13a13246dcd5474a888c56aa8abf5110f1219c0ceb0ae05f',
                'folder': 'CUSA07001-1.00', 'regional_archive': None, 'extra_files': (),
            },
            # Each update is laid over the base package alone: 1.03 replaces every file 1.02 changes.
            '1.02': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.00', 'update': False},
                           {'app_ver': '01.02', 'sfo_version': '01.00', 'update': True}),
                'eboot_sha256': 'e512685aef4fa1e50eafec8c9699b611992356cbae84e9b0147febceb49843fe',
                'folder': 'CUSA07001-1.02', 'regional_archive': 'DRACRO_PATCH_JP.CPK',
                'extra_files': ('sce_discmap_patch.plt',),
            },
            '1.03': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.00', 'update': False},
                           {'app_ver': '01.03', 'sfo_version': '01.00', 'update': True}),
                'eboot_sha256': 'dccfd331b02d462f6586330664b230a887928ed1b3909f8625b49623630d1a64',
                'folder': 'CUSA07001-1.03', 'regional_archive': 'DRACRO_PATCH_JP.CPK',
                'extra_files': ('sce_discmap_patch.plt',),
            },
        },
        'entry': 'eboot.bin',
        'extra_executables': (),
        'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
        'core_patterns': ('eboot.bin', 'sce_module/*', 'sce_sys/*', '*.plt'),
    },
    'CUSA10209': {
        'name': "Dragon's Crown Pro",
        'region': 'Asia/Hong Kong',
        'content_id': 'HP0177-CUSA10209_00-DRAGONSCROWN0001',
        'versions': {
            # Same executable as 1.02; the package metadata tells the builds apart.
            '1.00': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.02', 'update': False},),
                'eboot_sha256': 'd37cbc39d84524dd38b818a18bba26ec556bb13e075c493fc927163fec8d039b',
                'folder': 'CUSA10209-1.00', 'regional_archive': 'DRACRO_CK.CPK', 'extra_files': (),
            },
            # TARGET_APP_VER (01.01 in this build) is not an identity field.
            '1.02': {
                'layers': ({'app_ver': '01.02', 'sfo_version': '01.00', 'update': False},),
                'eboot_sha256': 'd37cbc39d84524dd38b818a18bba26ec556bb13e075c493fc927163fec8d039b',
                'folder': 'CUSA10209-1.02', 'regional_archive': 'DRACRO_CK.CPK', 'extra_files': (),
            },
        },
        'entry': 'eboot.bin',
        'extra_executables': (),
        'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
        'core_patterns': ('eboot.bin', 'sce_module/*', 'sce_sys/*', '*.plt'),
    },
}


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        return parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, KeyError):
        return None


def profile(game_or_title):
    """The profile for a TITLE_ID or a game folder, or None for an unknown title."""
    key = str(game_or_title)
    if key not in PROFILES:
        key = title_id(game_or_title) or ''
    return PROFILES.get(key)


def finished_layer(build):
    """The layer whose param.sfo the installed game has."""
    return build['layers'][-1]


def layer_identity(title, layer):
    """(TITLE_ID, APP_VER, VERSION, is an update) of the package a layer is."""
    return title, layer['app_ver'], layer['sfo_version'], layer['update']


def package_identity(info):
    """The identity of a package, as layer_identity() names it, or None when it is not a package of a supported title."""
    sfo = info['param_sfo']
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found or sfo.get('CONTENT_ID') != found['content_id']:
        return None
    return sfo['TITLE_ID'], sfo.get('APP_VER'), sfo.get('VERSION'), sfo.get('CATEGORY') == 'gp'


def finishing_version(title, identity):
    """The version of `title` whose finished game is the package with this identity, or None."""
    for version, build in PROFILES[title]['versions'].items():
        if layer_identity(title, finished_layer(build)) == identity:
            return version
    return None


def version_of(game):
    """The profile version whose finished param.sfo matches the game folder, or None."""
    try:
        sfo = parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError):
        return None
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found:
        return None
    return next((v for v, build in found['versions'].items()
                 if (finished_layer(build)['app_ver'], finished_layer(build)['sfo_version'])
                 == (sfo.get('APP_VER'), sfo.get('VERSION'))), None)


def package_layer(info):
    """(TITLE_ID, profile version, layer index) of the package of a supported build's finished game, or None.

    `info` is what scripts/pkg_info.py reads. The index is the package's place in the build's layers.
    """
    identity = package_identity(info)
    if identity is None:
        return None
    version = finishing_version(identity[0], identity)
    if version is None:
        return None
    return identity[0], version, len(PROFILES[identity[0]]['versions'][version]['layers']) - 1
