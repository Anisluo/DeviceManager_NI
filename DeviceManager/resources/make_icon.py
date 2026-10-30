"""Generates the DeviceManager application icon.

    python make_icon.py            # writes app.ico and app_<size>.png next to this file

Design: terracotta rounded square (the UI accent, #D97757) with a paper-white chip:
solid body with pins on four sides and a digital pulse cut into it — "chip test & measurement".
Sizes >= 48 px are drawn in full; 16–32 px use a simplified mark (fewer pins, thicker
strokes) so the taskbar icon stays crisp. Everything is drawn 8x oversized and downsampled.
"""

import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ACCENT = (217, 119, 87, 255)        # #D97757
ACCENT_DARK = (190, 98, 70, 255)    # bottom edge shading
PAPER = (247, 245, 238, 255)        # #F7F5EE
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 8                              # supersampling factor


def draw_small(size):
    """16–32 px: solid chip body with the pulse cut out in the accent colour, snapped to whole
    pixels so the taskbar icon stays sharp."""
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = size / 16.0                                      # design grid: 16 units

    def px(v):                                           # units -> supersampled, on whole pixels
        return round(v * u) * SS

    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=px(3.5), fill=ACCENT)
    b0, b1 = 4, 12                                       # chip body 8x8 units
    d.rounded_rectangle([px(b0), px(b0), px(b1) - 1, px(b1) - 1], radius=max(SS, px(1)), fill=PAPER)
    for t in (6, 9):                                     # two pins per side, 1 unit wide, 2 long
        d.rectangle([px(t), px(2), px(t + 1) - 1, px(b0) - 1], fill=PAPER)
        d.rectangle([px(t), px(b1), px(t + 1) - 1, px(14) - 1], fill=PAPER)
        d.rectangle([px(2), px(t), px(b0) - 1, px(t + 1) - 1], fill=PAPER)
        d.rectangle([px(b1), px(t), px(14) - 1, px(t + 1) - 1], fill=PAPER)
    # pulse: low (y 9) – high (y 6) – low, 1 unit thick, cut into the body
    w = px(1)
    for x0, y0, x1, y1 in ((5, 9, 7, 10), (6, 6, 7, 10), (6, 6, 10, 7), (9, 6, 10, 10), (9, 9, 11, 10)):
        d.rectangle([px(x0), px(y0), px(x1) - 1, px(y1) - 1], fill=ACCENT)
    del w
    return img.resize((size, size), Image.LANCZOS)


def draw(size):
    if size <= 32:
        return draw_small(size)
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    small = False

    # background tile: rounded square, slight darker band at the bottom for depth
    pad = round(S * (0.04 if small else 0.06))
    radius = round(S * 0.22)
    d.rounded_rectangle([pad, pad, S - pad - 1, S - pad - 1], radius=radius, fill=ACCENT_DARK)
    d.rounded_rectangle([pad, pad, S - pad - 1, S - pad - 1 - round(S * 0.025)], radius=radius, fill=ACCENT)

    # chip body
    c = S / 2
    body = S * (0.46 if small else 0.42)
    x0, y0, x1, y1 = c - body / 2, c - body / 2, c + body / 2, c + body / 2
    d.rounded_rectangle([x0, y0, x1, y1], radius=round(S * 0.05), fill=PAPER)        # solid body, as in the small sizes

    # pins
    pins = 2 if small else 3
    pin_len = S * (0.085 if small else 0.075)
    pin_w = S * (0.07 if small else 0.045)
    span = body * (0.56 if small else 0.62)
    for i in range(pins):
        t = c - span / 2 + (span * i / (pins - 1) if pins > 1 else span / 2)
        # top / bottom
        d.rectangle([t - pin_w / 2, y0 - pin_len, t + pin_w / 2, y0], fill=PAPER)
        d.rectangle([t - pin_w / 2, y1, t + pin_w / 2, y1 + pin_len], fill=PAPER)
        # left / right
        d.rectangle([x0 - pin_len, t - pin_w / 2, x0, t + pin_w / 2], fill=PAPER)
        d.rectangle([x1, t - pin_w / 2, x1 + pin_len, t + pin_w / 2], fill=PAPER)

    # pulse trace inside the body: low – high – low – high
    w = round(S * (0.07 if small else 0.042))
    ix0, ix1 = x0 + body * 0.2, x1 - body * 0.2
    lo, hi = c + body * 0.15, c - body * 0.15
    if small:
        xs = [ix0, c - body * 0.08, c - body * 0.08, c + body * 0.12, c + body * 0.12, ix1]
        ys = [lo, lo, hi, hi, lo, lo]
    else:
        q = (ix1 - ix0) / 5
        xs = [ix0, ix0 + q, ix0 + q, ix0 + 2 * q, ix0 + 2 * q, ix0 + 3 * q, ix0 + 3 * q, ix0 + 4 * q, ix0 + 4 * q, ix1]
        ys = [lo, lo, hi, hi, lo, lo, hi, hi, lo, lo]
    d.line(list(zip(xs, ys)), fill=ACCENT, width=w, joint="curve")    # pulse cut into the body
    r = w / 2                                           # round the open ends
    for x, y in ((xs[0], ys[0]), (xs[-1], ys[-1])):
        d.ellipse([x - r, y - r, x + r, y + r], fill=ACCENT)

    return img.resize((size, size), Image.LANCZOS)


def main():
    images = {s: draw(s) for s in SIZES}
    for s in (16, 24, 32, 48, 64, 128, 256):
        images[s].save(os.path.join(HERE, f"app_{s}.png"))
    images[256].save(os.path.join(HERE, "app.ico"), sizes=[(s, s) for s in SIZES],
                     append_images=[images[s] for s in SIZES if s != 256])
    # preview strip for review
    strip = Image.new("RGBA", (sum(SIZES) + 12 * len(SIZES), 270), (245, 244, 238, 255))
    x = 6
    for s in SIZES:
        strip.alpha_composite(images[s], (x, 135 - s // 2))
        x += s + 12
    strip.save(os.path.join(HERE, "icon_preview.png"))
    print("wrote app.ico,", ", ".join(f"app_{s}.png" for s in (16, 24, 32, 48, 64, 128, 256)))


if __name__ == "__main__":
    main()
