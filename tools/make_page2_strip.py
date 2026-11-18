# make_page2_strip.py PAGE2_BG OUT : page 2's right strip (133x600).
# - Sky, clouds and the dead tree: the page's own right edge mirrored.
# - The wall: its last stretch (x 690..799, rising towards us: cap top 286 -> 242, grass line 406 -> 438, so 1.63 times taller at 799)
#   carried on as the next stretch of the same wall, 1.63 times larger along the same line, so it keeps
#   coming towards the viewer to the screen's edge.
# - Below the wall: the grass carried on in its own direction, the mail pot (in those columns) covered by the
#   page's own grass from x 75.
import sys, numpy as np
from PIL import Image, ImageFilter
p2 = np.asarray(Image.open(sys.argv[1]).convert('RGB'), np.float32)
W, H = 133, 600
# above the wall the page's edge mirrored (the dead tree turns back on itself); below it the grass carries on
# in its own direction (the page's last 133 columns again, not mirrored)
mir = p2[:, 800 - W:800][:, ::-1]
cont = p2[:, 800 - W:800]
k = np.clip((np.arange(H if 'H' in dir() else 600) - 330) / 40, 0, 1)[:, None, None]
strip = mir * (1 - k) + cont * k
# the pot, covered where its own pixels are (brown and the white letter), with the grass from x 75
R, G, B = strip[..., 0], strip[..., 1], strip[..., 2]
pot = (R > G + 12) | ((R > 165) & (G > 165) & (B > 165))
rows = np.zeros_like(pot); rows[405:568, :] = True
m = Image.fromarray(((pot & rows) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(19)).filter(ImageFilter.GaussianBlur(4))
a = np.asarray(m, np.float32)[..., None] / 255
strip = strip * (1 - a) + p2[:, 75:75 + W] * a
# the wall's next stretch
X0, X1, TOP0, TOP1, F = 690.0, 799.0, 286.0, 242.0, 1.63
slope = (TOP1 - TOP0) / (X1 - X0)
def bilinear(img, x, y):
    x0 = np.clip(np.floor(x).astype(int), 0, img.shape[1] - 2); y0 = np.clip(np.floor(y).astype(int), 0, img.shape[0] - 2)
    fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy) + img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)
xs_t, ys_t = np.meshgrid(np.arange(W, dtype=np.float32), np.arange(H, dtype=np.float32))
xs = X0 + xs_t / F                                  # source column of the stretch before
top_t = TOP1 + slope * xs_t                          # the cap's top line, carried on
top_s = TOP0 + slope * (xs - X0)
ys = top_s + (ys_t - top_t) / F                       # same place below the cap, at the stretch's scale
slab = bilinear(p2, xs, ys)
# use it from just above the cap down to where the source reaches the pot's rows; fade at both ends
wa = np.clip((ys_t - (top_t - 4)) / 3, 0, 1) * np.clip((421 - ys) / 4, 0, 1)
wa = wa[..., None]
out = strip * (1 - wa) + slab * wa
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(sys.argv[2])
