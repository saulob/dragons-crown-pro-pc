#!/usr/bin/env python3
"""The released PT-BR visuals for the builds that read the base stage files (Japan, Asia/Hong Kong).

    regional_visuals.py build    writes localization/pt-BR/visual/base-*.dcpx.z from the prepared games
    regional_visuals.py check    applies them to the prepared games and proves every label; exit 1 on a failure
    regional_visuals.py preview  writes before/after sheets to cache/localization/pt-BR/work/regional-visuals

The USA packs add *_Add atlases and the European-language layouts draw their labels from them. The base packs have no
such atlases: Japan and Hong Kong draw NOW + LOADING, YES, NO, the DATA books, "new" and the copyright words from their
ordinary atlases. Each released label is moved there: the released art replaces the regional sprite, and the layout's
position quads are set to where the released layout puts it. Only those blocks and quads enter the patches.
"""
import argparse
import json
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts' / 'prompts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bc7  # noqa: E402
import ftex  # noqa: E402
import visual_assets as va  # noqa: E402
from cpk import Cpk  # noqa: E402

PREPARED = REPO / 'game' / 'prepared'
VISUAL = REPO / 'localization' / 'pt-BR' / 'visual'
WORK = REPO / 'cache' / 'localization' / 'pt-BR' / 'work' / 'regional-visuals'
UNIT = 0.25  # layout units per texel of the released labels

PAD = 4
INK = 24
SEAM_OVERLAP = 3  # texels both sprites draw where a word is cut through a letter
LABEL_DIFFERENCE = 40  # the released books were recompressed whole: only a change this strong is the new word
MAX_PACK_BLOCK_SHARE = 0.05  # the labels, never a whole texture: the check fails a patch that grows past them
MAX_ART_DIFFERENCE = 12.0  # mean per-channel distance from the released art, what BC7 itself loses
MIN_SPRITE_CHANGE = 12.0  # the sprite must no longer be the regional word
POSITIONS, UV, QUADS, ELEMENTS, PARTS = 2, 1, 4, 6, 9


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


# ------------------------------------------------------------------ DCPX2

def dcpx2(targets):
    """targets: [{name, states: [(before, after)...], size, runs: [(offset, 16 bytes)...]}]; one run list, several sources."""
    out = bytearray(b'DCPX2\0\0\0' + struct.pack('<I', len(targets)))
    for target in targets:
        name = target['name'].encode('ascii')
        out += struct.pack('<H', len(name)) + name + struct.pack('<II', target['size'], len(target['states']))
        for before, after in target['states']:
            out += struct.pack('<II', before, after)
        out += struct.pack('<I', len(target['runs']))
        for offset, data in target['runs']:
            out += struct.pack('<I', offset) + data
    return bytes(out)


def parse_dcpx2(data):
    if data[:8] != b'DCPX2\0\0\0':
        raise ValueError('not a DCPX2 patch')
    count = struct.unpack_from('<I', data, 8)[0]
    pos = 12
    targets = []
    for _ in range(count):
        length = struct.unpack_from('<H', data, pos)[0]
        name = data[pos + 2:pos + 2 + length].decode('ascii')
        pos += 2 + length
        size, states = struct.unpack_from('<II', data, pos)
        pos += 8
        pairs = [struct.unpack_from('<II', data, pos + 8 * i) for i in range(states)]
        pos += 8 * states
        runs_count = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        runs = [(struct.unpack_from('<I', data, pos + 20 * i)[0], data[pos + 20 * i + 4:pos + 20 * i + 20]) for i in range(runs_count)]
        pos += 20 * runs_count
        targets.append({'name': name, 'size': size, 'states': pairs, 'runs': runs})
    return targets


# ------------------------------------------------------------------ layouts

class Layout:
    def __init__(self, data):
        self.data = bytearray(data)
        self.offsets = [struct.unpack_from('<Q', self.data, 0xb0 + 8 * i)[0] for i in range(11)]

    def box(self, group):
        points = [struct.unpack_from('<2f', self.data, self.offsets[POSITIONS] + 8 * (group * 6 + k)) for k in range(1, 6)]
        xs, ys = zip(*points)
        return min(xs), min(ys), max(xs), max(ys)

    def set_box(self, group, x0, y0, x1, y1):
        points = [((x0 + x1) / 2, (y0 + y1) / 2), (x0, y0), (x0, y1), (x1, y1), (x1, y0), (x0, y0)]
        struct.pack_into('<12f', self.data, self.offsets[POSITIONS] + 48 * group, *[v for p in points for v in p])

    def uv_box(self, group, width, height):
        points = [struct.unpack_from('<2f', self.data, self.offsets[UV] + 8 * (group * 6 + k)) for k in range(1, 6)]
        xs, ys = zip(*points)
        return round(min(xs) * width), round(min(ys) * height), round(max(xs) * width), round(max(ys) * height)

    def element_quads(self, element):
        """[(texture index, uv group, position group)] of an element; its quad count is the high half of the word."""
        first, counts = struct.unpack_from('<2I', self.data, self.offsets[ELEMENTS] + 28 * element + 16)
        quads = []
        for quad in range(first, first + (counts >> 16)):
            record = struct.unpack_from('<8H', self.data, self.offsets[QUADS] + 16 * quad)
            quads.append((self.data[self.offsets[QUADS] + 16 * quad + 7], record[5], record[7]))
        return quads

    def set_element_bounds(self, element, x0, y0, x1, y1):
        struct.pack_into('<4f', self.data, self.offsets[ELEMENTS] + 28 * element, x0, y0, x1, y1)

    def part_named(self, name):
        count = struct.unpack_from('<H', self.data, 0x64)[0]
        for i in range(count):
            at = self.offsets[PARTS] + 48 * i
            if bytes(self.data[at + 16:at + 40]).split(b'\0')[0].decode('latin1') == name:
                return at
        raise KeyError(name)

    def copy_part_bounds(self, other, name):
        self.data[self.part_named(name):self.part_named(name) + 16] = other.data[other.part_named(name):other.part_named(name) + 16]


def block_runs(before, after):
    if len(before) != len(after):
        raise ValueError('sizes differ')
    return [(i, bytes(after[i:i + 16])) for i in range(0, len(before) & ~15, 16) if before[i:i + 16] != after[i:i + 16]]


# ------------------------------------------------------------------ art

def texture(blob, name):
    return next(t for t in ftex.textures(blob) if t['name'] == name)


def pixels(blob, name):
    return np.array(ftex.decode(blob, texture(blob, name)))


def resized(tile, width, height):
    """Premultiplied, so the transparent surround does not bleed into the letters' edges."""
    alpha = tile[..., 3:4].astype(np.float32) / 255.0
    premult = np.concatenate([tile[..., :3].astype(np.float32) * alpha, alpha * 255.0], axis=2)
    out = np.stack([np.asarray(Image.fromarray(premult[..., c]).resize((width, height), Image.LANCZOS), np.float32)
                    for c in range(4)], axis=2)
    a = np.clip(out[..., 3:4], 0, 255) / 255.0
    rgb = np.where(a > 0.003, out[..., :3] / np.maximum(a, 0.003), 0)
    return np.concatenate([np.clip(rgb, 0, 255), a * 255.0], axis=2).astype(np.uint8)


def ink_box(tile):
    ys, xs = np.nonzero(tile[..., 3] > INK)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def gap_cut(tile, low, high):
    """The last column in [low, high] with the least ink, so a word splits between its letters."""
    ink = (tile[..., 3] > INK).sum(axis=0)
    low, high = max(low, 0), min(high, tile.shape[1] - 1)
    return high - int(np.argmin(ink[low:high + 1][::-1]))


@dataclass
class Atlas:
    """A base texture with the rectangles the labels rewrite."""
    name: str
    image: np.ndarray
    touched: np.ndarray = None
    label_rects: list = field(default_factory=list)

    def __post_init__(self):
        self.touched = np.zeros(self.image.shape[:2], bool)

    def replace(self, rect, tile):
        x0, y0, x1, y1 = rect
        if any(v % 4 for v in rect):
            raise ValueError(f'{rect}: not on the block grid')
        self.image[y0:y1, x0:x1] = 0
        self.image[y0:y0 + tile.shape[0], x0:x0 + tile.shape[1]] = tile
        self.touched[y0:y1, x0:x1] = True
        self.label_rects.append(rect)

    def paste(self, rect, tile, mask):
        x0, y0, x1, y1 = rect
        self.image[y0:y1, x0:x1][mask] = tile[mask]
        self.touched[y0:y1, x0:x1] |= mask
        self.label_rects.append(rect)


def encoded_runs(blob, atlas):
    tex = texture(blob, atlas.name)
    bw, bh, pitch = tex['width'] // 4, tex['height'] // 4, tex['pitch'] // 4
    blocks = atlas.touched.reshape(bh, 4, bw, 4).any(axis=(1, 3))
    ys, xs = np.nonzero(blocks)
    tiles = np.stack([atlas.image[y * 4:y * 4 + 4, x * 4:x * 4 + 4] for y, x in zip(ys, xs)]).reshape(-1, 16, 4)
    encoded = bc7.encode_blocks(tiles)
    return [(tex['data'] + ftex.tiled_offset(int(x), int(y), pitch), bytes(encoded[k * 16:(k + 1) * 16]))
            for k, (y, x) in enumerate(zip(ys, xs))]


# ------------------------------------------------------------------ the games

def read_entry(folder, entry):
    archive = Cpk(PREPARED / folder / 'DRACRO.CPK')
    return archive.read(archive.find(entry))


def usa_sources():
    """The original USA packs and the released PT-BR packs built from them."""
    cpk = Cpk(va.ARCHIVE)
    rows = va.original_entries(cpk)
    interface = va.original_data(cpk, rows, 'stage/Interface_US.ftx')
    title = cpk.read(cpk.find('stage/title_US.ftx'))
    patch = lambda name: va.parse_dcpx(va.unpack_patch((VISUAL / name).read_bytes()))[0]  # noqa: E731
    return {
        'interface': interface, 'interface_pt': va.apply_patch(interface, patch('interface-us.dcpx.z')),
        'interface_runs': patch('interface-us.dcpx.z')['runs'],
        'title': title, 'title_pt': va.apply_patch(title, patch('title-us.dcpx.z')),
        'layout_fr': cpk.read(cpk.find('stage/Interface_FR.mbs')), 'title_layout_fr': cpk.read(cpk.find('stage/title_FR.mbs')),
    }


def shared_atlas_runs(usa, base):
    """The released label runs that lie in atlases every pack shares: same texels, another offset in the pack."""
    us, jp = {t['name']: t for t in ftex.textures(usa['interface'])}, {t['name']: t for t in ftex.textures(base)}
    runs, texels = [], {}
    for offset, data in usa['interface_runs']:
        tex = next(t for t in us.values() if t['data'] <= offset < t['data'] + t['size'])
        if tex['name'] not in jp:
            continue
        if usa['interface'][tex['data']:tex['data'] + tex['size']] != base[jp[tex['name']]['data']:jp[tex['name']]['data'] + tex['size']]:
            raise SystemExit(f"{tex['name']}: not the same atlas in the USA and base packs")
        runs.append((offset - tex['data'] + jp[tex['name']]['data'], data))
        texels[tex['name']] = texels.get(tex['name'], 0) + 1
    return runs, texels


NOW_LOADING = {'art': (0, 485, 1426, 644), 'first': (0, 0, 472, 156), 'second': (0, 1540, 952, 1704), 'groups': (251, 252), 'right': 196.0}
NOW_SAVING = {'art': (0, 323, 1470, 485), 'first': (0, 156, 472, 312), 'second': (0, 1704, 812, 1872), 'groups': (256, 257), 'right': 177.0}
YES = {'art': (0, 1106, 380, 1269), 'rect': (0, 800, 404, 952)}
NO = {'art': (1460, 158, 1900, 314), 'rect': (1000, 828, 1296, 992), 'groups': (873, 874), 'elements': (200,)}
NO_QUADS = ((-47.0, -17.5, 51.0, 21.5), (-49.0, -19.5, 49.0, 19.5))
TREASURE_GROUP, TREASURE_BOX, TREASURE_SHIFT = 864, (-280.0, -104.0, 0.0, -62.0), 26.0
TREASURE_POSITION_ENTRIES = 6
ORIGINAL_BOXES = {251: (-191.0, 268.0, -73.0, 307.0), 252: (-42.0, 265.0, 196.0, 306.0), 256: (-175.0, 268.0, -57.0, 307.0),
                  257: (-26.0, 265.0, 177.0, 307.0), 873: (-35.0, -19.0, 39.0, 22.0), 874: (-37.0, -21.0, 37.0, 20.0),
                  TREASURE_GROUP: TREASURE_BOX}
TITLE_ORIGINAL_BOXES = {1042: (-201.0, -15.0, -128.0, 9.0), 1043: (-182.0, -14.0, -61.0, 14.0), 1044: (-101.0, -15.0, -28.0, 9.0),
                        1045: (-79.0, -14.0, 72.0, 8.0), 1046: (32.0, -14.0, 134.0, 15.0), 1047: (106.0, -14.0, 259.0, 8.0),
                        1106: (-22.0, -8.6, 23.0, 8.4)}


def two_sprite_word(word, art, first_atlas, second_atlas, layout):
    """A word the released layout draws as one sprite, in the two sprites of the base layout, cut between letters."""
    x0, y0, x1, y1 = word['art']
    tile = art[y0:y1, x0:x1]
    ix0, iy0, ix1, iy1 = ink_box(tile)
    tile = tile[iy0:iy1, ix0:ix1]
    first_w, first_h = word['first'][2] - word['first'][0], word['first'][3] - word['first'][1]
    second_w, second_h = word['second'][2] - word['second'][0], word['second'][3] - word['second'][1]
    scale = min(1.0, (first_w + second_w - PAD * 2) / tile.shape[1], min(first_h, second_h) / tile.shape[0])
    tile = resized(tile, round(tile.shape[1] * scale), round(tile.shape[0] * scale))
    width = tile.shape[1]
    cut = gap_cut(tile, width - second_w + PAD, first_w - PAD)
    first_atlas.replace(word['first'], tile[:, :min(cut + SEAM_OVERLAP, first_w)])
    second_atlas.replace(word['second'], tile[:, cut:])
    unit = UNIT / scale
    left = word['right'] - width * UNIT
    top = 266.7
    a, b = word['groups']
    layout.set_box(a, left, top, left + first_w * UNIT, top + first_h * UNIT)
    layout.set_box(b, left + cut * UNIT, top, left + cut * UNIT + second_w * UNIT, top + second_h * UNIT)
    return {'scale': scale, 'cut': int(cut), 'width': int(width), 'unit': unit}


def require_boxes(layout, boxes):
    for group, box in boxes.items():
        found = tuple(round(v, 1) for v in layout.box(group))
        if found != box:
            raise SystemExit(f'position group {group} is {found}, not {box}')


def interface_patch(usa, base, base_xbox, base_layout):
    art = pixels(usa['interface_pt'], 'Interface_Add_arranged000.gnf')
    atlas8 = Atlas('Interface_arranged0812.gnf', pixels(base, 'Interface_arranged0812.gnf'))
    atlas9 = Atlas('Interface_arranged0912.gnf', pixels(base, 'Interface_arranged0912.gnf'))
    layout = Layout(base_layout)
    require_boxes(layout, ORIGINAL_BOXES)
    french = Layout(usa['layout_fr'])
    report = {'loading': two_sprite_word(NOW_LOADING, art, atlas9, atlas8, layout),
              'saving': two_sprite_word(NOW_SAVING, art, atlas9, atlas8, layout)}

    x0, y0, x1, y1 = YES['art']
    rect = YES['rect']
    atlas9.replace(rect, resized(art[y0:y1, x0:x1], rect[2] - rect[0], rect[3] - rect[1]))
    x0, y0, x1, y1 = NO['art']
    rect = NO['rect']
    atlas9.replace(rect, resized(art[y0:y1, x0:x1], rect[2] - rect[0], rect[3] - rect[1]))
    for group, quad in zip(NO['groups'], NO_QUADS):
        layout.set_box(group, *quad)
    for element in NO['elements']:
        layout.set_element_bounds(element, -49.0, -19.5, 51.0, 21.5)
    for part in ('SELECT', 'SELECT_NO', 'SELECT_NO_1', 'SELECT_YES', 'SELECT_YES_1'):
        layout.copy_part_bounds(french, part)

    start = layout.offsets[POSITIONS] + 48 * TREASURE_GROUP
    for entry in range(TREASURE_POSITION_ENTRIES):
        at = start + 8 * entry
        struct.pack_into('<f', layout.data, at, struct.unpack_from('<f', layout.data, at)[0] + TREASURE_SHIFT)

    shared, texels = shared_atlas_runs(usa, base)
    report['shared_runs'] = texels
    written = {offset: data for offset, data in shared}
    for atlas in (atlas8, atlas9):
        written.update(dict(encoded_runs(base, atlas)))
    runs = sorted(written.items())
    pack_target = {'name': 'stage/Interface.ftx', 'size': len(base), 'runs': runs,
                   'states': [(crc32(source), crc32(apply_runs(source, runs))) for source in (base, base_xbox)]}
    layout_runs = block_runs(base_layout, layout.data)
    layout_target = {'name': 'stage/Interface.mbs', 'size': len(base_layout), 'runs': layout_runs,
                     'states': [(crc32(base_layout), crc32(apply_runs(base_layout, layout_runs)))]}
    report['pack_runs'], report['layout_runs'] = len(runs), len(layout_runs)
    return [pack_target, layout_target], report, (atlas8, atlas9)


BOOKS = (((2212, 0, 2765, 554), (1120, 832, 1680, 1392)), ((0, 0, 553, 555), (0, 832, 560, 1392)),
         ((553, 0, 1106, 555), (560, 832, 1120, 1392)), ((1106, 0, 1659, 555), (0, 1392, 560, 1952)))
NEW = {'art': (1659, 1665, 2020, 1723), 'rect': (660, 1952, 844, 2020), 'group': 1106, 'element': 136,
       'quad': (-45.2, -7.2, 45.0, 7.2)}
COPYRIGHT_ART = (0, 1665, 1597, 1764)
COPYRIGHT_ELEMENTS = (120, 121)
COPYRIGHT_PIECES = 3
COPYRIGHT_TEXTURE, COPYRIGHT_ATLAS = 2, 2048
COPYRIGHT_BOX = (-199.5, -14.0, 199.8, 10.8)
COPYRIGHT_WORDS = ('\u00a9ATLUS', '\u00a9SEGA', 'Todos', 'os', 'direitos', 'reservados.', 'Tradu\u00e7\u00e3o', 'Uma', 'Ficha')
COPYRIGHT_PARTS = ('TITLE_0', 'TITLE_0_1', 'TITLE_0_PS4')
NEW_PARTS = ('GALLERY_MARK_1', 'GALLERY_MARK_1_1', 'GALLERY_MARK_1_PS4')


def book_labels(usa_before, usa_after, atlas):
    """Only what the released art changed on each book: the word on its cover, at the base sprite's scale."""
    for art_rect, rect in BOOKS:
        ax0, ay0, ax1, ay1 = art_rect
        width, height = rect[2] - rect[0], rect[3] - rect[1]
        before, after = usa_before[ay0:ay1, ax0:ax1].astype(np.int16), usa_after[ay0:ay1, ax0:ax1].astype(np.int16)
        strong = np.abs(before - after).max(axis=2) > LABEL_DIFFERENCE
        labels, _ = ndimage.label(ndimage.binary_dilation(strong, iterations=8))
        word = labels == np.bincount(labels[labels > 0]).argmax()
        ys, xs = np.nonzero(word)
        band = np.zeros((height, width), bool)
        sx, sy = width / strong.shape[1], height / strong.shape[0]
        band[int(ys.min() * sy):int(ys.max() * sy) + 2, int(xs.min() * sx):int(xs.max() * sx) + 2] = True
        atlas.paste(rect, resized(usa_after[ay0:ay1, ax0:ax1], width, height), band)


def copyright_sprites(layout):
    """The rectangles the copyright words are drawn from, with their quads; a rectangle may be drawn twice (the two \u00a9)."""
    quads = [layout.element_quads(element) for element in COPYRIGHT_ELEMENTS]
    if quads[0] != quads[1] or any(texture_index != COPYRIGHT_TEXTURE for texture_index, _, _ in quads[0]):
        raise SystemExit('the title copyright elements are not the known layout')
    sprites = {}
    for _, uv, position in quads[0]:
        sprites.setdefault(layout.uv_box(uv, COPYRIGHT_ATLAS, COPYRIGHT_ATLAS), []).append(position)
    return sprites


def copyright_line(art, atlases, layout):
    """The released line in the widest sprites drawn once, in its order; the other copyright quads collapse."""
    x0, y0, x1, y1 = COPYRIGHT_ART
    tile = art[y0:y1, x0:x1]
    top, bottom = COPYRIGHT_BOX[1], COPYRIGHT_BOX[3]
    scale_x = (COPYRIGHT_BOX[2] - COPYRIGHT_BOX[0]) / tile.shape[1]
    sprites = copyright_sprites(layout)
    pieces = sorted((rect for rect, groups in sprites.items() if len(groups) == 1), key=lambda r: r[0] - r[2])[:COPYRIGHT_PIECES]
    start, cuts = 0, []
    for rect in pieces[:-1]:
        cut = gap_cut(tile, start + (rect[2] - rect[0]) - 120, start + (rect[2] - rect[0]) - 1)
        cuts.append((start, cut))
        start = cut
    cuts.append((start, tile.shape[1]))
    for (a, b), rect in zip(cuts, pieces):
        width, height = rect[2] - rect[0], rect[3] - rect[1]
        if b - a > width:
            raise SystemExit(f'copyright piece {a}..{b} does not fit {rect}')
        atlases.replace(rect, resized(tile[:, a:b], width, height))
        layout.set_box(sprites[rect][0], COPYRIGHT_BOX[0] + a * scale_x, top, COPYRIGHT_BOX[0] + b * scale_x, bottom)
    for rect, groups in sprites.items():
        if rect not in pieces:
            for group in groups:
                layout.set_box(group, 0.0, 0.0, 0.0, 0.0)
    return cuts


def title_patch(usa, base, base_layout):
    art_before = pixels(usa['title'], 'Title_Add_arranged000.gnf')
    art = pixels(usa['title_pt'], 'Title_Add_arranged000.gnf')
    atlas2 = Atlas('title_arranged022.gnf', pixels(base, 'title_arranged022.gnf'))
    atlas3 = Atlas('title_arranged033.gnf', pixels(base, 'title_arranged033.gnf'))
    layout = Layout(base_layout)
    require_boxes(layout, TITLE_ORIGINAL_BOXES)
    french = Layout(usa['title_layout_fr'])
    book_labels(art_before, art, atlas3)
    x0, y0, x1, y1 = NEW['art']
    rect = NEW['rect']
    atlas3.replace(rect, resized(art[y0:y1, x0:x1], rect[2] - rect[0], rect[3] - rect[1]))
    layout.set_box(NEW['group'], *NEW['quad'])
    layout.set_element_bounds(NEW['element'], *NEW['quad'])
    cuts = copyright_line(art, atlas2, layout)
    for part in COPYRIGHT_PARTS + NEW_PARTS:
        layout.copy_part_bounds(french, part)
    written = {}
    for atlas in (atlas2, atlas3):
        written.update(dict(encoded_runs(base, atlas)))
    runs = sorted(written.items())
    layout_runs = block_runs(base_layout, layout.data)
    targets = [{'name': 'stage/title.ftx', 'size': len(base), 'runs': runs, 'states': [(crc32(base), crc32(apply_runs(base, runs)))]},
               {'name': 'stage/title.mbs', 'size': len(base_layout), 'runs': layout_runs,
                'states': [(crc32(base_layout), crc32(apply_runs(base_layout, layout_runs)))]}]
    return targets, {'pack_runs': len(runs), 'layout_runs': len(layout_runs), 'copyright_cuts': cuts}, (atlas2, atlas3)


def apply_runs(source, runs):
    out = bytearray(source)
    for offset, data in runs:
        out[offset:offset + 16] = data
    return bytes(out)


def sources():
    return {'base': read_entry('CUSA07001-1.02', 'stage/Interface.ftx'), 'base_xbox': read_entry('CUSA07001-1.00', 'stage/Interface.ftx'),
            'layout': read_entry('CUSA07001-1.02', 'stage/Interface.mbs'), 'title': read_entry('CUSA07001-1.02', 'stage/title.ftx'),
            'title_layout': read_entry('CUSA07001-1.02', 'stage/title.mbs')}


def write_patch(name, targets):
    path = VISUAL / name
    path.write_bytes(va.pack_patch(dcpx2(targets)))
    return path.stat().st_size


def cmd_build(_args):
    usa, base = usa_sources(), sources()
    targets, report, _ = interface_patch(usa, base['base'], base['base_xbox'], base['layout'])
    size = write_patch('base-interface.dcpx.z', targets)
    print('base-interface', size, json.dumps(report, default=int))
    targets, report, _ = title_patch(usa, base['title'], base['title_layout'])
    size = write_patch('base-title.dcpx.z', targets)
    print('base-title', size, json.dumps(report, default=int))
    return 0


def load_targets(name):
    return parse_dcpx2(va.unpack_patch((VISUAL / name).read_bytes()))


def apply_state(source, target):
    state = next((s for s in target['states'] if s[0] == crc32(source)), None)
    if state is None or len(source) != target['size']:
        return None
    out = apply_runs(source, target['runs'])
    return out if crc32(out) == state[1] else None


GAMES = ('CUSA07001-1.00', 'CUSA07001-1.02', 'CUSA07001-1.03', 'CUSA10209-1.00')


def same_art(result, art):
    """How far a decoded label is from the released art, over the art's ink."""
    solid = art[..., 3] > 128
    return float(np.abs(result[..., :4].astype(np.float32) - art.astype(np.float32))[solid].mean())


def word_from_sprites(word, atlas8, atlas9, art):
    """The word the two sprites of a game's patched pack draw, against the released sprite."""
    x0, y0, x1, y1 = word['art']
    tile = art[y0:y1, x0:x1]
    ix0, iy0, ix1, iy1 = ink_box(tile)
    tile = tile[iy0:iy1, ix0:ix1]
    cut = gap_cut(tile, tile.shape[1] - (word['second'][2] - word['second'][0]) + PAD, word['first'][2] - word['first'][0] - PAD)
    first, second = word['first'], word['second']
    left = atlas9[first[1]:first[1] + tile.shape[0], first[0]:first[0] + cut]
    right = atlas8[second[1]:second[1] + tile.shape[0], second[0]:second[0] + tile.shape[1] - cut]
    return np.concatenate([left, right], axis=1), tile


def layout_checks(folder, patches, errors):
    source = read_entry(folder, 'stage/Interface.mbs')
    patched = apply_state(source, patches['stage/Interface.mbs'])
    if patched is None:
        return
    before, after = Layout(source), Layout(patched)
    moved = {g for g in range((before.offsets[3] - before.offsets[2]) // 48) if before.box(g) != after.box(g)}
    expected = {g for g in ORIGINAL_BOXES}
    if moved != expected:
        errors.append(f'{folder} stage/Interface.mbs: moved quads {sorted(moved)}, expected {sorted(expected)}')
    box = tuple(round(v, 1) for v in after.box(TREASURE_GROUP))
    print(f'  {folder} RANKS DE TESOURO first word quad: {box}')
    if box != (-254.0, -104.0, 26.0, -62.0):
        errors.append(f'{folder} the treasure label did not move next to its second word')


CANVAS_PER_UNIT = 4
CANVAS_BOX = (-260, -24, 280, 24)
WORD_GAP = 12  # canvas columns: letters of a word are closer than this
MIN_WORD_COVER = 0.80


def on_canvas(canvas, tile, box):
    x0, y0, x1, y1 = [round(v * CANVAS_PER_UNIT) for v in box]
    left, top = -CANVAS_BOX[0] * CANVAS_PER_UNIT + x0, -CANVAS_BOX[1] * CANVAS_PER_UNIT + y0
    if x1 <= x0 or y1 <= y0:
        return
    scaled = resized(tile, x1 - x0, y1 - y0).astype(np.float32)
    alpha = scaled[..., 3:4] / 255.0
    area = canvas[top:top + (y1 - y0), left:left + (x1 - x0)]
    area[..., :3] = scaled[..., :3] * alpha + area[..., :3] * (1 - alpha)
    area[..., 3:4] = 255 * (alpha + area[..., 3:4] / 255 * (1 - alpha))


def blank_canvas():
    return np.zeros(((CANVAS_BOX[3] - CANVAS_BOX[1]) * CANVAS_PER_UNIT, (CANVAS_BOX[2] - CANVAS_BOX[0]) * CANVAS_PER_UNIT, 4), np.float32)


def released_line(usa):
    """The USA line where the USA layout draws it: the reference every region must reproduce."""
    x0, y0, x1, y1 = COPYRIGHT_ART
    canvas = blank_canvas()
    on_canvas(canvas, pixels(usa['title_pt'], 'Title_Add_arranged000.gnf')[y0:y1, x0:x1], COPYRIGHT_BOX)
    return canvas


def regional_line(pack, layout):
    """What the regional layout draws from the regional pack, quad by quad."""
    atlas = pixels(pack, 'title_arranged022.gnf')
    canvas = blank_canvas()
    boxes = []
    for _, uv, position in layout.element_quads(COPYRIGHT_ELEMENTS[0]):
        x0, y0, x1, y1 = layout.uv_box(uv, COPYRIGHT_ATLAS, COPYRIGHT_ATLAS)
        box = layout.box(position)
        if box[2] > box[0]:
            boxes.append(box)
            on_canvas(canvas, atlas[y0:y1, x0:x1], box)
    return canvas, boxes


def word_columns(canvas):
    ink = np.nonzero((canvas[..., 3] > INK).any(axis=0))[0]
    return [(int(run[0]), int(run[-1]) + 1) for run in np.split(ink, np.nonzero(np.diff(ink) > WORD_GAP)[0] + 1)]


def copyright_checks(folder, usa, patches, errors):
    pack = apply_state(read_entry(folder, 'stage/title.ftx'), patches['stage/title.ftx'])
    source_layout = read_entry(folder, 'stage/title.mbs')
    layout_bytes = apply_state(source_layout, patches['stage/title.mbs'])
    if pack is None or layout_bytes is None:
        return
    reference = released_line(usa)
    result, boxes = regional_line(pack, Layout(layout_bytes))
    words = word_columns(reference)
    if len(words) != len(COPYRIGHT_WORDS):
        errors.append(f'the released line has {len(words)} words, expected {len(COPYRIGHT_WORDS)}')
        return
    solid = reference[..., 3] > INK
    drawn = result[..., 3] > INK
    covers = []
    for word, (a, b) in zip(COPYRIGHT_WORDS, words):
        cover = float((solid[:, a:b] & drawn[:, a:b]).sum() / max(1, solid[:, a:b].sum()))
        covers.append(f'{word} {cover:.2f}')
        if cover < MIN_WORD_COVER:
            errors.append(f'{folder} copyright: {word} is not visible where the USA line has it ({cover:.2f})')
    extra = float((drawn & ~ndimage.binary_dilation(solid, iterations=3)).sum() / max(1, solid.sum()))
    print(f'  {folder} copyright: {", ".join(covers)}; ink outside the line {extra:.3f}')
    if extra > 0.02:
        errors.append(f'{folder} copyright: ink where the USA line has none ({extra:.3f})')
    ordered = sorted(boxes)
    if len(ordered) != COPYRIGHT_PIECES or any(a[2] > b[0] + 0.01 for a, b in zip(ordered, ordered[1:])):
        errors.append(f'{folder} copyright: the drawn quads overlap or are not {COPYRIGHT_PIECES}: {ordered}')
    if any(box[1] < COPYRIGHT_BOX[1] - 0.01 or box[3] > COPYRIGHT_BOX[3] + 0.01 for box in ordered):
        errors.append(f'{folder} copyright: a quad left the line: {ordered}')
    before, after = Layout(source_layout), Layout(layout_bytes)
    moved = {g for g in range((before.offsets[3] - before.offsets[2]) // 48) if before.box(g) != after.box(g)}
    if moved != set(TITLE_ORIGINAL_BOXES):
        errors.append(f'{folder} stage/title.mbs: moved quads {sorted(moved)}, expected {sorted(TITLE_ORIGINAL_BOXES)}')


def label_checks(folder, usa, patches, errors):
    source = read_entry(folder, 'stage/Interface.ftx')
    patched = apply_state(source, patches['stage/Interface.ftx'])
    if patched is None:
        return
    art = pixels(usa['interface_pt'], 'Interface_Add_arranged000.gnf')
    atlas8, atlas9 = pixels(patched, 'Interface_arranged0812.gnf'), pixels(patched, 'Interface_arranged0912.gnf')
    native8, native9 = pixels(source, 'Interface_arranged0812.gnf'), pixels(source, 'Interface_arranged0912.gnf')
    for label, word in (('NOW LOADING -> CARREGANDO', NOW_LOADING), ('NOW SAVING -> SALVANDO', NOW_SAVING)):
        result, released = word_from_sprites(word, atlas8, atlas9, art)
        native, _ = word_from_sprites(word, native8, native9, art)
        distance, change = same_art(result, released), same_art(native, released)
        print(f'  {folder} {label}: {distance:.2f} from the released art, the regional word was {change:.2f}')
        if distance > MAX_ART_DIFFERENCE or change < MIN_SPRITE_CHANGE:
            errors.append(f'{folder} {label}: the patched sprites are not the released art')
    for label, rect, word in (('YES -> SIM', YES['rect'], YES), ('NO -> NAO', NO['rect'], NO)):
        x0, y0, x1, y1 = word['art']
        released = resized(art[y0:y1, x0:x1], rect[2] - rect[0], rect[3] - rect[1])
        distance = same_art(atlas9[rect[1]:rect[3], rect[0]:rect[2]], released)
        print(f'  {folder} {label}: {distance:.2f} from the released art')
        if distance > MAX_ART_DIFFERENCE:
            errors.append(f'{folder} {label}: the patched sprite is not the released art')


def cmd_check(_args):
    errors = []
    patches = {t['name']: t for name in ('base-interface.dcpx.z', 'base-title.dcpx.z') for t in load_targets(name)}
    for folder in GAMES:
        if not (PREPARED / folder / 'DRACRO.CPK').is_file():
            print(f'{folder}: DATA UNAVAILABLE')
            continue
        applied = 0
        for entry, target in patches.items():
            if apply_state(read_entry(folder, entry), target) is None:
                errors.append(f'{folder} {entry}: the source is not one the patch knows')
            else:
                applied += 1
        print(f'{folder}: {applied}/{len(patches)} regional visuals apply')
    usa, base = usa_sources(), sources()
    for folder in GAMES:
        if (PREPARED / folder / 'DRACRO.CPK').is_file():
            label_checks(folder, usa, patches, errors)
            layout_checks(folder, patches, errors)
            copyright_checks(folder, usa, patches, errors)
    fresh_interface, _, _ = interface_patch(usa, base['base'], base['base_xbox'], base['layout'])
    fresh_title, _, _ = title_patch(usa, base['title'], base['title_layout'])
    for fresh in fresh_interface + fresh_title:
        if fresh['states'] != patches[fresh['name']]['states'] or [r[0] for r in fresh['runs']] != [r[0] for r in patches[fresh['name']]['runs']]:
            errors.append(f"{fresh['name']}: the tracked patch is not what the released art gives")
    for name in ('stage/Interface.ftx', 'stage/title.ftx'):
        share = len(patches[name]['runs']) * 16 / patches[name]['size']
        print(f'{name}: {len(patches[name]["runs"])} blocks, {share:.2%} of the pack')
        if share > MAX_PACK_BLOCK_SHARE:
            errors.append(f'{name}: the patch holds {share:.2%} of the pack, more than labels')
    for line in errors:
        print('error:', line)
    print(f'{len(errors)} errors')
    return 1 if errors else 0


def on_panel(rgba, colour=(36, 36, 80)):
    alpha = rgba[..., 3:4] / 255.0
    return (rgba[..., :3] * alpha + np.array(colour) * (1 - alpha)).astype(np.uint8)


def cmd_preview(_args):
    WORK.mkdir(parents=True, exist_ok=True)
    usa, base = usa_sources(), sources()
    _, _, (atlas8, atlas9) = interface_patch(usa, base['base'], base['base_xbox'], base['layout'])
    targets, _, (atlas2, atlas3) = title_patch(usa, base['title'], base['title_layout'])
    line, _ = regional_line(apply_runs(base['title'], targets[0]['runs']), Layout(apply_runs(base['title_layout'], targets[1]['runs'])))
    Image.fromarray(on_panel(np.vstack([released_line(usa), line]).astype(np.uint8))).save(WORK / 'copyright.png')
    Image.fromarray(on_panel(atlas8.image)).save(WORK / 'interface0812.png')
    Image.fromarray(on_panel(atlas9.image)).save(WORK / 'interface0912.png')
    Image.fromarray(on_panel(atlas2.image)).save(WORK / 'title022.png')
    Image.fromarray(on_panel(atlas3.image)).save(WORK / 'title033.png')
    print(WORK)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('build', 'check', 'preview'):
        sub.add_parser(name)
    args = parser.parse_args()
    return globals()['cmd_' + args.command](args)


if __name__ == '__main__':
    sys.exit(main())
