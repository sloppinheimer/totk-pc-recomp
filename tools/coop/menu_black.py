"""Counts the inventory openings whose backdrop is black, in a folder of a game's TOTK_SHOTS frames (f<n>.png).

A frame shows the inventory when its bottom bar (the cream strip under the item grid) is bright; its backdrop is black
when the area between the item panel and Link (x 55..62 %, y 15..65 %) is nearly all dark, where it otherwise shows the
blurred world. Consecutive inventory frames are one opening; it counts as black when most of its frames are.

    python tools/coop/menu_black.py build/menushots/solo
"""
import os
import sys

from PIL import Image


def frame_state(path):
    im = Image.open(path).convert('L')
    w, h = im.size
    bar = im.crop((int(w * 0.02), int(h * 0.945), int(w * 0.30), int(h * 0.985)))
    px = bar.tobytes()
    menu = sum(px) / len(px) > 190
    back = im.crop((int(w * 0.55), int(h * 0.15), int(w * 0.62), int(h * 0.65))).tobytes()
    dark = sum(1 for v in back if v < 14) / len(back)
    return menu, dark


def main():
    d = sys.argv[1]
    files = sorted((f for f in os.listdir(d) if f.startswith('f') and f.endswith('.png')), key=lambda f: int(f[1:-4]))
    openings, cur = [], None
    for f in files:
        menu, dark = frame_state(os.path.join(d, f))
        if menu:
            if cur is None:
                cur = [f, 0, 0]
            cur[1] += 1
            cur[2] += dark > 0.8
        elif cur is not None:
            openings.append(cur)
            cur = None
    if cur is not None:
        openings.append(cur)
    black = [o for o in openings if o[2] * 2 > o[1]]
    print(f'{d}: {len(openings)} inventory openings, {len(black)} with a black backdrop'
          + (': at ' + ', '.join(o[0] for o in black) if black else ''))


if __name__ == '__main__':
    main()
