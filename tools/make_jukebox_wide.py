#!/usr/bin/env python3
"""make_jukebox_wide.py OUT: the music screen's background (jukebox_background.png, 1280x720), cut to the part the
game shows (it draws it unscaled at -133, 0: columns 0..1066, rows 0..600 fill the 16:9 screen). At 1280x720 it
was drawn only within the 4:3 window (too big for the wide path), the menu showing beside it."""
import sys
from PIL import Image

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/'  # the project folder

im = Image.open(ROOT + 'analysis/main_pak/images/jukebox_background.png').convert('RGB')
im.crop((0, 0, 1066, 600)).save(sys.argv[1])
print(sys.argv[1])
