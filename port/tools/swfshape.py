"""Minimal SWF/GFx parser and writer helpers (used by controls_screen_patch.py).

Reads tags (also inside sprites), PlaceObject2, DefineEditText and DefineShape1-4 (styles and edge records);
writes bit fields, MATRIX, RECT and tags."""
import struct
import zlib


class Bits:
    def __init__(self, b, pos=0):
        self.b, self.pos, self.bit = b, pos, 0

    def ub(self, n):
        v = 0
        for _ in range(n):
            byte = self.b[self.pos]
            v = (v << 1) | ((byte >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.pos += 1
        return v

    def sb(self, n):
        v = self.ub(n)
        if n and v & (1 << (n - 1)):
            v -= 1 << n
        return v

    def fb(self, n):
        return self.sb(n) / 65536.0

    def align(self):
        if self.bit:
            self.bit = 0
            self.pos += 1


def load_movie(path):
    raw = open(path, 'rb').read()
    sig = raw[:3]
    body = zlib.decompress(raw[8:]) if sig in (b'CFX', b'CWS') else raw[8:]
    return raw[:8], body


def rect(br):
    n = br.ub(5)
    x0, x1, y0, y1 = br.sb(n), br.sb(n), br.sb(n), br.sb(n)
    return (x0, x1, y0, y1)


def matrix(br):
    sx = sy = 1.0
    r0 = r1 = 0.0
    if br.ub(1):
        n = br.ub(5)
        sx, sy = br.fb(n), br.fb(n)
    if br.ub(1):
        n = br.ub(5)
        r0, r1 = br.fb(n), br.fb(n)
    n = br.ub(5)
    tx, ty = br.sb(n), br.sb(n)
    br.align()
    return (sx, r0, r1, sy, tx, ty)


def iter_tags(body, p):
    while p < len(body):
        h = struct.unpack_from('<H', body, p)[0]
        code, ln = h >> 6, h & 0x3F
        hl = 2
        if ln == 0x3F:
            ln = struct.unpack_from('<I', body, p + 2)[0]
            hl = 6
        yield code, p, p + hl, ln
        if code == 0:
            return
        p += hl + ln


def top_tags(body):
    br = Bits(body)
    rect(br)
    br.align()
    p = br.pos + 4
    return iter_tags(body, p)


def place_object2(body, s, ln):
    flags = body[s]
    depth = struct.unpack_from('<H', body, s + 1)[0]
    p = s + 3
    out = dict(depth=depth, move=bool(flags & 1))
    if flags & 2:
        out['char'] = struct.unpack_from('<H', body, p)[0]
        p += 2
    if flags & 4:
        br = Bits(body, p)
        out['matrix'] = matrix(br)
        p = br.pos
    if flags & 8:
        br = Bits(body, p)
        has_add, has_mult = br.ub(1), br.ub(1)
        n = br.ub(4)
        for _ in range((4 if has_mult else 0) + (4 if has_add else 0)):
            br.sb(n)
        br.align()
        p = br.pos
    if flags & 0x10:
        out['ratio'] = struct.unpack_from('<H', body, p)[0]
        p += 2
    if flags & 0x20:
        e = body.index(b'\0', p)
        out['name'] = body[p:e].decode('latin-1')
        p = e + 1
    return out


def edit_text(body, s, ln):
    cid = struct.unpack_from('<H', body, s)[0]
    br = Bits(body, s + 2)
    bounds = rect(br)
    br.align()
    p = br.pos
    f1, f2 = body[p], body[p + 1]
    p += 2
    out = dict(id=cid, bounds=bounds)
    if f1 & 1:  # HasFont
        out['font'], out['height'] = struct.unpack_from('<HH', body, p)
        p += 4
    if f2 & 0x80:  # HasFontClass
        e = body.index(b'\0', p)
        p = e + 1
    if f1 & 4:  # HasTextColor
        out['color'] = body[p:p + 4].hex()
        p += 4
    if f1 & 2:  # HasMaxLength
        p += 2
    if f2 & 0x20:  # HasLayout
        out['align'] = body[p]
        p += 9
    e = body.index(b'\0', p)
    out['var'] = body[p:e].decode('latin-1')
    p = e + 1
    if f1 & 0x80:  # HasText
        e = body.index(b'\0', p)
        out['text'] = body[p:e].decode('utf-8', 'replace')
    return out


def shape(body, s, ln, code):
    """DefineShape/2/3/4: returns id, bounds, fill styles (raw), line styles, and a list of paths (point lists)."""
    cid = struct.unpack_from('<H', body, s)[0]
    br = Bits(body, s + 2)
    bounds = rect(br)
    br.align()
    if code == 83:
        rect(br)
        br.align()
        br.pos += 1
    p = br.pos
    rgba = code in (32, 83)

    def fill_styles(p):
        n = body[p]
        p += 1
        if n == 0xFF and code != 2:
            n = struct.unpack_from('<H', body, p)[0]
            p += 2
        styles = []
        for _ in range(n):
            t = body[p]
            p += 1
            if t == 0:
                c = body[p:p + (4 if rgba else 3)]
                p += 4 if rgba else 3
                styles.append(('solid', c.hex()))
            elif t in (0x10, 0x12, 0x13):
                br2 = Bits(body, p)
                matrix(br2)
                p = br2.pos
                b = body[p]
                p += 1
                cnt = b & 0x0F
                p += cnt * (1 + (4 if rgba else 3))
                if t == 0x13:
                    p += 2
                styles.append(('gradient', t))
            elif t in (0x40, 0x41, 0x42, 0x43):
                bid = struct.unpack_from('<H', body, p)[0]
                p += 2
                br2 = Bits(body, p)
                m = matrix(br2)
                p = br2.pos
                styles.append(('bitmap', bid, m))
            else:
                raise ValueError('fill %x' % t)
        return styles, p

    def line_styles(p):
        n = body[p]
        p += 1
        if n == 0xFF:
            n = struct.unpack_from('<H', body, p)[0]
            p += 2
        styles = []
        for _ in range(n):
            w = struct.unpack_from('<H', body, p)[0]
            p += 2
            if code == 83:
                f1, f2 = body[p], body[p + 1]
                p += 2
                join = (f1 >> 4) & 3
                if join == 2:
                    p += 2
                if f1 & 8:  # HasFill
                    st, p = fill_styles_one(p)
                    styles.append(('line2', w, st))
                else:
                    styles.append(('line2', w, body[p:p + 4].hex()))
                    p += 4
            else:
                styles.append(('line', w, body[p:p + (4 if rgba else 3)].hex()))
                p += 4 if rgba else 3
        return styles, p

    def fill_styles_one(p):
        raise ValueError('line fill')

    styles_start = p
    fills, p = fill_styles(p)
    lines, p = line_styles(p)
    records_start = p
    br = Bits(body, p)
    nf, nl = br.ub(4), br.ub(4)
    x = y = 0
    paths = []
    cur = None
    while True:
        if br.ub(1):  # edge
            if br.ub(1):  # straight
                n = br.ub(4) + 2
                if br.ub(1):
                    dx, dy = br.sb(n), br.sb(n)
                else:
                    if br.ub(1):
                        dx, dy = 0, br.sb(n)
                    else:
                        dx, dy = br.sb(n), 0
                x += dx
                y += dy
                cur['pts'].append((x, y, 'L'))
            else:
                n = br.ub(4) + 2
                cx, cy = x + br.sb(n), y + br.sb(n)
                x, y = cx + br.sb(n), cy + br.sb(n)
                cur['pts'].append((x, y, 'C', cx, cy))
        else:
            flags = br.ub(5)
            if flags == 0:
                break
            new = dict(pts=[], fill0=None, fill1=None, line=None)
            if cur:
                new.update(fill0=cur['fill0'], fill1=cur['fill1'], line=cur['line'])
            if flags & 1:
                n = br.ub(5)
                x, y = br.sb(n), br.sb(n)
            if flags & 2:
                new['fill0'] = br.ub(nf)
            if flags & 4:
                new['fill1'] = br.ub(nf)
            if flags & 8:
                new['line'] = br.ub(nl)
            if flags & 16:
                br.align()
                fills, pp = fill_styles(br.pos)
                lines, pp = line_styles(pp)
                br = Bits(body, pp)
                nf, nl = br.ub(4), br.ub(4)
            new['pts'].append((x, y, 'M'))
            cur = new
            paths.append(cur)
    return dict(id=cid, bounds=bounds, fills=fills, lines=lines, paths=paths, styles_start=styles_start,
                records_start=records_start, nfill=body[records_start] >> 4, nline=body[records_start] & 15)


# ---------------------------------------------------------------------------- writing


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def ub(self, v, n):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = self.n = 0

    def sb(self, v, n):
        self.ub(v & ((1 << n) - 1), n)

    def align(self):
        if self.n:
            self.out.append(self.acc << (8 - self.n))
            self.acc = self.n = 0

    def bytes(self):
        self.align()
        return bytes(self.out)


def sbits(*vals):
    """Bits needed to store every value as a signed field."""
    n = 1
    for v in vals:
        v = int(v)
        n = max(n, (v.bit_length() if v >= 0 else (~v).bit_length()) + 1)
    return n


def write_rect(bw, x0, x1, y0, y1):
    n = sbits(x0, x1, y0, y1)
    bw.ub(n, 5)
    for v in (x0, x1, y0, y1):
        bw.sb(int(v), n)


def write_matrix(bw, m):
    sx, r0, r1, sy, tx, ty = m
    if (sx, sy) != (1.0, 1.0):
        a, b = int(round(sx * 65536)), int(round(sy * 65536))
        n = sbits(a, b)
        bw.ub(1, 1)
        bw.ub(n, 5)
        bw.sb(a, n)
        bw.sb(b, n)
    else:
        bw.ub(0, 1)
    if (r0, r1) != (0.0, 0.0):
        a, b = int(round(r0 * 65536)), int(round(r1 * 65536))
        n = sbits(a, b)
        bw.ub(1, 1)
        bw.ub(n, 5)
        bw.sb(a, n)
        bw.sb(b, n)
    else:
        bw.ub(0, 1)
    n = sbits(tx, ty) if (tx or ty) else 0
    bw.ub(n, 5)
    if n:
        bw.sb(int(tx), n)
        bw.sb(int(ty), n)
    bw.align()


def tag(code, payload):
    if len(payload) < 63:
        return struct.pack('<H', (code << 6) | len(payload)) + payload
    return struct.pack('<HI', (code << 6) | 0x3F, len(payload)) + payload


def place_object2_set_matrix(body, s, ln, matrix_):
    """The payload of a PlaceObject2 tag with its matrix replaced (it must have one)."""
    flags = body[s]
    assert flags & 4, 'no matrix'
    p = s + 3 + (2 if flags & 2 else 0)
    br = Bits(body, p)
    matrix(br)
    rest = body[br.pos:s + ln]
    bw = BitWriter()
    write_matrix(bw, matrix_)
    return body[s:p] + bw.bytes() + rest
