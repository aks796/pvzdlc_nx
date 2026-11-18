# make_wide_backgrounds.py OUT_DIR : 16:9 (1066x600) versions of the DLC's 800x600 screen backgrounds. The
# DLC's own picture stays in the middle; the 133 px on each side come from the widescreen mod's 1280x720
# version of the scene (QotL Debug/images), placed where the DLC's picture fits inside it, and the DLC's edges
# fade into it over FEATHER px.
import sys, numpy as np
from PIL import Image
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

O = ROOT + 'analysis/main_pak/images/'
M = ROOT + 'decomps/PvZ-QotL-Widescreen-main/Debug/images/'
# name: (DLC file, mod file, scale, x, y[, feather]) -- the DLC picture's place in the mod picture scaled by
# `scale`; 'mirror' for the mod file uses the picture's own edges mirrored; feather 'whole' takes the mod's
# picture alone
FITS = {
    'bg_greenhouse':      ('Background_Greenhouse.jpg', 'Background_Greenhouse.png', 1.0, 240, 60, 'whole'),
    'bg_mushroom':        ('Background_MushroomGarden.jpg', 'Background_MushroomGarden.png', 1.0, 240, 60, 'whole'),
    'bg_store_night':     ('Store_BackgroundNight.jpg', 'Store_BackgroundNight.png', 1.0, 240, 60, 'whole'),
    # where the mod has the scene in widescreen it is used whole, at its own framing (the shop keeps its
    # sidewalk and the house behind the car; the DLC's car, Dave and the gardens' plants stand on it as
    # they do on the DLC's pictures)
    'bg_store':           ('Store_Background.jpg', 'Store_Background.png', 1.0, 240, 60, 'whole'),
    # the mod's aquarium is a re-composed tank: its whole picture, fitted to the screen
    'bg_aquarium':        ('aquarium1.jpg', 'aquarium1.png', 1066 / 1280, 133, 0, 'whole'),
}
W, X0, FEATHER = 1066, 133, int(sys.argv[2]) if len(sys.argv) > 2 else 24
names = sys.argv[3:] or list(FITS)
for name in names:
    dlc, mod, s, ox, oy = FITS[name][:5]
    feather = FITS[name][5] if len(FITS[name]) > 5 else FEATHER
    d = Image.open(O + dlc).convert('RGB')
    out = Image.new('RGB', (W, 600))
    if mod != 'mirror':
        m = Image.open(M + mod).convert('RGB')
        m = m.resize((round(m.size[0] * s), round(m.size[1] * s)), Image.LANCZOS)
        out.paste(m, (X0 - ox, -oy))
    if mod == 'mirror':
        # no matching scene in the mod: the picture's own edges, mirrored (sky, fence and kerb carry on)
        dd = np.asarray(d)
        out.paste(Image.fromarray(dd[:, :X0][:, ::-1].copy()), (0, 0))
        out.paste(Image.fromarray(dd[:, 800 - (W - X0 - 800):][:, ::-1].copy()), (X0 + 800, 0))
    if feather == 'whole':
        out.save(sys.argv[1] + '/' + name + '.png')
        print(name)
        continue
    a = np.ones((600, 800), np.float32)
    ramp = np.clip((np.arange(800) + 0.5) / feather, 0, 1) * np.clip((800 - np.arange(800) - 0.5) / feather, 0, 1)
    a *= ramp[None, :]
    mid = np.asarray(out.crop((X0, 0, X0 + 800, 600)), np.float32)
    blend = mid * (1 - a[..., None]) + np.asarray(d, np.float32) * a[..., None]
    out.paste(Image.fromarray(blend.astype(np.uint8)), (X0, 0))
    out.save(sys.argv[1] + '/' + name + '.png')
    print(name)
