# seam_shadow.py: page 2's dirt mound is shaded darker over its first columns at the page seam; carried across to
# page 1 (make_page1_mound.py) that shadow doubled into a dark band down the middle of the mound. lift(p2, dirt)
# brightens those columns to the mound's body (each row's median brightness at x 28..60), by a smooth gain from
# a vertically blurred brightness, fading out by x 34 (the strokes stay).
import numpy as np
def lift(p2, dirt):
    out = p2.copy()
    lum = p2[..., 0] * 0.3 + p2[..., 1] * 0.59 + p2[..., 2] * 0.11
    ref = np.zeros(600, np.float32); ok = np.zeros(600, bool)
    for y in range(600):
        v = lum[y, 28:61][dirt[y, 28:61]]
        if len(v) >= 8: ref[y] = np.median(v); ok[y] = True
    ys = np.nonzero(ok)[0]
    if not len(ys): return out
    ref = np.interp(np.arange(600), ys, ref[ys])
    L = np.where(dirt, lum, np.nan)[:, :34]
    Lb = np.zeros_like(L)
    for x in range(34):
        col = L[:, x]
        for y in range(600):
            w = col[max(0, y - 6):y + 7]; w = w[~np.isnan(w)]
            Lb[y, x] = w.mean() if len(w) else np.nan
    for x in range(34):
        g = np.clip(ref / np.where(np.isnan(Lb[:, x]), ref, Lb[:, x]), 1.0, 1.5)
        g = 1 + (g - 1) * (1 - x / 34.0)
        m = dirt[:, x]
        out[m, x] = np.clip(p2[m, x] * g[m, None], 0, 255)
    return out
