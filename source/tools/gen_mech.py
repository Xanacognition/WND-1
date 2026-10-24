#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""GEARFRAME's blueprint: firmware/gen/gear_mech.h, the drawing behind every page. A frame (a mech, front view) as
an engineer's sheet would have it: a grid, the frame's outline, its joints, a centre line, dimension lines, a
reticle on the head and call-outs to the parts that make the sound. 240 x 240, 2 bits a pixel:
    0 the paper, 1 the grid, 2 the drawing, 3 what glows (the visor, the reticle, the core)
The firmware shows it in the theme's colours wherever a page paints its background (kawa_gfx.c, g_rect).
Drawn here with Pillow, without smoothing (the screen's pixels are the drawing's).
    python3 tools/gen_mech.py [--png FILE]      (--png: a preview, 2x)"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "firmware" / "gen" / "gear_mech.h"
FONT = ROOT / "assets" / "fonts" / "PressStart2P-Regular.ttf"
W = H = 240
GRID, LINE, GLOW = 1, 2, 3


def mirror(pts, cx):
    return [(2 * cx - x, y) for x, y in pts]


def poly(d, pts, v, cx=None):
    d.line(pts + [pts[0]], fill=v)
    if cx is not None:
        m = mirror(pts, cx)
        d.line(m + [m[0]], fill=v)


def dashed(d, a, b, v, on=4, off=3):
    (x0, y0), (x1, y1) = a, b
    n = max(abs(x1 - x0), abs(y1 - y0))
    for i in range(0, n, on + off):
        t0, t1 = i / n, min(1, (i + on) / n)
        d.line([(round(x0 + (x1 - x0) * t0), round(y0 + (y1 - y0) * t0)),
                (round(x0 + (x1 - x0) * t1), round(y0 + (y1 - y0) * t1))], fill=v)


def circle(d, c, r, v):
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=v)


def draw():
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(str(FONT), 8)
    # the sheet: dots every 8 px, a line every 40
    for y in range(0, H, 8):
        for x in range(0, W, 8):
            d.point((x, y), fill=GRID)
    for k in range(0, W, 40):
        dashed(d, (k, 0), (k, H - 1), GRID, 1, 3)
        dashed(d, (0, k), (W - 1, k), GRID, 1, 3)
    cx = 150                                    # the frame stands right of centre (the pages' text is on the left)
    # the head: a helmet, a V fin, the visor
    poly(d, [(cx, 30), (cx - 9, 34), (cx - 11, 46), (cx - 7, 54), (cx, 56)], LINE, cx)
    d.line([(cx - 3, 32), (cx - 18, 14)], fill=LINE)
    d.line([(cx + 3, 32), (cx + 18, 14)], fill=LINE)
    d.line([(cx - 18, 14), (cx - 13, 22)], fill=LINE)
    d.line([(cx + 18, 14), (cx + 13, 22)], fill=LINE)
    d.line([(cx - 8, 42), (cx + 8, 42)], fill=GLOW)
    d.line([(cx - 7, 43), (cx + 7, 43)], fill=GLOW)
    # the reticle on the head
    circle(d, (cx, 42), 20, GLOW)
    for a in range(0, 360, 90):
        x0, y0 = cx + 23 * math.cos(math.radians(a)), 42 + 23 * math.sin(math.radians(a))
        x1, y1 = cx + 28 * math.cos(math.radians(a)), 42 + 28 * math.sin(math.radians(a))
        d.line([(round(x0), round(y0)), (round(x1), round(y1))], fill=GLOW)
    # the neck, the chest, the core
    d.rectangle([cx - 5, 56, cx + 5, 62], outline=LINE)
    poly(d, [(cx, 62), (cx - 30, 64), (cx - 36, 78), (cx - 26, 104), (cx - 12, 110), (cx, 110)], LINE, cx)
    poly(d, [(cx, 70), (cx - 14, 72), (cx - 16, 86), (cx - 8, 94), (cx, 94)], LINE, cx)
    circle(d, (cx, 84), 6, GLOW)
    circle(d, (cx, 84), 2, GLOW)
    for k in range(4):                          # the vents
        d.line([(cx - 28 + k * 3, 90 + k * 3), (cx - 18, 90 + k * 3)], fill=LINE)
        d.line([(cx + 28 - k * 3, 90 + k * 3), (cx + 18, 90 + k * 3)], fill=LINE)
    # the shoulders and the arms
    for s in (-1, 1):
        sx = cx + s * 46
        poly(d, [(sx - 14, 62), (sx + 14, 62), (sx + 17, 74), (sx + 10, 84), (sx - 10, 84), (sx - 17, 74)], LINE)
        d.line([(sx - 12, 68), (sx + 12, 68)], fill=LINE)
        d.rectangle([sx - 6, 84, sx + 6, 108], outline=LINE)        # the upper arm
        circle(d, (sx, 112), 5, LINE)                               # the elbow
        poly(d, [(sx - 9, 117), (sx + 9, 117), (sx + 11, 146), (sx - 11, 146)], LINE)   # the forearm
        d.line([(sx - 9, 126), (sx + 9, 126)], fill=LINE)
        d.rectangle([sx - 6, 147, sx + 6, 158], outline=LINE)      # the hand
        d.line([(sx - 2, 158), (sx - 2, 164)], fill=LINE)
        d.line([(sx + 2, 158), (sx + 2, 164)], fill=LINE)
    # the waist and the skirt plates
    d.rectangle([cx - 12, 110, cx + 12, 122], outline=LINE)
    poly(d, [(cx - 13, 122), (cx - 30, 124), (cx - 32, 140), (cx - 16, 142)], LINE, cx)
    d.polygon([(cx - 6, 122), (cx + 6, 122), (cx + 4, 134), (cx - 4, 134)], outline=LINE)
    # the legs
    for s in (-1, 1):
        lx = cx + s * 18
        poly(d, [(lx - 9, 142), (lx + 9, 142), (lx + 8, 172), (lx - 8, 172)], LINE)   # the thigh
        circle(d, (lx, 177), 6, LINE)                                                 # the knee
        poly(d, [(lx - 12, 182), (lx + 12, 182), (lx + 10, 216), (lx - 10, 216)], LINE)   # the shin
        d.line([(lx - 11, 194), (lx + 11, 194)], fill=LINE)
        d.line([(lx - 10, 204), (lx + 10, 204)], fill=LINE)
        poly(d, [(lx - 10, 216), (lx + 10, 216), (lx + 16, 226), (lx - 16, 226)], LINE)   # the foot
    # the centre line, the ground, the dimensions
    dashed(d, (cx, 8), (cx, 232), GRID, 6, 2)
    d.line([(cx - 60, 227), (cx + 60, 227)], fill=LINE)
    for k in range(cx - 60, cx + 61, 6):
        d.line([(k, 227), (k - 3, 231)], fill=GRID)
    d.line([(cx + 72, 14), (cx + 72, 226)], fill=LINE)               # the height
    for y in (14, 226):
        d.line([(cx + 68, y), (cx + 76, y)], fill=LINE)
    d.line([(cx + 72, 14), (cx + 70, 19)], fill=LINE); d.line([(cx + 72, 14), (cx + 74, 19)], fill=LINE)
    d.line([(cx + 72, 226), (cx + 70, 221)], fill=LINE); d.line([(cx + 72, 226), (cx + 74, 221)], fill=LINE)
    d.text((cx + 60, 112), "H", font=f, fill=LINE)
    d.line([(cx - 64, 234), (cx + 64, 234)], fill=GRID)              # the width
    # call-outs: the parts that make the sound
    def callout(p, q, text):
        d.line([p, q], fill=LINE)
        d.line([q, (q[0] + (6 if q[0] > p[0] else -6), q[1])], fill=LINE)
        d.point(p, fill=GLOW)
        tw = d.textlength(text, font=f)
        tx = q[0] + 8 if q[0] > p[0] else q[0] - 8 - tw
        d.text((tx, q[1] - 4), text, font=f, fill=LINE)
    callout((cx + 6, 84), (cx + 34, 96), "SPU")
    callout((cx - 46, 70), (cx - 74, 56), "VOX24")
    callout((cx - 18, 196), (cx - 52, 206), "ADPCM")
    callout((cx + 46, 130), (cx + 58, 160), "REV")
    d.text((4, 228), "GF-01", font=f, fill=GRID)
    # the sheet's corners
    for x, y, sx, sy in ((1, 1, 1, 1), (238, 1, -1, 1), (1, 238, 1, -1), (238, 238, -1, -1)):
        d.line([(x, y), (x + 10 * sx, y)], fill=LINE)
        d.line([(x, y), (x, y + 10 * sy)], fill=LINE)
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", type=Path)
    a = ap.parse_args()
    im = draw()
    px = im.load()
    data = bytearray(W * H // 4)
    for y in range(H):
        for x in range(W):
            v = min(3, px[x, y])
            data[(y * W + x) >> 2] |= v << ((x & 3) * 2)
    L = ["/* generated by tools/gen_mech.py: GEARFRAME's blueprint, 240 x 240, 2 bits a pixel (the low bits first):",
         " * 0 the paper, 1 the grid, 2 the drawing, 3 what glows. Shown in the theme's colours (kawa_gfx.c) */",
         "#pragma once", "#include <stdint.h>", f"static const uint8_t MECH_BP[{len(data)}] = {{"]
    for i in range(0, len(data), 40):
        L.append("    " + ",".join(str(b) for b in data[i:i + 40]) + ",")
    L.append("};")
    OUT.write_text("\n".join(L) + "\n")
    print("->", OUT, len(data), "bytes")
    if a.png:
        pal = {0: (14, 18, 20), 1: (34, 46, 46), 2: (60, 110, 104), 3: (232, 176, 72)}
        rgb = Image.new("RGB", (W, H))
        rp = rgb.load()
        for y in range(H):
            for x in range(W):
                rp[x, y] = pal[min(3, px[x, y])]
        rgb.resize((W * 2, H * 2), Image.NEAREST).save(a.png)


if __name__ == "__main__":
    main()
