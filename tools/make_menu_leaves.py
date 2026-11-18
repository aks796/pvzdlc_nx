#!/usr/bin/env python3
"""make_menu_leaves.py OUT: the main menu's leaves strip (reanim/SelectorScreen_Leaves.png, 240x62, the grass over
the tree's foot) with its see-through pixels coloured as the leaves beside them. They were white: drawn smoothed
(scaled, filtered), the white bled into the strip's soft left edge as a pale line in the dark grass. Visible
pixels are unchanged."""
import sys
import numpy as np
from PIL import Image

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

a = np.asarray(Image.open(ROOT + 'analysis/main_pak/reanim/SelectorScreen_Leaves.png')
               .convert('RGBA')).astype(np.float32)
rgb, al = a[..., :3].copy(), a[..., 3]
known = al > 0
# grow the known colours outwards a pixel at a time (the mean of known neighbours) until every pixel has one
for _ in range(300):
    if known.all():
        break
    acc = np.zeros_like(rgb)
    cnt = np.zeros(al.shape, np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            k = np.roll(np.roll(known, dy, 0), dx, 1)
            acc += np.roll(np.roll(rgb, dy, 0), dx, 1) * k[..., None]
            cnt += k
    new = ~known & (cnt > 0)
    rgb[new] = acc[new] / cnt[new][:, None]
    known = known | new
out = np.dstack([rgb, al]).clip(0, 255).astype(np.uint8)
Image.fromarray(out).save(sys.argv[1])
print(sys.argv[1], out.shape)
