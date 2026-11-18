#!/usr/bin/env python3
"""bake_sign_lit.py DUMP_DIR ART_DIR: the main-menu signs' lit pictures, made ahead.

The Mac host with RT_SIGN_BAKE=1 makes each focused sign's lit picture (pad_input.c lettering_image) and writes
it as shots/signlit_WxH_K.argb (B G R A bytes; W x H the sign's picture, the lit one 1 px larger each side;
K 1 the big signs, 0 the small). This writes them as ART_DIR/signlit_WxH_K.png, which tools/embed_art.py builds
into the NRO; the game then loads them instead of making them (about a second each on the Switch)."""
import glob, os, re, sys
import numpy as np
from PIL import Image

dump, art = sys.argv[1], sys.argv[2]
for path in sorted(glob.glob(os.path.join(dump, "signlit_*.argb"))):
    m = re.match(r"signlit_(\d+)x(\d+)_(\d)\.argb$", os.path.basename(path))
    w, h = int(m.group(1)) + 2, int(m.group(2)) + 2
    a = np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)
    out = os.path.join(art, os.path.basename(path)[:-5] + ".png")
    Image.fromarray(np.ascontiguousarray(a[:, :, [2, 1, 0, 3]])).save(out, optimize=True)
    print(out, w, h)
