#!/usr/bin/env python3
"""Convert univerify's PPM frames to PNG with their HUD drawn, and build the flythrough
contact sheet (D-018 fallback: no stb_image_write, Pillow does the PNG encoding).

Usage: make_images.py WORK_RENDER_DIR IMAGES_DIR
"""

import glob
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def with_hud(img, lines, size):
    d = ImageDraw.Draw(img)
    f = font(size)
    y = 4
    for line in lines:
        # Shadowed text so it reads on bright regions too.
        d.text((5, y + 1), line, fill=(0, 0, 0), font=f)
        d.text((4, y), line, fill=(120, 255, 160), font=f)
        y += size + 3
    return img


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    stills = []
    for ppm in sorted(glob.glob(os.path.join(src, "still_*.ppm"))):
        name = os.path.splitext(os.path.basename(ppm))[0]
        hud = json.load(open(ppm[:-4] + ".hud.json"))["lines"]
        img = with_hud(Image.open(ppm).convert("RGB"), hud, 14)
        img.save(os.path.join(dst, name + ".png"), optimize=True)
        stills.append(name)

    n_frames = 0
    for prefix, out in (("fly_", "flythrough_contact_sheet.png"), ("volfly_", "flythrough_volume_contact_sheet.png")):
        frames = sorted(glob.glob(os.path.join(src, prefix + "*.ppm")))
        if not frames:
            continue
        n_frames += len(frames)
        tiles = []
        for ppm in frames:
            hud = json.load(open(ppm[:-4] + ".hud.json"))["lines"]
            tiles.append(with_hud(Image.open(ppm).convert("RGB"), hud, 11))
        w, h = tiles[0].size
        cols = 8
        rows = (len(tiles) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * w + (cols + 1) * 2, rows * h + (rows + 1) * 2), (40, 40, 48))
        for i, t in enumerate(tiles):
            sheet.paste(t, (2 + (i % cols) * (w + 2), 2 + (i // cols) * (h + 2)))
        sheet.save(os.path.join(dst, out), optimize=True)
    print(f"{len(stills)} stills, {n_frames} flythrough frames -> {dst}")


if __name__ == "__main__":
    main()
