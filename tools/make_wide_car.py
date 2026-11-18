# make_wide_car.py OUT_DIR : Crazy Dave's car whole. The DLC's car pictures stop at the 4:3 screen's edge; the
# widescreen mod's are the whole car (at 0.86 the DLC's is its part at 2,1). The DLC's own car stays on top
# and fades into the mod's over FEATHER px before its cut edges.
import sys, numpy as np
from PIL import Image
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

O = ROOT + 'analysis/main_pak/images/'
M = ROOT + 'decomps/PvZ-QotL-Widescreen-main/Debug/images/'
S, OX, OY, FEATHER = 0.86, 2, 1, 40
for name, rgb, mask, mod in (('store_car', 'Store_Car.jpg', 'Store_Car_.png', 'Store_Car.png'),
                             ('store_car_closed', 'Store_CarClosed.jpg', 'Store_CarClosed_.png', 'Store_CarClosed.png')):
    d = np.asarray(Image.open(O + rgb).convert('RGB'), np.float32)
    a = np.asarray(Image.open(O + mask).convert('L'), np.float32) / 255
    h, w = a.shape
    # fade out towards the right and bottom edges, where the DLC's picture is cut
    a = a * np.clip((w - 1 - np.arange(w)) / FEATHER, 0, 1)[None, :] * np.clip((h - 1 - np.arange(h)) / FEATHER, 0, 1)[:, None]
    m = Image.open(M + mod).convert('RGBA')
    m = np.asarray(m.resize((round(m.size[0] * S), round(m.size[1] * S)), Image.LANCZOS), np.float32)
    out = m.copy()
    reg = out[OY:OY + h, OX:OX + w]
    reg[..., :3] = reg[..., :3] * (1 - a[..., None]) + d * a[..., None]
    reg[..., 3] = np.maximum(reg[..., 3], a * 255)
    Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), 'RGBA').save(sys.argv[1] + '/' + name + '.png')
    print(name, out.shape)
