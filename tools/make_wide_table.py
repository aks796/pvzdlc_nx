#!/usr/bin/env python3
"""make_wide_table.py OUT: the letters' wooden table (Mailbox_Table.jpg, 800x600) widened to 1066x600 for the
Multiplayer HELP pages, which lay it under see-through sheets with no frame (the mail screen keeps its 4:3 table
under its leaves frame). The planks run across, so each side continues them mirrored from its own edge
(columns 0..133 and 667..800 flipped): the plank seams line up, and the dirt heap at the bottom right ends in
a mirrored heap, as a heap does."""
import sys
import numpy as np
from PIL import Image

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

a = np.asarray(Image.open(ROOT + 'analysis/main_pak/images/Mailbox_Table.jpg').convert('RGB'))
P = 133
out = np.concatenate([a[:, P - 1::-1], a, a[:, :-P - 1:-1]], axis=1)
Image.fromarray(out).save(sys.argv[1])
print(sys.argv[1], out.shape[1], 'x', out.shape[0])
