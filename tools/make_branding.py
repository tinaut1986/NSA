#!/usr/bin/env python3
"""Draws NSA's HOME menu icon (48x48) and banner (256x128) into meta/.

Pure Pillow, whole-pixel shapes and a hand-made pixel font, so the result matches the app's
Cards theme (blue accent, white cards). Run from the repository root; needs Pillow.
"""
from PIL import Image, ImageDraw

BLUE = (0x18, 0x5F, 0xA5)
BLUE_LIGHT = (0x37, 0x8A, 0xDD)
NAVY = (0x0B, 0x1F, 0x3A)
WHITE = (255, 255, 255)
SOFT = (0xE6, 0xF1, 0xFB)

# 5x7 pixel glyphs.
GLYPHS = {
    "N": ["10001", "11001", "11001", "10101", "10011", "10011", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
}


def text(d, x, y, s, scale, fill, gap=1):
    for ch in s:
        for r, row in enumerate(GLYPHS[ch]):
            for c, bit in enumerate(row):
                if bit == "1":
                    d.rectangle([x + c * scale, y + r * scale,
                                 x + (c + 1) * scale - 1, y + (r + 1) * scale - 1], fill=fill)
        x += (5 + gap) * scale


def width(s, scale, gap=1):
    return (len(s) * 5 + (len(s) - 1) * gap) * scale


def icon():
    im = Image.new("RGB", (48, 48), BLUE)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, 47, 47], 8, fill=BLUE)
    d.rounded_rectangle([5, 9, 42, 42], 5, fill=WHITE)          # card
    d.rounded_rectangle([5, 5, 42, 14], 4, fill=BLUE_LIGHT)     # card's header strip
    d.rectangle([5, 10, 42, 14], fill=BLUE_LIGHT)
    for i in range(3):                                           # window dots
        d.rectangle([9 + i * 4, 8, 10 + i * 4, 9], fill=WHITE)
    w = width("NSA", 2)
    text(d, (48 - w) // 2, 21, "NSA", 2, BLUE)
    d.rectangle([(48 - w) // 2, 38, (48 - w) // 2 + w - 1, 38], fill=BLUE_LIGHT)
    mask = Image.new("L", (48, 48), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, 47, 47], 8, fill=255)
    out = Image.new("RGB", (48, 48), (0, 0, 0))
    out.paste(im, mask=mask)
    return out


def banner():
    im = Image.new("RGB", (256, 128), NAVY)
    d = ImageDraw.Draw(im)
    # Three cards cascading on the right; the name stays clear of the first one.
    for x, y, c in [(142, 14, BLUE), (162, 38, BLUE_LIGHT), (182, 62, SOFT)]:
        d.rounded_rectangle([x, y, x + 70, y + 46], 7, fill=c)
        bar = NAVY if c == SOFT else WHITE
        d.rectangle([x + 8, y + 10, x + 34, y + 12], fill=bar)
        d.rectangle([x + 8, y + 20, x + 50, y + 21], fill=bar)
    w = width("NSA", 6)
    text(d, 16, 34, "NSA", 6, WHITE)
    d.rectangle([16, 92, 16 + w - 1, 95], fill=BLUE_LIGHT)
    return im


if __name__ == "__main__":
    icon().save("meta/icon_3ds.png")
    banner().save("meta/banner_3ds.png")
    prev = Image.new("RGB", (48 * 4 + 256 * 2 + 40, 256 + 20), (40, 40, 40))
    prev.paste(icon().resize((192, 192), Image.NEAREST), (10, 10))
    prev.paste(banner().resize((512, 256), Image.NEAREST), (222, 10))
    prev.save("/tmp/claude-1000/-home-tinaut1986-repos/1aad8b45-c6b1-4558-8235-32fc34c55f43/scratchpad/branding-preview.png")
