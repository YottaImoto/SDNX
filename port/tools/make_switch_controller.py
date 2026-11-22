#!/usr/bin/env python3
"""Draw the Switch for the controls-layout screen (UI texture CONTROLLER, 512x256 DXT5) in the style of the game's
Xbox 360 drawing: cream body, near-black contour lines, a light grunge, a front view. The game's own logo (SD_LOGO)
is traced as a contour on the console's screen.

All shapes are drawn here; the letters use the stroke font of make_switch_buttons.py. The logo is read from the
user's own game data (see controls_screen_patch.py) and only its contour is drawn.

BUTTONS gives every button's centre in texture pixels: controls_screen_patch.py points the callout lines there.

Usage (preview): make_switch_controller.py logo.png out.png
"""
import math
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

from make_switch_buttons import GLYPHS, ADVANCE

W, H = 512, 256
SS = 4
FILL = (222, 217, 189)
INK = (16, 12, 8)
SCREEN = (34, 30, 26)

# Body (texture pixels): left Joy-Con, tablet, right Joy-Con; proportions of the real console (239 x 102 mm).
X0, X1 = 14, 498
Y0, Y1 = 36, 238
JC = 73
TAB0, TAB1 = X0 + JC, X1 - JC
SCR = (104, 52, 408, 222)  # screen (16:9 inside the bezel)

BUTTONS = {
    # left Joy-Con
    "ZL": (50, 22), "L": (31, 32), "MINUS": (70, 47), "LSTICK": (50, 88), "L3": (54, 92),
    "DPAD_UP": (50, 135), "DPAD_DOWN": (50, 169), "DPAD_LEFT": (33, 152), "DPAD_RIGHT": (67, 152),
    "CAPTURE": (66, 203),
    # right Joy-Con
    "ZR": (462, 22), "R": (481, 32), "PLUS": (442, 47), "X": (462, 71), "A": (479, 88), "B": (462, 105),
    "Y": (445, 88), "RSTICK": (462, 152), "R3": (458, 156), "HOME": (446, 203),
}


def S(v):
    return v * SS


def _lw(px):
    return max(1, int(round(px * SS)))


def _rounded_side(x0, y0, x1, y1, r, left):
    """Outline of a Joy-Con: outer corners round (radius r), inner side straight."""
    pts = []
    if left:
        pts += [(x1, y0)]
        pts += [(x0 + r + r * math.cos(math.radians(a)), y0 + r + r * math.sin(math.radians(a)))
                for a in range(270, 179, -6)]
        pts += [(x0 + r + r * math.cos(math.radians(a)), y1 - r + r * math.sin(math.radians(a)))
                for a in range(180, 89, -6)]
        pts += [(x1, y1)]
    else:
        pts += [(x0, y0)]
        pts += [(x1 - r + r * math.cos(math.radians(a)), y0 + r + r * math.sin(math.radians(a)))
                for a in range(270, 361, 6)]
        pts += [(x1 - r + r * math.cos(math.radians(a)), y1 - r + r * math.sin(math.radians(a)))
                for a in range(0, 91, 6)]
        pts += [(x0, y1)]
    return [(S(x), S(y)) for x, y in pts]


def _shoulder(d, x0, x1, ytop, ybot, left):
    """A shoulder button seen from the front: a rounded cap on the top edge."""
    r = (ybot - ytop)
    box = [S(x0), S(ytop), S(x1), S(ybot + r)]
    d.rounded_rectangle(box, radius=S(r), fill=FILL, outline=INK, width=_lw(2.2))


def _circle(d, cx, cy, r, w=2.2, fill=FILL):
    d.ellipse([S(cx - r), S(cy - r), S(cx + r), S(cy + r)], fill=fill, outline=INK, width=_lw(w))


def _letter(d, ch, cx, cy, h, w=1.8):
    adv = ADVANCE[ch]
    for stroke in GLYPHS[ch]:
        pts = [(S(cx - adv * h / 2 + px * h), S(cy - h / 2 + py * h)) for px, py in stroke]
        d.line(pts, fill=INK, width=_lw(w), joint="curve")


def _stick(d, cx, cy):
    _circle(d, cx, cy, 20, 2.4)
    _circle(d, cx, cy, 15.5, 2.0)
    d.ellipse([S(cx - 10), S(cy - 10), S(cx + 10), S(cy + 10)], outline=INK, width=_lw(1.2))
    for a in range(0, 360, 45):  # grip pattern of the cap
        x, y = cx + 13 * math.cos(math.radians(a)), cy + 13 * math.sin(math.radians(a))
        d.ellipse([S(x - 0.9), S(y - 0.9), S(x + 0.9), S(y + 0.9)], fill=INK)


def _arrow(d, cx, cy, ang, size=3.2):
    pts = []
    for a in (0, 140, 220):
        t = math.radians(ang + a)
        pts.append((S(cx + size * math.cos(t)), S(cy + size * math.sin(t))))
    d.polygon(pts, fill=INK)


def _grunge(rng):
    """Low-frequency blotches + speckles, as a multiplicative factor around 1."""
    f = np.zeros((H * SS, W * SS), np.float32)
    for scale, amp in ((64, 0.022), (24, 0.018), (8, 0.016), (2, 0.022)):
        small = rng.standard_normal((H * SS // (scale * SS) + 2, W * SS // (scale * SS) + 2)).astype(np.float32)
        big = Image.fromarray(small).resize((W * SS, H * SS), Image.BICUBIC)
        f += np.asarray(big, np.float32) * amp
    speck = Image.fromarray(((rng.random((H * SS, W * SS)) < 0.0018) * 255).astype(np.uint8))
    speck = np.asarray(speck.filter(ImageFilter.GaussianBlur(SS * 0.6)), np.float32) / 255.0
    return 1.0 + f - speck * 0.9


def _logo_contour(logo_rgba, box):
    """Contour lines of the shield and the letters of the game's logo, fitted into box (supersampled px)."""
    x0, y0, x1, y1 = box
    a = np.asarray(logo_rgba, np.float32)
    alpha, lum = a[..., 3], a[..., :3].mean(-1)
    ys, xs = np.nonzero(alpha > 128)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    alpha, lum = a[..., 3], a[..., :3].mean(-1)
    h0, w0 = alpha.shape
    k = min((x1 - x0) / w0, (y1 - y0) / h0)
    tw, th = int(w0 * k), int(h0 * k)
    shield = Image.fromarray(((alpha > 128) * 255).astype(np.uint8)).resize((tw, th), Image.BILINEAR)
    letters = Image.fromarray((((alpha > 128) & (lum > 110)) * 255).astype(np.uint8)).resize((tw, th), Image.BILINEAR)
    lines = Image.new("L", (tw, th), 0)
    for m, width in ((shield, 2.2), (letters, 1.5)):
        m = m.point(lambda v: 255 if v > 127 else 0)
        n = _lw(width) | 1
        edge = ImageChops.subtract(m.filter(ImageFilter.MaxFilter(n)), m.filter(ImageFilter.MinFilter(3)))
        lines = ImageChops.lighter(lines, edge)
    # the inner border of the shield (the white rim of the logo)
    inner = Image.fromarray((((alpha > 128) & (lum < 110)) * 255).astype(np.uint8)).resize((tw, th), Image.NEAREST)
    lines = ImageChops.lighter(lines, ImageChops.subtract(inner.filter(ImageFilter.MaxFilter(3)), inner))
    return lines.filter(ImageFilter.GaussianBlur(SS * 0.25)), (int((x0 + x1 - tw) / 2), int((y0 + y1 - th) / 2))


def draw(logo_rgba=None, seed=7):
    img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # shoulders behind the body: ZL / ZR (back, higher), L / R (front)
    _shoulder(d, 26, 78, 12, 26, True)
    _shoulder(d, 434, 486, 12, 26, False)
    _shoulder(d, 18, 74, 24, 36, True)
    _shoulder(d, 438, 494, 24, 36, False)
    # body
    d.polygon(_rounded_side(X0, Y0, TAB0, Y1, 34, True), fill=FILL, outline=INK, width=_lw(2.6))
    d.polygon(_rounded_side(TAB1, Y0, X1, Y1, 34, False), fill=FILL, outline=INK, width=_lw(2.6))
    d.rectangle([S(TAB0), S(Y0), S(TAB1), S(Y1)], fill=FILL, outline=INK, width=_lw(2.6))
    # rails between the Joy-Cons and the tablet
    for x in (TAB0, TAB1):
        d.line([(S(x), S(Y0)), (S(x), S(Y1))], fill=INK, width=_lw(2.6))
    # screen and its bezel line
    d.rounded_rectangle([S(SCR[0] - 6), S(SCR[1] - 8), S(SCR[2] + 6), S(SCR[3] + 8)], radius=S(4), outline=INK,
                        width=_lw(1.2))
    d.rounded_rectangle([S(v) for v in SCR], radius=S(2), fill=SCREEN, outline=INK, width=_lw(2.0))
    # left Joy-Con
    bx, by = BUTTONS["MINUS"]
    d.rounded_rectangle([S(bx - 6), S(by - 2), S(bx + 6), S(by + 2)], radius=S(2), fill=FILL, outline=INK,
                        width=_lw(1.6))
    _stick(d, *BUTTONS["LSTICK"])
    for name, ang in (("DPAD_UP", -90), ("DPAD_DOWN", 90), ("DPAD_LEFT", 180), ("DPAD_RIGHT", 0)):
        cx, cy = BUTTONS[name]
        _circle(d, cx, cy, 8, 2.0)
        _arrow(d, cx, cy, ang)
    cx, cy = BUTTONS["CAPTURE"]
    d.rounded_rectangle([S(cx - 6), S(cy - 6), S(cx + 6), S(cy + 6)], radius=S(2), fill=FILL, outline=INK,
                        width=_lw(1.8))
    d.ellipse([S(cx - 3), S(cy - 3), S(cx + 3), S(cy + 3)], outline=INK, width=_lw(1.2))
    # right Joy-Con
    cx, cy = BUTTONS["PLUS"]
    d.line([(S(cx - 6), S(cy)), (S(cx + 6), S(cy))], fill=INK, width=_lw(3.6))
    d.line([(S(cx), S(cy - 6)), (S(cx), S(cy + 6))], fill=INK, width=_lw(3.6))
    d.line([(S(cx - 5), S(cy)), (S(cx + 5), S(cy))], fill=FILL, width=_lw(1.2))
    d.line([(S(cx), S(cy - 5)), (S(cx), S(cy + 5))], fill=FILL, width=_lw(1.2))
    for name in ("X", "A", "B", "Y"):
        cx, cy = BUTTONS[name]
        _circle(d, cx, cy, 8.5, 2.0)
        _letter(d, name, cx, cy, 8.5, 1.7)
    _stick(d, *BUTTONS["RSTICK"])
    cx, cy = BUTTONS["HOME"]
    _circle(d, cx, cy, 7, 1.8)
    d.polygon([(S(cx - 3.2), S(cy - 0.2)), (S(cx), S(cy - 3.2)), (S(cx + 3.2), S(cy - 0.2))], fill=INK)
    d.rectangle([S(cx - 2.2), S(cy - 0.2), S(cx + 2.2), S(cy + 2.8)], fill=INK)
    # the logo's contour on the screen
    if logo_rgba is not None:
        lines, (lx, ly) = _logo_contour(logo_rgba, (S(SCR[0] + 70), S(SCR[1] + 14), S(SCR[2] - 70), S(SCR[3] - 14)))
        cream = Image.new("RGBA", lines.size, FILL + (255,))
        img.paste(cream, (lx, ly), lines)
    # grunge on the cream parts, then down to the texture size
    a = np.asarray(img, np.float32)
    g = _grunge(np.random.default_rng(seed))
    cream = (a[..., :3].mean(-1) > 120)[..., None]
    a[..., :3] = np.where(cream, np.clip(a[..., :3] * g[..., None], 0, 255), a[..., :3])
    out = Image.fromarray(a.astype(np.uint8), "RGBA").resize((W, H), Image.LANCZOS)
    return np.asarray(out)


def main(argv):
    logo = Image.open(argv[1]).convert("RGBA") if len(argv) > 1 and argv[1] != "-" else None
    img = draw(logo)
    Image.fromarray(img, "RGBA").save(argv[2] if len(argv) > 2 else "controller_switch.png")


if __name__ == "__main__":
    main(sys.argv)
