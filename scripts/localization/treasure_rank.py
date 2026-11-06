#!/usr/bin/env python3
"""Portuguese "RANKS DE TESOURO" label for stage/Interface_US.ftx (texture Interface_arranged0812).

    treasure_rank.py preview    writes comparison images to the work folder
    treasure_rank.py check      validates the rectangles, the layouts and that only the label blocks change

The label is three sprites of one 2048 x 2048 atlas: the gold TREASURE word, the gold RANK word and a white glow
strip holding both. The Portuguese words are assembled from the real ornate letters of those two sprites (R A N K S T
E U) plus the atlas's own ornate D (purple) and O (the OK plate), recoloured to the same gold. The sprite rectangles
and layouts are unchanged. The rank letters are other sprites
(Interface_arranged0912) and are not touched.
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts' / 'prompts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ftex  # noqa: E402
import mbs_rects  # noqa: E402
import patch_ftx  # noqa: E402

TEXTURE = 'Interface_arranged0812.gnf'

LEFT = (0, 1372, 1120, 1540)
RIGHT = (424, 1208, 1028, 1372)
GLOW = (0, 1928, 944, 2032)
RECTS = (LEFT, RIGHT, GLOW)
WORDS = ('RANKS DE', 'TESOURO')

TREASURE_CUTS = (5, 139, 273, 389, 568, 695, 846, 979, 1118)
RANK_CUTS = (5, 143, 299, 450, 601)
SOURCES = {
    'T': ('TREASURE', 0), 'R': ('RANK', 0), 'E': ('TREASURE', 2), 'A': ('RANK', 1),
    'N': ('RANK', 2), 'K': ('RANK', 3), 'S': ('TREASURE', 4), 'U': ('TREASURE', 5),
}
ALPHA_FLOOR = 24
SPACE_WIDTH = 64
HEIGHT_SCALE = 0.76
RAMP_STEPS = 48
BORROWED = {'D': (1301, 834, 1488, 1019), 'O': (1183, 520, 1322, 669)}
LETTER_ATLAS = 'Interface_arranged0912.gnf'
GLOW_PAD = 10
GLOW_SPREAD = 9
SIDE_MARGIN = 6

UNIT = 0.25
FIRST_QUAD = (-280.0, -104.0, 0.0, -62.0)
SECOND_QUAD = (33.0, -104.0, 184.0, -63.0)
GLOW_QUAD = (-286.0, -110.0, 186.0, -56.0)
SPRITE_ENTRIES = 6
POSITION_OFFSET = 3552
GLOW_SCALE = (UNIT / ((GLOW_QUAD[2] - GLOW_QUAD[0]) / (GLOW[2] - GLOW[0])),
              UNIT / ((GLOW_QUAD[3] - GLOW_QUAD[1]) / (GLOW[3] - GLOW[1])))


def trim(tile):
    ys, xs = np.nonzero(tile[..., 3] > ALPHA_FLOOR)
    return tile[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def real_letters(atlas):
    """{letter: RGBA tile} cut out of the English TREASURE and RANK sprites."""
    parts = {'TREASURE': (LEFT, TREASURE_CUTS), 'RANK': (RIGHT, RANK_CUTS)}
    tiles = {}
    for letter, (word, index) in SOURCES.items():
        rect, cuts = parts[word]
        tiles[letter] = trim(atlas[rect[1]:rect[3], rect[0] + cuts[index]:rect[0] + cuts[index + 1]].copy())
    return tiles


def luminance(rgb):
    return rgb[..., 0] * 0.3 + rgb[..., 1] * 0.59 + rgb[..., 2] * 0.11


def gold_ramp(tiles):
    """(luminance percentiles, colours): the real gold letters' colour at each brightness."""
    pixels = np.concatenate([t[..., :3][t[..., 3] > 230].astype(np.float32) for t in tiles.values()])
    lum = luminance(pixels)
    edges = np.percentile(lum, np.linspace(0, 100, RAMP_STEPS + 1))
    index = np.clip(np.searchsorted(edges, lum, side='right') - 1, 0, RAMP_STEPS - 1)
    colours = np.stack([pixels[index == k].mean(axis=0) if (index == k).any() else pixels.mean(axis=0)
                        for k in range(RAMP_STEPS)])
    return edges, colours


def recoloured(tile, ramp):
    """`tile` with its brightness mapped onto the real gold ramp."""
    edges, colours = ramp
    rgb = tile[..., :3].astype(np.float32)
    lum = luminance(rgb)
    solid = tile[..., 3] > 230
    low, high = np.percentile(lum[solid], (1, 99))
    unit = np.clip((lum - low) / max(high - low, 1.0), 0, 1)
    target = edges[0] + unit * (edges[-1] - edges[0])
    index = np.clip(np.searchsorted(edges, target, side='right') - 1, 0, RAMP_STEPS - 1)
    out = tile.copy()
    out[..., :3] = colours[index].astype(np.uint8)
    return out


def component(atlas, box):
    """The ornate letter whose pixels fill `box` of an atlas, without the neighbours' pixels."""
    x0, y0, x1, y1 = box
    region = atlas[y0:y1, x0:x1].copy()
    labels, _ = ndimage.label(region[..., 3] > ALPHA_FLOOR)
    keep = ndimage.binary_dilation(labels == np.bincount(labels[labels > 0]).argmax(), iterations=3)
    region[~keep] = 0
    return trim(region)


def borrowed_letters(letters, rank_atlas):
    """D and O from the other ornate sprites of the atlas pack (a purple D, the O of the OK plate), in gold."""
    ramp = gold_ramp(letters)
    height = int(np.median([letters[c].shape[0] for c in 'RNKETU']))
    out = {}
    for letter, box in BORROWED.items():
        tile = recoloured(component(rank_atlas, box), ramp)
        out[letter] = scaled(tile, height / tile.shape[0], height / tile.shape[0])
    return out


def scaled(tile, sx, sy):
    h, w = tile.shape[:2]
    size = (max(1, round(w * sx)), max(1, round(h * sy)))
    alpha = tile[..., 3:4].astype(np.float32) / 255.0
    premult = np.concatenate([tile[..., :3].astype(np.float32) * alpha, alpha * 255.0], axis=2)
    out = np.stack([np.asarray(Image.fromarray(premult[..., c]).resize(size, Image.LANCZOS), np.float32)
                    for c in range(4)], axis=2)
    a = np.clip(out[..., 3:4], 0, 255) / 255.0
    rgb = np.where(a > 0.003, out[..., :3] / np.maximum(a, 0.003), 0)
    return np.concatenate([np.clip(rgb, 0, 255), a * 255.0], axis=2).astype(np.uint8)


def word_tile(text, letters, sx, sy):
    pieces = [None if ch == ' ' else scaled(letters[ch], sx, sy) for ch in text]
    gap = round(SPACE_WIDTH * sx)
    width = sum(gap if p is None else p.shape[1] for p in pieces)
    height = max(p.shape[0] for p in pieces if p is not None)
    canvas = np.zeros((height, width, 4), np.float32)
    x = 0
    for piece in pieces:
        if piece is None:
            x += gap
            continue
        h, w = piece.shape[:2]
        y = (height - h) // 2
        src = piece.astype(np.float32)
        dst = canvas[y:y + h, x:x + w]
        sa = src[..., 3:4] / 255.0
        da = dst[..., 3:4] / 255.0
        oa = sa + da * (1 - sa)
        rgb = np.where(oa > 0, (src[..., :3] * sa + dst[..., :3] * da * (1 - sa)) / np.maximum(oa, 1e-6), 0)
        canvas[y:y + h, x:x + w] = np.concatenate([rgb, oa * 255.0], axis=2)
        x += w
    return canvas.astype(np.uint8)


def natural_width(text, letters):
    return sum(SPACE_WIDTH if ch == ' ' else letters[ch].shape[1] for ch in text)


def glow_tile(tile):
    alpha = tile[..., 3].astype(np.float32) / 255.0
    h, w = alpha.shape
    img = Image.fromarray((alpha * 255).astype(np.uint8)).resize(
        (max(1, round(w * GLOW_SCALE[0])), max(1, round(h * GLOW_SCALE[1]))), Image.LANCZOS)
    a = np.pad(np.asarray(img, np.float32) / 255.0, GLOW_PAD)
    a = np.clip(ndimage.gaussian_filter(ndimage.maximum_filter(a, size=GLOW_SPREAD), 3.0) * 2.4, 0, 1)
    out = np.full(a.shape + (4,), 255, np.uint8)
    out[..., 3] = (a * 255).astype(np.uint8)
    return out


def paste(canvas, tile, x, rect):
    x0, y0, x1, y1 = rect
    h, w = tile.shape[:2]
    y = y0 + ((y1 - y0) - h) // 2
    canvas[y:y + h, x0 + x:x0 + x + w] = tile


def plan(atlas, other):
    """The two gold word tiles, the layout shift of the first sprite (position units) and the letter scale."""
    letters = real_letters(atlas)
    letters.update(borrowed_letters(letters, other))
    avail = RIGHT[2] - RIGHT[0] - 2 * SIDE_MARGIN
    sy = HEIGHT_SCALE
    sx = min(1.0, avail / natural_width(WORDS[1], letters))
    first, second = word_tile(WORDS[0], letters, sx, sy), word_tile(WORDS[1], letters, sx, sy)
    space = SPACE_WIDTH * sx * UNIT
    shift = round(SECOND_QUAD[0] - FIRST_QUAD[2] + 2 * SIDE_MARGIN * UNIT - space)
    return first, second, shift


def layout_shift(blob):
    atlas = np.array(ftex.decode(blob, texture_of(blob)).convert('RGBA'))
    return plan(atlas, np.array(ftex.decode(blob, texture_of(blob, LETTER_ATLAS)).convert('RGBA')))[2]


def glow_x(unit_left, tile):
    x = round((unit_left - GLOW_QUAD[0]) / ((GLOW_QUAD[2] - GLOW_QUAD[0]) / (GLOW[2] - GLOW[0]))) - GLOW_PAD
    return int(min(max(x, 0), GLOW[2] - GLOW[0] - tile.shape[1]))


def bleed(atlas, rect):
    """Gives fully transparent pixels the colour of the nearest solid ones so filtering and BC7 never pull in black."""
    x0, y0, x1, y1 = rect
    region = atlas[y0:y1, x0:x1]
    solid = region[..., 3] > 128
    if not solid.any():
        return
    _, (iy, ix) = ndimage.distance_transform_edt(~solid, return_indices=True)
    clear = region[..., 3] == 0
    region[..., :3][clear] = region[..., :3][iy, ix][clear]


def painted(atlas, other):
    """A copy of the atlas with the three sprites replaced."""
    first, second, shift = plan(atlas, other)
    out = atlas.copy()
    for x0, y0, x1, y1 in RECTS:
        out[y0:y1, x0:x1] = 0
    first_x = LEFT[2] - LEFT[0] - first.shape[1] - SIDE_MARGIN
    paste(out, first, first_x, LEFT)
    paste(out, second, SIDE_MARGIN, RIGHT)
    for rect in (LEFT, RIGHT):
        bleed(out, rect)
    glow_first, glow_second = glow_tile(first), glow_tile(second)
    paste(out, glow_first, glow_x(FIRST_QUAD[0] + shift + first_x * UNIT, glow_first), GLOW)
    paste(out, glow_second, glow_x(SECOND_QUAD[0] + SIDE_MARGIN * UNIT, glow_second), GLOW)
    out[GLOW[1]:GLOW[3], GLOW[0]:GLOW[2], :3] = 255
    return out


def layout_edits(layout, shift):
    """[(file offset, old bytes, new bytes)]: the first gold sprite's six position entries moved right by `shift`."""
    first = mbs_rects.position_quads(layout)[FIRST_QUAD]
    start = mbs_rects.position_start(layout)
    edits = []
    for entry in range(first, first + SPRITE_ENTRIES):
        offset = start + 8 * entry
        old = layout[offset:offset + 4]
        edits.append((offset, old, struct.pack('<f', struct.unpack('<f', old)[0] + shift)))
    return edits


def texture_of(blob, name=TEXTURE):
    return next(t for t in ftex.textures(blob) if t['name'] == name)


def apply(blob):
    """The interface pack with the Portuguese label spliced into texture 0812; every other block is untouched."""
    tex = texture_of(blob)
    atlas = np.array(ftex.decode(blob, tex).convert('RGBA'))
    if atlas.shape[:2] != (2048, 2048):
        raise ValueError('unexpected atlas size')
    new = painted(atlas, np.array(ftex.decode(blob, texture_of(blob, LETTER_ATLAS)).convert('RGBA')))
    changed = np.zeros(atlas.shape[:2], bool)
    for x0, y0, x1, y1 in RECTS:
        changed[y0:y1, x0:x1] = True
    out, _ = patch_ftx.patch(blob, tex, new, changed)
    return out


def check_layouts(us_mbs, fr_mbs):
    errors = []
    for label, data in (('US', us_mbs), ('FR', fr_mbs)):
        found = mbs_rects.rectangles(data)
        placed = mbs_rects.position_quads(data)
        for rect, quad in zip(RECTS, (FIRST_QUAD, SECOND_QUAD, GLOW_QUAD)):
            sources = found.get(tuple(float(v) for v in rect))
            if not sources:
                errors.append(f'{label} layout has no sprite for {rect}')
            elif placed.get(quad) is None or placed[quad] - sources[0] != POSITION_OFFSET:
                errors.append(f'{label} layout does not place {rect} at {quad}')
    return errors


def cpp_table(edits):
    rows = []
    for offset, old, new in edits:
        before = ', '.join(f'0x{b:02x}' for b in old)
        after = ', '.join(f'0x{b:02x}' for b in new)
        rows.append(f'    {{0x{offset:x}u, {{{before}}}, {{{after}}}}},')
    return chr(10).join(rows)


def over(rgba, background=(46, 46, 56)):
    bg = Image.new('RGBA', (rgba.shape[1], rgba.shape[0]), background + (255,))
    bg.alpha_composite(Image.fromarray(rgba))
    return np.asarray(bg.convert('RGB'))


def stacked(atlas, margin=8):
    return np.concatenate([over(np.ascontiguousarray(atlas[r[1] - margin:r[3] + margin, :1200])) for r in RECTS], axis=0)


def screen_view(atlas, shift):
    """The three sprites placed as the layout places them (4 pixels per position unit), English or shifted."""
    left, top, scale = -300.0, -118.0, 4
    canvas = Image.new('RGBA', (int((200 - left) * scale), int((-48 - top) * scale)), (46, 46, 56, 255))

    def put(rect, quad, offset=0.0):
        sprite = Image.fromarray(np.ascontiguousarray(atlas[rect[1]:rect[3], rect[0]:rect[2]]))
        size = (round((quad[2] - quad[0]) * scale), round((quad[3] - quad[1]) * scale))
        canvas.alpha_composite(sprite.resize(size, Image.LANCZOS), (round((quad[0] + offset - left) * scale), round((quad[1] - top) * scale)))

    put(GLOW, GLOW_QUAD)
    put(LEFT, FIRST_QUAD, shift)
    put(RIGHT, SECOND_QUAD)
    return np.asarray(canvas.convert('RGB'))


def preview(work, blob):
    atlas = np.array(ftex.decode(blob, texture_of(blob)).convert('RGBA'))
    new = painted(atlas, np.array(ftex.decode(blob, texture_of(blob, LETTER_ATLAS)).convert('RGBA')))
    work.mkdir(parents=True, exist_ok=True)
    english, portuguese = stacked(atlas), stacked(new)
    shift = plan(atlas, np.array(ftex.decode(blob, texture_of(blob, LETTER_ATLAS)).convert('RGBA')))[2]
    screen = np.concatenate([screen_view(atlas, 0), screen_view(new, shift)], axis=0)
    Image.fromarray(screen).save(work / 'treasure_screen.png')
    Image.fromarray(english).save(work / 'treasure_english.png')
    Image.fromarray(portuguese).save(work / 'treasure_pt.png')
    Image.fromarray(over(np.ascontiguousarray(new[GLOW[1] - 8:GLOW[3] + 8, :1000]))).save(work / 'treasure_glow.png')
    panels = [english, portuguese]
    old = work / 'treasure_bad_ranque.png'
    if old.is_file():
        panels.insert(1, np.asarray(Image.open(old).convert('RGB')))
    width = max(p.shape[1] for p in panels)
    padded = [np.pad(p, ((0, 12), (0, width - p.shape[1]), (0, 0))) for p in panels]
    Image.fromarray(np.concatenate(padded, axis=0)).save(work / 'treasure_compare.png')
    print(work / 'treasure_compare.png')


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else 'check'
    work = REPO / 'cache' / 'localization' / 'pt-BR' / 'work' / 'treasure'
    import visual_assets as va  # noqa: E402
    blob = va.INTERFACE_FR.read_bytes()
    if command == 'preview':
        preview(work, blob)
        return 0
    errors = []
    if ' '.join(WORDS) != 'RANKS DE TESOURO':
        errors.append('the label wording is not RANKS DE TESOURO')
    cpk = va.Cpk(va.ARCHIVE)
    errors += check_layouts(va.read_archive(cpk, 'stage/Interface_US.mbs'), va.read_archive(cpk, 'stage/Interface_FR.mbs'))
    first_tile, second_tile, shift = plan(
        np.array(ftex.decode(blob, texture_of(blob)).convert('RGBA')),
        np.array(ftex.decode(blob, texture_of(blob, LETTER_ATLAS)).convert('RGBA')))
    space = SPACE_WIDTH * UNIT
    print(f'gap between the sprites: {SECOND_QUAD[0] - FIRST_QUAD[2]:.0f} units; first sprite moves {shift:+d} units, '
          f'ink gap {SECOND_QUAD[0] - FIRST_QUAD[2] - shift + 2 * SIDE_MARGIN * UNIT:.1f} units')
    fr_layout = va.read_archive(cpk, 'stage/Interface_FR.mbs')
    edits = layout_edits(fr_layout, shift)
    patched = bytearray(fr_layout)
    for offset, old, new in edits:
        if fr_layout[offset:offset + 4] != old:
            errors.append('layout edit source mismatch')
        patched[offset:offset + 4] = new
    moved = mbs_rects.position_quads(bytes(patched))
    if (FIRST_QUAD[0] + shift, FIRST_QUAD[1], FIRST_QUAD[2] + shift, FIRST_QUAD[3]) not in moved:
        errors.append('the edited layout does not hold the moved first sprite')
    if len([i for i in range(len(fr_layout)) if fr_layout[i] != patched[i]]) > 4 * SPRITE_ENTRIES:
        errors.append('the layout edit changes more than its position entries')
    source = (REPO / 'launcher' / 'game_translation.cpp').read_text(encoding='utf-8')
    if cpp_table(edits) not in source:
        errors.append('launcher/game_translation.cpp does not carry the same layout edits')
    out = apply(blob)
    if len(out) != len(blob) or va.ftx_structure(out) != va.ftx_structure(blob):
        errors.append('the pack structure changed')
    before = np.array(ftex.decode(blob, texture_of(blob)).convert('RGBA'))
    after = np.array(ftex.decode(out, texture_of(out)).convert('RGBA'))
    if before.shape != after.shape or before.shape[:2] != (2048, 2048):
        errors.append('atlas dimensions changed')
    diff = (before != after).any(axis=2)
    inside = np.zeros_like(diff)
    for x0, y0, x1, y1 in RECTS:
        inside[y0:y1, x0:x1] = True
    if (diff & ~inside).any():
        errors.append('pixels changed outside the label rectangles')
    ys, xs = np.nonzero(diff)
    print(f'changed pixels {diff.sum()}, bounding box x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}')
    for x0, y0, x1, y1 in RECTS:
        old = before[y0:y1, x0:x1]
        new = after[y0:y1, x0:x1]
        opaque = new[..., 3] > 128
        same = (old == new).all(axis=2) & opaque if (x0, y0, x1, y1) != GLOW else opaque & False
        share = same.sum() / max(opaque.sum(), 1)
        corr = np.corrcoef(old[..., 3].ravel().astype(float), new[..., 3].ravel().astype(float))[0, 1]
        print(f'rect {(x0, y0, x1, y1)}: {opaque.sum()} opaque pixels, {share:.1%} identical, alpha correlation {corr:.2f}')
        if share > 0.05 or corr > 0.5:
            errors.append(f'the English artwork still shows in {(x0, y0, x1, y1)}')
    for t in ftex.textures(blob):
        if t['name'] == TEXTURE:
            continue
        t2 = next(u for u in ftex.textures(out) if u['name'] == t['name'])
        if blob[t['data']:t['data'] + t['size']] != out[t2['data']:t2['data'] + t2['size']]:
            errors.append(f'{t["name"]} changed')
    tex = texture_of(blob)
    changed_blocks = [i for i in range(0, len(blob), 16) if blob[i:i + 16] != out[i:i + 16]]
    if any(not (tex['data'] <= i < tex['data'] + tex['size']) for i in changed_blocks):
        errors.append('bytes changed outside texture 0812')
    for e in errors:
        print('error:', e)
    print(f'{len(changed_blocks)} changed 16-byte blocks, {len(errors)} errors')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
