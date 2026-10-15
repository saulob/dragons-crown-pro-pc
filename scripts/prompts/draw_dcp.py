#!/usr/bin/env python3
"""Paint Xbox glyphs over the PlayStation ones in Dragon's Crown Pro's button atlas
(Interface_arranged000, 1024x512), in place: every sprite keeps its rectangle.

    draw_dcp.py atlas.png out.png [--font Poppins-Bold.ttf]

writes the edited atlas, a mask of the changed pixels (out.changed.npy) and a before/after sheet. The letters are set
in Poppins Bold (SIL Open Font License, fonts.google.com/specimen/Poppins); --font names the file when it is not
installed where the script looks.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy import ndimage

FONT_CANDIDATES = ['Poppins-Bold.ttf', '/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf',
                   'C:/Windows/Fonts/Poppins-Bold.ttf']
FONT_BOLD = next((f for f in FONT_CANDIDATES if Path(f).is_file()), FONT_CANDIDATES[0])
FONT_LABEL = FONT_BOLD
XBOX = {'A': (98, 192, 78), 'B': (228, 68, 60), 'X': (70, 142, 236), 'Y': (242, 200, 56)}

# (x0, y0, x1, y1) boxes from the atlas inventory and what to do with each.
DISCS = {(449, 1, 543, 95): 'B', (545, 1, 639, 95): 'Y', (641, 1, 735, 95): 'A',
         (477, 129, 571, 223): 'X', (573, 129, 667, 223): 'B',
         (513, 341, 607, 435): 'A', (609, 341, 703, 435): 'X', (105, 301, 199, 395): 'Y'}
GUIDE = [(669, 129, 763, 223), (1, 208, 147, 299)]            # PS logo discs -> plain guide ring
SMALL = {(883, 3, 941, 61): 'B', (947, 3, 997, 56): 'A', (737, 60, 807, 119): 'Y', (809, 57, 863, 111): 'B',
         (855, 130, 913, 190): 'A', (917, 129, 963, 175): 'X', (633, 438, 695, 495): 'Y', (777, 409, 831, 463): 'X'}
LABELS = {(739, 3, 876, 53): 'MENU', (201, 399, 385, 449): 'MENU',
          (203, 455, 370, 505): 'VIEW', (483, 251, 631, 301): 'VIEW'}
OVALS = {(203, 249, 347, 339): 'MENU', (2, 112, 178, 203): 'VIEW'}
DIGITS = {(854, 193, 889, 243): 'T', (635, 249, 667, 299): 'B'}
BUMPERS = {(407, 342, 510, 435): 'LB', (2, 406, 105, 499): 'RB'}


def glyph_mask(a, box, thresh=80):
    x0, y0, x1, y1 = box
    sub = a[y0:y1, x0:x1]
    lum = sub[..., :3].astype(int).max(-1)
    m = (lum > thresh) & (sub[..., 3] > 32)
    return m


def rebuild_disc(a, box, frac=0.78):
    """Erases the inside of a glossy disc: every pixel closer than frac*R to the centre takes the
    colour found at frac*R along the same angle, so the rim and the top highlight survive."""
    x0, y0, x1, y1 = box
    sub = a[y0:y1, x0:x1]
    h, w = sub.shape[:2]
    cy, cx = (h - 1) / 2, (w - 1) / 2
    R = 1.0                      # normalised elliptical radius: 1 at the box edge
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.sqrt(((yy - cy) / (h / 2)) ** 2 + ((xx - cx) / (w / 2)) ** 2)
    ang = np.arctan2((yy - cy) / (h / 2), (xx - cx) / (w / 2))
    # sample the clean annulus [frac, frac+0.05]*R, averaged over a small window, per angle bin
    nb = 180
    bins = ((ang + np.pi) / (2 * np.pi) * nb).astype(int) % nb
    ann = (r >= frac * R) & (r < (frac + 0.06) * R)
    prof = np.zeros((nb, 4))
    for b in range(nb):
        sel = ann & (bins == b)
        if sel.sum() == 0:
            sel = ann & (np.abs(((bins - b + nb // 2) % nb) - nb // 2) <= 2)
        prof[b] = np.median(sub[sel], axis=0)
    # smooth the profile around the circle
    k = np.array([1, 2, 3, 4, 5, 4, 3, 2, 1], float); k /= k.sum()
    for c in range(4):
        prof[:, c] = np.convolve(np.r_[prof[-4:, c], prof[:, c], prof[:4, c]], k, 'valid')
    inner = r < frac * R
    sub[inner] = prof[bins[inner]].round().astype(np.uint8)


def text_layer(size, text, font_path, px, fill, stroke=0, stroke_fill=(0, 0, 0, 255), scale=4):
    big = Image.new('RGBA', (size[0] * scale, size[1] * scale), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    f = ImageFont.truetype(font_path, px * scale)
    box = d.textbbox((0, 0), text, font=f, anchor='ls', stroke_width=stroke * scale)
    x = (big.width - (box[2] - box[0])) / 2 - box[0]
    y = (big.height - (box[3] - box[1])) / 2 - box[1]
    d.text((x, y), text, font=f, fill=fill, anchor='ls', stroke_width=stroke * scale, stroke_fill=stroke_fill)
    return big.resize(size, Image.LANCZOS)


def paste(a, box, layer):
    x0, y0, x1, y1 = box
    base = Image.fromarray(a[y0:y1, x0:x1])
    base.alpha_composite(layer)
    a[y0:y1, x0:x1] = np.array(base)


def fit_px(box, frac):
    return max(8, int((box[3] - box[1]) * frac))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('atlas'); ap.add_argument('out')
    ap.add_argument('--font', help='Poppins-Bold.ttf (default: the first of %s that exists)' % ', '.join(FONT_CANDIDATES))
    args = ap.parse_args()
    global FONT_BOLD, FONT_LABEL
    if args.font:
        FONT_BOLD = FONT_LABEL = args.font
    if not Path(FONT_BOLD).is_file():
        ap.error(f'font not found: {FONT_BOLD} (pass --font)')
    src = Image.open(args.atlas).convert('RGBA')
    a = np.array(src)
    changed = np.zeros(a.shape[:2], bool)

    def mark(box):
        changed[box[1]:box[3], box[0]:box[2]] = True

    # face buttons: inpaint the symbol, draw the letter with a soft dark edge so it sits in the gloss
    for box, letter in DISCS.items():
        rebuild_disc(a, box)
        size = (box[2] - box[0], box[3] - box[1])
        shadow = text_layer(size, letter, FONT_BOLD, fit_px(box, 0.6), (0, 0, 0, 200)).filter(ImageFilter.GaussianBlur(1.5))
        paste(a, box, shadow)
        paste(a, box, text_layer(size, letter, FONT_BOLD, fit_px(box, 0.6), XBOX[letter] + (255,)))
        mark(box)
    # PS logo discs -> a plain ring (guide button), same ink as the logo
    for box in GUIDE:
        rebuild_disc(a, box)
        w, h = box[2] - box[0], box[3] - box[1]
        size = (w, h)
        ring = Image.new('RGBA', (w * 4, h * 4), (0, 0, 0, 0)); d = ImageDraw.Draw(ring)
        cx, cy, r = w * 2, h * 2, min(w, h) * 4 * 0.22
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(235, 235, 235, 255), width=int(min(w, h) * 4 * 0.055))
        d.ellipse((cx - r * 0.35, cy - r * 0.35, cx + r * 0.35, cy + r * 0.35), fill=(235, 235, 235, 255))
        paste(a, box, ring.resize(size, Image.LANCZOS)); mark(box)
    # small outline glyphs -> white letters with a dark edge (they sit on text)
    for box, letter in SMALL.items():
        x0, y0, x1, y1 = box
        a[y0:y1, x0:x1] = 0
        size = (x1 - x0, y1 - y0)
        paste(a, box, text_layer(size, letter, FONT_BOLD, fit_px(box, 0.95), (255, 255, 255, 255), stroke=2)); mark(box)
    # word labels (START/OPTIONS/SELECT/SHARE) -> MENU / VIEW in the same bold outlined style
    for box, word in LABELS.items():
        x0, y0, x1, y1 = box
        a[y0:y1, x0:x1] = 0
        size = (x1 - x0, y1 - y0)
        paste(a, box, text_layer(size, word, FONT_LABEL, fit_px(box, 0.78), (255, 255, 255, 255), stroke=3)); mark(box)
    # oval START/SELECT buttons: blank the white word inside the oval, draw MENU/VIEW
    for box, word in OVALS.items():
        x0, y0, x1, y1 = box
        sub = a[y0:y1, x0:x1]
        lum = sub[..., :3].astype(int).max(-1)
        h, w = lum.shape; yy, xx = np.mgrid[0:h, 0:w]
        rho = ((xx - (w - 1) / 2) / (w * 0.5)) ** 2 + ((yy - (h - 1) / 2) / (h * 0.5)) ** 2
        inside = rho < 0.96
        # the word is a horizontal band; bridge it column by column with a linear gradient
        rows = np.nonzero(((lum > 90) & inside).any(1))[0]
        top, bot = max(rows.min() - 4, 1), min(rows.max() + 5, h - 2)
        f = sub.astype(np.float64)
        t = ((np.arange(top, bot) - (top - 1)) / (bot - top + 1))[:, None, None]
        band = f[top - 1][None] * (1 - t) + f[bot][None] * t
        m = inside[top:bot]
        sub[top:bot][m] = band.round().astype(np.uint8)[m]
        size = (w, h)
        paste(a, box, text_layer(size, word, FONT_LABEL, fit_px(box, 0.40), (255, 255, 255, 255), stroke=1)); mark(box)
    # the 1 / 2 glyphs composed onto L/R bodies -> B / T
    for box, ch in DIGITS.items():
        x0, y0, x1, y1 = box
        a[y0:y1, x0:x1] = 0
        size = (x1 - x0, y1 - y0)
        paste(a, box, text_layer(size, ch, FONT_LABEL, fit_px(box, 0.95), (255, 255, 255, 255), stroke=2)); mark(box)
    # the L / R bumper shapes with the letter inside -> LB / RB
    for box, word in BUMPERS.items():
        x0, y0, x1, y1 = box
        sub = a[y0:y1, x0:x1]
        lum = sub[..., :3].astype(int).max(-1)
        light = (lum > 170) & (sub[..., 3] > 200)
        interior = ndimage.binary_erosion(ndimage.binary_fill_holes(light), iterations=4)
        # the letter is whatever is darker than its row's body colour
        rowmed = np.array([np.median(lum[y][interior[y]]) if interior[y].any() else 255 for y in range(lum.shape[0])])
        letter = ndimage.binary_dilation(interior & (lum < rowmed[:, None] - 12), iterations=3) & interior
        # the body is a vertical gradient: fill each row of the letter with that row's light median
        for y in np.unique(np.nonzero(letter)[0]):
            row_light = light[y] & ~letter[y]
            if row_light.sum() < 3:
                continue
            sub[y, letter[y]] = np.median(sub[y, row_light], axis=0).round().astype(np.uint8)
        h, w = lum.shape
        size = (w, h)
        shadow = text_layer(size, word, FONT_LABEL, fit_px(box, 0.42), (255, 255, 255, 255), stroke=2, stroke_fill=(140, 140, 140, 255))
        paste(a, box, text_layer(size, word, FONT_LABEL, fit_px(box, 0.42), (70, 70, 70, 255), stroke=1, stroke_fill=(120, 120, 120, 255)))
        mark(box)
    out = Image.fromarray(a)
    out.save(args.out)
    np.save(Path(args.out).with_suffix('.changed.npy'), changed)
    # before/after sheet
    S = Image.new('RGBA', (1024, 1024 + 8), (60, 60, 60, 255))
    for k, im in enumerate((src, out)):
        bg = Image.new('RGBA', im.size, (60, 60, 60, 255)); bg.alpha_composite(im); S.paste(bg, (0, k * 516))
    S.save(Path(args.out).with_name('dcp_compare.png'))
    print(f'edited {changed.sum()} px in {len(DISCS) + len(GUIDE) + len(SMALL) + len(LABELS) + len(OVALS) + len(DIGITS) + len(BUMPERS)} boxes -> {args.out}')


if __name__ == '__main__':
    main()
