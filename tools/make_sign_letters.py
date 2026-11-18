#!/usr/bin/env python3
"""make_sign_letters.py DUMP_DIR ART_DIR [REVIEW_DIR]: the letters of the five main-menu signs, picked one by one,
as masks for their "selected" look (pad_input.c lettering_image lights only these pixels).

Input: the signs' own pictures, dumped by the Mac host (RT_SIGN_DUMP=1 writes shots/sign_WxH.argb, B G R A bytes).
Output: ART_DIR/sign_WxH.png (R = letter, G = the LEVEL plaque left see-through), embedded by tools/embed_art.py.

Each letter is found the same way: the sign's dark ink (brightness < 60) is shrunk by R px, which cuts the thin
cracks and the stone's outline off the letters and leaves one core per letter (a few letters in two). The cores
named below (picked by hand from numbered pictures, letter by letter) are grown back inside the ink by R + 2 px,
then 1 px more for the anti-aliased edge. A crack too thin for the shrink ends where it meets the letter; a crack
as thick as a letter is its own core and isn't named. Per-letter fixes (ERASE boxes) are listed with the letters.
REVIEW_DIR gets each letter cut out at 8x with its mask beside it."""
import os, sys
from collections import deque
import numpy as np
from PIL import Image

INK = 60

# sign size -> (shrink R, letters as {name: [core ids]}, plaque core ids, erase boxes (x0, y0, x1, y1) inclusive)
SIGNS = {
    "329x143": (3, {"A": [2, 12], "D": [3], "V": [4], "E": [5], "N": [6], "T": [8], "U": [9], "R": [10], "E2": [11]}, [1], []),
    "313x133": (2, {"m": [1, 4], "i": [2], "n": [3], "i2": [5], "-": [8], "G": [6], "A": [7], "M": [9], "E": [11],
                    "S": [13, 14, 20]}, [], []),
    "286x122": (2, {"P": [1, 2], "U": [3], "Z": [4], "Z2": [5], "L": [6], "E": [7]}, [], []),
    "266x123": (2, {"S": [1], "U": [2], "R": [3], "V": [4], "I": [5], "V2": [6], "A": [7], "L": [8]}, [], []),
    "322x142": (2, {"C": [14], "H": [13], "A": [6], "L": [9], "L2": [7], "E": [5, 15], "N": [10], "G": [8], "E2": [11, 16],
                    "S": [12]}, [], []),
}


def disk(r):
    return [(dy, dx) for dy in range(-r, r + 1) for dx in range(-r, r + 1) if dx * dx + dy * dy <= r * r + r]


def erode(m, r):
    h, w = m.shape
    p = np.pad(m, r, constant_values=False)
    out = m.copy()
    for dy, dx in disk(r):
        out &= p[r + dy:r + dy + h, r + dx:r + dx + w]
    return out


def dilate1(m):
    h, w = m.shape
    p = np.pad(m, 1, constant_values=False)
    out = m.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out |= p[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
    return out


def label(m):
    h, w = m.shape
    lab = np.zeros((h, w), int)
    n = 0
    for y in range(h):
        for x in range(w):
            if m[y, x] and not lab[y, x]:
                n += 1
                q = deque([(y, x)])
                lab[y, x] = n
                while q:
                    cy, cx = q.popleft()
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            yy, xx = cy + dy, cx + dx
                            if 0 <= yy < h and 0 <= xx < w and m[yy, xx] and not lab[yy, xx]:
                                lab[yy, xx] = n
                                q.append((yy, xx))
    return lab, n


def grow(core, within, steps):
    """grown 1 px at a time, never leaving `within` (so it doesn't jump a gap)"""
    m = core & within
    for _ in range(steps):
        m = dilate1(m) & within
    return m


def main():
    dump, art = sys.argv[1], sys.argv[2]
    review = sys.argv[3] if len(sys.argv) > 3 else None
    for size, (r, letters, plaque, erase) in SIGNS.items():
        w, h = map(int, size.split("x"))
        a = np.fromfile(os.path.join(dump, "sign_%s.argb" % size), dtype=np.uint8).reshape(h, w, 4)
        rgb = a[:, :, [2, 1, 0]].astype(float)
        lum = (rgb[:, :, 0] * 3 + rgb[:, :, 1] * 6 + rgb[:, :, 2]) / 10
        ink = (a[:, :, 3] >= 128) & (lum < INK)
        lab, _ = label(erode(ink, r))
        allowed = np.ones((h, w), bool)
        for x0, y0, x1, y1 in erase:
            allowed[y0:y1 + 1, x0:x1 + 1] = False
        letter_mask = np.zeros((h, w), bool)
        per = {}
        for name, ids in letters.items():
            m = dilate1(grow(np.isin(lab, ids), ink & allowed, r + 2)) & allowed
            per[name] = m
            letter_mask |= m
        clear = np.zeros((h, w), bool)
        if plaque:
            pm = grow(np.isin(lab, plaque), ink, r + 2)
            ys, xs = np.nonzero(pm)
            box = np.zeros((h, w), bool)
            box[ys.min():ys.max() + 1, xs.min():xs.max() + 1] = True
            white = (rgb.min(axis=2) > 150) & box  # (its LEVEL lettering)
            clear = dilate1(dilate1(pm | white))
            letter_mask &= ~clear
        out = np.zeros((h, w, 4), np.uint8)
        out[:, :, 0] = letter_mask * 255
        out[:, :, 1] = clear * 255
        out[:, :, 3] = 255
        Image.fromarray(out).save(os.path.join(art, "sign_%s.png" % size), optimize=True)
        print("sign_%s.png: %d letters, %d px lit, %d px plaque" % (size, len(letters), letter_mask.sum(), clear.sum()))
        if review:
            S = 8
            bg = np.array([40, 120, 40], float)
            al = a[:, :, 3:4] / 255.0
            pic = rgb * al + bg * (1 - al)
            for li, (name, m) in enumerate(per.items()):
                ys, xs = np.nonzero(m)
                y0, y1, x0, x1 = max(0, ys.min() - 4), min(h, ys.max() + 5), max(0, xs.min() - 4), min(w, xs.max() + 5)
                crop = pic[y0:y1, x0:x1]
                lit = crop.copy()
                mm = m[y0:y1, x0:x1]
                lit[mm] = lit[mm] * 0.2 + np.array([235, 225, 240]) * 0.8
                other = (letter_mask & ~m)[y0:y1, x0:x1]
                lit[other] = lit[other] * 0.5 + np.array([120, 120, 255]) * 0.5
                pair = np.concatenate([crop, np.full((y1 - y0, 2, 3), 255.0), lit], axis=1)
                Image.fromarray(pair.astype(np.uint8)).resize((pair.shape[1] * S, pair.shape[0] * S), Image.NEAREST).save(
                    os.path.join(review, "%s_%02d_%s.png" % (size, li, name)))


if __name__ == "__main__":
    main()
