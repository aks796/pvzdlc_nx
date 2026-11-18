# make_page2_day.py OUT [MOUND_OUT [BRICK_OUT]] : page 2's left 160 px (the mound, wall and grass that continue page 1's
# tombstone hill) in page 1's day light: each row coloured to match the DLC's own hill piece (SelectorScreen_BG_Right,
# at window 71,41, its last column at page 2's x 0) where they meet, so the mound is one piece across the pages.
# By material: the tombstone's last sliver is left as it is (page 2's stone is page 1's, pixel for pixel), the
# dirt mound keeps page 2's own colours (page 1's part is recoloured to match: make_page1_mound.py), the rest
# takes the row's gain. Alpha: none
# in the sky (page 1's sky is drawn there; page 2's painted cloud stays). MOUND_OUT: the mound alone (drawn whole in either view).
import sys, numpy as np
from PIL import Image
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

O = ROOT + 'analysis/main_pak/'
p2 = np.asarray(Image.open(O + 'images/SelectorScreen_BackgroundPage2.jpg').convert('RGB'), np.float32).copy()
# a thick dark dot on the first brick's lower mortar line (columns 8..10, rows 284..290, the user's report): each
# row there taken across from its two sides, so the line runs through evenly
for y in range(284, 291):
    l, r = p2[y, 6:8].mean(0), p2[y, 11:13].mean(0)
    for x in range(8, 11):
        t = (x - 7) / 4.0
        p2[y, x] = l * (1 - t) + r * t
hr = np.asarray(Image.open(O + 'reanim/SelectorScreen_BG_Right.jpg').convert('RGB'), np.float32)
hm = np.asarray(Image.open(O + 'reanim/SelectorScreen_BG_Right_.png').convert('L'), np.float32)
W, X0, Y0 = 160, 71, 41
def smooth(v, k):
    w = np.ones(k) / k
    return np.stack([np.convolve(np.pad(v[:, c], k // 2, mode='edge'), w, 'valid') for c in range(3)], 1)
gain = np.ones((600, 3), np.float32)
ok = np.zeros(600, bool)
for y in range(Y0, min(600, Y0 + hr.shape[0])):
    r = y - Y0
    # only the dirt and grass beside the tombstone (coloured, not its grey stone)
    cols = [c for c in range(717, 729) if hm[r, c] > 200 and hr[r, c].max() - hr[r, c].min() > 25 and hr[r, c, 0] < hr[r, c, 1] + 60]
    if len(cols) < 6: continue
    a = hr[r, cols].mean(0); b = p2[y, 0:12].mean(0)
    gain[y] = (a + 4) / (b + 4); ok[y] = True
ys = np.nonzero(ok)[0]
# rows with no dirt or grass beside them (the wall, beside the tombstone): HenryJk's day-lit piece shows this same
# stretch of wall (its columns 730.. are page 2's 0..), so the gain is its wall against page 2's
hj = np.asarray(Image.open(ROOT + 'decomps/henryjk/PvZWidescreen/resources/reanim/'
                           'SelectorScreen_BG_Right.png').convert('RGBA'), np.float32)
med = np.median(gain[ok], 0)
wall = np.zeros(600, bool)
for y in range(600):
    if ok[y]: continue
    r = y - Y0
    if 0 <= r < hj.shape[0]:
        m = hj[r, 735:860, 3] > 200
        if m.sum() > 40:
            gain[y] = (hj[r, 735:860, :3][m].mean(0) + 4) / (p2[y, 5:130][m].mean(0) + 4)
            wall[y] = True
            continue
    gain[y] = med
# the wall is one stone: one gain for all its rows (the median of HenryJk's), or its blocks band where the
# per-row gains differ
if wall.any(): gain[wall] = np.median(gain[wall], 0)
L = p2[:, :W].mean((1, 2)); B = p2[:, :W, 2].mean(1)
top = int(np.argmax((B < 150) & (np.arange(600) > 150)))  # the wall's top: the first non-sky row
gain = np.clip(smooth(gain, 41), 0.6, 1.8)
# at the seam itself each row is matched to the hill's very last columns (a short vertical average), easing
# into the broad gain over the first 48 columns, so the two pictures meet without a line
edge = np.ones((600, 3), np.float32); okE = np.zeros(600, bool)
for y in range(Y0, min(600, Y0 + hr.shape[0])):
    r = y - Y0
    cols = [c for c in range(724, 730) if hm[r, c] > 200 and hr[r, c, 0] < hr[r, c, 1] + 60 and hr[r, c].mean() > 70]
    if len(cols) < 3: continue
    edge[y] = (hr[r, cols].mean(0) + 4) / (p2[y, 0:4].mean(0) + 4); okE[y] = True
edge[~okE] = gain[~okE]
edge[wall] = gain[wall]  # (the wall's rows beside the tombstone: its grey edge isn't a colour to match)
edge = np.clip(smooth(edge, 9), 0.5, 2.0)
w = np.clip(np.arange(W) / 48.0, 0, 1)[None, :, None]
g2 = edge[:, None, :] * (1 - w) + gain[:, None, :] * w
src = p2[:, :W]
Y = np.arange(600)[:, None]; X = np.arange(W)[None, :]
# the tombstone's sliver: from x 0 along each row until the sky or the (lighter) wall, and its outline
stone = np.zeros((600, W), bool)
for y in range(100, 300):
    x = 0
    while x < 40 and not ((src[y, x, 2] > src[y, x, 0] + 60 and src[y, x, 2] > 120) or src[y, x].mean() > 95): x += 1
    if x: stone[y, :min(W, x + 2)] = True
# the dirt mound: brown or olive (its lit top), left of the grass
def is_dirt(im):
    return (im[..., 0] > im[..., 2] + 20) & (im[..., 1] < im[..., 0] * 1.25) & (im.mean(2) < 120) & (im.mean(2) > 25)
dirt = is_dirt(src) & (X < 110) & (Y > 395) & (Y < 570)
hdirt = is_dirt(hr) & (hm > 200)
lum = lambda im: im[..., 0] * 0.3 + im[..., 1] * 0.59 + im[..., 2] * 0.11
# the mound: page 2's own colours (page 1's part is recoloured to them instead: make_page1_mound.py), its seam
# shadow lifted (seam_shadow.py)
from seam_shadow import lift
dcol = lift(p2, is_dirt(p2) & (np.arange(600)[:, None] > 395) & (np.arange(800)[None, :] < 110))[:, :W]
def feather(m):
    return np.asarray(Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
                      .filter(ImageFilter.GaussianBlur(1)), np.float32)[..., None] / 255
from PIL import ImageFilter
fs, fd = feather(stone), feather(dirt)
g = g2 * (1 - fs) + 1.0 * fs
out = np.clip(src * g * (1 - fd) + dcol * fd, 0, 255)
print('stone px', stone.sum(), 'dirt px', dirt.sum())
# none of page 2's own (night) sky: cut along the wall's outline, softened by a pixel
sky = (src[..., 2] > src[..., 0] + 60) & (src[..., 2] > 120) & (np.arange(600)[:, None] < 330)
alpha = 1 - np.asarray(Image.fromarray((sky * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
                       .filter(ImageFilter.GaussianBlur(1)), np.float32) / 255
# page 2's painted cloud (above the tombstone's top, out of the stone): its own colours, unmixed from page 2's
# night sky so its soft edge and thin tail blend into page 1's sky instead of being cut out with a hard edge:
# alpha from how far the red channel is above the sky's (sky ~5, the cloud's underside ~120, its top ~220)
cloud_rows = np.arange(600)[:, None] < 140
skyc = np.zeros((600, 3), np.float32)
for y in range(140):
    m = sky[y]
    skyc[y] = src[y, m].mean(0) if m.sum() >= 4 else (skyc[y - 1] if y else np.array([5, 88, 191], np.float32))
ca = np.clip((src[..., 0] - skyc[:, None, 0]) / 90.0, 0, 1)
cloud = cloud_rows & ~stone & (ca > 0)
crgb = np.clip((src - (1 - ca[..., None]) * skyc[:, None, :]) / np.maximum(ca[..., None], 1e-3), 0, 255)
out = np.where(cloud[..., None], crgb, out)
alpha = np.where(cloud_rows & ~stone, ca, alpha)
# the first wall brick beside the tombstone (page 2's columns 0..28, up to its post at 29): its mortar lines sat a
# pixel or so above page 1's where they meet the stone. The brick is moved down by SH at the stone, easing to
# nothing at the post (a vertical resample per column, rows 240..305, softened in and out over 8 rows), so its
# lines meet the stone's and stay whole along the brick. (Blending in HenryJk's piece instead left a step where
# the two pictures' lines crossed, mid-brick.)
# the tombstone's last sliver and its outline: HenryJk's (his hill piece is page 1's widened to the right, pixel for
# pixel: his column 730 + x, row y - 40 is page 2's x, y; correlation 0.999). Page 2's own drawing of it has a thin,
# soft line where page 1's and HenryJk's is thick and black, with a green rim inside (the user's report). Taken over
# the stone (page 2's mask, rows 100..231, to the wall's first brick) widened 5 px towards the sky, with
# HenryJk's alpha (his outline's soft edge against page 1's sky); inside the stone the two agree, so it fades in
# over the mask's inner 3 px without a seam.
HX, HY, WALL = 730, 40, 232
reg = np.zeros((600, W), bool)
for y in range(100, WALL):
    xs = np.nonzero(stone[y])[0]
    if len(xs): reg[y, :min(40, xs.max() + 6)] = True
hjr = np.zeros((600, W, 4), np.float32)
hw = hj.shape[1] - HX  # (his 133 extra columns)
for y in range(600):
    r = y - HY
    if 0 <= r < hj.shape[0]: hjr[y, :hw] = hj[r, HX:]
k = np.asarray(Image.fromarray((reg * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(3))
               .filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255 * reg
# towards the sky (past page 2's stone) HenryJk's pixels outright: there's nothing of page 2's to blend with there
k = np.where(reg & ~stone, 1.0, k)
out = out * (1 - k[..., None]) + hjr[..., :3] * k[..., None]
alpha = alpha * (1 - k) + (hjr[..., 3] / 255) * k
print('stone outline from HenryJk:', int(reg.sum()), 'px')
SH, POST = 1.5, 29
o0, a0 = out.copy(), alpha.copy()
for x in range(POST):
    d = SH * (1 - x / POST)
    for y in range(232, 313):
        k = min(1.0, (y - 232) / 8.0, (312 - y) / 8.0)
        yy = y - d * k
        y0 = int(np.floor(yy)); f = yy - y0
        out[y, x] = o0[y0, x] * (1 - f) + o0[y0 + 1, x] * f
        alpha[y, x] = a0[y0, x] * (1 - f) + a0[y0 + 1, x] * f
rgba = np.dstack([out, alpha * 255]).astype(np.uint8)
Image.fromarray(rgba, 'RGBA').save(sys.argv[1])
# BRICK_OUT: the same brick, moved alike, in page 2's own (night) colours, drawn over page 2 in either view
if len(sys.argv) > 3:
    nb = np.zeros((600, POST + 4, 4), np.float32)
    for x in range(POST + 4):
        d = SH * (1 - x / POST) if x < POST else 0
        for y in range(600):
            k = min(1.0, max(0.0, (y - 232) / 8.0), max(0.0, (312 - y) / 8.0)) if 232 <= y <= 312 else 0
            yy = y - d * k
            y0 = int(np.floor(yy)); f = yy - y0
            nb[y, x, :3] = p2[y0, x] * (1 - f) + p2[min(599, y0 + 1), x] * f
    # only where it moved (rows 232..312), the rest see-through; out of the sky
    nsky = (p2[..., 2] > p2[..., 0] + 60) & (p2[..., 2] > 120)
    for x in range(POST + 4):
        for y in range(232, 313):
            nb[y, x, 3] = 0 if nsky[y, x] else 255
    Image.fromarray(nb.astype(np.uint8), 'RGBA').save(sys.argv[3])
if len(sys.argv) > 2:
    md = feather(dirt)[..., 0] * alpha
    Image.fromarray(np.dstack([out, md * 255]).astype(np.uint8), 'RGBA').save(sys.argv[2])
print('wall top row', top, 'gain range', gain[top:].min(0).round(2), gain[top:].max(0).round(2))
