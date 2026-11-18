#!/usr/bin/env python3
"""make_wide_moon.py SRC OUT: the night roof's moon where the game has it. HenryJk's wide night roof
(hj_background6boss.png, IMAGE_BACKGROUND6BOSS, the night roof and the Moon stages) draws the moon 10 px right
and 51 px up of the DLC's 4:3 picture (found by matching the crescent), which put it behind the seed bank in a
level; in the 4:3 game it shows just below the bank. HenryJk's own moon (with its glow) is moved back to the
game's place (open sky there, above the pole), and its old place gets the sky from 130 px along the same rows
(stars, no moon), feathered."""
import sys
import numpy as np
from PIL import Image

h = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.float32)
OLD, NEW, R = (416, 77), (406, 128), 52   # the moon's centre in HenryJk's picture and where the game has it
yy, xx = np.mgrid[-R:R, -R:R]
dist = np.sqrt(xx ** 2 + yy ** 2)
feather = np.clip((R - dist) / 14.0, 0, 1)[..., None]


def box(c):
    return slice(max(0, c[1] - R), c[1] + R), slice(c[0] - R, c[0] + R)


def crop(a, c):
    """the R-radius square at c (rows above the picture padded from its top row)"""
    out = np.zeros((2 * R, 2 * R, 3), np.float32)
    y0 = c[1] - R
    for i in range(2 * R):
        out[i] = a[max(0, y0 + i), c[0] - R:c[0] + R]
    return out


moon = crop(h, OLD)
# the old place, out to its glow's edge (RG): the sky along its rows
RG = 80
gy, gx = np.mgrid[OLD[1] - RG:OLD[1] + RG, OLD[0] - RG:OLD[0] + RG]
gd = np.sqrt((gx - OLD[0]) ** 2 + (gy - OLD[1]) ** 2)
gw = np.clip((RG - gd) / 16.0, 0, 1) * np.clip((122 - gy) / 10.0, 0, 1)  # (above the roof the shifted rows reach)
for i in range(2 * RG):
    y = OLD[1] - RG + i
    if y < 0: continue
    for j in range(2 * RG):
        x = OLD[0] - RG + j
        if gw[i, j] > 0: h[y, x] = h[y, x] * (1 - gw[i, j]) + h[y, x + 2 * RG + 10] * gw[i, j]
# the new place: the moon and its glow over the sky there (lighter of the two: its glow fades into the sky)
dst = h[box(NEW)]
m = moon[NEW[1] - R - (NEW[1] - R):]
dst[:] = np.maximum(dst, dst * (1 - feather) + m * feather)
Image.fromarray(np.clip(h, 0, 255).astype(np.uint8)).save(sys.argv[2])
print(sys.argv[2])
