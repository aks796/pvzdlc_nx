#!/usr/bin/env python3
"""make_pole_bloodmoon.py OUT: HenryJk's night pole (wide/hj_pole_night.png) under the Moon's blood moon.

The DLC's blood moon (IMAGE_BACKGROUND_BLOODMOON, background6.png) is its night roof (background6boss) relit red,
faded in over it (Board +0x5a50), the red falling off from the moon's side to the right. The pole stands in front of
the wall at the right; it gets the light the wall beside it gets in the port's blood moon (wide/hj_bloodmoon.png,
the reddening made by make_wide_bloodmoon.py): each channel's gain (blood / night, means over the wall's pixels
next to the pole: rows 60..330, the 180 columns from 600 in from the right edge). A gain, not an offset: a light
(the shadows stay dark; an offset fitted on the roof's dish washed the pole pink), scaled so the pole darkens as
much as the wall does (its bluer colours lost more under the plain gain). Its alpha is kept; the port
draws it over the pole with the blood moon's own alpha (wide_art.c pole_upto)."""
import sys
import numpy as np
from PIL import Image

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

P = ROOT + ''
pole = np.asarray(Image.open(P + 'gamedata/wide/hj_pole_night.png').convert('RGBA')).astype(np.float32)

night_w = np.asarray(Image.open(P + 'gamedata/wide/hj_background6boss.png').convert('RGB')).astype(np.float32)
blood_w = np.asarray(Image.open(P + 'gamedata/wide/hj_bloodmoon.png').convert('RGB')).astype(np.float32)
W = night_w.shape[1]
n = night_w[60:330, W - 600:W - 420].reshape(-1, 3)
b = blood_w[60:330, W - 600:W - 420].reshape(-1, 3)
wall = n.sum(1) > 90  # (not the sky above it)
gain = b[wall].mean(0) / n[wall].mean(0)
# the same darkening as the wall too: the per-channel gain on the pole's own (bluer) colours darkened it more
# (in game: pole 0.83 of its night brightness, the wall beside it 0.89), so the gain is scaled until the pole's
# brightness changes by the wall's ratio
lum = np.array([0.299, 0.587, 0.114], np.float32)
wall_ratio = (b[wall] @ lum).mean() / (n[wall] @ lum).mean()
solid = pole[..., 3] > 128
pole_ratio = ((pole[solid][:, :3] * gain) @ lum).mean() / (pole[solid][:, :3] @ lum).mean()
gain *= wall_ratio / pole_ratio
print('gain', gain.round(3).tolist(), 'brightness ratio', round(float(wall_ratio), 3))
out = pole.copy()
out[..., :3] = pole[..., :3] * gain
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), 'RGBA').save(sys.argv[1])
print(sys.argv[1], out.shape[1], 'x', out.shape[0])
