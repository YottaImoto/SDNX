#!/usr/bin/env python3
"""Controls-layout screen (Scaleform movie Options_Controllers, UI.big 0x6CBBBDB6) for the Switch drawing.

The movie shows the texture CONTROLLER (a 512x256 drawing, replaced by make_switch_controller.py) and, over it,
callout lines with a dot on each button (shape 76: on foot, shape 77: driving; clip mc_lines, scale 0.5) from the
text fields of the labels (sprites 57 / 75: tf_* / dtf_*, filled by the game from $PAUSE_HELP_* strings).

Changes:
  * shapes 76 and 77 are generated again: from the same label anchors to the Switch buttons
    (make_switch_controller.BUTTONS), with the X/Y swap of the NRO (input_swap_xy): Xbox X action (button1) -> Y,
    Xbox Y action (button2) -> X, A -> A, B -> B (by letter);
  * labels moved: Map (select) and Pause (start) both above the console (- and + are on top of the Joy-Cons),
    accept (A) above back (B) - on the Switch A is the right button, B the bottom one.

Usage (review): controls_screen_patch.py movie.swf controller.png out_preview.png [labels.json]
"""
import json
import math
import struct
import sys
import zlib

import swfshape as sp
from make_switch_controller import BUTTONS

ENTRY_UID = 0x6CBBBDB6
TEXTURE_PERM = 0x35C57C49
TEXTURE_TEMP = 0xDA987C4D
MARKER_SHAPE = 76

# Texture pixel -> sprite-118 space (mcController at -168,-100; the bitmap is placed at -24 px, 0.78125 px/texel).
TEX_SCALE = 0.78125


def tex_to_s(u, v):
    return (-192 + TEX_SCALE * u, -100 + TEX_SCALE * v)


# mc_lines: placed at (463.8, 171.8) scale 0.5 inside mcX360Controls (-457, -171.8).
def s_to_lines(x, y):
    return ((x - 6.8) * 2 * 20, y * 2 * 20)  # twips of the shape


COMBAT = {
    "tf_R2": ["ZR"], "tf_R1": ["R"], "tf_button2": ["X"], "tf_back": ["B"], "tf_accept": ["A"], "tf_button1": ["Y"],
    "tf_RStick1": ["RSTICK"], "tf_start": ["PLUS"], "tf_UP": ["DPAD_UP"], "tf_L2": ["ZL"], "tf_select": ["MINUS"],
    "tf_LStick": ["LSTICK"], "tf_dpad_DOWN": ["DPAD_DOWN"], "tf_L1": ["L"], "tf_L3": ["L3"],
}
DRIVING = {
    "dtf_R2": ["ZR"], "dtf_R1": ["R"], "dtf_button2": ["X"], "dtf_back": ["B"], "dtf_accept": ["A"],
    "dtf_button1": ["Y"], "dtf_RStick1": ["RSTICK"], "dtf_RStick2": ["R3"], "dtf_start": ["PLUS"],
    "dtf_LStick": ["LSTICK"], "dtf_L2": ["ZL"], "dtf_select": ["MINUS"], "dtf_dpad_DOWN": ["DPAD_LEFT", "DPAD_RIGHT"],
    "dtf_L1": ["L"], "dtf_dpad_UP": ["DPAD_UP"], "dtf_L3": ["L3"],
}
SPRITES = {57: ("combat", COMBAT, 6.5), 75: ("driving", DRIVING, 0.0)}  # sprite id -> mode, map, txt* y offset
SHAPE_OF = {"combat": 76, "driving": 77}

# Label moves (field name -> new (tx, ty) in pixels; None keeps that coordinate). Absolute, so patching twice is safe.
# accept / back trade places: the original back (B, right on the Xbox) above accept (A, bottom).
MOVES = {
    "combat": {"tf_select": (331.0, 24.0), "tf_start": (526.0, 24.0), "tf_accept": (None, 135.8),
               "tf_back": (None, 215.0)},
    "driving": {"dtf_select": (331.0, 30.0), "dtf_start": (526.0, 30.0), "dtf_accept": (None, 153.2),
                "dtf_back": (None, 210.6), "dtf_RStick2": (610.0, 354.0)},
}
# Text fields made larger for the second line of the touch controls (field id -> (width, height) px):
# Map (52 on foot, 69 driving) wider, the camera labels (48, 65) two lines high.
FIELD_SIZE = {52: (170.0, 39.1), 69: (170.0, 39.1), 48: (288.0, 40.0), 65: (288.0, 40.0)}
# Callouts that turn at a given texture x (u) instead of near their label: Y (the left face button) is reached
# from below through the gap between the tablet and the right stick, not across A and B.
# B (the bottom face button) is reached down the right edge of the Joy-Con and from below, around the stick and A;
# the camera / horn labels under the console go straight to the stick.
ROUTE = {
    "tf_button1": [("x", 431)], "dtf_button1": [("x", 431)],
    "tf_back": [("x", 490), ("xy", 490, 105)], "dtf_back": [("x", 490), ("xy", 490, 105)],
    "tf_RStick1": [], "dtf_RStick1": [], "dtf_RStick2": [],
}

LINE_W = 2.0  # px in sprite-118 space (the original: 4 units of the 0.5-scaled clip)
DOT_R = 2.7


def _load(movie):
    sig, ver = movie[:3], movie[3]
    body = zlib.decompress(movie[8:]) if sig == b"CFX" else movie[8:]
    return sig, ver, body


def _collect(body):
    texts, sprites, shapes = {}, {}, {}

    def walk(tags, owner=None):
        for code, start, s, ln in tags:
            if code == 37:
                t = sp.edit_text(body, s, ln)
                texts[t["id"]] = t
            elif code in (2, 22, 32, 83):
                sh = sp.shape(body, s, ln, code)
                sh["tag"] = (code, start, s, ln)
                shapes[sh["id"]] = sh
            elif code == 39:
                sid = struct.unpack_from("<H", body, s)[0]
                sprites[sid] = []
                walk(sp.iter_tags(body, s + 4), sid)
            elif code == 26 and owner is not None:
                po = sp.place_object2(body, s, ln)
                po["tag"] = (start, s, ln)
                sprites[owner].append(po)
    walk(sp.top_tags(body))
    return texts, sprites, shapes


def _layout(body, apply_moves=True):
    """Per mode: field name -> (box in sprite-118 space, align, placement); with the label moves applied."""
    texts, sprites, shapes = _collect(body)
    out = {}
    for sid, (mode, mapping, yoff) in SPRITES.items():
        fields = {}
        for po in sprites[sid]:
            name = po.get("name", "")
            t = texts.get(po.get("char"))
            if not t or "matrix" not in po:
                continue
            fields[name] = dict(t=t, po=po, tx=po["matrix"][4] / 20, ty=po["matrix"][5] / 20)
        for name, (tx, ty) in (MOVES[mode].items() if apply_moves else ()):
            if name in fields:
                if tx is not None:
                    fields[name]["tx"] = tx
                if ty is not None:
                    fields[name]["ty"] = ty
        for f in fields.values():
            x0, x1, y0, y1 = [v / 20 for v in f["t"]["bounds"]]
            if apply_moves and f["t"]["id"] in FIELD_SIZE:
                w, h = FIELD_SIZE[f["t"]["id"]]
                x1, y1 = x0 + w + 2, y0 + h + 2
            X, Y = -457 - 27 + f["tx"], -171.8 + yoff + f["ty"]
            f["box"] = (X + x0, Y + y0, X + x1, Y + y1)
            f["align"] = f["t"].get("align", 0)
        out[mode] = fields
    return out, texts, sprites, shapes


def _callouts(mode, fields):
    """Polylines (sprite-118 space) and dot centres for one mode."""
    mapping = COMBAT if mode == "combat" else DRIVING
    lines, dots = [], []
    for name, buttons in mapping.items():
        f = fields.get(name)
        if not f:
            continue
        x0, y0, x1, y1 = f["box"]
        targets = [tex_to_s(*BUTTONS[b]) for b in buttons]
        if name.endswith("select") or name.endswith("start"):
            # centred labels above the console: from the middle of the label's bottom straight to the button
            a = ((x0 + x1) / 2, y0 + (41.0 if name.endswith("select") else 21.0))  # Map has two lines
            for d in targets:
                lines.append([a, d])
                dots.append(d)
            continue
        right = f["align"] == 0  # right column: left-aligned text, the line leaves from its left edge
        a = (x0 - 0.7, y0 + 11.3) if right else (x1 + 0.7, y0 + 11.3)
        for d in targets:
            dx = a[0] - d[0]
            inset = max(12.0, min(60.0, abs(dx) * 0.42))
            e = (a[0] - inset if right else a[0] + inset, a[1])
            if name in ROUTE:
                via = [(tex_to_s(r[1], 0)[0], a[1]) if r[0] == "x" else tex_to_s(r[1], r[2]) for r in ROUTE[name]]
                lines.append([a] + via + [d])
            elif abs(a[1] - d[1]) < 3.0:
                lines.append([a, d])
            else:
                lines.append([a, e, d])
            dots.append(d)
    return lines, dots


def _stroke_polygon(pts, w):
    """A polyline as a closed polygon of width w (miter joins, flat ends)."""
    h = w / 2
    n = len(pts)
    left, right = [], []
    for i in range(n):
        if i == 0:
            dx, dy = pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]
            ln = math.hypot(dx, dy) or 1
            nx, ny = -dy / ln, dx / ln
            k = 1.0
        elif i == n - 1:
            dx, dy = pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]
            ln = math.hypot(dx, dy) or 1
            nx, ny = -dy / ln, dx / ln
            k = 1.0
        else:
            ax, ay = pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]
            bx, by = pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]
            la, lb = math.hypot(ax, ay) or 1, math.hypot(bx, by) or 1
            n1 = (-ay / la, ax / la)
            n2 = (-by / lb, bx / lb)
            mx, my = n1[0] + n2[0], n1[1] + n2[1]
            ml = math.hypot(mx, my) or 1
            nx, ny = mx / ml, my / ml
            k = min(3.0, 1.0 / max(0.2, nx * n1[0] + ny * n1[1]))
        left.append((pts[i][0] + nx * h * k, pts[i][1] + ny * h * k))
        right.append((pts[i][0] - nx * h * k, pts[i][1] - ny * h * k))
    return left + right[::-1]


def _area(poly):
    return 0.5 * sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
                     for i in range(len(poly)))


def _encode_shape(orig_body, sh, lines, dots):
    """A DefineShape4 payload: the original styles, new records (fill 1 inside, line style 1)."""
    code, start, s, ln = sh["tag"]
    nf, nl = sh["nfill"], sh["nline"]
    polys = [_stroke_polygon([s_to_lines(*p) for p in pl], LINE_W * 2 * 20) for pl in lines]
    circles = [s_to_lines(*d) for d in dots]
    r = DOT_R * 2 * 20
    xs = [p[0] for pl in polys for p in pl] + [c[0] + k * r for c in circles for k in (-1, 1)]
    ys = [p[1] for pl in polys for p in pl] + [c[1] + k * r for c in circles for k in (-1, 1)]
    pad = 40
    bounds = (int(min(xs)) - pad, int(max(xs)) + pad, int(min(ys)) - pad, int(max(ys)) + pad)
    bw = sp.BitWriter()
    sp.write_rect(bw, *bounds)
    bw.align()
    sp.write_rect(bw, *bounds)
    bw.align()
    head = struct.pack("<H", sh["id"]) + bw.bytes()
    flags = orig_body[s + 2 + (len(head) - 2):s + 2 + (len(head) - 2) + 1]  # placeholder, replaced below
    # the original flags byte sits right before the styles
    flags = orig_body[sh["styles_start"] - 1:sh["styles_start"]]
    styles = orig_body[sh["styles_start"]:sh["records_start"]]
    rec = sp.BitWriter()
    rec.ub(nf, 4)
    rec.ub(nl, 4)
    cur = [0, 0]

    def move(x, y, fill0, fill1, line):
        x, y = int(round(x)), int(round(y))
        rec.ub(0, 1)            # style change
        rec.ub(0, 1)            # new styles
        rec.ub(1, 1)            # line style
        rec.ub(1, 1)            # fill style 1
        rec.ub(1, 1)            # fill style 0
        rec.ub(1, 1)            # move to
        n = sp.sbits(x, y)
        rec.ub(n, 5)
        rec.sb(x, n)
        rec.sb(y, n)
        rec.ub(fill0, nf)
        rec.ub(fill1, nf)
        rec.ub(line, nl)
        cur[0], cur[1] = x, y

    def straight(x, y):
        x, y = int(round(x)), int(round(y))
        dx, dy = x - cur[0], y - cur[1]
        if not dx and not dy:
            return
        n = max(2, sp.sbits(dx, dy))
        rec.ub(1, 1)
        rec.ub(1, 1)
        rec.ub(n - 2, 4)
        rec.ub(1, 1)            # general line
        rec.sb(dx, n)
        rec.sb(dy, n)
        cur[0], cur[1] = x, y

    def curve(cx, cy, x, y):
        cx, cy, x, y = int(round(cx)), int(round(cy)), int(round(x)), int(round(y))
        a, b, c, d = cx - cur[0], cy - cur[1], x - cx, y - cy
        n = max(2, sp.sbits(a, b, c, d))
        rec.ub(1, 1)
        rec.ub(0, 1)
        rec.ub(n - 2, 4)
        for v in (a, b, c, d):
            rec.sb(v, n)
        cur[0], cur[1] = x, y

    for poly in polys:
        if _area(poly) > 0:      # interior on the left of the path (y down): fill style 0
            poly = poly[::-1]
        move(*poly[0], 1, 0, 1)
        for p in poly[1:] + [poly[0]]:
            straight(*p)
    k = 1 / math.cos(math.pi / 8)
    for cx, cy in circles:
        pts = [(cx + r * math.cos(-i * math.pi / 4), cy + r * math.sin(-i * math.pi / 4)) for i in range(9)]
        move(*pts[0], 1, 0, 1)   # counter-clockwise on screen (angles decreasing, y down): inside on the left
        for i in range(8):
            a = -(i + 0.5) * math.pi / 4
            curve(cx + r * k * math.cos(a), cy + r * k * math.sin(a), *pts[i + 1])
    rec.ub(0, 6)                 # end of shape
    return head + flags + styles + rec.bytes()


def patch_movie(movie: bytes) -> bytes:
    sig, ver, body = _load(movie)
    layout, texts, sprites, shapes = _layout(body)
    new_shapes = {}
    for mode, fields in layout.items():
        lines, dots = _callouts(mode, fields)
        sid = SHAPE_OF[mode]
        new_shapes[sid] = _encode_shape(body, shapes[sid], lines, dots)
    # new placements of the moved labels
    replaced = {}
    for sid, (mode, mapping, yoff) in SPRITES.items():
        fields = layout[mode]
        for name, f in fields.items():
            po = f["po"]
            m = po["matrix"]
            tx, ty = int(round(f["tx"] * 20)), int(round(f["ty"] * 20))
            if (tx, ty) != (m[4], m[5]):
                start, s, ln = po["tag"]
                replaced[start] = sp.tag(26, sp.place_object2_set_matrix(body, s, ln, (m[0], m[1], m[2], m[3], tx, ty)))

    def emit(tags):
        out = bytearray()
        for code, start, s, ln in tags:
            if code == 37 and struct.unpack_from("<H", body, s)[0] in FIELD_SIZE:
                cid = struct.unpack_from("<H", body, s)[0]
                br = sp.Bits(body, s + 2)
                x0, x1, y0, y1 = sp.rect(br)
                br.align()
                w, h = FIELD_SIZE[cid]
                bw = sp.BitWriter()
                sp.write_rect(bw, x0, x0 + int(round((w + 2) * 20)), y0, y0 + int(round((h + 2) * 20)))
                out += sp.tag(37, body[s:s + 2] + bw.bytes() + body[br.pos:s + ln])
                continue
            if start in replaced:
                out += replaced[start]
            elif code == 83 and struct.unpack_from("<H", body, s)[0] in new_shapes:
                out += sp.tag(83, new_shapes[struct.unpack_from("<H", body, s)[0]])
            elif code == 39:
                sid, frames = struct.unpack_from("<HH", body, s)
                inner = emit(sp.iter_tags(body, s + 4))
                out += sp.tag(39, struct.pack("<HH", sid, frames) + inner)
            else:
                out += body[start:s + ln]
        return bytes(out)

    br = sp.Bits(body)
    sp.rect(br)
    br.align()
    head = body[:br.pos + 4]
    nb = head + emit(sp.top_tags(body))
    return b"CFX" + bytes([ver]) + struct.pack("<I", len(nb) + 8) + zlib.compress(nb, 9)


def patch_entry(data: bytes) -> bytes:
    """Entry = chunk 0x442A39D9: header(16) + resource header(0x40) + u32 movie size + u32 + movie + pad16."""
    msize, rel = struct.unpack_from(">II", data, 0x50)
    movie = data[0x58:0x58 + msize]
    nm = patch_movie(movie)
    out = bytearray(data[:0x50]) + struct.pack(">II", len(nm), rel) + nm
    while len(out) % 16:
        out.append(0)
    tail = data[16 + struct.unpack_from(">I", data, 4)[0]:]
    out += tail
    struct.pack_into(">II", out, 4, len(out) - 16 - len(tail), len(out) - 16 - len(tail))
    return bytes(out)


def is_patched(data: bytes) -> bool:
    """The generated shape 76 has one path per callout and no hand-drawn polygons with 9+ points."""
    try:
        msize = struct.unpack_from(">I", data, 0x50)[0]
        m = data[0x58:0x58 + msize]
        sig, ver, body = _load(m)
        texts, sprites, shapes = _collect(body)
        for po in sprites.get(57, []):
            if po.get("name") == "tf_select":
                return po["matrix"][4] == int(round(MOVES["combat"]["tf_select"][0] * 20))
    except Exception:  # noqa: BLE001
        pass
    return False


def preview(movie, texture_rgba, out_png, labels=None, mode="combat"):
    from PIL import Image, ImageDraw, ImageFont
    sig, ver, body = _load(movie)
    layout, texts, sprites, shapes = _layout(body, apply_moves=False)
    sc, ox, oy = 2.0, 520, 200

    def P(x, y):
        return ((x + ox) * sc, (y + oy) * sc)
    img = Image.new("RGB", (int(1040 * sc), int(400 * sc)), (12, 10, 10))
    d = ImageDraw.Draw(img)
    tex = Image.fromarray(texture_rgba, "RGBA")
    size = (int(512 * TEX_SCALE * sc), int(256 * TEX_SCALE * sc))
    t2 = tex.resize(size, Image.LANCZOS)
    img.paste(t2, tuple(int(v) for v in P(-192, -100)), t2)
    sh = sp.shape(body, *shapes[SHAPE_OF[mode]]["tag"][2:], 83)
    for pa in sh["paths"]:
        pts = [P(6.8 + 0.5 * p[0] / 20, 0.5 * p[1] / 20) for p in pa["pts"]]
        if len(pts) >= 3:
            curved = any(p[2] == "C" for p in pa["pts"])
            d.polygon(pts, fill=(173, 47, 35) if curved else (215, 205, 185), outline=(0, 0, 0))
    try:
        font = ImageFont.truetype("arial.ttf", int(8.5 * sc))
    except Exception:  # noqa: BLE001
        font = ImageFont.load_default()
    for name, f in layout[mode].items():
        x0, y0, x1, y1 = f["box"]
        text = (labels or {}).get(name, name)
        y = y0 + 2
        for line in text.split("\n"):
            w = d.textlength(line, font=font) / sc
            x = x0 + 2 if f["align"] == 0 else (x1 - 2 - w if f["align"] == 1 else (x0 + x1) / 2 - w / 2)
            d.text(P(x, y), line, fill=(235, 225, 200), font=font)
            y += 10.5
    img.save(out_png)


def main(argv):
    from PIL import Image
    import numpy as np
    movie = open(argv[1], "rb").read()
    tex = np.asarray(Image.open(argv[2]).convert("RGBA"))
    labels = json.load(open(argv[4], encoding="utf-8")) if len(argv) > 4 else None
    new = patch_movie(movie)
    for mode in ("combat", "driving"):
        preview(new, tex, argv[3].replace(".png", "_%s.png" % mode), labels and labels.get(mode), mode)
    print("movie %d -> %d bytes" % (len(movie), len(new)))


if __name__ == "__main__":
    main(sys.argv)
