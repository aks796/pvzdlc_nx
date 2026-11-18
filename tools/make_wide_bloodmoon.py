#!/usr/bin/env python3
"""make_wide_bloodmoon.py OUT: the Moon stages' blood moon (IMAGE_BACKGROUND_BLOODMOON, background6.png, 1400x600),
widened to HenryJk's night roof (wide/hj_background6boss.png, 1666x600: the original 133 px further right).

The DLC's blood moon is its night roof (background6boss) reddened, its white moon turned into a red one; the game
fades it in over the night roof. Over HenryJk's wider night roof it didn't fit: HenryJk's moon is drawn 10 right
and 51 up of the original's (found by matching the crescent, correlation 0.999), so the red moon rose below it,
and the red stopped 133 px short of the right edge. Here:
  - the reddening as a smooth gain per pixel (blood / night roof), measured away from the moon and the glow
    around it, run out to the wider picture's ends, and applied to HenryJk's night roof;
  - the blood moon's red moon (with its glow) laid over HenryJk's moon, feathered into the sky.
"""
import sys
import numpy as np
from PIL import Image

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

P = ROOT + ''
night = np.asarray(Image.open(P + 'analysis/main_pak/images/background6boss.jpg').convert('RGB')).astype(np.float32)
blood = np.asarray(Image.open(P + 'analysis/main_pak/images/background6.png').convert('RGB')).astype(np.float32)
wide = np.asarray(Image.open(P + 'gamedata/wide/hj_background6boss.png').convert('RGB')).astype(np.float32)
PAD = 133
MOON = (273, 128)      # the moon's centre in the DLC's pictures
SHIFT = (0, 0)         # HenryJk's moon from it (0: make_wide_moon.py has put it back at the game's place; HenryJk's own was +10, -51)
GLOW = 75              # the red moon's glow reaches this far

h, w = night.shape[:2]
yy, xx = np.mgrid[0:h, 0:w]
near_moon = (xx - MOON[0]) ** 2 + (yy - MOON[1]) ** 2 < GLOW ** 2


def blur(a, r):
    """box blur, 3 passes, radius r (both axes), edges held"""
    for _ in range(3):
        for ax in (0, 1):
            n = a.shape[ax]
            c = np.cumsum(np.pad(a, [(r + 1, r) if i == ax else (0, 0) for i in range(a.ndim)], mode='edge'), axis=ax)
            hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=ax)
            lo = np.take(c, np.arange(0, n), axis=ax)
            a = (hi - lo) / (2 * r + 1)
    return a


# the reddening: blood / night, smoothed with the moon's area left out (normalised: weights blurred alike)
gain = np.clip(blood / np.maximum(night, 8.0), 0.2, 4.0)
wt = (~near_moon).astype(np.float32)[..., None]
R = 14
wt3 = np.repeat(wt, 3, axis=2)
g_far = blur(gain * wt, 60) / np.maximum(blur(wt3, 60), 1e-3)  # (for the moon's area, which has no samples near)
g = (blur(gain * wt, R) + 0.02 * g_far) / (blur(wt3, R) + 0.02)
# out to the wider picture: the edge columns held
g_wide = np.pad(g, ((0, 0), (PAD, PAD), (0, 0)), mode='edge')
out = wide * g_wide

# the red moon: the blood moon's own, with its glow and the sky round it, laid over HenryJk's white moon (the
# same crescent: it covers it), feathered into the reddened sky
mx, my = MOON
r = GLOW
patch = blood[my - r:my + r, mx - r:mx + r]
d = np.sqrt((np.mgrid[-r:r, -r:r] ** 2).sum(0))
tx, ty = mx + SHIFT[0] + PAD, my + SHIFT[1]
y0, x0 = ty - r, tx - r
ys0 = max(0, -y0)  # (HenryJk's moon is near the top: the patch may start above the picture)
dst = out[y0 + ys0:ty + r, x0:tx + r]
p = patch[ys0:]
# the moon itself whole (radius 30); past it only the glow: where the blood moon adds light over the reddened
# night roof (the treetop under the DLC's moon is in both, so it stays out), fading out by the glow's edge
lum = lambda a: a @ np.array([0.3, 0.59, 0.11], np.float32)
added = lum((blood - night * g)[my - r:my + r, mx - r:mx + r])[ys0:]
inner = np.clip((34 - d[ys0:]) / 4.0, 0, 1)
glow = np.clip(added / 25.0, 0, 1) * np.clip((r - d[ys0:]) / 20.0, 0, 1)
wt = np.maximum(inner, glow)[..., None]
dst[:] = dst * (1 - wt) + p * wt
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(sys.argv[1])
print(sys.argv[1], out.shape[1], 'x', out.shape[0])
