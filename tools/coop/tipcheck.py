# Which screenshots show an instant tip (the "To shield surf..." box at the top right): counts the box's dark
# text pixels in each PNG of the given folders and prints the frames that have one. Black screens (Game Over,
# loading) are listed apart: their dark pixels are not a tip. Frames with the red ability tint are listed too.
#   .venv\Scripts\python tools\coop\tipcheck.py build\surfshots\A build\surfshots\B
import glob
import os
import sys

from PIL import Image

for d in sys.argv[1:]:
    shown, black, tinted = [], [], []
    files = sorted(glob.glob(os.path.join(d, '*.png')))
    for f in files:
        im = Image.open(f).convert('L')
        w, h = im.size
        small = im.resize((64, 36))
        if sum(small.get_flattened_data()) / (64 * 36) < 40:
            black.append(os.path.basename(f))
            continue
        box = im.crop((int(w * 0.75), int(h * 0.13), int(w * 0.97), int(h * 0.25)))
        if sum(1 for p in box.get_flattened_data() if p < 50) > 3000:
            shown.append(os.path.basename(f))
        # The Ultrahand / Recall world tint: many strongly red pixels (2-9 of 5184 in a normal frame, 130+ tinted;
        # a close-up of a red tunic also counts, so read it on the game whose player did not use the ability).
        rgb = Image.open(f).convert('RGB').resize((96, 54)).get_flattened_data()
        red = sum(1 for r, g, b in rgb if r > 110 and r > g * 1.6 and r > b * 1.6)
        if red > 60:
            tinted.append(os.path.basename(f))
    line = f'{d}: tip in {len(shown)} of {len(files)} frames'
    if shown:
        line += ': ' + ', '.join(shown)
    if black:
        line += f'; black screen (Game Over?) in {len(black)}, from {black[0]}'
    if tinted:
        line += f'; red world tint in {len(tinted)}: ' + ', '.join(tinted)
    print(line)
