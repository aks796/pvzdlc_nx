#!/usr/bin/env python3
"""make_woods_trunk.py OUT_DIR: a thick trunk for the Woods' right edge (backgrounds 11 day, 15 night), drawn in the
place the roofs have HenryJk's pole, matching the thick trunks along the top of the Woods picture: the Tree of
Wisdom's grown trunk (reanim/tree9.png), recoloured to those trunks (background7.png's top band: a gain per
channel from the two's median bark colours; background8alt.png's at night), and darkened (DARK: it stands nearer, in
the shade); from under its cut top down to where its roots start
to spread (rows 60..395: its own gentle taper and a little flare at the foot, not the wide roots and grass),
scaled to the screen's height. With knots in its part in view: the Woods' own (a round knot from
its background trunks, scaled and tinted to the bark where it goes) (its own
bark grain kept: smoothed into a twisted look, it looked artificial). Out: woods_trunk.png, woods_trunk_night.png (W x 600)."""
import sys
import numpy as np
from PIL import Image

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

I = ROOT + 'analysis/main_pak/'
tree = np.asarray(Image.open(I + 'reanim/tree9.png').convert('RGBA')).astype(np.float32)
day = np.asarray(Image.open(I + 'images/background7.png').convert('RGB')).astype(np.float32)
night = np.asarray(Image.open(I + 'images/background8alt.png').convert('RGB')).astype(np.float32)
lw = np.array([0.3, 0.59, 0.11], np.float32)


def bark(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    l = a @ lw
    return (r > g + 12) & (g > b + 8) & (l > 60) & (l < 200)


band_d, band_n = day[0:60, 850:1400], night[0:60, 850:1400]
m = bark(band_d)
td, tn = np.median(band_d[m], 0), np.median(band_n[m], 0)
mt = bark(tree[..., :3]) & (tree[..., 3] > 200)
tt = np.median(tree[..., :3][mt], 0)
grass = (tree[..., 1] > tree[..., 0] + 20) & (tree[..., 1] > tree[..., 2])
print('bark: tree', tt.round(), 'woods day', td.round(), 'night', tn.round())
DARK, W = 0.72, 330
mid = tree[60:395, 80:400]
for name, target in (('woods_trunk', td), ('woods_trunk_night', tn)):
    out = mid.copy()
    out[..., :3] = np.clip(mid[..., :3] * (target / tt) * DARK, 0, 255)
    img = Image.fromarray(out.astype(np.uint8), 'RGBA').resize((W, 600), Image.LANCZOS)
    a = np.asarray(img).astype(np.float32)
    yy, xx = np.mgrid[0:600, 0:W]

    def knot(cx, cy, k):
        # the Woods' own knot (a round one on a background trunk: background7 x 948..966, y 5..22, centred 957, 13.5),
        # k x as big, tinted to the bark where it goes (its bark round it to ours), blended in at its edge
        src = day[5:23, 948:967] if name == 'woods_trunk' else night[5:23, 948:967]
        h0, w0 = src.shape[:2]
        sy, sx = np.mgrid[0:h0, 0:w0]
        dk = np.sqrt(((sx - 9.0) / 7.0) ** 2 + ((sy - 8.5) / 6.5) ** 2)
        ring = (dk > 1.05) & (dk < 1.4)
        w1, h1 = int(round(w0 * k)), int(round(h0 * k))
        big = np.asarray(Image.fromarray(src.clip(0, 255).astype(np.uint8)).resize((w1, h1), Image.BICUBIC)).astype(np.float32)
        mk = np.clip((1.3 - np.sqrt(((np.mgrid[0:h1, 0:w1][1] / k - 9.0) / 7.0) ** 2 + ((np.mgrid[0:h1, 0:w1][0] / k - 8.5) / 6.5) ** 2)) / 0.3, 0, 1)
        x0, y0 = int(cx - w1 / 2), int(cy - h1 / 2)
        dst = a[y0:y0 + h1, x0:x0 + w1, :3]
        gain = np.median(dst.reshape(-1, 3), 0) / np.maximum(np.median(src[ring], 0), 1)
        dst[:] = dst * (1 - mk[..., None]) + np.clip(big * gain, 0, 255) * mk[..., None]

    knot(118, 175, 3.2)
    knot(150, 400, 1.6)
    knot(95, 120, 1.1)
    Image.fromarray(a.clip(0, 255).astype(np.uint8), 'RGBA').save(f'{sys.argv[1]}/{name}.png')
    print(f'{sys.argv[1]}/{name}.png')
