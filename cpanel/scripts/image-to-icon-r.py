#!/usr/bin/env python3
"""
image-to-icon-r.py -- turn ordinary images into 32x32 classic Mac icon families.

    image-to-icon-r.py <out.r> <image> <id> [<image> <id> ...]

⚠ NAMED DISTINCTLY ON PURPOSE. This repo already lost time twice to two scripts
sharing one name (set-custom-icon.py, then icons-to-r.py). The three generators do
three different things and none of them is interchangeable:

    scripts/custom-icon-to-r.py         a resource fork -> ONE family at ID -16455
    cpanel/scripts/icon-family-to-r.py  a resource fork -> EVERY family, real IDs
    cpanel/scripts/image-to-icon-r.py   ordinary IMAGES -> families at IDs you name

WHY 32x32 AND NOT THE IMAGE'S OWN SIZE. PlotIconID only understands the standard
icon geometries, and the source artwork here is 17x26 -- the Bluetooth roundel's
natural proportion, which is neither 16x16 nor 32x32. The image is therefore CENTRED
in a 32x32 field at its native pixel size rather than resampled: a non-integer
upscale of pixel art produces exactly the mush this project's aesthetic rules exist
to avoid, and the surround costs nothing because the mask makes it transparent.

⚠ THE MASK IS WHAT MAKES THE SURROUND DISAPPEAR. PlotIconID goes through CopyMask, so
only mask-set pixels are written and the platinum ground shows through everywhere
else. An icon whose mask were all-ones would paint a hard rectangle over the group box.

Rez cannot `read` these types -- the silent no-op trap this project hit with PICT --
so the bytes are emitted inline as hex.
"""
import os
import sys
import tempfile

from PIL import Image

# ---- the two classic system palettes ---------------------------------------- #
# 'clut' 4: the fixed 16 colours every 4-bit Mac icon indexes into.
CLUT4 = [
    (0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x03), (0xDD, 0x09, 0x07),
    (0xF2, 0x08, 0x84), (0x47, 0x00, 0xA5), (0x00, 0x00, 0xD3), (0x02, 0xAB, 0xEA),
    (0x1F, 0xB7, 0x14), (0x00, 0x64, 0x12), (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A),
    (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40), (0x00, 0x00, 0x00),
]


def clut8():
    """'clut' 8: the 6x6x6 cube on levels FF CC 99 66 33 00, then the four 10-step
    pure ramps, with black last. Index 0 is white."""
    L = [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    t = [(L[i // 36], L[(i // 6) % 6], L[i % 6]) for i in range(216)]
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    for v in ramp:
        t.append((v, 0, 0))
    for v in ramp:
        t.append((0, v, 0))
    for v in ramp:
        t.append((0, 0, v))
    for v in ramp:
        t.append((v, v, v))
    return t[:255] + [(0, 0, 0)]


CLUT8 = clut8()


def nearest(palette, rgb):
    """Index of the closest palette entry, by squared distance in RGB."""
    r, g, b = rgb
    best, bi = None, 0
    for i, (pr, pg, pb) in enumerate(palette):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best is None or d < best:
            best, bi = d, i
    return bi


def centred(path):
    """The image as a 32x32 RGBA field, centred, never resampled."""
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    if w > 32 or h > 32:
        sys.exit("%s is %dx%d; this script centres, it does not downscale" % (path, w, h))
    field = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    field.paste(im, ((32 - w) // 2, (32 - h) // 2))
    return field


def rows_1bit(bits):
    """32 rows of 32 bits -> 128 bytes, MSB leftmost."""
    out = bytearray()
    for y in range(32):
        for byte in range(4):
            v = 0
            for bit in range(8):
                if bits[y][byte * 8 + bit]:
                    v |= 1 << (7 - bit)
            out.append(v)
    return bytes(out)


def families(field):
    """(ICN#, icl4, icl8) for one 32x32 RGBA field."""
    px = field.load()
    opaque = [[px[x, y][3] > 127 for x in range(32)] for y in range(32)]

    # ICN#: bit set = black ink. The roundel body is dark, its glyph is light, so
    # luminance splits them the way a 1-bit display would.
    #
    # ⚠⚠ THE THRESHOLD IS A TRAP FOR LIGHT ARTWORK, found while lightening the radio
    # Off logo. A body of #999999 has luminance 153, so NOTHING crosses < 128 and the
    # ink plane comes out EMPTY -- a 1-bit or 4-bit screen would draw a blank
    # silhouette. The mask is unaffected (it comes from alpha, and it is the mask
    # PlotIconID uses to composite the colour members), so this is invisible on the
    # millions-of-colours display this panel actually runs on, which is exactly why it
    # would have shipped unnoticed.
    #
    # ⇒ If the luminance split finds no ink at all, fall back to the OPAQUE silhouette.
    # A filled shape is what a 1-bit screen should show for artwork that is uniformly
    # light; drawing nothing is never right. Reported, not silent, so a future artwork
    # change that trips this is visible at build time.
    ink = [[False] * 32 for _ in range(32)]
    for y in range(32):
        for x in range(32):
            r, g, b, a = px[x, y]
            if a > 127 and (r * 299 + g * 587 + b * 114) // 1000 < 128:
                ink[y][x] = True
    if not any(any(row) for row in ink):
        print('    note: no pixel is dark enough for the 1-bit ink plane, so the '
              'OPAQUE silhouette is used instead (light artwork).')
        ink = [row[:] for row in opaque]
    icn = rows_1bit(ink) + rows_1bit(opaque)

    # icl4: two 4-bit indices per byte. Masked-out pixels index 0 (white) -- they are
    # never drawn, so the value only has to be legal.
    icl4 = bytearray()
    for y in range(32):
        for x in range(0, 32, 2):
            hi = nearest(CLUT4, px[x, y][:3]) if opaque[y][x] else 0
            lo = nearest(CLUT4, px[x + 1, y][:3]) if opaque[y][x + 1] else 0
            icl4.append((hi << 4) | lo)

    # icl8: one byte per pixel.
    icl8 = bytearray()
    for y in range(32):
        for x in range(32):
            icl8.append(nearest(CLUT8, px[x, y][:3]) if opaque[y][x] else 0)

    return bytes(icn), bytes(icl4), bytes(icl8)


EXPECT = {"ICN#": 256, "icl4": 512, "icl8": 1024}


def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2 != 0:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    out, rest = sys.argv[1], sys.argv[2:]

    # ⚠⚠ --note EXISTS BECAUSE A REGENERATION DESTROYED REAL DOCUMENTATION.
    #
    # bt_radio_icons.r carried thirty lines of hard-won prose -- that the two masks are
    # identical and why that makes the On/Off swap safe, that icl8 is the deepest a
    # classic family goes so the system cube is what OS 9 displays at ANY depth, and
    # that Rez #include deps are untracked by CMake. All of it was hand-added BELOW a
    # banner reading "do not hand-edit", and the next run of this script wiped it.
    #
    # The banner was not the problem; documentation living only in a generated file
    # was. A note passed here is re-emitted every time, so regenerating can no longer
    # lose it. If the artwork's invariants need explaining, explain them THROUGH this.
    note = None
    if "--note" in rest:
        i = rest.index("--note")
        note = rest[i + 1]
        del rest[i:i + 2]

    jobs = [(rest[i], int(rest[i + 1])) for i in range(0, len(rest), 2)]

    blocks = []
    for path, rid in jobs:
        icn, icl4, icl8 = families(centred(path))
        for ty, body in (("ICN#", icn), ("icl4", icl4), ("icl8", icl8)):
            # ⚠ Checked here, not trusted. A family member of the wrong length is
            # accepted by Rez and then plots as garbage, which is how this project
            # spent a hardware cycle on a "washed-out garble" that was really a
            # colour-state bug -- so the lengths are asserted at generation time.
            if len(body) != EXPECT[ty]:
                sys.exit("%s id %d came out %d bytes, must be %d"
                         % (ty, rid, len(body), EXPECT[ty]))
            blocks.append((ty, rid, body))

    self_path = "/".join(os.path.abspath(__file__).split(os.sep)[-3:])
    # ⚠ Temp file + os.replace, never open(out,'w'). A truncating open that then
    # raises is exactly how this project lost ~2,800 lines of ehci_vhub.c; CLAUDE.md
    # makes the rule absolute and it applies to generators too.
    d = os.path.dirname(os.path.abspath(out)) or "."
    fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("/*\n *  GENERATED by %s -- do not hand-edit.\n"
                    " *  Regenerate if the artwork changes.\n *\n"
                    " *  Sources: %s\n *\n"
                    " *  Rez cannot `read` these types, so the bytes are inline hex.\n"
                    " */\n" % (self_path,
                               ", ".join("%s -> ID %d" % (os.path.basename(p), i)
                                         for p, i in jobs)))
            if note:
                f.write("\n/*\n")
                for ln in note.split("\n"):
                    f.write((" *  " + ln).rstrip() + "\n")
                f.write(" */\n")
            f.write("\n")
            for ty, rid, body in blocks:
                f.write("data '%s' (%d) {\n" % (ty, rid))
                h = body.hex().upper()
                for k in range(0, len(h), 64):
                    f.write('    $"' + h[k:k + 64] + '"\n')
                f.write("};\n\n")
        os.replace(tmp, out)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise

    for ty, rid, body in blocks:
        print("  %s id %d  %d bytes" % (ty, rid, len(body)))
    print("wrote %s" % out)


if __name__ == "__main__":
    main()
