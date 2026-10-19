#!/usr/bin/env python3
"""Paint the VIEW label over the TOUCH PAD word of Interface_Add_arranged000 (2048 x 2048, in each language pack),
taking the label from the painted button atlas, so that patch_ftx.py can splice it like the button atlas:

    paint_add_word.py add.png atlas_xbox.png out.png

writes out.png and out.changed.npy.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import draw_dcp


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('add'); ap.add_argument('atlas'); ap.add_argument('out')
    a = ap.parse_args()
    add = np.array(Image.open(a.add).convert('RGBA'))
    atlas = np.array(Image.open(a.atlas).convert('RGBA'))
    changed = np.zeros(add.shape[:2], bool)
    draw_dcp.paint_add_word(add, atlas, changed)
    Image.fromarray(add).save(a.out)
    np.save(Path(a.out).with_suffix('.changed.npy'), changed)
    print(f'edited {changed.sum()} px in the TOUCH PAD word -> {a.out}')


if __name__ == '__main__':
    main()
