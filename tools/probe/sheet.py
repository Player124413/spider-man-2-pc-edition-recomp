"""Контактный лист: сетка уменьшенных снимков (SNX_SHOT) с подписями — быстро просмотреть серию кадров.

sheet.py --out sheet.png [--cols 4] [--width 320] shot_*.png
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("shots", nargs="+")
    args = ap.parse_args()
    imgs = []
    for p in args.shots:
        im = Image.open(p).convert("RGB")
        h = im.height * args.width // im.width
        imgs.append((os.path.basename(p), im.resize((args.width, h))))
    th = max(im.height for _, im in imgs) + 14
    rows = (len(imgs) + args.cols - 1) // args.cols
    sheet = Image.new("RGB", (args.cols * args.width, rows * th), "white")
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(imgs):
        x, y = (i % args.cols) * args.width, (i // args.cols) * th
        sheet.paste(im, (x, y + 14))
        d.text((x + 2, y + 1), name, fill="black")
    sheet.save(args.out)


if __name__ == "__main__":
    sys.exit(main())
