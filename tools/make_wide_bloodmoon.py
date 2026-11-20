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
# The glow, as the original has it (user, 2026-11-20: fix the glow on the moon only; it was the blood moon's own
# pixels blended in out to 75 px with its sky, a darker disc round the moon where its purple sky met the port's
# navy one). The original's colours round the moon are read in polar form (distance, angle) from its sky only:
# not the chimney or walls (bricks), the tree right of the moon, or its stars (a median across each ring's
# neighbouring angles); directions the tree or chimney block are filled from the angles either side. Those colours
# are used for the port's sky round its moon: exactly the original's near it, easing into the port's own sky colour
# by 135 px, with the port's own detail on top (HenryJk's stars and power lines, as their difference from the sky
# round them). The chimney and walls keep their relit colours. Then the moon's disc: the original's pixels.


def box(a, r):
    """mean over a (2r+1) square window, edges held"""
    for ax in (0, 1):
        n = a.shape[ax]
        c = np.cumsum(np.pad(a, [(r + 1, r) if i == ax else (0, 0) for i in range(a.ndim)], mode='edge'), axis=ax)
        a = (np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=ax) - np.take(c, np.arange(0, n), axis=ax)) / (2 * r + 1)
    return a


H, W = blood.shape[:2]
RMAX, NA = 140, 180
by, bx = np.mgrid[0:H, 0:W]
br = np.sqrt((bx - mx) ** 2 + (by - my) ** 2)
ba = (np.degrees(np.arctan2(by - my, bx - mx)) + 180.0) * NA / 360.0
nR, nG, nB = night[..., 0], night[..., 1], night[..., 2]
bricks = (nR > 60) & (nB - nG > 25)
bang = np.degrees(np.arctan2(by - my, bx - mx))
tree = (nG > 28) & (br > 58) & (bang > -75) & (bang < 75)   # (right of the moon; its dark parts too)
tree = box(tree.astype(np.float32), 8) > 0   # (and its lit edge)
clear = ~bricks & ~tree & (br < RMAX + 2)
polar = np.full((RMAX + 2, NA, 3), np.nan, np.float32)
ri, ai = br.astype(int), np.minimum(ba.astype(int), NA - 1)
for q in range(36, RMAX + 2):
    ring = clear & (ri == q)
    if not ring.any(): continue
    for k in range(3):
        v = np.full(NA, np.nan, np.float32)
        vals, idx = blood[..., k][ring], ai[ring]
        for t in np.unique(idx):
            v[t] = np.median(vals[idx == t])
        polar[q, :, k] = v
for q in range(36, RMAX + 2):   # (the gaps from the angles either side, round the circle; stars out by a median)
    for k in range(3):
        v = polar[q, :, k]
        ok = ~np.isnan(v)
        if ok.sum() < 3: continue
        t = np.arange(NA)
        tt = np.concatenate([t[ok] - NA, t[ok], t[ok] + NA])
        vv = np.tile(v[ok], 3)
        v = np.interp(t, tt, vv)
        polar[q, :, k] = np.array([np.median(np.take(v, range(j - 3, j + 4), mode='wrap')) for j in range(NA)])
polar[:36] = polar[36]
for k in range(3):   # (along each ray too: the filled directions' rings even)
    polar[36:, :, k] = box(polar[36:, :, k], 2)
far_dlc = np.nanmedian(polar[RMAX - 10:RMAX + 2].reshape(-1, 3), axis=0)

GR = RMAX
cy, cx = my + SHIFT[1], mx + SHIFT[0] + PAD
ya, xa = max(0, cy - GR), cx - GR
sl = (slice(ya, cy + GR), slice(xa, cx + GR))
o, hjn = out[sl], wide[sl]
oy, ox = np.mgrid[ya - cy:cy + GR - cy, -GR:GR]
od = np.sqrt(ox ** 2 + oy ** 2)
oa = (np.degrees(np.arctan2(oy, ox)) + 180.0) * NA / 360.0
r0 = np.clip(od, 0, RMAX).astype(int)
a0 = oa.astype(int) % NA
fa = (oa - np.floor(oa))[..., None]
glow = polar[r0, a0] * (1 - fa) + polar[r0, (a0 + 1) % NA] * fa
hR, hG, hB = hjn[..., 0], hjn[..., 1], hjn[..., 2]
hj_sky = (~((hR > 60) & (hB - hG > 25)) | (od < 40)).astype(np.float32)[..., None]   # (his crescent: under the disc)
hj_sky = box(hj_sky, 1)


def sky_mean(a, r):
    return box(a * hj_sky, r) / np.maximum(box(hj_sky, r), 1e-3)


far_port = sky_mean(o, 14)   # (the port's own sky round each pixel, so the glow ends in it with no ring)
ease = np.clip((od - 70) / 60.0, 0, 1)[..., None] ** 0.8
glow = glow + (far_port - far_dlc) * ease
detail = o - sky_mean(o, 4)
detail[od < 75] = 0   # (HenryJk's own moon and halo: not his detail to keep)
f = np.clip((RMAX - od) / 20.0, 0, 1)[..., None] * hj_sky
o[:] = o * (1 - f) + (glow + detail) * f
dd = d[ys0:]
# near the moon, out to 56 px, nothing but its glow and sky in the original: its own pixels (where the port has sky)
py0, px0 = max(0, ty - r) - ya, (tx - r) - xa   # (the patch within the glow's area)
near = np.clip((56 - dd) / 6.0, 0, 1)[..., None] * hj_sky[py0:py0 + dd.shape[0], px0:px0 + dd.shape[1]]
dst[:] = dst * (1 - near) + p * near
# HenryJk's sky has faint horizontal stripes along its rows (in his night roof too); relit red they showed as
# ghost lines across the blood moon's sky (user, 2026-11-20). The sky is smoothed up and down (9 rows), not the
# stars (brighter than the sky round them), the moon and its rim, or anything that isn't sky (walls, chimney, lines).
lw = wide @ np.array([0.3, 0.59, 0.11], np.float32)
hR, hB = wide[..., 0], wide[..., 2]
yy2, xx2 = np.mgrid[0:wide.shape[0], 0:wide.shape[1]]
moon_d = np.sqrt((xx2 - (mx + SHIFT[0] + PAD)) ** 2 + (yy2 - (my + SHIFT[1])) ** 2)
loc = lw.copy()
for ax in (0, 1):
    loc = np.apply_along_axis(lambda v: np.convolve(np.pad(v, 3, mode='edge'), np.ones(7) / 7, 'valid'), ax, loc)
star = lw - loc > 10
for ax in (0, 1):   # (and a pixel round each)
    star = np.apply_along_axis(lambda v: np.convolve(v.astype(np.float32), np.ones(3), 'same') > 0, ax, star)
sm = ((hR < 60) & (hB - hR > 45) & ~star & (moon_d > 44)).astype(np.float32)[..., None]
num, den = out * sm, sm.copy()
k9 = np.ones(9, np.float32)
num = np.apply_along_axis(lambda v: np.convolve(v, k9, 'same'), 0, num)
den = np.apply_along_axis(lambda v: np.convolve(v, k9, 'same'), 0, den)
out = out * (1 - sm) + (num / np.maximum(den, 1e-3)) * sm
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(sys.argv[1])
print(sys.argv[1], out.shape[1], 'x', out.shape[0])
