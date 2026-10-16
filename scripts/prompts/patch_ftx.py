#!/usr/bin/env python3
"""Splices an edited atlas back into a Vanillaware FTEX pack: only the 4x4 blocks whose pixels
changed are re-encoded (BC7 mode 6) and written into the tiled texture; everything else is kept
byte for byte.

    patch_ftx.py pack.ftx texture_name edited.png changed.npy out.ftx [--verify]
"""
import argparse
import io
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bc7
import ftex


def patch(blob, tex, rgba, changed):
    bw, bh, pb = tex['width'] // 4, tex['height'] // 4, tex['pitch'] // 4
    blocks_changed = changed.reshape(bh, 4, bw, 4).any(axis=(1, 3))
    ys, xs = np.nonzero(blocks_changed)
    px = np.stack([rgba[y * 4:y * 4 + 4, x * 4:x * 4 + 4] for y, x in zip(ys, xs)]).reshape(-1, 16, 4)
    enc = bc7.encode_blocks(px)
    out = bytearray(blob)
    base = tex['data']
    for k, (y, x) in enumerate(zip(ys, xs)):
        o = base + ftex.tiled_offset(int(x), int(y), pb)
        out[o:o + 16] = enc[k * 16:(k + 1) * 16]
    return bytes(out), len(ys)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('pack'); ap.add_argument('name'); ap.add_argument('png'); ap.add_argument('changed')
    ap.add_argument('out'); ap.add_argument('--verify', action='store_true')
    a = ap.parse_args()
    blob = Path(a.pack).read_bytes()
    tex = next(t for t in ftex.textures(blob) if t['name'] == a.name)
    rgba = np.array(Image.open(a.png).convert('RGBA'))
    changed = np.load(a.changed)
    assert rgba.shape[:2] == (tex['height'], tex['width']) == changed.shape
    out, n = patch(blob, tex, rgba, changed)
    Path(a.out).write_bytes(out)
    print(f'{a.name}: re-encoded {n} of {tex["width"] * tex["height"] // 16} blocks -> {a.out} ({len(out)} bytes)')
    if a.verify:
        back = np.array(ftex.decode(out, tex).convert('RGBA')).astype(np.float64)
        diff = ((back - rgba) ** 2).sum(-1)
        mse_changed = diff[changed].mean()
        print(f'  changed pixels PSNR {10 * np.log10(255 ** 2 * 4 / max(mse_changed, 1e-9)):.1f} dB, '
              f'untouched pixels differ: {(diff[~changed] > 0).sum()}')


if __name__ == '__main__':
    main()
