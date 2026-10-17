#!/usr/bin/env python3
"""Arachnophobia Safe Mode art: redraws the decoded Giant Spider atlas (GiantSpider00_arranged000) so the enemy
reads as two soft cream bodies with two orange-red eyes. Abdomen sprites become the rear body, head sprites the front
body with the eyes, every other body sprite (legs, hooks, claws, face strips, tufts) becomes transparent. Web, smoke
and the other effect pixels are kept. Everything is drawn procedurally inside the original sprites' rectangles.

    draw_spider.py original.png [atlas.png] [preview.png] [changed.npy]

Defaults write launcher/prompts/arachnophobia-spider-atlas.png, the preview next to it and changed.npy in the current
folder; changed.npy is the mask patch_ftx.py takes.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

PROMPTS = Path(__file__).resolve().parent.parent.parent / 'launcher' / 'prompts'
GROUP_GAP = 3
BLEED = 2
PURPLE_MARGIN = 10
PURPLE_SHARE = 0.5
MIN_PART_PIXELS = 2500
ABDOMEN_PIXELS = 100000
HEAD_PIXELS = (40000, 60000)
WHITE_EFFECT_BRIGHTNESS = 200

CREAM = np.array([252, 243, 218], float)
BEIGE = np.array([226, 192, 144], float)
SHADOW = np.array([158, 112, 74], float)
EYE_CENTER = np.array([255, 150, 60], float)
EYE_EDGE = np.array([205, 52, 22], float)
EYE_GLINT = np.array([255, 238, 205], float)
LIGHT = np.array([-0.48, -0.58, 0.66])
FALLBACK_FACING = np.array([0.92, 0.38])


def purple_pixels(rgba):
    r, g, b, a = (rgba[..., i].astype(int) for i in range(4))
    return (b > g + 40) & (b >= r - 10) & (a > 0)


def effect_pixels(rgba):
    r, g, b, a = (rgba[..., i].astype(int) for i in range(4))
    web = (np.minimum(np.minimum(r, g), b) >= 235) & (a < 235)
    return (purple_pixels(rgba) | web) & (a > 0)


def orange_pixels(rgba):
    r, g, b = (rgba[..., i].astype(int) for i in range(3))
    return (r > 170) & (g < 120) & (b < 70) & (r - g > 70)


def is_silk_glow(width, height, brightness):
    """The black sprites that carry a bright silk thread (thin bars, a square with a curve): the game blends them
    so the black vanishes, so they are kept as they are."""
    thin_bar = height >= 120 and width / height <= 0.25
    black_square = width >= 120 and brightness < 10 and 0.9 <= width / height <= 1.1
    return thin_bar or black_square


def classify(pixels, brightness, width, height):
    if brightness >= WHITE_EFFECT_BRIGHTNESS or is_silk_glow(width, height, brightness):
        return 'keep'
    if pixels >= ABDOMEN_PIXELS:
        return 'abdomen'
    if HEAD_PIXELS[0] <= pixels <= HEAD_PIXELS[1]:
        return 'head'
    return 'remove'


class Ellipse:
    """The ellipse with the second moments of a mask: its centre, semi-axes (major first) and major axis direction."""

    def __init__(self, mask):
        ys, xs = np.nonzero(mask)
        self.centre = np.array([xs.mean(), ys.mean()])
        values, vectors = np.linalg.eigh(np.cov(np.stack([xs, ys])))
        self.minor, self.major = 2 * np.sqrt(np.maximum(values, 1.0))
        self.axis = vectors[:, 1]
        self.normal = vectors[:, 0]
        height, width = mask.shape
        reach = np.array([np.hypot(self.major * self.axis[i], self.minor * self.normal[i]) for i in (0, 1)])
        room = np.array([min(self.centre[0], width - self.centre[0]), min(self.centre[1], height - self.centre[1])])
        scale = min(1.0, float(np.min((room - 1) / reach)))
        self.major *= scale
        self.minor *= scale

    def local(self, shape):
        """Pixel coordinates along (major, minor), in units of the semi-axes, and the pixel distance to the edge."""
        y, x = np.mgrid[:shape[0], :shape[1]].astype(float)
        dx, dy = x - self.centre[0], y - self.centre[1]
        u = (dx * self.axis[0] + dy * self.axis[1]) / self.major
        v = (dx * self.normal[0] + dy * self.normal[1]) / self.minor
        radius = np.hypot(u, v)
        return u, v, (1.0 - radius) * self.minor


def paint_body(ellipse, shape):
    """Soft cream body: RGB (H, W, 3) floats and alpha (H, W)."""
    u, v, edge = ellipse.local(shape)
    radius2 = np.clip(u * u + v * v, 0, 1)
    height = np.sqrt(1 - radius2)
    nx = u * ellipse.axis[0] + v * ellipse.normal[0]
    ny = u * ellipse.axis[1] + v * ellipse.normal[1]
    lit = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + height * LIGHT[2], 0, 1)
    lit = lit ** 0.9
    low = np.clip(lit * 2, 0, 1)[..., None]
    high = np.clip(lit * 2 - 1, 0, 1)[..., None]
    rgb = SHADOW * (1 - low) + BEIGE * low
    rgb = rgb * (1 - high) + CREAM * high
    rim = np.clip(1 - height / 0.45, 0, 1)[..., None] ** 1.5
    rgb = rgb * (1 - 0.30 * rim) + SHADOW * 0.30 * rim
    glint = np.exp(-(((nx + 0.38) ** 2 + (ny + 0.42) ** 2) / 0.05))[..., None]
    rgb = rgb + (255 - rgb) * 0.55 * glint
    return np.clip(rgb, 0, 255), np.clip(edge + 0.5, 0, 1)


def bleed_edges(region, clear, drawn):
    """Transparent texels next to the drawn body take its colour, so filtering does not darken the edge."""
    distance, (ny, nx) = ndi.distance_transform_edt(~drawn, return_indices=True)
    ring = clear & ~drawn & (distance <= BLEED)
    region[ring, :3] = region[ny[ring], nx[ring], :3]


def inside_purple(purple, visible, box):
    """Whether a small part sits in a purple effect (the dark centres of the droplets)."""
    y, x = box
    window = (slice(max(y.start - PURPLE_MARGIN, 0), y.stop + PURPLE_MARGIN),
              slice(max(x.start - PURPLE_MARGIN, 0), x.stop + PURPLE_MARGIN))
    return purple[window].sum() >= PURPLE_SHARE * visible[window].sum()


def facing(rgba, mask, ellipse):
    """Unit vector from the head's centre toward its orange face pixels (the way the head looks)."""
    ys, xs = np.nonzero(orange_pixels(rgba) & mask)
    if len(xs) < 200:
        return FALLBACK_FACING / np.linalg.norm(FALLBACK_FACING)
    pull = np.array([xs.mean(), ys.mean()]) - ellipse.centre
    return pull / np.linalg.norm(pull) if np.linalg.norm(pull) > 4 else FALLBACK_FACING / np.linalg.norm(FALLBACK_FACING)


def paint_eyes(rgb, alpha, ellipse, direction):
    """Two orange-red eyes with a small glint, sized and spaced by the body so every head looks alike."""
    size = np.sqrt(ellipse.major * ellipse.minor)
    side = np.array([-direction[1], direction[0]])
    y, x = np.mgrid[:alpha.shape[0], :alpha.shape[1]].astype(float)
    for sign in (-1, 1):
        centre = ellipse.centre + direction * 0.34 * size + side * sign * 0.25 * size
        radius = 0.16 * size
        dx, dy = x - centre[0], y - centre[1]
        d = np.hypot(dx, dy) / radius
        cover = np.clip((1 - d) * radius + 0.5, 0, 1)[..., None]
        colour = EYE_EDGE * np.clip(d, 0, 1)[..., None] ** 1.4 + EYE_CENTER * (1 - np.clip(d, 0, 1)[..., None] ** 1.4)
        ring = np.clip((d - 0.78) / 0.22, 0, 1)[..., None]
        colour = colour * (1 - 0.35 * ring)
        glint_d = np.hypot(dx + 0.30 * radius, dy + 0.34 * radius) / (0.30 * radius)
        glint = np.clip(1.4 - glint_d, 0, 1)[..., None]
        colour = colour * (1 - glint) + EYE_GLINT * glint
        rgb[:] = rgb * (1 - cover) + colour * cover
    return rgb


def redraw(rgba):
    out = rgba.copy()
    changed = np.zeros(rgba.shape[:2], bool)
    effects = effect_pixels(rgba)
    purple = purple_pixels(rgba)
    visible = rgba[..., 3] > 0
    body = (rgba[..., 3] > 0) & ~effects
    groups, count = ndi.label(ndi.binary_dilation(body, iterations=GROUP_GAP), structure=np.ones((3, 3)))
    kinds = {'abdomen': 0, 'head': 0, 'remove': 0, 'keep': 0}
    for index, box in enumerate(ndi.find_objects(groups), 1):
        part = ndi.binary_fill_holes((groups[box] == index) & body[box])
        kind = classify(int(part.sum()), float(rgba[box][part][:, :3].mean()), part.shape[1], part.shape[0])
        kinds[kind] += 1
        if kind == 'remove' and inside_purple(purple, visible, box):
            kind = 'keep'
        if kind == 'keep':
            continue
        allowed = part | ~effects[box]
        clear = part & allowed
        region = out[box]
        region[clear] = (0, 0, 0, 0)
        shape = part.shape
        if kind != 'remove':
            ellipse = Ellipse(part)
            rgb, alpha = paint_body(ellipse, shape)
            if kind == 'head':
                rgb = paint_eyes(rgb, alpha, ellipse, facing(rgba[box], part, ellipse))
            alpha = alpha * allowed
            drawn = alpha > 0
            region[drawn, :3] = np.round(rgb[drawn])
            region[drawn, 3] = np.round(alpha[drawn] * 255)
            bleed_edges(region, clear, drawn)
            clear = clear | drawn
        changed[box] |= clear
    return out, changed, kinds


def checkerboard(shape, cell=32):
    y, x = np.mgrid[:shape[0], :shape[1]]
    return np.where(((x // cell + y // cell) % 2)[..., None] == 0, 52, 70).astype(np.uint8).repeat(3, axis=2)


def preview(atlas):
    """The atlas over a checkerboard, cropped to the painted area, at full resolution."""
    ys, xs = np.nonzero(atlas[..., 3] > 0)
    top, bottom, left, right = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    crop = atlas[top:bottom, left:right].astype(float)
    back = checkerboard(crop.shape[:2]).astype(float)
    alpha = crop[..., 3:4] / 255
    return Image.fromarray(np.round(crop[..., :3] * alpha + back * (1 - alpha)).astype(np.uint8))


def main():
    original = Path(sys.argv[1])
    atlas_out = Path(sys.argv[2]) if len(sys.argv) > 2 else PROMPTS / 'arachnophobia-spider-atlas.png'
    preview_out = Path(sys.argv[3]) if len(sys.argv) > 3 else PROMPTS / 'arachnophobia-spider-preview.png'
    changed_out = Path(sys.argv[4]) if len(sys.argv) > 4 else Path('changed.npy')
    atlas, changed, kinds = redraw(np.array(Image.open(original).convert('RGBA')))
    Image.fromarray(atlas).save(atlas_out)
    preview(atlas).save(preview_out)
    np.save(changed_out, changed)
    print(f"{kinds['abdomen']} abdomens, {kinds['head']} heads, {kinds['remove']} parts removed, {kinds['keep']} kept, "
          f'{int(changed.sum())} pixels changed')


if __name__ == '__main__':
    main()
