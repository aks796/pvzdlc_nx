# make_wide_credits.py OUT_WIDE.jpg OUT_43.jpg : the DLC credits page's background (creditsbackground.jpg, 800x7568)
# at 1066 wide, and both it and the 800-wide original 224 rows taller (INS_H: two of the strata's cycles, so the band
# repeats seamlessly on every column): the band, a copy of the 224 rows above it, goes in at row INS_Y, between the
# playtesters and the special thanks, where the port's own section is drawn (pad_credits_port; everything below
# moves down as much).
# The dirt shaft (below the grass) has a side wall at each edge whose strata run down and outwards; each wall is
# carried on outwards in that same direction (nothing mirrored): the extra 133 columns repeat a stretch of the
# wall's own columns, moved down along the strata (measured: one 112-row cycle, 0.44 rows lower per column
# outwards on the left, 0.53 on the right), skipping rows that hold the bottom's copyright lines. The wall's
# stones are left out of the carried-on part (their pixels come from a neighbouring column of the wall, along
# the strata), so it doesn't fill with a grid of repeated stones. The picture at the top (plants and zombies) is
# zoomed 4/3 instead, its grass line kept where it was, so nobody is doubled. The two meet in plain dirt,
# crossfaded over 30 rows.
import sys, numpy as np
from PIL import Image, ImageFilter
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

src = Image.open(ROOT + 'analysis/main_pak/images/creditsbackground.jpg').convert('RGB')
W0, H = src.size
X0, W = 133, 1066
a = np.asarray(src, np.float32)
T = 111.9                       # the strata's vertical cycle
CLEAN0 = 600                    # rows above this are the top picture
TEXT0 = H - 90                  # the copyright lines (left), from here down
rock = (a.max(2) - a.min(2) < 23) | (a.mean(2) < 27)
rock = np.asarray(Image.fromarray((rock * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(13))) > 0

def wall(xa, xb, g, cols, left):
    # g(x): how many rows lower the strata are at column x; the wall's columns xa..xb-1 repeat outwards
    out = np.zeros((H, len(cols), 3), np.float32)
    P = xb - xa
    ys = np.arange(H, dtype=np.float64)
    for i, x in enumerate(cols):
        xs = xa + (x - xa) % P
        y = ys + g(xs) - g(x)
        y = np.where(y < CLEAN0, y + T * np.ceil((CLEAN0 - y) / T), y)
        if left: y = np.where(y >= TEXT0 - 1, y - T * np.ceil((y - TEXT0 + 2) / T), y)
        y = np.where(y > H - 2, y - T * np.ceil((y - H + 2) / T), y)
        # off the stones: the first stone-free column near xs (same strata), else a cycle up or down
        px = np.full(H, -1); py = y.copy()
        for dx in (0, 9, -9, 18, -18, 27, -27, 36, -36):
            x2 = xs + dx
            if not xa <= x2 < xb: continue
            for k in (0, -1, 1):
                yk = y + g(x2) - g(xs) + k * T
                ok = (yk >= CLEAN0) & (yk < H - 2) & ~(left & (yk >= TEXT0 - 1))
                yi = np.clip(np.floor(yk).astype(int), 0, H - 2)
                free = ok & ~rock[yi, x2] & ~rock[yi + 1, x2] & (px < 0)
                px = np.where(free, x2, px); py = np.where(free, yk, py)
        px = np.where(px < 0, xs, px)
        y0 = np.clip(np.floor(py).astype(int), 0, H - 2); f = (py - y0)[:, None]
        out[:, i] = a[y0, px] * (1 - f) + a[y0 + 1, px] * f
    return out

left = wall(4, 76, lambda x: -0.44 * x, np.arange(-X0, 0), True)
right = wall(740, 796, lambda x: 0.53 * x, np.arange(W0, W0 + X0), False)
side = np.concatenate([left, a, right], 1)
B, F = 500, 30                     # the zoomed top ends at row B, fading over the F rows before it
k = W / W0
top = np.asarray(src.crop((0, 0, W0, B + 4)).resize((W, int((B + 4) * k)), Image.LANCZOS), np.float32)
shift = int(round(k * B - B))
out = side.copy()
for y in range(B):
    ty = y + shift
    if ty >= top.shape[0]: break
    t = 1.0 if y < B - F else (B - y) / F
    out[y] = top[ty] * t + side[y] * (1 - t)
INS_Y, INS_H = 5740, 224
def taller(img):
    return np.concatenate([img[:INS_Y], img[INS_Y - INS_H:INS_Y], img[INS_Y:]], 0)
wide = taller(np.clip(out, 0, 255).astype(np.uint8))
Image.fromarray(wide).save(sys.argv[1], quality=92)
four3 = taller(np.asarray(src))
Image.fromarray(four3).save(sys.argv[2], quality=92)
print(wide.shape, four3.shape)
