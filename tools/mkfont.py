#!/usr/bin/env python3
"""Extract the CP437 8x16, 8x14 and 8x8 fonts from a SeaVGABIOS image (the public-domain
fonts SeaBIOS ships in vgasrc/vgafonts.c), and build the VGA's default 256-colour palette
from its structure.

usage: mkfont.py <vgabios.bin> <outdir>
Writes font8x16.bin (4096 bytes), font8x14.bin (3584), font8x8.bin (2048) and
vgapal.bin (768 bytes).
"""
import os
import sys

rom = open(sys.argv[1], "rb").read()
out = sys.argv[2]

A16 = bytes.fromhex("000010386cc6c6fec6c6c6c600000000")
A14 = bytes.fromhex("000010386cc6c6fec6c6c6000000")
A8 = bytes.fromhex("3078ccccfccccc00")


def table(sig, glyph_size, index):
    pos = rom.find(sig)
    if pos < 0:
        sys.exit("signature not found")
    start = pos - index * glyph_size
    data = rom[start:start + 256 * glyph_size]
    assert data[:glyph_size] == bytes(glyph_size), "glyph 0 is not blank"
    return data


f16 = table(A16, 16, 0x41)
f8 = table(A8, 8, 0x41)
f14 = table(A14, 14, 0x41)


def vga_palette():
    """The DAC contents after a mode set: the 16 EGA colours, 16 greys, then 24 hues around
    the colour wheel at three saturations for each of three intensities; the rest black."""
    p = []
    for i in range(16):
        hi = 0x15 if i & 8 else 0
        c = [((i >> 2) & 1) * 0x2A + hi, ((i >> 1) & 1) * 0x2A + hi, (i & 1) * 0x2A + hi]
        if i == 6:
            c[1] = 0x15  # brown, not dark yellow
        p += c
    for v in (0x00, 0x05, 0x08, 0x0B, 0x0E, 0x11, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2D, 0x32,
              0x38, 0x3F):
        p += [v] * 3
    levels = [  # per intensity, high to low saturation: five steps from the low to the high value
        [(0x00, 0x10, 0x1F, 0x2F, 0x3F), (0x1F, 0x27, 0x2F, 0x37, 0x3F), (0x2D, 0x31, 0x36, 0x3A, 0x3F)],
        [(0x00, 0x07, 0x0E, 0x15, 0x1C), (0x0E, 0x11, 0x15, 0x18, 0x1C), (0x14, 0x16, 0x18, 0x1A, 0x1C)],
        [(0x00, 0x04, 0x08, 0x0C, 0x10), (0x08, 0x0A, 0x0C, 0x0E, 0x10), (0x0B, 0x0C, 0x0D, 0x0F, 0x10)],
    ]
    for intensity in levels:
        for a in intensity:
            lo, hi = a[0], a[4]
            for k in range(4):
                p += [a[k], lo, hi]  # blue to magenta
            for k in range(4):
                p += [hi, lo, a[4 - k]]  # to red
            for k in range(4):
                p += [hi, a[k], lo]  # to yellow
            for k in range(4):
                p += [a[4 - k], hi, lo]  # to green
            for k in range(4):
                p += [lo, hi, a[k]]  # to cyan
            for k in range(4):
                p += [lo, a[4 - k], hi]  # back to blue
    return bytes(p + [0] * (768 - len(p)))


pal = vga_palette()

open(os.path.join(out, "font8x16.bin"), "wb").write(f16)
open(os.path.join(out, "font8x14.bin"), "wb").write(f14)
open(os.path.join(out, "font8x8.bin"), "wb").write(f8)
open(os.path.join(out, "vgapal.bin"), "wb").write(pal)

if "-v" in sys.argv:
    for ch in (0x01, 0x41, 0xC9, 0xDB):
        for row in f16[ch * 16:ch * 16 + 16]:
            print("".join("#" if row & (0x80 >> b) else "." for b in range(8)))
        print()
    print(pal[:48].hex(), pal[96:120].hex(), pal[744:].hex())
