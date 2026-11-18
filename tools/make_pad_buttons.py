#!/usr/bin/env python3
"""make_pad_buttons.py OUT_DIR: Multiplayer's P1 / P2 device buttons with a controller (the Switch's players are
controllers). From the game's own player1_mouse / player2_mouse (69x41; the keyboard pictures' keyboard overlaps the
lettering's outline, the mouse's doesn't): the mouse is cleared to the button's own body colour (flat per row) and port_art/pad_icon.png (the same white, dark-outlined style) is put
in its place. Variants: _off = that controller not connected (faded), player2_cpu = the AI plays player 2
(the whole button dimmed and greyed, no controller)."""
import sys
import numpy as np
from PIL import Image

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

IMG = ROOT + 'analysis/main_pak/images/'
PAD = ROOT + 'port_art/pad_icon.png'
out = sys.argv[1]
pad = Image.open(PAD).convert('RGBA')


for p in (1, 2):
    a = np.asarray(Image.open(IMG + f'player{p}_mouse.png').convert('RGBA')).copy()
    h, w = a.shape[:2]
    # the mouse's columns: where any row differs from that row's body colour, right of the "P1" text
    body = a[:, 4:5].astype(int)  # (the body left of the lettering: flat per row)
    diff = (np.abs(a[3:36].astype(int) - body[3:36]).sum(axis=2) > 30)
    # (not the button's own right edge, darker on every row)
    cols = [x for x in range(36, w - 3) if diff[:, x].any() and not diff[1, x]]
    x0, x1 = min(cols), max(cols) + 1
    rows = [y for y in range(3, 36) if diff[y - 3, x0:x1].any()]
    y0, y1 = min(rows), max(rows) + 1
    print(p, 'mouse at', x0, y0, x1, y1)
    base = a.copy()
    for y in range(3, 36):
        base[y, x0:x1] = a[y, 4]
    # the controller where the keyboard sat (x 35..63), clear of the lettering
    cx0, cx1 = 35, 63
    icon = pad.resize((cx1 - cx0, round((cx1 - cx0) * pad.size[1] / pad.size[0])), Image.LANCZOS)
    im = Image.fromarray(base)
    oy = 21 - icon.size[1] // 2
    on = im.copy()
    on.alpha_composite(icon, (cx0, oy))
    on.save(f'{out}/player{p}_pad.png')
    faded = icon.copy()
    faded.putalpha(Image.fromarray((np.asarray(icon)[..., 3] * 0.35).astype(np.uint8)))
    off = im.copy()
    off.alpha_composite(faded, (cx0, oy))
    off.save(f'{out}/player{p}_pad_off.png')
    if p == 2:
        g = np.asarray(im).astype(np.float32)
        lum = g[..., :3] @ np.array([0.3, 0.59, 0.11], np.float32)
        g[..., :3] = (0.55 * g[..., :3] + 0.45 * lum[..., None]) * 0.62
        Image.fromarray(g.clip(0, 255).astype(np.uint8)).save(f'{out}/player2_cpu.png')
