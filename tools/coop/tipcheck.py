# Which screenshots show an instant tip (the "To shield surf..." box at the top right): counts the box's dark
# text pixels in each PNG of the given folders and prints the frames that have one. Black screens (Game Over,
# loading) are listed apart: their dark pixels are not a tip.
#   .venv\Scripts\python tools\coop\tipcheck.py build\surfshots\A build\surfshots\B
import glob
import os
import sys

from PIL import Image

for d in sys.argv[1:]:
    shown, black = [], []
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
    line = f'{d}: tip in {len(shown)} of {len(files)} frames'
    if shown:
        line += ': ' + ', '.join(shown)
    if black:
        line += f'; black screen (Game Over?) in {len(black)}, from {black[0]}'
    print(line)
