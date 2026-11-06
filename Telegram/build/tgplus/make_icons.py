#!/usr/bin/env python3
# Regenerates every TGPlus app icon in Telegram/Resources/art.
# Usage: python Telegram/build/tgplus/make_icons.py [--preview out.png]

import os
import sys

from PIL import Image, ImageDraw, ImageFilter

SUPER = 2048
ART = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Resources', 'art'))

PLANE_WING = [(0.205, 0.500), (0.760, 0.270), (0.365, 0.600)]
PLANE_BODY = [(0.760, 0.270), (0.660, 0.750), (0.452, 0.620)]
PLUS_CENTER = (0.315, 0.715)
PLUS_ARM = 0.075
PLUS_THICKNESS = 0.034


def scaled(points, size, inset):
    span = size - 2 * inset
    return [(inset + x * span, inset + y * span) for x, y in points]


def rounded(mask, radius):
    # WHY: blur + threshold rounds polygon corners without a vector library,
    # matching the soft geometry of the stock icon at every size.
    if radius <= 0:
        return mask
    blurred = mask.filter(ImageFilter.GaussianBlur(radius))
    return blurred.point(lambda v: 255 if v >= 128 else 0)


def draw_glyph(size, inset, with_plus):
    mask = Image.new('L', (size, size), 0)
    draw = ImageDraw.Draw(mask)
    draw.polygon(scaled(PLANE_WING, size, inset), fill=255)
    draw.polygon(scaled(PLANE_BODY, size, inset), fill=255)
    mask = rounded(mask, size * 0.006)
    if with_plus:
        span = size - 2 * inset
        cx = inset + PLUS_CENTER[0] * span
        cy = inset + PLUS_CENTER[1] * span
        arm = PLUS_ARM * span
        half = PLUS_THICKNESS * span / 2
        plus = ImageDraw.Draw(mask)
        plus.rounded_rectangle(
            (cx - arm, cy - half, cx + arm, cy + half), half, fill=255)
        plus.rounded_rectangle(
            (cx - half, cy - arm, cx + half, cy + arm), half, fill=255)
    return mask


def render(target, margin, with_plus=True):
    size = SUPER
    inset = round(size * margin)
    canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0))

    disc = Image.new('L', (size, size), 0)
    ImageDraw.Draw(disc).ellipse(
        (inset, inset, size - inset - 1, size - inset - 1), fill=255)

    shade = Image.linear_gradient('L').resize((size, size))
    top, bottom = (34, 34, 34), (4, 4, 4)
    fill = Image.merge('RGB', [
        shade.point(lambda v, a=a, b=b: round(a + (b - a) * v / 255))
        for a, b in zip(top, bottom)])
    canvas.paste(fill, (0, 0), disc)

    ring_width = max(1, round(size * 0.010))
    ring = Image.new('L', (size, size), 0)
    ImageDraw.Draw(ring).ellipse(
        (inset, inset, size - inset - 1, size - inset - 1),
        outline=70,
        width=ring_width)
    canvas.paste((255, 255, 255, 255), (0, 0), ring)

    glyph = draw_glyph(size, inset, with_plus)
    canvas.paste((255, 255, 255, 255), (0, 0), glyph)

    return canvas.resize((target, target), Image.LANCZOS)


def save_all():
    small_without_plus = 24
    for base in (16, 32, 48, 64, 128, 256, 512):
        for scale, suffix in ((1, ''), (2, '@2x')):
            size = base * scale
            image = render(size, 0.04, size > small_without_plus)
            image.save(os.path.join(ART, f'icon{base}{suffix}.png'))

    render(256, 0.04).save(os.path.join(ART, 'logo_256.png'))
    render(256, 0.0).save(os.path.join(ART, 'logo_256_no_margin.png'))
    render(1024, 0.05).save(os.path.join(ART, 'icon_round512@2x.png'))
    render(1024, 0.04).save(os.path.join(ART, 'icon_green.png'))
    render(256, 0.0).save(os.path.join(ART, 'iconbig_green.png'))

    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [render(s, 0.04, s > small_without_plus) for s in sizes]
    frames[-1].save(
        os.path.join(ART, 'icon256.ico'),
        sizes=[(s, s) for s in sizes],
        append_images=frames[:-1])


def preview(path):
    sizes = [16, 24, 32, 48, 64, 128, 256]
    width = sum(sizes) + 20 * (len(sizes) + 1)
    sheet = Image.new('RGBA', (width, 2 * 256 + 60), (0, 0, 0, 0))
    for row, background in enumerate(((243, 243, 243), (32, 32, 32))):
        y = 20 + row * (256 + 20)
        ImageDraw.Draw(sheet).rectangle(
            (0, y - 10, width, y + 256 + 10), fill=background + (255,))
        x = 20
        for size in sizes:
            image = render(size, 0.04, size > 24)
            sheet.alpha_composite(image, (x, y + 256 - size))
            x += size + 20
    sheet.save(path)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--preview':
        preview(sys.argv[2])
    else:
        save_all()
