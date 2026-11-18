# make_page1_mound.py OUT.png : the DLC's hill piece (SelectorScreen_BG_Right, 730x560, its own alpha) with page 1's part of the dirt mound beside the tombstone (the DLC's hill piece,
# SelectorScreen_BG_Right, its mound against its right edge) in page 2's colour, so the mound is one piece
# across the pages in page 2's look: page 2's own mound pixels carried on across the seam (its streaks and lit
# top). Only the mound's pixels (a fill from the seam through dirt-coloured pixels: not the MORE post, the
# vase's flowers or the outlines), blended in by a soft alpha a pixel inside its edge.
import sys, numpy as np
from collections import deque
from PIL import Image, ImageFilter
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

O = ROOT + 'analysis/main_pak/'
p2 = np.asarray(Image.open(O + 'images/SelectorScreen_BackgroundPage2.jpg').convert('RGB'), np.float32)
hr = np.asarray(Image.open(O + 'reanim/SelectorScreen_BG_Right.jpg').convert('RGB'), np.float32)
hm = np.asarray(Image.open(O + 'reanim/SelectorScreen_BG_Right_.png').convert('L'), np.float32)
H, W = hm.shape
Y0 = 41                                   # the piece's top on the screen (page 2's row = piece row + 41)
def is_dirt(im):
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    return (r > b + 20) & (g > b + 10) & (r - g < 48) & (g < r * 1.25) & (im.mean(2) < 95) & (im.mean(2) > 22)
dirt = is_dirt(hr) & (hm > 200)
mound = np.zeros_like(dirt); q = deque()
for y in range(H):
    if dirt[y, W - 1]: mound[y, W - 1] = True; q.append((y, W - 1))
while q:
    y, x = q.popleft()
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        a, c = y + dy, x + dx
        if 0 <= a < H and 0 <= c < W and dirt[a, c] and not mound[a, c]: mound[a, c] = True; q.append((a, c))
d2 = is_dirt(p2)
from seam_shadow import lift
p2 = lift(p2, d2)  # (page 2's seam shadow lifted, as in its own strip)
# page 2's own mound, carried on across the seam (its streaks and lit top, meeting exactly at the seam): each
# row of page 1's mound takes the same row of page 2's going the other way from the seam, s columns left taking
# s columns right; where page 2's row of dirt is narrower than page 1's (near the top) it is stretched across
ys_, xs_ = np.nonzero(mound)
x0, x1, y0, y1 = xs_.min() - 2, W, ys_.min() - 2, ys_.max() + 3
sub = (slice(y0, y1), slice(x0, x1))
Ms = mound[sub].astype(np.float32)
rgb = hr[sub].copy()
def run(v):  # the length of the dirt run from index 0 (gaps of up to 3 px bridged)
    n = gap = 0
    for i, t in enumerate(v):
        if t: n, gap = i + 1, 0
        else:
            gap += 1
            if gap > 3: break
    return n
for r in range(y0, y1):
    y = r + Y0
    if y >= 600: break
    w1 = run(mound[r, ::-1]); w2 = run(d2[y, :110])
    if w1 == 0: continue
    if w2 < 4:  # (above page 2's mound: its top row)
        for yy in range(y + 1, min(600, y + 40)):
            w2 = run(d2[yy, :110])
            if w2 >= 4: y = yy; break
        else: continue
    k = min(1.0, (w2 - 1) / max(w1 - 1, 1))
    xs2 = np.arange(110)
    for x in range(x0, x1):
        if not mound[r, x]: continue
        f = (W - 1 - x) * k
        i = int(f); t = f - i
        rgb[r - y0, x - x0] = p2[y, i] * (1 - t) + p2[y, min(i + 1, 109)] * t
soft = np.asarray(Image.fromarray((Ms * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)), np.float32) / 255
alpha = (Ms * np.clip(soft * 1.6, 0, 1))[..., None]
out = hr.copy()
out[sub] = out[sub] * (1 - alpha) + rgb * alpha
# where the first wall brick's lower mortar line meets page 2 (the piece's last two columns, rows 241..243 = screen
# 282..284) it was a near-black dot two rows thick (the user's report): one row of the line's own darkness, the
# band's colour above it and the wall's below, as the line runs on either side
lw = np.array([0.3, 0.59, 0.11], np.float32)
for c in (W - 2, W - 1):
    band, wall, line = out[239, c].copy(), out[245, c].copy(), out[241, W - 4].copy()
    out[241, c] = band
    out[242, c] = line * (25.0 / max(1.0, float(line @ lw))) if line @ lw > 25 else line
    out[243, c] = wall * 0.65 + out[242, c] * 0.35
# the tombstone's top-right outline against the sky (piece rows 40..140, columns 680..): the DLC's mask cut through
# it, so the dark outline showed half see-through (the user: "missing its outline"). The outline's dark pixels
# (the picture's own, L < 30) next to the stone are made solid, one pixel out, as HenryJk's piece has them.
hmo = hm.astype(np.float32).copy()
lum = hr @ lw
for r in range(40, 141):
    for c in range(680, W):
        if hmo[r, c] < 250 and lum[r, c] < 30 and (hm[r, c - 1] >= 250 or hm[r - 1, c] >= 250 or hm[r + 1, c] >= 250):
            hmo[r, c] = 255
hm = hmo
Image.fromarray(np.dstack([np.clip(out, 0, 255), hm]).astype(np.uint8), 'RGBA').save(sys.argv[1])
print('mound', x0, y0, x1 - x0, y1 - y0, 'px', mound.sum())
