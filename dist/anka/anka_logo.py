# SPDX-FileCopyrightText: Copyright 2026 EddizEge
# SPDX-License-Identifier: GPL-3.0-or-later

"""Anka's mark: a phoenix (Zumruduanka) rising, wings as flames, on a dark warm tile.

The source of dist/anka.ico, dist/anka.bmp and dist/qt_themes/default/icons/256x256/anka.png;
needs only Pillow:
    python dist/anka/anka_logo.py [output folder for a preview sheet]
Drawn in a unit square at 8x and scaled down, so the edges stay smooth. 16 and 24 px use a simpler
bird (two feathers a wing, a two-tongue tail): the full one turns to a blob that small.
"""

import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.dirname(HERE)

TILE_TOP = (58, 18, 16)
TILE_BOTTOM = (28, 8, 8)
FIRE = [(0.00, (255, 216, 92)), (0.45, (255, 146, 36)), (1.00, (226, 52, 30))]  # gold, orange, red


def quad(p0, c, p1, steps=24):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p1[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p1[1])
            for t in (i / steps for i in range(steps + 1))]


def path(start, segments):
    """segments: (control, end) pairs of quadratic curves from start."""
    points, here = [], start
    for control, end in segments:
        points += quad(here, control, end)[:-1]
        here = end
    return points + [here]


def mirror(points):
    return [(1 - x, y) for x, y in points]


def bird(simple):
    """Polygons of the bird in the unit square (y down): wings, tail, body, and the head as a flame."""
    if simple:
        wing = path((0.53, 0.40), [((0.66, 0.20), (0.90, 0.12)), ((0.80, 0.30), (0.74, 0.36)),
                                   ((0.90, 0.38), (0.88, 0.50)), ((0.70, 0.52), (0.55, 0.56))])
        tail = path((0.44, 0.60), [((0.40, 0.76), (0.34, 0.90)), ((0.46, 0.80), (0.50, 0.78)),
                                   ((0.54, 0.80), (0.66, 0.90)), ((0.60, 0.76), (0.56, 0.60))])
        body = path((0.50, 0.30), [((0.66, 0.42), (0.50, 0.68)), ((0.34, 0.42), (0.50, 0.30))])
        head = path((0.50, 0.08), [((0.53, 0.18), (0.58, 0.25)), ((0.50, 0.36), (0.42, 0.25)),
                                   ((0.47, 0.18), (0.50, 0.08))])
    else:
        wing = path((0.53, 0.37), [((0.64, 0.19), (0.88, 0.12)), ((0.80, 0.24), (0.75, 0.30)),
                                   ((0.86, 0.28), (0.92, 0.31)), ((0.82, 0.38), (0.76, 0.42)),
                                   ((0.84, 0.45), (0.85, 0.53)), ((0.70, 0.52), (0.555, 0.55))])
        tail = path((0.455, 0.60), [((0.42, 0.74), (0.35, 0.86)), ((0.43, 0.80), (0.46, 0.77)),
                                    ((0.48, 0.86), (0.50, 0.94)), ((0.52, 0.86), (0.54, 0.77)),
                                    ((0.57, 0.80), (0.65, 0.86)), ((0.58, 0.74), (0.545, 0.60))])
        body = path((0.50, 0.29), [((0.645, 0.42), (0.50, 0.67)), ((0.355, 0.42), (0.50, 0.29))])
        head = path((0.50, 0.075), [((0.52, 0.17), (0.565, 0.235)), ((0.50, 0.33), (0.435, 0.235)),
                                    ((0.48, 0.17), (0.50, 0.075))])
    return [wing, mirror(wing), tail, body, head]


def gradient(size, stops):
    """A vertical gradient image (RGB) through the (position, colour) stops."""
    column = Image.new('RGB', (1, size))
    for y in range(size):
        t = y / max(1, size - 1)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0)
                column.putpixel((0, y), tuple(round(a + (b - a) * k) for a, b in zip(c0, c1)))
                break
    return column.resize((size, size))


def render(size, simple=None, tile=True):
    simple = size <= 24 if simple is None else simple
    scale = 8
    big = size * scale
    image = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    if tile:
        mask = Image.new('L', (big, big), 0)
        inset = 0 if size <= 24 else round(big * 0.02)
        ImageDraw.Draw(mask).rounded_rectangle((inset, inset, big - 1 - inset, big - 1 - inset),
                                               radius=round(big * 0.22), fill=255)
        image.paste(gradient(big, [(0, TILE_TOP), (1, TILE_BOTTOM)]), (0, 0), mask)
    shapes = bird(simple)
    mask = Image.new('L', (big, big), 0)
    draw = ImageDraw.Draw(mask)
    for shape in shapes:
        draw.polygon([(x * big, y * big) for x, y in shape], fill=255)
    image.paste(gradient(big, FIRE), (0, 0), mask)
    return image.resize((size, size), Image.LANCZOS)


def write_bmp(image, path):
    """32-bit BI_RGB bottom-up BMP with alpha in the fourth byte, like dist/eden.bmp (anka-cli's
    SDL window icon, compiled in by yuzu_cmd's CMakeLists)."""
    import struct
    w, h = image.size
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = image.getpixel((x, y))
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    pixels = b''.join(rows)
    header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + info + pixels)


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [render(s) for s in sizes]
    ico = os.path.join(DIST, 'anka.ico')
    frames[-1].save(ico, format='ICO', sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    png = os.path.join(DIST, 'qt_themes', 'default', 'icons', '256x256', 'anka.png')
    render(256).save(png, optimize=True)
    bmp = os.path.join(DIST, 'anka.bmp')
    write_bmp(frames[-1], bmp)
    print(ico)
    print(png)
    print(bmp)
    if len(sys.argv) > 1:  # preview: every size at 1x and 4x, on light and dark
        sheet = Image.new('RGBA', (1500, 620), (240, 240, 240, 255))
        sheet.paste((32, 32, 36, 255), (0, 310, 1500, 620))
        x = 10
        for s, f in zip(sizes, frames):
            for y0 in (10, 320):
                sheet.alpha_composite(f, (x, y0))
                if s <= 64:
                    sheet.alpha_composite(f.resize((s * 4, s * 4), Image.NEAREST), (x, y0 + 270 - s * 4))
            x += max(s, s * 4 if s <= 64 else s) + 12
        out = os.path.join(sys.argv[1], 'anka_preview.png')
        sheet.save(out)
        print(out)


if __name__ == '__main__':
    main()
