# make_frost_bushes.py OUT_DIR : HenryJk's day bushes (BD0..4, B_base) whitened for the DLC's snow map, in the
# manner of a snow-covered tree: each pixel's brightness kept as shading, from a soft lavender blue in the
# shadows to near white on the lit leaves. Alpha unchanged.
import sys, numpy as np
from PIL import Image
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

H = ROOT + 'decomps/henryjk/PvZWidescreen/resources/images/'
SHADOW, LIGHT = np.array([104, 112, 182], np.float32), np.array([220, 224, 250], np.float32)
for src, dst in [('BD%d' % i, 'hj_BS%d' % i) for i in range(5)] + [('B_base', 'hj_BS_base')]:
    a = np.asarray(Image.open(H + src + '.png').convert('RGBA'), np.float32)
    lum = (a[..., :3] @ np.array([0.3, 0.59, 0.11], np.float32)) / 255
    t = np.clip((lum - 0.08) / 0.55, 0, 1)  # the leaves' range stretched over shadow..light
    a[..., :3] = SHADOW + (LIGHT - SHADOW) * t[..., None]
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGBA').save(sys.argv[1] + '/' + dst + '.png')
print('ok')
