#!/usr/bin/env python3
"""Builds NSA's boot logo (meta/logo_3ds.bcma.lz), the animation the HOME menu shows while a CIA starts.

The logo is an LZ11-compressed darc holding Nintendo's logo layout (a .bclyt and three .bclan scenes per
screen) and its textures (.bclim). The base is FBI's old logo (removed in 706c209, read back from git); NSA's
redraws it and adds to it:
  - timg/3dsLogo_01.bclim, 128x128 ETC1A4: the square in the middle of the top screen (FBI's said "FBI"):
    a square take on the banner (meta/banner_3ds.png), cards and NSA on navy, fading out to clear around it;
    its pane is shown bigger (SQUARE_SHOWN) so the fade fits;
  - timg/3dsLogo_00 / _02 (top) and _03 / _04 (bottom), 16x256 ETC1 (240 used): each screen's background as
    the app draws it with the Cards theme, by day and by night (bars, border lines, background). The night one
    is a pane, material and texture of its own, added above the day one, that fades in during the intro;
  - the intro's animation: the square comes from far behind (3D depth) and grows, then day turns to night.
The whole file must stay within the NCCH logo region, 0x2000 bytes.

The darc is followed by an HMAC-SHA256 of it, which the HOME menu checks: a logo with a wrong one makes
the HOME menu panic (svcBreak in the "menu" process) when the CIA starts. The key is in the HOME menu's
code; it is not in this repository. Put it, as 64 hex digits, in ~/.3ds/logo_hmackey_text (the file
yellows8's ctr-logobuilder reads) or pass --key-file. To get it: NSA's "Extract ExeFS" on the HOME menu
(0004003000009802 on a European console), then a 32-byte window of its decompressed .code whose HMAC
of FBI's old logo gives that logo's last 32 bytes. Only the regenerated meta/logo_3ds.bcma.lz is committed.

Run from the repository root; needs Pillow. `--preview DIR` also writes the textures as the console
will decode them (ETC1 is lossy), scaled up. Checked on a 2DS and a New 3DS XL (2026-11-10).
"""
import argparse
import copy
import hashlib
import hmac
import os
import struct
import subprocess
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_branding import BLUE, BLUE_LIGHT, NAVY, SOFT, WHITE, text, width  # noqa: E402

TEMPLATE_REV = "706c209^:meta/logo_3ds.bcma.lz"
OUTPUT = "meta/logo_3ds.bcma.lz"
LOGO_REGION = 0x2000
KEY_FILE = os.path.join(os.path.expanduser("~"), ".3ds", "logo_hmackey_text")


# --- LZ11 -------------------------------------------------------------------------------------------

def lz11_decompress(d):
    if d[0] != 0x11:
        raise ValueError("not LZ11")
    n = d[1] | d[2] << 8 | d[3] << 16
    p, o = 4, bytearray()
    while len(o) < n:
        flags = d[p]
        p += 1
        for b in range(8):
            if len(o) >= n:
                break
            if flags & (0x80 >> b):
                a, t = d[p], d[p] >> 4
                if t == 0:
                    length = ((a & 15) << 4 | d[p + 1] >> 4) + 0x11
                    dist = ((d[p + 1] & 15) << 8 | d[p + 2]) + 1
                    p += 3
                elif t == 1:
                    length = ((a & 15) << 12 | d[p + 1] << 4 | d[p + 2] >> 4) + 0x111
                    dist = ((d[p + 2] & 15) << 8 | d[p + 3]) + 1
                    p += 4
                else:
                    length = t + 1
                    dist = ((a & 15) << 8 | d[p + 1]) + 1
                    p += 2
                for _ in range(length):
                    o.append(o[-dist])
            else:
                o.append(d[p])
                p += 1
    return bytes(o)


def lz11_compress(data):
    """Greedy LZ11 with a hash chain over the 4 KB window, never at distance 1."""
    n = len(data)
    out = bytearray([0x11, n & 0xFF, n >> 8 & 0xFF, n >> 16 & 0xFF])
    heads = {}
    i = 0
    while i < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for b in range(8):
            if i >= n:
                break
            best_len, best_dist = 0, 0
            if i + 3 <= n:
                for j in reversed(heads.get(data[i:i + 3], [])[-256:]):
                    dist = i - j
                    if dist > 0x1000:
                        break
                    if dist < 2:   # as FBI's and buildtools' logos: the console's decoder wants 2 and up
                        continue
                    length = 0
                    while length < 0x10110 and i + length < n and data[j + length] == data[i + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_dist = length, dist
            if best_len >= 3:
                flags |= 0x80 >> b
                dd = best_dist - 1
                if best_len <= 0x10:
                    out += bytes([(best_len - 1) << 4 | dd >> 8, dd & 0xFF])
                elif best_len <= 0x110:
                    m = best_len - 0x11
                    out += bytes([m >> 4, (m & 15) << 4 | dd >> 8, dd & 0xFF])
                else:
                    m = best_len - 0x111
                    out += bytes([0x10 | m >> 12, m >> 4 & 0xFF, (m & 15) << 4 | dd >> 8, dd & 0xFF])
                step = best_len
            else:
                out.append(data[i])
                step = 1
            for k in range(i, min(i + step, n - 2)):
                heads.setdefault(data[k:k + 3], []).append(k)
            i += step
        out[flag_pos] = flags
    while len(out) % 4:
        out.append(0)
    return bytes(out)


# --- darc -------------------------------------------------------------------------------------------

def darc_files(raw):
    """{path: (offset, size)} of a darc's files."""
    _, _, _, _, _, toff, _, _ = struct.unpack_from("<4sHHIIIII", raw, 0)
    count = struct.unpack_from("<III", raw, toff)[2]
    names = toff + count * 12
    files, dirs = {}, []
    for i in range(count):
        a, b, c = struct.unpack_from("<III", raw, toff + i * 12)
        q, name = names + (a & 0xFFFFFF), b""
        while raw[q:q + 2] != b"\0\0":
            name += raw[q:q + 2]
            q += 2
        name = name.decode("utf-16le")
        while dirs and dirs[-1][0] <= i:   # a directory entry's third field is the index past its last
            dirs.pop()
        if a >> 24:
            dirs.append((c, name))
        else:
            files["/".join([d for _, d in dirs if d not in ("", ".")] + [name])] = (b, c)
    return files


# --- ETC1 as the 3DS stores it (8x8 tiles of four 4x4 blocks, each block a little-endian u64) ------

TABLES = [(2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183)]
MODS = [lambda t: t[0], lambda t: t[1], lambda t: -t[0], lambda t: -t[1]]  # index = msb*2+lsb


def _clamp(v):
    return 0 if v < 0 else 255 if v > 255 else v


def _expand5(v):
    return v << 3 | v >> 2


def _half(pixels, base):
    """Best table for a half block on a base colour: (error, clipped, table, [index per pixel]).

    Ties go to no clipping (base + modifier inside 0..255), then the smaller table: FBI's working
    logo is encoded that way, and the same picture encoded with clipping crashed the HOME menu."""
    best = None
    for t, tab in enumerate(TABLES):
        err, clip, idx = 0, 0, []
        for px in pixels:
            e_best, k_best = None, 0
            for k in range(4):
                m = MODS[k](tab)
                c_over = sum(1 for c in base if not 0 <= c + m <= 255)
                e = (sum((_clamp(c + m) - p) ** 2 for c, p in zip(base, px)), c_over)
                if e_best is None or e < e_best:
                    e_best, k_best = e, k
            err += e_best[0]
            clip += e_best[1]
            idx.append(k_best)
        if best is None or (err, clip) < best[:2]:
            best = (err, clip, t, idx)
    return best


def _bases5(pixels):
    avg = [sum(p[c] for p in pixels) / len(pixels) for c in range(3)]
    out = []
    for shift in (0, -1, 1, -2, 2, -3, 3, -4, 4):
        q = tuple(min(31, max(0, round((a + shift * 6) * 31 / 255))) for a in avg)
        if q not in out:
            out.append(q)
    return out


def etc1_encode_block(block):
    """block[y][x] = (r, g, b) -> 64-bit ETC1 word (differential mode, the better flip)."""
    best = None
    for flip in (0, 1):
        halves = [[], []]
        coords = [[], []]
        for x in range(4):
            for y in range(4):
                h = (y >= 2) if flip else (x >= 2)
                halves[h].append(block[y][x])
                coords[h].append((x, y))
        choice = []
        for h in (0, 1):
            cands = []
            for b5 in _bases5(halves[h]):
                base = tuple(_expand5(v) for v in b5)
                err, clip, t, idx = _half(halves[h], base)
                cands.append(((err, clip), b5, t, idx))
            cands.sort(key=lambda c: c[0])
            choice.append(cands)
        # Differential mode: the second base must be within -4..3 of the first on every channel.
        found = None
        for c1 in choice[0]:
            for c2 in choice[1]:
                if all(-4 <= c2[1][k] - c1[1][k] <= 3 for k in range(3)):
                    cost = (c1[0][0] + c2[0][0], c1[0][1] + c2[0][1])
                    if found is None or cost < found[0]:
                        found = (cost, c1, c2)
                    break
        if found is None:
            c1 = choice[0][0]
            b2 = tuple(c1[1][k] + max(-4, min(3, choice[1][0][1][k] - c1[1][k])) for k in range(3))
            base2 = tuple(_expand5(v) for v in b2)
            e2, clip2, t2, i2 = _half(halves[1], base2)
            found = ((c1[0][0] + e2, c1[0][1] + clip2), c1, ((e2, clip2), b2, t2, i2))
        if best is None or found[0] < best[0]:
            best = (found[0], flip, found[1], found[2], coords)
    _, flip, (_, b1, t1, i1), (_, b2, t2, i2), coords = best
    hi = 0
    for k, shift in enumerate((27, 19, 11)):
        hi |= b1[k] << shift
        hi |= ((b2[k] - b1[k]) & 7) << (shift - 3)
    hi |= t1 << 5 | t2 << 2 | 1 << 1 | flip
    lo = 0
    for h, idxs in ((0, i1), (1, i2)):
        for (x, y), k in zip(coords[h], idxs):
            i = x * 4 + y
            lo |= (k >> 1) << (16 + i) | (k & 1) << i
    return hi << 32 | lo


def etc1_decode_block(w):
    hi, lo = w >> 32, w & 0xFFFFFFFF
    flip, t1, t2 = hi & 1, hi >> 5 & 7, hi >> 2 & 7
    if hi >> 1 & 1:
        b1 = [hi >> s & 31 for s in (27, 19, 11)]
        d = [hi >> s & 7 for s in (24, 16, 8)]
        b2 = [(b + (v - 8 if v >= 4 else v)) & 31 for b, v in zip(b1, d)]
        c1, c2 = [_expand5(v) for v in b1], [_expand5(v) for v in b2]
    else:
        c1 = [(hi >> s & 15) * 17 for s in (28, 20, 12)]
        c2 = [(hi >> s & 15) * 17 for s in (24, 16, 8)]
    block = [[None] * 4 for _ in range(4)]
    for x in range(4):
        for y in range(4):
            i = x * 4 + y
            second = (y >= 2) if flip else (x >= 2)
            k = (lo >> (16 + i) & 1) * 2 + (lo >> i & 1)
            m = MODS[k](TABLES[t2 if second else t1])
            block[y][x] = tuple(_clamp(c + m) for c in (c2 if second else c1))
    return block


def _tile_blocks(w, h):
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for by, bx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                yield tx + bx, ty + by


def etc1_encode(im):
    im = im.convert("RGB")
    px = im.load()
    out = bytearray()
    for x0, y0 in _tile_blocks(*im.size):
        block = [[px[x0 + x, y0 + y] for x in range(4)] for y in range(4)]
        out += struct.pack("<Q", etc1_encode_block(block))
    return bytes(out)


def etc1_decode(data, w, h):
    im = Image.new("RGB", (w, h))
    for n, (x0, y0) in enumerate(_tile_blocks(w, h)):
        block = etc1_decode_block(struct.unpack_from("<Q", data, n * 8)[0])
        for y in range(4):
            for x in range(4):
                im.putpixel((x0 + x, y0 + y), block[y][x])
    return im


def etc1a4_encode(im):
    """ETC1 with 4-bit alpha (format 11): each block is its alpha, a little-endian u64 with pixel (x, y) in bits
    4 * (x * 4 + y), then its ETC1 block."""
    im = im.convert("RGBA")
    px = im.load()
    out = bytearray()
    for x0, y0 in _tile_blocks(*im.size):
        alpha = 0
        for x in range(4):
            for y in range(4):
                alpha |= (px[x0 + x, y0 + y][3] * 15 + 127) // 255 << (4 * (x * 4 + y))
        block = [[px[x0 + x, y0 + y][:3] for x in range(4)] for y in range(4)]
        out += struct.pack("<QQ", alpha, etc1_encode_block(block))
    return bytes(out)


def etc1a4_decode(data, w, h):
    im = Image.new("RGBA", (w, h))
    for n, (x0, y0) in enumerate(_tile_blocks(w, h)):
        alpha, word = struct.unpack_from("<QQ", data, n * 16)
        block = etc1_decode_block(word)
        for y in range(4):
            for x in range(4):
                a = (alpha >> (4 * (x * 4 + y)) & 15) * 17
                im.putpixel((x0 + x, y0 + y), tuple(block[y][x]) + (a,))
    return im


# --- The pictures ------------------------------------------------------------------------------------

# The app's Cards theme (source/core/theme.c), by day and by night: surface (its bars), border, background.
DAY_THEME = ((0xFF, 0xFF, 0xFF), (0xD5, 0xDE, 0xDB), (0xEE, 0xF2, 0xF1))
NIGHT_THEME = ((0x1F, 0x26, 0x24), (0x34, 0x40, 0x3C), (0x14, 0x19, 0x17))
TOP_BAR = 21      # rows of the top screen's top bar (WIDGET_TOP_BAR_HEIGHT 22, its border line included)
BOTTOM_BAR = 25   # and of the bottom screen's title bar (WIDGET_TITLE_BAR_HEIGHT 26)
HINT_BAR = 21     # and of its hint bar at the foot (WIDGET_HINT_BAR_HEIGHT 22)


SQUARE_SHOWN = 200   # the square pane's size on the top screen (144 in FBI's): room for the fade around it
CORE = 88           # the picture inside the square texture, in texels; the rest fades from navy to clear


def picture():
    """The banner's look made square: three cards cascading on navy, NSA in white below them (128x128)."""
    im = Image.new("RGB", (128, 128), NAVY)
    d = ImageDraw.Draw(im)
    for x, y, c in [(30, 10, BLUE), (44, 22, BLUE_LIGHT), (58, 34, SOFT)]:
        d.rounded_rectangle([x, y, x + 40, y + 26], 4, fill=c)
        bar = NAVY if c == SOFT else WHITE
        d.rectangle([x + 5, y + 6, x + 20, y + 7], fill=bar)
        d.rectangle([x + 5, y + 12, x + 29, y + 12], fill=bar)
    w = width("NSA", 4)
    x = (128 - w) // 2
    text(d, x, 74, "NSA", 4, WHITE)
    d.rectangle([x, 107, x + w - 1, 109], fill=BLUE_LIGHT)          # underline, as in the banner
    return im


def square():
    """128x128 with alpha: the picture in the middle, its navy fading out around it, so it melts into the
    background whatever its colour (day or night)."""
    m = (128 - CORE) // 2
    radius = 10
    feather = m
    im = Image.new("RGBA", (128, 128), NAVY + (0,))
    alpha = Image.new("L", (128, 128), 0)
    ap = alpha.load()
    lo, hi = m + radius, 128 - m - radius
    for y in range(128):
        for x in range(128):
            cx, cy = x + 0.5, y + 0.5
            dx = max(lo - cx, 0, cx - hi)
            dy = max(lo - cy, 0, cy - hi)
            dist = (dx * dx + dy * dy) ** 0.5 - radius
            t = min(max(1 - dist / feather, 0.0), 1.0)
            ap[x, y] = round(255 * t * t * (3 - 2 * t))   # smoothstep
    im.paste(picture().resize((CORE, CORE), Image.LANCZOS), (m, m))
    im.putalpha(alpha)
    return im


def band(bar, colours, hint=0):
    """16x256 (240 shown), stretched across a screen behind everything: the app's screen in one theme (colours
    is its surface, border and background): its top bar, `bar` rows of surface with a border line under them,
    the background, and at the foot, if hint, a bar of `hint` rows with a border line over it."""
    surface, border, background = colours
    im = Image.new("RGB", (16, 256), background)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 15, bar - 1], fill=surface)
    d.rectangle([0, bar, 15, bar], fill=border)
    if hint:
        d.rectangle([0, 240 - hint, 15, 255], fill=surface)
        d.rectangle([0, 239 - hint, 15, 239 - hint], fill=border)
    return im


# --- Layout (CLYT) -------------------------------------------------------------------------------------
#
# Each screen's band gets a twin above it, the night one, faded in over the day one (see animate_scene): one
# layer cannot turn into the other, since by day the bar is lighter than the background and by night too, but
# the border line is darker by day and lighter by night.

def _clyt_sections(data):
    if data[:4] != b"CLYT":
        raise ValueError("not a CLYT")
    p = struct.unpack_from("<H", data, 6)[0]
    out = []
    for _ in range(struct.unpack_from("<I", data, 0x10)[0]):
        magic, length = data[p:p + 4], struct.unpack_from("<I", data, p + 4)[0]
        out.append([magic, bytearray(data[p + 8:p + length])])
        p += length
    return data[:struct.unpack_from("<H", data, 6)[0]], out


def _clyt_bytes(head, sections):
    body = b"".join(m + struct.pack("<I", 8 + len(b)) + bytes(b) for m, b in sections)
    head = bytearray(head)
    struct.pack_into("<II", head, 0x0C, len(head) + len(body), len(sections))
    return bytes(head) + body


def _name(b, at, size):
    return bytes(b[at:at + size]).split(bytes(1))[0].decode()


def _txl_names(body):
    count = struct.unpack_from("<I", body, 0)[0]
    return [_name(body, 4 + o, 64) for o in struct.unpack_from(f"<{count}I", body, 4)]


def _txl_body(names):
    raw = [n.encode() + bytes(1) for n in names]
    out = bytearray(struct.pack("<I", len(names)))
    at = 4 * len(names)
    for r in raw:
        out += struct.pack("<I", at)
        at += len(r)
    out += b"".join(raw)
    return out + bytes(-len(out) % 4)


def _mat_list(body):
    count = struct.unpack_from("<I", body, 0)[0]
    offs = [o - 8 for o in struct.unpack_from(f"<{count}I", body, 4)] + [len(body)]
    return [bytearray(body[offs[i]:offs[i + 1]]) for i in range(count)]


def _mat_body(mats):
    out = bytearray(struct.pack("<I", len(mats)))
    at = 8 + 4 + 4 * len(mats)   # from the section's start
    for m in mats:
        out += struct.pack("<I", at)
        at += len(m)
    return out + b"".join(bytes(m) for m in mats)


def layout(data, square=None, day=None, night=None, textures=None):
    """The CLYT with the square pane resized (square = (pane, size)) and the band pane `day` given a twin `night`
    drawn right above it (same place, same groups), transparent until animated, showing a texture of its own;
    textures = (day's texture, night's texture) by file name."""
    head, sections = _clyt_sections(data)
    if square:
        for magic, b in sections:
            if magic == b"pic1" and _name(b, 4, 16) == square[0]:
                struct.pack_into("<2f", b, 0x3C, square[1], square[1])
    if day:
        txl = next(b for m, b in sections if m == b"txl1")
        mat = next(b for m, b in sections if m == b"mat1")
        names = _txl_names(txl)
        mats = _mat_list(mat)
        dm = next(m for m in mats if _name(m, 0, 20) == day)
        names[struct.unpack_from("<H", dm, 0x34)[0]] = textures[0]
        names.append(textures[1])
        nm = bytearray(dm)
        nm[0:20] = night.encode().ljust(20, bytes(1))
        struct.pack_into("<H", nm, 0x34, len(names) - 1)
        mats.append(nm)
        txl[:] = _txl_body(names)
        mat[:] = _mat_body(mats)
        out = []
        for magic, b in sections:
            out.append([magic, b])
            if magic == b"pic1" and _name(b, 4, 16) == day:
                twin = bytearray(b)
                twin[4:20] = night.encode().ljust(16, bytes(1))
                twin[2] = 0                                                  # alpha
                struct.pack_into("<H", twin, 0x54, len(mats) - 1)           # material (after the pane, 0x44, and 4 vertex colours)
                out.append([b"pic1", twin])
            if magic == b"grp1":
                count = struct.unpack_from("<I", b, 16)[0]
                members = [_name(b, 20 + 16 * i, 16) for i in range(count)]
                if day in members:
                    b += night.encode().ljust(16, bytes(1))
                    struct.pack_into("<I", b, 16, count + 1)
        sections = out
    return _clyt_bytes(head, sections)


# --- Animation (CLAN) --------------------------------------------------------------------------------
#
# FBI's layout animates each screen in three scenes: A (the intro, played first), B (held, looped on the top
# screen until the app is ready) and C (the way out). In A the band shrinks to its height (scale Y 1.25 -> 1) and
# the square comes forward (translation Z), grows and fades in: Z is what the 3D slider shows as depth.
# In NSA's intro the square comes from far behind and stays a little in front of the screen, then the
# background goes from the app's look by day to its look by night: a second band, the night one, fades in over
# the day one. The template has neither that pane nor its texture, so layouts, animations and the darc are rebuilt.

# Scene A keeps FBI's 60 frames: the HOME menu plays each scene within its pat1 range (A is frames 181-240),
# and a longer A (100 frames, with the change of colour at 55-95) never got to the night look on the console.
SQUARE_FROM, SQUARE_TO = 8, 40   # the square's entrance
DEPTH_FROM = -80.0   # where the square starts, far behind the screen
DEPTH_REST = 8.0     # where it stays: a little in front of the screen, with the 3D slider up
SCALE_FROM = 0.5
NIGHT_FROM, NIGHT_TO = 30, 58   # the frames of the day-to-night change, in scene A
CLPA_Z, CLPA_SCALE_X, CLPA_SCALE_Y, CLVC_ALPHA = 2, 6, 7, 16
HERMITE = 2


def clan_parse(data):
    """A CLAN as (version, [section]): a section is (b"pai1", animation) or (magic, raw bytes). An animation is
    {"frames", "loop", "entries": [{"name", "material", "tags": [{"magic", "groups": [{"index", "target",
    "keys": [(frame, value, slope)]}]}]}]}."""
    if data[:4] != b"CLAN":
        raise ValueError("not a CLAN")
    hlen, version, _, count = struct.unpack_from("<HIII", data, 6)
    sections, p = [], hlen
    for _ in range(count):
        magic, length = data[p:p + 4], struct.unpack_from("<I", data, p + 4)[0]
        if magic != b"pai1":
            sections.append((magic, data[p:p + length]))
        else:
            frames, loop, textures, n, table = struct.unpack_from("<HBxHHI", data, p + 8)
            if textures:
                raise ValueError("pai1 with textures")
            entries = []
            for eo in struct.unpack_from(f"<{n}I", data, p + table):
                e = p + eo
                ntags, material = data[e + 20], data[e + 21]
                tags = []
                for to in struct.unpack_from(f"<{ntags}I", data, e + 24):
                    t = e + to
                    groups = []
                    for go in struct.unpack_from(f"<{data[t + 4]}I", data, t + 8):
                        g = t + go
                        index, target, curve, keys, ko = struct.unpack_from("<BBBxH2xI", data, g)
                        if curve != HERMITE:
                            raise ValueError("not a hermite curve")
                        groups.append({"index": index, "target": target,
                                       "keys": [struct.unpack_from("<3f", data, g + ko + 12 * k) for k in range(keys)]})
                    tags.append({"magic": data[t:t + 4], "groups": groups})
                entries.append({"name": data[e:e + 20], "material": material, "tags": tags})
            sections.append((magic, {"frames": frames, "loop": loop, "entries": entries}))
        p += length
    return version, sections


def _pai1_bytes(anim):
    def group(g):
        keys = b"".join(struct.pack("<3f", *k) for k in g["keys"])
        return struct.pack("<BBBxH2xI", g["index"], g["target"], HERMITE, len(g["keys"]), 12) + keys

    def table(head, parts):
        """head, then the offsets of parts (from the start of head), then parts."""
        out = bytearray(head)
        at = len(head) + 4 * len(parts)
        for part in parts:
            out += struct.pack("<I", at)
            at += len(part)
        return bytes(out) + b"".join(parts)

    def tag(t):
        return table(t["magic"] + struct.pack("<B3x", len(t["groups"])), [group(g) for g in t["groups"]])

    def entry(e):
        return table(e["name"] + struct.pack("<BB2x", len(e["tags"]), e["material"]), [tag(t) for t in e["tags"]])

    entries = [entry(e) for e in anim["entries"]]
    head = struct.pack("<HBxHHI", anim["frames"], anim["loop"], 0, len(entries), 0x14)
    body = table(b"", entries)
    # The entry offsets count from the section's start: 8 bytes of magic and size, then head.
    body = bytearray(body)
    for i in range(len(entries)):
        o = struct.unpack_from("<I", body, 4 * i)[0]
        struct.pack_into("<I", body, 4 * i, o + 8 + len(head))
    return b"pai1" + struct.pack("<I", 8 + len(head) + len(body)) + head + bytes(body)


def clan_bytes(version, sections):
    parts = [_pai1_bytes(s) if m == b"pai1" else s for m, s in sections]
    size = 0x14 + sum(len(p) for p in parts)
    return b"CLAN" + struct.pack("<HHIII", 0xFEFF, 0x14, version, size, len(parts)) + b"".join(parts)


def _find(anim, pane):
    for e in anim["entries"]:
        if e["name"].split(bytes(1))[0].decode() == pane:
            return e
    raise KeyError(pane)


def _set(entry, magic, target, keys):
    for t in entry["tags"]:
        if t["magic"] == magic:
            for g in t["groups"]:
                if g["target"] == target:
                    g["keys"] = keys
                    return
    raise KeyError((magic, target))


def ease(frame0, frame1, v0, v1):
    """Two keys that go from v0 to v1 fast and slow down at the end (the slope is 0 there)."""
    return [(frame0, v0, 2 * (v1 - v0) / (frame1 - frame0)), (frame1, v1, 0.0)]


def animate_scene(data, scene, day, night, square=None):
    """One scene's CLAN with NSA's timing: the night band (a copy of the day one's animation) fading in over the
    day one in the intro and staying, and the square's deeper entrance."""
    version, sections = clan_parse(data)
    out = []
    for magic, s in sections:
        if magic == b"pai1":
            twin = copy.deepcopy(_find(s, day))
            twin["name"] = night.encode().ljust(20, bytes(1))
            fade = [(0, 0, 0), (NIGHT_FROM, 0, 0)] + ease(NIGHT_FROM, NIGHT_TO, 0.0, 255.0) if scene == "A" else [(0, 255, 0)]
            twin["tags"].append({"magic": b"CLVC", "groups": [{"index": 0, "target": CLVC_ALPHA, "keys": fade}]})
            s["entries"].insert(s["entries"].index(_find(s, day)) + 1, twin)
            if square:
                sq = _find(s, square)
                if scene == "A":
                    _set(sq, b"CLPA", CLPA_Z, [(0, DEPTH_FROM, 0), (SQUARE_FROM, DEPTH_FROM, 0)] + ease(SQUARE_FROM, SQUARE_TO, DEPTH_FROM, DEPTH_REST))
                    for target in (CLPA_SCALE_X, CLPA_SCALE_Y):
                        _set(sq, b"CLPA", target, [(0, SCALE_FROM, 0), (SQUARE_FROM, SCALE_FROM, 0)] + ease(SQUARE_FROM, SQUARE_TO, SCALE_FROM, 1.0))
                    _set(sq, b"CLVC", CLVC_ALPHA, [(0, 0, 0), (SQUARE_FROM, 0, 0)] + ease(SQUARE_FROM, SQUARE_FROM + 16, 0.0, 255.0))
                else:
                    frame = [g for t in sq["tags"] for g in t["groups"] if g["target"] == CLPA_Z][0]["keys"][0][0]
                    _set(sq, b"CLPA", CLPA_Z, [(frame, DEPTH_REST, 0.0)])
        out.append((magic, s))
    return clan_bytes(version, out)


# --- darc, rebuilt -------------------------------------------------------------------------------------

def darc_tree(raw):
    """The darc's tree: {name: subtree or (offset, size)}, in the archive's order."""
    _, _, _, _, _, toff, _, _ = struct.unpack_from("<4sHHIIIII", raw, 0)
    count = struct.unpack_from("<III", raw, toff)[2]
    names = toff + count * 12
    root = {}
    stack = [(count, root)]
    for i in range(count):
        a, b, c = struct.unpack_from("<III", raw, toff + i * 12)
        q, name = names + (a & 0xFFFFFF), b""
        while raw[q:q + 2] != bytes(2):
            name += raw[q:q + 2]
            q += 2
        name = name.decode("utf-16le")
        while stack[-1][0] <= i:
            stack.pop()
        if a >> 24:
            sub = {}
            stack[-1][1][name] = sub
            stack.append((c, sub))
        else:
            stack[-1][1][name] = (b, c)
    return root


def darc_build(tree, data_of):
    """A darc of tree, each file's bytes from data_of(path): textures aligned to 0x80, the rest to 0x20, as in
    the template."""
    entries = []   # [name, is dir, parent index or path, end index]

    def walk(node, parent, path):
        for name, sub in node.items():
            i = len(entries)
            if isinstance(sub, dict):
                entries.append([name, True, parent, 0])
                walk(sub, i, path + [name] if name not in ("", ".") else path)
                entries[i][3] = len(entries)
            else:
                entries.append([name, False, "/".join(path + [name]), 0])

    walk(tree, 0, [])
    names = bytearray()
    name_at = []
    for e in entries:
        name_at.append(len(names))
        names += e[0].encode("utf-16le") + bytes(2)
    table_len = 12 * len(entries) + len(names)
    head = 0x1C
    data_at = head + table_len
    data_at += -data_at % 0x20
    out = bytearray(data_at)
    where = []
    for e in entries:
        if e[1]:
            where.append((e[2], e[3]))
            continue
        blob = data_of(e[2])
        out += bytes(-len(out) % (0x80 if e[0].endswith(".bclim") else 0x20))
        where.append((len(out), len(blob)))
        out += blob
    struct.pack_into("<4sHHIIIII", out, 0, b"darc", 0xFEFF, head, 0x01000000, len(out), head, data_at - head, data_at)   # the table's length counts its padding
    for i, e in enumerate(entries):
        struct.pack_into("<III", out, head + 12 * i, name_at[i] | (1 << 24 if e[1] else 0), *where[i])
    out[head + 12 * len(entries):head + table_len] = names
    return out


# --- Build ---------------------------------------------------------------------------------------------

def template():
    return subprocess.run(["git", "show", TEMPLATE_REV], check=True, capture_output=True).stdout


def read_key(path):
    with open(path, encoding="ascii") as f:
        key = bytes.fromhex(f.read().strip())
    if len(key) != 32:
        raise ValueError(f"{path}: {len(key)} bytes of key, expected 32")
    return key


def clim(im, fmt, footer):
    """The texture as a BCLIM: ETC1 (10) or ETC1A4 (11), with the template's footer told its format and size."""
    data = etc1a4_encode(im) if fmt == 11 else etc1_encode(im)
    foot = bytearray(footer)
    if foot[:4] != b"CLIM" or foot[0x14:0x18] != b"imag":
        raise ValueError("no CLIM footer")
    struct.pack_into("<I", foot, 0x0C, len(data) + 0x28)
    struct.pack_into("<HHII", foot, 0x14 + 8, im.size[0], min(im.size[1], 240) if im.size[0] == 16 else im.size[1], fmt, len(data))
    return data + bytes(foot)


def build(key, preview=None):
    template_raw = lz11_decompress(template())
    darc_size = struct.unpack_from("<I", template_raw, 12)[0]
    if len(template_raw) != darc_size + 32:
        raise ValueError(f"template: {len(template_raw)} bytes for a darc of {darc_size} and its HMAC")
    if hmac.new(key, template_raw[:darc_size], hashlib.sha256).digest() != template_raw[darc_size:]:
        raise ValueError("wrong key: it does not give the HMAC of FBI's logo")
    raw = bytes(template_raw[:darc_size])
    files = darc_files(raw)

    def old(path):
        off, size = files[path]
        return raw[off:off + size]

    footer = old("timg/3dsLogo_00.bclim")[-0x28:]
    textures = {   # file: (picture, format)
        "3dsLogo_01.bclim": (square(), 11),
        "3dsLogo_00.bclim": (band(TOP_BAR, DAY_THEME), 10),
        "3dsLogo_02.bclim": (band(TOP_BAR, NIGHT_THEME), 10),
        "3dsLogo_03.bclim": (band(BOTTOM_BAR, DAY_THEME, HINT_BAR), 10),
        "3dsLogo_04.bclim": (band(BOTTOM_BAR, NIGHT_THEME, HINT_BAR), 10),
    }
    new = {"timg/" + name: clim(im, fmt, footer) for name, (im, fmt) in textures.items()}
    if preview:
        os.makedirs(preview, exist_ok=True)
        for name, (im, fmt) in textures.items():
            data = new["timg/" + name]
            dec = etc1a4_decode(data, *im.size) if fmt == 11 else etc1_decode(data, *im.size)
            dec.resize((im.size[0] * 4, im.size[1] * 4), Image.NEAREST).save(os.path.join(preview, name + ".png"))

    screens = (("U", "P_Nin_00", "P_Nit_00", ("3dsLogo_00.bclim", "3dsLogo_02.bclim"), ("P_Ds_00", SQUARE_SHOWN)),
               ("D", "P_NinLogo_00", "P_NitLogo_00", ("3dsLogo_03.bclim", "3dsLogo_04.bclim"), None))
    for screen, day, night, tex, sq in screens:
        path = f"blyt/NintendoLogo_{screen}_00.bclyt"
        new[path] = layout(old(path), sq, day, night, tex)
        for scene in "ABC":
            path = f"anim/NintendoLogo_{screen}_00_SceneOut{scene}.bclan"
            new[path] = animate_scene(old(path), scene, day, night, sq[0] if sq else None)

    tree = darc_tree(raw)
    timg = tree[""]["."]["timg"]
    for name in textures:
        timg.setdefault(name, None)
    out = darc_build(tree, lambda path: new.get(path) or old(path))
    out += hmac.new(key, bytes(out), hashlib.sha256).digest()
    packed = lz11_compress(bytes(out))
    if lz11_decompress(packed) != bytes(out):
        raise AssertionError("LZ11 round trip failed")
    if len(packed) > LOGO_REGION:
        raise ValueError(f"logo is {len(packed)} bytes, over the {LOGO_REGION} of the logo region")
    # The HOME menu reads the whole 0x2000-byte region: a shorter logo (1972 bytes, unpadded) ended in
    # ErrDisp "The SD card was removed" on the 2DS. FBI's and buildtools' logos are zero-padded to 0x2000.
    return packed + bytes(LOGO_REGION - len(packed))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--preview", help="also write the decoded textures (x4) into this folder")
    ap.add_argument("--key-file", default=KEY_FILE, help="the logo HMAC key, 64 hex digits (default: %(default)s)")
    args = ap.parse_args()
    data = build(read_key(args.key_file), args.preview)
    with open(OUTPUT, "wb") as f:
        f.write(data)
    print(f"{OUTPUT}: {len(data)} bytes")
